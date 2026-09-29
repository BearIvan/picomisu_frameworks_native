// Copyright 2026 Picomisu contributors
// SPDX-License-Identifier: Apache-2.0
// Local Binder relay comparison only. Never contacts the real freeze service.
#include <binder/Binder.h>
#include <binder/IFreezeManager.h>
#include <binder/IUnFreezeCallback.h>
#include <binder/Parcel.h>
#include <climits>
#include <cstdio>
#include <new>
#include <vector>
using namespace android;
namespace {
template<class T, size_t Reserve>
sp<T> padded() {
    static_assert(sizeof(T) <= Reserve, "Insufficient object storage");
    return ::new (::operator new(Reserve)) T;
}
class Recorder : public BnUnFreezeCallback {
public:
    void onUnFreeze(int pid) override { pids.push_back(pid); }
    std::vector<int> pids;
};
class FreezeRelay : public BBinder {
public:
    const String16& getInterfaceDescriptor() const override { return IFreezeManager::descriptor; }
    int calls = 0, pid = 0;
    uint32_t lastCode = 0, lastFlags = 0;
    bool flag = false, parsed = false, hasReply = false;
    sp<IBinder> callback;
protected:
    status_t onTransact(uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags) override {
        ++calls; lastCode = code; lastFlags = flags; hasReply = reply != nullptr;
        parsed = data.checkInterface(this);
        parsed = data.readInt32(&pid) == NO_ERROR && parsed;
        callback = data.readStrongBinder();
        if (code == IFreezeManager::REGISTER) parsed = data.readBool(&flag) == NO_ERROR && parsed;
        parsed = data.dataAvail() == 0 && parsed;
        return UNKNOWN_TRANSACTION; // Both factory void proxy methods ignore transport status.
    }
};
class CallbackRelay : public BBinder {
public:
    sp<IBinder> target;
    uint32_t code = 0, flags = 0;
    bool hasReply = false;
protected:
    status_t onTransact(uint32_t c, const Parcel& data, Parcel* reply, uint32_t f) override {
        code = c; flags = f; hasReply = reply != nullptr;
        return target->transact(c, data, reply, f);
    }
};
bool check(bool good, const char* label) {
    if (!good) fprintf(stderr, "freeze-wire fixture failed: %s\n", label);
    else printf("freeze-wire %s=1\n", label);
    return good;
}
}
int main() {
    if (!check(IFreezeManager::descriptor == String16("android.app.IFreezeManager") &&
               IUnFreezeCallback::descriptor == String16("android.app.IUnFreezeCallback"),
               "descriptors")) return 1;
    if (!check(IFreezeManager::asInterface(nullptr) == nullptr &&
               IUnFreezeCallback::asInterface(nullptr) == nullptr, "null-interface")) return 1;
    auto relay = padded<FreezeRelay, 1024>();
    auto service = IFreezeManager::asInterface(relay);
    auto receiver = padded<Recorder, 1024>();
    auto binder = IInterface::asBinder(receiver);
    if (!check(IUnFreezeCallback::asInterface(binder).get() == receiver.get(), "local-callback-identity")) return 1;
    service->registerUnFreezeListener(42, nullptr, false);
    if (!check(relay->calls == 1 && relay->lastCode == 1 && relay->lastFlags == 0 &&
               relay->pid == 42 && !relay->callback && !relay->flag && relay->parsed && relay->hasReply,
               "register-null-false-synchronous")) return 1;
    service->registerUnFreezeListener(INT_MIN, receiver, true);
    if (!check(relay->calls == 2 && relay->lastCode == 1 && relay->lastFlags == 0 &&
               relay->pid == INT_MIN && relay->callback == binder && relay->flag && relay->parsed,
               "register-callback-true-signed-pid")) return 1;
    service->unRegisterUnFreezeListener(INT_MAX, receiver);
    if (!check(relay->calls == 3 && relay->lastCode == 2 && relay->lastFlags == 0 &&
               relay->pid == INT_MAX && relay->callback == binder && relay->parsed && relay->hasReply,
               "unregister-without-boolean")) return 1;
    auto callbackRelay = padded<CallbackRelay, 1024>();
    callbackRelay->target = binder;
    auto proxy = IUnFreezeCallback::asInterface(callbackRelay);
    proxy->onUnFreeze(-42);
    if (!check(receiver->pids == std::vector<int>{-42} && callbackRelay->code == 1 &&
               callbackRelay->flags == IBinder::FLAG_ONEWAY && callbackRelay->hasReply,
               "callback-oneway")) return 1;
    for (int pid : {0, INT_MIN, INT_MAX}) {
        Parcel data, reply;
        data.writeInterfaceToken(IUnFreezeCallback::descriptor);
        data.writeInt32(pid);
        const size_t before = receiver->pids.size();
        if (binder->transact(1, data, &reply, 0) != NO_ERROR ||
            receiver->pids.size() != before + 1 || receiver->pids.back() != pid || reply.dataSize() != 0) return 1;
    }
    if (!check(receiver->pids.size() == 4, "callback-signed-boundaries-empty-reply")) return 1;
    {
        Parcel data, reply;
        data.writeInterfaceToken(String16("invalid.freeze.callback"));
        data.writeInt32(123);
        if (!check(binder->transact(1, data, &reply, 0) == PERMISSION_DENIED && receiver->pids.size() == 4,
                   "callback-rejects-wrong-token")) return 1;
    }
    {
        Parcel data, reply;
        data.writeInterfaceToken(IUnFreezeCallback::descriptor);
        if (!check(binder->transact(1, data, &reply, 0) == NO_ERROR && receiver->pids.back() == 0,
                   "callback-legacy-short-payload-is-zero")) return 1;
    }
    {
        Parcel data, reply;
        data.writeInterfaceToken(IUnFreezeCallback::descriptor);
        data.writeInt32(77); data.writeInt32(88);
        if (!check(binder->transact(1, data, &reply, 0) == NO_ERROR && receiver->pids.back() == 77,
                   "callback-ignores-trailing-data")) return 1;
    }
    {
        Parcel data, reply;
        const size_t before = receiver->pids.size();
        if (!check(binder->transact(100, data, &reply, 0) == UNKNOWN_TRANSACTION && receiver->pids.size() == before,
                   "callback-unknown-transaction")) return 1;
    }
    puts("freeze-wire-probe passed=12");
    return 0;
}
