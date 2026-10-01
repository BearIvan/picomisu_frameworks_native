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

#include "Framebuffer.h"

namespace pico_layer {

Framebuffer::Framebuffer(GLuint name)
      : NamedObject(name),
        mColorAttachType(kTexture2D),
        mColorTexture(0),
        mTarget(GL_FRAMEBUFFER),
        mLevel(0),
        mSamples(0),
        mBaseViewIndex(0),
        mNumViews(0) {}

Framebuffer::~Framebuffer() {}

void Framebuffer::setColorTexture2D(GLenum target, GLuint texture, GLint level) {
    mColorAttachType = kTexture2D;
    mColorTexture = texture;
    mTarget = target;
    mLevel = level;
}

void Framebuffer::setColorTexture2DMultisampleEXT(GLenum target, GLuint texture, GLint level,
                                                  GLsizei samples) {
    mColorAttachType = kTexture2DMultisampleEXT;
    mColorTexture = texture;
    mTarget = target;
    mLevel = level;
    mSamples = samples;
}

void Framebuffer::setColorTextureMultiviewOVR(GLenum target, GLuint texture, GLint level,
                                              GLint baseViewIndex, GLsizei numViews) {
    mColorAttachType = kTextureMultiviewOVR;
    mColorTexture = texture;
    mTarget = target;
    mLevel = level;
    mBaseViewIndex = baseViewIndex;
    mNumViews = numViews;
}

void Framebuffer::setColorTextureMultisampleMultiviewOVR(GLenum target, GLuint texture,
                                                         GLint level, GLsizei samples,
                                                         GLint baseViewIndex, GLsizei numViews) {
    mColorAttachType = kTextureMultisampleMultiviewOVR;
    mColorTexture = texture;
    mTarget = target;
    mLevel = level;
    mSamples = samples;
    mBaseViewIndex = baseViewIndex;
    mNumViews = numViews;
}

} // namespace pico_layer
