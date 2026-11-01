// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "GraphicsTypes.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

namespace oxrsys::video
{

// Draws quad and cylinder composition layers into one eye's projection image
// on the encoder's command buffer, back to front, alpha-blended per the layer
// flags. The geometry (clip <- layer matrices) is computed by the Session from
// the projection view's own pose and fov; see CompositionOverlay.h.
class OverlayCompositor
{
public:
    OverlayCompositor() = default;
    ~OverlayCompositor();
    OverlayCompositor(const OverlayCompositor&) = delete;
    OverlayCompositor& operator=(const OverlayCompositor&) = delete;

    // commandBuffer: id<MTLCommandBuffer>; target: id<MTLTexture> with
    // RenderTarget usage, whose contents are kept (loaded) under the overlays.
    // Returns how many overlays were drawn, or -1 if the pass could not be set up.
    int Draw(void* commandBuffer, void* target, uint32_t eyeIndex,
             const std::vector<FrameOverlayLayer>& overlays);

    static bool AnyVisibleTo(const std::vector<FrameOverlayLayer>& overlays, uint32_t eyeIndex)
    {
        for (const auto& overlay : overlays)
        {
            if ((overlay.eyeMask & (1u << eyeIndex)) != 0)
            {
                return true;
            }
        }
        return false;
    }

private:
    void* PipelineFor(void* device, uint64_t pixelFormat);

    std::mutex mutex_;
    void* device_ = nullptr;   // id<MTLDevice>, not retained (owned by the encoder)
    void* library_ = nullptr;  // id<MTLLibrary>
    void* sampler_ = nullptr;  // id<MTLSamplerState>
    std::map<uint64_t, void*> pipelines_; // MTLPixelFormat -> id<MTLRenderPipelineState>
    bool failed_ = false;
};

} // namespace oxrsys::video
