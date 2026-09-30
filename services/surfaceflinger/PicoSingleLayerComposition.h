// Copyright 2026 Picomisu contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <compositionengine/RenderSurface.h>
#include <renderengine/LayerSettings.h>
#include <vector>
namespace android {
enum class PicoSingleLayerAction { Render, Attached, SkipFrame, RenderAndTrack };
PicoSingleLayerAction preparePicoSingleLayerComposition(
        compositionengine::RenderSurface& surface, bool enabled, bool& renderFallback,
        const std::vector<renderengine::LayerSettings>& settings, const sp<Layer>& owner,
        base::unique_fd* readyFence, const char* displayName = "");
void finishPicoSingleLayerRender(compositionengine::RenderSurface& surface, const sp<Layer>& owner,
                                const sp<GraphicBuffer>& buffer, const sp<Fence>& acquireFence,
                                base::unique_fd* readyFence);
}
