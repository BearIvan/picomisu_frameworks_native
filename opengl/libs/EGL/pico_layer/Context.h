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

#include <map>
#include <memory>
#include <unordered_map>

#include <GLES2/gl2.h>

#include "Framebuffer.h"

namespace pico_layer {

// Framebuffer names of one context. Objects are owned through Object::addRef/release.
template <typename T, unsigned kFirstName>
class NameSpace {
public:
    T* get(GLuint name) const {
        auto it = mObjects.find(name);
        return it == mObjects.end() ? nullptr : it->second;
    }

    // Reserves the name without an object (glGenFramebuffers).
    void reserve(GLuint name) { mObjects.insert(std::make_pair(name, nullptr)); }

    void set(GLuint name, T* object) { mObjects[name] = object; }

    // Forgets the name and hands back its object (nullptr if none).
    T* remove(GLuint name) {
        auto it = mObjects.find(name);
        if (it == mObjects.end()) {
            return nullptr;
        }
        T* object = it->second;
        mObjects.erase(it);
        return object;
    }

    std::map<GLuint, T*> mObjects;
};

// The texture redirect map, shared by every context of a share group.
struct TextureRedirect {
    bool enabled = false;
    std::unordered_map<GLuint, GLuint> textures;
};

class Context {
public:
    Context(void* context, std::shared_ptr<Context> shareContext);

    void createFramebuffer(GLuint name);
    Framebuffer* getFramebuffer(GLuint name) const;
    void bindReadFramebuffer(GLuint name);
    void bindDrawFramebuffer(GLuint name);
    GLuint getCurrentFramebuffer(GLenum target) const;
    void deleteFramebuffer(GLuint name);

    // Re-attaches every framebuffer whose colour texture is |from| to |to|.
    void framebufferTexture2Dchange(GLuint from, GLuint to);

    void setRedirect(int texture, GLuint redirect);
    GLuint getRedirect(int texture);

    void* getEglContext() const { return mContext; }

private:
    GLuint mReadFramebuffer;
    GLuint mDrawFramebuffer;
    NameSpace<Framebuffer, 1u> mFramebuffers;
    void* mContext;
    std::shared_ptr<TextureRedirect> mTextureRedirect;
};

std::shared_ptr<Context> getCurrentContext();
void CreateContext(void* context, void* shareContext);
void MakeCurrent(void* context);
void DestroyContext(void* context);

} // namespace pico_layer
