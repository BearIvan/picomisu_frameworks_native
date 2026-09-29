// Copyright 2026 Picomisu contributors
// SPDX-License-Identifier: Apache-2.0
// Exported defaultServiceManager interposition confines both libraries to a fake service.
#include <binder/FreezeManager.h>
#include <binder/IPCThreadState.h>
#include <cstdio>
#include <unistd.h>
#include "PicoFreezeRegistryFake.h"
using namespace android;
namespace { sp<picomisu::ServiceManager> fakeManager; }
namespace android {
sp<IServiceManager> defaultServiceManager() { return fakeManager; }
}
namespace {
bool check(bool good, const char* label) {
    if (good) printf("freeze-registry %s=1\n", label);
    else fprintf(stderr, "freeze-registry fixture failed: %s\n", label);
    return good;
}
class IdleCallback : public BnUnFreezeCallback {
public: void onUnFreeze(int) override {}
};
}
int main() {
    fakeManager = new picomisu::ServiceManager;
    sp<picomisu::FreezeService> service = new picomisu::FreezeService;
    fakeManager->service = IInterface::asBinder(service);
    auto* manager = FreezeManager::getInstance();
    // Abort before any registration if library lookup did not resolve the fake service.
    auto selected = manager->getService();
    if (!check(selected && IInterface::asBinder(selected) == fakeManager->service &&
               fakeManager->lookups == 1 && !fakeManager->wrongName, "fake-service-preflight")) return 1;
    if (!check(FreezeManager::getInstance() == manager && manager->getService() == selected &&
               fakeManager->lookups == 1, "singleton-and-service-cache")) return 1;
    int key1, key2, key3, first = 0, replacement = 0, second = 0;
    auto count = [](const void* argument) { ++*static_cast<int*>(const_cast<void*>(argument)); };
    manager->registerSelfUnFreezeListener(&key1, count, &first, true);
    if (!check(service->registrations.size() == 1 && service->registrations.back().pid == getpid() &&
               service->registrations.back().flag && service->latest() != nullptr,
               "first-self-registers-process-callback")) return 1;
    manager->registerSelfUnFreezeListener(&key1, count, &replacement, false);
    service->latest()->onUnFreeze(getpid());
    if (!check(first == 1 && replacement == 0 && service->registrations.size() == 1,
               "duplicate-key-retains-original")) return 1;
    manager->registerSelfUnFreezeListener(&key2, count, &second, false);
    service->latest()->onUnFreeze(getpid());
    if (!check(first == 2 && second == 1 && service->registrations.size() == 1,
               "multiple-keys-share-process-registration")) return 1;
    service->latest()->onUnFreeze(getpid() + 100000);
    if (!check(first == 2 && second == 1, "self-events-filter-pid")) return 1;
    manager->unRegisterSelfUnFreezeListener(&key1);
    manager->unRegisterSelfUnFreezeListener(&key3);
    service->latest()->onUnFreeze(getpid());
    if (!check(first == 2 && second == 2 && service->removals.empty(),
               "remove-key-and-unknown-key-without-service-unregister")) return 1;
    manager->unRegisterSelfUnFreezeListener(&key2);
    service->latest()->onUnFreeze(getpid());
    if (!check(second == 2 && service->removals.empty(), "empty-registry-keeps-service-callback")) return 1;
    manager->registerSelfUnFreezeListener(nullptr, count, &first, false);
    service->latest()->onUnFreeze(getpid());
    if (!check(first == 3 && service->registrations.size() == 2 && !service->registrations.back().flag,
               "null-key-and-reregister-after-empty")) return 1;
    manager->unRegisterSelfUnFreezeListener(nullptr);
    manager->registerSelfUnFreezeListener(&key3, {}, nullptr, false);
    service->latest()->onUnFreeze(getpid());
    if (!check(service->registrations.size() == 3 && first == 3 && second == 2,
               "empty-callable-is-skipped")) return 1;
    manager->unRegisterSelfUnFreezeListener(&key3);
    sp<IdleCallback> direct = new IdleCallback;
    auto* thread = IPCThreadState::self();
    const int64_t original = thread->clearCallingIdentity();
    thread->restoreCallingIdentity((int64_t(10000) << 32) | uint32_t(getpid()));
    manager->registerUnFreezeListener(55, direct, true);
    thread->restoreCallingIdentity(original);
    if (!check(service->registrations.size() == 4 && service->registrations.back().pid == 55 &&
               service->registrations.back().callback == direct, "uid-10000-is-allowed")) return 1;
    const int64_t beforeHigh = thread->clearCallingIdentity();
    thread->restoreCallingIdentity((int64_t(10001) << 32) | uint32_t(getpid()));
    manager->registerUnFreezeListener(56, direct, false);
    manager->registerUnFreezeListenerInner(57, direct, false);
    thread->restoreCallingIdentity(beforeHigh);
    if (!check(service->registrations.size() == 5 && service->registrations.back().pid == 57,
               "uid-guard-and-inner-registration")) return 1;
    manager->unRegisterUnFreezeListener(55, direct);
    if (!check(service->removals.size() == 1 && service->removals.back().pid == 55 &&
               service->removals.back().callback == direct, "explicit-pid-unregister")) return 1;
    sp<picomisu::FreezeService> fresh = new picomisu::FreezeService;
    service->alive = false;
    fakeManager->service = IInterface::asBinder(fresh);
    auto reloaded = manager->getService();
    if (!check(reloaded && IInterface::asBinder(reloaded) == fakeManager->service && fakeManager->lookups == 2,
               "dead-service-reloads")) return 1;
    fresh->alive = false;
    fakeManager->service.clear();
    if (!check(manager->getService() == nullptr && fakeManager->lookups == 3,
               "unavailable-service-returns-null")) return 1;
    puts("freeze-registry-probe passed=15");
    return 0;
}
