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
#include <sys/types.h>
#include <utils/Mutex.h>
#include <utils/String8.h>
#include <utils/Timers.h>

namespace android {

// PICO: one recorded producer frame (factory android::SurfaceClientItem, 0xa8 bytes). The
// compositor copies it into its per-display history (SysDisplayClient) and hands a vector of
// them to the PICO system monitor (mtp::SysMtpClient::addDisplayFrame).
struct SurfaceClientItem {
    char name[128];
    pid_t callingTid;
    int32_t frameNumber;
    nsecs_t timestamps[4];
};

// PICO client-side frame history consumed by the factory compositor.
class SurfaceClient {
public:
    using Frame = SurfaceClientItem;

    SurfaceClient();
    ~SurfaceClient();
    void setFrameItem(int index);
    void addFrame(String8 name, int frameNumber);
    Frame* findCurrentFrame(int frameNumber);

private:
    SurfaceClient(const SurfaceClient&) = delete;
    SurfaceClient& operator=(const SurfaceClient&) = delete;

    Frame mFrames[120];
    uint64_t mCurrentIndex;
    size_t mFrameCount;
    // As in PICO, callers set the four stages before recording a frame.
    nsecs_t mTimestamps[4];
    Mutex mMutex;
    pid_t mCallingTid;
};

} // namespace android
