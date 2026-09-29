// Copyright 2026 Picomisu contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <binder/IFreezeManager.h>
#include <binder/IServiceManager.h>
#include <vector>
namespace android::picomisu {
class FreezeService : public BnInterface<IFreezeManager> {
public:
    struct Entry { int pid; sp<IUnFreezeCallback> callback; bool flag; };
    std::vector<Entry> registrations, removals;
    bool alive = true;
    bool isBinderAlive() const override { return alive; }
    void registerUnFreezeListener(int pid, const sp<IUnFreezeCallback>& callback, bool flag) override {
        registrations.push_back({pid, callback, flag});
    }
    void unRegisterUnFreezeListener(int pid, const sp<IUnFreezeCallback>& callback) override {
        removals.push_back({pid, callback, false});
    }
    sp<IUnFreezeCallback> latest() const { return registrations.back().callback; }
};
class ServiceManager : public BnInterface<IServiceManager> {
public:
    sp<IBinder> service;
    mutable int lookups = 0;
    mutable bool wrongName = false;
    sp<IBinder> getService(const String16& name) const override {
        ++lookups;
        wrongName |= name != String16("freeze");
        return service;
    }
    sp<IBinder> checkService(const String16& name) const override { return getService(name); }
    status_t addService(const String16&, const sp<IBinder>&, bool, int) override { return INVALID_OPERATION; }
    Vector<String16> listServices(int) override { return {}; }
};
} // namespace android::picomisu
