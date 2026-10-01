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

// Entry points of the built-in PICO GLES layer. LayerLoader installs it as layer 0, ahead of any
// debug layer (see LayerLoader::getInstance and LayerLoader::InitPicoLayer).

#include "egl_pico_layer.h"

#include "pico_layer/HookApi.h"
#include "pico_layer/gl_funcs.h"

extern "C" {

EGLFuncPointer PicoLayerInit(const void* layer_id,
                             android::LayerLoader::PFNEGLGETNEXTLAYERPROCADDRESSPROC
                                     get_next_layer_proc_address) {
    for (const std::string& name : local::gl_funcs_names) {
        local::gl_funcs_map[name] = get_next_layer_proc_address(const_cast<void*>(layer_id),
                                                                name.c_str());
    }
    local::init_local_gl_funcs();
    return nullptr;
}

EGLFuncPointer PicoLayerSetup(const char* name, EGLFuncPointer next) {
    if (pico_layer::hook_funcs_map.find(name) != pico_layer::hook_funcs_map.end()) {
        return reinterpret_cast<EGLFuncPointer>(pico_layer::hook_funcs_map[name]);
    }
    return next;
}

// Exported so that clients can detect the texture redirect support of this libEGL.
__attribute__((visibility("default"))) void libegl_texture_redirect_mark() {}

} // extern "C"
