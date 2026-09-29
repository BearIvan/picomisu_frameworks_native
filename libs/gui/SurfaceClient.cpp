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

#include <gui/SurfaceClient.h>

#include <algorithm>
#include <cstring>
#include <binder/IPCThreadState.h>

namespace android {

SurfaceClient::SurfaceClient() : mCurrentIndex(UINT64_MAX), mFrameCount(0), mCallingTid(0) {}
SurfaceClient::~SurfaceClient() = default;

void SurfaceClient::setFrameItem(int index) {
    if (mCallingTid == 0) {
        mCallingTid = IPCThreadState::self()->getCallingTid();
    }
    const nsecs_t timestamp = systemTime(SYSTEM_TIME_MONOTONIC);
    if (static_cast<unsigned int>(index) < 4) {
        mTimestamps[index] = timestamp;
    }
}

void SurfaceClient::addFrame(String8 name, int frameNumber) {
    Mutex::Autolock lock(mMutex);
    mCurrentIndex = (mCurrentIndex + 1) % 120;
    if (mFrameCount < 120) ++mFrameCount;
    Frame& frame = mFrames[mCurrentIndex];
    std::memset(frame.name, 0, sizeof(frame.name));
    std::strncpy(frame.name, name.string(), sizeof(frame.name) - 1);
    frame.callingTid = mCallingTid;
    frame.frameNumber = frameNumber;
    for (size_t index = 0; index < 4; ++index) {
        frame.timestamps[index] = mTimestamps[index];
    }
}

SurfaceClient::Frame* SurfaceClient::findCurrentFrame(int frameNumber) {
    const size_t limit = std::min(mFrameCount, size_t(8));
    for (size_t distance = 0; distance < limit; ++distance) {
        const size_t index = (mFrameCount + mCurrentIndex - distance) % mFrameCount;
        if (mFrames[index].frameNumber == frameNumber) return &mFrames[index];
    }
    return nullptr;
}

} // namespace android
