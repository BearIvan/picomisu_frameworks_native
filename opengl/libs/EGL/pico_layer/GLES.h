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

#include <GLES2/gl2.h>

// State tracking of the PICO layer, applied to the current context after the driver call.
namespace pico_layer {

void SetTextureRedirect(int texture, int redirect);
GLuint GetTextureRedirect(int texture);
void GenFramebuffers(GLsizei n, GLuint* framebuffers);
void BindFramebuffer(GLenum target, GLuint framebuffer);
void DeleteFramebuffers(GLsizei n, GLuint* framebuffers);
void FramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture,
                          GLint level);
void FramebufferTexture2DMultisampleEXT(GLenum target, GLenum attachment, GLenum textarget,
                                        GLuint texture, GLint level, GLsizei samples);
void FramebufferTextureMultiviewOVR(GLenum target, GLenum attachment, GLuint texture, GLint level,
                                    GLint baseViewIndex, GLsizei numViews);
void FramebufferTextureMultisampleMultiviewOVR(GLenum target, GLenum attachment, GLuint texture,
                                               GLint level, GLsizei samples, GLint baseViewIndex,
                                               GLsizei numViews);
void FramebufferTexture2Dchange(GLuint from, GLuint to);

} // namespace pico_layer
