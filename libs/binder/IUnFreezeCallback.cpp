// Copyright 2026 Picomisu contributors
// SPDX-License-Identifier: Apache-2.0
#include <binder/IUnFreezeCallback.h>
#include <binder/Parcel.h>
namespace android {
class BpUnFreezeCallback : public BpInterface<IUnFreezeCallback> {
public:
    explicit BpUnFreezeCallback(const sp<IBinder>& remote) : BpInterface<IUnFreezeCallback>(remote) {}
    void onUnFreeze(int pid) override {
        Parcel data, reply;
        data.writeInterfaceToken(IUnFreezeCallback::getInterfaceDescriptor());
        data.writeInt32(pid);
        remote()->transact(ON_UNFREEZE, data, &reply, IBinder::FLAG_ONEWAY);
    }
};
IMPLEMENT_META_INTERFACE(UnFreezeCallback, "android.app.IUnFreezeCallback");
status_t BnUnFreezeCallback::onTransact(uint32_t code, const Parcel& data, Parcel* reply,
                                     uint32_t flags) {
    if (code != ON_UNFREEZE) return BBinder::onTransact(code, data, reply, flags);
    CHECK_INTERFACE(IUnFreezeCallback, data, reply);
    // Factory uses the legacy readInt32() behavior, including zero for a short payload.
    onUnFreeze(data.readInt32());
    return NO_ERROR;
}
} // namespace android
