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

#include "gl_funcs.h"

namespace local {

std::unordered_map<std::string, void*> gl_funcs_map;

// The names PicoLayerInit resolves through getNextLayerProcAddress (factory order).
std::vector<std::string> gl_funcs_names = {
        "eglCreateContext",
        "eglMakeCurrent",
        "eglDestroyContext",
        "glGenTextures",
        "glDeleteTextures",
        "glBindTexture",
        "glGenFramebuffers",
        "glBindFramebuffer",
        "glFramebufferTexture2D",
        "glFramebufferTexture2DMultisampleEXT",
        "glFramebufferTextureMultiviewOVR",
        "glFramebufferTextureMultisampleMultiviewOVR",
        "glDeleteFramebuffers",
        "glGetIntegerv",
};

EGLContext (*eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint*);
EGLBoolean (*eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
EGLBoolean (*eglDestroyContext)(EGLDisplay, EGLContext);
void (*glGenTextures)(GLsizei, GLuint*);
void (*glBindTexture)(GLenum, GLuint);
void (*glDeleteTextures)(GLsizei, const GLuint*);
void (*glGenFramebuffers)(GLsizei, GLuint*);
void (*glBindFramebuffer)(GLenum, GLuint);
void (*glFramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
void (*glFramebufferTexture2DMultisampleEXT)(GLenum, GLenum, GLenum, GLuint, GLint, GLsizei);
void (*glFramebufferTextureMultiviewOVR)(GLenum, GLenum, GLuint, GLint, GLint, GLsizei);
void (*glFramebufferTextureMultisampleMultiviewOVR)(GLenum, GLenum, GLuint, GLint, GLsizei, GLint,
                                                    GLsizei);
void (*glDeleteFramebuffers)(GLsizei, const GLuint*);
void (*glGetIntegerv)(GLenum, GLint*);

#define LOAD_GL_FUNC(name) name = reinterpret_cast<decltype(name)>(gl_funcs_map[#name])

void init_local_gl_funcs() {
    LOAD_GL_FUNC(eglCreateContext);
    LOAD_GL_FUNC(eglMakeCurrent);
    LOAD_GL_FUNC(eglDestroyContext);
    LOAD_GL_FUNC(glGenTextures);
    LOAD_GL_FUNC(glBindTexture);
    LOAD_GL_FUNC(glDeleteTextures);
    LOAD_GL_FUNC(glGenFramebuffers);
    LOAD_GL_FUNC(glBindFramebuffer);
    LOAD_GL_FUNC(glFramebufferTexture2D);
    LOAD_GL_FUNC(glFramebufferTexture2DMultisampleEXT);
    LOAD_GL_FUNC(glFramebufferTextureMultiviewOVR);
    LOAD_GL_FUNC(glFramebufferTextureMultisampleMultiviewOVR);
    LOAD_GL_FUNC(glDeleteFramebuffers);
    LOAD_GL_FUNC(glGetIntegerv);
}

#undef LOAD_GL_FUNC

} // namespace local
