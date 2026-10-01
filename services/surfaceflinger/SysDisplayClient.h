/*
 * Copyright 2026 The Picomisu Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <stdint.h>

#include <vector>

#include <gui/SurfaceClient.h>
#include <utils/Timers.h>

namespace android {

// PICO: per-display history of the producer frames that were composed (factory
// android::SysDisplayClient, embedded in DisplayDevice at +0x118, 0x1520 bytes).
//
// doComposeSurfaces() records, for every visible layer, the producer frame that the layer
// latched (SurfaceClient::findCurrentFrame(latch slot)). After the client target of a display
// has been queued, handleMessageRefresh() hands the frames recorded since the previous
// composition, together with the composition times, to the PICO system monitor
// (mtp::SysMtpClient::addDisplayFrame) and starts a new history.
class SysDisplayClient {
public:
    static constexpr size_t kCapacity = 32;

    // Factory 0x10e2d8: copies the frame of `slot` (if the client knows it) into the history.
    void updateSurfaceFrame(SurfaceClient* client, int slot);
    // Factory 0x10e36c: returns the recorded frames and clears the history.
    std::vector<SurfaceClientItem> getCurrentSurfaceBuffer();

    // Start of the previous and of the current composition of the display
    // (systemTime(SYSTEM_TIME_MONOTONIC), set by handleMessageRefresh; factory +0x1510/+0x1518).
    nsecs_t mLastComposeTime = 0;
    nsecs_t mComposeTime = 0;

private:
    SurfaceClientItem mItems[kCapacity];
    uint64_t mIndex = UINT64_MAX; // last written entry
    size_t mCount = 0;
};

} // namespace android
