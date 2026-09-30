// Copyright 2026 Picomisu contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <binder/IFreezeManager.h>
#include <utils/Mutex.h>
#include <functional>
#include <map>
namespace android {
// Client of the PICO/Smartisan "freeze" service, reconstructed from the factory
// PICO OS 5.13.7 libbinder. The object layout (144 bytes on ARM64) and the nested
// record types match the factory library.
class FreezeManager {
public:
    // Remote registration key: caller-chosen key and the watched binder.
    struct KeyPair {
        const void* key;
        sp<IBinder> binder;
    };
    class RemoteDeathNotifier : public IBinder::DeathRecipient {
    public:
        RemoteDeathNotifier(int pid, FreezeManager* manager) : mPid(pid), mManager(manager) {}
        void binderDied(const wp<IBinder>& who) override;
    private:
        int mPid;
        FreezeManager* mManager;
    };
    struct CallBackData {
        std::function<void(const void*)> callback;
        void* argument;
        sp<RemoteDeathNotifier> notifier;
    };
    using CallBackMap = std::map<KeyPair*, CallBackData*>;
    using RemoteRegistry = std::map<int, CallBackMap>;
    // Process-wide unfreeze callback registered with the freeze service.
    class Callback : public BnUnFreezeCallback {
    public:
        void onUnFreeze(int pid) override;
        void unFreezeCallback(int pid, RemoteRegistry& registry);
    };

    FreezeManager();
    virtual ~FreezeManager();
    static FreezeManager* getInstance();
    sp<IFreezeManager> getService();
    void registerUnFreezeListener(int pid, const sp<IUnFreezeCallback>& callback, bool flag);
    void registerUnFreezeListenerInner(int pid, const sp<IUnFreezeCallback>& callback, bool flag);
    void unRegisterUnFreezeListener(int pid, const sp<IUnFreezeCallback>& callback);
    // Callbacks for the Binder peer that last replied "frozen" to this thread.
    void registerUnFreezeListener(void* key, const sp<IBinder>& binder,
                                  const std::function<void(const void*)>& callback,
                                  void* argument, bool once);
    void unRegisterUnFreezeListener(const void* key, const sp<IBinder>& binder);
    void unRegisterUnFreezeListener(const sp<IBinder>& binder);
    void unRegisterUnFreezeListener(int pid, const void* key, const sp<IBinder>& binder,
                                    RemoteRegistry& registry);
    void registerSelfUnFreezeListener(void* key, const std::function<void(const void*)>& callback,
                                     void* argument, bool flag);
    void unRegisterSelfUnFreezeListener(void* key);

private:
    static FreezeManager* instance;
    sp<IUnFreezeCallback> mCallback;
    Mutex mServiceMutex;
    sp<IFreezeManager> mService;
    int mPid;
    RemoteRegistry mOnceRegistry;
    RemoteRegistry mPersistentRegistry;
    std::map<void*, CallBackData*> mSelfRegistry;
};

// Registered key lookups used by the remote registry.
FreezeManager::KeyPair* containsKey(FreezeManager::CallBackMap* callbacks, const void* key,
                                    const sp<IBinder>& binder);
FreezeManager::KeyPair* containsKey(FreezeManager::CallBackMap* callbacks,
                                    const sp<IBinder>& binder);
} // namespace android
