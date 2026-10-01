/*
 * Copyright 2026 The Picomisu Project
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

#pragma once

// PICO: the PICO system monitor client of the factory PICO OS 5.13.7 SurfaceFlinger.
//
// The factory libsurfaceflinger links libsysperftracker.so and reports to
// mtp::SysMtpClient::getInstance(): every hardware vsync (onVsyncReceived), every composed
// frame of each display with the producer frames it showed (handleMessageRefresh), and the
// sysmonitor binder codes 2002..2016 of SurfaceFlinger::onTransact. SysMtpClient is the
// on-device client of the "sysmtpserver" binder service (shared memory, protobuf files); it
// does not talk to the network itself. libsysperftracker.so is a factory prebuilt carried on
// the image (/system/lib64) and is not part of the source tree, so SurfaceFlinger binds it at
// run time: the exported SysMtpClient members are resolved once with dlsym() by their factory
// mangled names and every call is a no-op when the library or a symbol is missing.
//
// The class deliberately does not use the factory names (mtp::SysMtpClient): inline members
// of a class with those names would be exported by libsurfaceflinger and could interpose the
// real ones inside libsysperftracker.

#include <dlfcn.h>
#include <stdint.h>

#include <string>
#include <vector>

#include <gui/SurfaceClient.h>
#include <log/log.h>

namespace android {
namespace pico {

class SysMtpClient {
public:
    // mtp::SysMtpClient::getInstance()->onVsync(timestamp)
    static void onVsync(int64_t timestamp) {
        const Api& api = get();
        if (api.onVsync != nullptr) api.onVsync(api.instance(), timestamp);
    }
    // mtp::SysMtpClient::addDisplayFrame(frames, layerStack, lastComposeTime, composeTime,
    //                                    queueStartTime, queueEndTime)
    static void addDisplayFrame(std::vector<SurfaceClientItem> frames, int layerStack,
                                int64_t lastComposeTime, int64_t composeTime,
                                int64_t queueStartTime, int64_t queueEndTime) {
        const Api& api = get();
        if (api.addDisplayFrame != nullptr) {
            api.addDisplayFrame(api.instance(), std::move(frames), layerStack, lastComposeTime,
                                composeTime, queueStartTime, queueEndTime);
        }
    }
    static void sendTaskToWritePb() { call(get().sendTaskToWritePb); }
    static void sendTaskToWritePtpPb() { call(get().sendTaskToWritePtpPb); }
    static void setDeviceProp(std::string a, std::string b, std::string c) {
        const Api& api = get();
        if (api.setDeviceProp != nullptr) {
            api.setDeviceProp(api.instance(), std::move(a), std::move(b), std::move(c));
        }
    }
    static void shutDown() { call(get().shutDown); }
    static void notifyDisplayRefresh(int value) { call(get().notifyDisplayRefresh, value); }
    static void notifyAutoDumpInfo(std::string name, std::vector<int> values) {
        callStringVector(get().notifyAutoDumpInfo, std::move(name), std::move(values));
    }
    static void setDailyDumpPerfettoCount(int count) {
        call(get().setDailyDumpPerfettoCount, count);
    }
    static void notifyCrashReportDumpInfo(std::string name, std::vector<int> values) {
        callStringVector(get().notifyCrashReportDumpInfo, std::move(name), std::move(values));
    }
    static void notifyBacklight(int a, int b, int c) {
        const Api& api = get();
        if (api.notifyBacklight != nullptr) api.notifyBacklight(api.instance(), a, b, c);
    }
    static void notifyLowPowerLevel(int level) { call(get().notifyLowPowerLevel, level); }
    static void updateTerribleJankScope(int scope) {
        call(get().updateTerribleJankScope, scope);
    }
    static void updateCameraRefresh(int value) { call(get().updateCameraRefresh, value); }
    static void notifyVirtualDisplaySurfaceChanged(int value, std::string name) {
        const Api& api = get();
        if (api.notifyVirtualDisplaySurfaceChanged != nullptr) {
            api.notifyVirtualDisplaySurfaceChanged(api.instance(), value, std::move(name));
        }
    }
    static void notifyLaunchPackageInfo(std::string a, std::string b, int value, int64_t time) {
        const Api& api = get();
        if (api.notifyLaunchPackageInfo != nullptr) {
            api.notifyLaunchPackageInfo(api.instance(), std::move(a), std::move(b), value, time);
        }
    }
    static void notifySchedInfoDumpInfo(std::vector<int> values) {
        const Api& api = get();
        if (api.notifySchedInfoDumpInfo != nullptr) {
            api.notifySchedInfoDumpInfo(api.instance(), std::move(values));
        }
    }
    static void notifyLayerDumpInfo(std::string name, std::vector<int> values) {
        callStringVector(get().notifyLayerDumpInfo, std::move(name), std::move(values));
    }

private:
    // mtp::SysMtpClient* mtp::SysMtpClient::getInstance() and the (non virtual) members,
    // called with the client as first (this) argument. Class-type parameters are passed by
    // value exactly like the factory declarations (Itanium ABI: caller-owned temporaries).
    struct Api {
        void* (*getInstance)() = nullptr;
        void (*onVsync)(void*, long) = nullptr;
        void (*addDisplayFrame)(void*, std::vector<SurfaceClientItem>, int, long, long, long,
                                long) = nullptr;
        void (*sendTaskToWritePb)(void*) = nullptr;
        void (*sendTaskToWritePtpPb)(void*) = nullptr;
        void (*setDeviceProp)(void*, std::string, std::string, std::string) = nullptr;
        void (*shutDown)(void*) = nullptr;
        void (*notifyDisplayRefresh)(void*, int) = nullptr;
        void (*notifyAutoDumpInfo)(void*, std::string, std::vector<int>) = nullptr;
        void (*setDailyDumpPerfettoCount)(void*, int) = nullptr;
        void (*notifyCrashReportDumpInfo)(void*, std::string, std::vector<int>) = nullptr;
        void (*notifyBacklight)(void*, int, int, int) = nullptr;
        void (*notifyLowPowerLevel)(void*, int) = nullptr;
        void (*updateTerribleJankScope)(void*, int) = nullptr;
        void (*updateCameraRefresh)(void*, int) = nullptr;
        void (*notifyVirtualDisplaySurfaceChanged)(void*, int, std::string) = nullptr;
        void (*notifyLaunchPackageInfo)(void*, std::string, std::string, int, long) = nullptr;
        void (*notifySchedInfoDumpInfo)(void*, std::vector<int>) = nullptr;
        void (*notifyLayerDumpInfo)(void*, std::string, std::vector<int>) = nullptr;
        void* instance() const { return getInstance(); }
    };

    static void call(void (*fn)(void*)) {
        if (fn != nullptr) fn(get().instance());
    }
    static void call(void (*fn)(void*, int), int value) {
        if (fn != nullptr) fn(get().instance(), value);
    }
    static void callStringVector(void (*fn)(void*, std::string, std::vector<int>),
                                 std::string name, std::vector<int> values) {
        if (fn != nullptr) fn(get().instance(), std::move(name), std::move(values));
    }

    template <typename T>
    static void resolve(void* handle, const char* name, T* fn) {
        *fn = reinterpret_cast<T>(dlsym(handle, name));
    }

    static const Api& get() {
        static const Api api = [] {
            Api a;
            void* handle = dlopen("libsysperftracker.so", RTLD_NOW);
            if (handle == nullptr) {
                ALOGW("PICO SysMtpClient unavailable: %s", dlerror());
                return a;
            }
            resolve(handle, "_ZN3mtp12SysMtpClient11getInstanceEv", &a.getInstance);
            if (a.getInstance == nullptr) return a;
            resolve(handle, "_ZN3mtp12SysMtpClient7onVsyncEl", &a.onVsync);
            resolve(handle,
                    "_ZN3mtp12SysMtpClient15addDisplayFrameENSt3__16vectorIN7android17SurfaceCl"
                    "ientItemENS1_9allocatorIS4_EEEEillll",
                    &a.addDisplayFrame);
            resolve(handle, "_ZN3mtp12SysMtpClient17sendTaskToWritePbEv", &a.sendTaskToWritePb);
            resolve(handle, "_ZN3mtp12SysMtpClient20sendTaskToWritePtpPbEv",
                    &a.sendTaskToWritePtpPb);
            resolve(handle,
                    "_ZN3mtp12SysMtpClient13setDevicePropENSt3__112basic_stringIcNS1_11char_trai"
                    "tsIcEENS1_9allocatorIcEEEES7_S7_",
                    &a.setDeviceProp);
            resolve(handle, "_ZN3mtp12SysMtpClient8shutDownEv", &a.shutDown);
            resolve(handle, "_ZN3mtp12SysMtpClient20notifyDisplayRefreshEi",
                    &a.notifyDisplayRefresh);
            resolve(handle,
                    "_ZN3mtp12SysMtpClient18notifyAutoDumpInfoENSt3__112basic_stringIcNS1_11char_"
                    "traitsIcEENS1_9allocatorIcEEEENS1_6vectorIiNS5_IiEEEE",
                    &a.notifyAutoDumpInfo);
            resolve(handle, "_ZN3mtp12SysMtpClient25setDailyDumpPerfettoCountEi",
                    &a.setDailyDumpPerfettoCount);
            resolve(handle,
                    "_ZN3mtp12SysMtpClient25notifyCrashReportDumpInfoENSt3__112basic_stringIcNS1_"
                    "11char_traitsIcEENS1_9allocatorIcEEEENS1_6vectorIiNS5_IiEEEE",
                    &a.notifyCrashReportDumpInfo);
            resolve(handle, "_ZN3mtp12SysMtpClient15notifyBacklightEiii", &a.notifyBacklight);
            resolve(handle, "_ZN3mtp12SysMtpClient19notifyLowPowerLevelEi",
                    &a.notifyLowPowerLevel);
            resolve(handle, "_ZN3mtp12SysMtpClient23updateTerribleJankScopeEi",
                    &a.updateTerribleJankScope);
            resolve(handle, "_ZN3mtp12SysMtpClient19updateCameraRefreshEi",
                    &a.updateCameraRefresh);
            resolve(handle,
                    "_ZN3mtp12SysMtpClient34notifyVirtualDisplaySurfaceChangedEiNSt3__112basic_st"
                    "ringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEE",
                    &a.notifyVirtualDisplaySurfaceChanged);
            resolve(handle,
                    "_ZN3mtp12SysMtpClient23notifyLaunchPackageInfoENSt3__112basic_stringIcNS1_11c"
                    "har_traitsIcEENS1_9allocatorIcEEEES7_il",
                    &a.notifyLaunchPackageInfo);
            resolve(handle,
                    "_ZN3mtp12SysMtpClient23notifySchedInfoDumpInfoENSt3__16vectorIiNS1_9allocato"
                    "rIiEEEE",
                    &a.notifySchedInfoDumpInfo);
            resolve(handle,
                    "_ZN3mtp12SysMtpClient19notifyLayerDumpInfoENSt3__112basic_stringIcNS1_11char_"
                    "traitsIcEENS1_9allocatorIcEEEENS1_6vectorIiNS5_IiEEEE",
                    &a.notifyLayerDumpInfo);
            return a;
        }();
        return api;
    }
};

} // namespace pico
} // namespace android
