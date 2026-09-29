// Copyright 2026 Picomisu contributors
// SPDX-License-Identifier: Apache-2.0
#include <binder/FreezeManager.h>
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <mutex>
#include <unistd.h>
#include <vector>
namespace android {
namespace {
// Factory serializes self callbacks with unregister under its singleton lock.
// Recursive locking additionally permits a callback to remove itself safely.
std::recursive_mutex sRegistryMutex;
}
FreezeManager* FreezeManager::instance = nullptr;
struct FreezeManager::CallBackData {
    std::function<void(const void*)> callback;
    void* argument;
    sp<IBinder::DeathRecipient> deathRecipient;
};
class FreezeManager::Callback : public BnUnFreezeCallback {
public:
    void onUnFreeze(int pid) override {
        std::lock_guard<std::recursive_mutex> lock(sRegistryMutex);
        FreezeManager* manager = FreezeManager::getInstance();
        if (pid != manager->mPid) return;
        // Shared snapshots keep callable objects alive during self removal;
        // another removed record is skipped before its invocation.
        std::vector<std::pair<void*, std::shared_ptr<CallBackData>>> entries(
                manager->mSelfRegistry.begin(), manager->mSelfRegistry.end());
        for (const auto& entry : entries) {
            auto current = manager->mSelfRegistry.find(entry.first);
            if (current == manager->mSelfRegistry.end() || current->second != entry.second) continue;
            if (entry.second->callback) entry.second->callback(entry.second->argument);
        }
    }
};
FreezeManager::FreezeManager() : mCallback(new Callback), mPid(getpid()) {}
FreezeManager::~FreezeManager() = default;
FreezeManager* FreezeManager::getInstance() {
    std::lock_guard<std::recursive_mutex> lock(sRegistryMutex);
    if (!instance) instance = new FreezeManager;
    return instance;
}
sp<IFreezeManager> FreezeManager::getService() {
    Mutex::Autolock lock(mServiceMutex);
    sp<IFreezeManager> service = mService;
    while (!service || !IInterface::asBinder(service)->isBinderAlive()) {
        const sp<IServiceManager> manager = defaultServiceManager();
        const sp<IBinder> binder = manager ? manager->getService(String16("freeze")) : nullptr;
        if (!binder) return nullptr;
        service = interface_cast<IFreezeManager>(binder);
        mService = service;
    }
    return service;
}
void FreezeManager::registerUnFreezeListener(int pid, const sp<IUnFreezeCallback>& callback, bool flag) {
    if (static_cast<int32_t>(IPCThreadState::self()->getCallingUid()) > 10000) return;
    registerUnFreezeListenerInner(pid, callback, flag);
}
void FreezeManager::registerUnFreezeListenerInner(int pid, const sp<IUnFreezeCallback>& callback, bool flag) {
    const sp<IFreezeManager> service = getService();
    if (service) service->registerUnFreezeListener(pid, callback, flag);
}
void FreezeManager::unRegisterUnFreezeListener(int pid, const sp<IUnFreezeCallback>& callback) {
    const sp<IFreezeManager> service = getService();
    if (service) service->unRegisterUnFreezeListener(pid, callback);
}
void FreezeManager::registerSelfUnFreezeListener(void* key,
                                                const std::function<void(const void*)>& callback,
                                                void* argument, bool flag) {
    std::lock_guard<std::recursive_mutex> lock(sRegistryMutex);
    if (mSelfRegistry.empty()) {
        const sp<IFreezeManager> service = getService();
        if (service) service->registerUnFreezeListener(mPid, mCallback, flag);
    }
    // Factory keeps the first registration for a repeated key.
    if (mSelfRegistry.count(key)) return;
    mSelfRegistry.emplace(key, std::make_shared<CallBackData>(CallBackData{callback, argument, nullptr}));
}
void FreezeManager::unRegisterSelfUnFreezeListener(void* key) {
    std::lock_guard<std::recursive_mutex> lock(sRegistryMutex);
    mSelfRegistry.erase(key);
    // Factory leaves its process callback registered even when the registry is empty.
}
} // namespace android
