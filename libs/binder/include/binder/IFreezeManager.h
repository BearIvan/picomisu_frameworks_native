// Copyright 2026 Picomisu contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <binder/IInterface.h>
#include <binder/IUnFreezeCallback.h>
namespace android {
class IFreezeManager : public IInterface {
public:
    DECLARE_META_INTERFACE(FreezeManager)
    virtual void registerUnFreezeListener(int pid, const sp<IUnFreezeCallback>& callback,
                                          bool flag) = 0;
    virtual void unRegisterUnFreezeListener(int pid, const sp<IUnFreezeCallback>& callback) = 0;
    enum { REGISTER = IBinder::FIRST_CALL_TRANSACTION, UNREGISTER };
};
} // namespace android
