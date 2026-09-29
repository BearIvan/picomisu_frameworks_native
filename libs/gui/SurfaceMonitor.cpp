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
#include <gui/SurfaceMonitor.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <unistd.h>
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <binder/Parcel.h>
#include <binder/ProcessState.h>
#include <cutils/properties.h>
#include <log/log.h>
#include <utils/String16.h>

namespace android {
namespace {
constexpr nsecs_t kOperationGap = 300000000;
const String16 kTransferDescriptor("com.android.internal.app.ITransferServer");
const String16 kSysTransDescriptor("com.android.internal.app.ISysTransServer");
}

SurfaceMonitor::SurfaceMonitor()
      : mLegacyWord(0), mProcessState(1), mCallingPid(-1), mUpdating(false) {
    if (property_get_int32("sys.boot_completed", 0) == 0) return;
    {
        const int monitor = property_get_int32("persist.sys.monitor", 0);
        char buildType[PROPERTY_VALUE_MAX];
        property_get("ro.build.type", buildType, "user");
        if (ProcessState::self()->getDriverName() != String8("/dev/vndbinder") &&
                ((monitor & 1) || std::memcmp(buildType, "userdebug", 10) == 0)) {
            const sp<IServiceManager> manager = defaultServiceManager();
            if (manager) mTransfer = manager->getService(String16("transferserver"));
            const sp<IServiceManager> sysManager = defaultServiceManager();
            if (sysManager) mSysTrans = sysManager->getService(String16("systransserver"));
        } else {
            __android_log_print(ANDROID_LOG_DEBUG, nullptr, "SurfaceMonitor closed!");
        }
    }
    if (mTransfer) {
        __android_log_print(ANDROID_LOG_DEBUG, nullptr, "SurfaceMonitor init completed!");
    } else {
        __android_log_print(ANDROID_LOG_ERROR, nullptr, "SurfaceMonitor get transfer service failed!");
    }
    loadConfig();
    updateCurrentDisplayFps(mModeCount - 1, String8("init"));
    initParameter();
}

SurfaceMonitor::~SurfaceMonitor() = default;

void SurfaceMonitor::loadConfig() {
    char line[256] = {};
    FILE* config = std::fopen("/system/etc/fps_config", "r");
    bool valid = false;
    if (config) {
        std::fgets(line, sizeof(line), config);
        std::fclose(config);
        size_t count = 0;
        char* token = std::strtok(line, " ");
        bool overflow = false;
        while (token) {
            if (count >= 8) { overflow = true; break; }
            mModeFps[count++] = static_cast<float>(std::atof(token));
            token = std::strtok(nullptr, " ");
        }
        if (!overflow && count >= 2) {
            mCurrentMode = static_cast<int32_t>(count) - 2;
            mModeCount = static_cast<int32_t>(count) - 1;
            valid = true;
            for (int i = 0; i < mModeCount; ++i) {
                valid &= std::isfinite(mModeFps[i]) && mModeFps[i] > 0;
            }
        }
    }
    // Factory parsing can overwrite its mutex or index before the array for a
    // malformed file. Preserve valid-file behavior and use defaults otherwise.
    if (!valid) {
        mModeFps[0] = 72.0f;
        mModeFps[1] = 90.0f;
        mModeFps[2] = 120.0f;
        mCurrentMode = 2;
        mModeCount = 3;
    }
    for (int i = 0; i < mModeCount; ++i) {
        mPeriods[i] = static_cast<nsecs_t>(1000000000.0f / mModeFps[i]);
    }
}

void SurfaceMonitor::updateCurrentDisplayFps(int index, String8 name) {
    if (!mTransfer) return;
    mUpdating = true;
    if (mFrames.count) scanOperationArea(name);
    char display[PROPERTY_VALUE_MAX];
    property_get("sys.pvr.display.type", display, "72");
    const int hz = std::atoi(display);
    if (hz > 0) {
        const int coarsePeriod = (1000000000 / hz) / 10;
        for (int i = 0; i < mModeCount; ++i) {
            if (mPeriods[i] / 10 == coarsePeriod) { index = i; break; }
        }
    }
    // The factory caller supplies a valid configured mode. Reject invalid
    // external indices instead of reading outside the period array.
    if (index >= 0 && index < mModeCount) {
        mCurrentMode = index;
        mStandardPeriod = mPeriods[index];
    }
    mAverageFps.clear();
    mFrameDurations.clear();
    mUpdating = false;
}

void SurfaceMonitor::initParameter() {
    const int32_t defaults[10] = {85, 70, 55, 0, 0, 1, 1, 10, 10, 60};
    std::copy(std::begin(defaults), std::end(defaults), mParameters);
    mParameterLimit = 3.0f;
    if (!mSysTrans) return;
    Parcel data, reply;
    data.writeInterfaceToken(kSysTransDescriptor);
    mSysTrans->transact(4, data, &reply, 0);
    reply.readExceptionCode();
    std::vector<int32_t> parameters;
    reply.readInt32Vector(&parameters);
    if (parameters.size() == 11) {
        std::copy(parameters.begin(), parameters.begin() + 10, mParameters);
        mParameterLimit = static_cast<float>(parameters[10]);
    }
}

void SurfaceMonitor::getProcComm(int pid, char* name) {
    char path[128];
    std::snprintf(path, sizeof(path), "/proc/%d/comm", pid);
    FILE* file = std::fopen(path, "r");
    if (file) {
        std::fgets(name, 128, file);
        std::fclose(file);
    }
}

void SurfaceMonitor::setFrameItem(MonitorIndex index) {
    if ((mProcessState & 2) || !mTransfer) return;
    if (mProcessState & 1) {
        mCallingPid = IPCThreadState::self()->getCallingPid();
        if (mCallingPid == getpid()) {
            mProcessState = 16;
            mTransfer.clear();
            return;
        }
        char process[128] = {};
        char thread[128] = {};
        getProcComm(mCallingPid, process);
        if (std::memcmp(process, "system_server", 13) == 0) {
            mProcessState = 16;
            mTransfer.clear();
            return;
        }
        getProcComm(IPCThreadState::self()->getCallingTid(), thread);
        mProcessState = std::memcmp(thread, "RenderThread", 12) == 0 ? 2 : 4;
    }
    const nsecs_t now = systemTime(SYSTEM_TIME_MONOTONIC);
    const unsigned int stage = static_cast<unsigned int>(index);
    if (stage < 4) {
        mPendingTimes[stage] = now;
        if (stage == 3) mPendingTimes[4] = systemTime(SYSTEM_TIME_REALTIME);
    }
}

void SurfaceMonitor::addFrame(String8 name) {
    if ((mProcessState & 2) || !mTransfer) return;
    Mutex::Autolock lock(mMutex);
    mFrames.index = (mFrames.index + 1) % 120;
    if (mFrames.count < 120) ++mFrames.count;
    MonitorItem& item = mFrames.items[mFrames.index];
    item.state = 0;
    std::copy(std::begin(mPendingTimes), std::end(mPendingTimes), item.timestamps);
    if (mFrames.count == 120) scanOperationArea(name);
}

void SurfaceMonitor::clear() { mFrames.clear(); }

void SurfaceMonitor::scanOperationArea(const String8& name) {
    if (!mFrames.count) return;
    if (mFrames.count == 1) {
        doAnalysis(0, 0, name);
    } else {
        size_t first = 0;
        size_t i = 0;
        for (; i + 1 < mFrames.count; ++i) {
            const nsecs_t gap = mFrames.logical(i + 1).timestamps[3] -
                    mFrames.logical(i).timestamps[3];
            if (gap > kOperationGap) {
                if (first == i) continue;
            } else {
                if (first == i) continue;
                const auto& next = mFrames.logical(i + 1);
                if (next.timestamps[3] - next.timestamps[0] <= kOperationGap) continue;
            }
            doAnalysis(first, i, name);
            first = i + 1;
        }
        doAnalysis(first, i, name);
    }
    mFrames.clear();
}

void SurfaceMonitor::doAnalysis(size_t first, size_t last, const String8& name) {
    if (!mFrames.count || last < first) return;
    const int count = static_cast<int>(last - first + 1);
    const bool full = first == 0 && last == 119;
    std::vector<double> perFrameFps(full ? count : 0);
    size_t alarmFirst = 120;
    size_t alarmLast = 0;
    ssize_t alarmScore = 0;
    nsecs_t peak = 0;
    for (size_t i = first; i <= last; ++i) {
        const nsecs_t start = i > first ? mFrames.logical(i - 1).timestamps[3] :
                mFrames.logical(i).timestamps[0];
        const nsecs_t duration = mFrames.logical(i).timestamps[3] - start;
        const float delay = static_cast<float>(duration) /
                static_cast<float>(mStandardPeriod * 4);
        if (delay >= 1.0f) {
            if (alarmFirst == 120) alarmFirst = i;
            alarmScore = static_cast<ssize_t>(static_cast<float>(alarmScore) + (delay - 1.0f) * 3.0f);
            peak = std::max(peak, duration);
            alarmLast = i;
        } else if (alarmFirst < 120) {
            if (i - alarmLast < 3) {
                alarmScore -= 3;
            } else {
                reportAlarm(alarmFirst, alarmLast,
                            alarmScore + static_cast<ssize_t>(3 * (i - alarmLast)) - 3,
                            peak, name, 1);
                alarmFirst = 120;
                alarmLast = 0;
                alarmScore = 0;
                peak = 0;
            }
        }
        mFrameDurations.add(duration);
        if (full) {
            perFrameFps[i - first] = 1000000000.0 /
                    static_cast<double>(std::max(duration, mStandardPeriod));
        }
    }
    const nsecs_t total = mFrames.logical(last).timestamps[3] -
            mFrames.logical(first).timestamps[0];
    const double average = 1000000000.0 /
            static_cast<double>(std::max(total / count, mStandardPeriod));
    reportFps(average, name, total);
    if (mUpdating) return;
    if (!full || !quickConfirmFps(perFrameFps.data(), average, count)) {
        if (count > 10) {
            mAverageFps.add(average);
            if (mAverageFps.count == 10) confirmCurrentFps();
        }
    }
    if (alarmFirst < 120) {
        reportAlarm(alarmFirst, alarmLast,
                    alarmScore + static_cast<ssize_t>(3 * (last - alarmLast)),
                    peak, name, 1);
    }
}

void SurfaceMonitor::reportAlarm(size_t first, size_t last, ssize_t score,
                                 nsecs_t maximumDelay, const String8& name, int type) {
    if (score < 1 || mCallingPid == -1 || last < first || !mFrames.count) return;
    const nsecs_t total = mFrames.logical(last).timestamps[3] -
            mFrames.logical(first).timestamps[0];
    if (total < kOperationGap || !mTransfer) return;
    Parcel data, reply;
    data.writeInterfaceToken(kTransferDescriptor);
    data.writeInt32(mCallingPid);
    data.writeInt32(type);
    data.writeInt32(-1);
    data.writeInt32(0);
    data.writeInt32(kOperationGap / mStandardPeriod);
    data.writeInt64(maximumDelay);
    data.writeInt64(total);
    data.writeInt64(mFrames.logical(last).timestamps[4]);
    data.writeString16(String16(name.string()));
    data.writeInt32(mCurrentMode);
    mTransfer->transact(1, data, &reply, IBinder::FLAG_ONEWAY);
}

void SurfaceMonitor::reportFps(double fps, const String8& name, nsecs_t duration) {
    if (!mTransfer) return;
    Parcel data, reply;
    data.writeInterfaceToken(kTransferDescriptor);
    data.writeInt32(mCallingPid);
    data.writeDouble(fps);
    data.writeString16(String16(name.string()));
    data.writeInt32(mCurrentMode);
    data.writeInt64(duration);
    data.writeInt32(1);
    mTransfer->transact(5, data, &reply, IBinder::FLAG_ONEWAY);
}

void SurfaceMonitor::requestChangeDisplayFps(int index) {
    if (!mSysTrans) return;
    Parcel data, reply;
    data.writeInterfaceToken(kSysTransDescriptor);
    data.writeInt32(mCallingPid);
    data.writeInt32(index);
    data.writeInt32(1);
    mSysTrans->transact(1, data, &reply, IBinder::FLAG_ONEWAY);
}

bool SurfaceMonitor::quickConfirmFps(double* frameFps, double average, int count) {
    if (mCurrentMode != mModeCount - 1 || average >= mParameters[9]) return false;
    int outliers = 0;
    for (int i = 1; i < count; ++i) {
        __android_log_print(ANDROID_LOG_DEBUG, nullptr,
                            "SurfaceMonitor quickConfirmFps frameFps %lf", frameFps[i]);
        if (frameFps[i] - average > mParameters[8] &&
                ++outliers >= mParameters[6] * (count / 10)) return false;
    }
    requestChangeDisplayFps(mCurrentMode - 1);
    mAverageFps.clear();
    mFrameDurations.clear();
    return true;
}

void SurfaceMonitor::confirmCurrentFps() {
    std::vector<double> fps;
    for (size_t i = 0; i < mAverageFps.count; ++i) fps.push_back(mAverageFps.logical(i));
    std::sort(fps.begin(), fps.end());
    bool change = false;
    int next = mCurrentMode;
    if (mCurrentMode == mModeCount - 1) {
        int score = 0;
        for (double value : fps) {
            score += value >= mParameters[0] ? 3 : value < mParameters[1] ? -1 : 1;
        }
        if (score < mParameters[3]) { change = true; next = mCurrentMode - 1; }
    } else if (mCurrentMode == 0 &&
               std::all_of(fps.begin(), fps.end(), [&](double value) { return value > mParameters[2]; }) &&
               mFrameDurations.count > 0) {
        size_t slow = 0;
        for (size_t i = 0; i + 1 < mFrameDurations.count; ++i) {
            const nsecs_t duration = mFrameDurations.logical(i);
            if (duration > mPeriods[1]) {
                ++slow;
                __android_log_print(ANDROID_LOG_DEBUG, nullptr,
                                    "SurfaceMonitor spend time %lld, standard %lld",
                                    static_cast<long long>(duration), static_cast<long long>(mPeriods[1]));
            }
        }
        const size_t limit = static_cast<size_t>(mParameters[5]) * (mFrameDurations.count / 10);
        const int score = slow <= limit ? 1 : -1;
        if (score > mParameters[4]) { change = true; next = 1; }
    }
    if (change) {
        requestChangeDisplayFps(next);
        mAverageFps.clear();
        mFrameDurations.clear();
    }
}

} // namespace android
