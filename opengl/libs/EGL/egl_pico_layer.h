/*
 * Copyright 2026 The Android Open Source Project
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

#ifndef ANDROID_EGL_PICO_LAYER_H
#define ANDROID_EGL_PICO_LAYER_H

#include "egl_layers.h"

extern "C" {

EGLFuncPointer PicoLayerInit(
        const void* layer_id,
        android::LayerLoader::PFNEGLGETNEXTLAYERPROCADDRESSPROC get_next_layer_proc_address);
EGLFuncPointer PicoLayerSetup(const char* name, EGLFuncPointer next);
__attribute__((visibility("default"))) void libegl_texture_redirect_mark();

} // extern "C"

#endif // ANDROID_EGL_PICO_LAYER_H
