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

#include "SensorLooper.h"

#include <android/log.h>
#include <android_runtime/AndroidRuntime.h>
#include <jni.h>
#include <stddef.h>

// The factory keeps the tag of the NDK sample this looper comes from.
#define TAG "NativeCodec-looper"
#define LOGV(...) __android_log_print(ANDROID_LOG_VERBOSE, TAG, __VA_ARGS__)

struct loopermessage {
    int what;
    void* obj;
    loopermessage* next;
    bool quit;
};

void* SensorLooper::trampoline(void* p) {
    JNIEnv* env;
    android::AndroidRuntime::javaAttachThread("sensorLoop", &env);
    ((SensorLooper*)p)->loop();
    return NULL;
}

SensorLooper::SensorLooper() {
    sem_init(&headdataavailable, 0, 0);
    sem_init(&headwriteprotect, 0, 1);
    pthread_attr_t attr;
    pthread_attr_init(&attr);

    pthread_create(&worker, &attr, trampoline, this);
    running = true;
    head = NULL;
}

SensorLooper::~SensorLooper() {
    if (running) {
        LOGV("Looper deleted while still running. Some messages will not be processed");
        quit();
    }
}

void SensorLooper::post(int what, void* data, bool flush) {
    loopermessage* msg = new loopermessage();
    msg->what = what;
    msg->obj = data;
    msg->next = NULL;
    msg->quit = false;
    addmsg(msg, flush);
}

void SensorLooper::addmsg(loopermessage* msg, bool flush) {
    sem_wait(&headwriteprotect);
    loopermessage* h = head;

    if (flush) {
        while (h) {
            loopermessage* next = h->next;
            delete h;
            h = next;
        }
        h = NULL;
    }
    if (h) {
        while (h->next) {
            h = h->next;
        }
        h->next = msg;
    } else {
        head = msg;
    }
    LOGV("post msg %d", msg->what);
    sem_post(&headwriteprotect);
    sem_post(&headdataavailable);
}

void SensorLooper::loop() {
    while (true) {
        // wait for available message
        sem_wait(&headdataavailable);

        // get next available message
        sem_wait(&headwriteprotect);
        loopermessage* msg = head;
        if (msg == NULL) {
            LOGV("no msg");
            sem_post(&headwriteprotect);
            continue;
        }
        head = msg->next;
        sem_post(&headwriteprotect);

        if (msg->quit) {
            LOGV("quitting");
            delete msg;
            return;
        }
        LOGV("processing msg %d", msg->what);
        handle(msg->what, msg->obj);
        delete msg;
    }
}

void SensorLooper::quit() {
    LOGV("quit");
    loopermessage* msg = new loopermessage();
    msg->what = 0;
    msg->obj = NULL;
    msg->next = NULL;
    msg->quit = true;
    addmsg(msg, false);
    void* retval;
    pthread_join(worker, &retval);
    sem_destroy(&headdataavailable);
    sem_destroy(&headwriteprotect);
    running = false;
}

void SensorLooper::handle(int what, void* obj) {
    LOGV("dropping msg %d %p", what, obj);
}
