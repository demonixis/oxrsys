// SPDX-License-Identifier: MPL-2.0

#include "XrApp.h"

#include <android/hardware_buffer.h>
#include <android/log.h>
#include <android_native_app_glue.h>
#include <arpa/inet.h>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <numeric>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <openxr/openxr_platform.h>

#define LOG_TAG "OXRSys-Android"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

#define XR_CHECK(result, msg)                                       \
    do                                                              \
    {                                                               \
        XrResult xrResult = (result);                               \
        if (XR_FAILED(xrResult))                                    \
        {                                                           \
            LOGE("%s failed: %d", msg, xrResult);                   \
            return false;                                           \
        }                                                           \
    } while (0)

// ─── Blit shader sources ──────────────────────────────────────────────────────
// Uses GL_OES_EGL_image_external_essl3 for zero-copy video frame rendering.
// The GPU handles YUV→RGB conversion natively — no CPU color conversion needed.

static const char* BLIT_VERTEX_SHADER = R"(#version 300 es
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
out vec2 vUV;
void main() {
    vUV = aUV;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

namespace
{

constexpr auto kUsbAdbRetryInterval = std::chrono::seconds(1);
constexpr uint32_t kUsbAdbRetryLogInterval = 10;

} // namespace

static const char* BLIT_FRAGMENT_SHADER_OES = R"(#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
precision mediump float;
in vec2 vUV;
out vec4 fragColor;
uniform samplerExternalOES uTexture;
void main() {
    fragColor = texture(uTexture, vUV);
}
)";

