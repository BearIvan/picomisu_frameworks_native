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

#include <mutex>

#include <utils/Timers.h>

// PICO: vsync period of the refresh rate that SurfaceFlinger switches to (factory global
// VsyncRecord, a function-local static shared by SurfaceFlinger::setDesiredActiveConfig and
// Scheduler::setVsyncPeriod). Once a config change has been requested, the DispSync model is
// resynced to the period of the requested config instead of the period the caller measured.
class VsyncRecord {
public:
    static VsyncRecord& getInstance() {
        static VsyncRecord sInstance;
        return sInstance;
    }

    std::mutex mutex;
    // 1e9 / fps of the desired active config; 0 until a config change was requested.
    nsecs_t period = 0;
};
