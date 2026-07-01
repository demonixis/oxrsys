// SPDX-License-Identifier: MPL-2.0

#include "XrApp.h"

#include <android/hardware_buffer.h>
#include <android/log.h>
#include <android_native_app_glue.h>
#include <arpa/inet.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <numeric>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <openxr/openxr_platform.h>
#include <oxrsys/protocol/Foveation.h>

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

static const char* SHELL_VERTEX_SHADER = R"(#version 300 es
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aColor;
uniform mat4 uMvp;
out vec4 vColor;
void main() {
    vColor = aColor;
    gl_Position = uMvp * vec4(aPos, 1.0);
}
)";

static const char* SHELL_FRAGMENT_SHADER = R"(#version 300 es
precision mediump float;
in vec4 vColor;
out vec4 fragColor;
void main() {
    fragColor = vColor;
}
)";

namespace
{

constexpr auto kUsbAdbRetryInterval = std::chrono::seconds(1);
constexpr uint32_t kUsbAdbRetryLogInterval = 10;

const char* VideoCodecName(oxr::protocol::VideoCodec codec)
{
    switch (codec)
    {
        case oxr::protocol::VideoCodec::H264:
            return "H.264";
        case oxr::protocol::VideoCodec::H265:
            return "H.265";
        case oxr::protocol::VideoCodec::AV1:
            return "AV1";
        default:
            return "unknown";
    }
}

} // namespace

// Foveated decompression follows ALVR's AADT inverse mapping (MIT licensed).
// The upscaling branch is an OXRSys edge-aware shader path using ALVR's public defaults.
static const char* BLIT_FRAGMENT_SHADER_OES = R"(#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
precision highp float;
in vec2 vUV;
out vec4 fragColor;
uniform samplerExternalOES uTexture;
uniform vec2 uEyeSourceMin;
uniform vec2 uEyeSourceMax;
uniform vec2 uLogicalTexelSize;
uniform int uFoveatedEncodingEnabled;
uniform int uClientUpscalingEnabled;
uniform float uUpscaleEdgeThreshold;
uniform float uUpscaleSharpness;
uniform vec2 uFoveationCenterSize;
uniform vec2 uFoveationCenterShift;
uniform vec2 uFoveationEdgeRatio;
uniform vec2 uFoveationEyeSizeRatio;
uniform int uReprojectionWarpEnabled;
uniform vec2 uReprojectionWarpOffset;
uniform int uPassthroughAlphaEnabled;

float compressAxis(float eyeUv, float centerSize, float centerShift, float edgeRatio) {
    float c0 = (1.0 - centerSize) * 0.5;
    float c1 = (edgeRatio - 1.0) * c0 * (centerShift + 1.0) / edgeRatio;
    float c2 = (edgeRatio - 1.0) * centerSize + 1.0;
    float loBound = c0 * (centerShift + 1.0) / c2;
    float hiBound = c0 * (centerShift - 1.0) / c2 + 1.0;
    float center = eyeUv * c2 / edgeRatio + c1;
    float d2 = eyeUv * c2;
    float d3 = (eyeUv - 1.0) * c2 + 1.0;
    float g1 = loBound > 0.0 ? eyeUv / loBound : 1.0;
    float g2 = (1.0 - hiBound) > 0.0 ? (1.0 - eyeUv) / (1.0 - hiBound) : 1.0;
    float leftEdge = g1 * center + (1.0 - g1) * d2;
    float rightEdge = g2 * center + (1.0 - g2) * d3;
    if (eyeUv < loBound) {
        return leftEdge;
    }
    if (eyeUv > hiBound) {
        return rightEdge;
    }
    return center;
}

