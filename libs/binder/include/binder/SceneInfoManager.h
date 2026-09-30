// Copyright 2026 Picomisu contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <binder/ISceneInfoManager.h>
#include <utils/Mutex.h>
namespace android {
// Native client singleton of the PICO "sceneinfo_service", reconstructed from the
// factory PICO OS 5.13.7 libbinder (used by /system/bin/pxrmediametrics).
class SceneInfoManager {
public:
    class DeathNotifier : public IBinder::DeathRecipient {
    public:
        DeathNotifier();
        void binderDied(const wp<IBinder>& who) override;
    };

    SceneInfoManager();
    virtual ~SceneInfoManager();
    static SceneInfoManager* getInstance();
    sp<ISceneInfoManager> getService();
    bool reportDataInfo(int type, int subType, SceneData data);
    void serverDied();

private:
    static SceneInfoManager* instance;
    Mutex mLock;
    sp<IBinder> mBinder;
    sp<ISceneInfoManager> mService;
    sp<DeathNotifier> mDeathNotifier;
};
} // namespace android
