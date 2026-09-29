// Copyright 2026 Picomisu contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <binder/IInterface.h>
namespace android {
class IUnFreezeCallback : public IInterface {
public:
    DECLARE_META_INTERFACE(UnFreezeCallback)
    virtual void onUnFreeze(int pid) = 0;
    enum { ON_UNFREEZE = IBinder::FIRST_CALL_TRANSACTION };
};
class BnUnFreezeCallback : public BnInterface<IUnFreezeCallback> {
public:
    status_t onTransact(uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags) override;
};
} // namespace android
