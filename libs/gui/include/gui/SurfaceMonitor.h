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

#include <array>
#include <cstddef>
#include <cstdint>
#include <binder/IBinder.h>
#include <utils/Mutex.h>
#include <utils/String8.h>
#include <utils/Timers.h>

namespace android {

enum class MonitorIndex : int32_t {
    Begin = 0,
    End = 1,
    RenderBegin = 2,
    RenderEnd = 3,
};

struct MonitorItem {
    int32_t state;
    nsecs_t timestamps[5];
    MonitorItem() : state(0) {}
};

// Fixed-size history of the monitor (factory android::RingBuffer<T, N>; the constructor of
// RingBuffer<MonitorItem, 120> is out of line in the factory libgui, 0xd66ec).
template <typename T, size_t Capacity>
struct RingBuffer {
    T items[Capacity];
    uint64_t index;
    size_t count;
    RingBuffer() : index(UINT64_MAX), count(0) {}
    void clear() { index = UINT64_MAX; count = 0; }
    void add(const T& item) {
        index = (index + 1) % Capacity;
        if (count < Capacity) ++count;
        items[index] = item;
    }
    T& logical(size_t ordinal) { return items[(count + ordinal) % count]; }
};

class SurfaceMonitor {
public:
    SurfaceMonitor();
    ~SurfaceMonitor();
    void setFrameItem(MonitorIndex index);
    void addFrame(String8 name);
    void updateCurrentDisplayFps(int index, String8 name);
    void loadConfig();
    void initParameter();
    void getProcComm(int pid, char* name);
    void scanOperationArea(const String8& name);
    void doAnalysis(size_t first, size_t last, const String8& name);
    void clear();
    void reportAlarm(size_t first, size_t last, ssize_t score, nsecs_t maximumDelay,
                     const String8& name, int type);
    void reportFps(double fps, const String8& name, nsecs_t duration);
    bool quickConfirmFps(double* frameFps, double average, int count);
    void confirmCurrentFps();
    void requestChangeDisplayFps(int index);

private:
    SurfaceMonitor(const SurfaceMonitor&) = delete;
    SurfaceMonitor& operator=(const SurfaceMonitor&) = delete;

    [[maybe_unused]] int32_t mLegacyWord;
    // The meaning of these factory bytes is unresolved. None of the inspected
    // monitor routines accesses them. Preserve their observed position rather
    // than assigning them invented timing/state semantics. Full ABI remains open.
    [[maybe_unused]] alignas(8) std::array<std::byte, 40> mUnresolvedFactoryState;
    int32_t mProcessState;
    pid_t mCallingPid;
    sp<IBinder> mTransfer;
    sp<IBinder> mSysTrans;
    RingBuffer<MonitorItem, 120> mFrames;
    RingBuffer<double, 10> mAverageFps;
    RingBuffer<nsecs_t, 60> mFrameDurations;
    nsecs_t mPendingTimes[5];
    nsecs_t mStandardPeriod;
    nsecs_t mPeriods[8];
    int32_t mCurrentMode;
    int32_t mModeCount;
    float mModeFps[8];
    Mutex mMutex;
    int32_t mParameters[10];
    float mParameterLimit;
    bool mUpdating;
};

} // namespace android
