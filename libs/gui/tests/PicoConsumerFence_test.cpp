// Copyright 2026 Picomisu contributors
// SPDX-License-Identifier: Apache-2.0
#include <binder/Binder.h>
#include <binder/Parcel.h>
#include <gui/BufferItem.h>
#include <gui/BufferQueueConsumer.h>
#include <gui/BufferQueueCore.h>
#include <gui/BufferQueueProducer.h>
#include <gui/ConsumerBase.h>
#include <gui/IProducerListener.h>
#include <gui/IConsumerListener.h>
#include <gtest/gtest.h>
#include <system/window.h>

#include <future>
#include <thread>

namespace android {
namespace {
constexpr uint64_t kMask = 0xf00000000ULL;

class IdleFenceConsumer : public BnConsumerListener {
public:
    void onFrameAvailable(const BufferItem&) override {}
    void onBuffersReleased() override {}
    void onSidebandStreamChanged() override {}
};

class ReleaseRecorder : public BnProducerListener {
public:
    bool needsReleaseNotify() override { return false; }
    void onBufferReleased() override { ++ordinary; }
    void onBufferReleasedWithFence(const sp<Fence>& fence, uint64_t id, bool flag) override {
        ++fences;
        lastFence = fence;
        lastId = id;
        lastFlag = flag;
        if (producer != nullptr) {
            int value;
            queryResult = producer->query(NATIVE_WINDOW_WIDTH, &value);
        }
    }
    sp<IGraphicBufferProducer> producer;
    int ordinary = 0, fences = 0;
    status_t queryResult = UNKNOWN_ERROR;
    sp<Fence> lastFence;
    uint64_t lastId = 0;
    bool lastFlag = true;
};

class ConsumerRelay : public BBinder {
public:
    int calls = 0;
protected:
    status_t onTransact(uint32_t, const Parcel&, Parcel*, uint32_t) override {
        ++calls;
        return UNKNOWN_TRANSACTION;
    }
};

class LatchConsumer : public ConsumerBase {
public:
    explicit LatchConsumer(const sp<IGraphicBufferConsumer>& consumer) : ConsumerBase(consumer) {}
    int slot() { Mutex::Autolock lock(mMutex); return getLatchAcquireSlotLocked(); }
    void fallback(void (*callback)(int), int argument) {
        mPicoFrameCallback = callback;
        mPicoFrameCallbackArgument = argument;
    }
    void notifyFrames() {
        BufferItem item;
        onFrameAvailable(item);
        onFrameReplaced(item);
    }
    size_t callbackOffset() const {
        return reinterpret_cast<const char*>(&mPicoFrameCallback) - reinterpret_cast<const char*>(this);
    }
    size_t argumentOffset() const {
        return reinterpret_cast<const char*>(&mPicoFrameCallbackArgument) - reinterpret_cast<const char*>(this);
    }
    status_t acquire(BufferItem* item) {
        Mutex::Autolock lock(mMutex);
        return acquireBufferLocked(item, 0);
    }
};
class OverriddenLatchConsumer : public LatchConsumer {
public:
    using LatchConsumer::LatchConsumer;
    int getLatchAcquireSlotLocked() override { return 123; }
};
class FrameRecorder : public ConsumerBase::FrameAvailableListener {
public:
    void onFrameAvailable(const BufferItem&) override { ++available; }
    void onFrameReplaced(const BufferItem&) override { ++replaced; }
    int available = 0, replaced = 0;
};
static int fallbackCalls, fallbackArgument;
static LatchConsumer* fallbackConsumer;
static void recordFallback(int argument) {
    ++fallbackCalls;
    fallbackArgument = argument;
    // Re-enter the API that takes mFrameAvailableMutex: callback must be unlocked.
    fallbackConsumer->setFrameAvailableListener(wp<ConsumerBase::FrameAvailableListener>());
}
}

// This fixture's friend access is only for waking an actual wait while holding
// the queue mutex. Ordinary tests use public producer/consumer methods.
class PicoConsumerFenceTest : public testing::Test {
protected:
    void SetUp() override {
        core = new BufferQueueCore;
        producer = new BufferQueueProducer(core);
        consumer = new BufferQueueConsumer(core);
        ASSERT_EQ(NO_ERROR, consumer->connect(new IdleFenceConsumer, false));
        IGraphicBufferProducer::QueueBufferOutput output;
        listener = new ReleaseRecorder;
        listener->producer = producer;
        ASSERT_EQ(NO_ERROR, producer->connect(listener, NATIVE_WINDOW_API_CPU, false, &output));
        ASSERT_EQ(NO_ERROR, producer->allowAllocation(false));
    }
    void TearDown() override { if (listener != nullptr) listener->producer.clear(); }
    sp<GraphicBuffer> buffer(uint64_t usage) {
        sp<GraphicBuffer> result = new GraphicBuffer;
        result->width = result->height = result->stride = result->layerCount = 1;
        result->format = HAL_PIXEL_FORMAT_RGBA_8888;
        result->usage = usage;
        return result;
    }
    void attachFree(const sp<GraphicBuffer>& buffer, int* slot, const sp<Fence>& fence = Fence::NO_FENCE) {
        ASSERT_EQ(NO_ERROR, consumer->attachBuffer(slot, buffer));
        ASSERT_EQ(NO_ERROR, consumer->releaseBuffer(*slot, 0, fence, EGL_NO_DISPLAY, EGL_NO_SYNC_KHR));
    }
    status_t dequeue(int* slot, sp<Fence>* fence) {
        return producer->dequeueBuffer(slot, fence, 1, 1, HAL_PIXEL_FORMAT_RGBA_8888, 0, nullptr, nullptr);
    }
    void waitWithWake(int slot, sp<Fence>* fence, bool notify, uint64_t id) {
        std::promise<void> entered;
        auto ready = entered.get_future();
        std::thread wait([&] {
            BufferQueueCore::MutexLock lock(core->mMutex);
            entered.set_value();
            producer->waitForFenceReadyBufferLocked(slot, fence);
        });
        ready.wait();
        if (notify) {
            EXPECT_EQ(NO_ERROR, consumer->notifyFenceReady(*fence, id, slot));
        } else {
            // This lock can only be obtained once wait has released the mutex.
            BufferQueueCore::MutexLock lock(core->mMutex);
            pthread_cond_broadcast(&core->mPicoFenceCondition);
        }
        wait.join();
    }
    sp<BufferQueueCore> core;
    sp<BufferQueueProducer> producer;
    sp<BufferQueueConsumer> consumer;
    sp<ReleaseRecorder> listener;
};

TEST_F(PicoConsumerFenceTest, LocalReadyFenceIsConsumedByDequeue) {
    sp<GraphicBuffer> b = buffer(0x200000000ULL);
    int attached, dequeued;
    attachFree(b, &attached);
    sp<Fence> ready = new Fence;
    ASSERT_EQ(NO_ERROR, consumer->notifyFenceReady(ready, b->getId(), attached));
    sp<Fence> output;
    ASSERT_EQ(IGraphicBufferProducer::BUFFER_NEEDS_REALLOCATION, dequeue(&dequeued, &output));
    EXPECT_EQ(attached, dequeued);
    EXPECT_EQ(ready.get(), output.get());
    EXPECT_EQ(0u, b->getUsage() & kMask);
}

TEST_F(PicoConsumerFenceTest, StaleBufferIdDoesNotReplaceReadyFence) {
    sp<GraphicBuffer> b = buffer(0x200000000ULL);
    int slot, dequeued;
    attachFree(b, &slot);
    sp<Fence> expected = new Fence, stale = new Fence;
    ASSERT_EQ(NO_ERROR, consumer->notifyFenceReady(expected, b->getId(), slot));
    ASSERT_EQ(NO_ERROR, consumer->notifyFenceReady(stale, b->getId() + 1, slot));
    sp<Fence> output;
    ASSERT_EQ(IGraphicBufferProducer::BUFFER_NEEDS_REALLOCATION, dequeue(&dequeued, &output));
    EXPECT_EQ(expected.get(), output.get());
}

TEST_F(PicoConsumerFenceTest, LocalNotifyLogsAndIgnoresInvalidArguments) {
    // The factory logs every rejection and returns NO_ERROR.
    EXPECT_EQ(NO_ERROR, consumer->notifyFenceReady(Fence::NO_FENCE, 1, -1));
    EXPECT_EQ(NO_ERROR, consumer->notifyFenceReady(Fence::NO_FENCE, 1, BufferQueueDefs::NUM_BUFFER_SLOTS));
    EXPECT_EQ(NO_ERROR, consumer->notifyFenceReady(nullptr, 1, 0));
    EXPECT_EQ(NO_ERROR, consumer->notifyFenceReady(Fence::NO_FENCE, 1, 0));
}

TEST_F(PicoConsumerFenceTest, BinderProxyUsesLocalDefaultAndDoesNotSendNewCommand) {
    sp<ConsumerRelay> relay = new ConsumerRelay;
    sp<IGraphicBufferConsumer> proxy = interface_cast<IGraphicBufferConsumer>(relay);
    EXPECT_EQ(NO_ERROR, proxy->notifyFenceReady(Fence::NO_FENCE, UINT64_MAX, 50));
    EXPECT_EQ(0, relay->calls);
    Parcel data, reply;
    data.writeInterfaceToken(consumer->getInterfaceDescriptor());
    EXPECT_EQ(UNKNOWN_TRANSACTION, IInterface::asBinder(consumer)->transact(21, data, &reply));
}

TEST_F(PicoConsumerFenceTest, TimeoutMarksBufferAndPreservesOrdinaryReleaseFence) {
    sp<GraphicBuffer> b = buffer(0x200000000ULL);
    sp<Fence> original = new Fence;
    int slot, dequeued;
    attachFree(b, &slot, original);
    sp<Fence> output;
    ASSERT_EQ(IGraphicBufferProducer::BUFFER_NEEDS_REALLOCATION, dequeue(&dequeued, &output));
    EXPECT_EQ(original.get(), output.get());
    EXPECT_EQ(0x800000000ULL, b->getUsage() & kMask);
}

TEST_F(PicoConsumerFenceTest, ConsumedReadyStateIsNotReused) {
    sp<GraphicBuffer> b = buffer(0x200000000ULL);
    int slot, dequeued;
    attachFree(b, &slot);
    sp<Fence> ready = new Fence, output;
    ASSERT_EQ(NO_ERROR, consumer->notifyFenceReady(ready, b->getId(), slot));
    ASSERT_EQ(IGraphicBufferProducer::BUFFER_NEEDS_REALLOCATION, dequeue(&dequeued, &output));
    ASSERT_EQ(NO_ERROR, producer->cancelBuffer(dequeued, Fence::NO_FENCE));
    b->usage = 0x200000000ULL;
    ASSERT_EQ(NO_ERROR, dequeue(&dequeued, &output));
    EXPECT_NE(ready.get(), output.get());
    EXPECT_EQ(0x800000000ULL, b->getUsage() & kMask);
}

TEST_F(PicoConsumerFenceTest, WakeWithoutReadyClearsTheMarker) {
    // Factory "wait fence missed": no marker, the release fence is kept.
    sp<GraphicBuffer> b = buffer(0x200000000ULL);
    int slot;
    ASSERT_EQ(NO_ERROR, consumer->attachBuffer(&slot, b));
    sp<Fence> output = Fence::NO_FENCE;
    waitWithWake(slot, &output, false, b->getId());
    EXPECT_EQ(0u, b->getUsage() & kMask);
}

TEST_F(PicoConsumerFenceTest, ReadyNotificationWakesAWaitingProducer) {
    sp<GraphicBuffer> b = buffer(0x200000000ULL);
    int slot;
    ASSERT_EQ(NO_ERROR, consumer->attachBuffer(&slot, b));
    sp<Fence> output = new Fence;
    sp<Fence> expected = output;
    waitWithWake(slot, &output, true, b->getId());
    EXPECT_EQ(expected.get(), output.get());
    // Factory "wait fence hit": the fence only arrived while waiting.
    EXPECT_EQ(0x400000000ULL, b->getUsage() & kMask);
}

TEST_F(PicoConsumerFenceTest, SpecialReleaseDetachesAndNotifiesWithNoQueueMutexHeld) {
    sp<GraphicBuffer> b = buffer(0x100000000ULL);
    int slot;
    ASSERT_EQ(NO_ERROR, consumer->attachBuffer(&slot, b));
    sp<Fence> fence = new Fence;
    ASSERT_EQ(NO_ERROR, consumer->releaseBuffer(slot, 0, fence, EGL_NO_DISPLAY, EGL_NO_SYNC_KHR));
    EXPECT_EQ(1, listener->fences);
    EXPECT_EQ(0, listener->ordinary);
    EXPECT_EQ(fence.get(), listener->lastFence.get());
    EXPECT_EQ(b->getId(), listener->lastId);
    EXPECT_FALSE(listener->lastFlag);
    EXPECT_EQ(NO_ERROR, listener->queryResult);
    EXPECT_EQ(BAD_VALUE, consumer->releaseBuffer(slot, 0, fence, EGL_NO_DISPLAY, EGL_NO_SYNC_KHR));
    EXPECT_EQ(1, listener->fences);
}

TEST_F(PicoConsumerFenceTest, ConsumerBaseRetainsLastSuccessfulAcquireSlot) {
    sp<BufferQueueCore> localCore = new BufferQueueCore;
    sp<BufferQueueProducer> localProducer = new BufferQueueProducer(localCore);
    sp<BufferQueueConsumer> localConsumer = new BufferQueueConsumer(localCore);
    sp<LatchConsumer> tracked = new LatchConsumer(localConsumer);
    EXPECT_EQ(-1, tracked->slot());
    IGraphicBufferProducer::QueueBufferOutput output;
    ASSERT_EQ(NO_ERROR, localProducer->connect(new DummyProducerListener,
                NATIVE_WINDOW_API_CPU, false, &output));
    sp<GraphicBuffer> b = buffer(0);
    int slot;
    ASSERT_EQ(NO_ERROR, localProducer->attachBuffer(&slot, b));
    IGraphicBufferProducer::QueueBufferInput input(0, false, HAL_DATASPACE_UNKNOWN,
            Rect::EMPTY_RECT, NATIVE_WINDOW_SCALING_MODE_FREEZE, 0, Fence::NO_FENCE);
    ASSERT_EQ(NO_ERROR, localProducer->queueBuffer(slot, input, &output));
    BufferItem item;
    ASSERT_EQ(NO_ERROR, tracked->acquire(&item));
    EXPECT_EQ(slot, tracked->slot());
    EXPECT_EQ(IGraphicBufferConsumer::NO_BUFFER_AVAILABLE, tracked->acquire(&item));
    EXPECT_EQ(slot, tracked->slot());
    tracked->abandon();
    EXPECT_EQ(slot, tracked->slot());
}

TEST_F(PicoConsumerFenceTest, ReadyFreeBufferIsPreferredAfterModeActivation) {
    sp<GraphicBuffer> first = buffer(0x200000000ULL), second = buffer(0x200000000ULL);
    int firstSlot, secondSlot, dequeued;
    sp<Fence> firstReady = new Fence, secondReady = new Fence, output;
    attachFree(first, &firstSlot);
    ASSERT_EQ(NO_ERROR, consumer->notifyFenceReady(firstReady, first->getId(), firstSlot));
    ASSERT_EQ(IGraphicBufferProducer::BUFFER_NEEDS_REALLOCATION, dequeue(&dequeued, &output));
    ASSERT_EQ(NO_ERROR, producer->cancelBuffer(dequeued, Fence::NO_FENCE));
    first->usage = 0x200000000ULL;
    attachFree(second, &secondSlot);
    ASSERT_EQ(NO_ERROR, consumer->notifyFenceReady(secondReady, second->getId(), secondSlot));
    ASSERT_EQ(IGraphicBufferProducer::BUFFER_NEEDS_REALLOCATION, dequeue(&dequeued, &output));
    EXPECT_EQ(secondSlot, dequeued);
    EXPECT_EQ(secondReady.get(), output.get());
    EXPECT_EQ(0x200000000ULL, first->getUsage() & kMask);
}

TEST(PicoConsumerBaseAbi, FallbackRequiresCallbackAndNonSentinelArgument) {
    sp<BufferQueueCore> core = new BufferQueueCore;
    sp<LatchConsumer> tracked = new LatchConsumer(new BufferQueueConsumer(core));
    fallbackConsumer = tracked.get();
    fallbackCalls = 0;
    tracked->notifyFrames();
    tracked->fallback(nullptr, 7);
    tracked->notifyFrames();
    tracked->fallback(recordFallback, -1);
    tracked->notifyFrames();
    EXPECT_EQ(0, fallbackCalls);
    tracked->fallback(recordFallback, 7);
    tracked->notifyFrames();
    EXPECT_EQ(2, fallbackCalls);
    EXPECT_EQ(7, fallbackArgument);
    fallbackConsumer = nullptr;
}

TEST(PicoConsumerBaseAbi, LiveListenerTakesPriorityAndExpiredListenerUsesFallback) {
    sp<BufferQueueCore> core = new BufferQueueCore;
    sp<LatchConsumer> tracked = new LatchConsumer(new BufferQueueConsumer(core));
    fallbackConsumer = tracked.get();
    fallbackCalls = 0;
    tracked->fallback(recordFallback, 0);
    sp<FrameRecorder> listener = new FrameRecorder;
    tracked->setFrameAvailableListener(listener);
    tracked->notifyFrames();
    EXPECT_EQ(1, listener->available);
    EXPECT_EQ(1, listener->replaced);
    EXPECT_EQ(0, fallbackCalls);
    listener.clear();
    tracked->notifyFrames();
    EXPECT_EQ(2, fallbackCalls);
    EXPECT_EQ(0, fallbackArgument);
    fallbackConsumer = nullptr;
}

TEST(PicoConsumerBaseAbi, LatchGetterDispatchesVirtually) {
    sp<BufferQueueCore> core = new BufferQueueCore;
    sp<LatchConsumer> tracked = new OverriddenLatchConsumer(new BufferQueueConsumer(core));
    EXPECT_EQ(123, tracked->slot());
}

TEST(PicoConsumerBaseAbi, ObservedFactoryTailOffsetsMatch) {
    sp<BufferQueueCore> core = new BufferQueueCore;
    sp<LatchConsumer> tracked = new LatchConsumer(new BufferQueueConsumer(core));
    EXPECT_EQ(sizeof(void*) == 8 ? 1672u : 1068u, tracked->callbackOffset());
    EXPECT_EQ(sizeof(void*) == 8 ? 1680u : 1072u, tracked->argumentOffset());
    auto baseOffset = reinterpret_cast<char*>(static_cast<RefBase*>(tracked.get())) -
            reinterpret_cast<char*>(tracked.get());
    EXPECT_EQ(sizeof(void*) == 8 ? 1688 : 1076, baseOffset);
}

} // namespace android
