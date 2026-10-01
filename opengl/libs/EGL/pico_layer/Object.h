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

namespace pico_layer {

// Intrusively reference-counted GL object (not thread safe; owned by one EGL context).
class Object {
public:
    Object() = default;
    virtual void addRef();
    virtual void release();
    virtual ~Object();

protected:
    int mRefCount = 0;
};

class NamedObject : public Object {
public:
    explicit NamedObject(GLuint name);
    ~NamedObject() override;

    GLuint getName() const { return mName; }

protected:
    GLuint mName;
};

} // namespace pico_layer
