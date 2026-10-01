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

#include "Object.h"

namespace pico_layer {

// What the PICO layer remembers about a framebuffer: its GL_COLOR_ATTACHMENT0 texture and the
// call that attached it, so the attachment can be replayed with another texture.
class Framebuffer : public NamedObject {
public:
    enum ColorAttachType {
        kTexture2D = 0,
        kTexture2DMultisampleEXT = 1,
        kTextureMultiviewOVR = 2,
        kTextureMultisampleMultiviewOVR = 3,
    };

    explicit Framebuffer(GLuint name);
    ~Framebuffer() override;

    void setColorTexture2D(GLenum target, GLuint texture, GLint level);
    void setColorTexture2DMultisampleEXT(GLenum target, GLuint texture, GLint level,
                                         GLsizei samples);
    void setColorTextureMultiviewOVR(GLenum target, GLuint texture, GLint level,
                                     GLint baseViewIndex, GLsizei numViews);
    void setColorTextureMultisampleMultiviewOVR(GLenum target, GLuint texture, GLint level,
                                                GLsizei samples, GLint baseViewIndex,
                                                GLsizei numViews);

    int mColorAttachType;
    GLuint mColorTexture;
    GLenum mTarget;
    GLint mLevel;
    GLsizei mSamples;
    GLint mBaseViewIndex;
    GLsizei mNumViews;
};

} // namespace pico_layer
