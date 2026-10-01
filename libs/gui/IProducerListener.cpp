/*
 * Copyright 2014 The Android Open Source Project
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

#include <binder/Parcel.h>
#include <gui/bufferqueue/1.0/H2BProducerListener.h>
#include <gui/bufferqueue/2.0/H2BProducerListener.h>
#include <gui/IProducerListener.h>

namespace android {

enum {
    ON_BUFFER_RELEASED = IBinder::FIRST_CALL_TRANSACTION,
    NEEDS_RELEASE_NOTIFY,
    ON_BUFFERS_DISCARDED,
    ON_BUFFER_RELEASED_WITH_FENCE,
};

class BpProducerListener : public BpInterface<IProducerListener>
{
public:
    explicit BpProducerListener(const sp<IBinder>& impl)
        : BpInterface<IProducerListener>(impl) {}

    virtual ~BpProducerListener();

    virtual void onBufferReleased() {
        Parcel data, reply;
        data.writeInterfaceToken(IProducerListener::getInterfaceDescriptor());
        remote()->transact(ON_BUFFER_RELEASED, data, &reply, IBinder::FLAG_ONEWAY);
    }

    virtual bool needsReleaseNotify() {
        bool result;
        Parcel data, reply;
        data.writeInterfaceToken(IProducerListener::getInterfaceDescriptor());
        status_t err = remote()->transact(NEEDS_RELEASE_NOTIFY, data, &reply);
        if (err != NO_ERROR) {
            ALOGE("IProducerListener: binder call \'needsReleaseNotify\' failed");
            return true;
        }
        err = reply.readBool(&result);
        if (err != NO_ERROR) {
            ALOGE("IProducerListener: malformed binder reply");
            return true;
        }
        return result;
    }

    virtual void onBuffersDiscarded(const std::vector<int>& discardedSlots) {
        Parcel data, reply;
        data.writeInterfaceToken(IProducerListener::getInterfaceDescriptor());
        data.writeInt32Vector(discardedSlots);
        remote()->transact(ON_BUFFERS_DISCARDED, data, &reply, IBinder::FLAG_ONEWAY);
    }

    // Factory: no fence check and no write-status checks; the fence must not be null.
    void onBufferReleasedWithFence(const sp<Fence>& fence, uint64_t bufferId,
                                  bool flag) override {
        Parcel data, reply;
        data.writeInterfaceToken(IProducerListener::getInterfaceDescriptor());
        data.write(*fence);
        data.writeUint64(bufferId);
        data.writeBool(flag);
        remote()->transact(ON_BUFFER_RELEASED_WITH_FENCE, data, &reply, IBinder::FLAG_ONEWAY);
    }
};

// Out-of-line virtual method definition to trigger vtable emission in this
// translation unit (see clang warning -Wweak-vtables)
BpProducerListener::~BpProducerListener() {}

class HpProducerListener : public HpInterface<
        BpProducerListener,
        hardware::graphics::bufferqueue::V1_0::utils::H2BProducerListener,
        hardware::graphics::bufferqueue::V2_0::utils::H2BProducerListener> {
public:
    explicit HpProducerListener(const sp<IBinder>& base) : PBase{base} {}

    virtual void onBufferReleased() override {
        mBase->onBufferReleased();
    }

    virtual bool needsReleaseNotify() override {
        return mBase->needsReleaseNotify();
    }

    virtual void onBuffersDiscarded(const std::vector<int32_t>& discardedSlots) override {
        return mBase->onBuffersDiscarded(discardedSlots);
    }

    void onBufferReleasedWithFence(const sp<Fence>& fence, uint64_t bufferId,
                                  bool flag) override {
        mBase->onBufferReleasedWithFence(fence, bufferId, flag);
    }
};

IMPLEMENT_HYBRID_META_INTERFACE(ProducerListener,
        "android.gui.IProducerListener")

status_t BnProducerListener::onTransact(uint32_t code, const Parcel& data,
        Parcel* reply, uint32_t flags) {
    switch (code) {
        case ON_BUFFER_RELEASED:
            CHECK_INTERFACE(IProducerListener, data, reply);
            onBufferReleased();
            return NO_ERROR;
        case NEEDS_RELEASE_NOTIFY:
            CHECK_INTERFACE(IProducerListener, data, reply);
            reply->writeBool(needsReleaseNotify());
            return NO_ERROR;
        case ON_BUFFERS_DISCARDED: {
            CHECK_INTERFACE(IProducerListener, data, reply);
            std::vector<int32_t> discardedSlots;
            status_t result = data.readInt32Vector(&discardedSlots);
            if (result != NO_ERROR) {
                ALOGE("ON_BUFFERS_DISCARDED failed to read discardedSlots: %d", result);
                return result;
            }
            onBuffersDiscarded(discardedSlots);
            return NO_ERROR;
        }
        case ON_BUFFER_RELEASED_WITH_FENCE: {
            // Factory: the fence read status is ignored, the callback always runs and the
            // transaction returns the readUint64 status.
            CHECK_INTERFACE(IProducerListener, data, reply);
            sp<Fence> fence = new Fence;
            uint64_t bufferId = 0;
            data.read(*fence);
            status_t result = data.readUint64(&bufferId);
            bool flag = data.readBool();
            onBufferReleasedWithFence(fence, bufferId, flag);
            return result;
        }
    }
    return BBinder::onTransact(code, data, reply, flags);
}

ProducerListener::~ProducerListener() = default;

DummyProducerListener::~DummyProducerListener() = default;

bool BnProducerListener::needsReleaseNotify() {
    return true;
}

void BnProducerListener::onBuffersDiscarded(const std::vector<int32_t>& /*discardedSlots*/) {
}

void BnProducerListener::onBufferReleasedWithFence(const sp<Fence>& /*fence*/,
                                                 uint64_t /*bufferId*/, bool /*flag*/) {
}

VirtualDisplayProducerListener::~VirtualDisplayProducerListener() = default;

void VirtualDisplayProducerListener::setCallback(
        std::function<int(const sp<Fence>&, uint64_t, bool)> callback) {
    Mutex::Autolock lock(mCallbackMutex);
    mCallback = callback;
}

void VirtualDisplayProducerListener::onBufferReleasedWithFence(
        const sp<Fence>& fence, uint64_t bufferId, bool flag) {
    Mutex::Autolock lock(mCallbackMutex);
    if (mCallback) {
        // Factory ignores the callback's int result; the listener returns void.
        mCallback(fence, bufferId, flag);
    } else {
        ALOGE("onBufferReleasedWithFence callback is null!");
    }
}

} // namespace android
