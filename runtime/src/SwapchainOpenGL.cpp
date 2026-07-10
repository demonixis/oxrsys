// SPDX-License-Identifier: MPL-2.0

#include "Swapchain.h"
#include "Runtime.h"

#include <spdlog/spdlog.h>

#ifdef XR_USE_GRAPHICS_API_OPENGL
#include <GL/gl.h>

#include <cstring>

#include "OpenXRPlatform.h"

namespace
{

constexpr uint32_t kOpenGLReadbackSlotsPerLayer = 2;

bool IsOpenGLDepthFormat(int64_t format)
{
    return format == GL_DEPTH_COMPONENT16 ||
           format == GL_DEPTH_COMPONENT24 ||
           format == GL_DEPTH_COMPONENT32F ||
           format == GL_DEPTH24_STENCIL8 ||
           format == GL_DEPTH32F_STENCIL8;
}

GLenum OpenGLReadFormatForInternalFormat(int64_t format)
{
    (void)format;
    return GL_RGBA;
}

} // namespace

void Swapchain::InitOpenGL(const OpenGLGraphicsContext& openGLContext,
                           const XrSwapchainCreateInfo* createInfo)
{
    if (createInfo == nullptr)
    {
        initializationResult_ = XR_ERROR_VALIDATION_FAILURE;
        Runtime::Get().RegisterHandle(handle_, this);
        return;
    }

    width_ = createInfo->width;
    height_ = createInfo->height;
    format_ = createInfo->format;
    arraySize_ = createInfo->arraySize > 0 ? createInfo->arraySize : 1;
    imageCount_ = (createInfo->createFlags & XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) != 0
        ? 1
        : SwapchainImageCount;
    graphicsApi_ = GraphicsApi::OpenGL;
    openGLContext_ = openGLContext;

    if (openGLContext_.display == nullptr || openGLContext_.context == nullptr)
    {
        initializationResult_ = XR_ERROR_GRAPHICS_DEVICE_INVALID;
        Runtime::Get().RegisterHandle(handle_, this);
        spdlog::error("OXRSys: missing OpenGL GLX context for swapchain creation");
        return;
    }

    glTextures_.resize(imageCount_, 0);
    textures_.resize(imageCount_, nullptr);
    imageStates_.assign(imageCount_, ImageState::Available);
    glReadbackSlots_.resize(static_cast<size_t>(arraySize_) * kOpenGLReadbackSlotsPerLayer);
    glReadbackNextSlotByLayer_.assign(arraySize_, 0);
    lastOpenGLSnapshots_.assign(arraySize_, {});

    const GLenum target = arraySize_ > 1 ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D;
    const bool depth = IsOpenGLDepthFormat(format_);
    const GLint internalFormat = static_cast<GLint>(format_);
    const GLenum uploadFormat = depth ? GL_DEPTH_COMPONENT : GL_RGBA;
    const GLenum uploadType = depth ? GL_FLOAT : GL_UNSIGNED_BYTE;

    glGenTextures(static_cast<GLsizei>(imageCount_), glTextures_.data());
    for (uint32_t i = 0; i < imageCount_; ++i)
    {
        glBindTexture(target, glTextures_[i]);
        glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        if (arraySize_ > 1)
        {
            glTexImage3D(target, 0, internalFormat, static_cast<GLsizei>(width_),
                         static_cast<GLsizei>(height_), static_cast<GLsizei>(arraySize_),
                         0, uploadFormat, uploadType, nullptr);
        }
        else
        {
            glTexImage2D(target, 0, internalFormat, static_cast<GLsizei>(width_),
                         static_cast<GLsizei>(height_), 0, uploadFormat, uploadType, nullptr);
        }
        textures_[i] = reinterpret_cast<void*>(static_cast<uintptr_t>(glTextures_[i]));
    }
    glBindTexture(target, 0);

    GLenum error = glGetError();
    if (error != GL_NO_ERROR)
    {
        initializationResult_ = XR_ERROR_RUNTIME_FAILURE;
        spdlog::error("OXRSys: OpenGL swapchain texture creation failed: 0x{:x}",
                      static_cast<unsigned>(error));
    }

    Runtime::Get().RegisterHandle(handle_, this);
    spdlog::info("OXRSys: OpenGL swapchain created {}x{} format={} arraySize={} images={}",
                 width_, height_, format_, arraySize_, imageCount_);
}

