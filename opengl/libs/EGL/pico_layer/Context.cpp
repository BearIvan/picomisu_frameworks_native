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

#include "Context.h"

#include <mutex>
#include <vector>

#include <GLES2/gl2ext.h>
#include <GLES3/gl3.h>

#include "gl_funcs.h"

namespace pico_layer {

static std::mutex sContextsLock;
static std::unordered_map<void*, std::shared_ptr<Context>> sContexts;
static thread_local std::shared_ptr<Context> sCurrentContext;

std::shared_ptr<Context> getCurrentContext() {
    return sCurrentContext;
}

static std::shared_ptr<Context> getContext(void* context) {
    std::lock_guard<std::mutex> lock(sContextsLock);
    if (sContexts.find(context) != sContexts.end()) {
        return sContexts[context];
    }
    return nullptr;
}

// Called with the context eglCreateContext returned, even EGL_NO_CONTEXT (as the factory does).
void CreateContext(void* context, void* shareContext) {
    std::shared_ptr<Context> share = getContext(shareContext);
    std::shared_ptr<Context> created = std::make_shared<Context>(context, share);
    std::lock_guard<std::mutex> lock(sContextsLock);
    sContexts.insert(std::make_pair(context, created));
}

// Called after every eglMakeCurrent, whatever it returned.
void MakeCurrent(void* context) {
    sCurrentContext = getContext(context);
}

void DestroyContext(void* context) {
    std::shared_ptr<Context> current = sCurrentContext;
    if (current.get() == getContext(context).get()) {
        MakeCurrent(nullptr);
    }
    std::lock_guard<std::mutex> lock(sContextsLock);
    sContexts.erase(context);
}

Context::Context(void* context, std::shared_ptr<Context> shareContext)
      : mReadFramebuffer(0), mDrawFramebuffer(0), mContext(context) {
    if (shareContext) {
        mTextureRedirect = shareContext->mTextureRedirect;
    } else {
        mTextureRedirect = std::make_shared<TextureRedirect>();
    }
}

void Context::createFramebuffer(GLuint name) {
    mFramebuffers.reserve(name);
}

Framebuffer* Context::getFramebuffer(GLuint name) const {
    return mFramebuffers.get(name);
}

void Context::bindReadFramebuffer(GLuint name) {
    if (getFramebuffer(name) == nullptr) {
        Framebuffer* framebuffer = new Framebuffer(name);
        mFramebuffers.set(name, framebuffer);
        framebuffer->addRef();
    }
    mReadFramebuffer = name;
}

void Context::bindDrawFramebuffer(GLuint name) {
    if (getFramebuffer(name) == nullptr) {
        Framebuffer* framebuffer = new Framebuffer(name);
        mFramebuffers.set(name, framebuffer);
        framebuffer->addRef();
    }
    mDrawFramebuffer = name;
}

GLuint Context::getCurrentFramebuffer(GLenum target) const {
    switch (target) {
        case GL_READ_FRAMEBUFFER:
            return mReadFramebuffer;
        case GL_FRAMEBUFFER:
        case GL_DRAW_FRAMEBUFFER:
            return mDrawFramebuffer;
        default:
            return 0;
    }
}

void Context::deleteFramebuffer(GLuint name) {
    if (mReadFramebuffer == name) {
        bindReadFramebuffer(0);
    }
    if (mDrawFramebuffer == name) {
        bindDrawFramebuffer(0);
    }
    Framebuffer* framebuffer = mFramebuffers.remove(name);
    if (framebuffer != nullptr) {
        framebuffer->release();
    }
}

void Context::framebufferTexture2Dchange(GLuint from, GLuint to) {
    std::vector<Framebuffer*> framebuffers;
    for (const auto& entry : mFramebuffers.mObjects) {
        if (entry.second != nullptr) {
            framebuffers.push_back(entry.second);
        }
    }

    for (Framebuffer* framebuffer : framebuffers) {
        if (framebuffer == nullptr || framebuffer->mColorTexture != from) {
            continue;
        }
        GLint previous = 0;
        const GLenum target = framebuffer->mTarget;
        if (target == GL_READ_FRAMEBUFFER) {
            local::glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous);
        } else if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER) {
            local::glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous);
        }
        local::glBindFramebuffer(target, framebuffer->getName());
        switch (framebuffer->mColorAttachType) {
            case Framebuffer::kTexture2D:
                local::glFramebufferTexture2D(target, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, to,
                                              framebuffer->mLevel);
                break;
            case Framebuffer::kTexture2DMultisampleEXT:
                local::glFramebufferTexture2DMultisampleEXT(target, GL_COLOR_ATTACHMENT0,
                                                            GL_TEXTURE_2D, to, framebuffer->mLevel,
                                                            framebuffer->mSamples);
                break;
            case Framebuffer::kTextureMultiviewOVR:
                local::glFramebufferTextureMultiviewOVR(target, GL_COLOR_ATTACHMENT0, to,
                                                        framebuffer->mLevel,
                                                        framebuffer->mBaseViewIndex,
                                                        framebuffer->mNumViews);
                break;
            case Framebuffer::kTextureMultisampleMultiviewOVR:
                local::glFramebufferTextureMultisampleMultiviewOVR(target, GL_COLOR_ATTACHMENT0,
                                                                   to, framebuffer->mLevel,
                                                                   framebuffer->mSamples,
                                                                   framebuffer->mBaseViewIndex,
                                                                   framebuffer->mNumViews);
                break;
            default:
                break;
        }
        local::glBindFramebuffer(target, previous);
    }
}

// An existing redirect of |texture| is kept; any call enables the redirect lookup.
void Context::setRedirect(int texture, GLuint redirect) {
    std::unordered_map<GLuint, GLuint>& textures = mTextureRedirect->textures;
    if (textures.find(texture) == textures.end()) {
        textures.insert(std::make_pair(static_cast<GLuint>(texture), redirect));
    }
    mTextureRedirect->enabled = true;
}

GLuint Context::getRedirect(int texture) {
    if (!mTextureRedirect->enabled) {
        return texture;
    }
    std::unordered_map<GLuint, GLuint>& textures = mTextureRedirect->textures;
    if (textures.find(texture) == textures.end()) {
        return texture;
    }
    return textures[texture];
}

} // namespace pico_layer
