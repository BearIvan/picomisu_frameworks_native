// Copyright 2026 Picomisu contributors
// SPDX-License-Identifier: Apache-2.0
// Observation only: fake service and token, all BINDER_WRITE_READ intercepted.
#include <binder/Binder.h>
#include <binder/FreezeManager.h>
#include <binder/IPCThreadState.h>
#include <binder/Parcel.h>
#include <private/binder/PicoFreeze.h>
#include <linux/android/binder.h>
#include <dlfcn.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>
#include "PicoFreezeRegistryFake.h"
using namespace android;
namespace {
sp<picomisu::ServiceManager> fakeManager;
std::vector<int32_t> responses;
int exchanges = 0;
}
namespace android { sp<IServiceManager> defaultServiceManager() { return fakeManager; } }
extern "C" int ioctl(int fd, int request, ...) {
    va_list ap; va_start(ap, request); void* arg = va_arg(ap, void*); va_end(ap);
    if (static_cast<uint32_t>(request) != static_cast<uint32_t>(BINDER_WRITE_READ)) {
        return static_cast<int>(syscall(SYS_ioctl, fd, request, arg));
    }
    auto* transfer = static_cast<binder_write_read*>(arg);
    ++exchanges;
    transfer->write_consumed = transfer->write_size;
    transfer->read_consumed = 0;
    if (transfer->read_size) {
        if (responses.empty() || responses.size() * sizeof(int32_t) > transfer->read_size) {
            errno = EIO; return -1;
        }
        std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(transfer->read_buffer)),
                    responses.data(), responses.size() * sizeof(int32_t));
        transfer->read_consumed = responses.size() * sizeof(int32_t);
        responses.clear();
    }
    return 0;
}
extern "C" status_t selectedWait(IPCThreadState*, Parcel*, status_t*)
        asm("_ZN7android14IPCThreadState15waitForResponseEPNS_6ParcelEPi");
