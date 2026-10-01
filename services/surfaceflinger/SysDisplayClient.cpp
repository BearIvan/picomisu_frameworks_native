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

#include "SysDisplayClient.h"

namespace android {

void SysDisplayClient::updateSurfaceFrame(SurfaceClient* client, int slot) {
    if (client == nullptr) return;
    const SurfaceClientItem* frame = client->findCurrentFrame(slot);
    if (frame == nullptr) return;
    mIndex = (mIndex + 1) % kCapacity;
    if (mCount < kCapacity) ++mCount;
    mItems[mIndex] = *frame;
}

std::vector<SurfaceClientItem> SysDisplayClient::getCurrentSurfaceBuffer() {
    std::vector<SurfaceClientItem> frames;
    // As in the factory, the entries are returned in storage order.
    const int count = static_cast<int>(mCount);
    for (int i = 0; i < count; ++i) {
        frames.push_back(mItems[(mCount + i) % mCount]);
    }
    mIndex = UINT64_MAX;
    mCount = 0;
    return frames;
}

} // namespace android
