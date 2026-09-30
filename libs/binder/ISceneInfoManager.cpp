// Copyright 2026 Picomisu contributors
// SPDX-License-Identifier: Apache-2.0
// Reconstructed from the factory PICO OS 5.13.7 libbinder (ISceneInfoManager.cpp).
#include <binder/ISceneInfoManager.h>
#include <binder/Parcel.h>
#include <utils/Log.h>
namespace android {
class BpSceneInfoManager : public BpInterface<ISceneInfoManager> {
public:
    explicit BpSceneInfoManager(const sp<IBinder>& impl) : BpInterface<ISceneInfoManager>(impl) {}
    bool reportDataInfo(int type, int subType, SceneData sceneData) override {
        Parcel data, reply;
        data.writeInterfaceToken(ISceneInfoManager::getInterfaceDescriptor());
        data.writeInt32(type);
        data.writeInt32(subType);
        if (sceneData.writeToParcel(data) != NO_ERROR) {
            ALOGE("reportDataInfo fail, sceneData err");
            return false;
        }
        remote()->transact(REPORT_DATA_INFO, data, &reply, 0);
        reply.readExceptionCode();
        return reply.readInt32() != 0;
    }
};

IMPLEMENT_META_INTERFACE(SceneInfoManager, "android.app.ISceneInfoManager");
} // namespace android
