// Copyright 2026 Picomisu contributors
// SPDX-License-Identifier: Apache-2.0
#include <binder/SoundSettings.h>
#include <binder/IPCThreadState.h>
#include <log/log.h>
namespace android {
namespace {
pthread_mutex_t* newMutex() {
    pthread_mutex_t* mutex = new pthread_mutex_t;
    pthread_mutex_init(mutex, nullptr);
    return mutex;
}
} // namespace
std::mutex mtxSound;
pthread_mutex_t* mMutexSound = newMutex();
SoundSettings* SoundSettings::instance = nullptr;
SoundSettings::SoundSettings() : mPrefetch(false) {
    pthread_mutex_init(&mLock, nullptr);
}
SoundSettings::~SoundSettings() {
    pthread_mutex_destroy(&mLock);
}
SoundSettings* SoundSettings::getInstance() {
    if (instance == nullptr) {
        std::lock_guard<std::mutex> lock(mtxSound);
        if (instance == nullptr) instance = new SoundSettings();
    }
    return instance;
}
void SoundSettings::initPrefetch(bool prefetch) {
    mPrefetch = prefetch;
}
void SoundSettings::registerTrackMuteListener(int trackId, ISoundCallback* callback) {
    // Only application processes register (factory: calling uid >= 10000).
    if (static_cast<int>(IPCThreadState::self()->getCallingUid()) < 10000) return;
    pthread_mutex_lock(mMutexSound);
    SoundSettings* settings = instance;
    __android_log_print(ANDROID_LOG_INFO, nullptr, "register trackId = %d ", trackId);
    settings->mCallbacks.insert(std::pair<int, ISoundCallback*>(trackId, callback));
    callback->onSoundCallback(mPrefetch, true);
    pthread_mutex_unlock(mMutexSound);
}
void SoundSettings::unRregisterTrackMuteListener(int trackId) {
    pthread_mutex_lock(mMutexSound);
    auto& callbacks = instance->mCallbacks;
    auto entry = callbacks.find(trackId);
    if (entry != callbacks.end()) callbacks.erase(trackId);
    pthread_mutex_unlock(mMutexSound);
}
void SoundSettings::onSoundCallback(bool prefetch) {
    pthread_mutex_lock(mMutexSound);
    mPrefetch = prefetch;
    for (const auto& entry : instance->mCallbacks) {
        if (entry.second != nullptr) entry.second->onSoundCallback(prefetch, false);
    }
    pthread_mutex_unlock(mMutexSound);
}
} // namespace android
