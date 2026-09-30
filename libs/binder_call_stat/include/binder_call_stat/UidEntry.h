// Copyright 2026 Picomisu contributors
// SPDX-License-Identifier: Apache-2.0

// Per work source UID call statistics of the factory PICO OS
// libbinder_call_stat.so (port of BinderCallsStats.UidEntry/CallStat).
//
// The packed records are 4-byte aligned and their time and size fields are
// `long` (32-bit in 32-bit processes), as in the factory binary.

#ifndef ANDROID_BINDER_CALL_STAT_UIDENTRY_H
#define ANDROID_BINDER_CALL_STAT_UIDENTRY_H

#include <sys/types.h>

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace android {
namespace BinderStat {

#pragma pack(push, 4)

struct CallStatKey {
    int callingUid = 0;
    std::string binderClass = "";
    int transactionCode = 0;
    int screenInteractive = 0;
};

struct CallStat {
    CallStat(int uid, std::string cls, int code, int screen) : callingUid(uid) {
        binderClass = cls;
        transactionCode = code;
        screenInteractive = screen;
    }

    int callingUid;
    std::string binderClass;
    int transactionCode;
    int screenInteractive;
    int flags = 0;
    long quickTimeMicros = 0;
    long maxQuickTimeMicros = 0;
    long recordedCallCount = 0;
    long callCount = 0;
    long cpuTimeMicros = 0;
    long maxCpuTimeMicros = 0;
    long latencyMicros = 0;
    long maxLatencyMicros = 0;
    long maxRequestSizeBytes = 0;
    long maxReplySizeBytes = 0;
    long exceptionCount = 0;
    long callingPid = 0;
};

#pragma pack(pop)

size_t CallStatKey_hash(const CallStatKey& key);
bool CallStatKey_eq(const CallStatKey& a, const CallStatKey& b);

class UidEntry {
public:
    explicit UidEntry(int uid);
    ~UidEntry();

    std::shared_ptr<CallStat> get(int callingUid, std::string binderClass, int transactionCode,
                                  int screenInteractive);
    std::shared_ptr<CallStat> getOrCreate(int callingUid, std::string binderClass,
                                          int transactionCode, int flags, int screenInteractive,
                                          bool maxCallStatsReached, long callingPid);

    int workSourceUid;
    long recordedCallCount = 0;
    long callCount = 0;
    long cpuTimeMicros = 0;
    std::unordered_map<CallStatKey, std::shared_ptr<CallStat>,
                       std::function<size_t(const CallStatKey&)>,
                       std::function<size_t(const CallStatKey&, const CallStatKey&)>>
            mCallStats;
    CallStatKey mTempKey;
};

} // namespace BinderStat
} // namespace android

#endif // ANDROID_BINDER_CALL_STAT_UIDENTRY_H
