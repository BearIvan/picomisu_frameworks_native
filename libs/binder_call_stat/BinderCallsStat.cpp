// Copyright 2026 Picomisu contributors
// SPDX-License-Identifier: Apache-2.0

// Reconstruction of the factory PICO OS libbinder_call_stat.so
// (BinderCallsStat.cpp). Behaviour, including its failure modes, follows the
// factory machine code:
// - getSysTime() computes tv_sec * 1000000 in `long`; in 32-bit processes the
//   integer sanitizer aborts the process ("ubsan: mul-overflow") whenever a
//   call is timed (sampled calls and oneway calls).
// - the dump text is appended to the dumpsys fd after lseek(SEEK_END), the
//   process name buffer is not cleared before it is read, and every dump
//   clears the collected data.

#define LOG_TAG "NativeBinderStat"

#include <binder_call_stat/BinderCallsStat.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>

#include <log/log.h>
#include <utils/String8.h>
#include <utils/Timers.h>

namespace android {
namespace BinderStat {

namespace {
// IBinder::DUMP_TRANSACTION and IBinder::FLAG_ONEWAY.
constexpr int kDumpTransaction = ('_' << 24) | ('D' << 16) | ('M' << 8) | 'P';
constexpr int kFlagOneway = 0x01;
constexpr long kMinQuickDuration = 20;
} // namespace

// Serializes the in-flight call counter checked by the destructor.
static Mutex& gActiveCallsLock = *new Mutex;

BinderCallsStat::BinderCallsStat() : mCallSessionsPool(2) {
    srand(100);
    mLock = new Mutex();
    mEnabled = true;
}

BinderCallsStat::~BinderCallsStat() {
    mEnabled = false;
    while (mActiveCalls != 0) {
        sleep(1);
    }
    delete mLock;
}

CallSession* BinderCallsStat::binderCallStarted(String16 binderClass, int code, int flags,
                                                int callingPid) {
    if (code == kDumpTransaction || !mEnabled) {
        return nullptr;
    }
    {
        Mutex::Autolock _l(gActiveCallsLock);
        mActiveCalls++;
    }
    CallSession* s = obtainCallSession();
    s->binderClass = std::string(String8(binderClass).string());
    s->transactionCode = code;
    s->flags = flags;
    s->quickTimeStarted = getQuickTime();
    s->timeStarted = -1;
    s->cpuTimeStarted = -1;
    if (shouldRecordDetailedData() || (flags & kFlagOneway)) {
        s->timeStarted = getSysTime();
        s->cpuTimeStarted = getThreadTime();
    }
    s->callingPid = callingPid;
    {
        Mutex::Autolock _l(gActiveCallsLock);
        mActiveCalls--;
    }
    return s;
}

long BinderCallsStat::getQuickTime() {
    return -1;
}

bool BinderCallsStat::shouldRecordDetailedData() {
    return rand() % mPeriodicSamplingInterval == 0;
}

long BinderCallsStat::getSysTime() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return tv.tv_sec * 1000000 + tv.tv_usec;
}

long BinderCallsStat::getThreadTime() {
    return systemTime(SYSTEM_TIME_THREAD) / 1000;
}

void BinderCallsStat::binderCallEnded(CallSession* s, int parcelRequestSize, int parcelReplySize,
                                      int workSourceUid, int err) {
    if (s == nullptr) {
        return;
    }
    if (!mEnabled) {
        delete s;
        return;
    }
    {
        Mutex::Autolock _l(gActiveCallsLock);
        mActiveCalls++;
    }
    if (err != 0) {
        s->exceptionThrown = true;
    }
    processCallEnded(s, parcelRequestSize, parcelReplySize, workSourceUid);
    mCallSessionsPool.release_object(s);
    {
        Mutex::Autolock _l(gActiveCallsLock);
        mActiveCalls--;
    }
}

void BinderCallsStat::processCallEnded(CallSession* s, long parcelRequestSize,
                                       long parcelReplySize, int workSourceUid) {
    // A non-negative time signals that detailed data is recorded for this call.
    const bool recordCall = s->cpuTimeStarted >= 0;
    const long quickDuration = getQuickTime() - s->quickTimeStarted;
    long duration;
    long latencyDuration;
    if (recordCall) {
        duration = getThreadTime() - s->cpuTimeStarted;
        latencyDuration = getSysTime() - s->timeStarted;
    } else {
        duration = 0;
        latencyDuration = 0;
    }

    Mutex::Autolock _l(mLock);
    std::shared_ptr<UidEntry> uidEntry = getUidEntry(workSourceUid);
    uidEntry->callCount++;
    if (recordCall || quickDuration > kMinQuickDuration) {
        uidEntry->cpuTimeMicros += duration;
        uidEntry->recordedCallCount++;

        std::shared_ptr<CallStat> callStat = uidEntry->getOrCreate(
                workSourceUid, s->binderClass, s->transactionCode, s->flags,
                getScreenInteractive(), mCallStatsCount >= mMaxBinderCallStatsCount,
                s->callingPid);
        const bool isNewCallStat = callStat->callCount == 0;
        if (isNewCallStat) {
            mCallStatsCount++;
        }
        callStat->callCount++;
        callStat->recordedCallCount++;
        callStat->cpuTimeMicros += duration;
        callStat->maxCpuTimeMicros = std::max(callStat->maxCpuTimeMicros, duration);
        callStat->quickTimeMicros += quickDuration;
        callStat->maxQuickTimeMicros = std::max(callStat->maxQuickTimeMicros, quickDuration);
        callStat->latencyMicros += latencyDuration;
        callStat->maxLatencyMicros = std::max(callStat->maxLatencyMicros, latencyDuration);
        callStat->exceptionCount += s->exceptionThrown;
        if (mDetailedTracking) {
            callStat->maxRequestSizeBytes =
                    std::max(callStat->maxRequestSizeBytes, parcelRequestSize);
            callStat->maxReplySizeBytes = std::max(callStat->maxReplySizeBytes, parcelReplySize);
        }
    } else {
        // Only record the total call count if data is already tracked for this key.
        std::shared_ptr<CallStat> callStat = uidEntry->get(
                workSourceUid, s->binderClass, s->transactionCode, getScreenInteractive());
        if (callStat != nullptr) {
            callStat->callCount++;
            callStat->quickTimeMicros += quickDuration;
            callStat->maxQuickTimeMicros = std::max(callStat->maxQuickTimeMicros, quickDuration);
        }
    }
}

std::shared_ptr<UidEntry> BinderCallsStat::getUidEntry(int workSourceUid) {
    auto it = mUidEntries.find(workSourceUid);
    if (it != mUidEntries.end()) {
        return it->second;
    }
    std::shared_ptr<UidEntry> uidEntry = std::make_shared<UidEntry>(workSourceUid);
    mUidEntries.insert(std::make_pair(workSourceUid, uidEntry));
    return uidEntry;
}

int BinderCallsStat::getScreenInteractive() {
    return 0;
}

CallSession* BinderCallsStat::obtainCallSession() {
    return mCallSessionsPool.acquire_object();
}

void BinderCallsStat::setSamplingInterval(int interval) {
    mPeriodicSamplingInterval = interval;
}

int BinderCallsStat::dump(int fd, const Vector<String16>& args) {
    Mutex::Autolock _l(mLock);
    int argc = args.size();
    for (int i = 0; i < argc; i++) {
        if (args[i] == String16("--disable")) {
            ALOGD("--disable");
            clearData();
            return 0;
        }
        if (args[i] == String16("--sample-interval")) {
            if (i + 1 < argc) {
                mPeriodicSamplingInterval = atoi(String8(args[i + 1]).string());
                ALOGD("sample interval setting %d\n", mPeriodicSamplingInterval);
                return 1;
            }
            ALOGE("parameter error");
            return 2;
        }
    }
    dumpLock(fd);
    clearData();
    return 3;
}

void BinderCallsStat::clearData() {
    mCallStatsCount = 0;
    while (mUidEntries.size() != 0) {
        mUidEntries.erase(mUidEntries.begin());
    }
}

void BinderCallsStat::dumpLock(int fd) {
    writeHeadInfo(fd);
    std::vector<std::shared_ptr<ExportedCallStat>> exportedCallStats;
    getExportedCallStats(&exportedCallStats);
    if (!exportedCallStats.empty()) {
        std::sort(exportedCallStats.begin(), exportedCallStats.end(), compareByCpuDesc);
    }
    while (!exportedCallStats.empty()) {
        std::shared_ptr<ExportedCallStat> e = exportedCallStats.back();
        std::string line = std::to_string(e->callingPid);
        line = line.append(",") + std::to_string(e->workSourceUid);
        line = line.append(",") + e->className;
        line = line.append("#") + std::to_string(e->transactionCode);
        line = line.append(",") + std::to_string(e->flags);
        line = line.append(",") + std::to_string(e->quickTimeMicros);
        line = line.append(",") + std::to_string(e->maxQuickTimeMicros);
        line = line.append(",") + std::to_string(e->screenInteractive);
        line = line.append(",") + std::to_string(e->cpuTimeMicros);
        line = line.append(",") + std::to_string(e->maxCpuTimeMicros);
        line = line.append(",") + std::to_string(e->latencyMicros);
        line = line.append(",") + std::to_string(e->maxLatencyMicros);
        line = line.append(",") + std::to_string(e->exceptionCount);
        line = line.append(",") + std::to_string(e->maxRequestSizeBytes);
        line = line.append(",") + std::to_string(e->maxReplySizeBytes);
        line = line.append(",") + std::to_string(e->recordedCallCount);
        line = line.append(",") + std::to_string(e->callCount);
        line.append("\n");
        write(fd, line.c_str(), line.size());
        exportedCallStats.pop_back();
    }
    std::string end = "====================\n\n\n";
    write(fd, end.c_str(), end.size());
}

void BinderCallsStat::writeHeadInfo(int fd) {
    lseek(fd, 0, SEEK_END);
    time_t now = time(nullptr);
    char buf[128];
    readProcName(buf);
    std::string head = "";
    head.append("proc name:");
    head.append(buf);
    memset(buf, 0, sizeof(buf));
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", localtime(&now));
    head.append("\nStart time: ");
    head.append(buf);
    head.append("\nOn battery time (ms): 0");
    head.append("\nSampling interval period: ");
    head.append(std::to_string(mPeriodicSamplingInterval));
    head.append("\n");
    write(fd, head.c_str(), head.size());
}

int BinderCallsStat::getExportedCallStats(
        std::vector<std::shared_ptr<ExportedCallStat>>* exportedCallStats) {
    for (auto& uidPair : mUidEntries) {
        std::shared_ptr<UidEntry> entry = uidPair.second;
        for (auto& callPair : entry->mCallStats) {
            std::shared_ptr<ExportedCallStat> exported = std::make_shared<ExportedCallStat>();
            std::shared_ptr<CallStat> stat = callPair.second;
            exported->callingPid = stat->callingPid;
            exported->workSourceUid = entry->workSourceUid;
            exported->callingUid = stat->callingUid;
            exported->className = stat->binderClass;
            exported->transactionCode = stat->transactionCode;
            exported->flags = stat->flags;
            exported->screenInteractive = stat->screenInteractive;
            exported->quickTimeMicros = stat->quickTimeMicros;
            exported->maxQuickTimeMicros = stat->maxQuickTimeMicros;
            exported->cpuTimeMicros = stat->cpuTimeMicros;
            exported->maxCpuTimeMicros = stat->maxCpuTimeMicros;
            exported->latencyMicros = stat->latencyMicros;
            exported->maxLatencyMicros = stat->maxLatencyMicros;
            exported->recordedCallCount = stat->recordedCallCount;
            exported->callCount = stat->callCount;
            exported->maxRequestSizeBytes = stat->maxRequestSizeBytes;
            exported->maxReplySizeBytes = stat->maxReplySizeBytes;
            exported->exceptionCount = stat->exceptionCount;
            exportedCallStats->push_back(exported);
        }
    }
    return exportedCallStats->size();
}

bool BinderCallsStat::compareByCpuDesc(std::shared_ptr<ExportedCallStat> a,
                                       std::shared_ptr<ExportedCallStat> b) {
    return a->cpuTimeMicros > b->cpuTimeMicros;
}

void BinderCallsStat::readProcName(char* procName) {
    FILE* f = fopen("/proc/self/cmdline", "r");
    if (f != nullptr) {
        fread(procName, 1, 128, f);
        fclose(f);
    }
}

} // namespace BinderStat
} // namespace android
