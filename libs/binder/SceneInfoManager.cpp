// Copyright 2026 Picomisu contributors
// SPDX-License-Identifier: Apache-2.0
// Reconstructed from the factory PICO OS 5.13.7 libbinder (SceneInfoManager.cpp).
#include <binder/SceneInfoManager.h>
#include <binder/IServiceManager.h>
#include <utils/Log.h>
#include <mutex>
namespace android {

std::mutex scenemtx;
Mutex* mSceneMutex = new Mutex();

SceneInfoManager* SceneInfoManager::instance = nullptr;

SceneInfoManager::SceneInfoManager() {
    mDeathNotifier = new DeathNotifier();
}

SceneInfoManager::DeathNotifier::DeathNotifier() {
}

sp<ISceneInfoManager> SceneInfoManager::getService() {
    Mutex::Autolock _l(mLock);
    sp<ISceneInfoManager> service = mService;
    while (service == nullptr || !IInterface::asBinder(service)->isBinderAlive()) {
        mBinder = defaultServiceManager()->checkService(String16("sceneinfo_service"));
        if (mBinder == nullptr) {
            ALOGW("Waiting too long for sceneinfo_service service, giving up");
            service = nullptr;
            return service;
        }
        mBinder->linkToDeath(mDeathNotifier);
        service = interface_cast<ISceneInfoManager>(mBinder);
        mService = service;
    }
    return service;
}

SceneInfoManager* SceneInfoManager::getInstance() {
    if (instance == nullptr) {
        scenemtx.lock();
        if (instance == nullptr) {
            instance = new SceneInfoManager();
        }
        scenemtx.unlock();
    }
    return instance;
}

bool SceneInfoManager::reportDataInfo(int type, int subType, SceneData data) {
    Mutex::Autolock _l(mSceneMutex);
    sp<ISceneInfoManager> service = getService();
    if (service == nullptr) {
        return false;
    }
    return service->reportDataInfo(type, subType, data);
}

void SceneInfoManager::serverDied() {
    Mutex::Autolock _l(mLock);
    if (mBinder != nullptr && mBinder->isBinderAlive()) {
        mBinder->unlinkToDeath(mDeathNotifier);
    }
    sp<ISceneInfoManager> service = mService;
    while (service == nullptr || !IInterface::asBinder(service)->isBinderAlive()) {
        mBinder = defaultServiceManager()->checkService(String16("sceneinfo_service"));
        if (mBinder == nullptr) {
            ALOGW("Waiting too long for sceneinfo_service service, giving up");
            service = nullptr;
            return;
        }
        mBinder->linkToDeath(mDeathNotifier);
        service = interface_cast<ISceneInfoManager>(mBinder);
        mService = service;
    }
}

SceneInfoManager::~SceneInfoManager() {
    if (mBinder != nullptr && mBinder->isBinderAlive()) {
        mBinder->unlinkToDeath(mDeathNotifier);
    }
}

void SceneInfoManager::DeathNotifier::binderDied(const wp<IBinder>& /*who*/) {
    ALOGE("SceneInfoManager binder service died.");
    SceneInfoManager::getInstance()->serverDied();
}

} // namespace android
