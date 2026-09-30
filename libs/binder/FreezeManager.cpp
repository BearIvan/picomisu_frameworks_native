// Copyright 2026 Picomisu contributors
// SPDX-License-Identifier: Apache-2.0
// Reconstructed from the factory PICO OS 5.13.7 libbinder (FreezeManager.cpp).
// Factory behaviour is kept as is, including:
//  - callbacks run while the global registry lock is held (a callback that
//    registers or unregisters on the same thread deadlocks);
//  - KeyPair and CallBackData records are never freed;
//  - remote registrations are added to a local copy of the registry, so the
//    remote registries stay empty: only the service registration and the
//    death link take effect, and remote callbacks are never delivered.
#include <binder/FreezeManager.h>
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <utils/Log.h>
#include <mutex>
#include <unistd.h>
namespace android {

std::mutex mtx;
Mutex* mMutex = new Mutex();

FreezeManager* FreezeManager::instance = nullptr;

FreezeManager::FreezeManager() {
    mCallback = new Callback();
    mPid = getpid();
}

sp<IFreezeManager> FreezeManager::getService() {
    Mutex::Autolock _l(mServiceMutex);
    sp<IFreezeManager> service = mService;
    while (service == nullptr || !IInterface::asBinder(service)->isBinderAlive()) {
        sp<IBinder> binder = defaultServiceManager()->checkService(String16("freeze"));
        if (binder == nullptr) {
            ALOGW("Waiting too long for freeze service, giving up");
            service = nullptr;
            return service;
        }
        service = interface_cast<IFreezeManager>(binder);
        mService = service;
    }
    return service;
}

FreezeManager* FreezeManager::getInstance() {
    if (instance == nullptr) {
        mtx.lock();
        if (instance == nullptr) {
            instance = new FreezeManager();
        }
        mtx.unlock();
    }
    return instance;
}

void FreezeManager::registerUnFreezeListener(int pid, const sp<IUnFreezeCallback>& callback,
                                             bool flag) {
    if (static_cast<int32_t>(IPCThreadState::self()->getCallingUid()) > 10000) return;
    sp<IFreezeManager> service = getService();
    if (service != nullptr) {
        service->registerUnFreezeListener(pid, callback, flag);
    }
}

void FreezeManager::registerUnFreezeListenerInner(int pid, const sp<IUnFreezeCallback>& callback,
                                                  bool flag) {
    sp<IFreezeManager> service = getService();
    if (service != nullptr) {
        service->registerUnFreezeListener(pid, callback, flag);
    }
}

void FreezeManager::unRegisterUnFreezeListener(int pid, const sp<IUnFreezeCallback>& callback) {
    sp<IFreezeManager> service = getService();
    if (service != nullptr) {
        service->unRegisterUnFreezeListener(pid, callback);
    }
}

FreezeManager::KeyPair* containsKey(FreezeManager::CallBackMap* callbacks, const void* key,
                                    const sp<IBinder>& binder) {
    for (auto it = callbacks->begin(); it != callbacks->end(); ++it) {
        FreezeManager::KeyPair* keyPair = it->first;
        if (keyPair->key == key && keyPair->binder == binder) {
            return keyPair;
        }
    }
    return nullptr;
}

FreezeManager::KeyPair* containsKey(FreezeManager::CallBackMap* callbacks,
                                    const sp<IBinder>& binder) {
    for (auto it = callbacks->begin(); it != callbacks->end(); ++it) {
        FreezeManager::KeyPair* keyPair = it->first;
        if (keyPair->binder == binder) {
            return keyPair;
        }
    }
    return nullptr;
}

void FreezeManager::unRegisterUnFreezeListener(int pid, const void* key,
                                               const sp<IBinder>& binder,
                                               RemoteRegistry& registry) {
    Mutex::Autolock _l(mMutex);
    CallBackMap callbacks;
    auto entry = registry.find(pid);
    if (entry == registry.end()) {
        return;
    }
    callbacks = CallBackMap(entry->second);
    KeyPair* keyPair = key != nullptr ? containsKey(&callbacks, key, binder)
                                      : containsKey(&callbacks, binder);
    auto found = callbacks.find(keyPair);
    if (found == callbacks.end()) {
        return;
    }
    binder->unlinkToDeath(found->second->notifier);
    callbacks.erase(keyPair);
    if (callbacks.size() != 0) {
        return;
    }
    sp<IFreezeManager> service = getService();
    if (service != nullptr) {
        service->unRegisterUnFreezeListener(pid, mCallback);
    }
    auto remaining = registry.find(pid);
    if (remaining != registry.end()) {
        registry.erase(remaining);
    }
}

void FreezeManager::RemoteDeathNotifier::binderDied(const wp<IBinder>& who) {
    ALOGW("RemoteDeathNotifier binderDied");
    sp<IBinder> binder = who.promote();
    mManager->unRegisterUnFreezeListener(mPid, nullptr, binder, mManager->mOnceRegistry);
    mManager->unRegisterUnFreezeListener(mPid, nullptr, binder, mManager->mPersistentRegistry);
}

void FreezeManager::registerSelfUnFreezeListener(void* key,
                                                const std::function<void(const void*)>& callback,
                                                void* argument, bool flag) {
    Mutex::Autolock _l(mMutex);
    if (mSelfRegistry.size() == 0) {
        sp<IFreezeManager> service = getService();
        if (service != nullptr) {
            service->registerUnFreezeListener(mPid, mCallback, flag);
        }
    }
    sp<RemoteDeathNotifier> notifier = new RemoteDeathNotifier(mPid, instance);
    CallBackData* data = new CallBackData{callback, argument, notifier};
    // A repeated key keeps the first record; the new one is not freed.
    mSelfRegistry.insert(std::pair<void*, CallBackData*>(key, data));
}

void FreezeManager::unRegisterSelfUnFreezeListener(void* key) {
    Mutex::Autolock _l(mMutex);
    if (mSelfRegistry.size() != 0 && mSelfRegistry.find(key) != mSelfRegistry.end()) {
        mSelfRegistry.erase(key);
    }
}

void FreezeManager::registerUnFreezeListener(void* key, const sp<IBinder>& binder,
                                             const std::function<void(const void*)>& callback,
                                             void* argument, bool once) {
    int pid = IPCThreadState::self()->getLastFrozenPid();
    Mutex::Autolock _l(mMutex);
    CallBackMap callbacks;
    RemoteRegistry registry;
    if (once) {
        registry = instance->mOnceRegistry;
    } else {
        registry = instance->mPersistentRegistry;
    }
    auto entry = registry.find(pid);
    if (entry == registry.end()) {
        sp<IFreezeManager> service = getService();
        if (service != nullptr) {
            service->registerUnFreezeListener(pid, mCallback, once);
        }
    } else {
        callbacks = CallBackMap(entry->second);
    }
    sp<RemoteDeathNotifier> notifier = new RemoteDeathNotifier(pid, instance);
    CallBackData* data = new CallBackData{callback, argument, notifier};
    KeyPair* keyPair = containsKey(&callbacks, key, binder);
    if (keyPair == nullptr) {
        keyPair = new KeyPair;
        keyPair->key = key;
        keyPair->binder = binder;
    }
    callbacks.insert(std::pair<KeyPair*, CallBackData*>(keyPair, data));
    registry.insert(std::pair<int, CallBackMap>(pid, callbacks));
    binder->linkToDeath(notifier);
}

void FreezeManager::unRegisterUnFreezeListener(const void* key, const sp<IBinder>& binder) {
    int pid = IPCThreadState::self()->getLastFrozenPid();
    unRegisterUnFreezeListener(pid, key, binder, instance->mOnceRegistry);
    unRegisterUnFreezeListener(pid, key, binder, instance->mPersistentRegistry);
}

void FreezeManager::unRegisterUnFreezeListener(const sp<IBinder>& binder) {
    int pid = IPCThreadState::self()->getLastFrozenPid();
    unRegisterUnFreezeListener(pid, nullptr, binder, instance->mOnceRegistry);
    unRegisterUnFreezeListener(pid, nullptr, binder, instance->mPersistentRegistry);
}

void FreezeManager::Callback::unFreezeCallback(int pid, RemoteRegistry& registry) {
    auto entry = registry.find(pid);
    if (entry == registry.end()) {
        return;
    }
    CallBackMap callbacks(entry->second);
    for (auto it = callbacks.begin(); it != callbacks.end(); ++it) {
        CallBackData* data = it->second;
        sp<IBinder> binder = it->first->binder;
        binder->unlinkToDeath(data->notifier);
        std::function<void(const void*)> function = data->callback;
        if (function) {
            function(data->argument);
        }
    }
}

void FreezeManager::Callback::onUnFreeze(int pid) {
    Mutex::Autolock _l(mMutex);
    if (instance->mPid == pid) {
        for (auto it = instance->mSelfRegistry.begin(); it != instance->mSelfRegistry.end();
             ++it) {
            CallBackData* data = it->second;
            std::function<void(const void*)> function = data->callback;
            if (function) {
                function(data->argument);
            }
        }
        return;
    }
    unFreezeCallback(pid, instance->mPersistentRegistry);
    unFreezeCallback(pid, instance->mOnceRegistry);
    auto entry = instance->mOnceRegistry.find(pid);
    if (entry != instance->mOnceRegistry.end()) {
        instance->mOnceRegistry.erase(entry);
    }
}

FreezeManager::~FreezeManager() = default;

} // namespace android
