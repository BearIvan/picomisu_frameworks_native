// Copyright 2026 Picomisu contributors
// SPDX-License-Identifier: Apache-2.0

// Native binder call statistics of the factory PICO OS libbinder_call_stat.so,
// a port of com.android.internal.os.BinderCallsStats. libbinder creates one
// process-wide instance (android::mObserver) on "dumpsys <service> --enable"
// and reports every BBinder::transact() to it.
//
// The factory binary fixes the layouts below: libbinder allocates
// sizeof(BinderCallsStat) itself, the packed records are 4-byte aligned, and
// the time and size fields are `long` (32-bit in 32-bit processes).

#ifndef ANDROID_BINDER_CALL_STAT_BINDERCALLSSTAT_H
#define ANDROID_BINDER_CALL_STAT_BINDERCALLSSTAT_H

#include <sys/types.h>

#include <deque>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <binder_call_stat/UidEntry.h>
#include <utils/Mutex.h>
#include <utils/String16.h>
#include <utils/Vector.h>

namespace android {
namespace BinderStat {

// Guards the creation of the observer in libbinder (BBinder::setObserver).
// Every translation unit including this header gets its own instance.
static Mutex& gBinderStatLock = *new Mutex;

#pragma pack(push, 4)

struct CallSession {
    std::string binderClass = "";
    int transactionCode = 0;
    long cpuTimeStarted = 0;
    long timeStarted = 0;
    bool exceptionThrown = false;
    int flags = 0;
    long quickTimeStarted = 0;
    long callingPid = 0;
};

struct ExportedCallStat {
    int callingUid = 0;
    int workSourceUid = 0;
    std::string className = "";
    bool screenInteractive = false;
    long cpuTimeMicros = 0;
    long maxCpuTimeMicros = 0;
    long latencyMicros = 0;
    long maxLatencyMicros = 0;
    long callCount = 0;
    long recordedCallCount = 0;
    long maxRequestSizeBytes = 0;
    long maxReplySizeBytes = 0;
    long exceptionCount = 0;
    int transactionCode = 0;
    int flags = 0;
    long quickTimeMicros = 0;
    long maxQuickTimeMicros = 0;
    long callingPid = 0;
};

#pragma pack(pop)

template <typename T>
class ObjectPool {
public:
    explicit ObjectPool(size_t size);
    ~ObjectPool();

    T* acquire_object();
    void release_object(T* object);

private:
    std::deque<T*> mPool;
    size_t mSize;
    Mutex mLock;
};

// Out of line (not implicitly inline) so that the instantiations are exported
// as weak symbols like in the factory library.
template <typename T>
ObjectPool<T>::ObjectPool(size_t size) : mSize(size) {
    if (mSize == 0) {
        std::cout << "Object size invalid" << std::endl;
    }
}

template <typename T>
ObjectPool<T>::~ObjectPool() {
    Mutex::Autolock _l(mLock);
    while (!mPool.empty()) {
        T* object = mPool.front();
        delete object;
        mPool.pop_front();
    }
}

template <typename T>
T* ObjectPool<T>::acquire_object() {
    Mutex::Autolock _l(mLock);
    if (!mPool.empty()) {
        T* object = mPool.front();
        mPool.pop_front();
        return object;
    }
    return new T;
}

template <typename T>
void ObjectPool<T>::release_object(T* object) {
    Mutex::Autolock _l(mLock);
    if (mPool.size() < mSize) {
        mPool.push_back(object);
    } else {
        delete object;
    }
}

class BinderCallsStat {
public:
    BinderCallsStat();
    ~BinderCallsStat();

    // Used by libbinder only; weak so that it stays out of line, and hidden
    // (-fvisibility-inlines-hidden) so that it stays local to libbinder.
    __attribute__((weak)) static BinderCallsStat* statsInternalInstance() {
        return new BinderCallsStat();
    }

    // libbinder imports these as weak references.
    __attribute__((weak)) CallSession* binderCallStarted(String16 binderClass, int code, int flags,
                                                         int callingPid);
    __attribute__((weak)) void binderCallEnded(CallSession* s, int parcelRequestSize,
                                               int parcelReplySize, int workSourceUid, int err);
    // Returns 0 after "--disable" (libbinder then deletes the observer), 1 after
    // "--sample-interval N", 2 on a missing interval and 3 after a dump.
    __attribute__((weak)) int dump(int fd, const Vector<String16>& args);
    __attribute__((weak)) void setSamplingInterval(int interval);

    void processCallEnded(CallSession* s, long parcelRequestSize, long parcelReplySize,
                          int workSourceUid);
    CallSession* obtainCallSession();
    std::shared_ptr<UidEntry> getUidEntry(int workSourceUid);
    int getExportedCallStats(std::vector<std::shared_ptr<ExportedCallStat>>* exportedCallStats);
    void dumpLock(int fd);
    void writeHeadInfo(int fd);
    void readProcName(char* procName);
    void clearData();
    bool shouldRecordDetailedData();
    int getScreenInteractive();
    long getSysTime();
    long getQuickTime();
    long getThreadTime();

private:
    __attribute__((visibility("hidden"))) static bool compareByCpuDesc(
            std::shared_ptr<ExportedCallStat> a, std::shared_ptr<ExportedCallStat> b);

    ObjectPool<CallSession> mCallSessionsPool;
    std::unordered_map<int, std::shared_ptr<UidEntry>> mUidEntries;
    Mutex* mLock;
    int mCallStatsCount = 0;
    int mMaxBinderCallStatsCount = 2000;
    int mPeriodicSamplingInterval = 1000;
    bool mDetailedTracking = true;
    bool mEnabled = false;
    int mActiveCalls = 0;
};

} // namespace BinderStat
} // namespace android

#endif // ANDROID_BINDER_CALL_STAT_BINDERCALLSSTAT_H