float decompressAxis(float targetUv, float centerSize, float centerShift, float edgeRatio) {
    float lo = 0.0;
    float hi = 1.0;
    for (int i = 0; i < 10; ++i) {
        float mid = (lo + hi) * 0.5;
        float mapped = compressAxis(mid, centerSize, centerShift, edgeRatio);
        if (mapped < targetUv) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return (lo + hi) * 0.5;
}

vec2 mapEyeUvToSource(vec2 eyeUv) {
    vec2 corrected = eyeUv;
    if (uFoveatedEncodingEnabled != 0) {
        corrected.x = decompressAxis(eyeUv.x, uFoveationCenterSize.x,
                                     uFoveationCenterShift.x,
                                     uFoveationEdgeRatio.x) * uFoveationEyeSizeRatio.x;
        corrected.y = decompressAxis(eyeUv.y, uFoveationCenterSize.y,
                                     uFoveationCenterShift.y,
                                     uFoveationEdgeRatio.y) * uFoveationEyeSizeRatio.y;
    }
    corrected = clamp(corrected, vec2(0.0), vec2(1.0));
    return mix(uEyeSourceMin, uEyeSourceMax, corrected);
}

vec3 sampleVideo(vec2 eyeUv) {
    return texture(uTexture, mapEyeUvToSource(clamp(eyeUv, vec2(0.0), vec2(1.0)))).rgb;
}

float luma(vec3 color) {
    return dot(color, vec3(0.299, 0.587, 0.114));
}

void main() {
    vec2 eyeUv = clamp(vUV, vec2(0.0), vec2(1.0));
    if (uReprojectionWarpEnabled != 0) {
        vec2 centered = eyeUv - vec2(0.5);
        float edgeWeight = 1.0 + dot(centered, centered);
        eyeUv = clamp(eyeUv + uReprojectionWarpOffset * edgeWeight,
                      vec2(0.0), vec2(1.0));
    }
    vec3 color = sampleVideo(eyeUv);
    if (uClientUpscalingEnabled != 0) {
        vec2 stepUv = max(uLogicalTexelSize, vec2(0.00001));
        vec3 left = sampleVideo(eyeUv + vec2(-stepUv.x, 0.0));
        vec3 right = sampleVideo(eyeUv + vec2(stepUv.x, 0.0));
        vec3 up = sampleVideo(eyeUv + vec2(0.0, -stepUv.y));
        vec3 down = sampleVideo(eyeUv + vec2(0.0, stepUv.y));
        float edgeVote = abs(luma(left) - luma(right)) + abs(luma(up) - luma(down));
        if (edgeVote > uUpscaleEdgeThreshold) {
            vec3 detail = color * 4.0 - left - right - up - down;
            color = clamp(color + detail * (0.125 * uUpscaleSharpness), vec3(0.0), vec3(1.0));
        }
    }
    float alpha = 1.0;
    if (uPassthroughAlphaEnabled != 0) {
        float maxChannel = max(max(color.r, color.g), color.b);
        alpha = smoothstep(0.02, 0.10, maxChannel);
    }
    fragColor = vec4(color, alpha);
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

bool HasValidJointOrientation(const XrHandJointLocationEXT& joint)
{
    return (joint.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0 &&
           std::isfinite(joint.pose.orientation.x) &&
           std::isfinite(joint.pose.orientation.y) &&
           std::isfinite(joint.pose.orientation.z) &&
           std::isfinite(joint.pose.orientation.w);
}

bool HasValidCriticalHandJoints(const XrHandJointLocationEXT* joints)
{
    return HasValidJointPosition(joints[XR_HAND_JOINT_PALM_EXT]) &&
           HasValidJointPosition(joints[XR_HAND_JOINT_WRIST_EXT]) &&
           HasValidJointPosition(joints[XR_HAND_JOINT_THUMB_TIP_EXT]) &&
           HasValidJointPosition(joints[XR_HAND_JOINT_INDEX_TIP_EXT]);
}

uint32_t CountValidHandJoints(const XrHandJointLocationEXT* joints)
{
    uint32_t validCount = 0;
    for (uint32_t i = 0; i < XR_HAND_JOINT_COUNT_EXT; ++i)
    {
        if (HasValidJointPosition(joints[i]))
        {
            ++validCount;
        }
    }
    return validCount;
}

bool HasUsableHandJoints(const XrHandJointLocationEXT* joints)
{
    return CountValidHandJoints(joints) >= 8 &&
           HasValidJointPosition(joints[XR_HAND_JOINT_PALM_EXT]) &&
           HasValidJointPosition(joints[XR_HAND_JOINT_THUMB_TIP_EXT]) &&
           HasValidJointPosition(joints[XR_HAND_JOINT_INDEX_TIP_EXT]);
}

std::string MissingCriticalHandJoints(const XrHandJointLocationEXT* joints)
{
    struct JointName
    {
        XrHandJointEXT joint;
        const char* name;
    };
    constexpr std::array<JointName, 6> criticalJoints = {{
        {XR_HAND_JOINT_PALM_EXT, "palm"},
        {XR_HAND_JOINT_WRIST_EXT, "wrist"},
        {XR_HAND_JOINT_THUMB_TIP_EXT, "thumb_tip"},
        {XR_HAND_JOINT_INDEX_TIP_EXT, "index_tip"},
        {XR_HAND_JOINT_INDEX_METACARPAL_EXT, "index_metacarpal"},
        {XR_HAND_JOINT_LITTLE_METACARPAL_EXT, "little_metacarpal"},
    }};

    std::string missing;
    for (const JointName& criticalJoint : criticalJoints)
    {
        if (HasValidJointPosition(joints[criticalJoint.joint]))
        {
            continue;
        }
        if (!missing.empty())
        {
            missing += ",";
        }
        missing += criticalJoint.name;
    }
    return missing.empty() ? "none" : missing;
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

static XrFoveationLevelFB ToXrFoveationLevel(protocol::ClientFoveationPreset preset)
{
    switch (preset)
    {
    case protocol::ClientFoveationPreset::Light:
        return XR_FOVEATION_LEVEL_LOW_FB;
    case protocol::ClientFoveationPreset::High:
        return XR_FOVEATION_LEVEL_HIGH_FB;
    case protocol::ClientFoveationPreset::Medium:
    default:
        return XR_FOVEATION_LEVEL_MEDIUM_FB;
    }
}

static float FoveationActiveRatio(float targetEyeSize, float encodedEyeSize,
                                  float centerSize, float edgeRatio)
{
    if (targetEyeSize <= 0.0f || encodedEyeSize <= 0.0f || edgeRatio <= 1.0f)
    {
        return 1.0f;
    }
    float scale = centerSize + (1.0f - centerSize) / edgeRatio;
    return std::clamp((scale * targetEyeSize) / encodedEyeSize, 0.0001f, 1.0f);
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

static XrQuaternionf ConjugateQuaternion(const XrQuaternionf& q)
{
    return {-q.x, -q.y, -q.z, q.w};
}

static XrQuaternionf MultiplyQuaternion(const XrQuaternionf& a, const XrQuaternionf& b)
{
    return NormalizeQuaternion({
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    });
}

static float VectorDistance(const XrVector3f& a, const XrVector3f& b)
{
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

static const char* ReprojectionModeName(protocol::ClientReprojectionMode mode)
{
    switch (mode)
    {
        case protocol::ClientReprojectionMode::Off:
            return "off";
        case protocol::ClientReprojectionMode::PoseWarp:
            return "pose_warp";
        case protocol::ClientReprojectionMode::Pose:
        default:
            return "pose";
    }
}

struct MetricSummary
{
    double average = 0.0;
    double p50 = 0.0;
    double p95 = 0.0;
    size_t count = 0;
};

struct Mat4
{
    float m[16] = {};
};

struct ShellColor
{
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float a = 1.0f;
};

struct ShellVertex
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float a = 1.0f;
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

static quest_shell::Vec3 AddVec3(const quest_shell::Vec3& a, const quest_shell::Vec3& b)
{
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

static quest_shell::Vec3 SubVec3(const quest_shell::Vec3& a, const quest_shell::Vec3& b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

static quest_shell::Vec3 ScaleVec3(const quest_shell::Vec3& v, float scale)
{
    return {v.x * scale, v.y * scale, v.z * scale};
}

static float DotVec3(const quest_shell::Vec3& a, const quest_shell::Vec3& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static quest_shell::Vec3 CrossVec3(const quest_shell::Vec3& a, const quest_shell::Vec3& b)
{
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
    };
}

static quest_shell::Vec3 NormalizeVec3(const quest_shell::Vec3& v)
{
    const float length = std::sqrt(DotVec3(v, v));
    if (!std::isfinite(length) || length < 0.0001f)
    {
        return {0.0f, 0.0f, -1.0f};
    }
    return ScaleVec3(v, 1.0f / length);
}

static float DistanceVec3(const quest_shell::Vec3& a, const quest_shell::Vec3& b)
{
    const quest_shell::Vec3 delta = SubVec3(a, b);
    return std::sqrt(DotVec3(delta, delta));
}

static quest_shell::Vec3 ToShellVec3(const XrVector3f& v)
{
    return {v.x, v.y, v.z};
}

static Mat4 MultiplyMat4(const Mat4& a, const Mat4& b)
{
    Mat4 result = {};
    for (int col = 0; col < 4; ++col)
    {
        for (int row = 0; row < 4; ++row)
        {
            float value = 0.0f;
            for (int k = 0; k < 4; ++k)
            {
                value += a.m[k * 4 + row] * b.m[col * 4 + k];
            }
            result.m[col * 4 + row] = value;
        }
    }
    return result;
}

static Mat4 ProjectionFromFov(const XrFovf& fov)
{
    constexpr float nearZ = 0.05f;
    constexpr float farZ = 50.0f;
    const float tanLeft = std::tan(fov.angleLeft);
    const float tanRight = std::tan(fov.angleRight);
    const float tanDown = std::tan(fov.angleDown);
    const float tanUp = std::tan(fov.angleUp);
    const float tanWidth = tanRight - tanLeft;
    const float tanHeight = tanUp - tanDown;

    Mat4 projection = {};
    projection.m[0] = 2.0f / tanWidth;
    projection.m[5] = 2.0f / tanHeight;
    projection.m[8] = (tanRight + tanLeft) / tanWidth;
    projection.m[9] = (tanUp + tanDown) / tanHeight;
    projection.m[10] = -(farZ + nearZ) / (farZ - nearZ);
    projection.m[11] = -1.0f;
    projection.m[14] = -(2.0f * farZ * nearZ) / (farZ - nearZ);
    return projection;
}

static Mat4 ViewFromPose(const XrPosef& pose)
{
    const XrQuaternionf q = NormalizeQuaternion(ConjugateQuaternion(pose.orientation));
    const float x = q.x;
    const float y = q.y;
    const float z = q.z;
    const float w = q.w;

    Mat4 view = {};
    view.m[0] = 1.0f - 2.0f * (y * y + z * z);
    view.m[1] = 2.0f * (x * y + z * w);
    view.m[2] = 2.0f * (x * z - y * w);
    view.m[4] = 2.0f * (x * y - z * w);
    view.m[5] = 1.0f - 2.0f * (x * x + z * z);
    view.m[6] = 2.0f * (y * z + x * w);
    view.m[8] = 2.0f * (x * z + y * w);
    view.m[9] = 2.0f * (y * z - x * w);
    view.m[10] = 1.0f - 2.0f * (x * x + y * y);
    view.m[15] = 1.0f;

    const XrVector3f& p = pose.position;
    view.m[12] = -(view.m[0] * p.x + view.m[4] * p.y + view.m[8] * p.z);
    view.m[13] = -(view.m[1] * p.x + view.m[5] * p.y + view.m[9] * p.z);
    view.m[14] = -(view.m[2] * p.x + view.m[6] * p.y + view.m[10] * p.z);
    return view;
}

static void AppendVertex(std::vector<ShellVertex>& vertices,
                         const quest_shell::Vec3& p,
                         const ShellColor& color)
{
    vertices.push_back({p.x, p.y, p.z, color.r, color.g, color.b, color.a});
}

static void AppendLine(std::vector<ShellVertex>& lines,
                       const quest_shell::Vec3& a,
                       const quest_shell::Vec3& b,
                       const ShellColor& color)
{
    AppendVertex(lines, a, color);
    AppendVertex(lines, b, color);
}

static void AppendQuad(std::vector<ShellVertex>& triangles,
                       const quest_shell::Vec3& a,
                       const quest_shell::Vec3& b,
                       const quest_shell::Vec3& c,
                       const quest_shell::Vec3& d,
                       const ShellColor& color)
{
    AppendVertex(triangles, a, color);
    AppendVertex(triangles, b, color);
    AppendVertex(triangles, c, color);
    AppendVertex(triangles, a, color);
    AppendVertex(triangles, c, color);
    AppendVertex(triangles, d, color);
}

static void AppendCube(std::vector<ShellVertex>& triangles,
                       const quest_shell::Vec3& center,
                       float size,
                       const ShellColor& color)
{
    const float half = size * 0.5f;
    const quest_shell::Vec3 p000 = {center.x - half, center.y - half, center.z - half};
    const quest_shell::Vec3 p001 = {center.x - half, center.y - half, center.z + half};
    const quest_shell::Vec3 p010 = {center.x - half, center.y + half, center.z - half};
    const quest_shell::Vec3 p011 = {center.x - half, center.y + half, center.z + half};
    const quest_shell::Vec3 p100 = {center.x + half, center.y - half, center.z - half};
    const quest_shell::Vec3 p101 = {center.x + half, center.y - half, center.z + half};
    const quest_shell::Vec3 p110 = {center.x + half, center.y + half, center.z - half};
    const quest_shell::Vec3 p111 = {center.x + half, center.y + half, center.z + half};

    AppendQuad(triangles, p000, p100, p110, p010, color);
    AppendQuad(triangles, p101, p001, p011, p111, color);
    AppendQuad(triangles, p001, p000, p010, p011, color);
    AppendQuad(triangles, p100, p101, p111, p110, color);
    AppendQuad(triangles, p010, p110, p111, p011, color);
    AppendQuad(triangles, p001, p101, p100, p000, color);
}

static quest_shell::Vec3 PanelPoint(const quest_shell::PanelLayout& panel,
                                    float localX,
                                    float localY,
                                    float normalOffset = 0.0f)
{
    return AddVec3(
        AddVec3(panel.center, ScaleVec3(panel.right, localX)),
        AddVec3(ScaleVec3(panel.up, localY), ScaleVec3(panel.normal, normalOffset)));
}

static void AppendPanelQuad(std::vector<ShellVertex>& triangles,
                            const quest_shell::PanelLayout& panel,
                            float centerX,
                            float centerY,
                            float width,
                            float height,
                            float normalOffset,
                            const ShellColor& color)
{
    const float left = centerX - width * 0.5f;
    const float right = centerX + width * 0.5f;
    const float bottom = centerY - height * 0.5f;
    const float top = centerY + height * 0.5f;
    AppendQuad(triangles,
               PanelPoint(panel, left, bottom, normalOffset),
               PanelPoint(panel, right, bottom, normalOffset),
               PanelPoint(panel, right, top, normalOffset),
               PanelPoint(panel, left, top, normalOffset),
               color);
}

static const std::array<uint8_t, 7>& GlyphRows(char ch)
{
    static const std::array<uint8_t, 7> space = {0, 0, 0, 0, 0, 0, 0};
    static const std::array<uint8_t, 7> unknown = {14, 17, 1, 2, 4, 0, 4};
    static const std::array<uint8_t, 7> glyphs[36] = {
        std::array<uint8_t, 7>{14, 17, 17, 31, 17, 17, 17}, // A
        std::array<uint8_t, 7>{30, 17, 17, 30, 17, 17, 30}, // B
        std::array<uint8_t, 7>{14, 17, 16, 16, 16, 17, 14}, // C
        std::array<uint8_t, 7>{30, 17, 17, 17, 17, 17, 30}, // D
        std::array<uint8_t, 7>{31, 16, 16, 30, 16, 16, 31}, // E
        std::array<uint8_t, 7>{31, 16, 16, 30, 16, 16, 16}, // F
        std::array<uint8_t, 7>{14, 17, 16, 23, 17, 17, 15}, // G
        std::array<uint8_t, 7>{17, 17, 17, 31, 17, 17, 17}, // H
        std::array<uint8_t, 7>{14, 4, 4, 4, 4, 4, 14},      // I
        std::array<uint8_t, 7>{7, 2, 2, 2, 18, 18, 12},     // J
        std::array<uint8_t, 7>{17, 18, 20, 24, 20, 18, 17}, // K
        std::array<uint8_t, 7>{16, 16, 16, 16, 16, 16, 31}, // L
        std::array<uint8_t, 7>{17, 27, 21, 21, 17, 17, 17}, // M
        std::array<uint8_t, 7>{17, 25, 21, 19, 17, 17, 17}, // N
        std::array<uint8_t, 7>{14, 17, 17, 17, 17, 17, 14}, // O
        std::array<uint8_t, 7>{30, 17, 17, 30, 16, 16, 16}, // P
        std::array<uint8_t, 7>{14, 17, 17, 17, 21, 18, 13}, // Q
        std::array<uint8_t, 7>{30, 17, 17, 30, 20, 18, 17}, // R
        std::array<uint8_t, 7>{15, 16, 16, 14, 1, 1, 30},   // S
        std::array<uint8_t, 7>{31, 4, 4, 4, 4, 4, 4},       // T
        std::array<uint8_t, 7>{17, 17, 17, 17, 17, 17, 14}, // U
        std::array<uint8_t, 7>{17, 17, 17, 17, 17, 10, 4},  // V
        std::array<uint8_t, 7>{17, 17, 17, 21, 21, 21, 10}, // W
        std::array<uint8_t, 7>{17, 17, 10, 4, 10, 17, 17},  // X
        std::array<uint8_t, 7>{17, 17, 10, 4, 4, 4, 4},     // Y
        std::array<uint8_t, 7>{31, 1, 2, 4, 8, 16, 31},     // Z
        std::array<uint8_t, 7>{14, 17, 19, 21, 25, 17, 14}, // 0
        std::array<uint8_t, 7>{4, 12, 4, 4, 4, 4, 14},      // 1
        std::array<uint8_t, 7>{14, 17, 1, 2, 4, 8, 31},     // 2
        std::array<uint8_t, 7>{30, 1, 1, 14, 1, 1, 30},     // 3
        std::array<uint8_t, 7>{2, 6, 10, 18, 31, 2, 2},     // 4
        std::array<uint8_t, 7>{31, 16, 16, 30, 1, 1, 30},   // 5
        std::array<uint8_t, 7>{14, 16, 16, 30, 17, 17, 14}, // 6
        std::array<uint8_t, 7>{31, 1, 2, 4, 8, 8, 8},       // 7
        std::array<uint8_t, 7>{14, 17, 17, 14, 17, 17, 14}, // 8
        std::array<uint8_t, 7>{14, 17, 17, 15, 1, 1, 14},   // 9
    };
    static const std::array<uint8_t, 7> dash = {0, 0, 0, 31, 0, 0, 0};
    static const std::array<uint8_t, 7> colon = {0, 4, 4, 0, 4, 4, 0};
    static const std::array<uint8_t, 7> dot = {0, 0, 0, 0, 0, 4, 4};
    static const std::array<uint8_t, 7> slash = {1, 1, 2, 4, 8, 16, 16};

    const unsigned char upper = static_cast<unsigned char>(std::toupper(static_cast<unsigned char>(ch)));
    if (upper >= 'A' && upper <= 'Z')
    {
        return glyphs[upper - 'A'];
    }
    if (upper >= '0' && upper <= '9')
    {
        return glyphs[26 + upper - '0'];
    }
    switch (upper)
    {
    case ' ':
        return space;
    case '-':
        return dash;
    case ':':
        return colon;
    case '.':
        return dot;
    case '/':
        return slash;
    default:
        return unknown;
    }
}

static std::string UppercaseAscii(std::string text, size_t maxChars)
{
    if (text.size() > maxChars)
    {
        text.resize(maxChars);
    }
    for (char& ch : text)
    {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    return text;
}

static void AppendPanelText(std::vector<ShellVertex>& triangles,
                            const quest_shell::PanelLayout& panel,
                            float left,
                            float top,
                            float pixelSize,
                            const std::string& text,
                            const ShellColor& color)
{
    float cursorX = left;
    constexpr int glyphWidth = 5;
    constexpr int glyphHeight = 7;
    for (char ch : text)
    {
        const auto& rows = GlyphRows(ch);
        for (int row = 0; row < glyphHeight; ++row)
        {
            const uint8_t bits = rows[row];
            for (int col = 0; col < glyphWidth; ++col)
            {
                if ((bits & (1u << (glyphWidth - 1 - col))) == 0)
                {
                    continue;
                }
                const float cellX = cursorX + (static_cast<float>(col) + 0.5f) * pixelSize;
                const float cellY = top - (static_cast<float>(row) + 0.5f) * pixelSize;
                AppendPanelQuad(triangles, panel, cellX, cellY,
                                pixelSize * 0.88f, pixelSize * 0.88f,
                                0.004f, color);
            }
        }
        cursorX += static_cast<float>(glyphWidth + 1) * pixelSize;
    }
}

static void AppendPanelTextCentered(std::vector<ShellVertex>& triangles,
                                    const quest_shell::PanelLayout& panel,
                                    float centerX,
                                    float centerY,
                                    float pixelSize,
                                    const std::string& text,
                                    const ShellColor& color)
{
    if (text.empty())
    {
        return;
    }

    constexpr int glyphWidth = 5;
    constexpr int glyphHeight = 7;
    constexpr int glyphAdvance = glyphWidth + 1;
    const float textWidth =
        static_cast<float>((text.size() - 1) * glyphAdvance + glyphWidth) * pixelSize;
    const float textHeight = static_cast<float>(glyphHeight) * pixelSize;
    AppendPanelText(triangles, panel,
                    centerX - textWidth * 0.5f,
                    centerY + textHeight * 0.5f,
                    pixelSize, text, color);
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

static GLuint CreateProgram(const char* vertexShaderSource, const char* fragmentShaderSource)
{
    GLuint vs = CompileShader(GL_VERTEX_SHADER, vertexShaderSource);
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, fragmentShaderSource);
    if (vs == 0 || fs == 0)
    {
        if (vs != 0)
        {
            glDeleteShader(vs);
        }
        if (fs != 0)
        {
            glDeleteShader(fs);
        }
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

static GLuint CreateBlitProgram(const char* fragmentShaderSource)
{
    return CreateProgram(BLIT_VERTEX_SHADER, fragmentShaderSource);
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
    passthroughExtensionAvailable_ = hasExtension(XR_FB_PASSTHROUGH_EXTENSION_NAME);
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
    if (passthroughExtensionAvailable_)
    {
        extensions.push_back(XR_FB_PASSTHROUGH_EXTENSION_NAME);
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
    XrSystemPassthroughPropertiesFB passthroughProperties = {
        XR_TYPE_SYSTEM_PASSTHROUGH_PROPERTIES_FB};
    XrSystemProperties systemProperties = {XR_TYPE_SYSTEM_PROPERTIES};
    void* systemPropertiesNext = nullptr;
    if (passthroughExtensionAvailable_)
    {
        passthroughProperties.next = systemPropertiesNext;
        systemPropertiesNext = &passthroughProperties;
    }
    if (handTrackingExtensionAvailable_)
    {
        handTrackingProperties.next = systemPropertiesNext;
        systemPropertiesNext = &handTrackingProperties;
    }
    systemProperties.next = systemPropertiesNext;
    if (XR_SUCCEEDED(xrGetSystemProperties(instance_, systemId_, &systemProperties)))
    {
        strncpy(headsetSystemName_, systemProperties.systemName, sizeof(headsetSystemName_) - 1);
        headsetSystemName_[sizeof(headsetSystemName_) - 1] = '\0';
        if (handTrackingExtensionAvailable_)
        {
            handTrackingSupported_ = handTrackingProperties.supportsHandTracking == XR_TRUE;
        }
        if (passthroughExtensionAvailable_)
        {
            passthroughSupported_ = passthroughProperties.supportsPassthrough == XR_TRUE;
        }
        LOGI("OpenXR system: name='%s' vendor=%u handTracking=%d/%d passthrough=%d/%d",
             headsetSystemName_, systemProperties.vendorId,
             handTrackingExtensionAvailable_ ? 1 : 0,
             handTrackingSupported_ ? 1 : 0,
             passthroughExtensionAvailable_ ? 1 : 0,
             passthroughSupported_ ? 1 : 0);
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

    if (passthroughExtensionAvailable_)
    {
        xrGetInstanceProcAddr(instance_, "xrCreatePassthroughFB",
                              reinterpret_cast<PFN_xrVoidFunction*>(&xrCreatePassthroughFB_));
        xrGetInstanceProcAddr(instance_, "xrDestroyPassthroughFB",
                              reinterpret_cast<PFN_xrVoidFunction*>(&xrDestroyPassthroughFB_));
        xrGetInstanceProcAddr(instance_, "xrPassthroughStartFB",
                              reinterpret_cast<PFN_xrVoidFunction*>(&xrPassthroughStartFB_));
        xrGetInstanceProcAddr(instance_, "xrPassthroughPauseFB",
                              reinterpret_cast<PFN_xrVoidFunction*>(&xrPassthroughPauseFB_));
        xrGetInstanceProcAddr(instance_, "xrCreatePassthroughLayerFB",
                              reinterpret_cast<PFN_xrVoidFunction*>(&xrCreatePassthroughLayerFB_));
        xrGetInstanceProcAddr(instance_, "xrDestroyPassthroughLayerFB",
                              reinterpret_cast<PFN_xrVoidFunction*>(&xrDestroyPassthroughLayerFB_));
        xrGetInstanceProcAddr(instance_, "xrPassthroughLayerResumeFB",
                              reinterpret_cast<PFN_xrVoidFunction*>(&xrPassthroughLayerResumeFB_));
        xrGetInstanceProcAddr(instance_, "xrPassthroughLayerPauseFB",
                              reinterpret_cast<PFN_xrVoidFunction*>(&xrPassthroughLayerPauseFB_));
        xrGetInstanceProcAddr(instance_, "xrPassthroughLayerSetStyleFB",
                              reinterpret_cast<PFN_xrVoidFunction*>(&xrPassthroughLayerSetStyleFB_));
        LOGI("Passthrough support: ext=%d runtime=%d create=%d layer=%d",
             passthroughExtensionAvailable_ ? 1 : 0,
             passthroughSupported_ ? 1 : 0,
             xrCreatePassthroughFB_ != nullptr ? 1 : 0,
             xrCreatePassthroughLayerFB_ != nullptr ? 1 : 0);
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
    appSpaceIsStage_ = XR_SUCCEEDED(spaceResult);
    if (XR_FAILED(spaceResult))
    {
        LOGW("STAGE space not available (%d), falling back to LOCAL", spaceResult);
        spaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        XR_CHECK(xrCreateReferenceSpace(session_, &spaceCreateInfo, &appSpace_),
                 "xrCreateReferenceSpace(LOCAL)");
        appSpaceIsStage_ = false;
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

    if (!InitializePassthrough())
    {
        LOGW("Passthrough unavailable on headset, shell will use the 3D fallback");
    }

    if (!InitializeDisplayRefreshRate(kPreferredDisplayRefreshRateHz))
    {
        LOGW("Display refresh rate control unavailable, continuing at runtime default");
    }

    return true;
}

bool XrApp::InitializeDisplayRefreshRate(float preferredRefreshRateHz)
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
                                        [preferredRefreshRateHz](float rate) {
                                            return std::fabs(rate - preferredRefreshRateHz) < 0.5f;
                                        });
    if (!hasPreferredRate)
    {
        LOGW("Preferred %.1fHz not advertised by runtime, keeping current refresh rate",
             preferredRefreshRateHz);
        if (xrGetDisplayRefreshRateFB_ != nullptr)
        {
            float currentRate = 0.0f;
            if (XR_SUCCEEDED(xrGetDisplayRefreshRateFB_(session_, &currentRate)))
            {
                LOGI("Current display refresh rate: %.1fHz", currentRate);
                clientRefreshRateHz_ = static_cast<uint32_t>(std::max(1.0f, std::round(currentRate)));
            }
        }
        return true;
    }

    XrResult requestResult =
        xrRequestDisplayRefreshRateFB_(session_, preferredRefreshRateHz);
    if (XR_FAILED(requestResult))
    {
        LOGW("Failed to request %.1fHz display refresh rate: %d",
             preferredRefreshRateHz, requestResult);
        return false;
    }

    LOGI("Requested display refresh rate: %.1fHz", preferredRefreshRateHz);

    if (xrGetDisplayRefreshRateFB_ != nullptr)
    {
        float currentRate = 0.0f;
        if (XR_SUCCEEDED(xrGetDisplayRefreshRateFB_(session_, &currentRate)))
        {
            LOGI("Current display refresh rate after request: %.1fHz", currentRate);
            clientRefreshRateHz_ = static_cast<uint32_t>(std::max(1.0f, std::round(currentRate)));
        }
        else
        {
            clientRefreshRateHz_ =
                static_cast<uint32_t>(std::max(1.0f, std::round(preferredRefreshRateHz)));
        }
    }
    else
    {
        clientRefreshRateHz_ =
            static_cast<uint32_t>(std::max(1.0f, std::round(preferredRefreshRateHz)));
    }

    return true;
}

bool XrApp::InitializeHandTracking()
{
    if (!handTrackingExtensionAvailable_ || !handTrackingSupported_ ||
        !xrCreateHandTrackerEXT_ || !xrDestroyHandTrackerEXT_ || !xrLocateHandJointsEXT_)
    {
        LOGW("Hand tracking unavailable: extension=%d runtime=%d create=%d destroy=%d locate=%d",
             handTrackingExtensionAvailable_ ? 1 : 0,
             handTrackingSupported_ ? 1 : 0,
             xrCreateHandTrackerEXT_ != nullptr ? 1 : 0,
             xrDestroyHandTrackerEXT_ != nullptr ? 1 : 0,
             xrLocateHandJointsEXT_ != nullptr ? 1 : 0);
        return false;
    }

    if (handTrackers_[0] != XR_NULL_HANDLE && handTrackers_[1] != XR_NULL_HANDLE)
    {
        return true;
    }

    if (handTrackers_[0] != XR_NULL_HANDLE || handTrackers_[1] != XR_NULL_HANDLE)
    {
        ShutdownHandTracking();
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

bool XrApp::InitializePassthrough()
{
    if (!passthroughExtensionAvailable_ || !passthroughSupported_ ||
        session_ == XR_NULL_HANDLE ||
        xrCreatePassthroughFB_ == nullptr ||
        xrCreatePassthroughLayerFB_ == nullptr ||
        xrPassthroughStartFB_ == nullptr ||
        xrPassthroughPauseFB_ == nullptr ||
        xrPassthroughLayerResumeFB_ == nullptr ||
        xrPassthroughLayerPauseFB_ == nullptr ||
        xrDestroyPassthroughFB_ == nullptr ||
        xrDestroyPassthroughLayerFB_ == nullptr)
    {
        return false;
    }

    if (passthrough_ != XR_NULL_HANDLE && passthroughLayer_ != XR_NULL_HANDLE)
    {
        return true;
    }

    XrPassthroughCreateInfoFB passthroughInfo = {XR_TYPE_PASSTHROUGH_CREATE_INFO_FB};
    XrResult result = xrCreatePassthroughFB_(session_, &passthroughInfo, &passthrough_);
    if (XR_FAILED(result))
    {
        LOGW("xrCreatePassthroughFB failed: %d", result);
        passthroughSupported_ = false;
        return false;
    }

    XrPassthroughLayerCreateInfoFB layerInfo = {XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB};
    layerInfo.passthrough = passthrough_;
    layerInfo.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
    result = xrCreatePassthroughLayerFB_(session_, &layerInfo, &passthroughLayer_);
    if (XR_FAILED(result))
    {
        LOGW("xrCreatePassthroughLayerFB failed: %d", result);
        ShutdownPassthrough();
        passthroughSupported_ = false;
        return false;
    }

    if (xrPassthroughLayerSetStyleFB_ != nullptr)
    {
        XrPassthroughStyleFB style = {XR_TYPE_PASSTHROUGH_STYLE_FB};
        style.textureOpacityFactor = 1.0f;
        style.edgeColor = {0.0f, 0.0f, 0.0f, 0.0f};
        (void)xrPassthroughLayerSetStyleFB_(passthroughLayer_, &style);
    }

    LOGI("Passthrough objects created for local shell");
    return true;
}

void XrApp::ShutdownPassthrough()
{
    SetShellPassthroughActive(false);
    if (passthroughLayer_ != XR_NULL_HANDLE && xrDestroyPassthroughLayerFB_ != nullptr)
    {
        xrDestroyPassthroughLayerFB_(passthroughLayer_);
        passthroughLayer_ = XR_NULL_HANDLE;
    }
    if (passthrough_ != XR_NULL_HANDLE && xrDestroyPassthroughFB_ != nullptr)
    {
        xrDestroyPassthroughFB_(passthrough_);
        passthrough_ = XR_NULL_HANDLE;
    }
    passthroughRunning_ = false;
    passthroughLayerRunning_ = false;
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

    // Aim pose action for local shell controller lasers. Falls back to grip pose at runtime.
    actionInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
    strncpy(actionInfo.actionName, "aim_pose", XR_MAX_ACTION_NAME_SIZE);
    strncpy(actionInfo.localizedActionName, "Aim Pose", XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
    XR_CHECK(xrCreateAction(actionSet_, &actionInfo, &aimPoseAction_), "xrCreateAction(aimPose)");

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

    XrPath bindingPaths[15];
    xrStringToPath(instance_, "/user/hand/left/input/grip/pose", &bindingPaths[0]);
    xrStringToPath(instance_, "/user/hand/right/input/grip/pose", &bindingPaths[1]);
    xrStringToPath(instance_, "/user/hand/left/input/aim/pose", &bindingPaths[2]);
    xrStringToPath(instance_, "/user/hand/right/input/aim/pose", &bindingPaths[3]);
    xrStringToPath(instance_, "/user/hand/left/input/trigger/value", &bindingPaths[4]);
    xrStringToPath(instance_, "/user/hand/right/input/trigger/value", &bindingPaths[5]);
    xrStringToPath(instance_, "/user/hand/left/input/squeeze/value", &bindingPaths[6]);
    xrStringToPath(instance_, "/user/hand/right/input/squeeze/value", &bindingPaths[7]);
    xrStringToPath(instance_, "/user/hand/left/input/thumbstick", &bindingPaths[8]);
    xrStringToPath(instance_, "/user/hand/right/input/thumbstick", &bindingPaths[9]);
    xrStringToPath(instance_, "/user/hand/left/input/x/click", &bindingPaths[10]);
    xrStringToPath(instance_, "/user/hand/right/input/a/click", &bindingPaths[11]);
    xrStringToPath(instance_, "/user/hand/left/input/y/click", &bindingPaths[12]);
    xrStringToPath(instance_, "/user/hand/right/input/b/click", &bindingPaths[13]);
    xrStringToPath(instance_, "/user/hand/left/input/menu/click", &bindingPaths[14]);

    XrActionSuggestedBinding bindingsWithAim[] = {
        {gripPoseAction_, bindingPaths[0]},
        {gripPoseAction_, bindingPaths[1]},
        {aimPoseAction_, bindingPaths[2]},
        {aimPoseAction_, bindingPaths[3]},
        {triggerAction_, bindingPaths[4]},
        {triggerAction_, bindingPaths[5]},
        {gripAction_, bindingPaths[6]},
        {gripAction_, bindingPaths[7]},
        {thumbstickAction_, bindingPaths[8]},
        {thumbstickAction_, bindingPaths[9]},
        {aButtonAction_, bindingPaths[10]},
        {aButtonAction_, bindingPaths[11]},
        {bButtonAction_, bindingPaths[12]},
        {bButtonAction_, bindingPaths[13]},
        {menuAction_, bindingPaths[14]},
    };
    XrActionSuggestedBinding bindingsWithoutAim[] = {
        {gripPoseAction_, bindingPaths[0]},
        {gripPoseAction_, bindingPaths[1]},
        {triggerAction_, bindingPaths[4]},
        {triggerAction_, bindingPaths[5]},
        {gripAction_, bindingPaths[6]},
        {gripAction_, bindingPaths[7]},
        {thumbstickAction_, bindingPaths[8]},
        {thumbstickAction_, bindingPaths[9]},
        {aButtonAction_, bindingPaths[10]},
        {aButtonAction_, bindingPaths[11]},
        {bButtonAction_, bindingPaths[12]},
        {bButtonAction_, bindingPaths[13]},
        {menuAction_, bindingPaths[14]},
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
        suggestedBindings.suggestedBindings = bindingsWithAim;
        suggestedBindings.countSuggestedBindings =
            sizeof(bindingsWithAim) / sizeof(bindingsWithAim[0]);
        XrResult suggestResult = xrSuggestInteractionProfileBindings(instance_, &suggestedBindings);
        if (XR_FAILED(suggestResult))
        {
            suggestedBindings.suggestedBindings = bindingsWithoutAim;
            suggestedBindings.countSuggestedBindings =
                sizeof(bindingsWithoutAim) / sizeof(bindingsWithoutAim[0]);
            suggestResult = xrSuggestInteractionProfileBindings(instance_, &suggestedBindings);
        }
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

        spaceInfo.action = aimPoseAction_;
        XR_CHECK(xrCreateActionSpace(session_, &spaceInfo, &aimSpaces_[hand]),
                 "xrCreateActionSpace(aim)");
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
    blitTextureUniform_ = glGetUniformLocation(blitProgram_, "uTexture");
    blitEyeSourceMinUniform_ = glGetUniformLocation(blitProgram_, "uEyeSourceMin");
    blitEyeSourceMaxUniform_ = glGetUniformLocation(blitProgram_, "uEyeSourceMax");
    blitLogicalTexelSizeUniform_ = glGetUniformLocation(blitProgram_, "uLogicalTexelSize");
    blitFoveatedEncodingEnabledUniform_ =
        glGetUniformLocation(blitProgram_, "uFoveatedEncodingEnabled");
    blitClientUpscalingEnabledUniform_ =
        glGetUniformLocation(blitProgram_, "uClientUpscalingEnabled");
    blitUpscaleEdgeThresholdUniform_ =
        glGetUniformLocation(blitProgram_, "uUpscaleEdgeThreshold");
    blitUpscaleSharpnessUniform_ = glGetUniformLocation(blitProgram_, "uUpscaleSharpness");
    blitFoveationCenterSizeUniform_ =
        glGetUniformLocation(blitProgram_, "uFoveationCenterSize");
    blitFoveationCenterShiftUniform_ =
        glGetUniformLocation(blitProgram_, "uFoveationCenterShift");
    blitFoveationEdgeRatioUniform_ =
        glGetUniformLocation(blitProgram_, "uFoveationEdgeRatio");
    blitFoveationEyeSizeRatioUniform_ =
        glGetUniformLocation(blitProgram_, "uFoveationEyeSizeRatio");
    blitReprojectionWarpEnabledUniform_ =
        glGetUniformLocation(blitProgram_, "uReprojectionWarpEnabled");
    blitReprojectionWarpOffsetUniform_ =
        glGetUniformLocation(blitProgram_, "uReprojectionWarpOffset");
    blitPassthroughAlphaEnabledUniform_ =
        glGetUniformLocation(blitProgram_, "uPassthroughAlphaEnabled");

    glUseProgram(blitProgram_);
    glUniform1i(blitTextureUniform_, 0);
    glUniform1f(blitUpscaleEdgeThresholdUniform_, 4.0f / 255.0f);
    glUniform1f(blitUpscaleSharpnessUniform_, 2.0f);
    glUniform1i(blitReprojectionWarpEnabledUniform_, 0);
    glUniform2f(blitReprojectionWarpOffsetUniform_, 0.0f, 0.0f);
    glUniform1i(blitPassthroughAlphaEnabledUniform_, 0);
    glUseProgram(0);

    // Full-screen quad
    float quadVertices[] = {
        // pos x,y    uv u,v
        -1.0f, -1.0f,  0.0f, 1.0f,
         1.0f, -1.0f,  1.0f, 1.0f,
         1.0f,  1.0f,  1.0f, 0.0f,
        -1.0f, -1.0f,  0.0f, 1.0f,
         1.0f,  1.0f,  1.0f, 0.0f,
        -1.0f,  1.0f,  0.0f, 0.0f,
    };

    glGenVertexArrays(1, &blitVao_);
    glGenBuffers(1, &blitVbo_);
    glBindVertexArray(blitVao_);
    glBindBuffer(GL_ARRAY_BUFFER, blitVbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), quadVertices, GL_STATIC_DRAW);
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

    if (!CreateShellResources())
    {
        LOGE("Failed to create local shell GL resources");
        return false;
    }

    if (clientFoveationPreset_ != protocol::ClientFoveationPreset::Off &&
        !InitializeFoveation())
    {
        LOGW("Foveation unavailable on headset, continuing without it");
    }

    // Start networking after swapchains are ready
    StartNetworking();

    return true;
}

bool XrApp::InitializeFoveation()
{
    if (clientFoveationPreset_ == protocol::ClientFoveationPreset::Off)
    {
        ShutdownFoveation();
        LOGI("Dynamic FFR disabled or unmanaged by server preset");
        return false;
    }

    if (!foveationAvailable_ || !foveationConfigurationAvailable_ || !swapchainUpdateAvailable_ ||
        !xrCreateFoveationProfileFB_ || !xrDestroyFoveationProfileFB_ || !xrUpdateSwapchainFB_)
    {
        return false;
    }

    ShutdownFoveation();

    XrFoveationLevelProfileCreateInfoFB levelProfile = {
        XR_TYPE_FOVEATION_LEVEL_PROFILE_CREATE_INFO_FB};
    levelProfile.level = ToXrFoveationLevel(clientFoveationPreset_);
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

    LOGI("Dynamic FFR enabled via XR_FB_foveation preset=%u",
         static_cast<uint32_t>(clientFoveationPreset_));
    return true;
}

void XrApp::ApplyClientFoveationPreset(protocol::ClientFoveationPreset preset)
{
    clientFoveationPreset_ = preset;
    if (swapchains_[0] == XR_NULL_HANDLE && swapchains_[1] == XR_NULL_HANDLE)
    {
        return;
    }
    if (!InitializeFoveation() && preset != protocol::ClientFoveationPreset::Off)
    {
        LOGW("Requested client foveation preset %u could not be applied",
             static_cast<uint32_t>(preset));
    }
}

void XrApp::ShutdownFoveation()
{
    if (foveationProfile_ != XR_NULL_HANDLE && xrDestroyFoveationProfileFB_ != nullptr)
    {
        if (xrUpdateSwapchainFB_ != nullptr)
        {
            for (int eye = 0; eye < 2; ++eye)
            {
                if (swapchains_[eye] == XR_NULL_HANDLE)
                {
                    continue;
                }
                XrSwapchainStateFoveationFB state = {XR_TYPE_SWAPCHAIN_STATE_FOVEATION_FB};
                state.profile = XR_NULL_HANDLE;
                xrUpdateSwapchainFB_(
                    swapchains_[eye],
                    reinterpret_cast<const XrSwapchainStateBaseHeaderFB*>(&state));
            }
        }
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
    StartStreamConfigWorker();
    lastLatencyReportTime_ = {};
    lastKeyframeRequestTime_ = {};
    lastObservedDroppedFrames_ = 0;
    latencySamples_ = {};
    lastFrameReceiveTimeNs_ = 0;
    lastFrameSubmitTimeNs_ = 0;
    lastFrameAcquireTimeNs_ = 0;
    lastReportedAcquireTimeNs_ = 0;
    skippedDecodedFrames_ = 0;
    decoderEncodedWidth_ = 0;
    decoderEncodedHeight_ = 0;
    activeVideoCodec_.store(protocol::VideoCodec::H265);
    presentedVideoFrame_ = {};
    hasObservedProtocolAlphaFrame_ = false;
    loggedTransparentClearFallback_ = false;
    reprojectedFramesSinceLastReport_ = 0;
    renderPoseFallbacksSinceLastReport_ = 0;
    staleFrameReusesSinceLastReport_ = 0;
    reprojectionWarpEnabled_ = false;
    reprojectionWarpOffsetX_ = 0.0f;
    reprojectionWarpOffsetY_ = 0.0f;
    transportMode_ = TransportMode::WifiUdp;
    lastUsbAdbRetryTime_ = {};
    usbAdbRetryAttempts_ = 0;
    connectionState_.store(ConnectionState::Disconnected);
    shellStatusText_ = "Preparing network";
    needsReconnect_.store(false);

    if (TryStartUsbAdbTransport())
    {
        LOGI("USB ADB transport selected");
        return;
    }
    lastUsbAdbRetryTime_ = std::chrono::steady_clock::now();

    LOGI("USB ADB transport unavailable, starting WiFi discovery mode");
    connectionState_.store(ConnectionState::Discovering);
    shellStatusText_ = "Waiting for server";

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
    StopControlReceiver();
    StopStreamConfigWorker();
    CloseControlSocket();
    CloseSpatialSocket();
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
        std::lock_guard<std::mutex> decoderLock(videoDecoderMutex_);
        videoDecoder_->Shutdown();
        videoDecoder_.reset();
    }

    transportMode_ = TransportMode::WifiUdp;
    connectionState_.store(ConnectionState::Disconnected);
    shellStatusText_ = reason != nullptr ? reason : "Disconnected";
    needsReconnect_.store(false);
    serverIp_[0] = '\0';
    serverVideoPort_ = 0;
    serverTrackingPort_ = 0;
    serverSpatialPort_ = 0;
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
    presentedVideoFrame_ = {};
    currentRenderPose_ = {};
    reprojectionWarpEnabled_ = false;
    reprojectionWarpOffsetX_ = 0.0f;
    reprojectionWarpOffsetY_ = 0.0f;
    lastVideoFrameTime_ = {};
    videoContentUMin_ = 0.0f;
    videoContentUMax_ = 1.0f;
    videoContentVMin_ = 0.0f;
    videoContentVMax_ = 1.0f;
    serverFoveatedEncodingEnabled_ = false;
    clientUpscalingEnabled_ = false;
    serverMixedRealityPassthroughEnabled_ = false;
    hasObservedProtocolAlphaFrame_ = false;
    loggedTransparentClearFallback_ = false;
    clientReprojectionMode_ = protocol::ClientReprojectionMode::Pose;
    foveationCenterSizeX_ = 1.0f;
    foveationCenterSizeY_ = 1.0f;
    foveationCenterShiftX_ = 0.0f;
    foveationCenterShiftY_ = 0.0f;
    foveationEdgeRatioX_ = 1.0f;
    foveationEdgeRatioY_ = 1.0f;
    foveationEyeWidthRatio_ = 1.0f;
    foveationEyeHeightRatio_ = 1.0f;
    decodedTexelWidth_ = 1.0f;
    decodedTexelHeight_ = 1.0f;
    lastFrameReceiveTimeNs_ = 0;
    lastFrameSubmitTimeNs_ = 0;
    lastFrameAcquireTimeNs_ = 0;
    lastReportedAcquireTimeNs_ = 0;
    skippedDecodedFrames_ = 0;
    reprojectedFramesSinceLastReport_ = 0;
    renderPoseFallbacksSinceLastReport_ = 0;
    staleFrameReusesSinceLastReport_ = 0;
    renderPoseHitCount_ = 0;
    renderPoseMissCount_ = 0;
    lastRenderPoseLogTime_ = {};
    lastLatencyReportTime_ = {};
    lastKeyframeRequestTime_ = {};
    lastObservedDroppedFrames_ = 0;
    {
        std::lock_guard<std::mutex> pendingLock(pendingStreamConfigMutex_);
        pendingStreamConfigUpdate_ = {};
        hasPendingStreamConfigUpdate_ = false;
    }
    {
        std::lock_guard<std::mutex> completedLock(completedStreamConfigMutex_);
        completedStreamConfigUpdate_ = {};
        hasCompletedStreamConfigUpdate_ = false;
        completedStreamConfigAccepted_ = false;
    }
    streamConfigWorkerSequence_.store(0);
    streamConfigSequence_ = 0;
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
    shellStatusText_ = reason != nullptr ? reason : "Connection lost";
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

    timeval timeout = {0, 250000};
    setsockopt(controlSocket_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
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

bool XrApp::OpenUsbSpatialSocket(uint16_t spatialPort)
{
    CloseSpatialSocket();
    if (spatialPort == 0)
    {
        return false;
    }

    spatialTcpSocket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (spatialTcpSocket_ < 0)
    {
        LOGE("Failed to create USB TCP spatial socket");
        return false;
    }
    ConfigureTcpSocket(spatialTcpSocket_);

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(spatialPort);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(spatialTcpSocket_, (sockaddr*)&addr, sizeof(addr)) < 0)
    {
        LOGW("USB TCP spatial socket unavailable on port %u", spatialPort);
        close(spatialTcpSocket_);
        spatialTcpSocket_ = -1;
        return false;
    }

    LOGI("USB TCP spatial channel connected on port %u", spatialPort);
    return true;
}

void XrApp::CloseSpatialSocket()
{
    if (spatialTcpSocket_ >= 0)
    {
        SendTcpRecord(spatialTcpSocket_, protocol::TcpRecordType::Disconnect, nullptr, 0);
        close(spatialTcpSocket_);
        spatialTcpSocket_ = -1;
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
        payload.size() < protocol::SERVER_ANNOUNCE_BASE_SIZE)
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

    protocol::ServerAnnounce server = {};
    memcpy(&server, payload.data(), std::min(payload.size(), sizeof(server)));
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
    shellStatusText_ = transportMode == TransportMode::UsbAdbTcp
        ? "Connecting USB"
        : "Connecting WiFi";

    transportMode_ = transportMode;
    const bool usbAdb = transportMode_ == TransportMode::UsbAdbTcp;

    LOGI("Server discovered via %s: %s at %s (%ux%u @ %uHz, encoded %ux%u)",
         usbAdb ? "USB ADB" : "WiFi",
         server.serverName, serverIp, server.renderWidth, server.renderHeight,
         server.refreshRateHz, server.encodedWidth, server.encodedHeight);

    serverVideoPort_ = server.videoPort;
    serverTrackingPort_ = server.trackingPort;
    serverSpatialPort_ = server.spatialPort != 0 ? static_cast<uint16_t>(server.spatialPort)
                                                  : protocol::SPATIAL_PORT;
    videoWidth_ = server.renderWidth;
    videoHeight_ = server.renderHeight;
    strncpy(serverIp_, serverIp, sizeof(serverIp_) - 1);
    serverIp_[sizeof(serverIp_) - 1] = '\0';
    connectionTime_ = std::chrono::steady_clock::now();

    serverFoveatedEncodingEnabled_ =
        (server.serverFeatures & protocol::SERVER_FEATURE_FOVEATED_ENCODING) != 0 &&
        server.foveatedEncodingPreset != protocol::FoveationPreset::Off;
    clientUpscalingEnabled_ =
        (server.serverFeatures & protocol::SERVER_FEATURE_CLIENT_UPSCALING) != 0 &&
        server.clientUpscalingMode != protocol::ClientUpscalingMode::Off;
    const bool serverRequestsPassthrough =
        (server.serverFeatures & protocol::SERVER_FEATURE_MIXED_REALITY_PASSTHROUGH) != 0;
    serverMixedRealityPassthroughEnabled_ =
        serverRequestsPassthrough && CanUseShellPassthrough();
    hasObservedProtocolAlphaFrame_ = false;
    loggedTransparentClearFallback_ = false;
    if (serverRequestsPassthrough && !serverMixedRealityPassthroughEnabled_)
    {
        LOGW("Server enabled passthrough, but this headset cannot use XR_FB_passthrough "
             "(extension=%d runtime=%d create=%d layer=%d handles=%d/%d)",
             passthroughExtensionAvailable_ ? 1 : 0,
             passthroughSupported_ ? 1 : 0,
             xrCreatePassthroughFB_ != nullptr ? 1 : 0,
             xrCreatePassthroughLayerFB_ != nullptr ? 1 : 0,
             passthrough_ != XR_NULL_HANDLE ? 1 : 0,
             passthroughLayer_ != XR_NULL_HANDLE ? 1 : 0);
    }
    switch (server.clientReprojectionMode)
    {
        case protocol::ClientReprojectionMode::Off:
        case protocol::ClientReprojectionMode::Pose:
        case protocol::ClientReprojectionMode::PoseWarp:
            clientReprojectionMode_ = server.clientReprojectionMode;
            break;
        default:
            clientReprojectionMode_ = protocol::ClientReprojectionMode::Pose;
            break;
    }
    foveationCenterSizeX_ = server.foveationCenterSizeX > 0.0f ? server.foveationCenterSizeX : 1.0f;
    foveationCenterSizeY_ = server.foveationCenterSizeY > 0.0f ? server.foveationCenterSizeY : 1.0f;
    foveationCenterShiftX_ = server.foveationCenterShiftX;
    foveationCenterShiftY_ = server.foveationCenterShiftY;
    foveationEdgeRatioX_ = std::max(server.foveationEdgeRatioX, 1.0f);
    foveationEdgeRatioY_ = std::max(server.foveationEdgeRatioY, 1.0f);
    const bool clientFoveationOverride =
        (server.serverFeatures & protocol::SERVER_FEATURE_CLIENT_FOVEATION) != 0;
    if (clientFoveationOverride)
    {
        ApplyClientFoveationPreset(server.clientFoveationPreset);
    }
    else
    {
        clientFoveationPreset_ = protocol::ClientFoveationPreset::Off;
        ShutdownFoveation();
    }

    if (server.refreshRateHz > 0)
    {
        if (!InitializeDisplayRefreshRate(static_cast<float>(server.refreshRateHz)))
        {
            LOGW("Server-requested %.1fHz refresh unavailable; reporting active refresh",
                 static_cast<float>(server.refreshRateHz));
        }
    }

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
    decodedTexelWidth_ = 1.0f / static_cast<float>(std::max(decoderWidth, 1u));
    decodedTexelHeight_ = 1.0f / static_cast<float>(std::max(decoderHeight, 1u));
    if (serverFoveatedEncodingEnabled_)
    {
        foveationEyeWidthRatio_ = FoveationActiveRatio(
            static_cast<float>(std::max(server.renderWidth / 2, 1u)),
            static_cast<float>(std::max(decoderWidth / 2, 1u)),
            foveationCenterSizeX_,
            foveationEdgeRatioX_);
        foveationEyeHeightRatio_ = FoveationActiveRatio(
            static_cast<float>(std::max(server.renderHeight, 1u)),
            static_cast<float>(std::max(decoderHeight, 1u)),
            foveationCenterSizeY_,
            foveationEdgeRatioY_);
    }
    else
    {
        foveationEyeWidthRatio_ = 1.0f;
        foveationEyeHeightRatio_ = 1.0f;
    }

    LOGI("Server stream options: features=0x%x ffe=%d ffr=%s(%u) upscaling=%d reprojection=%s passthrough=%d spatialPort=%u activeRatio=%.4fx%.4f",
         server.serverFeatures,
         serverFoveatedEncodingEnabled_ ? 1 : 0,
         clientFoveationOverride ? "override" : "auto",
         static_cast<uint32_t>(server.clientFoveationPreset),
         clientUpscalingEnabled_ ? 1 : 0,
         ReprojectionModeName(clientReprojectionMode_),
         serverMixedRealityPassthroughEnabled_ ? 1 : 0,
         serverSpatialPort_,
         foveationEyeWidthRatio_,
         foveationEyeHeightRatio_);

    // CRITICAL: Start video receiver BEFORE sending ClientConnect.
    // The server starts encoding immediately upon receiving ClientConnect.
    // If we send ClientConnect first, the initial keyframe packets arrive at port 9944
    // before we've bound the socket, and they're silently dropped by the OS.
    decoderEncodedWidth_ = decoderWidth;
    decoderEncodedHeight_ = decoderHeight;
    LOGI("Video decoder will initialize from first NAL codec: %ux%u (encoded), render %ux%u",
         decoderWidth, decoderHeight, videoWidth_, videoHeight_);

    // Start receiving video packets (bind socket BEFORE telling server we're ready)
    bool receivingStarted = false;
    if (networkReceiver_)
    {
        auto nalCallback = [this](const uint8_t* data, size_t size,
                                  int64_t timestampNs, int64_t receiveTimeNs,
                                  uint8_t flags, uint8_t codec)
        {
            OnNalUnitReceived(data, size, timestampNs, receiveTimeNs, flags, codec);
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
    if (usbAdb && controlTcpSocket_ >= 0)
    {
        timeval timeout = {0, 0};
        setsockopt(controlTcpSocket_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        OpenUsbSpatialSocket(serverSpatialPort_);
    }

    // NOW send ClientConnect — server will start sending video, and we're already listening
    SendClientConnect(serverIp);
    StartControlReceiver();
    connectionState_.store(ConnectionState::Connected);
    shellStatusText_ = "Waiting for video";
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
    connect.versionMinor = 2;
    connect.preferredCodec = static_cast<uint32_t>(protocol::VideoCodec::H265);
    connect.supportedCodecs =
        protocol::CLIENT_CODEC_CAPABILITY_H265 |
        protocol::CLIENT_CODEC_CAPABILITY_H264;
    connect.maxBitrateMbps = usbAdb
        ? protocol::CLIENT_MAX_BITRATE_USE_SERVER_CONFIG
        : 100;
    connect.refreshRateHz = clientRefreshRateHz_;
    connect.clientCapabilities =
        protocol::CLIENT_CAPABILITY_FOVEATED_ENCODING |
        protocol::CLIENT_CAPABILITY_CLIENT_UPSCALING |
        protocol::CLIENT_CAPABILITY_STREAM_RECONFIGURE;
    if (foveationAvailable_ && foveationConfigurationAvailable_ && swapchainUpdateAvailable_)
    {
        connect.clientCapabilities |= protocol::CLIENT_CAPABILITY_CLIENT_FOVEATION;
    }
    if (CanUseShellPassthrough())
    {
        connect.clientCapabilities |= protocol::CLIENT_CAPABILITY_MIXED_REALITY_PASSTHROUGH;
    }
    connect.audioSampleRateHz = 48000;
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

    LOGI("Sent ClientConnect via %s to %s:%d (device='%s' refresh=%uHz maxBitrate=%u capabilities=0x%x passthrough=%d)",
         usbAdb ? "USB ADB" : "WiFi", serverIp, protocol::CONTROL_PORT,
         connect.deviceName, clientRefreshRateHz_, connect.maxBitrateMbps,
         connect.clientCapabilities,
         (connect.clientCapabilities & protocol::CLIENT_CAPABILITY_MIXED_REALITY_PASSTHROUGH) != 0
             ? 1
             : 0);
}

void XrApp::StartControlReceiver()
{
    if (controlReceiveThread_.joinable())
    {
        StopControlReceiver();
    }

    const bool usbAdb = transportMode_ == TransportMode::UsbAdbTcp;
    if ((!usbAdb && controlSocket_ < 0) || (usbAdb && controlTcpSocket_ < 0))
    {
        return;
    }

    controlReceiveRunning_.store(true);
    controlReceiveThread_ = std::thread(&XrApp::ControlReceiveThreadMain, this);
}

void XrApp::StopControlReceiver()
{
    if (!controlReceiveRunning_.load() && !controlReceiveThread_.joinable())
    {
        return;
    }

    controlReceiveRunning_.store(false);
    if (controlTcpSocket_ >= 0)
    {
        shutdown(controlTcpSocket_, SHUT_RDWR);
    }
    if (controlSocket_ >= 0)
    {
        shutdown(controlSocket_, SHUT_RDWR);
    }
    if (controlReceiveThread_.joinable())
    {
        if (controlReceiveThread_.get_id() == std::this_thread::get_id())
        {
            controlReceiveThread_.detach();
        }
        else
        {
            controlReceiveThread_.join();
        }
    }
}

void XrApp::ControlReceiveThreadMain()
{
    const bool usbAdb = transportMode_ == TransportMode::UsbAdbTcp;
    uint8_t buffer[512] = {};

    while (controlReceiveRunning_.load())
    {
        if (usbAdb)
        {
            protocol::TcpRecordHeader header = {};
            std::vector<uint8_t> payload;
            if (!ReadTcpRecord(controlTcpSocket_, header, payload))
            {
                if (controlReceiveRunning_.load())
                {
                    OnConnectionLost("USB control socket closed");
                }
                break;
            }
            if (header.type == protocol::TcpRecordType::Control)
            {
                HandleControlPayload(payload.data(), payload.size());
            }
            else if (header.type == protocol::TcpRecordType::Disconnect)
            {
                OnConnectionLost("server disconnected");
                break;
            }
            continue;
        }

        const ssize_t received = recv(controlSocket_, buffer, sizeof(buffer), 0);
        if (received < 0)
        {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            {
                continue;
            }
            if (controlReceiveRunning_.load())
            {
                OnConnectionLost("control socket receive failed");
            }
            break;
        }
        if (received == 0)
        {
            continue;
        }
        HandleControlPayload(buffer, static_cast<size_t>(received));
    }
}

void XrApp::HandleControlPayload(const uint8_t* data, size_t size)
{
    if (data == nullptr || size < 1)
    {
        return;
    }
    const auto type = static_cast<protocol::ControlType>(data[0]);
    if (type == protocol::ControlType::StreamConfigUpdate &&
        size >= sizeof(protocol::StreamConfigUpdate))
    {
        protocol::StreamConfigUpdate update = {};
        memcpy(&update, data, sizeof(update));
        HandleStreamConfigUpdate(update);
    }
}

void XrApp::HandleStreamConfigUpdate(const protocol::StreamConfigUpdate& update)
{
    if (update.encodedWidth == 0 ||
        update.encodedHeight == 0 ||
        update.renderWidth == 0 ||
        update.renderHeight == 0)
    {
        SendStreamConfigAck(update, protocol::STREAM_CONFIG_ACK_REJECTED);
        return;
    }

    const uint32_t currentSequence = streamConfigSequence_.load();
    if (update.sequence < currentSequence)
    {
        SendStreamConfigAck(update, protocol::STREAM_CONFIG_ACK_REJECTED);
        return;
    }
    if (update.sequence == currentSequence)
    {
        SendStreamConfigAck(update, protocol::STREAM_CONFIG_ACK_OK);
        return;
    }

    const uint32_t workerSequence = streamConfigWorkerSequence_.load();
    if (workerSequence == update.sequence)
    {
        return;
    }
    if (workerSequence != 0)
    {
        SendStreamConfigAck(update, protocol::STREAM_CONFIG_ACK_REJECTED);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(pendingStreamConfigMutex_);
        pendingStreamConfigUpdate_ = update;
        hasPendingStreamConfigUpdate_ = true;
        streamConfigWorkerSequence_.store(update.sequence);
    }
    pendingStreamConfigCv_.notify_one();

    LOGI("Queued stream config update seq=%u render=%ux%u encoded=%ux%u flags=0x%x",
         update.sequence,
         update.renderWidth,
         update.renderHeight,
         update.encodedWidth,
         update.encodedHeight,
         update.flags);
}

void XrApp::StartStreamConfigWorker()
{
    StopStreamConfigWorker();
    streamConfigWorkerRunning_.store(true);
    streamConfigWorkerThread_ = std::thread(&XrApp::StreamConfigWorkerMain, this);
}

void XrApp::StopStreamConfigWorker()
{
    if (!streamConfigWorkerRunning_.load() && !streamConfigWorkerThread_.joinable())
    {
        return;
    }

    streamConfigWorkerRunning_.store(false);
    {
        std::lock_guard<std::mutex> lock(pendingStreamConfigMutex_);
        hasPendingStreamConfigUpdate_ = false;
        pendingStreamConfigUpdate_ = {};
    }
    pendingStreamConfigCv_.notify_all();
    if (streamConfigWorkerThread_.joinable())
    {
        streamConfigWorkerThread_.join();
    }
}

void XrApp::StreamConfigWorkerMain()
{
    while (streamConfigWorkerRunning_.load())
    {
        protocol::StreamConfigUpdate update = {};
        {
            std::unique_lock<std::mutex> lock(pendingStreamConfigMutex_);
            pendingStreamConfigCv_.wait(lock, [this] {
                return !streamConfigWorkerRunning_.load() || hasPendingStreamConfigUpdate_;
            });
            if (!streamConfigWorkerRunning_.load())
            {
                break;
            }
            update = pendingStreamConfigUpdate_;
            pendingStreamConfigUpdate_ = {};
            hasPendingStreamConfigUpdate_ = false;
        }

        bool accepted = false;
        {
            std::lock_guard<std::mutex> decoderLock(videoDecoderMutex_);
            if (videoDecoder_)
            {
                const protocol::VideoCodec codec = activeVideoCodec_.load();
                videoDecoder_->Shutdown();
                accepted = videoDecoder_->Initialize(update.encodedWidth, update.encodedHeight, codec);
                if (accepted)
                {
                    decoderEncodedWidth_ = update.encodedWidth;
                    decoderEncodedHeight_ = update.encodedHeight;
                }
            }
        }

        {
            std::lock_guard<std::mutex> completedLock(completedStreamConfigMutex_);
            completedStreamConfigUpdate_ = update;
            completedStreamConfigAccepted_ = accepted;
            hasCompletedStreamConfigUpdate_ = true;
        }
    }
}

void XrApp::ApplyCompletedStreamConfigUpdate()
{
    protocol::StreamConfigUpdate update = {};
    bool accepted = false;
    {
        std::lock_guard<std::mutex> completedLock(completedStreamConfigMutex_);
        if (!hasCompletedStreamConfigUpdate_)
        {
            return;
        }
        update = completedStreamConfigUpdate_;
        accepted = completedStreamConfigAccepted_;
        completedStreamConfigUpdate_ = {};
        completedStreamConfigAccepted_ = false;
        hasCompletedStreamConfigUpdate_ = false;
    }

    const uint32_t workerSequence = streamConfigWorkerSequence_.load();
    if (connectionState_.load() != ConnectionState::Connected ||
        workerSequence != update.sequence ||
        update.sequence <= streamConfigSequence_.load())
    {
        LOGW("Ignored stale stream config result seq=%u worker_seq=%u current_seq=%u",
             update.sequence,
             workerSequence,
             streamConfigSequence_.load());
        if (workerSequence == update.sequence)
        {
            streamConfigWorkerSequence_.store(0);
        }
        return;
    }

    if (!accepted)
    {
        LOGE("Rejected stream config update seq=%u: decoder init failed for %ux%u",
             update.sequence, update.encodedWidth, update.encodedHeight);
        SendStreamConfigAck(update, protocol::STREAM_CONFIG_ACK_REJECTED);
        streamConfigWorkerSequence_.store(0);
        return;
    }

    videoWidth_ = update.renderWidth;
    videoHeight_ = update.renderHeight;
    decodedTexelWidth_ = 1.0f / static_cast<float>(std::max(update.encodedWidth, 1u));
    decodedTexelHeight_ = 1.0f / static_cast<float>(std::max(update.encodedHeight, 1u));
    serverFoveatedEncodingEnabled_ =
        (update.flags & protocol::STREAM_CONFIG_FLAG_FOVEATED_ENCODING) != 0 &&
        update.foveatedEncodingPreset != protocol::FoveationPreset::Off;
    clientUpscalingEnabled_ =
        (update.flags & protocol::STREAM_CONFIG_FLAG_CLIENT_UPSCALING) != 0 &&
        update.clientUpscalingMode != protocol::ClientUpscalingMode::Off;
    foveationCenterSizeX_ = update.foveationCenterSizeX > 0.0f
        ? update.foveationCenterSizeX
        : 1.0f;
    foveationCenterSizeY_ = update.foveationCenterSizeY > 0.0f
        ? update.foveationCenterSizeY
        : 1.0f;
    foveationCenterShiftX_ = update.foveationCenterShiftX;
    foveationCenterShiftY_ = update.foveationCenterShiftY;
    foveationEdgeRatioX_ = std::max(update.foveationEdgeRatioX, 1.0f);
    foveationEdgeRatioY_ = std::max(update.foveationEdgeRatioY, 1.0f);
    if (serverFoveatedEncodingEnabled_)
    {
        foveationEyeWidthRatio_ = FoveationActiveRatio(
            static_cast<float>(std::max(update.renderWidth / 2, 1u)),
            static_cast<float>(std::max(update.encodedWidth / 2, 1u)),
            foveationCenterSizeX_,
            foveationEdgeRatioX_);
        foveationEyeHeightRatio_ = FoveationActiveRatio(
            static_cast<float>(std::max(update.renderHeight, 1u)),
            static_cast<float>(std::max(update.encodedHeight, 1u)),
            foveationCenterSizeY_,
            foveationEdgeRatioY_);
    }
    else
    {
        foveationEyeWidthRatio_ = 1.0f;
        foveationEyeHeightRatio_ = 1.0f;
    }

    hasVideoTexture_ = false;
    hasCurrentRenderPose_ = false;
    presentedVideoFrame_ = {};
    currentRenderPose_ = {};
    videoContentUMin_ = 0.0f;
    videoContentUMax_ = 1.0f;
    videoContentVMin_ = 0.0f;
    videoContentVMax_ = 1.0f;
    lastVideoFrameTime_ = {};
    lastFrameReceiveTimeNs_ = 0;
    lastFrameSubmitTimeNs_ = 0;
    lastFrameAcquireTimeNs_ = 0;
    lastReportedAcquireTimeNs_ = 0;
    skippedDecodedFrames_ = 0;
    decodedFrameCount_ = 0;
    nalUnitsReceived_ = 0;
    streamConfigSequence_.store(update.sequence);

    SendStreamConfigAck(update, protocol::STREAM_CONFIG_ACK_OK);
    streamConfigWorkerSequence_.store(0);
    RequestKeyframe(protocol::KEYFRAME_REASON_STREAM_RECOVERY, update.sequence);

    LOGI("Applied stream config update seq=%u render=%ux%u encoded=%ux%u ffe=%d upscaling=%d",
         update.sequence,
         update.renderWidth,
         update.renderHeight,
         update.encodedWidth,
         update.encodedHeight,
         serverFoveatedEncodingEnabled_ ? 1 : 0,
         clientUpscalingEnabled_ ? 1 : 0);
}

void XrApp::SendStreamConfigAck(const protocol::StreamConfigUpdate& update, uint8_t status)
{
    const bool usbAdb = transportMode_ == TransportMode::UsbAdbTcp;
    if ((!usbAdb && controlSocket_ < 0) || (usbAdb && controlTcpSocket_ < 0))
    {
        return;
    }

    protocol::StreamConfigAck ack = {};
    ack.status = status;
    ack.sequence = update.sequence;
    ack.encodedWidth = update.encodedWidth;
    ack.encodedHeight = update.encodedHeight;

    if (usbAdb)
    {
        SendTcpRecord(controlTcpSocket_, protocol::TcpRecordType::Control,
                      &ack, sizeof(ack));
    }
    else
    {
        send(controlSocket_, &ack, sizeof(ack), MSG_DONTWAIT);
    }
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
        compositorSummary.count == 0 && totalSummary.count == 0 &&
        ageSummary.count == 0 &&
        reprojectedFramesSinceLastReport_ == 0 &&
        staleFrameReusesSinceLastReport_ == 0 &&
        renderPoseFallbacksSinceLastReport_ == 0)
    {
        return;
    }

    protocol::LatencyReport report = {};
    report.type = protocol::ControlType::LatencyReport;
    report.receiveToDecoderSubmitMs = static_cast<float>(receiveSummary.average);
    report.decodeLatencyMs = static_cast<float>(decodeSummary.average);
    report.compositorLatencyMs = static_cast<float>(compositorSummary.average);
    report.totalClientLatencyMs = static_cast<float>(totalSummary.average);
    report.displayedFrameAgeMs = static_cast<float>(ageSummary.average);
    report.reprojectedFrames = reprojectedFramesSinceLastReport_;
    report.staleFrameReuses = staleFrameReusesSinceLastReport_;
    report.renderPoseFallbacks = renderPoseFallbacksSinceLastReport_;
    report.reprojectionMode = clientReprojectionMode_;
    if (usbAdb)
    {
        SendTcpRecord(controlTcpSocket_, protocol::TcpRecordType::Control,
                      &report, sizeof(report));
    }
    else
    {
        send(controlSocket_, &report, sizeof(report), MSG_DONTWAIT);
    }

    LOGI("Latency report: recv->submit avg/p95=%.2f/%.2fms decode=%.2f/%.2f compositor=%.2f/%.2f total=%.2f/%.2f age=%.2f/%.2f reproj=%u stale=%u poseFallbacks=%u mode=%s skipped=%u",
         receiveSummary.average, receiveSummary.p95,
         decodeSummary.average, decodeSummary.p95,
         compositorSummary.average, compositorSummary.p95,
         totalSummary.average, totalSummary.p95,
         ageSummary.average, ageSummary.p95,
         reprojectedFramesSinceLastReport_,
         staleFrameReusesSinceLastReport_,
         renderPoseFallbacksSinceLastReport_,
         ReprojectionModeName(clientReprojectionMode_),
         skippedDecodedFrames_);

    latencySamples_.receiveToSubmitMs.clear();
    latencySamples_.submitToDecodeMs.clear();
    latencySamples_.decodeToCompositorMs.clear();
    latencySamples_.totalClientMs.clear();
    latencySamples_.frameAgeMs.clear();
    skippedDecodedFrames_ = 0;
    reprojectedFramesSinceLastReport_ = 0;
    staleFrameReusesSinceLastReport_ = 0;
    renderPoseFallbacksSinceLastReport_ = 0;
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

XrPosef XrApp::BuildCurrentHeadPose() const
{
    XrPosef pose = {};
    pose.orientation = NormalizeQuaternion(views_[0].pose.orientation);
    pose.position = {
        (views_[0].pose.position.x + views_[1].pose.position.x) * 0.5f,
        (views_[0].pose.position.y + views_[1].pose.position.y) * 0.5f,
        (views_[0].pose.position.z + views_[1].pose.position.z) * 0.5f,
    };
    return pose;
}

bool XrApp::ResolveRenderPoseForFrame(int64_t presentationTimeUs,
                                      NetworkReceiver::RenderPose* renderPose,
                                      bool* usedFallback)
{
    if (renderPose == nullptr || usedFallback == nullptr || networkReceiver_ == nullptr)
    {
        return false;
    }

    *usedFallback = false;
    NetworkReceiver::RenderPose matchedRenderPose = {};
    if (networkReceiver_->TakeRenderPoseForPresentationTimeUs(
        presentationTimeUs, &matchedRenderPose))
    {
        *renderPose = matchedRenderPose;
        renderPoseHitCount_++;
        return true;
    }

    renderPoseMissCount_++;
    if (clientReprojectionMode_ == protocol::ClientReprojectionMode::Off)
    {
        return false;
    }

    NetworkReceiver::RenderPose latestRenderPose = networkReceiver_->GetLatestRenderPose();
    if (!latestRenderPose.valid || latestRenderPose.presentationTimeUs <= 0)
    {
        return false;
    }
    if (latestRenderPose.presentationTimeUs > presentationTimeUs)
    {
        return false;
    }
    if (presentedVideoFrame_.hasRenderPose &&
        latestRenderPose.presentationTimeUs < presentedVideoFrame_.renderPose.presentationTimeUs)
    {
        return false;
    }

    const int64_t maxFallbackAgeUs =
        std::max<int64_t>((predictedDisplayPeriodNs_ / 1000) * 2, 50000);
    if (presentationTimeUs - latestRenderPose.presentationTimeUs > maxFallbackAgeUs)
    {
        return false;
    }

    *renderPose = latestRenderPose;
    *usedFallback = true;
    renderPoseFallbacksSinceLastReport_++;
    return true;
}

void XrApp::UpdateReprojectionWarp(bool reusingFrame)
{
    reprojectionWarpEnabled_ = false;
    reprojectionWarpOffsetX_ = 0.0f;
    reprojectionWarpOffsetY_ = 0.0f;

    if (clientReprojectionMode_ != protocol::ClientReprojectionMode::PoseWarp ||
        !reusingFrame ||
        !presentedVideoFrame_.valid ||
        !presentedVideoFrame_.hasRenderPose ||
        presentedVideoFrame_.consecutiveReuses > 3 ||
        presentedVideoFrame_.localReceiveTimeNs <= 0)
    {
        return;
    }

    const int64_t nowNs = SteadyClockNowNs();
    if (nowNs < presentedVideoFrame_.localReceiveTimeNs)
    {
        return;
    }
    const double ageMs =
        static_cast<double>(nowNs - presentedVideoFrame_.localReceiveTimeNs) / 1.0e6;
    if (ageMs > 75.0)
    {
        return;
    }
    const auto nowSteady = std::chrono::steady_clock::now();
    if (lastKeyframeRequestTime_.time_since_epoch().count() != 0 &&
        nowSteady - lastKeyframeRequestTime_ < std::chrono::milliseconds(300))
    {
        return;
    }

    XrPosef currentHeadPose = BuildCurrentHeadPose();
    XrPosef renderHeadPose = {};
    renderHeadPose.orientation = NormalizeQuaternion({
        presentedVideoFrame_.renderPose.orientation[0],
        presentedVideoFrame_.renderPose.orientation[1],
        presentedVideoFrame_.renderPose.orientation[2],
        presentedVideoFrame_.renderPose.orientation[3],
    });
    renderHeadPose.position = {
        presentedVideoFrame_.renderPose.position[0],
        presentedVideoFrame_.renderPose.position[1],
        presentedVideoFrame_.renderPose.position[2],
    };

    if (VectorDistance(currentHeadPose.position, renderHeadPose.position) > 0.08f)
    {
        return;
    }

    XrQuaternionf delta = MultiplyQuaternion(
        currentHeadPose.orientation, ConjugateQuaternion(renderHeadPose.orientation));
    if (delta.w < 0.0f)
    {
        delta = {-delta.x, -delta.y, -delta.z, -delta.w};
    }

    const float vectorLength =
        std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
    const float angle = 2.0f * std::atan2(vectorLength, std::max(delta.w, 0.0001f));
    if (!std::isfinite(angle) || angle > 0.12f)
    {
        return;
    }

    reprojectionWarpOffsetX_ = std::clamp(-delta.y * 0.07f, -0.012f, 0.012f);
    reprojectionWarpOffsetY_ = std::clamp(delta.x * 0.07f, -0.012f, 0.012f);
    if (std::abs(reprojectionWarpOffsetX_) < 0.0001f &&
        std::abs(reprojectionWarpOffsetY_) < 0.0001f)
    {
        return;
    }

    reprojectionWarpEnabled_ = true;
}

void XrApp::OnNalUnitReceived(const uint8_t* data, size_t size,
                              int64_t timestampNs, int64_t receiveTimeNs, uint8_t flags, uint8_t codec)
{
    nalUnitsReceived_++;
    const auto videoCodec = static_cast<protocol::VideoCodec>(codec);
    if (videoCodec != protocol::VideoCodec::H265 && videoCodec != protocol::VideoCodec::H264)
    {
        if (nalUnitsReceived_ <= 10)
        {
            LOGW("Dropping NAL unit with unsupported codec=%u (%s)",
                 codec, VideoCodecName(videoCodec));
        }
        return;
    }

    if (nalUnitsReceived_ <= 10 || nalUnitsReceived_ % 300 == 0)
    {
        const char* nalType = "unknown";
        if (size > 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1)
        {
            if (videoCodec == protocol::VideoCodec::H264)
            {
                uint8_t nalTypeId = data[4] & 0x1F;
                switch (nalTypeId)
                {
                    case 7: nalType = "SPS"; break;
                    case 8: nalType = "PPS"; break;
                    case 5: nalType = "IDR"; break;
                    case 1: nalType = "P-slice"; break;
                    default: nalType = "other"; break;
                }
            }
            else
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
        }
        LOGI("NAL unit #%u: size=%zu codec=%s type=%s ts=%lld",
             nalUnitsReceived_, size,
             VideoCodecName(videoCodec), nalType, (long long)timestampNs);
    }

    std::lock_guard<std::mutex> decoderLock(videoDecoderMutex_);
    if (videoDecoder_ && (!videoDecoder_->IsInitialized() || videoDecoder_->GetCodec() != videoCodec))
    {
        const uint32_t decoderWidth = decoderEncodedWidth_ > 0 ? decoderEncodedWidth_ : videoWidth_;
        const uint32_t decoderHeight = decoderEncodedHeight_ > 0 ? decoderEncodedHeight_ : videoHeight_;
        if (videoDecoder_->IsInitialized())
        {
            LOGW("Video codec changed from %u to %u; recreating decoder",
                 static_cast<uint32_t>(videoDecoder_->GetCodec()),
                 static_cast<uint32_t>(videoCodec));
        }
        if (!videoDecoder_->Initialize(decoderWidth, decoderHeight, videoCodec))
        {
            LOGE("Failed to initialize decoder for codec=%u (%s)",
                 codec, VideoCodecName(videoCodec));
            return;
        }
        activeVideoCodec_.store(videoCodec);
    }

    if (videoDecoder_ && videoDecoder_->IsInitialized())
    {
        const bool alphaBlend = (flags & protocol::VIDEO_FLAG_ALPHA_BLEND) != 0;
        bool submitted = videoDecoder_->SubmitNalUnit(
            data, size, timestampNs / 1000, receiveTimeNs, alphaBlend);
        if (!submitted && nalUnitsReceived_ <= 10)
        {
            LOGW("Failed to submit NAL unit #%u to decoder (no input buffer available)",
                 nalUnitsReceived_);
        }
    }
    else if (nalUnitsReceived_ <= 5)
    {
        LOGW("NAL unit received but decoder is unavailable");
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
    if (shellPendingNetworkReset_.exchange(false))
    {
        shellStatusText_ = "Resetting client";
        ResetConnection("local shell reset");
        StartNetworking();
    }

    RetryUsbAdbTransportIfNeeded();
    ApplyCompletedStreamConfigUpdate();

    if (connectionState_.load() == ConnectionState::Connected &&
        !hasVideoTexture_ &&
        connectionTime_ != std::chrono::steady_clock::time_point{})
    {
        auto noInitialVideoFor = std::chrono::steady_clock::now() - connectionTime_;
        if (std::chrono::duration_cast<std::chrono::seconds>(noInitialVideoFor).count() >= 5)
        {
            OnConnectionLost("no initial video frames for 5 seconds");
        }
    }

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
    XrCompositionLayerPassthroughFB passthroughCompositionLayer = {
        XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
    const XrCompositionLayerBaseHeader* layers[2] = {};
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

        protocol::TrackingPacket trackingPacket =
            BuildTrackingPacket(frameState.predictedDisplayTime);

        // Render to swapchains
        const bool renderedVideo = RenderFrame(frameState.predictedDisplayTime);
        bool submitPassthroughLayer = false;
        if (renderedVideo)
        {
            submitPassthroughLayer =
                serverMixedRealityPassthroughEnabled_ &&
                CanUseShellPassthrough() &&
                SetShellPassthroughActive(true);
            ReleaseShellForStream(submitPassthroughLayer);
        }
        else
        {
            submitPassthroughLayer =
                shellPassthroughMode_ &&
                CanUseShellPassthrough() &&
                SetShellPassthroughActive(true);
            if (!submitPassthroughLayer)
            {
                SetShellPassthroughActive(false);
            }
            UpdateShellInteractions();
        }

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

        if (submitPassthroughLayer)
        {
            passthroughCompositionLayer.layerHandle = passthroughLayer_;
            passthroughCompositionLayer.flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            passthroughCompositionLayer.space = XR_NULL_HANDLE;
            layers[layerCount++] =
                reinterpret_cast<const XrCompositionLayerBaseHeader*>(&passthroughCompositionLayer);
        }

        for (int eye = 0; eye < 2; eye++)
        {
            projectionViews[eye].pose = useRenderPose
                ? BuildEyePoseFromRenderPose(currentRenderPose_, eye)
                : views_[eye].pose;
            projectionViews[eye].fov = views_[eye].fov;
            projectionViews[eye].subImage.swapchain = swapchains_[eye];
            if (renderedVideo && blitWidth_ > 0)
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
        if (submitPassthroughLayer)
        {
            projectionLayer.layerFlags =
                XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
        }
        projectionLayer.viewCount = 2;
        projectionLayer.views = projectionViews;
        layers[layerCount++] =
            reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projectionLayer);

        // Send tracking data
        SendTracking(trackingPacket);
    }

    // xrEndFrame
    XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = frameState.predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = layerCount;
    endInfo.layers = (layerCount > 0) ? layers : nullptr;

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

bool XrApp::RenderFrame(XrTime predictedDisplayTime)
{
    // Try to get a decoded frame from the video decoder
    bool hasVideo = false;
    bool reusingPresentedFrame = false;
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
            bool usedRenderPoseFallback = false;
            const bool hasMatchedRenderPose = ResolveRenderPoseForFrame(
                frame.presentationTimeUs, &matchedRenderPose, &usedRenderPoseFallback);
            currentRenderPose_ = hasMatchedRenderPose ? matchedRenderPose : NetworkReceiver::RenderPose{};
            hasCurrentRenderPose_ = hasMatchedRenderPose;

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
                    LOGI("Render pose match: hit=%u miss=%u rate=%.1f%% fallbacks=%u",
                         renderPoseHitCount_, renderPoseMissCount_, hitRate,
                         renderPoseFallbacksSinceLastReport_);
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
                        presentedVideoFrame_.valid = true;
                        presentedVideoFrame_.texture = videoTexture_;
                        presentedVideoFrame_.presentationTimeUs = frame.presentationTimeUs;
                        presentedVideoFrame_.localReceiveTimeNs = frame.localReceiveTimeNs;
                        presentedVideoFrame_.localSubmitTimeNs = frame.localSubmitTimeNs;
                        presentedVideoFrame_.localAcquireTimeNs = frame.localAcquireTimeNs;
                        presentedVideoFrame_.renderPose = matchedRenderPose;
                        presentedVideoFrame_.hasRenderPose = hasMatchedRenderPose;
                        presentedVideoFrame_.alphaBlend = frame.alphaBlend;
                        if (frame.alphaBlend && !hasObservedProtocolAlphaFrame_)
                        {
                            LOGI("First alpha-blend video frame received");
                        }
                        hasObservedProtocolAlphaFrame_ =
                            hasObservedProtocolAlphaFrame_ || frame.alphaBlend;
                        presentedVideoFrame_.headsetPoseAtPresentation = BuildCurrentHeadPose();
                        presentedVideoFrame_.consecutiveReuses = 0;
                        (void)usedRenderPoseFallback;

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
                presentedVideoFrame_ = {};
                OnConnectionLost("no video frames for 2 seconds");
            }
            else
            {
                // Reuse the last frame — the AHardwareBuffer is still alive (not released).
                if (presentedVideoFrame_.valid)
                {
                    hasVideo = true;
                    reusingPresentedFrame = true;
                    presentedVideoFrame_.consecutiveReuses++;
                    staleFrameReusesSinceLastReport_++;

                    const int64_t nowNs = SteadyClockNowNs();
                    const double ageMs =
                        presentedVideoFrame_.localReceiveTimeNs > 0 && nowNs >= presentedVideoFrame_.localReceiveTimeNs
                            ? static_cast<double>(nowNs - presentedVideoFrame_.localReceiveTimeNs) / 1.0e6
                            : 0.0;
                    const bool canUseReprojectedPose =
                        clientReprojectionMode_ != protocol::ClientReprojectionMode::Off &&
                        presentedVideoFrame_.hasRenderPose &&
                        ageMs <= 120.0;
                    if (canUseReprojectedPose)
                    {
                        currentRenderPose_ = presentedVideoFrame_.renderPose;
                        hasCurrentRenderPose_ = true;
                        reprojectedFramesSinceLastReport_++;
                    }
                    else
                    {
                        currentRenderPose_ = {};
                        hasCurrentRenderPose_ = false;
                    }
                }
            }
        }
    }

    if (hasVideo && presentedVideoFrame_.valid && presentedVideoFrame_.localReceiveTimeNs > 0)
    {
        const int64_t nowNs = SteadyClockNowNs();
        if (nowNs >= presentedVideoFrame_.localReceiveTimeNs)
        {
            latencySamples_.frameAgeMs.push_back(
                static_cast<double>(nowNs - presentedVideoFrame_.localReceiveTimeNs) / 1.0e6);
        }
    }
    if (!hasVideo)
    {
        currentRenderPose_ = {};
        hasCurrentRenderPose_ = false;
        reusingPresentedFrame = false;
    }
    UpdateReprojectionWarp(reusingPresentedFrame && hasCurrentRenderPose_);
    if (!hasVideo)
    {
        UpdateShellPose();
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
            const quest_passthrough::AlphaKeyDecision alphaKey =
                EvaluatePassthroughAlphaKey();
            if (alphaKey.usingTransparentClearFallback &&
                !loggedTransparentClearFallback_)
            {
                LOGW("Passthrough stream has no alpha flags; using black-key fallback");
                loggedTransparentClearFallback_ = true;
            }

            // Clear full swapchain to black. In alpha passthrough frames the
            // letterbox/pillarbox bars should reveal the passthrough underlay.
            glViewport(0, 0, swapchainWidth_, swapchainHeight_);
            glClearColor(0.0f, 0.0f, 0.0f,
                         alphaKey.useBlackKeyAlpha ? 0.0f : 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            glDisable(GL_BLEND);

            // Blit video into aspect-ratio-correct viewport
            glViewport(blitOffsetX_, blitOffsetY_, blitWidth_, blitHeight_);
            BlitVideoToSwapchain(eye);
        }
        else if (hasVideo)
        {
            // Fallback before server discovery (blitWidth_ == 0): full swapchain
            glViewport(0, 0, swapchainWidth_, swapchainHeight_);
            glDisable(GL_BLEND);
            BlitVideoToSwapchain(eye);
        }
        else
        {
            // No video — render the local shell instead of a flat status color.
            glViewport(0, 0, swapchainWidth_, swapchainHeight_);
            const bool transparentShell =
                shellPassthroughMode_ && CanUseShellPassthrough();
            glClearColor(0.01f, 0.014f, 0.018f, transparentShell ? 0.0f : 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            RenderShellToSwapchain(eye, transparentShell);
        }

        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        // Release swapchain image
        XrSwapchainImageReleaseInfo releaseInfo = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(swapchains_[eye], &releaseInfo);
    }

    return hasVideo;
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

    glUseProgram(blitProgram_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, videoTexture_);
    glUniform2f(blitEyeSourceMinUniform_, uMin, contentVMin);
    glUniform2f(blitEyeSourceMaxUniform_, uMax, contentVMax);
    glUniform2f(blitLogicalTexelSizeUniform_, decodedTexelWidth_ * 2.0f, decodedTexelHeight_);
    glUniform1i(blitFoveatedEncodingEnabledUniform_, serverFoveatedEncodingEnabled_ ? 1 : 0);
    glUniform1i(blitClientUpscalingEnabledUniform_, clientUpscalingEnabled_ ? 1 : 0);
    glUniform2f(blitFoveationCenterSizeUniform_,
                foveationCenterSizeX_, foveationCenterSizeY_);
    glUniform2f(blitFoveationCenterShiftUniform_,
                foveationCenterShiftX_, foveationCenterShiftY_);
    glUniform2f(blitFoveationEdgeRatioUniform_,
                foveationEdgeRatioX_, foveationEdgeRatioY_);
    glUniform2f(blitFoveationEyeSizeRatioUniform_,
                foveationEyeWidthRatio_, foveationEyeHeightRatio_);
    glUniform1i(blitReprojectionWarpEnabledUniform_, reprojectionWarpEnabled_ ? 1 : 0);
    glUniform2f(blitReprojectionWarpOffsetUniform_,
                reprojectionWarpOffsetX_, reprojectionWarpOffsetY_);
    glUniform1i(blitPassthroughAlphaEnabledUniform_,
                EvaluatePassthroughAlphaKey().useBlackKeyAlpha ? 1 : 0);

    glBindVertexArray(blitVao_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    glUseProgram(0);
}

bool XrApp::CreateShellResources()
{
    if (shellProgram_ != 0 && shellVao_ != 0 && shellVbo_ != 0)
    {
        return true;
    }
    DestroyShellResources();

    shellProgram_ = CreateProgram(SHELL_VERTEX_SHADER, SHELL_FRAGMENT_SHADER);
    if (shellProgram_ == 0)
    {
        return false;
    }
    shellMvpUniform_ = glGetUniformLocation(shellProgram_, "uMvp");

    glGenVertexArrays(1, &shellVao_);
    glGenBuffers(1, &shellVbo_);
    glBindVertexArray(shellVao_);
    glBindBuffer(GL_ARRAY_BUFFER, shellVbo_);
    glBufferData(GL_ARRAY_BUFFER, 1, nullptr, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(ShellVertex),
                          reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(ShellVertex),
                          reinterpret_cast<void*>(3 * sizeof(float)));
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    LOGI("GL resources created for local shell");
    return true;
}

void XrApp::DestroyShellResources()
{
    if (shellProgram_ != 0)
    {
        glDeleteProgram(shellProgram_);
        shellProgram_ = 0;
    }
    if (shellVao_ != 0)
    {
        glDeleteVertexArrays(1, &shellVao_);
        shellVao_ = 0;
    }
    if (shellVbo_ != 0)
    {
        glDeleteBuffers(1, &shellVbo_);
        shellVbo_ = 0;
    }
    shellMvpUniform_ = -1;
}

void XrApp::ReleaseShellForStream(bool keepPassthrough)
{
    if (!keepPassthrough)
    {
        SetShellPassthroughActive(false);
    }
    if (shellProgram_ != 0 || shellVao_ != 0 || shellVbo_ != 0)
    {
        DestroyShellResources();
        LOGI("Local shell GL resources released while video stream is active");
    }

    shellPanelInitialized_ = false;
    shellHoveredButton_ = quest_shell::ButtonId::None;
    for (auto& controller : shellControllers_)
    {
        controller.clickState.pressed = true;
    }
    for (auto& hand : shellHands_)
    {
        hand.pinchClickState.pressed = true;
    }
}

void XrApp::RenderShellToSwapchain(int eye, bool transparentBackground)
{
    if (shellProgram_ == 0 || shellVao_ == 0 || shellVbo_ == 0)
    {
        if (!CreateShellResources())
        {
            LOGE("Failed to recreate local shell GL resources");
            return;
        }
    }

    std::vector<ShellVertex> triangles;
    std::vector<ShellVertex> lines;
    triangles.reserve(8192);
    lines.reserve(768);

    const ShellColor gridMajor = {0.30f, 0.42f, 0.48f, 0.55f};
    const ShellColor gridMinor = {0.18f, 0.24f, 0.27f, 0.42f};
    const ShellColor panel = {0.025f, 0.032f, 0.038f, transparentBackground ? 0.72f : 0.88f};
    const ShellColor panelEdge = {0.28f, 0.50f, 0.54f, 0.95f};
    const ShellColor text = {0.86f, 0.94f, 0.90f, 1.0f};
    const ShellColor dimText = {0.52f, 0.62f, 0.64f, 0.95f};
    const ShellColor button = {0.12f, 0.18f, 0.20f, 0.94f};
    const ShellColor buttonHover = {0.18f, 0.42f, 0.45f, 0.98f};
    const ShellColor buttonDisabled = {0.10f, 0.11f, 0.12f, 0.70f};

    if (!transparentBackground)
    {
        constexpr float gridHalfExtent = 4.0f;
        constexpr int gridSteps = 16;
        constexpr float gridSpacing = gridHalfExtent * 2.0f / static_cast<float>(gridSteps);
        for (int i = 0; i <= gridSteps; ++i)
        {
            const float offset = -gridHalfExtent + gridSpacing * static_cast<float>(i);
            const bool major = (i % 4) == 0;
            AppendLine(lines,
                       {offset, shellFloorY_, -gridHalfExtent},
                       {offset, shellFloorY_, gridHalfExtent},
                       major ? gridMajor : gridMinor);
            AppendLine(lines,
                       {-gridHalfExtent, shellFloorY_, offset},
                       {gridHalfExtent, shellFloorY_, offset},
                       major ? gridMajor : gridMinor);
        }
    }

    AppendPanelQuad(triangles, shellPanelLayout_, 0.0f, 0.0f,
                    shellPanelLayout_.width, shellPanelLayout_.height,
                    0.0f, panel);
    AppendLine(lines,
               PanelPoint(shellPanelLayout_, -shellPanelLayout_.width * 0.5f,
                          -shellPanelLayout_.height * 0.5f, 0.006f),
               PanelPoint(shellPanelLayout_, shellPanelLayout_.width * 0.5f,
                          -shellPanelLayout_.height * 0.5f, 0.006f),
               panelEdge);
    AppendLine(lines,
               PanelPoint(shellPanelLayout_, shellPanelLayout_.width * 0.5f,
                          -shellPanelLayout_.height * 0.5f, 0.006f),
               PanelPoint(shellPanelLayout_, shellPanelLayout_.width * 0.5f,
                          shellPanelLayout_.height * 0.5f, 0.006f),
               panelEdge);
    AppendLine(lines,
               PanelPoint(shellPanelLayout_, shellPanelLayout_.width * 0.5f,
                          shellPanelLayout_.height * 0.5f, 0.006f),
               PanelPoint(shellPanelLayout_, -shellPanelLayout_.width * 0.5f,
                          shellPanelLayout_.height * 0.5f, 0.006f),
               panelEdge);
    AppendLine(lines,
               PanelPoint(shellPanelLayout_, -shellPanelLayout_.width * 0.5f,
                          shellPanelLayout_.height * 0.5f, 0.006f),
               PanelPoint(shellPanelLayout_, -shellPanelLayout_.width * 0.5f,
                          -shellPanelLayout_.height * 0.5f, 0.006f),
               panelEdge);

    for (const quest_shell::ButtonRect& shellButton : shellPanelLayout_.buttons)
    {
        ShellColor fill = shellButton.enabled ? button : buttonDisabled;
        if (shellHoveredButton_ == shellButton.id && shellButton.enabled)
        {
            fill = buttonHover;
        }
        AppendPanelQuad(triangles, shellPanelLayout_,
                        shellButton.centerX, shellButton.centerY,
                        shellButton.width, shellButton.height,
                        0.003f, fill);
    }

    AppendPanelText(triangles, shellPanelLayout_, -0.335f, 0.160f, 0.0072f,
                    "OXRSYS CLIENT", text);
    AppendPanelText(triangles, shellPanelLayout_, -0.335f, 0.075f, 0.0062f,
                    UppercaseAscii(ShellStatusText(), 27), text);
    const char* modeText = shellPassthroughMode_ && CanUseShellPassthrough()
        ? "MODE PASSTHROUGH"
        : "MODE 3D GRID";
    AppendPanelText(triangles, shellPanelLayout_, -0.335f, 0.015f, 0.0055f,
                    modeText, dimText);
    AppendPanelTextCentered(triangles, shellPanelLayout_,
                            shellPanelLayout_.buttons[0].centerX,
                            shellPanelLayout_.buttons[0].centerY,
                            0.0055f, "RESET", text);
    const char* toggleText = CanUseShellPassthrough()
        ? (shellPassthroughMode_ ? "3D" : "PASSTHRU")
        : "NO PASS";
    AppendPanelTextCentered(triangles, shellPanelLayout_,
                            shellPanelLayout_.buttons[1].centerX,
                            shellPanelLayout_.buttons[1].centerY,
                            0.0055f, toggleText,
                            CanUseShellPassthrough() ? text : dimText);

    for (int hand = 0; hand < 2; ++hand)
    {
        const ShellColor handColor = hand == 0
            ? ShellColor{0.92f, 0.58f, 0.30f, 1.0f}
            : ShellColor{0.30f, 0.70f, 0.94f, 1.0f};
        if (shellControllers_[hand].active)
        {
            const quest_shell::Vec3 origin = shellControllers_[hand].ray.origin;
            const quest_shell::Vec3 end = AddVec3(
                origin, ScaleVec3(shellControllers_[hand].ray.direction, 1.45f));
            AppendLine(lines, origin, end, handColor);
            constexpr float marker = 0.025f;
            AppendLine(lines, AddVec3(origin, {-marker, 0.0f, 0.0f}),
                       AddVec3(origin, {marker, 0.0f, 0.0f}), handColor);
            AppendLine(lines, AddVec3(origin, {0.0f, -marker, 0.0f}),
                       AddVec3(origin, {0.0f, marker, 0.0f}), handColor);
            AppendLine(lines, AddVec3(origin, {0.0f, 0.0f, -marker}),
                       AddVec3(origin, {0.0f, 0.0f, marker}), handColor);
        }

        if (shellHands_[hand].active)
        {
            const quest_shell::Vec3 origin = shellHands_[hand].ray.origin;
            const quest_shell::Vec3 end = AddVec3(
                origin, ScaleVec3(shellHands_[hand].ray.direction, 1.20f));
            AppendLine(lines, origin, end, handColor);

            const ShellColor jointColor = {
                handColor.r * 0.80f + 0.20f,
                handColor.g * 0.80f + 0.20f,
                handColor.b * 0.80f + 0.20f,
                0.80f,
            };
            const ShellColor pinchColor = {1.0f, 0.92f, 0.22f, 0.95f};
            for (uint32_t joint = 0; joint < XR_HAND_JOINT_COUNT_EXT; ++joint)
            {
                if (!shellHands_[hand].jointActive[joint])
                {
                    continue;
                }
                const bool pinchJoint =
                    joint == XR_HAND_JOINT_INDEX_TIP_EXT ||
                    joint == XR_HAND_JOINT_THUMB_TIP_EXT;
                const bool pinching = shellHands_[hand].pinchValue >= 0.75f;
                AppendCube(triangles,
                           shellHands_[hand].joints[joint],
                           pinchJoint ? 0.016f : 0.011f,
                           pinchJoint && pinching ? pinchColor : jointColor);
            }
        }
    }

    Mat4 view = ViewFromPose(views_[eye].pose);
    Mat4 projection = ProjectionFromFov(views_[eye].fov);
    Mat4 mvp = MultiplyMat4(projection, view);

    glUseProgram(shellProgram_);
    glUniformMatrix4fv(shellMvpUniform_, 1, GL_FALSE, mvp.m);
    glBindVertexArray(shellVao_);
    glBindBuffer(GL_ARRAY_BUFFER, shellVbo_);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    if (!triangles.empty())
    {
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(triangles.size() * sizeof(ShellVertex)),
                     triangles.data(), GL_DYNAMIC_DRAW);
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(triangles.size()));
    }
    if (!lines.empty())
    {
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(lines.size() * sizeof(ShellVertex)),
                     lines.data(), GL_DYNAMIC_DRAW);
        glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(lines.size()));
    }

    glDisable(GL_BLEND);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
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

void XrApp::UpdateShellPose()
{
    if (!CanUseShellPassthrough())
    {
        shellPassthroughMode_ = false;
    }

    if (!shellPanelInitialized_)
    {
        const XrPosef& viewPose = views_[0].pose;
        XrQuaternionf orientation = NormalizeQuaternion(viewPose.orientation);
        XrVector3f forwardXr = RotateVector(orientation, {0.0f, 0.0f, -1.0f});

        quest_shell::Vec3 headCenter = {
            (views_[0].pose.position.x + views_[1].pose.position.x) * 0.5f,
            (views_[0].pose.position.y + views_[1].pose.position.y) * 0.5f,
            (views_[0].pose.position.z + views_[1].pose.position.z) * 0.5f,
        };
        quest_shell::Vec3 forward = ToShellVec3(forwardXr);
        forward.y = 0.0f;
        forward = NormalizeVec3(forward);
        if (DotVec3(forward, forward) < 0.5f)
        {
            forward = {0.0f, 0.0f, -1.0f};
        }
        quest_shell::Vec3 up = {0.0f, 1.0f, 0.0f};
        quest_shell::Vec3 right = NormalizeVec3(CrossVec3(forward, up));
        quest_shell::Vec3 normal = ScaleVec3(forward, -1.0f);
        quest_shell::Vec3 center = AddVec3(headCenter, ScaleVec3(forward, 1.0f));
        shellPanelLayout_ = quest_shell::MakeDefaultPanelLayout(
            center, right, up, normal, CanUseShellPassthrough());
        shellFloorY_ = appSpaceIsStage_ ? 0.0f : headCenter.y - 1.45f;
        shellPanelInitialized_ = true;
    }
    shellPanelLayout_.buttons[1].enabled = CanUseShellPassthrough();
}

void XrApp::UpdateShellInteractions()
{
    shellHoveredButton_ = quest_shell::ButtonId::None;

    auto handleClick = [this](quest_shell::ButtonId button) {
        switch (button)
        {
        case quest_shell::ButtonId::Reset:
            shellStatusText_ = "Reset requested";
            shellPendingNetworkReset_.store(true);
            break;
        case quest_shell::ButtonId::TogglePassthrough:
            if (CanUseShellPassthrough())
            {
                shellPassthroughMode_ = !shellPassthroughMode_;
                shellStatusText_ = shellPassthroughMode_
                    ? "Passthrough shell"
                    : "3D shell";
            }
            break;
        case quest_shell::ButtonId::None:
        default:
            break;
        }
    };

    for (int hand = 0; hand < 2; ++hand)
    {
        if (shellControllers_[hand].active)
        {
            quest_shell::ControllerResult result = quest_shell::UpdateController(
                shellPanelLayout_, shellControllers_[hand].ray,
                shellControllers_[hand].triggerValue,
                &shellControllers_[hand].clickState);
            if (result.hover.id != quest_shell::ButtonId::None)
            {
                shellHoveredButton_ = result.hover.id;
            }
            if (result.click != quest_shell::ButtonId::None)
            {
                handleClick(result.click);
            }
        }

        if (shellHands_[hand].active)
        {
            quest_shell::ControllerResult result = quest_shell::UpdateController(
                shellPanelLayout_,
                shellHands_[hand].ray,
                shellHands_[hand].pinchValue,
                &shellHands_[hand].pinchClickState,
                0.75f,
                0.25f);
            if (result.hover.id != quest_shell::ButtonId::None)
            {
                shellHoveredButton_ = result.hover.id;
            }
            if (result.click != quest_shell::ButtonId::None)
            {
                handleClick(result.click);
            }
        }
    }
}

bool XrApp::CanUseShellPassthrough() const
{
    return passthroughExtensionAvailable_ &&
           passthroughSupported_ &&
           passthrough_ != XR_NULL_HANDLE &&
           passthroughLayer_ != XR_NULL_HANDLE;
}

quest_passthrough::AlphaKeyDecision XrApp::EvaluatePassthroughAlphaKey() const
{
    return quest_passthrough::EvaluateAlphaKey({
        serverMixedRealityPassthroughEnabled_ && CanUseShellPassthrough(),
        presentedVideoFrame_.alphaBlend,
        hasObservedProtocolAlphaFrame_,
    });
}

bool XrApp::SetShellPassthroughActive(bool active)
{
    if (active)
    {
        if (!CanUseShellPassthrough())
        {
            return false;
        }
        if (!passthroughRunning_)
        {
            XrResult startResult = xrPassthroughStartFB_(passthrough_);
            if (XR_FAILED(startResult) &&
                startResult != XR_ERROR_UNEXPECTED_STATE_PASSTHROUGH_FB)
            {
                LOGW("xrPassthroughStartFB failed: %d", startResult);
                passthroughSupported_ = false;
                shellPassthroughMode_ = false;
                return false;
            }
            passthroughRunning_ = true;
        }
        if (!passthroughLayerRunning_)
        {
            XrResult resumeResult = xrPassthroughLayerResumeFB_(passthroughLayer_);
            if (XR_FAILED(resumeResult) &&
                resumeResult != XR_ERROR_UNEXPECTED_STATE_PASSTHROUGH_FB)
            {
                LOGW("xrPassthroughLayerResumeFB failed: %d", resumeResult);
                passthroughSupported_ = false;
                shellPassthroughMode_ = false;
                return false;
            }
            passthroughLayerRunning_ = true;
        }
        return true;
    }

    if (passthroughLayerRunning_ && xrPassthroughLayerPauseFB_ != nullptr &&
        passthroughLayer_ != XR_NULL_HANDLE)
    {
        (void)xrPassthroughLayerPauseFB_(passthroughLayer_);
        passthroughLayerRunning_ = false;
    }
    if (passthroughRunning_ && xrPassthroughPauseFB_ != nullptr &&
        passthrough_ != XR_NULL_HANDLE)
    {
        (void)xrPassthroughPauseFB_(passthrough_);
        passthroughRunning_ = false;
    }
    return false;
}

const char* XrApp::ShellStatusText() const
{
    if (!shellStatusText_.empty())
    {
        return shellStatusText_.c_str();
    }

    switch (connectionState_.load())
    {
    case ConnectionState::Connecting:
        return "Connecting";
    case ConnectionState::Connected:
        return "Waiting for video";
    case ConnectionState::Discovering:
        return "Waiting for server";
    case ConnectionState::Disconnected:
    default:
        return "Disconnected";
    }
}

protocol::TrackingPacket XrApp::BuildTrackingPacket(XrTime predictedDisplayTime)
{
    for (auto& controller : shellControllers_)
    {
        controller.active = false;
        controller.aimActive = false;
        controller.triggerValue = 0.0f;
        controller.ray = {};
    }
    for (auto& hand : shellHands_)
    {
        hand.active = false;
        hand.ray = {};
        hand.pinchValue = 0.0f;
        hand.jointActive.fill(false);
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
        static uint32_t handLocateLogCounter[2] = {0, 0};
        static bool loggedInactiveWithUsableJoints[2] = {false, false};
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
            const uint32_t validJointCount = CountValidHandJoints(jointLocations);
            const bool hasUsableJoints = HasUsableHandJoints(jointLocations);
            const bool runtimeActive = locations.isActive == XR_TRUE;
            const bool handActive = XR_SUCCEEDED(locateResult) && hasUsableJoints &&
                                    (runtimeActive || validJointCount >= 20);
            if (handActive && !runtimeActive && !loggedInactiveWithUsableJoints[hand])
            {
                LOGW("Hand tracking %s usable joints while runtime isActive=0 "
                     "(valid=%u missing=%s); sending joints as active",
                     hand == 0 ? "left" : "right",
                     validJointCount,
                     MissingCriticalHandJoints(jointLocations).c_str());
                loggedInactiveWithUsableJoints[hand] = true;
            }
            if (handActive != lastHandActive[hand] ||
                ++handLocateLogCounter[hand] % 270 == 1)
            {
                LOGI("Hand tracking %s %s locateResult=%d isActive=%d "
                     "validJoints=%u criticalOk=%d usable=%d missing=%s",
                     hand == 0 ? "left" : "right",
                     handActive ? "active" : "inactive",
                     locateResult,
                     runtimeActive ? 1 : 0,
                     validJointCount,
                     HasValidCriticalHandJoints(jointLocations) ? 1 : 0,
                     hasUsableJoints ? 1 : 0,
                     MissingCriticalHandJoints(jointLocations).c_str());
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
                if (HasValidJointPosition(jointLocations[i]))
                {
                    shellHands_[hand].jointActive[i] = true;
                    shellHands_[hand].joints[i] = {
                        jointLocations[i].pose.position.x,
                        jointLocations[i].pose.position.y,
                        jointLocations[i].pose.position.z,
                    };
                }
            }
            quest_shell::Vec3 indexTip = {
                jointLocations[XR_HAND_JOINT_INDEX_TIP_EXT].pose.position.x,
                jointLocations[XR_HAND_JOINT_INDEX_TIP_EXT].pose.position.y,
                jointLocations[XR_HAND_JOINT_INDEX_TIP_EXT].pose.position.z,
            };
            quest_shell::Vec3 thumbTip = {
                jointLocations[XR_HAND_JOINT_THUMB_TIP_EXT].pose.position.x,
                jointLocations[XR_HAND_JOINT_THUMB_TIP_EXT].pose.position.y,
                jointLocations[XR_HAND_JOINT_THUMB_TIP_EXT].pose.position.z,
            };
            quest_shell::Vec3 palm = {
                jointLocations[XR_HAND_JOINT_PALM_EXT].pose.position.x,
                jointLocations[XR_HAND_JOINT_PALM_EXT].pose.position.y,
                jointLocations[XR_HAND_JOINT_PALM_EXT].pose.position.z,
            };
            auto jointPosition = [&jointLocations](XrHandJointEXT joint) {
                return quest_shell::Vec3{
                    jointLocations[joint].pose.position.x,
                    jointLocations[joint].pose.position.y,
                    jointLocations[joint].pose.position.z,
                };
            };
            quest_shell::Vec3 rayDirection = {0.0f, 0.0f, -1.0f};
            bool rayDirectionValid = false;
            auto useDirectionToJoint = [&](XrHandJointEXT joint) {
                if (!HasValidJointPosition(jointLocations[joint]))
                {
                    return false;
                }
                const quest_shell::Vec3 candidate = jointPosition(joint);
                if (DistanceVec3(candidate, palm) < 0.015f)
                {
                    return false;
                }
                rayDirection = NormalizeVec3(SubVec3(candidate, palm));
                rayDirectionValid = true;
                return true;
            };
            (void)(useDirectionToJoint(XR_HAND_JOINT_MIDDLE_PROXIMAL_EXT) ||
                   useDirectionToJoint(XR_HAND_JOINT_MIDDLE_METACARPAL_EXT) ||
                   useDirectionToJoint(XR_HAND_JOINT_INDEX_METACARPAL_EXT));
            if (!rayDirectionValid && HasValidJointOrientation(jointLocations[XR_HAND_JOINT_PALM_EXT]))
            {
                const XrVector3f palmForward = RotateVector(
                    NormalizeQuaternion(jointLocations[XR_HAND_JOINT_PALM_EXT].pose.orientation),
                    {0.0f, 0.0f, -1.0f});
                rayDirection = NormalizeVec3(ToShellVec3(palmForward));
                rayDirectionValid = true;
            }
            if (!rayDirectionValid)
            {
                rayDirection = NormalizeVec3(SubVec3(indexTip, palm));
            }
            const float pinchDistance = DistanceVec3(indexTip, thumbTip);
            const float pinchValue = std::clamp((0.055f - pinchDistance) / 0.030f,
                                                0.0f, 1.0f);
            shellHands_[hand].indexTip = indexTip;
            shellHands_[hand].ray.origin = palm;
            shellHands_[hand].ray.direction = rayDirection;
            shellHands_[hand].pinchValue = pinchValue;
            shellHands_[hand].active = true;
        }
    }

    auto currentInteractionProfileName = [&](int hand) {
        XrInteractionProfileState profileState = {XR_TYPE_INTERACTION_PROFILE_STATE};
        XrResult profileResult =
            xrGetCurrentInteractionProfile(session_, handPaths_[hand], &profileState);
        if (XR_FAILED(profileResult))
        {
            return std::string("<profile-error:") + std::to_string(profileResult) + ">";
        }
        return PathToString(instance_, profileState.interactionProfile);
    };

    auto readPoseActionActive = [&](XrAction action, int hand, XrResult* resultOut) {
        if (action == XR_NULL_HANDLE)
        {
            if (resultOut != nullptr)
            {
                *resultOut = XR_ERROR_HANDLE_INVALID;
            }
            return false;
        }

        XrActionStateGetInfo getInfo = {XR_TYPE_ACTION_STATE_GET_INFO};
        getInfo.action = action;
        getInfo.subactionPath = handPaths_[hand];
        XrActionStatePose poseState = {XR_TYPE_ACTION_STATE_POSE};
        XrResult result = xrGetActionStatePose(session_, &getInfo, &poseState);
        if (resultOut != nullptr)
        {
            *resultOut = result;
        }
        return XR_SUCCEEDED(result) && poseState.isActive == XR_TRUE;
    };

    auto readFloatAction = [&](XrAction action, XrPath subactionPath, float* valueOut) {
        if (action == XR_NULL_HANDLE)
        {
            return false;
        }

        XrActionStateGetInfo getInfo = {XR_TYPE_ACTION_STATE_GET_INFO};
        getInfo.action = action;
        getInfo.subactionPath = subactionPath;
        XrActionStateFloat floatState = {XR_TYPE_ACTION_STATE_FLOAT};
        XrResult result = xrGetActionStateFloat(session_, &getInfo, &floatState);
        if (XR_SUCCEEDED(result) && floatState.isActive == XR_TRUE)
        {
            *valueOut = floatState.currentState;
            return true;
        }
        return false;
    };

    auto readVector2Action = [&](XrAction action, XrPath subactionPath, float* xOut, float* yOut) {
        if (action == XR_NULL_HANDLE)
        {
            return false;
        }

        XrActionStateGetInfo getInfo = {XR_TYPE_ACTION_STATE_GET_INFO};
        getInfo.action = action;
        getInfo.subactionPath = subactionPath;
        XrActionStateVector2f vec2State = {XR_TYPE_ACTION_STATE_VECTOR2F};
        XrResult result = xrGetActionStateVector2f(session_, &getInfo, &vec2State);
        if (XR_SUCCEEDED(result) && vec2State.isActive == XR_TRUE)
        {
            *xOut = vec2State.currentState.x;
            *yOut = vec2State.currentState.y;
            return true;
        }
        return false;
    };

    auto readBooleanAction = [&](XrAction action, XrPath subactionPath) {
        if (action == XR_NULL_HANDLE)
        {
            return false;
        }

        XrActionStateGetInfo getInfo = {XR_TYPE_ACTION_STATE_GET_INFO};
        getInfo.action = action;
        getInfo.subactionPath = subactionPath;
        XrActionStateBoolean boolState = {XR_TYPE_ACTION_STATE_BOOLEAN};
        XrResult result = xrGetActionStateBoolean(session_, &getInfo, &boolState);
        return XR_SUCCEEDED(result) &&
               boolState.isActive == XR_TRUE &&
               boolState.currentState == XR_TRUE;
    };

    // Controller poses
    if (gripSpaces_[0] != XR_NULL_HANDLE && gripSpaces_[1] != XR_NULL_HANDLE)
    {
        static bool lastControllerActive[2] = {false, false};
        static bool lastPoseActionActive[2] = {false, false};
        for (int hand = 0; hand < 2; hand++)
        {
            XrResult poseStateResult = XR_ERROR_HANDLE_INVALID;
            const bool poseActionActive =
                readPoseActionActive(gripPoseAction_, hand, &poseStateResult);
            XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
            XrResult locResult = xrLocateSpace(gripSpaces_[hand], appSpace_,
                                                predictedDisplayTime, &loc);
            XrResult aimPoseStateResult = XR_ERROR_HANDLE_INVALID;
            const bool aimPoseActionActive =
                readPoseActionActive(aimPoseAction_, hand, &aimPoseStateResult);
            XrSpaceLocation aimLoc = {XR_TYPE_SPACE_LOCATION};
            XrResult aimLocResult = XR_ERROR_HANDLE_INVALID;
            if (aimSpaces_[hand] != XR_NULL_HANDLE)
            {
                aimLocResult = xrLocateSpace(aimSpaces_[hand], appSpace_,
                                             predictedDisplayTime, &aimLoc);
            }

            const bool controllerActive =
                poseActionActive &&
                XR_SUCCEEDED(locResult) &&
                (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
                (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
            const bool aimActive =
                aimPoseActionActive &&
                XR_SUCCEEDED(aimLocResult) &&
                (aimLoc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
                (aimLoc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
            if (controllerActive)
            {
                packet.trackingFlags |= (hand == 0)
                    ? protocol::TRACKING_FLAG_LEFT_CONTROLLER_ACTIVE
                    : protocol::TRACKING_FLAG_RIGHT_CONTROLLER_ACTIVE;
            }
            if (controllerActive != lastControllerActive[hand] ||
                poseActionActive != lastPoseActionActive[hand])
            {
                const std::string profileName = currentInteractionProfileName(hand);
                LOGI("Controller %s %s profile=%s poseActive=%d poseResult=%d "
                     "locateResult=%d flags=0x%lx packetFlags=0x%x",
                     hand == 0 ? "left" : "right",
                     controllerActive ? "active" : "inactive",
                     profileName.c_str(),
                     poseActionActive ? 1 : 0,
                     poseStateResult,
                     locResult, (unsigned long)loc.locationFlags,
                     packet.trackingFlags);
                lastControllerActive[hand] = controllerActive;
                lastPoseActionActive[hand] = poseActionActive;
            }

            // Debug: log controller locate results periodically
            static uint32_t ctrlLogCounter = 0;
            if (hand == 0 && ++ctrlLogCounter % 270 == 1) // First time + every ~3s
            {
                const std::string profileName = currentInteractionProfileName(hand);
                LOGI("Controller locate L: profile=%s poseActive=%d poseResult=%d "
                     "locateResult=%d flags=0x%lx",
                     profileName.c_str(),
                     poseActionActive ? 1 : 0,
                     poseStateResult,
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
            if (controllerActive || aimActive)
            {
                shellControllers_[hand].active = true;
                shellControllers_[hand].aimActive = aimActive;
                shellControllers_[hand].gripPose = controllerActive ? loc.pose : aimLoc.pose;
                shellControllers_[hand].aimPose = aimActive ? aimLoc.pose : shellControllers_[hand].gripPose;
                const XrPosef& rayPose = shellControllers_[hand].aimPose;
                const XrVector3f rayForward = RotateVector(
                    NormalizeQuaternion(rayPose.orientation), {0.0f, 0.0f, -1.0f});
                shellControllers_[hand].ray.origin = ToShellVec3(rayPose.position);
                shellControllers_[hand].ray.direction = NormalizeVec3(ToShellVec3(rayForward));
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
    readFloatAction(triggerAction_, handPaths_[0], &packet.leftTrigger);
    readFloatAction(triggerAction_, handPaths_[1], &packet.rightTrigger);
    readFloatAction(gripAction_, handPaths_[0], &packet.leftGrip);
    readFloatAction(gripAction_, handPaths_[1], &packet.rightGrip);
    shellControllers_[0].triggerValue = packet.leftTrigger;
    shellControllers_[1].triggerValue = packet.rightTrigger;

    // Thumbsticks
    readVector2Action(thumbstickAction_, handPaths_[0],
                      &packet.leftThumbstick[0], &packet.leftThumbstick[1]);
    readVector2Action(thumbstickAction_, handPaths_[1],
                      &packet.rightThumbstick[0], &packet.rightThumbstick[1]);

    // Buttons
    uint32_t buttons = 0;
    if (readBooleanAction(aButtonAction_, handPaths_[0]))
    {
        buttons |= protocol::BUTTON_X;
    }
    if (readBooleanAction(aButtonAction_, handPaths_[1]))
    {
        buttons |= protocol::BUTTON_A;
    }
    if (readBooleanAction(bButtonAction_, handPaths_[0]))
    {
        buttons |= protocol::BUTTON_Y;
    }
    if (readBooleanAction(bButtonAction_, handPaths_[1]))
    {
        buttons |= protocol::BUTTON_B;
    }
    if (readBooleanAction(menuAction_, handPaths_[0]))
    {
        buttons |= protocol::BUTTON_MENU;
    }
    packet.buttonState = buttons;

    // Debug: log position periodically to verify 6DOF tracking
    static uint32_t trackingLogCounter = 0;
    if (++trackingLogCounter % 270 == 0) // ~3 seconds at 90fps
    {
        LOGI("Tracking: pos=(%.3f, %.3f, %.3f) rot=(%.3f, %.3f, %.3f, %.3f) "
             "L=(%.3f,%.3f,%.3f) R=(%.3f,%.3f,%.3f) "
             "trig=%.2f/%.2f grip=%.2f/%.2f stickL=(%.2f,%.2f) "
             "btn=0x%x flags=0x%x",
             packet.headPosition[0], packet.headPosition[1], packet.headPosition[2],
             packet.headOrientation[0], packet.headOrientation[1],
             packet.headOrientation[2], packet.headOrientation[3],
             packet.leftControllerPos[0], packet.leftControllerPos[1], packet.leftControllerPos[2],
             packet.rightControllerPos[0], packet.rightControllerPos[1], packet.rightControllerPos[2],
             packet.leftTrigger, packet.rightTrigger,
             packet.leftGrip, packet.rightGrip,
             packet.leftThumbstick[0], packet.leftThumbstick[1],
             packet.buttonState, packet.trackingFlags);
    }

    return packet;
}

void XrApp::SendTracking(const protocol::TrackingPacket& packet)
{
    if (!trackingSender_ || !trackingSender_->IsConnected())
    {
        return;
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

            if (!InitializeHandTracking())
            {
                LOGW("Hand tracking unavailable for this session, continuing without it");
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
        SetShellPassthroughActive(false);
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
    ShutdownPassthrough();
    ShutdownHandTracking();
    ShutdownFoveation();

    // Destroy GL resources
    if (blitProgram_ != 0)
    {
        glDeleteProgram(blitProgram_);
        blitProgram_ = 0;
        blitTextureUniform_ = -1;
        blitEyeSourceMinUniform_ = -1;
        blitEyeSourceMaxUniform_ = -1;
        blitLogicalTexelSizeUniform_ = -1;
        blitFoveatedEncodingEnabledUniform_ = -1;
        blitClientUpscalingEnabledUniform_ = -1;
        blitUpscaleEdgeThresholdUniform_ = -1;
        blitUpscaleSharpnessUniform_ = -1;
        blitFoveationCenterSizeUniform_ = -1;
        blitFoveationCenterShiftUniform_ = -1;
        blitFoveationEdgeRatioUniform_ = -1;
        blitFoveationEyeSizeRatioUniform_ = -1;
        blitReprojectionWarpEnabledUniform_ = -1;
        blitReprojectionWarpOffsetUniform_ = -1;
        blitPassthroughAlphaEnabledUniform_ = -1;
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
    DestroyShellResources();
    shellMvpUniform_ = -1;
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
    for (int hand = 0; hand < 2; ++hand)
    {
        if (aimSpaces_[hand] != XR_NULL_HANDLE)
        {
            xrDestroySpace(aimSpaces_[hand]);
            aimSpaces_[hand] = XR_NULL_HANDLE;
        }
        if (gripSpaces_[hand] != XR_NULL_HANDLE)
        {
            xrDestroySpace(gripSpaces_[hand]);
            gripSpaces_[hand] = XR_NULL_HANDLE;
        }
    }
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
