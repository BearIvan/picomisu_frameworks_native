// Copyright 2026 Picomisu contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <binder/IFreezeManager.h>
#include <utils/Mutex.h>
#include <functional>
#include <map>
#include <memory>
namespace android {
class FreezeManager {
public:
    FreezeManager();
    virtual ~FreezeManager();
    static FreezeManager* getInstance();
    sp<IFreezeManager> getService();
    void registerUnFreezeListener(int pid, const sp<IUnFreezeCallback>& callback, bool flag);
    void registerUnFreezeListenerInner(int pid, const sp<IUnFreezeCallback>& callback, bool flag);
    void unRegisterUnFreezeListener(int pid, const sp<IUnFreezeCallback>& callback);
    void registerSelfUnFreezeListener(void* key, const std::function<void(const void*)>& callback,
                                     void* argument, bool flag);
    void unRegisterSelfUnFreezeListener(void* key);
private:
    class Callback;
    struct KeyPair;
    struct CallBackData;
    // The factory prefix offsets are retained. Remote registry behavior and
    // IPCThreadState frozen-PID support are a separate, unfinished port.
    sp<IUnFreezeCallback> mCallback;
    Mutex mServiceMutex;
    sp<IFreezeManager> mService;
    int mPid;
    using RemoteRegistry = std::map<int, std::map<KeyPair*, std::shared_ptr<CallBackData>>>;
    RemoteRegistry mOnceRegistry;
    RemoteRegistry mPersistentRegistry;
    std::map<void*, std::shared_ptr<CallBackData>> mSelfRegistry;
    static FreezeManager* instance;
};
} // namespace android