namespace {
using Register = void (*)(FreezeManager*, void*, const sp<IBinder>&,
                           const std::function<void(const void*)>&, void*, bool);
using RemoveKey = void (*)(FreezeManager*, const void*, const sp<IBinder>&);
using RemoveToken = void (*)(FreezeManager*, const sp<IBinder>&);
class Token : public BBinder {
public:
    int links = 0, unlinks = 0;
    bool parametersValid = true;
    std::vector<sp<DeathRecipient>> recipients;
    status_t linkToDeath(const sp<DeathRecipient>& who, void* cookie, uint32_t flags) override {
        ++links;
        parametersValid &= who != nullptr && cookie == nullptr && flags == 0;
        recipients.push_back(who);
        return NO_ERROR;
    }
    status_t unlinkToDeath(const wp<DeathRecipient>& who, void* cookie,
                           uint32_t flags, wp<DeathRecipient>* out) override {
        ++unlinks;
        parametersValid &= cookie == nullptr && flags == 0;
        for (auto it = recipients.begin(); it != recipients.end(); ++it) {
            if (wp<DeathRecipient>(*it) == who) {
                if (out) *out = who;
                recipients.erase(it);
                return NO_ERROR;
            }
        }
        return NAME_NOT_FOUND;
    }
    void deliverDeath(const sp<IBinder>& self) {
        auto snapshot = recipients;
        for (const auto& entry : snapshot) entry->binderDied(wp<IBinder>(self));
    }
};
bool check(bool ok, const char* label) {
    if (!ok) fprintf(stderr, "freeze-remote preflight failed: %s\n", label);
    else printf("freeze-remote-check %s=1\n", label);
    return ok;
}
bool setPid(int pid) {
    auto* thread = IPCThreadState::self();
    responses = {static_cast<int32_t>(PICO_BR_FROZEN_REPLY), pid};
    const int before = exchanges;
    return selectedWait(thread, nullptr, nullptr) == PICO_FROZEN_TRANSACTION &&
           thread->getLastFrozenPid() == pid && responses.empty() && exchanges > before;
}
void snapshot(const char* label, FreezeManager* manager, const picomisu::FreezeService& service,
               const Token& a, const Token& b, int first, int replacement, int second, int once) {
    printf("freeze-remote %s registrations=%zu removals=%zu links-a=%d links-b=%d "
           "unlinks-a=%d unlinks-b=%d first=%d replacement=%d second=%d once=%d\n",
           label, service.registrations.size(), service.removals.size(), a.links, b.links,
           a.unlinks, b.unlinks, first, replacement, second, once);
#if defined(__aarch64__)
    // Authenticated factory ARM64 map size offsets; no foreign C++ containers are used.
    size_t onceSize, persistentSize;
    std::memcpy(&onceSize, reinterpret_cast<const char*>(manager) + 88, sizeof(onceSize));
    std::memcpy(&persistentSize, reinterpret_cast<const char*>(manager) + 112, sizeof(persistentSize));
    printf("freeze-remote-map %s once=%zu persistent=%zu\n", label, onceSize, persistentSize);
#else
    (void)manager;
#endif
}
}
int main() {
    auto reg = reinterpret_cast<Register>(dlsym(RTLD_DEFAULT,
        "_ZN7android13FreezeManager24registerUnFreezeListenerEPvRKNS_2spINS_7IBinderEEERKNSt3__18functionIFvPKvEEES1_b"));
    auto removeKey = reinterpret_cast<RemoveKey>(dlsym(RTLD_DEFAULT,
        "_ZN7android13FreezeManager26unRegisterUnFreezeListenerEPKvRKNS_2spINS_7IBinderEEE"));
    auto removeToken = reinterpret_cast<RemoveToken>(dlsym(RTLD_DEFAULT,
        "_ZN7android13FreezeManager26unRegisterUnFreezeListenerERKNS_2spINS_7IBinderEEE"));
    if (!check(reg && removeKey && removeToken, "remote-exports")) return 1;
    fakeManager = new picomisu::ServiceManager;
    sp<picomisu::FreezeService> service = new picomisu::FreezeService;
    fakeManager->service = IInterface::asBinder(service);
    FreezeManager* manager = FreezeManager::getInstance();
    auto selected = manager->getService();
    if (!check(selected && IInterface::asBinder(selected) == fakeManager->service &&
               fakeManager->lookups == 1 && !fakeManager->wrongName, "fake-service")) return 1;
    int selfKey, selfCalls = 0, key1, key2, key3;
    int first = 0, replacement = 0, second = 0, once = 0;
    auto count = [](const void* arg) { ++*static_cast<int*>(const_cast<void*>(arg)); };
    manager->registerSelfUnFreezeListener(&selfKey, count, &selfCalls, false);
    sp<IUnFreezeCallback> delivery = service->latest();
    delivery->onUnFreeze(getpid());
    manager->unRegisterSelfUnFreezeListener(&selfKey);
    if (!check(selfCalls == 1, "self-delivery-control")) return 1;
    service->registrations.clear();
    const int pid = getpid() + 100000;
    if (!check(setPid(pid), "synthetic-frozen-pid")) return 1;
    sp<Token> a = new Token, b = new Token;
    sp<IBinder> ta = a, tb = b;
    auto show = [&](const char* label) { snapshot(label, manager, *service, *a, *b,
                                               first, replacement, second, once); };
    reg(manager, &key1, ta, count, &first, false);
    if (!check(service->registrations.size() == 1 && service->registrations.back().pid == pid &&
               !service->registrations.back().flag && a->links == 1, "first-remote-side-effects")) return 1;
    delivery->onUnFreeze(pid);
    show("persistent-first");
    reg(manager, &key1, ta, count, &replacement, false);
    delivery->onUnFreeze(pid);
    show("persistent-duplicate");
    reg(manager, &key2, ta, count, &second, false);
    delivery->onUnFreeze(pid);
    show("persistent-two-keys");
    delivery->onUnFreeze(pid + 1);
    show("wrong-pid");
    reg(manager, &key3, tb, count, &once, true);
    delivery->onUnFreeze(pid);
    show("once-first");
    delivery->onUnFreeze(pid);
    show("once-second");
    removeKey(manager, &key1, ta);
    delivery->onUnFreeze(pid);
    show("key-remove");
    removeToken(manager, ta);
    delivery->onUnFreeze(pid);
    show("token-remove");
    reg(manager, &key1, ta, count, &first, false);
    if (!check(setPid(pid + 2), "changed-frozen-pid")) return 1;
    a->deliverDeath(ta);
    delivery->onUnFreeze(pid);
    show("death-after-pid-change");
    if (!check(a->parametersValid && b->parametersValid, "death-link-parameters")) return 1;
    puts("freeze-remote-observation completed=1");
    return 0;
}
