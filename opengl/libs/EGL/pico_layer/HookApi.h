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

#include <EGL/egl.h>
#include <GLES2/gl2.h>

namespace pico_layer {

// Entry point name -> PICO layer hook, consulted by PicoLayerSetup.
extern std::unordered_map<std::string, void*> hook_funcs_map;

EGLContext eglCreateContext_(EGLDisplay dpy, EGLConfig config, EGLContext shareContext,
                             const EGLint* attribList);
EGLBoolean eglMakeCurrent_(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext context);
EGLBoolean eglDestroyContext_(EGLDisplay dpy, EGLContext context);
void glGenTextures_(GLsizei n, GLuint* textures);
void glFramebufferTexture2Dchange_(GLuint from, GLuint to);
void glDeleteTextures_(GLsizei n, const GLuint* textures);
void glBindTexture_(GLenum target, GLuint texture);
void glGenFramebuffers_(GLsizei n, GLuint* framebuffers);
void glBindFramebuffer_(GLenum target, GLuint framebuffer);
void glFramebufferTexture2D_(GLenum target, GLenum attachment, GLenum textarget, GLuint texture,
                             GLint level);
void glFramebufferTexture2DMultisampleEXT_(GLenum target, GLenum attachment, GLenum textarget,
                                           GLuint texture, GLint level, GLsizei samples);
void glFramebufferTextureMultiviewOVR_(GLenum target, GLenum attachment, GLuint texture,
                                       GLint level, GLint baseViewIndex, GLsizei numViews);
void glFramebufferTextureMultisampleMultiviewOVR_(GLenum target, GLenum attachment,
                                                  GLuint texture, GLint level, GLsizei samples,
                                                  GLint baseViewIndex, GLsizei numViews);
void glDeleteFramebuffers_(GLsizei n, GLuint* framebuffers);

} // namespace pico_layer
