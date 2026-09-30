// Copyright 2026 Picomisu contributors
// SPDX-License-Identifier: Apache-2.0
#include "PicoSingleLayerComposition.h"
#include "Layer.h"
#include <log/log.h>
#include <ui/GraphicBuffer.h>
namespace android {
void finishPicoSingleLayerRender(compositionengine::RenderSurface& surface, const sp<Layer>& owner,
                                const sp<GraphicBuffer>& buffer, const sp<Fence>& acquireFence,
                                base::unique_fd* readyFence) {
    surface.setMultiLayerFlag(false);
    surface.setSingleLayer(owner, buffer);
    readyFence->reset(acquireFence ? acquireFence->dup() : -1);
}
PicoSingleLayerAction preparePicoSingleLayerComposition(
        compositionengine::RenderSurface& surface, bool enabled, bool& renderFallback,
        const std::vector<renderengine::LayerSettings>& settings, const sp<Layer>& owner,
        base::unique_fd* readyFence, const char* displayName) {
    if (!enabled || renderFallback || settings.size() != 1) return PicoSingleLayerAction::Render;
    sp<GraphicBuffer> buffer = settings.front().source.buffer.buffer;
    if (!buffer) return PicoSingleLayerAction::Render;
    // The producer marks buffers whose acquire fence timed out (0x4: waited, 0x8: fatal)
    // in usage bits 32..35; either one switches this display to GPU composition for good.
    constexpr uint64_t mask = 0xf00000000ULL;
    const uint64_t usage = buffer->usage & mask;
    renderFallback = usage == 0x400000000ULL || usage == 0x800000000ULL;
    buffer->usage &= ~mask;
    if (renderFallback) {
        ALOGE("SF single layer buffer wait fence fatal timeout! switch to normal flow. "
              "is_fatal %d is_wait %d %s",
              1, usage == 0x400000000ULL, displayName);
        return PicoSingleLayerAction::Render;
    }
    if (surface.getMultiLayerFlag()) return PicoSingleLayerAction::Render;
    const status_t status = surface.attachBuffer(buffer);
    if (status == NO_ERROR) {
        finishPicoSingleLayerRender(surface, owner, buffer, settings.front().source.buffer.fence, readyFence);
        return PicoSingleLayerAction::Attached;
    }
    if (status == ALREADY_EXISTS) return PicoSingleLayerAction::SkipFrame;
    if (status != NO_INIT) renderFallback = true;
    ALOGD("SF single layer buffer attach fail! switch to normal flow. res %d is_fatal:%d %s",
          status, renderFallback, displayName);
    return PicoSingleLayerAction::RenderAndTrack;
}
}
