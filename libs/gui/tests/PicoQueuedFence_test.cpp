// Copyright 2026 Picomisu contributors
// SPDX-License-Identifier: Apache-2.0
#include <binder/Binder.h>
#include <binder/Parcel.h>
#include <gui/BufferItem.h>
#include <gui/BufferQueueConsumer.h>
#include <gui/BufferQueueCore.h>
#include <gui/BufferQueueProducer.h>
#include <gui/IConsumerListener.h>
#include <gui/IProducerListener.h>
#include <gtest/gtest.h>
#include <system/window.h>

namespace android {
namespace {
class QueueConsumer : public BnConsumerListener {
public:
    void onDisconnect() override { events.push_back(1); }
    void onFrameAvailable(const BufferItem&) override { events.push_back(2); }
    void onFrameReplaced(const BufferItem&) override { events.push_back(3); }
    void onBuffersReleased() override { events.push_back(4); }
    void onSidebandStreamChanged() override { events.push_back(5); }
    std::vector<int> events;
};
class QueueProducer : public BnProducerListener {
public:
    bool needsReleaseNotify() override { return false; }
    void onBufferReleased() override { ++ordinary; }
    void onBufferReleasedWithFence(const sp<Fence>& fence, uint64_t id, bool flag) override {
        ++fences; lastFence = fence; lastId = id; lastFlag = flag;
        auto c = consumer.promote();
        if (c != nullptr) {
            c->events.push_back(6);
            // Re-enter the queue to prove the callback runs without its mutex.
            reentrantStatus = queue->setDefaultBufferSize(1, 1);
        }
    }
    wp<QueueConsumer> consumer;
    sp<IGraphicBufferConsumer> queue;
    int ordinary = 0, fences = 0;
    sp<Fence> lastFence;
    uint64_t lastId = 0;
    bool lastFlag = false;
    status_t reentrantStatus = UNKNOWN_ERROR;
};
class ListenerRelay : public BBinder {
public:
    explicit ListenerRelay(const sp<IBinder>& target) : mTarget(target) {}
    std::vector<uint32_t> codes, flags;
protected:
    status_t onTransact(uint32_t code, const Parcel& data, Parcel* reply, uint32_t flag) override {
        codes.push_back(code); flags.push_back(flag);
        return mTarget->transact(code, data, reply, flag);
    }
private:
    sp<IBinder> mTarget;
};

class QueuedFenceTest : public testing::Test {
protected:
    void SetUp() override {
        core = new BufferQueueCore;
        producer = new BufferQueueProducer(core);
        consumer = new BufferQueueConsumer(core);
        receiver = new QueueConsumer;
        ASSERT_EQ(NO_ERROR, consumer->connect(receiver, false));
        listener = new QueueProducer;
        listener->consumer = receiver;
        listener->queue = consumer;
        IGraphicBufferProducer::QueueBufferOutput out;
        ASSERT_EQ(NO_ERROR, producer->connect(listener, NATIVE_WINDOW_API_CPU, false, &out));
        ASSERT_EQ(NO_ERROR, producer->setAsyncMode(true));
    }
    void TearDown() override { if (listener != nullptr) listener->queue.clear(); }
    sp<GraphicBuffer> buffer(uint64_t usage) {
        sp<GraphicBuffer> b = new GraphicBuffer;
        b->width = b->height = b->stride = b->layerCount = 1;
        b->format = HAL_PIXEL_FORMAT_RGBA_8888;
        b->usage = usage;
        return b;
    }
    void enqueue(const sp<GraphicBuffer>& b, const sp<Fence>& fence, int* slot,
                 IGraphicBufferProducer::QueueBufferOutput* out) {
        ASSERT_EQ(NO_ERROR, producer->attachBuffer(slot, b));
        IGraphicBufferProducer::QueueBufferInput input(0, false, HAL_DATASPACE_UNKNOWN,
                Rect::EMPTY_RECT, NATIVE_WINDOW_SCALING_MODE_FREEZE, 0, fence);
        ASSERT_EQ(NO_ERROR, producer->queueBuffer(*slot, input, out));
    }
    sp<BufferQueueCore> core;
    sp<BufferQueueProducer> producer;
    sp<BufferQueueConsumer> consumer;
    sp<QueueConsumer> receiver;
    sp<QueueProducer> listener;
};

TEST_F(QueuedFenceTest, SpecialReplacementDetachesAndNotifiesAfterFrameCallback) {
    sp<GraphicBuffer> first = buffer(0x100000000ULL), second = buffer(0);
    sp<Fence> acquire = new Fence;
    int firstSlot, secondSlot;
    IGraphicBufferProducer::QueueBufferOutput out;
    enqueue(first, acquire, &firstSlot, &out);
    EXPECT_EQ(0, listener->fences);
    enqueue(second, Fence::NO_FENCE, &secondSlot, &out);
    ASSERT_TRUE(out.bufferReplaced);
    EXPECT_EQ(1, listener->fences);
    EXPECT_EQ(0, listener->ordinary);
    EXPECT_EQ(first->getId(), listener->lastId);
    EXPECT_EQ(Fence::NO_FENCE.get(), listener->lastFence.get());
    EXPECT_NE(acquire.get(), listener->lastFence.get());
    EXPECT_TRUE(listener->lastFlag);
    EXPECT_EQ(NO_ERROR, listener->reentrantStatus);
    EXPECT_EQ((std::vector<int>{2, 3, 6}), receiver->events);
    int thirdSlot;
    ASSERT_EQ(NO_ERROR, producer->attachBuffer(&thirdSlot, buffer(0)));
    EXPECT_EQ(firstSlot, thirdSlot); // Free slot is detached, not merely a free buffer.
}

TEST_F(QueuedFenceTest, OrdinaryReplacementPreservesBufferCacheAndDoesNotSendFence) {
    int firstSlot, secondSlot, thirdSlot;
    IGraphicBufferProducer::QueueBufferOutput out;
    enqueue(buffer(0), Fence::NO_FENCE, &firstSlot, &out);
    enqueue(buffer(0), Fence::NO_FENCE, &secondSlot, &out);
    EXPECT_TRUE(out.bufferReplaced);
    EXPECT_EQ(0, listener->fences);
    EXPECT_EQ((std::vector<int>{2, 3}), receiver->events);
    ASSERT_EQ(NO_ERROR, producer->attachBuffer(&thirdSlot, buffer(0)));
    EXPECT_NE(firstSlot, thirdSlot);
}

TEST_F(QueuedFenceTest, DisconnectDeliversExistingCallbacksInAospOrder) {
    EXPECT_EQ(NO_ERROR, producer->disconnect(NATIVE_WINDOW_API_CPU));
    EXPECT_EQ((std::vector<int>{4, 1}), receiver->events);
    EXPECT_EQ(0, listener->fences);
}

TEST_F(QueuedFenceTest, ConsumerBinderPreservesDisconnectAndFrameTransactionNumbers) {
    sp<ListenerRelay> relay = new ListenerRelay(IInterface::asBinder(receiver));
    sp<IConsumerListener> proxy = interface_cast<IConsumerListener>(relay);
    BufferItem item;
    proxy->onDisconnect();
    proxy->onFrameAvailable(item);
    proxy->onFrameReplaced(item);
    proxy->onBuffersReleased();
    proxy->onSidebandStreamChanged();
    EXPECT_EQ((std::vector<int>{1, 2, 3, 4, 5}), receiver->events);
    EXPECT_EQ((std::vector<uint32_t>{1, 2, 3, 4, 5}), relay->codes);
    for (uint32_t flags : relay->flags) EXPECT_EQ(IBinder::FLAG_ONEWAY, flags);
}
} // namespace
} // namespace android
