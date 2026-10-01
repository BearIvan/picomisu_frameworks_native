/*
 * Copyright (C) 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ANDROID_SENSOR_LOOPER_H
#define ANDROID_SENSOR_LOOPER_H

#include <pthread.h>
#include <semaphore.h>

// Message thread of the factory PICO OS 5.13.7 libsensorservice.so (the NDK native-codec sample
// looper, attached to the Java VM as "sensorLoop"). SensorService uses it to apply the Smartisan
// uid freeze changes outside the binder thread that reports them.
struct loopermessage;

class SensorLooper {
public:
    SensorLooper();
    SensorLooper& operator=(const SensorLooper&) = delete;
    SensorLooper(SensorLooper&) = delete;
    virtual ~SensorLooper();

    void post(int what, void* data, bool flush = false);
    void quit();

    virtual void handle(int what, void* data);

private:
    void addmsg(loopermessage* msg, bool flush);
    static void* trampoline(void* p);
    void loop();

    loopermessage* head;
    pthread_t worker;
    sem_t headwriteprotect;
    sem_t headdataavailable;
    bool running;
};

#endif // ANDROID_SENSOR_LOOPER_H
