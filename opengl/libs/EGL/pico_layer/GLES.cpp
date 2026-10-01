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

#include "GLES.h"

#include <GLES2/gl2ext.h>
#include <GLES3/gl3.h>

#include "Context.h"

namespace pico_layer {

void SetTextureRedirect(int texture, int redirect) {
    std::shared_ptr<Context> context = getCurrentContext();
    if (context) {
        context->setRedirect(texture, redirect);
    }
}

GLuint GetTextureRedirect(int texture) {
    std::shared_ptr<Context> context = getCurrentContext();
    if (context) {
        return context->getRedirect(texture);
    }
    return texture;
}

void GenFramebuffers(GLsizei n, GLuint* framebuffers) {
    std::shared_ptr<Context> context = getCurrentContext();
    if (n < 1 || !context) {
        return;
    }
    for (GLsizei i = 0; i < n; i++) {
        context->createFramebuffer(framebuffers[i]);
    }
}

void BindFramebuffer(GLenum target, GLuint framebuffer) {
    std::shared_ptr<Context> context = getCurrentContext();
    if (!context) {
        return;
    }
    if (target == GL_FRAMEBUFFER || target == GL_READ_FRAMEBUFFER) {
        context->bindReadFramebuffer(framebuffer);
    }
    if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER) {
        context->bindDrawFramebuffer(framebuffer);
    }
}

void DeleteFramebuffers(GLsizei n, GLuint* framebuffers) {
    std::shared_ptr<Context> context = getCurrentContext();
    if (n < 1 || !context) {
        return;
    }
    for (GLsizei i = 0; i < n; i++) {
        context->deleteFramebuffer(framebuffers[i]);
    }
}

// Only GL_COLOR_ATTACHMENT0 of the framebuffer bound to |target| is tracked.
void FramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture,
                          GLint level) {
    if (attachment != GL_COLOR_ATTACHMENT0) {
        return;
    }
    std::shared_ptr<Context> context = getCurrentContext();
    if (textarget != GL_TEXTURE_2D || !context) {
        return;
    }
    Framebuffer* framebuffer = context->getFramebuffer(context->getCurrentFramebuffer(target));
    if (framebuffer != nullptr) {
        framebuffer->setColorTexture2D(target, texture, level);
    }
}

void FramebufferTexture2DMultisampleEXT(GLenum target, GLenum attachment, GLenum textarget,
                                        GLuint texture, GLint level, GLsizei samples) {
    if (attachment != GL_COLOR_ATTACHMENT0) {
        return;
    }
    std::shared_ptr<Context> context = getCurrentContext();
    if (textarget != GL_TEXTURE_2D || !context) {
        return;
    }
    Framebuffer* framebuffer = context->getFramebuffer(context->getCurrentFramebuffer(target));
    if (framebuffer != nullptr) {
        framebuffer->setColorTexture2DMultisampleEXT(target, texture, level, samples);
    }
}

void FramebufferTextureMultiviewOVR(GLenum target, GLenum attachment, GLuint texture, GLint level,
                                    GLint baseViewIndex, GLsizei numViews) {
    if (attachment != GL_COLOR_ATTACHMENT0) {
        return;
    }
    std::shared_ptr<Context> context = getCurrentContext();
    if (!context) {
        return;
    }
    Framebuffer* framebuffer = context->getFramebuffer(context->getCurrentFramebuffer(target));
    if (framebuffer != nullptr) {
        framebuffer->setColorTextureMultiviewOVR(target, texture, level, baseViewIndex, numViews);
    }
}

void FramebufferTextureMultisampleMultiviewOVR(GLenum target, GLenum attachment, GLuint texture,
                                               GLint level, GLsizei samples, GLint baseViewIndex,
                                               GLsizei numViews) {
    if (attachment != GL_COLOR_ATTACHMENT0) {
        return;
    }
    std::shared_ptr<Context> context = getCurrentContext();
    if (!context) {
        return;
    }
    Framebuffer* framebuffer = context->getFramebuffer(context->getCurrentFramebuffer(target));
    if (framebuffer != nullptr) {
        framebuffer->setColorTextureMultisampleMultiviewOVR(target, texture, level, samples,
                                                            baseViewIndex, numViews);
    }
}

void FramebufferTexture2Dchange(GLuint from, GLuint to) {
    std::shared_ptr<Context> context = getCurrentContext();
    if (context) {
        context->framebufferTexture2Dchange(from, to);
    }
}

} // namespace pico_layer