namespace oxr
{

namespace
{

constexpr float kPreferredDisplayRefreshRateHz =
    static_cast<float>(OXRSYS_PREFERRED_DISPLAY_REFRESH_RATE_HZ);

std::string JoinExtensionNames(const std::vector<const char*>& extensionNames)
{
    std::string joined;
    for (const char* extensionName : extensionNames)
    {
        if (!joined.empty())
        {
            joined += ", ";
        }
        joined += extensionName;
    }
    return joined;
}

std::string PathToString(XrInstance instance, XrPath path)
{
    if (path == XR_NULL_PATH)
    {
        return "<none>";
    }

    char buffer[XR_MAX_PATH_LENGTH] = {};
    uint32_t count = 0;
    if (XR_FAILED(xrPathToString(instance, path, sizeof(buffer), &count, buffer)))
    {
        return "<unknown>";
    }
    return buffer;
}

bool HasValidJointPosition(const XrHandJointLocationEXT& joint)
{
    return (joint.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0 &&
           std::isfinite(joint.pose.position.x) &&
           std::isfinite(joint.pose.position.y) &&
           std::isfinite(joint.pose.position.z) &&
           std::isfinite(joint.radius);
}

bool HasValidCriticalHandJoints(const XrHandJointLocationEXT* joints)
{
    return HasValidJointPosition(joints[XR_HAND_JOINT_PALM_EXT]) &&
           HasValidJointPosition(joints[XR_HAND_JOINT_WRIST_EXT]) &&
           HasValidJointPosition(joints[XR_HAND_JOINT_THUMB_TIP_EXT]) &&
           HasValidJointPosition(joints[XR_HAND_JOINT_INDEX_TIP_EXT]);
}

} // namespace

static int64_t SteadyClockNowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static bool SendAll(int socket, const void* data, size_t size)
{
    const auto* bytes = static_cast<const uint8_t*>(data);
    size_t sentTotal = 0;
    while (sentTotal < size)
    {
        ssize_t sent = send(socket, bytes + sentTotal, size - sentTotal, MSG_NOSIGNAL);
        if (sent < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return false;
        }
        if (sent == 0)
        {
            return false;
        }
        sentTotal += static_cast<size_t>(sent);
    }
    return true;
}

static bool ReadAll(int socket, void* data, size_t size)
{
    auto* bytes = static_cast<uint8_t*>(data);
    size_t receivedTotal = 0;
    while (receivedTotal < size)
    {
        ssize_t received = recv(socket, bytes + receivedTotal, size - receivedTotal, 0);
        if (received < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return false;
        }
        if (received == 0)
        {
            return false;
        }
        receivedTotal += static_cast<size_t>(received);
    }
    return true;
}

static bool SendTcpRecord(int socket, protocol::TcpRecordType type,
                          const void* payload, size_t payloadSize)
{
    protocol::TcpRecordHeader header = {};
    header.type = type;
    header.payloadSize = static_cast<uint32_t>(payloadSize);
    return SendAll(socket, &header, sizeof(header)) &&
           (payloadSize == 0 || SendAll(socket, payload, payloadSize));
}

static bool ReadTcpRecord(int socket, protocol::TcpRecordHeader& header,
                          std::vector<uint8_t>& payload)
{
    if (!ReadAll(socket, &header, sizeof(header)))
    {
        return false;
    }
    if (header.magic != protocol::TCP_RECORD_MAGIC ||
        header.version != protocol::TCP_RECORD_VERSION ||
        header.payloadSize > protocol::TCP_MAX_RECORD_PAYLOAD)
    {
        return false;
    }
    payload.clear();
    payload.resize(header.payloadSize);
    return payload.empty() || ReadAll(socket, payload.data(), payload.size());
}

static void ConfigureTcpSocket(int socket)
{
    int nodelay = 1;
    setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
}

static float ClampNormalized(float value)
{
    return std::clamp(value, 0.0f, 1.0f);
}

static bool IsNearlyFullExtent(uint32_t fullExtent, int32_t croppedExtent)
{
    if (fullExtent == 0 || croppedExtent <= 0)
    {
        return false;
    }

    // MediaCodec may report a tiny conformance/alignment crop (a few pixels).
    // That is safe to honor. Large crops tend to describe decoder internals or
    // memory layout rather than the visible GL texture domain, which would make
    // us zoom into a quarter of the frame when resolution_scale < 1.
    constexpr uint32_t MaxAlignmentCrop = 64;
    uint32_t cropped = static_cast<uint32_t>(croppedExtent);
    if (fullExtent >= cropped && (fullExtent - cropped) <= MaxAlignmentCrop)
    {
        return true;
    }

    return static_cast<float>(cropped) >= static_cast<float>(fullExtent) * 0.95f;
}

static XrVector3f Cross(const XrVector3f& a, const XrVector3f& b)
{
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
    };
}

static XrQuaternionf NormalizeQuaternion(const XrQuaternionf& q)
{
    float length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (!std::isfinite(length) || length < 0.0001f)
    {
        return {0.0f, 0.0f, 0.0f, 1.0f};
    }

    float invLength = 1.0f / length;
    return {q.x * invLength, q.y * invLength, q.z * invLength, q.w * invLength};
}

static XrVector3f RotateVector(const XrQuaternionf& q, const XrVector3f& v)
{
    XrVector3f qv = {q.x, q.y, q.z};
    XrVector3f t = Cross(qv, v);
    t.x *= 2.0f;
    t.y *= 2.0f;
    t.z *= 2.0f;

    XrVector3f qCrossT = Cross(qv, t);
    return {
        v.x + q.w * t.x + qCrossT.x,
        v.y + q.w * t.y + qCrossT.y,
        v.z + q.w * t.z + qCrossT.z,
    };
}

struct MetricSummary
{
    double average = 0.0;
    double p50 = 0.0;
    double p95 = 0.0;
    size_t count = 0;
};

static MetricSummary Summarize(std::vector<double>* samples)
{
    MetricSummary summary = {};
    if (samples == nullptr || samples->empty())
    {
        return summary;
    }

    std::sort(samples->begin(), samples->end());
    summary.count = samples->size();
    summary.average = std::accumulate(samples->begin(), samples->end(), 0.0) / summary.count;
    summary.p50 = (*samples)[(samples->size() - 1) / 2];
    summary.p95 = (*samples)[static_cast<size_t>(0.95 * (samples->size() - 1))];
    return summary;
}

// ─── GL helpers ───────────────────────────────────────────────────────────────

static GLuint CompileShader(GLenum type, const char* source)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        LOGE("Shader compile error: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint CreateBlitProgram(const char* fragmentShaderSource)
{
    GLuint vs = CompileShader(GL_VERTEX_SHADER, BLIT_VERTEX_SHADER);
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, fragmentShaderSource);
    if (vs == 0 || fs == 0)
    {
        return 0;
    }

    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        char log[512];
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        LOGE("Program link error: %s", log);
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

// ─── Initialization ──────────────────────────────────────────────────────────

bool XrApp::Initialize(struct android_app* app)
{
    if (!CreateInstance(app))
    {
        return false;
    }

    if (!InitEgl())
    {
        return false;
    }

    if (!CreateSession())
    {
        return false;
    }

    // Don't create swapchains yet — wait for SESSION_READY
    running_ = true;
    LOGI("XrApp initialized successfully");
    return true;
}

bool XrApp::CreateInstance(struct android_app* app)
{
    LOGI("Preferred display refresh rate target: %.1fHz", kPreferredDisplayRefreshRateHz);

    // Initialize the OpenXR loader on Android
    PFN_xrInitializeLoaderKHR initializeLoader = nullptr;
    xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
                          (PFN_xrVoidFunction*)&initializeLoader);

    if (initializeLoader != nullptr)
    {
        XrLoaderInitInfoAndroidKHR loaderInitInfo = {XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
        loaderInitInfo.applicationVM = app->activity->vm;
        loaderInitInfo.applicationContext = app->activity->clazz;
        initializeLoader((XrLoaderInitInfoBaseHeaderKHR*)&loaderInitInfo);
    }

    uint32_t extensionCount = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr);
    std::vector<XrExtensionProperties> extensionProperties(
        extensionCount, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, extensionCount, &extensionCount,
                                           extensionProperties.data());

    auto hasExtension = [&extensionProperties](const char* extensionName) {
        return std::any_of(extensionProperties.begin(), extensionProperties.end(),
                           [extensionName](const XrExtensionProperties& property) {
                               return std::strcmp(property.extensionName, extensionName) == 0;
                           });
    };

    handTrackingExtensionAvailable_ = hasExtension(XR_EXT_HAND_TRACKING_EXTENSION_NAME);
    foveationAvailable_ = hasExtension(XR_FB_FOVEATION_EXTENSION_NAME);
    foveationConfigurationAvailable_ = hasExtension(XR_FB_FOVEATION_CONFIGURATION_EXTENSION_NAME);
    swapchainUpdateAvailable_ = hasExtension(XR_FB_SWAPCHAIN_UPDATE_STATE_EXTENSION_NAME);
    displayRefreshRateAvailable_ = hasExtension(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    const bool metaTouchControllerPlusAvailable =
        hasExtension(XR_META_TOUCH_CONTROLLER_PLUS_EXTENSION_NAME);
    const bool picoControllerInteractionAvailable =
        hasExtension(XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME);

    std::vector<const char*> extensions = {
        XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
    };
    if (handTrackingExtensionAvailable_)
    {
        extensions.push_back(XR_EXT_HAND_TRACKING_EXTENSION_NAME);
    }
    if (foveationAvailable_)
    {
        extensions.push_back(XR_FB_FOVEATION_EXTENSION_NAME);
    }
    if (foveationConfigurationAvailable_)
    {
        extensions.push_back(XR_FB_FOVEATION_CONFIGURATION_EXTENSION_NAME);
    }
    if (swapchainUpdateAvailable_)
    {
        extensions.push_back(XR_FB_SWAPCHAIN_UPDATE_STATE_EXTENSION_NAME);
    }
    if (displayRefreshRateAvailable_)
    {
        extensions.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    }
    if (metaTouchControllerPlusAvailable)
    {
        extensions.push_back(XR_META_TOUCH_CONTROLLER_PLUS_EXTENSION_NAME);
    }
    if (picoControllerInteractionAvailable)
    {
        extensions.push_back(XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME);
    }
    LOGI("Enabled OpenXR extensions: %s", JoinExtensionNames(extensions).c_str());

    XrInstanceCreateInfo createInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.enabledExtensionNames = extensions.data();
    strncpy(createInfo.applicationInfo.applicationName, "OXRSys Android",
            XR_MAX_APPLICATION_NAME_SIZE);
    createInfo.applicationInfo.applicationVersion = OXRSYS_BUILD;
    createInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    strncpy(createInfo.applicationInfo.engineName, "OXRSys",
            XR_MAX_ENGINE_NAME_SIZE);
    createInfo.applicationInfo.engineVersion = 1;

    XR_CHECK(xrCreateInstance(&createInfo, &instance_), "xrCreateInstance");

    // Get the system (headset)
    XrSystemGetInfo systemGetInfo = {XR_TYPE_SYSTEM_GET_INFO};
    systemGetInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XR_CHECK(xrGetSystem(instance_, &systemGetInfo, &systemId_), "xrGetSystem");

    LOGI("OpenXR instance created, systemId = %llu", (unsigned long long)systemId_);

    XrSystemHandTrackingPropertiesEXT handTrackingProperties = {
        XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT};
    XrSystemProperties systemProperties = {XR_TYPE_SYSTEM_PROPERTIES};
    if (handTrackingExtensionAvailable_)
    {
        systemProperties.next = &handTrackingProperties;
    }
    if (XR_SUCCEEDED(xrGetSystemProperties(instance_, systemId_, &systemProperties)))
    {
        strncpy(headsetSystemName_, systemProperties.systemName, sizeof(headsetSystemName_) - 1);
        headsetSystemName_[sizeof(headsetSystemName_) - 1] = '\0';
        if (handTrackingExtensionAvailable_)
        {
            handTrackingSupported_ = handTrackingProperties.supportsHandTracking == XR_TRUE;
        }
        LOGI("OpenXR system: name='%s' vendor=%u handTracking=%d/%d",
             headsetSystemName_, systemProperties.vendorId,
             handTrackingExtensionAvailable_ ? 1 : 0,
             handTrackingSupported_ ? 1 : 0);
    }

    if (handTrackingExtensionAvailable_)
    {
        xrGetInstanceProcAddr(instance_, "xrCreateHandTrackerEXT",
                              reinterpret_cast<PFN_xrVoidFunction*>(&xrCreateHandTrackerEXT_));
        xrGetInstanceProcAddr(instance_, "xrDestroyHandTrackerEXT",
                              reinterpret_cast<PFN_xrVoidFunction*>(&xrDestroyHandTrackerEXT_));
        xrGetInstanceProcAddr(instance_, "xrLocateHandJointsEXT",
                              reinterpret_cast<PFN_xrVoidFunction*>(&xrLocateHandJointsEXT_));

        LOGI("Hand tracking support: extension=%d runtime=%d",
             handTrackingExtensionAvailable_ ? 1 : 0,
             handTrackingSupported_ ? 1 : 0);
    }

    if (foveationAvailable_ && foveationConfigurationAvailable_ && swapchainUpdateAvailable_)
    {
        xrGetInstanceProcAddr(instance_, "xrCreateFoveationProfileFB",
                              reinterpret_cast<PFN_xrVoidFunction*>(&xrCreateFoveationProfileFB_));
        xrGetInstanceProcAddr(instance_, "xrDestroyFoveationProfileFB",
                              reinterpret_cast<PFN_xrVoidFunction*>(&xrDestroyFoveationProfileFB_));
        xrGetInstanceProcAddr(instance_, "xrUpdateSwapchainFB",
                              reinterpret_cast<PFN_xrVoidFunction*>(&xrUpdateSwapchainFB_));
        LOGI("Foveation support: ext=%d config=%d swapchainUpdate=%d",
             foveationAvailable_ ? 1 : 0,
             foveationConfigurationAvailable_ ? 1 : 0,
             swapchainUpdateAvailable_ ? 1 : 0);
    }

    if (displayRefreshRateAvailable_)
    {
        xrGetInstanceProcAddr(instance_, "xrEnumerateDisplayRefreshRatesFB",
                              reinterpret_cast<PFN_xrVoidFunction*>(
                                  &xrEnumerateDisplayRefreshRatesFB_));
        xrGetInstanceProcAddr(instance_, "xrGetDisplayRefreshRateFB",
                              reinterpret_cast<PFN_xrVoidFunction*>(
                                  &xrGetDisplayRefreshRateFB_));
        xrGetInstanceProcAddr(instance_, "xrRequestDisplayRefreshRateFB",
                              reinterpret_cast<PFN_xrVoidFunction*>(
                                  &xrRequestDisplayRefreshRateFB_));
        LOGI("Display refresh rate support: ext=%d enum=%d get=%d request=%d",
             displayRefreshRateAvailable_ ? 1 : 0,
             xrEnumerateDisplayRefreshRatesFB_ != nullptr ? 1 : 0,
             xrGetDisplayRefreshRateFB_ != nullptr ? 1 : 0,
             xrRequestDisplayRefreshRateFB_ != nullptr ? 1 : 0);
    }

    // Query view configurations
    uint32_t viewCount = 0;
    xrEnumerateViewConfigurationViews(instance_, systemId_,
                                       XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                       0, &viewCount, nullptr);
    if (viewCount >= 2)
    {
        viewConfigs_[0] = {XR_TYPE_VIEW_CONFIGURATION_VIEW};
        viewConfigs_[1] = {XR_TYPE_VIEW_CONFIGURATION_VIEW};
        xrEnumerateViewConfigurationViews(instance_, systemId_,
                                           XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                           2, &viewCount, viewConfigs_);

        swapchainWidth_ = viewConfigs_[0].recommendedImageRectWidth;
        swapchainHeight_ = viewConfigs_[0].recommendedImageRectHeight;
        LOGI("View config: %ux%u per eye", swapchainWidth_, swapchainHeight_);
    }

    return true;
}

bool XrApp::InitEgl()
{
    eglDisplay_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (eglDisplay_ == EGL_NO_DISPLAY)
    {
        LOGE("eglGetDisplay failed");
        return false;
    }

    EGLint major, minor;
    if (!eglInitialize(eglDisplay_, &major, &minor))
    {
        LOGE("eglInitialize failed");
        return false;
    }
    LOGI("EGL %d.%d initialized", major, minor);

    // Choose config
    EGLint configAttribs[] = {
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 0,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_NONE
    };

    EGLint numConfigs = 0;
    eglChooseConfig(eglDisplay_, configAttribs, &eglConfig_, 1, &numConfigs);
    if (numConfigs == 0)
    {
        LOGE("eglChooseConfig: no matching config");
        return false;
    }

    // Create context
    EGLint contextAttribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 0,
        EGL_NONE
    };

    eglContext_ = eglCreateContext(eglDisplay_, eglConfig_, EGL_NO_CONTEXT, contextAttribs);
    if (eglContext_ == EGL_NO_CONTEXT)
    {
        LOGE("eglCreateContext failed: 0x%x", eglGetError());
        return false;
    }

    // Make context current with no surface (we render to XR swapchains)
    if (!eglMakeCurrent(eglDisplay_, EGL_NO_SURFACE, EGL_NO_SURFACE, eglContext_))
    {
        LOGE("eglMakeCurrent failed: 0x%x", eglGetError());
        return false;
    }

    // Load EGL/GL extension function pointers for AHardwareBuffer → EGLImage → GL texture
    eglGetNativeClientBufferANDROID_ =
        (PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC)eglGetProcAddress("eglGetNativeClientBufferANDROID");
    eglCreateImageKHR_ =
        (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    eglDestroyImageKHR_ =
        (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    glEGLImageTargetTexture2DOES_ =
        (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");

    if (!eglGetNativeClientBufferANDROID_ || !eglCreateImageKHR_ ||
        !eglDestroyImageKHR_ || !glEGLImageTargetTexture2DOES_)
    {
        LOGE("Failed to load EGL extension functions for AHardwareBuffer import");
        return false;
    }

    LOGI("EGL context created (OpenGL ES 3.0) with AHardwareBuffer extensions");
    return true;
}

bool XrApp::CreateSession()
{
    // Check OpenGL ES requirements
    XrGraphicsRequirementsOpenGLESKHR gfxReqs = {XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};

    PFN_xrGetOpenGLESGraphicsRequirementsKHR getGfxReqs = nullptr;
    xrGetInstanceProcAddr(instance_, "xrGetOpenGLESGraphicsRequirementsKHR",
                          (PFN_xrVoidFunction*)&getGfxReqs);
    if (getGfxReqs != nullptr)
    {
        getGfxReqs(instance_, systemId_, &gfxReqs);
        LOGI("OpenGL ES requirements: min %d.%d, max %d.%d",
             XR_VERSION_MAJOR(gfxReqs.minApiVersionSupported),
             XR_VERSION_MINOR(gfxReqs.minApiVersionSupported),
             XR_VERSION_MAJOR(gfxReqs.maxApiVersionSupported),
             XR_VERSION_MINOR(gfxReqs.maxApiVersionSupported));
    }

    // Create session with EGL binding
    XrGraphicsBindingOpenGLESAndroidKHR gfxBinding = {XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
    gfxBinding.display = eglDisplay_;
    gfxBinding.config = eglConfig_;
    gfxBinding.context = eglContext_;

    XrSessionCreateInfo sessionCreateInfo = {XR_TYPE_SESSION_CREATE_INFO};
    sessionCreateInfo.next = &gfxBinding;
    sessionCreateInfo.systemId = systemId_;

    XR_CHECK(xrCreateSession(instance_, &sessionCreateInfo, &session_), "xrCreateSession");
    LOGI("XrSession created with EGL binding");

    // Create reference space (STAGE preferred, LOCAL fallback)
    XrReferenceSpaceCreateInfo spaceCreateInfo = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    spaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    spaceCreateInfo.poseInReferenceSpace.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
    spaceCreateInfo.poseInReferenceSpace.position = {0.0f, 0.0f, 0.0f};

    XrResult spaceResult = xrCreateReferenceSpace(session_, &spaceCreateInfo, &appSpace_);
    if (XR_FAILED(spaceResult))
    {
        LOGW("STAGE space not available (%d), falling back to LOCAL", spaceResult);
        spaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        XR_CHECK(xrCreateReferenceSpace(session_, &spaceCreateInfo, &appSpace_),
                 "xrCreateReferenceSpace(LOCAL)");
    }
    LOGI("Reference space created");

    // Create a VIEW space for head velocity tracking
    {
        XrReferenceSpaceCreateInfo viewSpaceInfo = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        viewSpaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        viewSpaceInfo.poseInReferenceSpace.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
        viewSpaceInfo.poseInReferenceSpace.position = {0.0f, 0.0f, 0.0f};
        XrResult viewResult = xrCreateReferenceSpace(session_, &viewSpaceInfo, &viewSpace_);
        if (XR_FAILED(viewResult))
        {
            LOGW("VIEW space not available (%d) — velocity tracking disabled", viewResult);
        }
    }

    // Set up controller actions
    if (!SetupActions())
    {
        LOGW("Failed to set up controller actions — controllers won't be tracked");
    }

    if (!InitializeHandTracking())
    {
        LOGW("Hand tracking unavailable on headset, continuing without it");
    }

    if (!InitializeDisplayRefreshRate())
    {
        LOGW("Display refresh rate control unavailable, continuing at runtime default");
    }

    return true;
}

bool XrApp::InitializeDisplayRefreshRate()
{
    if (!displayRefreshRateAvailable_ || session_ == XR_NULL_HANDLE)
    {
        return false;
    }

    if (!xrEnumerateDisplayRefreshRatesFB_ || !xrRequestDisplayRefreshRateFB_)
    {
        return false;
    }

    uint32_t rateCount = 0;
    XrResult enumResult =
        xrEnumerateDisplayRefreshRatesFB_(session_, 0, &rateCount, nullptr);
    if (XR_FAILED(enumResult) || rateCount == 0)
    {
        LOGW("Failed to enumerate display refresh rates: %d (count=%u)",
             enumResult, rateCount);
        return false;
    }

    std::vector<float> refreshRates(rateCount, 0.0f);
    enumResult = xrEnumerateDisplayRefreshRatesFB_(
        session_, rateCount, &rateCount, refreshRates.data());
    if (XR_FAILED(enumResult))
    {
        LOGW("Failed to fetch display refresh rates: %d", enumResult);
        return false;
    }

    std::string supported;
    for (uint32_t i = 0; i < rateCount; ++i)
    {
        if (!supported.empty())
        {
            supported += ", ";
        }
        supported += std::to_string(refreshRates[i]);
    }
    LOGI("Supported display refresh rates: [%s]", supported.c_str());

    auto hasPreferredRate = std::any_of(refreshRates.begin(), refreshRates.end(),
                                        [](float rate) {
                                            return std::fabs(rate - kPreferredDisplayRefreshRateHz) < 0.5f;
                                        });
    if (!hasPreferredRate)
    {
        LOGW("Preferred %.1fHz not advertised by runtime, keeping current refresh rate",
             kPreferredDisplayRefreshRateHz);
        if (xrGetDisplayRefreshRateFB_ != nullptr)
        {
            float currentRate = 0.0f;
            if (XR_SUCCEEDED(xrGetDisplayRefreshRateFB_(session_, &currentRate)))
            {
                LOGI("Current display refresh rate: %.1fHz", currentRate);
            }
        }
        return true;
    }

    XrResult requestResult =
        xrRequestDisplayRefreshRateFB_(session_, kPreferredDisplayRefreshRateHz);
    if (XR_FAILED(requestResult))
    {
        LOGW("Failed to request %.1fHz display refresh rate: %d",
             kPreferredDisplayRefreshRateHz, requestResult);
        return false;
    }

    LOGI("Requested display refresh rate: %.1fHz", kPreferredDisplayRefreshRateHz);

    if (xrGetDisplayRefreshRateFB_ != nullptr)
    {
        float currentRate = 0.0f;
        if (XR_SUCCEEDED(xrGetDisplayRefreshRateFB_(session_, &currentRate)))
        {
            LOGI("Current display refresh rate after request: %.1fHz", currentRate);
        }
    }

    return true;
}

bool XrApp::InitializeHandTracking()
{
    if (!handTrackingExtensionAvailable_ || !handTrackingSupported_ ||
        !xrCreateHandTrackerEXT_ || !xrDestroyHandTrackerEXT_ || !xrLocateHandJointsEXT_)
    {
        return false;
    }

    for (int hand = 0; hand < 2; ++hand)
    {
        XrHandTrackerCreateInfoEXT createInfo = {XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT};
        createInfo.hand = hand == 0 ? XR_HAND_LEFT_EXT : XR_HAND_RIGHT_EXT;
        createInfo.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
        XrResult result = xrCreateHandTrackerEXT_(session_, &createInfo, &handTrackers_[hand]);
        if (XR_FAILED(result))
        {
            LOGW("xrCreateHandTrackerEXT failed for hand %d: %d", hand, result);
            ShutdownHandTracking();
            return false;
        }
    }

    LOGI("Hand trackers created");
    return true;
}

void XrApp::ShutdownHandTracking()
{
    for (XrHandTrackerEXT& handTracker : handTrackers_)
    {
        if (handTracker != XR_NULL_HANDLE && xrDestroyHandTrackerEXT_ != nullptr)
        {
            xrDestroyHandTrackerEXT_(handTracker);
            handTracker = XR_NULL_HANDLE;
        }
    }
}

bool XrApp::SetupActions()
{
    // Create action set
    XrActionSetCreateInfo actionSetInfo = {XR_TYPE_ACTION_SET_CREATE_INFO};
    strncpy(actionSetInfo.actionSetName, "streaming", XR_MAX_ACTION_SET_NAME_SIZE);
    strncpy(actionSetInfo.localizedActionSetName, "Streaming", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE);
    XR_CHECK(xrCreateActionSet(instance_, &actionSetInfo, &actionSet_), "xrCreateActionSet");

    // Get hand paths
    xrStringToPath(instance_, "/user/hand/left", &handPaths_[0]);
    xrStringToPath(instance_, "/user/hand/right", &handPaths_[1]);

    // Grip pose action (both hands)
    XrActionCreateInfo actionInfo = {XR_TYPE_ACTION_CREATE_INFO};
    actionInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
    strncpy(actionInfo.actionName, "grip_pose", XR_MAX_ACTION_NAME_SIZE);
    strncpy(actionInfo.localizedActionName, "Grip Pose", XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
    actionInfo.countSubactionPaths = 2;
    actionInfo.subactionPaths = handPaths_;
    XR_CHECK(xrCreateAction(actionSet_, &actionInfo, &gripPoseAction_), "xrCreateAction(gripPose)");

    // Trigger action (float, both hands)
    actionInfo.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
    strncpy(actionInfo.actionName, "trigger", XR_MAX_ACTION_NAME_SIZE);
    strncpy(actionInfo.localizedActionName, "Trigger", XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
    XR_CHECK(xrCreateAction(actionSet_, &actionInfo, &triggerAction_), "xrCreateAction(trigger)");

    // Grip (squeeze) action (float, both hands)
    strncpy(actionInfo.actionName, "grip", XR_MAX_ACTION_NAME_SIZE);
    strncpy(actionInfo.localizedActionName, "Grip", XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
    XR_CHECK(xrCreateAction(actionSet_, &actionInfo, &gripAction_), "xrCreateAction(grip)");

    // Thumbstick action (vec2, both hands)
    actionInfo.actionType = XR_ACTION_TYPE_VECTOR2F_INPUT;
    strncpy(actionInfo.actionName, "thumbstick", XR_MAX_ACTION_NAME_SIZE);
    strncpy(actionInfo.localizedActionName, "Thumbstick", XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
    XR_CHECK(xrCreateAction(actionSet_, &actionInfo, &thumbstickAction_), "xrCreateAction(thumbstick)");

    // A/X button (boolean, both hands)
    actionInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    strncpy(actionInfo.actionName, "a_button", XR_MAX_ACTION_NAME_SIZE);
    strncpy(actionInfo.localizedActionName, "A/X Button", XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
    XR_CHECK(xrCreateAction(actionSet_, &actionInfo, &aButtonAction_), "xrCreateAction(a_button)");

    // B/Y button (boolean, both hands)
    strncpy(actionInfo.actionName, "b_button", XR_MAX_ACTION_NAME_SIZE);
    strncpy(actionInfo.localizedActionName, "B/Y Button", XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
    XR_CHECK(xrCreateAction(actionSet_, &actionInfo, &bButtonAction_), "xrCreateAction(b_button)");

    // Menu button (boolean, left hand only)
    actionInfo.countSubactionPaths = 1;
    actionInfo.subactionPaths = &handPaths_[0]; // left hand
    strncpy(actionInfo.actionName, "menu", XR_MAX_ACTION_NAME_SIZE);
    strncpy(actionInfo.localizedActionName, "Menu", XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
    XR_CHECK(xrCreateAction(actionSet_, &actionInfo, &menuAction_), "xrCreateAction(menu)");

    XrPath bindingPaths[13];
    xrStringToPath(instance_, "/user/hand/left/input/grip/pose", &bindingPaths[0]);
    xrStringToPath(instance_, "/user/hand/right/input/grip/pose", &bindingPaths[1]);
    xrStringToPath(instance_, "/user/hand/left/input/trigger/value", &bindingPaths[2]);
    xrStringToPath(instance_, "/user/hand/right/input/trigger/value", &bindingPaths[3]);
    xrStringToPath(instance_, "/user/hand/left/input/squeeze/value", &bindingPaths[4]);
    xrStringToPath(instance_, "/user/hand/right/input/squeeze/value", &bindingPaths[5]);
    xrStringToPath(instance_, "/user/hand/left/input/thumbstick", &bindingPaths[6]);
    xrStringToPath(instance_, "/user/hand/right/input/thumbstick", &bindingPaths[7]);
    xrStringToPath(instance_, "/user/hand/left/input/x/click", &bindingPaths[8]);
    xrStringToPath(instance_, "/user/hand/right/input/a/click", &bindingPaths[9]);
    xrStringToPath(instance_, "/user/hand/left/input/y/click", &bindingPaths[10]);
    xrStringToPath(instance_, "/user/hand/right/input/b/click", &bindingPaths[11]);
    xrStringToPath(instance_, "/user/hand/left/input/menu/click", &bindingPaths[12]);

    XrActionSuggestedBinding bindings[] = {
        {gripPoseAction_, bindingPaths[0]},
        {gripPoseAction_, bindingPaths[1]},
        {triggerAction_, bindingPaths[2]},
        {triggerAction_, bindingPaths[3]},
        {gripAction_, bindingPaths[4]},
        {gripAction_, bindingPaths[5]},
        {thumbstickAction_, bindingPaths[6]},
        {thumbstickAction_, bindingPaths[7]},
        {aButtonAction_, bindingPaths[8]},
        {aButtonAction_, bindingPaths[9]},
        {bButtonAction_, bindingPaths[10]},
        {bButtonAction_, bindingPaths[11]},
        {menuAction_, bindingPaths[12]},
    };

    const char* controllerProfiles[] = {
        "/interaction_profiles/oculus/touch_controller",
        "/interaction_profiles/meta/touch_controller_quest_1_rift_s",
        "/interaction_profiles/meta/touch_controller_quest_2",
        "/interaction_profiles/meta/touch_plus_controller",
        "/interaction_profiles/meta/touch_controller_plus",
        "/interaction_profiles/bytedance/pico_neo3_controller",
        "/interaction_profiles/bytedance/pico4_controller",
    };

    uint32_t acceptedProfiles = 0;
    for (const char* controllerProfile : controllerProfiles)
    {
        XrPath profilePath = XR_NULL_PATH;
        XrResult pathResult = xrStringToPath(instance_, controllerProfile, &profilePath);
        if (XR_FAILED(pathResult))
        {
            LOGW("Controller profile path unavailable: %s result=%d",
                 controllerProfile, pathResult);
            continue;
        }

        XrInteractionProfileSuggestedBinding suggestedBindings = {
            XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        suggestedBindings.interactionProfile = profilePath;
        suggestedBindings.suggestedBindings = bindings;
        suggestedBindings.countSuggestedBindings = sizeof(bindings) / sizeof(bindings[0]);
        XrResult suggestResult = xrSuggestInteractionProfileBindings(instance_, &suggestedBindings);
        if (XR_SUCCEEDED(suggestResult))
        {
            ++acceptedProfiles;
            LOGI("Controller bindings accepted for %s", controllerProfile);
        }
        else
        {
            LOGW("Controller bindings unsupported for %s result=%d",
                 controllerProfile, suggestResult);
        }
    }
    if (acceptedProfiles == 0)
    {
        LOGE("No controller interaction profile accepted");
        return false;
    }

    // Create grip pose spaces for each hand
    for (int hand = 0; hand < 2; hand++)
    {
        XrActionSpaceCreateInfo spaceInfo = {XR_TYPE_ACTION_SPACE_CREATE_INFO};
        spaceInfo.action = gripPoseAction_;
        spaceInfo.subactionPath = handPaths_[hand];
        spaceInfo.poseInActionSpace.orientation = {0, 0, 0, 1};
        spaceInfo.poseInActionSpace.position = {0, 0, 0};
        XR_CHECK(xrCreateActionSpace(session_, &spaceInfo, &gripSpaces_[hand]),
                 "xrCreateActionSpace(grip)");
    }

    LOGI("Controller actions set up (%u profile suggestions accepted)", acceptedProfiles);
    return true;
}

bool XrApp::CreateSwapchains()
{
    for (int eye = 0; eye < 2; eye++)
    {
        XrSwapchainCreateInfo swapchainCreateInfo = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
        XrSwapchainCreateInfoFoveationFB foveationCreateInfo = {
            XR_TYPE_SWAPCHAIN_CREATE_INFO_FOVEATION_FB};
        if (foveationAvailable_ && foveationConfigurationAvailable_ && swapchainUpdateAvailable_)
        {
            foveationCreateInfo.flags = XR_SWAPCHAIN_CREATE_FOVEATION_SCALED_BIN_BIT_FB;
            swapchainCreateInfo.next = &foveationCreateInfo;
        }
        swapchainCreateInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                                          XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        swapchainCreateInfo.format = GL_SRGB8_ALPHA8;
        swapchainCreateInfo.sampleCount = 1;
        swapchainCreateInfo.width = swapchainWidth_;
        swapchainCreateInfo.height = swapchainHeight_;
        swapchainCreateInfo.faceCount = 1;
        swapchainCreateInfo.arraySize = 1;
        swapchainCreateInfo.mipCount = 1;

        XR_CHECK(xrCreateSwapchain(session_, &swapchainCreateInfo, &swapchains_[eye]),
                 "xrCreateSwapchain");

        // Enumerate swapchain images
        uint32_t imageCount = 0;
        xrEnumerateSwapchainImages(swapchains_[eye], 0, &imageCount, nullptr);

        std::vector<XrSwapchainImageOpenGLESKHR> images(imageCount,
                                                         {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
        xrEnumerateSwapchainImages(swapchains_[eye], imageCount, &imageCount,
                                    (XrSwapchainImageBaseHeader*)images.data());

        swapchainImages_[eye].resize(imageCount);
        for (uint32_t i = 0; i < imageCount; i++)
        {
            swapchainImages_[eye][i] = images[i].image;
        }

        LOGI("Eye %d swapchain: %ux%u, %u images", eye, swapchainWidth_, swapchainHeight_, imageCount);
    }

    // Create GL resources for video blit (uses samplerExternalOES for GPU YUV→RGB)
    blitProgram_ = CreateBlitProgram(BLIT_FRAGMENT_SHADER_OES);
    if (blitProgram_ == 0)
    {
        LOGE("Failed to create blit shader program with external OES sampler");
        return false;
    }

    // Full-screen quad
    float quadVertices[] = {
        // pos x,y    uv u,v
        -1.0f, -1.0f,  0.0f, 0.0f,
         1.0f, -1.0f,  1.0f, 0.0f,
         1.0f,  1.0f,  1.0f, 1.0f,
        -1.0f, -1.0f,  0.0f, 0.0f,
         1.0f,  1.0f,  1.0f, 1.0f,
        -1.0f,  1.0f,  0.0f, 1.0f,
    };

    glGenVertexArrays(1, &blitVao_);
    glGenBuffers(1, &blitVbo_);
    glBindVertexArray(blitVao_);
    glBindBuffer(GL_ARRAY_BUFFER, blitVbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), quadVertices, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glBindVertexArray(0);

    glGenFramebuffers(1, &fbo_);

    // Create video texture as GL_TEXTURE_EXTERNAL_OES
    // (bound to decoded video frames via EGLImage from AHardwareBuffer)
    glGenTextures(1, &videoTexture_);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, videoTexture_);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);

    LOGI("GL resources created for video rendering (external OES texture)");

    if (!InitializeFoveation())
    {
        LOGW("Foveation unavailable on headset, continuing without it");
    }

    // Start networking after swapchains are ready
    StartNetworking();

    return true;
}

bool XrApp::InitializeFoveation()
{
    if (!foveationAvailable_ || !foveationConfigurationAvailable_ || !swapchainUpdateAvailable_ ||
        !xrCreateFoveationProfileFB_ || !xrDestroyFoveationProfileFB_ || !xrUpdateSwapchainFB_)
    {
        return false;
    }

    XrFoveationLevelProfileCreateInfoFB levelProfile = {
        XR_TYPE_FOVEATION_LEVEL_PROFILE_CREATE_INFO_FB};
    levelProfile.level = XR_FOVEATION_LEVEL_MEDIUM_FB;
    levelProfile.verticalOffset = 0.0f;
    levelProfile.dynamic = XR_FOVEATION_DYNAMIC_LEVEL_ENABLED_FB;

    XrFoveationProfileCreateInfoFB createInfo = {XR_TYPE_FOVEATION_PROFILE_CREATE_INFO_FB};
    createInfo.next = &levelProfile;

    XrResult profileResult = xrCreateFoveationProfileFB_(session_, &createInfo, &foveationProfile_);
    if (XR_FAILED(profileResult))
    {
        LOGW("xrCreateFoveationProfileFB failed: %d", profileResult);
        foveationProfile_ = XR_NULL_HANDLE;
        return false;
    }

    for (int eye = 0; eye < 2; ++eye)
    {
        if (swapchains_[eye] == XR_NULL_HANDLE)
        {
            continue;
        }

        XrSwapchainStateFoveationFB state = {XR_TYPE_SWAPCHAIN_STATE_FOVEATION_FB};
        state.profile = foveationProfile_;
        XrResult updateResult = xrUpdateSwapchainFB_(
            swapchains_[eye],
            reinterpret_cast<const XrSwapchainStateBaseHeaderFB*>(&state));
        if (XR_FAILED(updateResult))
        {
            LOGW("xrUpdateSwapchainFB failed for eye %d: %d", eye, updateResult);
            ShutdownFoveation();
            return false;
        }
    }

    LOGI("Dynamic FFR enabled via XR_FB_foveation");
    return true;
}

void XrApp::ShutdownFoveation()
{
    if (foveationProfile_ != XR_NULL_HANDLE && xrDestroyFoveationProfileFB_ != nullptr)
    {
        xrDestroyFoveationProfileFB_(foveationProfile_);
        foveationProfile_ = XR_NULL_HANDLE;
    }
}

// ─── Networking ───────────────────────────────────────────────────────────────

void XrApp::StartNetworking()
{
    if (networkReceiver_ || trackingSender_ || videoDecoder_)
    {
        ResetConnection("restarting networking");
    }

    networkReceiver_ = std::make_unique<NetworkReceiver>();
    trackingSender_ = std::make_unique<TrackingSender>();
    videoDecoder_ = std::make_unique<VideoDecoder>();
    lastLatencyReportTime_ = {};
    lastKeyframeRequestTime_ = {};
    lastObservedDroppedFrames_ = 0;
    latencySamples_ = {};
    lastFrameReceiveTimeNs_ = 0;
    lastFrameSubmitTimeNs_ = 0;
    lastFrameAcquireTimeNs_ = 0;
    lastReportedAcquireTimeNs_ = 0;
    skippedDecodedFrames_ = 0;
    transportMode_ = TransportMode::WifiUdp;
    lastUsbAdbRetryTime_ = {};
    usbAdbRetryAttempts_ = 0;
    connectionState_.store(ConnectionState::Disconnected);
    needsReconnect_.store(false);

    if (TryStartUsbAdbTransport())
    {
        LOGI("USB ADB transport selected");
        return;
    }
    lastUsbAdbRetryTime_ = std::chrono::steady_clock::now();

    LOGI("USB ADB transport unavailable, starting WiFi discovery mode");
    connectionState_.store(ConnectionState::Discovering);

    networkReceiver_->StartDiscovery(
        [this](const protocol::ServerAnnounce& server, const char* serverIp) {
            OnServerFound(server, serverIp);
        });

    LOGI("Network discovery started, listening for server broadcasts on port %d",
         protocol::DISCOVERY_PORT);
}

void XrApp::StopNetworking()
{
    ResetConnection("networking stopped");
}

void XrApp::ResetConnection(const char* reason)
{
    LOGI("Resetting connection: %s", reason != nullptr ? reason : "unspecified");
    CloseControlSocket();
    if (networkReceiver_)
    {
        networkReceiver_->Stop();
        networkReceiver_.reset();
    }
    if (trackingSender_)
    {
        trackingSender_->Disconnect();
        trackingSender_.reset();
    }
    if (videoDecoder_)
    {
        videoDecoder_->Shutdown();
        videoDecoder_.reset();
    }

    transportMode_ = TransportMode::WifiUdp;
    connectionState_.store(ConnectionState::Disconnected);
    needsReconnect_.store(false);
    serverIp_[0] = '\0';
    serverVideoPort_ = 0;
    serverTrackingPort_ = 0;
    connectionTime_ = {};
    lastUsbAdbRetryTime_ = {};
    usbAdbRetryAttempts_ = 0;

    videoWidth_ = 0;
    videoHeight_ = 0;
    blitWidth_ = 0;
    blitHeight_ = 0;
    blitOffsetX_ = 0;
    blitOffsetY_ = 0;
    macEyeAspect_ = 0.0f;
    hasVideoTexture_ = false;
    hasCurrentRenderPose_ = false;
    currentRenderPose_ = {};
    lastVideoFrameTime_ = {};
    videoContentUMin_ = 0.0f;
    videoContentUMax_ = 1.0f;
    videoContentVMin_ = 0.0f;
    videoContentVMax_ = 1.0f;
    lastFrameReceiveTimeNs_ = 0;
    lastFrameSubmitTimeNs_ = 0;
    lastFrameAcquireTimeNs_ = 0;
    lastReportedAcquireTimeNs_ = 0;
    skippedDecodedFrames_ = 0;
    renderPoseHitCount_ = 0;
    renderPoseMissCount_ = 0;
    lastRenderPoseLogTime_ = {};
    lastLatencyReportTime_ = {};
    lastKeyframeRequestTime_ = {};
    lastObservedDroppedFrames_ = 0;
    latencySamples_ = {};
    nalUnitsReceived_ = 0;
    decodedFrameCount_ = 0;
}

void XrApp::OnConnectionLost(const char* reason)
{
    ConnectionState state = connectionState_.load();
    if (state == ConnectionState::Disconnected || state == ConnectionState::Discovering)
    {
        return;
    }

    LOGI("Connection lost: %s", reason != nullptr ? reason : "unspecified");
    connectionState_.store(ConnectionState::Disconnected);
    needsReconnect_.store(true);
}

bool XrApp::OpenControlSocket(const char* serverIp)
{
    CloseControlSocket();

    controlSocket_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (controlSocket_ < 0)
    {
        LOGE("Failed to create control socket");
        return false;
    }

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(protocol::CONTROL_PORT);
    inet_pton(AF_INET, serverIp, &addr.sin_addr);

    if (connect(controlSocket_, (sockaddr*)&addr, sizeof(addr)) < 0)
    {
        LOGE("Failed to connect control socket to %s:%d", serverIp, protocol::CONTROL_PORT);
        close(controlSocket_);
        controlSocket_ = -1;
        return false;
    }

    return true;
}

bool XrApp::OpenUsbControlSocket()
{
    if (controlTcpSocket_ >= 0)
    {
        close(controlTcpSocket_);
        controlTcpSocket_ = -1;
    }

    controlTcpSocket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (controlTcpSocket_ < 0)
    {
        LOGE("Failed to create USB TCP control socket");
        return false;
    }
    ConfigureTcpSocket(controlTcpSocket_);
    timeval timeout = {0, 250000};
    setsockopt(controlTcpSocket_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(protocol::CONTROL_PORT);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(controlTcpSocket_, (sockaddr*)&addr, sizeof(addr)) < 0)
    {
        close(controlTcpSocket_);
        controlTcpSocket_ = -1;
        return false;
    }

    return true;
}

void XrApp::CloseControlSocket()
{
    if (controlSocket_ >= 0)
    {
        protocol::MessageType disconnect = protocol::MessageType::ServerDisconnect;
        send(controlSocket_, &disconnect, sizeof(disconnect), MSG_DONTWAIT);
        close(controlSocket_);
        controlSocket_ = -1;
    }
    if (controlTcpSocket_ >= 0)
    {
        SendTcpRecord(controlTcpSocket_, protocol::TcpRecordType::Disconnect, nullptr, 0);
        close(controlTcpSocket_);
        controlTcpSocket_ = -1;
    }
}

void XrApp::OnServerFound(const protocol::ServerAnnounce& server, const char* serverIp)
{
    ConfigureServerConnection(server, serverIp, TransportMode::WifiUdp);
}

bool XrApp::TryStartUsbAdbTransport(bool logUnavailable)
{
    if (IsConnected() || needsReconnect_.load())
    {
        return false;
    }

    ConnectionState previousState = connectionState_.load();
    if (!OpenUsbControlSocket())
    {
        if (logUnavailable)
        {
            LOGI("USB ADB control socket not available; adb reverse may not be configured");
        }
        connectionState_.store(previousState);
        return false;
    }

    protocol::TcpRecordHeader header = {};
    std::vector<uint8_t> payload;
    if (!ReadTcpRecord(controlTcpSocket_, header, payload) ||
        header.type != protocol::TcpRecordType::ServerAnnounce ||
        payload.size() < sizeof(protocol::ServerAnnounce))
    {
        if (logUnavailable)
        {
            LOGW("USB ADB control connected but no valid ServerAnnounce was received");
        }
        close(controlTcpSocket_);
        controlTcpSocket_ = -1;
        connectionState_.store(previousState);
        return false;
    }

    auto server = *reinterpret_cast<const protocol::ServerAnnounce*>(payload.data());
    ConfigureServerConnection(server, "127.0.0.1", TransportMode::UsbAdbTcp);
    return IsConnected();
}

void XrApp::RetryUsbAdbTransportIfNeeded()
{
    if (IsConnected() || needsReconnect_.load())
    {
        return;
    }
    if (!networkReceiver_ || !trackingSender_ || !videoDecoder_)
    {
        return;
    }

    auto now = std::chrono::steady_clock::now();
    if (lastUsbAdbRetryTime_.time_since_epoch().count() != 0 &&
        now - lastUsbAdbRetryTime_ < kUsbAdbRetryInterval)
    {
        return;
    }

    lastUsbAdbRetryTime_ = now;
    ++usbAdbRetryAttempts_;
    bool logAttempt = usbAdbRetryAttempts_ <= 3 ||
                      usbAdbRetryAttempts_ % kUsbAdbRetryLogInterval == 0;
    if (logAttempt)
    {
        LOGI("Retrying USB ADB transport (attempt %u)", usbAdbRetryAttempts_);
    }

    if (TryStartUsbAdbTransport(logAttempt))
    {
        LOGI("USB ADB transport selected after retry");
    }
}

void XrApp::ConfigureServerConnection(const protocol::ServerAnnounce& server,
                                      const char* serverIp, TransportMode transportMode)
{
    ConnectionState state = connectionState_.load();
    if (state == ConnectionState::Connected)
    {
        // Already connected — check if this is just a leftover broadcast from the same server.
        // Race condition: server may send a few more broadcasts before receiving our ClientConnect.
        // Only trigger reconnect if we've been connected for a while (> 3 seconds).
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - connectionTime_).count();

        if (elapsed < 3)
        {
            // Ignore — likely a duplicate broadcast during initial handshake
            return;
        }

        // Server is re-broadcasting after we've been connected — session must have restarted
        LOGI("Server re-broadcasting from %s after %lld seconds, flagging reconnect",
             serverIp, (long long)elapsed);
        needsReconnect_.store(true);
        return;
    }
    if (state == ConnectionState::Connecting)
    {
        return;
    }
    if (!connectionState_.compare_exchange_strong(state, ConnectionState::Connecting))
    {
        return;
    }

    transportMode_ = transportMode;
    const bool usbAdb = transportMode_ == TransportMode::UsbAdbTcp;

    LOGI("Server discovered via %s: %s at %s (%ux%u @ %uHz, encoded %ux%u)",
         usbAdb ? "USB ADB" : "WiFi",
         server.serverName, serverIp, server.renderWidth, server.renderHeight,
         server.refreshRateHz, server.encodedWidth, server.encodedHeight);

    serverVideoPort_ = server.videoPort;
    serverTrackingPort_ = server.trackingPort;
    videoWidth_ = server.renderWidth;
    videoHeight_ = server.renderHeight;
    strncpy(serverIp_, serverIp, sizeof(serverIp_) - 1);
    serverIp_[sizeof(serverIp_) - 1] = '\0';
    connectionTime_ = std::chrono::steady_clock::now();

    // Compute aspect-ratio-correct blit viewport within the Quest swapchain.
    // Mac renders stereo side-by-side: per-eye width = renderWidth / 2.
    macEyeAspect_ = static_cast<float>(server.renderWidth / 2) /
                     static_cast<float>(server.renderHeight);
    float questAspect = static_cast<float>(swapchainWidth_) /
                        static_cast<float>(swapchainHeight_);

    if (macEyeAspect_ <= questAspect)
    {
        // Mac is taller (narrower) — fit height, pillarbox sides
        blitHeight_ = swapchainHeight_;
        blitWidth_ = static_cast<uint32_t>(swapchainHeight_ * macEyeAspect_ + 0.5f);
        blitOffsetX_ = static_cast<int32_t>((swapchainWidth_ - blitWidth_) / 2);
        blitOffsetY_ = 0;
    }
    else
    {
        // Mac is wider — fit width, letterbox top/bottom
        blitWidth_ = swapchainWidth_;
        blitHeight_ = static_cast<uint32_t>(swapchainWidth_ / macEyeAspect_ + 0.5f);
        blitOffsetX_ = 0;
        blitOffsetY_ = static_cast<int32_t>((swapchainHeight_ - blitHeight_) / 2);
    }

    LOGI("Blit viewport: %ux%u at offset (%d,%d) in %ux%u swapchain "
         "(macAspect=%.3f, questAspect=%.3f)",
         blitWidth_, blitHeight_, blitOffsetX_, blitOffsetY_,
         swapchainWidth_, swapchainHeight_, macEyeAspect_, questAspect);

    // Use encoded resolution for decoder (may differ from render resolution due to scaling)
    // Fallback to render resolution if encodedWidth is 0 (old server without this field)
    uint32_t decoderWidth = server.encodedWidth > 0 ? server.encodedWidth : server.renderWidth;
    uint32_t decoderHeight = server.encodedHeight > 0 ? server.encodedHeight : server.renderHeight;

    // CRITICAL: Start video receiver and decoder BEFORE sending ClientConnect.
    // The server starts encoding immediately upon receiving ClientConnect.
    // If we send ClientConnect first, the initial keyframe packets arrive at port 9944
    // before we've bound the socket, and they're silently dropped by the OS.

    // Initialize video decoder at the actual encoded resolution
    if (videoDecoder_ && !videoDecoder_->IsInitialized())
    {
        if (videoDecoder_->Initialize(decoderWidth, decoderHeight))
        {
            LOGI("Video decoder initialized: %ux%u (encoded), render %ux%u",
                 decoderWidth, decoderHeight, videoWidth_, videoHeight_);
        }
        else
        {
            LOGE("Failed to initialize video decoder");
        }
    }

    // Start receiving video packets (bind socket BEFORE telling server we're ready)
    bool receivingStarted = false;
    if (networkReceiver_)
    {
        auto nalCallback = [this](const uint8_t* data, size_t size,
                                  int64_t timestampNs, int64_t receiveTimeNs)
        {
            OnNalUnitReceived(data, size, timestampNs, receiveTimeNs);
        };
        if (usbAdb)
        {
            receivingStarted = networkReceiver_->StartReceivingTcp(
                serverVideoPort_, nalCallback,
                [this](const char* reason) { OnConnectionLost(reason); });
        }
        else
        {
            receivingStarted = networkReceiver_->StartReceiving(
                serverIp, serverVideoPort_, nalCallback,
                [this](const char* reason) { OnConnectionLost(reason); });
        }
    }
    if (!receivingStarted)
    {
        LOGE("Failed to start %s video receiver", usbAdb ? "USB ADB TCP" : "WiFi UDP");
        transportMode_ = TransportMode::WifiUdp;
        connectionState_.store(ConnectionState::Disconnected);
        if (usbAdb && controlTcpSocket_ >= 0)
        {
            close(controlTcpSocket_);
            controlTcpSocket_ = -1;
        }
        return;
    }

    // Connect tracking sender to server
    if (trackingSender_)
    {
        bool trackingConnected = usbAdb
            ? trackingSender_->ConnectTcp(serverTrackingPort_)
            : trackingSender_->Connect(serverIp, serverTrackingPort_);
        if (trackingConnected)
        {
            LOGI("Tracking sender connected via %s to %s:%d",
                 usbAdb ? "USB ADB TCP" : "WiFi UDP", serverIp, serverTrackingPort_);
        }
        else
        {
            LOGE("Failed to connect tracking sender");
        }
    }

    if (!usbAdb && !OpenControlSocket(serverIp))
    {
        LOGE("Failed to open control socket");
    }
    else if (!usbAdb && networkReceiver_)
    {
        // Share control socket with NetworkReceiver for NACK sending
        networkReceiver_->SetControlSocket(controlSocket_, serverIp);
    }

    // NOW send ClientConnect — server will start sending video, and we're already listening
    SendClientConnect(serverIp);
    connectionState_.store(ConnectionState::Connected);
    if (networkReceiver_)
    {
        networkReceiver_->StopDiscovery();
    }

    LOGI("Connection setup complete via %s — video receiver ready before ClientConnect sent",
         usbAdb ? "USB ADB" : "WiFi");
}

void XrApp::SendClientConnect(const char* serverIp)
{
    const bool usbAdb = transportMode_ == TransportMode::UsbAdbTcp;
    if (!usbAdb && controlSocket_ < 0 && !OpenControlSocket(serverIp))
    {
        return;
    }
    if (usbAdb && controlTcpSocket_ < 0)
    {
        return;
    }

    protocol::ClientConnect connect = {};
    connect.type = protocol::MessageType::ClientConnect;
    connect.versionMajor = 1;
    connect.versionMinor = 0;
    connect.preferredCodec = static_cast<uint32_t>(protocol::VideoCodec::H265);
    connect.maxBitrateMbps = 100;
    connect.refreshRateHz = clientRefreshRateHz_;
    strncpy(connect.deviceName, headsetSystemName_, sizeof(connect.deviceName) - 1);
    connect.deviceName[sizeof(connect.deviceName) - 1] = '\0';

    if (usbAdb)
    {
        SendTcpRecord(controlTcpSocket_, protocol::TcpRecordType::ClientConnect,
                      &connect, sizeof(connect));
    }
    else
    {
        send(controlSocket_, &connect, sizeof(connect), MSG_DONTWAIT);
    }

    LOGI("Sent ClientConnect via %s to %s:%d (device='%s' refresh=%uHz)",
         usbAdb ? "USB ADB" : "WiFi", serverIp, protocol::CONTROL_PORT,
         connect.deviceName, clientRefreshRateHz_);
}

void XrApp::SendLatencyReport()
{
    const bool usbAdb = transportMode_ == TransportMode::UsbAdbTcp;
    if ((!usbAdb && controlSocket_ < 0) || (usbAdb && controlTcpSocket_ < 0))
    {
        return;
    }

    auto now = std::chrono::steady_clock::now();
    if (lastLatencyReportTime_.time_since_epoch().count() != 0 &&
        now - lastLatencyReportTime_ < std::chrono::seconds(1))
    {
        return;
    }

    MetricSummary receiveSummary = Summarize(&latencySamples_.receiveToSubmitMs);
    MetricSummary decodeSummary = Summarize(&latencySamples_.submitToDecodeMs);
    MetricSummary compositorSummary = Summarize(&latencySamples_.decodeToCompositorMs);
    MetricSummary totalSummary = Summarize(&latencySamples_.totalClientMs);
    MetricSummary ageSummary = Summarize(&latencySamples_.frameAgeMs);

    if (receiveSummary.count == 0 && decodeSummary.count == 0 &&
        compositorSummary.count == 0 && totalSummary.count == 0)
    {
        return;
    }

    protocol::LatencyReport report = {};
    report.type = protocol::ControlType::LatencyReport;
    report.receiveToDecoderSubmitMs = static_cast<float>(receiveSummary.average);
    report.decodeLatencyMs = static_cast<float>(decodeSummary.average);
    report.compositorLatencyMs = static_cast<float>(compositorSummary.average);
    report.totalClientLatencyMs = static_cast<float>(totalSummary.average);
    if (usbAdb)
    {
        SendTcpRecord(controlTcpSocket_, protocol::TcpRecordType::Control,
                      &report, sizeof(report));
    }
    else
    {
        send(controlSocket_, &report, sizeof(report), MSG_DONTWAIT);
    }

    LOGI("Latency report: recv->submit avg/p95=%.2f/%.2fms decode=%.2f/%.2f compositor=%.2f/%.2f total=%.2f/%.2f age=%.2f/%.2f skipped=%u",
         receiveSummary.average, receiveSummary.p95,
         decodeSummary.average, decodeSummary.p95,
         compositorSummary.average, compositorSummary.p95,
         totalSummary.average, totalSummary.p95,
         ageSummary.average, ageSummary.p95,
         skippedDecodedFrames_);

    latencySamples_.receiveToSubmitMs.clear();
    latencySamples_.submitToDecodeMs.clear();
    latencySamples_.decodeToCompositorMs.clear();
    latencySamples_.totalClientMs.clear();
    latencySamples_.frameAgeMs.clear();
    skippedDecodedFrames_ = 0;
    lastLatencyReportTime_ = now;
}

void XrApp::RequestKeyframe(uint32_t reasonFlags, uint32_t detail)
{
    const bool usbAdb = transportMode_ == TransportMode::UsbAdbTcp;
    if ((!usbAdb && controlSocket_ < 0) || (usbAdb && controlTcpSocket_ < 0))
    {
        return;
    }

    auto now = std::chrono::steady_clock::now();
    if (lastKeyframeRequestTime_.time_since_epoch().count() != 0 &&
        now - lastKeyframeRequestTime_ < std::chrono::milliseconds(100))
    {
        return;
    }

    protocol::RequestKeyframe request = {};
    request.type = protocol::ControlType::RequestKeyframe;
    request.reasonFlags = reasonFlags;
    request.detail = detail;
    if (usbAdb)
    {
        SendTcpRecord(controlTcpSocket_, protocol::TcpRecordType::Control,
                      &request, sizeof(request));
    }
    else
    {
        send(controlSocket_, &request, sizeof(request), MSG_DONTWAIT);
    }
    lastKeyframeRequestTime_ = now;

    LOGW("Requested keyframe reasons=0x%x detail=%u", reasonFlags, detail);
}

float XrApp::GetCurrentRefreshRateHz() const
{
    return static_cast<float>(clientRefreshRateHz_);
}

void XrApp::OnNalUnitReceived(const uint8_t* data, size_t size,
                              int64_t timestampNs, int64_t receiveTimeNs)
{
    nalUnitsReceived_++;

    if (nalUnitsReceived_ <= 10 || nalUnitsReceived_ % 300 == 0)
    {
        // Log NAL unit type for H.265 (type is in bits 1-6 of second byte after start code)
        const char* nalType = "unknown";
        if (size > 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1)
        {
            uint8_t nalTypeId = (data[4] >> 1) & 0x3F;
            switch (nalTypeId)
            {
                case 32: nalType = "VPS"; break;
                case 33: nalType = "SPS"; break;
                case 34: nalType = "PPS"; break;
                case 19: case 20: nalType = "IDR"; break;
                case 1: nalType = "P-slice"; break;
                default: nalType = "other"; break;
            }
        }
        LOGI("NAL unit #%u: size=%zu type=%s ts=%lld",
             nalUnitsReceived_, size, nalType, (long long)timestampNs);
    }

    if (videoDecoder_ && videoDecoder_->IsInitialized())
    {
        bool submitted = videoDecoder_->SubmitNalUnit(data, size, timestampNs / 1000, receiveTimeNs);
        if (!submitted && nalUnitsReceived_ <= 10)
        {
            LOGW("Failed to submit NAL unit #%u to decoder (no input buffer available)",
                 nalUnitsReceived_);
        }
    }
    else if (nalUnitsReceived_ <= 5)
    {
        LOGW("NAL unit received but decoder not initialized");
    }
}

// ─── Frame loop ──────────────────────────────────────────────────────────────

void XrApp::RunFrame()
{
    if (!running_)
    {
        return;
    }

    // Check if we need to reconnect (server restarted)
    if (needsReconnect_.load())
    {
        needsReconnect_.store(false);
        LOGI("Reconnecting to server...");
        ResetConnection("connection lost");
        StartNetworking();
    }

    RetryUsbAdbTransportIfNeeded();

    // Poll OpenXR events
    XrEventDataBuffer event = {XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(instance_, &event) == XR_SUCCESS)
    {
        switch (event.type)
        {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
        {
            auto* stateEvent = (XrEventDataSessionStateChanged*)&event;
            HandleSessionStateChange(stateEvent->state);
            break;
        }
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            running_ = false;
            break;
        default:
            break;
        }
        event = {XR_TYPE_EVENT_DATA_BUFFER};
    }

    if (!sessionRunning_)
    {
        return;
    }

    // xrWaitFrame
    XrFrameWaitInfo waitInfo = {XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState frameState = {XR_TYPE_FRAME_STATE};
    XrResult waitResult = xrWaitFrame(session_, &waitInfo, &frameState);
    if (XR_FAILED(waitResult))
    {
        LOGE("xrWaitFrame failed: %d", waitResult);
        return;
    }

    if (frameState.predictedDisplayPeriod > 0)
    {
        predictedDisplayPeriodNs_ = frameState.predictedDisplayPeriod;
        clientRefreshRateHz_ = static_cast<uint32_t>(
            std::max(1.0, std::round(1.0e9 / static_cast<double>(predictedDisplayPeriodNs_))));
    }

    // xrBeginFrame
    XrFrameBeginInfo beginInfo = {XR_TYPE_FRAME_BEGIN_INFO};
    XrResult beginResult = xrBeginFrame(session_, &beginInfo);
    if (XR_FAILED(beginResult))
    {
        LOGE("xrBeginFrame failed: %d", beginResult);
        return;
    }

    // Build composition layers
    XrCompositionLayerProjection projectionLayer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView projectionViews[2] = {
        {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
        {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}
    };
    XrCompositionLayerBaseHeader* layerPtr = nullptr;
    uint32_t layerCount = 0;

    if (frameState.shouldRender == XR_TRUE)
    {
        // Sync actions (controller input)
        if (actionSet_ != XR_NULL_HANDLE)
        {
            XrActiveActionSet activeSet = {};
            activeSet.actionSet = actionSet_;
            XrActionsSyncInfo syncInfo = {XR_TYPE_ACTIONS_SYNC_INFO};
            syncInfo.countActiveActionSets = 1;
            syncInfo.activeActionSets = &activeSet;
            XrResult syncResult = xrSyncActions(session_, &syncInfo);

            static uint32_t syncLogCount = 0;
            if (++syncLogCount % 270 == 1)
            {
                XrInteractionProfileState leftProfile = {XR_TYPE_INTERACTION_PROFILE_STATE};
                XrInteractionProfileState rightProfile = {XR_TYPE_INTERACTION_PROFILE_STATE};
                XrResult leftProfileResult =
                    xrGetCurrentInteractionProfile(session_, handPaths_[0], &leftProfile);
                XrResult rightProfileResult =
                    xrGetCurrentInteractionProfile(session_, handPaths_[1], &rightProfile);
                LOGI("xrSyncActions result=%d profiles L=%s(%d) R=%s(%d)",
                     syncResult,
                     PathToString(instance_, leftProfile.interactionProfile).c_str(),
                     leftProfileResult,
                     PathToString(instance_, rightProfile.interactionProfile).c_str(),
                     rightProfileResult);
            }
        }

        // Locate views
        XrViewLocateInfo viewLocateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
        viewLocateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        viewLocateInfo.displayTime = frameState.predictedDisplayTime;
        viewLocateInfo.space = appSpace_;

        XrViewState viewState = {XR_TYPE_VIEW_STATE};
        uint32_t viewCount = 2;
        xrLocateViews(session_, &viewLocateInfo, &viewState, 2, &viewCount, views_);

        // Render to swapchains
        RenderFrame(frameState.predictedDisplayTime);

        // Set up projection views.
        // The server now renders using the real headset IPD/FOV we send in
        // TrackingPacket, so we must declare the headset's actual per-eye FOV
        // here as well to keep projection geometry aligned.
        static bool loggedFov = false;
        if (!loggedFov)
        {
            LOGI("Declared headset FOV: L=(%.2f, %.2f, %.2f, %.2f) "
                 "R=(%.2f, %.2f, %.2f, %.2f) swapchain=%ux%u",
                 views_[0].fov.angleLeft, views_[0].fov.angleRight,
                 views_[0].fov.angleUp, views_[0].fov.angleDown,
                 views_[1].fov.angleLeft, views_[1].fov.angleRight,
                 views_[1].fov.angleUp, views_[1].fov.angleDown,
                 swapchainWidth_, swapchainHeight_);
            loggedFov = true;
        }
        bool useRenderPose = hasVideoTexture_ && hasCurrentRenderPose_;

        for (int eye = 0; eye < 2; eye++)
        {
            projectionViews[eye].pose = useRenderPose
                ? BuildEyePoseFromRenderPose(currentRenderPose_, eye)
                : views_[eye].pose;
            projectionViews[eye].fov = views_[eye].fov;
            projectionViews[eye].subImage.swapchain = swapchains_[eye];
            if (blitWidth_ > 0)
            {
                // Tell compositor exactly where the content is within the swapchain
                projectionViews[eye].subImage.imageRect.offset = {blitOffsetX_, blitOffsetY_};
                projectionViews[eye].subImage.imageRect.extent = {
                    (int32_t)blitWidth_, (int32_t)blitHeight_};
            }
            else
            {
                // Fallback: full swapchain (before server discovery)
                projectionViews[eye].subImage.imageRect.offset = {0, 0};
                projectionViews[eye].subImage.imageRect.extent = {
                    (int32_t)swapchainWidth_, (int32_t)swapchainHeight_};
            }
            projectionViews[eye].subImage.imageArrayIndex = 0;
        }

        projectionLayer.space = appSpace_;
        projectionLayer.viewCount = 2;
        projectionLayer.views = projectionViews;
        layerPtr = (XrCompositionLayerBaseHeader*)&projectionLayer;
        layerCount = 1;

        // Send tracking data
        SendTracking(frameState.predictedDisplayTime);
    }

    // xrEndFrame
    XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = frameState.predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = layerCount;
    endInfo.layers = (layerCount > 0) ? &layerPtr : nullptr;

    XrResult endResult = xrEndFrame(session_, &endInfo);
    if (XR_FAILED(endResult))
    {
        LOGE("xrEndFrame failed: %d", endResult);
    }
    else if (lastFrameAcquireTimeNs_ > 0 && lastFrameAcquireTimeNs_ != lastReportedAcquireTimeNs_)
    {
        int64_t nowNs = SteadyClockNowNs();
        if (nowNs >= lastFrameAcquireTimeNs_)
        {
            latencySamples_.decodeToCompositorMs.push_back(
                (double)(nowNs - lastFrameAcquireTimeNs_) / 1.0e6);
        }
        if (lastFrameReceiveTimeNs_ > 0 && nowNs >= lastFrameReceiveTimeNs_)
        {
            double totalClientMs = (double)(nowNs - lastFrameReceiveTimeNs_) / 1.0e6;
            latencySamples_.totalClientMs.push_back(totalClientMs);
            latencySamples_.frameAgeMs.push_back(totalClientMs);
        }
        lastReportedAcquireTimeNs_ = lastFrameAcquireTimeNs_;
    }

    if (IsConnected() && networkReceiver_)
    {
        uint32_t droppedFrames = networkReceiver_->GetFramesDropped();
        auto now = std::chrono::steady_clock::now();
        if (droppedFrames > lastObservedDroppedFrames_ &&
            now - lastKeyframeRequestTime_ >= std::chrono::milliseconds(100))
        {
            RequestKeyframe(protocol::KEYFRAME_REASON_FRAME_LOSS,
                            droppedFrames - lastObservedDroppedFrames_);
        }
        lastObservedDroppedFrames_ = droppedFrames;

        auto sinceVideo = now - lastVideoFrameTime_;
        if (IsConnected() && hasVideoTexture_ &&
            sinceVideo >= std::chrono::milliseconds(200) &&
            now - lastKeyframeRequestTime_ >= std::chrono::milliseconds(100))
        {
            RequestKeyframe(protocol::KEYFRAME_REASON_DECODE_STALL,
                            static_cast<uint32_t>(
                                std::chrono::duration_cast<std::chrono::milliseconds>(sinceVideo).count()));
        }
    }

    SendLatencyReport();
}

void XrApp::RenderFrame(XrTime predictedDisplayTime)
{
    // Try to get a decoded frame from the video decoder
    bool hasVideo = false;
    if (videoDecoder_ && videoDecoder_->IsInitialized())
    {
        VideoDecoder::DecodedFrame frame;

        if (videoDecoder_->AcquireFrame(&frame))
        {
            decodedFrameCount_++;
            if (decodedFrameCount_ <= 5 || decodedFrameCount_ % 300 == 0)
            {
                LOGI("Decoded frame #%u: pts=%lld hwBuffer=%p",
                     decodedFrameCount_, (long long)frame.presentationTimeUs,
                     (void*)frame.hardwareBuffer);
            }

            lastFrameReceiveTimeNs_ = frame.localReceiveTimeNs;
            lastFrameSubmitTimeNs_ = frame.localSubmitTimeNs;
            lastFrameAcquireTimeNs_ = frame.localAcquireTimeNs;
            skippedDecodedFrames_ += frame.skippedFramesBeforeAcquire;

            NetworkReceiver::RenderPose matchedRenderPose = {};
            if (networkReceiver_ != nullptr &&
                networkReceiver_->TakeRenderPoseForPresentationTimeUs(
                    frame.presentationTimeUs, &matchedRenderPose))
            {
                currentRenderPose_ = matchedRenderPose;
                hasCurrentRenderPose_ = true;
                renderPoseHitCount_++;
            }
            else
            {
                currentRenderPose_ = {};
                hasCurrentRenderPose_ = false;
                renderPoseMissCount_++;
            }

            auto renderPoseLogNow = std::chrono::steady_clock::now();
            if (lastRenderPoseLogTime_.time_since_epoch().count() == 0)
            {
                lastRenderPoseLogTime_ = renderPoseLogNow;
            }
            else if (renderPoseLogNow - lastRenderPoseLogTime_ >= std::chrono::seconds(1))
            {
                uint32_t total = renderPoseHitCount_ + renderPoseMissCount_;
                if (total > 0)
                {
                    float hitRate = 100.0f * static_cast<float>(renderPoseHitCount_) /
                                    static_cast<float>(total);
                    LOGI("Render pose match: hit=%u miss=%u rate=%.1f%%",
                         renderPoseHitCount_, renderPoseMissCount_, hitRate);
                }
                renderPoseHitCount_ = 0;
                renderPoseMissCount_ = 0;
                lastRenderPoseLogTime_ = renderPoseLogNow;
            }

            if (frame.localReceiveTimeNs > 0 && frame.localSubmitTimeNs >= frame.localReceiveTimeNs)
            {
                latencySamples_.receiveToSubmitMs.push_back(
                    (double)(frame.localSubmitTimeNs - frame.localReceiveTimeNs) / 1.0e6);
            }
            if (frame.localSubmitTimeNs > 0 && frame.localAcquireTimeNs >= frame.localSubmitTimeNs)
            {
                latencySamples_.submitToDecodeMs.push_back(
                    (double)(frame.localAcquireTimeNs - frame.localSubmitTimeNs) / 1.0e6);
            }

            // Import AHardwareBuffer as EGLImage and bind to GL_TEXTURE_EXTERNAL_OES.
            // The GPU handles YUV→RGB conversion natively — zero CPU copy.
            if (frame.hardwareBuffer != nullptr)
            {
                EGLClientBuffer clientBuf =
                    eglGetNativeClientBufferANDROID_(frame.hardwareBuffer);

                if (clientBuf != nullptr)
                {
                    EGLint imageAttribs[] = {
                        EGL_IMAGE_PRESERVED_KHR, EGL_TRUE,
                        EGL_NONE
                    };

                    EGLImageKHR eglImage = eglCreateImageKHR_(
                        eglDisplay_, EGL_NO_CONTEXT,
                        EGL_NATIVE_BUFFER_ANDROID,
                        clientBuf, imageAttribs);

                    if (eglImage != EGL_NO_IMAGE_KHR)
                    {
                        glBindTexture(GL_TEXTURE_EXTERNAL_OES, videoTexture_);
                        glEGLImageTargetTexture2DOES_(GL_TEXTURE_EXTERNAL_OES, eglImage);
                        glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);

                        uint32_t sampleWidth = frame.bufferWidth;
                        uint32_t sampleHeight = frame.bufferHeight;
                        if (sampleWidth == 0)
                        {
                            sampleWidth = 1;
                        }
                        if (sampleHeight == 0)
                        {
                            sampleHeight = 1;
                        }

                        uint32_t visibleWidth = frame.bufferWidth > 0 ? frame.bufferWidth : sampleWidth;
                        uint32_t visibleHeight = frame.bufferHeight > 0 ? frame.bufferHeight : sampleHeight;

                        int32_t cropLeft = 0;
                        int32_t cropRight = static_cast<int32_t>(visibleWidth);
                        int32_t cropTop = 0;
                        int32_t cropBottom = static_cast<int32_t>(visibleHeight);

                        int32_t requestedCropWidth = frame.cropRight - frame.cropLeft;
                        int32_t requestedCropHeight = frame.cropBottom - frame.cropTop;
                        bool useCropWidth = IsNearlyFullExtent(visibleWidth, requestedCropWidth);
                        bool useCropHeight = IsNearlyFullExtent(visibleHeight, requestedCropHeight);

                        if (useCropWidth)
                        {
                            cropLeft = std::clamp(frame.cropLeft, 0, (int32_t)visibleWidth - 1);
                            cropRight = std::clamp(frame.cropRight, cropLeft + 1,
                                                  (int32_t)visibleWidth);
                        }

                        if (useCropHeight)
                        {
                            cropTop = std::clamp(frame.cropTop, 0, (int32_t)visibleHeight - 1);
                            cropBottom = std::clamp(frame.cropBottom, cropTop + 1,
                                                   (int32_t)visibleHeight);
                        }

                        videoContentUMin_ = ClampNormalized(
                            static_cast<float>(cropLeft) / static_cast<float>(sampleWidth));
                        videoContentUMax_ = ClampNormalized(
                            static_cast<float>(cropRight) / static_cast<float>(sampleWidth));
                        videoContentVMin_ = ClampNormalized(
                            static_cast<float>(cropTop) / static_cast<float>(sampleHeight));
                        videoContentVMax_ = ClampNormalized(
                            static_cast<float>(cropBottom) / static_cast<float>(sampleHeight));

                        hasVideo = true;
                        hasVideoTexture_ = true;
                        lastVideoFrameTime_ = std::chrono::steady_clock::now();

                        if (decodedFrameCount_ <= 5 || decodedFrameCount_ % 300 == 0)
                        {
                            LOGI("Video content UVs: u=[%.5f, %.5f] v=[%.5f, %.5f] "
                                 "(buffer=%ux%u stride=%u crop=[%d,%d - %d,%d] useCrop=%d/%d)",
                                 videoContentUMin_, videoContentUMax_,
                                 videoContentVMin_, videoContentVMax_,
                                 frame.bufferWidth, frame.bufferHeight, frame.bufferStride,
                                 frame.cropLeft, frame.cropTop, frame.cropRight, frame.cropBottom,
                                 useCropWidth ? 1 : 0, useCropHeight ? 1 : 0);
                        }

                        // EGLImage can be destroyed after binding — texture retains the reference
                        eglDestroyImageKHR_(eglDisplay_, eglImage);
                    }
                    else if (decodedFrameCount_ <= 5)
                    {
                        LOGE("eglCreateImageKHR failed: 0x%x", eglGetError());
                    }
                }
                else if (decodedFrameCount_ <= 5)
                {
                    LOGE("eglGetNativeClientBufferANDROID failed");
                }
            }

            // DON'T call ReleaseFrame() here — keep the AImage/AHardwareBuffer alive
            // until next AcquireFrame(), so the texture data remains valid during rendering.
            // AcquireFrame() automatically releases the previous image.
        }
        else if (hasVideoTexture_)
        {
            // Check if the stream has been lost (no new frames for 2 seconds)
            auto elapsed = std::chrono::steady_clock::now() - lastVideoFrameTime_;
            if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() >= 2)
            {
                // Stream lost (Godot closed, Mac disconnected, etc.)
                hasVideoTexture_ = false;
                hasCurrentRenderPose_ = false;
                OnConnectionLost("no video frames for 2 seconds");
            }
            else
            {
                // Reuse the last frame — the AHardwareBuffer is still alive (not released).
                hasVideo = true;
            }
        }
    }

    for (int eye = 0; eye < 2; eye++)
    {
        // Acquire swapchain image
        XrSwapchainImageAcquireInfo acquireInfo = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        uint32_t imageIndex = 0;
        xrAcquireSwapchainImage(swapchains_[eye], &acquireInfo, &imageIndex);

        // Wait for image
        XrSwapchainImageWaitInfo swapWaitInfo = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        swapWaitInfo.timeout = XR_INFINITE_DURATION;
        xrWaitSwapchainImage(swapchains_[eye], &swapWaitInfo);

        // Render into this image
        GLuint texture = swapchainImages_[eye][imageIndex];

        glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);

        if (hasVideo && blitWidth_ > 0)
        {
            // Clear full swapchain to black (letterbox/pillarbox bars)
            glViewport(0, 0, swapchainWidth_, swapchainHeight_);
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);

            // Blit video into aspect-ratio-correct viewport
            glViewport(blitOffsetX_, blitOffsetY_, blitWidth_, blitHeight_);
            BlitVideoToSwapchain(eye);
        }
        else if (hasVideo)
        {
            // Fallback before server discovery (blitWidth_ == 0): full swapchain
            glViewport(0, 0, swapchainWidth_, swapchainHeight_);
            BlitVideoToSwapchain(eye);
        }
        else
        {
            // No video — show status color
            glViewport(0, 0, swapchainWidth_, swapchainHeight_);
            if (HasServerConnection())
            {
                // Server found, waiting for video stream — dark green
                glClearColor(0.0f, 0.1f, 0.0f, 1.0f);
            }
            else
            {
                // Waiting for server discovery — dark blue
                glClearColor(0.0f, 0.0f, 0.15f, 1.0f);
            }
            glClear(GL_COLOR_BUFFER_BIT);
        }

        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        // Release swapchain image
        XrSwapchainImageReleaseInfo releaseInfo = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(swapchains_[eye], &releaseInfo);
    }
}

void XrApp::BlitVideoToSwapchain(int eye)
{
    // Sample only the visible/cropped region of the decoded frame.
    // Some decoder outputs use a wider stride than the visible picture when
    // resolution scaling is enabled, which would shift each eye if we blindly
    // sampled 0..0.5 / 0.5..1.0 across the full texture.
    float contentUMin = videoContentUMin_;
    float contentUMax = videoContentUMax_;
    float contentVMin = videoContentVMin_;
    float contentVMax = videoContentVMax_;
    float contentMidU = contentUMin + (contentUMax - contentUMin) * 0.5f;

    float uMin = (eye == 0) ? contentUMin : contentMidU;
    float uMax = (eye == 0) ? contentMidU : contentUMax;

    float quadVertices[] = {
        -1.0f, -1.0f,  uMin, contentVMax,
         1.0f, -1.0f,  uMax, contentVMax,
         1.0f,  1.0f,  uMax, contentVMin,
        -1.0f, -1.0f,  uMin, contentVMax,
         1.0f,  1.0f,  uMax, contentVMin,
        -1.0f,  1.0f,  uMin, contentVMin,
    };

    glBindBuffer(GL_ARRAY_BUFFER, blitVbo_);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(quadVertices), quadVertices);

    glUseProgram(blitProgram_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, videoTexture_);
    glUniform1i(glGetUniformLocation(blitProgram_, "uTexture"), 0);

    glBindVertexArray(blitVao_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    glUseProgram(0);
}

void XrApp::ClearSwapchain(int eye, float r, float g, float b)
{
    glClearColor(r, g, b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

XrPosef XrApp::BuildEyePoseFromRenderPose(const NetworkReceiver::RenderPose& renderPose,
                                          int eye) const
{
    XrQuaternionf orientation = NormalizeQuaternion({
        renderPose.orientation[0],
        renderPose.orientation[1],
        renderPose.orientation[2],
        renderPose.orientation[3],
    });

    float dx = views_[1].pose.position.x - views_[0].pose.position.x;
    float dy = views_[1].pose.position.y - views_[0].pose.position.y;
    float dz = views_[1].pose.position.z - views_[0].pose.position.z;
    float ipd = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (!std::isfinite(ipd) || ipd < 0.03f || ipd > 0.09f)
    {
        ipd = 0.063f;
    }

    float side = (eye == 0) ? -0.5f : 0.5f;
    XrVector3f localEyeOffset = {side * ipd, 0.0f, 0.0f};
    XrVector3f eyeOffset = RotateVector(orientation, localEyeOffset);

    XrPosef pose = {};
    pose.orientation = orientation;
    pose.position = {
        renderPose.position[0] + eyeOffset.x,
        renderPose.position[1] + eyeOffset.y,
        renderPose.position[2] + eyeOffset.z,
    };
    return pose;
}

void XrApp::SendTracking(XrTime predictedDisplayTime)
{
    if (!trackingSender_ || !trackingSender_->IsConnected())
    {
        return;
    }

    protocol::TrackingPacket packet = {};
    packet.timestampNs = predictedDisplayTime;

    // Head pose — compute center head position from the two eye views
    // (average of left and right eye positions gives the head center)
    float centerX = (views_[0].pose.position.x + views_[1].pose.position.x) * 0.5f;
    float centerY = (views_[0].pose.position.y + views_[1].pose.position.y) * 0.5f;
    float centerZ = (views_[0].pose.position.z + views_[1].pose.position.z) * 0.5f;
    packet.headPosition[0] = centerX;
    packet.headPosition[1] = centerY;
    packet.headPosition[2] = centerZ;
    packet.headOrientation[0] = views_[0].pose.orientation.x;
    packet.headOrientation[1] = views_[0].pose.orientation.y;
    packet.headOrientation[2] = views_[0].pose.orientation.z;
    packet.headOrientation[3] = views_[0].pose.orientation.w;

    // Read head velocity via xrLocateSpace (VIEW space relative to app space)
    if (viewSpace_ != XR_NULL_HANDLE)
    {
        XrSpaceVelocity velocity = {XR_TYPE_SPACE_VELOCITY};
        XrSpaceLocation location = {XR_TYPE_SPACE_LOCATION};
        location.next = &velocity;
        if (XR_SUCCEEDED(xrLocateSpace(viewSpace_, appSpace_, predictedDisplayTime, &location)))
        {
            if (velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT)
            {
                packet.headLinearVelocity[0] = velocity.linearVelocity.x;
                packet.headLinearVelocity[1] = velocity.linearVelocity.y;
                packet.headLinearVelocity[2] = velocity.linearVelocity.z;
            }
            if (velocity.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT)
            {
                packet.headAngularVelocity[0] = velocity.angularVelocity.x;
                packet.headAngularVelocity[1] = velocity.angularVelocity.y;
                packet.headAngularVelocity[2] = velocity.angularVelocity.z;
            }
        }
    }

    // Send Quest's actual IPD so the Mac renders with the correct eye separation.
    // IPD = distance between the two eye positions.
    float dx = views_[1].pose.position.x - views_[0].pose.position.x;
    float dy = views_[1].pose.position.y - views_[0].pose.position.y;
    float dz = views_[1].pose.position.z - views_[0].pose.position.z;
    packet.ipd = sqrtf(dx * dx + dy * dy + dz * dz);

    // Send left eye FOV (the Mac will mirror it for the right eye)
    packet.eyeFov[0] = views_[0].fov.angleLeft;
    packet.eyeFov[1] = views_[0].fov.angleRight;
    packet.eyeFov[2] = views_[0].fov.angleUp;
    packet.eyeFov[3] = views_[0].fov.angleDown;

    // Hand tracking joints
    if (xrLocateHandJointsEXT_ != nullptr)
    {
        static bool lastHandActive[2] = {false, false};
        for (int hand = 0; hand < 2; ++hand)
        {
            if (handTrackers_[hand] == XR_NULL_HANDLE)
            {
                if (lastHandActive[hand])
                {
                    LOGI("Hand tracking %s inactive (tracker missing)", hand == 0 ? "left" : "right");
                    lastHandActive[hand] = false;
                }
                continue;
            }

            XrHandJointLocationEXT jointLocations[XR_HAND_JOINT_COUNT_EXT] = {};
            XrHandJointLocationsEXT locations = {XR_TYPE_HAND_JOINT_LOCATIONS_EXT};
            locations.jointCount = XR_HAND_JOINT_COUNT_EXT;
            locations.jointLocations = jointLocations;

            XrHandJointsLocateInfoEXT locateInfo = {XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT};
            locateInfo.baseSpace = appSpace_;
            locateInfo.time = predictedDisplayTime;

            XrResult locateResult =
                xrLocateHandJointsEXT_(handTrackers_[hand], &locateInfo, &locations);
            const bool handActive = XR_SUCCEEDED(locateResult) &&
                                    locations.isActive == XR_TRUE &&
                                    HasValidCriticalHandJoints(jointLocations);
            if (handActive != lastHandActive[hand])
            {
                LOGI("Hand tracking %s %s locateResult=%d isActive=%d",
                     hand == 0 ? "left" : "right",
                     handActive ? "active" : "inactive",
                     locateResult, locations.isActive == XR_TRUE ? 1 : 0);
                lastHandActive[hand] = handActive;
            }
            if (!handActive)
            {
                continue;
            }

            packet.trackingFlags |= (hand == 0)
                ? protocol::TRACKING_FLAG_LEFT_HAND_ACTIVE
                : protocol::TRACKING_FLAG_RIGHT_HAND_ACTIVE;

            auto& jointPayload = (hand == 0) ? packet.leftHandJoints : packet.rightHandJoints;
            for (uint32_t i = 0; i < XR_HAND_JOINT_COUNT_EXT; ++i)
            {
                jointPayload[i][0] = jointLocations[i].pose.position.x;
                jointPayload[i][1] = jointLocations[i].pose.position.y;
                jointPayload[i][2] = jointLocations[i].pose.position.z;
                jointPayload[i][3] = jointLocations[i].radius;
            }
        }
    }

    // Controller poses
    if (gripSpaces_[0] != XR_NULL_HANDLE && gripSpaces_[1] != XR_NULL_HANDLE)
    {
        static bool lastControllerActive[2] = {false, false};
        for (int hand = 0; hand < 2; hand++)
        {
            XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
            XrResult locResult = xrLocateSpace(gripSpaces_[hand], appSpace_,
                                                predictedDisplayTime, &loc);

            const bool controllerActive =
                XR_SUCCEEDED(locResult) &&
                (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
                (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
            if (controllerActive)
            {
                packet.trackingFlags |= (hand == 0)
                    ? protocol::TRACKING_FLAG_LEFT_CONTROLLER_ACTIVE
                    : protocol::TRACKING_FLAG_RIGHT_CONTROLLER_ACTIVE;
            }
            if (controllerActive != lastControllerActive[hand])
            {
                LOGI("Controller %s %s locateResult=%d flags=0x%lx",
                     hand == 0 ? "left" : "right",
                     controllerActive ? "active" : "inactive",
                     locResult, (unsigned long)loc.locationFlags);
                lastControllerActive[hand] = controllerActive;
            }

            // Debug: log controller locate results periodically
            static uint32_t ctrlLogCounter = 0;
            if (hand == 0 && ++ctrlLogCounter % 270 == 1) // First time + every ~3s
            {
                LOGI("Controller locate: result=%d flags=0x%lx (L) / checking R next",
                     locResult, (unsigned long)loc.locationFlags);
            }

            if (controllerActive)
            {
                float* pos = (hand == 0) ? packet.leftControllerPos : packet.rightControllerPos;
                float* rot = (hand == 0) ? packet.leftControllerRot : packet.rightControllerRot;
                pos[0] = loc.pose.position.x;
                pos[1] = loc.pose.position.y;
                pos[2] = loc.pose.position.z;
                rot[0] = loc.pose.orientation.x;
                rot[1] = loc.pose.orientation.y;
                rot[2] = loc.pose.orientation.z;
                rot[3] = loc.pose.orientation.w;
            }
        }
    }
    else
    {
        static bool loggedNoGrip = false;
        if (!loggedNoGrip)
        {
            LOGW("gripSpaces not set — SetupActions() may have failed. L=%p R=%p",
                 (void*)gripSpaces_[0], (void*)gripSpaces_[1]);
            loggedNoGrip = true;
        }
    }

    // Trigger/grip values
    if (triggerAction_ != XR_NULL_HANDLE)
    {
        XrActionStateGetInfo getInfo = {XR_TYPE_ACTION_STATE_GET_INFO};
        getInfo.action = triggerAction_;
        XrActionStateFloat floatState = {XR_TYPE_ACTION_STATE_FLOAT};

        getInfo.subactionPath = handPaths_[0];
        if (XR_SUCCEEDED(xrGetActionStateFloat(session_, &getInfo, &floatState)))
        {
            packet.leftTrigger = floatState.currentState;
        }
        getInfo.subactionPath = handPaths_[1];
        if (XR_SUCCEEDED(xrGetActionStateFloat(session_, &getInfo, &floatState)))
        {
            packet.rightTrigger = floatState.currentState;
        }
    }

    if (gripAction_ != XR_NULL_HANDLE)
    {
        XrActionStateGetInfo getInfo = {XR_TYPE_ACTION_STATE_GET_INFO};
        getInfo.action = gripAction_;
        XrActionStateFloat floatState = {XR_TYPE_ACTION_STATE_FLOAT};

        getInfo.subactionPath = handPaths_[0];
        if (XR_SUCCEEDED(xrGetActionStateFloat(session_, &getInfo, &floatState)))
        {
            packet.leftGrip = floatState.currentState;
        }
        getInfo.subactionPath = handPaths_[1];
        if (XR_SUCCEEDED(xrGetActionStateFloat(session_, &getInfo, &floatState)))
        {
            packet.rightGrip = floatState.currentState;
        }
    }

    // Thumbsticks
    if (thumbstickAction_ != XR_NULL_HANDLE)
    {
        XrActionStateGetInfo getInfo = {XR_TYPE_ACTION_STATE_GET_INFO};
        getInfo.action = thumbstickAction_;
        XrActionStateVector2f vec2State = {XR_TYPE_ACTION_STATE_VECTOR2F};

        getInfo.subactionPath = handPaths_[0];
        if (XR_SUCCEEDED(xrGetActionStateVector2f(session_, &getInfo, &vec2State)))
        {
            packet.leftThumbstick[0] = vec2State.currentState.x;
            packet.leftThumbstick[1] = vec2State.currentState.y;
        }
        getInfo.subactionPath = handPaths_[1];
        if (XR_SUCCEEDED(xrGetActionStateVector2f(session_, &getInfo, &vec2State)))
        {
            packet.rightThumbstick[0] = vec2State.currentState.x;
            packet.rightThumbstick[1] = vec2State.currentState.y;
        }
    }

    // Buttons
    uint32_t buttons = 0;
    if (aButtonAction_ != XR_NULL_HANDLE)
    {
        XrActionStateGetInfo getInfo = {XR_TYPE_ACTION_STATE_GET_INFO};
        getInfo.action = aButtonAction_;
        XrActionStateBoolean boolState = {XR_TYPE_ACTION_STATE_BOOLEAN};

        getInfo.subactionPath = handPaths_[0];
        if (XR_SUCCEEDED(xrGetActionStateBoolean(session_, &getInfo, &boolState)) && boolState.currentState)
        {
            buttons |= protocol::BUTTON_X;
        }
        getInfo.subactionPath = handPaths_[1];
        if (XR_SUCCEEDED(xrGetActionStateBoolean(session_, &getInfo, &boolState)) && boolState.currentState)
        {
            buttons |= protocol::BUTTON_A;
        }
    }
    if (bButtonAction_ != XR_NULL_HANDLE)
    {
        XrActionStateGetInfo getInfo = {XR_TYPE_ACTION_STATE_GET_INFO};
        getInfo.action = bButtonAction_;
        XrActionStateBoolean boolState = {XR_TYPE_ACTION_STATE_BOOLEAN};

        getInfo.subactionPath = handPaths_[0];
        if (XR_SUCCEEDED(xrGetActionStateBoolean(session_, &getInfo, &boolState)) && boolState.currentState)
        {
            buttons |= protocol::BUTTON_Y;
        }
        getInfo.subactionPath = handPaths_[1];
        if (XR_SUCCEEDED(xrGetActionStateBoolean(session_, &getInfo, &boolState)) && boolState.currentState)
        {
            buttons |= protocol::BUTTON_B;
        }
    }
    if (menuAction_ != XR_NULL_HANDLE)
    {
        XrActionStateGetInfo getInfo = {XR_TYPE_ACTION_STATE_GET_INFO};
        getInfo.action = menuAction_;
        getInfo.subactionPath = handPaths_[0];
        XrActionStateBoolean boolState = {XR_TYPE_ACTION_STATE_BOOLEAN};

        if (XR_SUCCEEDED(xrGetActionStateBoolean(session_, &getInfo, &boolState)) && boolState.currentState)
        {
            buttons |= protocol::BUTTON_MENU;
        }
    }
    packet.buttonState = buttons;

    // Debug: log position periodically to verify 6DOF tracking
    static uint32_t trackingLogCounter = 0;
    if (++trackingLogCounter % 270 == 0) // ~3 seconds at 90fps
    {
        LOGI("Tracking: pos=(%.3f, %.3f, %.3f) rot=(%.3f, %.3f, %.3f, %.3f) "
             "L=(%.3f,%.3f,%.3f) R=(%.3f,%.3f,%.3f) trig=%.2f/%.2f btn=0x%x hands=0x%x",
             packet.headPosition[0], packet.headPosition[1], packet.headPosition[2],
             packet.headOrientation[0], packet.headOrientation[1],
             packet.headOrientation[2], packet.headOrientation[3],
             packet.leftControllerPos[0], packet.leftControllerPos[1], packet.leftControllerPos[2],
             packet.rightControllerPos[0], packet.rightControllerPos[1], packet.rightControllerPos[2],
             packet.leftTrigger, packet.rightTrigger, packet.buttonState, packet.trackingFlags);
    }

    trackingSender_->Send(packet);
}

// ─── Session state ───────────────────────────────────────────────────────────

void XrApp::HandleSessionStateChange(XrSessionState newState)
{
    LOGI("Session state: %d -> %d", (int)sessionState_, (int)newState);
    sessionState_ = newState;

    switch (newState)
    {
    case XR_SESSION_STATE_READY:
    {
        XrSessionBeginInfo beginInfo = {XR_TYPE_SESSION_BEGIN_INFO};
        beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        XrResult result = xrBeginSession(session_, &beginInfo);
        if (XR_SUCCEEDED(result))
        {
            sessionRunning_ = true;
            LOGI("Session READY -> started");

            // Attach action set to session
            if (actionSet_ != XR_NULL_HANDLE)
            {
                XrSessionActionSetsAttachInfo attachInfo = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
                attachInfo.countActionSets = 1;
                attachInfo.actionSets = &actionSet_;
                XrResult attachResult = xrAttachSessionActionSets(session_, &attachInfo);
                if (XR_FAILED(attachResult))
                {
                    LOGW("xrAttachSessionActionSets failed: %d", attachResult);
                }
                else
                {
                    LOGI("Action set attached to session");
                }
            }

            if (!CreateSwapchains())
            {
                LOGE("Failed to create swapchains");
                running_ = false;
            }
        }
        else
        {
            LOGE("xrBeginSession failed: %d", result);
        }
        break;
    }
    case XR_SESSION_STATE_STOPPING:
        StopNetworking();
        ShutdownHandTracking();
        ShutdownFoveation();
        xrEndSession(session_);
        sessionRunning_ = false;
        LOGI("Session STOPPING -> ended");
        break;
    case XR_SESSION_STATE_EXITING:
    case XR_SESSION_STATE_LOSS_PENDING:
        running_ = false;
        break;
    default:
        break;
    }
}

// ─── Shutdown ────────────────────────────────────────────────────────────────

void XrApp::Shutdown()
{
    StopNetworking();
    ShutdownHandTracking();
    ShutdownFoveation();

    // Destroy GL resources
    if (blitProgram_ != 0)
    {
        glDeleteProgram(blitProgram_);
        blitProgram_ = 0;
    }
    if (blitVao_ != 0)
    {
        glDeleteVertexArrays(1, &blitVao_);
        blitVao_ = 0;
    }
    if (blitVbo_ != 0)
    {
        glDeleteBuffers(1, &blitVbo_);
        blitVbo_ = 0;
    }
    if (fbo_ != 0)
    {
        glDeleteFramebuffers(1, &fbo_);
        fbo_ = 0;
    }
    if (videoTexture_ != 0)
    {
        glDeleteTextures(1, &videoTexture_);
        videoTexture_ = 0;
    }

    // Destroy swapchains
    for (int eye = 0; eye < 2; eye++)
    {
        if (swapchains_[eye] != XR_NULL_HANDLE)
        {
            xrDestroySwapchain(swapchains_[eye]);
            swapchains_[eye] = XR_NULL_HANDLE;
        }
    }

    // Destroy OpenXR objects
    if (viewSpace_ != XR_NULL_HANDLE)
    {
        xrDestroySpace(viewSpace_);
        viewSpace_ = XR_NULL_HANDLE;
    }
    if (appSpace_ != XR_NULL_HANDLE)
    {
        xrDestroySpace(appSpace_);
        appSpace_ = XR_NULL_HANDLE;
    }
    if (session_ != XR_NULL_HANDLE)
    {
        xrDestroySession(session_);
        session_ = XR_NULL_HANDLE;
    }
    if (instance_ != XR_NULL_HANDLE)
    {
        xrDestroyInstance(instance_);
        instance_ = XR_NULL_HANDLE;
    }

    // Destroy EGL
    if (eglDisplay_ != EGL_NO_DISPLAY)
    {
        eglMakeCurrent(eglDisplay_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (eglContext_ != EGL_NO_CONTEXT)
        {
            eglDestroyContext(eglDisplay_, eglContext_);
            eglContext_ = EGL_NO_CONTEXT;
        }
        eglTerminate(eglDisplay_);
        eglDisplay_ = EGL_NO_DISPLAY;
    }

    running_ = false;
    LOGI("XrApp shut down");
}

} // namespace oxr