XrResult Swapchain::EnumerateOpenGLImages(uint32_t /*imageCapacityInput*/,
                                          XrSwapchainImageBaseHeader* images) const
{
    auto* openGLImages = reinterpret_cast<XrSwapchainImageOpenGLKHR*>(images);
    for (uint32_t i = 0; i < imageCount_; i++)
    {
        openGLImages[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
        openGLImages[i].next = nullptr;
        openGLImages[i].image = glTextures_[i];
    }
    return XR_SUCCESS;
}

void Swapchain::DestroyOpenGLResources()
{
    for (OpenGLReadbackSlot& slot : glReadbackSlots_)
    {
        if (slot.fence != nullptr)
        {
            glDeleteSync(slot.fence);
            slot.fence = nullptr;
        }
        if (slot.pbo != 0)
        {
            glDeleteBuffers(1, &slot.pbo);
            slot.pbo = 0;
        }
    }
    if (!glTextures_.empty())
    {
        glDeleteTextures(static_cast<GLsizei>(glTextures_.size()), glTextures_.data());
    }
}

void Swapchain::ResolveReadyOpenGLSnapshots()
{
    for (OpenGLReadbackSlot& slot : glReadbackSlots_)
    {
        if (!slot.pending || slot.fence == nullptr)
        {
            continue;
        }

        GLenum waitResult = glClientWaitSync(slot.fence, 0, 0);
        if (waitResult == GL_TIMEOUT_EXPIRED)
        {
            continue;
        }

        glDeleteSync(slot.fence);
        slot.fence = nullptr;

        if (waitResult == GL_WAIT_FAILED)
        {
            slot.pending = false;
            spdlog::warn("OXRSys: OpenGL readback fence wait failed");
            continue;
        }

        glBindBuffer(GL_PIXEL_PACK_BUFFER, slot.pbo);
        void* mapped = glMapBufferRange(
            GL_PIXEL_PACK_BUFFER, 0, static_cast<GLsizeiptr>(slot.size), GL_MAP_READ_BIT);
        if (mapped != nullptr)
        {
            auto snapshot = std::make_shared<OpenGLFrameSource>();
            snapshot->width = slot.width;
            snapshot->height = slot.height;
            snapshot->format = slot.format;
            snapshot->pixels.resize(slot.size);
            std::memcpy(snapshot->pixels.data(), mapped, slot.size);
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
            if (slot.arrayIndex < lastOpenGLSnapshots_.size())
            {
                lastOpenGLSnapshots_[slot.arrayIndex] = std::move(snapshot);
            }
        }
        else
        {
            spdlog::warn("OXRSys: OpenGL readback PBO map failed");
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        slot.pending = false;
    }
}

void Swapchain::SnapshotOpenGLReleasedImage(uint32_t releasedIndex)
{
    if (releasedIndex >= glTextures_.size() || IsOpenGLDepthFormat(format_))
    {
        for (auto& snapshot : lastOpenGLSnapshots_)
        {
            snapshot.reset();
        }
        return;
    }

    ResolveReadyOpenGLSnapshots();

    GLuint framebuffer = 0;
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);

    const GLenum readFormat = OpenGLReadFormatForInternalFormat(format_);
    const size_t readbackSize = static_cast<size_t>(width_) * height_ * 4u;
    for (uint32_t arrayIndex = 0; arrayIndex < arraySize_; ++arrayIndex)
    {
        const size_t layerSlotOffset =
            static_cast<size_t>(arrayIndex) * kOpenGLReadbackSlotsPerLayer;
        uint32_t selectedSlot = kOpenGLReadbackSlotsPerLayer;
        for (uint32_t slotOffset = 0; slotOffset < kOpenGLReadbackSlotsPerLayer; ++slotOffset)
        {
            const uint32_t candidate =
                (glReadbackNextSlotByLayer_[arrayIndex] + slotOffset) %
                kOpenGLReadbackSlotsPerLayer;
            OpenGLReadbackSlot& slot = glReadbackSlots_[layerSlotOffset + candidate];
            if (!slot.pending)
            {
                selectedSlot = candidate;
                break;
            }
        }

        if (selectedSlot == kOpenGLReadbackSlotsPerLayer)
        {
            spdlog::debug("OXRSys: OpenGL readback slots are busy for layer {}; dropping snapshot",
                          arrayIndex);
            continue;
        }

        OpenGLReadbackSlot& slot = glReadbackSlots_[layerSlotOffset + selectedSlot];
        if (slot.pbo == 0)
        {
            glGenBuffers(1, &slot.pbo);
        }

        if (arraySize_ > 1)
        {
            glFramebufferTextureLayer(
                GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, glTextures_[releasedIndex], 0,
                static_cast<GLint>(arrayIndex));
        }
        else
        {
            glFramebufferTexture2D(
                GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                glTextures_[releasedIndex], 0);
        }
        glReadBuffer(GL_COLOR_ATTACHMENT0);

        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        {
            spdlog::warn("OXRSys: OpenGL snapshot framebuffer is incomplete");
            continue;
        }

        glBindBuffer(GL_PIXEL_PACK_BUFFER, slot.pbo);
        glBufferData(
            GL_PIXEL_PACK_BUFFER, static_cast<GLsizeiptr>(readbackSize), nullptr, GL_STREAM_READ);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, static_cast<GLsizei>(width_), static_cast<GLsizei>(height_),
                     readFormat, GL_UNSIGNED_BYTE, nullptr);
        slot.fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        glFlush();
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

        if (slot.fence == nullptr)
        {
            spdlog::warn("OXRSys: OpenGL readback fence creation failed");
            continue;
        }

        slot.pending = true;
        slot.size = readbackSize;
        slot.arrayIndex = arrayIndex;
        slot.width = width_;
        slot.height = height_;
        slot.format = readFormat;
        glReadbackNextSlotByLayer_[arrayIndex] =
            (selectedSlot + 1) % kOpenGLReadbackSlotsPerLayer;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &framebuffer);
}

FrameImageSource Swapchain::SnapshotOpenGLFrameImageSource(uint32_t arrayIndex) const
{
    if (arrayIndex >= lastOpenGLSnapshots_.size() || !lastOpenGLSnapshots_[arrayIndex])
    {
        return {};
    }
    const std::shared_ptr<OpenGLFrameSource>& snapshot = lastOpenGLSnapshots_[arrayIndex];
    FrameImageSource source = {};
    source.api = GraphicsApi::OpenGL;
    source.image = snapshot;
    source.sourceWidth = snapshot->width;
    source.sourceHeight = snapshot->height;
    source.sourceFormat = snapshot->format;
    source.imageWidth = snapshot->width;
    source.imageHeight = snapshot->height;
    return source;
}

#endif // XR_USE_GRAPHICS_API_OPENGL
