// Copyright 2026 Picomisu contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <pthread.h>
#include <map>
#include <mutex>
namespace android {
// Receiver of the prefetch (pre-started process) sound mute state.
class ISoundCallback {
public:
    // mute: the process is still a prefetched process; registered: called on registration.
    virtual void onSoundCallback(bool mute, bool registered) = 0;
protected:
    // Not deleted through this interface; the factory vtable starts with onSoundCallback.
    ~ISoundCallback() = default;
};
// Per-process prefetch state and track mute listeners, reconstructed from the factory
// PICO OS 5.13.7 libbinder. ActivityThreadSmtBase sets the state through JNI
// (initPrefetch at bind, onSoundCallback when the prefetched process really starts).
class SoundSettings {
public:
    SoundSettings();
    virtual ~SoundSettings();
    static SoundSettings* getInstance();
    void initPrefetch(bool prefetch);
    void registerTrackMuteListener(int trackId, ISoundCallback* callback);
    void unRregisterTrackMuteListener(int trackId);
    void onSoundCallback(bool prefetch);
    static SoundSettings* instance;
private:
    pthread_mutex_t mLock;
    bool mPrefetch;
    std::map<int, ISoundCallback*> mCallbacks;
};
// Factory globals: the singleton creation lock and the listener lock.
extern std::mutex mtxSound;
extern pthread_mutex_t* mMutexSound;
} // namespace android
