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

#include "HookApi.h"

#include <log/log.h>

#include "Context.h"
#include "GLES.h"
#include "gl_funcs.h"

namespace pico_layer {

// Present in the factory TU, never read.
static std::unordered_map<GLuint, GLuint> sUnusedTextureMap;

EGLContext eglCreateContext_(EGLDisplay dpy, EGLConfig config, EGLContext shareContext,
                             const EGLint* attribList) {
    EGLContext context = local::eglCreateContext(dpy, config, shareContext, attribList);
    CreateContext(context, shareContext);
    return context;
}

EGLBoolean eglMakeCurrent_(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext context) {
    EGLBoolean result = local::eglMakeCurrent(dpy, draw, read, context);
    MakeCurrent(context);
    return result;
}

EGLBoolean eglDestroyContext_(EGLDisplay dpy, EGLContext context) {
    EGLBoolean result = local::eglDestroyContext(dpy, context);
    DestroyContext(context);
    return result;
}

// glGenTextures(-1, {t1, t2}) is the PICO texture redirect request: t1 becomes t2 for
// glBindTexture / glFramebufferTexture*, and framebuffers already using t1 are re-attached.
void glGenTextures_(GLsizei n, GLuint* textures) {
    if (n != -1) {
        local::glGenTextures(n, textures);
        return;
    }
    ALOGD("glTexRedirect2D");
    ALOGD("hook glGenTextures: %d, t1: %d, t2: %d\n", n, textures[0], textures[1]);
    GLuint from = textures[0];
    GLuint to = textures[1];
    ALOGD("hook glFramebufferTexture2Dchange: %d, %d\n", from, to);
    SetTextureRedirect(from, to);
    FramebufferTexture2Dchange(from, to);
}

void glFramebufferTexture2Dchange_(GLuint from, GLuint to) {
    ALOGD("hook glFramebufferTexture2Dchange: %d, %d\n", from, to);
    SetTextureRedirect(from, to);
    FramebufferTexture2Dchange(from, to);
}

void glDeleteTextures_(GLsizei n, const GLuint* textures) {
    local::glDeleteTextures(n, textures);
}

void glBindTexture_(GLenum target, GLuint texture) {
    local::glBindTexture(target, GetTextureRedirect(texture));
}

void glGenFramebuffers_(GLsizei n, GLuint* framebuffers) {
    local::glGenFramebuffers(n, framebuffers);
    GenFramebuffers(n, framebuffers);
}

void glBindFramebuffer_(GLenum target, GLuint framebuffer) {
    local::glBindFramebuffer(target, framebuffer);
    BindFramebuffer(target, framebuffer);
}

void glFramebufferTexture2D_(GLenum target, GLenum attachment, GLenum textarget, GLuint texture,
                             GLint level) {
    GLuint redirect = GetTextureRedirect(texture);
    local::glFramebufferTexture2D(target, attachment, textarget, redirect, level);
    FramebufferTexture2D(target, attachment, textarget, redirect, level);
}

void glFramebufferTexture2DMultisampleEXT_(GLenum target, GLenum attachment, GLenum textarget,
                                           GLuint texture, GLint level, GLsizei samples) {
    GLuint redirect = GetTextureRedirect(texture);
    local::glFramebufferTexture2DMultisampleEXT(target, attachment, textarget, redirect, level,
                                                samples);
    FramebufferTexture2DMultisampleEXT(target, attachment, textarget, redirect, level, samples);
}

void glFramebufferTextureMultiviewOVR_(GLenum target, GLenum attachment, GLuint texture,
                                       GLint level, GLint baseViewIndex, GLsizei numViews) {
    GLuint redirect = GetTextureRedirect(texture);
    local::glFramebufferTextureMultiviewOVR(target, attachment, redirect, level, baseViewIndex,
                                            numViews);
    FramebufferTextureMultiviewOVR(target, attachment, redirect, level, baseViewIndex, numViews);
}

void glFramebufferTextureMultisampleMultiviewOVR_(GLenum target, GLenum attachment,
                                                  GLuint texture, GLint level, GLsizei samples,
                                                  GLint baseViewIndex, GLsizei numViews) {
    GLuint redirect = GetTextureRedirect(texture);
    local::glFramebufferTextureMultisampleMultiviewOVR(target, attachment, redirect, level,
                                                       samples, baseViewIndex, numViews);
    FramebufferTextureMultisampleMultiviewOVR(target, attachment, redirect, level, samples,
                                              baseViewIndex, numViews);
}

void glDeleteFramebuffers_(GLsizei n, GLuint* framebuffers) {
    local::glDeleteFramebuffers(n, framebuffers);
    DeleteFramebuffers(n, framebuffers);
}

std::unordered_map<std::string, void*> hook_funcs_map = {
        {"eglCreateContext", reinterpret_cast<void*>(eglCreateContext_)},
        {"eglMakeCurrent", reinterpret_cast<void*>(eglMakeCurrent_)},
        {"eglDestroyContext", reinterpret_cast<void*>(eglDestroyContext_)},
        {"glGenTextures", reinterpret_cast<void*>(glGenTextures_)},
        {"glDeleteTextures", reinterpret_cast<void*>(glDeleteTextures_)},
        {"glBindTexture", reinterpret_cast<void*>(glBindTexture_)},
        {"glGenFramebuffers", reinterpret_cast<void*>(glGenFramebuffers_)},
        {"glBindFramebuffer", reinterpret_cast<void*>(glBindFramebuffer_)},
        {"glFramebufferTexture2D", reinterpret_cast<void*>(glFramebufferTexture2D_)},
        {"glFramebufferTexture2DMultisampleEXT",
         reinterpret_cast<void*>(glFramebufferTexture2DMultisampleEXT_)},
        {"glFramebufferTextureMultiviewOVR",
         reinterpret_cast<void*>(glFramebufferTextureMultiviewOVR_)},
        {"glFramebufferTextureMultisampleMultiviewOVR",
         reinterpret_cast<void*>(glFramebufferTextureMultisampleMultiviewOVR_)},
        {"glFramebufferTexture2Dchange", reinterpret_cast<void*>(glFramebufferTexture2Dchange_)},
        {"glDeleteFramebuffers", reinterpret_cast<void*>(glDeleteFramebuffers_)},
};

} // namespace pico_layer
