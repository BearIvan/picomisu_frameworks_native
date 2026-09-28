// Copyright 2026 Picomisu contributors
// SPDX-License-Identifier: Apache-2.0

#include <binder/Binder.h>
#include <binder/Parcel.h>
#include <gui/BufferQueueCore.h>
#include <gui/BufferQueueConsumer.h>
#include <gui/BufferQueueProducer.h>
#include <gui/BufferItem.h>
#include <gui/IConsumerListener.h>
#include <gui/IProducerListener.h>
#include <gtest/gtest.h>
#include <system/window.h>

#include <limits>
#include <mutex>

namespace android {
namespace {

constexpr int kSetPvrStatus = 10000;

// Force use of the actual Bp/Bn marshalling code even within this process.
class QueryRelay : public BBinder {
public:
    explicit QueryRelay(const sp<IBinder>& target) : mTarget(target) {}
    uint32_t lastCode = 0;
    int lastWhat = 0;
    int lastInput = 0;

protected:
    status_t onTransact(uint32_t code, const Parcel& data, Parcel* reply,
                        uint32_t flags) override {
        const auto start = data.dataPosition();
        if (!data.checkInterface(mTarget.get())) return PERMISSION_DENIED;
        lastCode = code;
        lastWhat = data.readInt32();
        lastInput = data.readInt32();
        data.setDataPosition(start);
        return mTarget->transact(code, data, reply, flags);
    }

private:
    sp<IBinder> mTarget;
};

class FrameListener : public BnConsumerListener {
public:
    void onFrameAvailable(const BufferItem& item) override {
        ++calls;
        callbackBuffer = item.mGraphicBuffer.get();
    }
    void onFrameReplaced(const BufferItem& item) override { onFrameAvailable(item); }
    void onBuffersReleased() override {}
    void onSidebandStreamChanged() override {}
    int calls = 0;
    GraphicBuffer* callbackBuffer = nullptr;
};

} // namespace

class PicoBufferQueueStatusTest : public testing::Test {
protected:
    void SetUp() override {
        core = new BufferQueueCore;
        producer = new BufferQueueProducer(core);
        consumer = new BufferQueueConsumer(core);
    }
    int status() {
        std::lock_guard<std::mutex> lock(core->mMutex);
        return core->mPicoVrStatus;
    }
    int defaultWidth() {
        std::lock_guard<std::mutex> lock(core->mMutex);
        return static_cast<int>(core->mDefaultWidth);
    }
    void abandon() {
        std::lock_guard<std::mutex> lock(core->mMutex);
        core->mIsAbandoned = true;
    }
    int otherStatus(const sp<BufferQueueCore>& other) {
        std::lock_guard<std::mutex> lock(other->mMutex);
        return other->mPicoVrStatus;
    }
    bool isPicoConsumer() {
        std::lock_guard<std::mutex> lock(core->mMutex);
        return core->mHasPicoConsumer;
    }
    int consumerId() {
        std::lock_guard<std::mutex> lock(core->mMutex);
        return core->mPicoConsumerId;
    }
    bool consumerLogging() {
        std::lock_guard<std::mutex> lock(core->mMutex);
        return core->mPicoConsumerLogging;
    }
    status_t configureConsumer(int id, int logging) {
        Parcel data, reply;
        data.writeInterfaceToken(consumer->getInterfaceDescriptor());
        data.writeInt32(id);
        data.writeInt32(logging);
        return IInterface::asBinder(consumer)->transact(10000, data, &reply);
    }
    void expectCallbackBuffer(bool markPico) {
        sp<FrameListener> listener = new FrameListener;
        ASSERT_EQ(NO_ERROR, consumer->connect(listener, false));
        IGraphicBufferProducer::QueueBufferOutput output;
        ASSERT_EQ(NO_ERROR, producer->connect(nullptr, NATIVE_WINDOW_API_CPU, false, &output));
        if (markPico) ASSERT_EQ(NO_ERROR, configureConsumer(5, 0));
        // An empty GraphicBuffer exercises the reference lifetime without a
        // gralloc allocation, a rendered frame or display interaction.
        sp<GraphicBuffer> buffer = new GraphicBuffer;
        int slot = -1;
        ASSERT_EQ(NO_ERROR, producer->attachBuffer(&slot, buffer));
        IGraphicBufferProducer::QueueBufferInput input(0, false, HAL_DATASPACE_UNKNOWN,
                Rect::EMPTY_RECT, NATIVE_WINDOW_SCALING_MODE_FREEZE, 0, Fence::NO_FENCE);
        ASSERT_EQ(NO_ERROR, producer->queueBuffer(slot, input, &output));
        ASSERT_EQ(1, listener->calls);
        EXPECT_EQ(markPico ? buffer.get() : nullptr, listener->callbackBuffer);
        BufferItem acquired;
        ASSERT_EQ(NO_ERROR, consumer->acquireBuffer(&acquired, 0));
        EXPECT_EQ(buffer.get(), acquired.mGraphicBuffer.get());
    }
    sp<BufferQueueCore> core;
    sp<BufferQueueProducer> producer;
    sp<BufferQueueConsumer> consumer;
};

TEST_F(PicoBufferQueueStatusTest, LocalQueryPreservesSignedValuesAndUpdatesState) {
    EXPECT_EQ(0, status());
    for (int input : {1, 0, -1, std::numeric_limits<int>::min(),
                      std::numeric_limits<int>::max()}) {
        int value = input;
        ASSERT_EQ(NO_ERROR, producer->query(kSetPvrStatus, &value));
        EXPECT_EQ(input, value);
        EXPECT_EQ(input, status());
    }
}

TEST_F(PicoBufferQueueStatusTest, OrdinaryAndUnknownQueriesDoNotModifyVrState) {
    int value = 42;
    ASSERT_EQ(NO_ERROR, producer->query(kSetPvrStatus, &value));
    ASSERT_EQ(NO_ERROR, producer->query(NATIVE_WINDOW_WIDTH, &value));
    EXPECT_EQ(defaultWidth(), value);
    EXPECT_EQ(42, status());
    value = 77;
    EXPECT_EQ(BAD_VALUE, producer->query(10001, &value));
    EXPECT_EQ(77, value);
    EXPECT_EQ(42, status());
}

TEST_F(PicoBufferQueueStatusTest, InvalidQueriesDoNotChangeState) {
    EXPECT_EQ(BAD_VALUE, producer->query(kSetPvrStatus, nullptr));
    abandon();
    int value = 9;
    EXPECT_EQ(NO_INIT, producer->query(kSetPvrStatus, &value));
    EXPECT_EQ(9, value);
    EXPECT_EQ(0, status());
}

TEST_F(PicoBufferQueueStatusTest, SeparateQueuesHaveIndependentState) {
    sp<BufferQueueCore> other = new BufferQueueCore;
    int value = 17;
    ASSERT_EQ(NO_ERROR, producer->query(kSetPvrStatus, &value));
    EXPECT_EQ(0, otherStatus(other));
}

TEST_F(PicoBufferQueueStatusTest, BinderProxyPreservesInputAndZeroesOrdinaryPayload) {
    sp<QueryRelay> relay = new QueryRelay(IInterface::asBinder(producer));
    sp<IGraphicBufferProducer> proxy = interface_cast<IGraphicBufferProducer>(relay);
    int value = -7;
    ASSERT_EQ(NO_ERROR, proxy->query(kSetPvrStatus, &value));
    EXPECT_EQ(-7, value);
    EXPECT_EQ(-7, status());
    EXPECT_EQ(kSetPvrStatus, relay->lastWhat);
    EXPECT_EQ(-7, relay->lastInput);
    value = 12345;
    ASSERT_EQ(NO_ERROR, proxy->query(NATIVE_WINDOW_WIDTH, &value));
    EXPECT_EQ(0, relay->lastInput);
    EXPECT_EQ(defaultWidth(), value);
    EXPECT_EQ(-7, status());
}

TEST_F(PicoBufferQueueStatusTest, ServerAcceptsLegacyQueryWithoutInputPayload) {
    sp<QueryRelay> relay = new QueryRelay(IInterface::asBinder(producer));
    sp<IGraphicBufferProducer> proxy = interface_cast<IGraphicBufferProducer>(relay);
    int value = 33;
    ASSERT_EQ(NO_ERROR, proxy->query(kSetPvrStatus, &value));
    Parcel data, reply;
    data.writeInterfaceToken(producer->getInterfaceDescriptor());
    data.writeInt32(NATIVE_WINDOW_WIDTH);
    ASSERT_EQ(NO_ERROR, IInterface::asBinder(producer)->transact(relay->lastCode, data, &reply));
    EXPECT_EQ(defaultWidth(), reply.readInt32());
    EXPECT_EQ(NO_ERROR, reply.readInt32());
    EXPECT_EQ(33, status());
}

TEST_F(PicoBufferQueueStatusTest, ConsumerMarkerIsSeparateFromProducerStatus) {
    EXPECT_FALSE(isPicoConsumer());
    ASSERT_EQ(NO_ERROR, configureConsumer(-13, 1));
    EXPECT_TRUE(isPicoConsumer());
    EXPECT_EQ(-13, consumerId());
    EXPECT_TRUE(consumerLogging());
    EXPECT_EQ(0, status());
    ASSERT_EQ(NO_ERROR, configureConsumer(7, 2));
    EXPECT_EQ(7, consumerId());
    EXPECT_FALSE(consumerLogging());
}

TEST_F(PicoBufferQueueStatusTest, ConsumerRejectsWrongInterfaceToken) {
    Parcel data, reply;
    data.writeInterfaceToken(producer->getInterfaceDescriptor());
    data.writeInt32(1);
    data.writeInt32(0);
    EXPECT_EQ(PERMISSION_DENIED, IInterface::asBinder(consumer)->transact(10000, data, &reply));
    EXPECT_FALSE(isPicoConsumer());
}

TEST_F(PicoBufferQueueStatusTest, ConsumerRejectsIncompleteConfiguration) {
    Parcel data, reply;
    data.writeInterfaceToken(consumer->getInterfaceDescriptor());
    data.writeInt32(99);
    EXPECT_EQ(BAD_VALUE, IInterface::asBinder(consumer)->transact(10000, data, &reply));
    EXPECT_FALSE(isPicoConsumer());
    EXPECT_EQ(0, consumerId());
}

TEST_F(PicoBufferQueueStatusTest, StandardConsumerBinderCommandsStillWork) {
    sp<QueryRelay> relay = new QueryRelay(IInterface::asBinder(consumer));
    sp<IGraphicBufferConsumer> proxy = interface_cast<IGraphicBufferConsumer>(relay);
    uint64_t mask = 0;
    ASSERT_EQ(NO_ERROR, configureConsumer(1, 0));
    EXPECT_EQ(NO_ERROR, proxy->getReleasedBuffers(&mask));
    EXPECT_NE(0u, mask);
}

TEST_F(PicoBufferQueueStatusTest, OrdinaryCallbackClearsGraphicBufferReference) {
    expectCallbackBuffer(false);
}

TEST_F(PicoBufferQueueStatusTest, PicoCallbackPreservesGraphicBufferReference) {
    expectCallbackBuffer(true);
}

} // namespace android
