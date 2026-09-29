// Copyright 2026 Picomisu contributors
// SPDX-License-Identifier: Apache-2.0
#include <binder/IFreezeManager.h>
#include <binder/Parcel.h>
namespace android {
class BpFreezeManager : public BpInterface<IFreezeManager> {
public:
    explicit BpFreezeManager(const sp<IBinder>& remote) : BpInterface<IFreezeManager>(remote) {}
    void registerUnFreezeListener(int pid, const sp<IUnFreezeCallback>& callback, bool flag) override {
        Parcel data, reply;
        data.writeInterfaceToken(IFreezeManager::getInterfaceDescriptor());
        data.writeInt32(pid);
        data.writeStrongBinder(IInterface::asBinder(callback));
        data.writeBool(flag);
        remote()->transact(REGISTER, data, &reply, 0);
    }
    void unRegisterUnFreezeListener(int pid, const sp<IUnFreezeCallback>& callback) override {
        Parcel data, reply;
        data.writeInterfaceToken(IFreezeManager::getInterfaceDescriptor());
        data.writeInt32(pid);
        data.writeStrongBinder(IInterface::asBinder(callback));
        remote()->transact(UNREGISTER, data, &reply, 0);
    }
};
IMPLEMENT_META_INTERFACE(FreezeManager, "android.app.IFreezeManager");
} // namespace android
