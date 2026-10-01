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

#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

// The entry points below the PICO GLES layer ("next" in the layer chain). PicoLayerInit fills
// gl_funcs_map from getNextLayerProcAddress for every name in gl_funcs_names, then
// init_local_gl_funcs() copies them into the typed pointers.
namespace local {

extern std::unordered_map<std::string, void*> gl_funcs_map;
extern std::vector<std::string> gl_funcs_names;

extern EGLContext (*eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint*);
extern EGLBoolean (*eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
extern EGLBoolean (*eglDestroyContext)(EGLDisplay, EGLContext);
extern void (*glGenTextures)(GLsizei, GLuint*);
extern void (*glBindTexture)(GLenum, GLuint);
extern void (*glDeleteTextures)(GLsizei, const GLuint*);
extern void (*glGenFramebuffers)(GLsizei, GLuint*);
extern void (*glBindFramebuffer)(GLenum, GLuint);
extern void (*glFramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
extern void (*glFramebufferTexture2DMultisampleEXT)(GLenum, GLenum, GLenum, GLuint, GLint, GLsizei);
extern void (*glFramebufferTextureMultiviewOVR)(GLenum, GLenum, GLuint, GLint, GLint, GLsizei);
extern void (*glFramebufferTextureMultisampleMultiviewOVR)(GLenum, GLenum, GLuint, GLint, GLsizei,
                                                           GLint, GLsizei);
extern void (*glDeleteFramebuffers)(GLsizei, const GLuint*);
extern void (*glGetIntegerv)(GLenum, GLint*);

void init_local_gl_funcs();

} // namespace local
