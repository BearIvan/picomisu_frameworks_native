// Copyright 2026 Picomisu contributors
// SPDX-License-Identifier: Apache-2.0

// Reconstruction of the factory PICO OS libbinder_call_stat.so UidEntry and
// CallStatKey functions (first translation unit of the factory library).

#include <binder_call_stat/UidEntry.h>

namespace android {
namespace BinderStat {

namespace {
constexpr int kOverflowCallingUid = -1;
constexpr const char* kOverflowBinder = "overflow";
constexpr int kOverflowTransactionCode = -1;
constexpr int kOverflowScreenInteractive = 0;
} // namespace

size_t CallStatKey_hash(const CallStatKey& key) {
    size_t hash = std::hash<std::string>()(key.binderClass);
    return hash ^ key.transactionCode ^ key.callingUid ^ (key.screenInteractive ? 1237 : 1231);
}

bool CallStatKey_eq(const CallStatKey& a, const CallStatKey& b) {
    return a.callingUid == b.callingUid && a.transactionCode == b.transactionCode &&
            a.screenInteractive == b.screenInteractive && a.binderClass == b.binderClass;
}

UidEntry::UidEntry(int uid) : mCallStats(10, CallStatKey_hash, CallStatKey_eq) {
    workSourceUid = uid;
}

UidEntry::~UidEntry() {
    while (mCallStats.size() != 0) {
        mCallStats.erase(mCallStats.begin());
    }
}

std::shared_ptr<CallStat> UidEntry::get(int callingUid, std::string binderClass,
                                        int transactionCode, int screenInteractive) {
    mTempKey.callingUid = callingUid;
    mTempKey.binderClass = binderClass;
    mTempKey.transactionCode = transactionCode;
    mTempKey.screenInteractive = screenInteractive;
    auto it = mCallStats.find(mTempKey);
    if (it == mCallStats.end()) {
        return nullptr;
    }
    return it->second;
}

std::shared_ptr<CallStat> UidEntry::getOrCreate(int callingUid, std::string binderClass,
                                                int transactionCode, int flags,
                                                int screenInteractive, bool maxCallStatsReached,
                                                long callingPid) {
    std::shared_ptr<CallStat> mapCallStat =
            get(callingUid, binderClass, transactionCode, screenInteractive);
    // Only create a CallStat for a new entry, otherwise update the existing one.
    if (mapCallStat == nullptr) {
        if (maxCallStatsReached) {
            mapCallStat = get(kOverflowCallingUid, kOverflowBinder, kOverflowTransactionCode,
                              kOverflowScreenInteractive);
            if (mapCallStat != nullptr) {
                return mapCallStat;
            }
            callingUid = kOverflowCallingUid;
            binderClass = kOverflowBinder;
            transactionCode = kOverflowTransactionCode;
            screenInteractive = kOverflowScreenInteractive;
        }
        mapCallStat = std::make_shared<CallStat>(callingUid, binderClass, transactionCode,
                                                 screenInteractive);
        mapCallStat->flags = flags;
        mapCallStat->callingPid = callingPid;
        CallStatKey key;
        key.callingUid = callingUid;
        key.binderClass = binderClass;
        key.transactionCode = transactionCode;
        key.screenInteractive = screenInteractive;
        mCallStats.insert({key, mapCallStat});
    }
    return mapCallStat;
}

} // namespace BinderStat
} // namespace android
