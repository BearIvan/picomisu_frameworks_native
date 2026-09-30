// Copyright 2026 Picomisu contributors
// SPDX-License-Identifier: Apache-2.0
#include <atomic>
#include <chrono>
#include <future>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <gui/IProducerListener.h>
#include "ColorLayer.h"
#include "DisplayHardware/VirtualDisplaySurface.h"
#include "TestableScheduler.h"
#include "TestableSurfaceFlinger.h"
#include "mock/DisplayHardware/MockComposer.h"
#include "mock/gui/MockGraphicBufferConsumer.h"
#include "mock/gui/MockGraphicBufferProducer.h"

namespace android {
using testing::_;
using testing::NiceMock;
using testing::Return;
using testing::DoAll;
using testing::SetArgPointee;
using testing::SaveArg;

class FenceRecordingLayer : public ColorLayer {
public:
    explicit FenceRecordingLayer(SurfaceFlinger* flinger)
          : ColorLayer(LayerCreationArgs(flinger, nullptr, String8("fence-test"), 1, 1, 0,
                                         LayerMetadata())) {}
    void notifyFenceReady(const sp<Fence>& fence, const sp<GraphicBuffer>& buffer, int slot) override {
        ++notifications;
        lastFence = fence;
        lastBuffer = buffer;
        lastSlot = slot;
        if (onNotify) onNotify();
    }
    void releasePendingBuffer(nsecs_t time) override {
        ++releases;
        EXPECT_GT(time, 0);
        EXPECT_GT(notifications, 0);
    }
    int notifications = 0, releases = 0, lastSlot = -1;
    sp<Fence> lastFence;
    sp<GraphicBuffer> lastBuffer;
    std::function<void()> onNotify;
};

class VirtualDisplaySurfaceFenceTest : public testing::Test {
protected:
    void SetUp() override {
        flinger.mutableScheduler().reset(new TestableScheduler(flinger.mutableRefreshRateConfigs()));
        flinger.setupComposer(std::make_unique<NiceMock<Hwc2::mock::Composer>>());
        sink = new NiceMock<mock::GraphicBufferProducer>;
        scratch = new NiceMock<mock::GraphicBufferProducer>;
        consumer = new NiceMock<mock::GraphicBufferConsumer>;
        ON_CALL(*sink, query(_, _)).WillByDefault(DoAll(SetArgPointee<1>(1), Return(NO_ERROR)));
        display = new VirtualDisplaySurface(flinger.getHwComposer(), std::nullopt,
                                             sink, scratch, consumer, "fence-test",
                                             false /* secure */);
        layer = new FenceRecordingLayer(flinger.mFlinger.get());
        layer->getCompositionLayer()->editState().frontEnd.bufferSlot = 3;
        buffer = new GraphicBuffer;
    }
    void TearDown() override {
        display.clear();
        layer.clear();
    }
    sp<IGraphicBufferProducer> producer() { return display; }
    void recreate(int usage, bool secure, bool twoBuffers) {
        display.clear();
        ON_CALL(*sink, query(NATIVE_WINDOW_CONSUMER_USAGE_BITS, _))
                .WillByDefault(DoAll(SetArgPointee<1>(usage), Return(NO_ERROR)));
        display = new VirtualDisplaySurface(flinger.getHwComposer(), std::nullopt,
                                             sink, scratch, consumer, "fence-test", secure, twoBuffers);
    }
    uint64_t outputUsage() const { return display->mOutputUsage; }
    void recomputeUsage(uint64_t ignored) { display->setOutputUsage(ignored); }
    sp<IProducerListener> connectListener() {
        sp<IProducerListener> listener;
        EXPECT_CALL(*sink, connect(_, NATIVE_WINDOW_API_EGL, false, _))
                .WillOnce(DoAll(SaveArg<0>(&listener), Return(NO_ERROR)));
        IGraphicBufferProducer::QueueBufferOutput output;
        EXPECT_EQ(NO_ERROR, producer()->connect(nullptr, NATIVE_WINDOW_API_EGL, false, &output));
        return listener;
    }
    TestableSurfaceFlinger flinger;
    sp<NiceMock<mock::GraphicBufferProducer>> sink, scratch;
    sp<NiceMock<mock::GraphicBufferConsumer>> consumer;
    sp<VirtualDisplaySurface> display;
    sp<FenceRecordingLayer> layer;
    sp<GraphicBuffer> buffer;
};

TEST_F(VirtualDisplaySurfaceFenceTest, DefaultsAndInvalidRegistration) {
    EXPECT_TRUE(display->getMultiLayerFlag());
    display->setMultiLayerFlag(false);
    EXPECT_FALSE(display->getMultiLayerFlag());
    EXPECT_EQ(BAD_VALUE, display->setSingleLayer(nullptr, buffer));
    EXPECT_EQ(BAD_VALUE, display->setSingleLayer(layer, nullptr));
    EXPECT_EQ(BAD_VALUE, display->handleSingleLayerFence(Fence::NO_FENCE, UINT64_MAX, false));
    EXPECT_EQ(NO_ERROR, display->handleSingleLayerFence(Fence::NO_FENCE, buffer->getId(), false));
}

TEST_F(VirtualDisplaySurfaceFenceTest, RepeatedBufferNotifiesOnlyAfterLastReleaseUsingLatestSlot) {
    ASSERT_EQ(NO_ERROR, display->setSingleLayer(layer, buffer));
    layer->getCompositionLayer()->editState().frontEnd.bufferSlot = 7;
    ASSERT_EQ(NO_ERROR, display->setSingleLayer(layer, buffer));
    EXPECT_EQ(NO_ERROR, display->handleSingleLayerFence(Fence::NO_FENCE, buffer->getId(), true));
    EXPECT_EQ(0, layer->notifications);
    EXPECT_EQ(0, layer->releases);
    EXPECT_EQ(NO_ERROR, display->handleSingleLayerFence(Fence::NO_FENCE, buffer->getId(), false));
    EXPECT_EQ(1, layer->notifications);
    EXPECT_EQ(7, layer->lastSlot);
    EXPECT_EQ(buffer, layer->lastBuffer);
    EXPECT_EQ(0, layer->releases);
}

TEST_F(VirtualDisplaySurfaceFenceTest, LatestLayerOwnsRepeatedBuffer) {
    sp<FenceRecordingLayer> latest = new FenceRecordingLayer(flinger.mFlinger.get());
    latest->getCompositionLayer()->editState().frontEnd.bufferSlot = 9;
    ASSERT_EQ(NO_ERROR, display->setSingleLayer(layer, buffer));
    ASSERT_EQ(NO_ERROR, display->setSingleLayer(latest, buffer));
    display->handleSingleLayerFence(Fence::NO_FENCE, buffer->getId(), false);
    display->handleSingleLayerFence(Fence::NO_FENCE, buffer->getId(), true);
    EXPECT_EQ(0, layer->notifications);
    EXPECT_EQ(1, latest->notifications);
    EXPECT_EQ(1, latest->releases);
    EXPECT_EQ(9, latest->lastSlot);
}

TEST_F(VirtualDisplaySurfaceFenceTest, UnknownIdDoesNotConsumeRegisteredFrame) {
    ASSERT_EQ(NO_ERROR, display->setSingleLayer(layer, buffer));
    sp<GraphicBuffer> other = new GraphicBuffer;
    EXPECT_EQ(BAD_VALUE, display->handleSingleLayerFence(Fence::NO_FENCE, other->getId(), false));
    EXPECT_EQ(NO_ERROR, display->handleSingleLayerFence(Fence::NO_FENCE, buffer->getId(), false));
    EXPECT_EQ(1, layer->notifications);
}

TEST_F(VirtualDisplaySurfaceFenceTest, ReleaseCallbackCanRegisterAgainOutsideMutex) {
    ASSERT_EQ(NO_ERROR, display->setSingleLayer(layer, buffer));
    layer->onNotify = [&] { EXPECT_EQ(NO_ERROR, display->setSingleLayer(layer, buffer)); };
    display->handleSingleLayerFence(Fence::NO_FENCE, buffer->getId(), false);
    layer->onNotify = {};
    display->handleSingleLayerFence(Fence::NO_FENCE, buffer->getId(), false);
    EXPECT_EQ(2, layer->notifications);
}

TEST_F(VirtualDisplaySurfaceFenceTest, DisconnectNotifiesSnapshotAndDisablesListener) {
    auto listener = connectListener();
    ASSERT_NE(nullptr, listener.get());
    ASSERT_EQ(NO_ERROR, display->setSingleLayer(layer, buffer));
    EXPECT_CALL(*sink, disconnect(NATIVE_WINDOW_API_EGL, IGraphicBufferProducer::DisconnectMode::Api))
            .WillOnce(Return(NO_ERROR));
    EXPECT_EQ(NO_ERROR, producer()->disconnect(NATIVE_WINDOW_API_EGL));
    EXPECT_EQ(1, layer->notifications);
    EXPECT_EQ(Fence::NO_FENCE, layer->lastFence);
    listener->onBufferReleasedWithFence(Fence::NO_FENCE, buffer->getId(), false);
    EXPECT_EQ(1, layer->notifications);
    // Snapshot notification preserves the outstanding registration.
    display->handleSingleLayerFence(Fence::NO_FENCE, buffer->getId(), false);
    EXPECT_EQ(2, layer->notifications);
}

TEST_F(VirtualDisplaySurfaceFenceTest, ConnectedListenerRoutesRelease) {
    auto listener = connectListener();
    ASSERT_EQ(NO_ERROR, display->setSingleLayer(layer, buffer));
    listener->onBufferReleasedWithFence(Fence::NO_FENCE, buffer->getId(), true);
    EXPECT_EQ(1, layer->notifications);
    EXPECT_EQ(1, layer->releases);
}

TEST_F(VirtualDisplaySurfaceFenceTest, RetainedListenerDoesNotKeepDisplayAlive) {
    auto listener = connectListener();
    wp<VirtualDisplaySurface> weak(display);
    display.clear();
    EXPECT_EQ(nullptr, weak.promote().get());
    listener->onBufferReleasedWithFence(Fence::NO_FENCE, buffer->getId(), false);
    EXPECT_EQ(0, layer->notifications);
}

TEST_F(VirtualDisplaySurfaceFenceTest, OutputUsageIncludesSinkUsageAndIgnoresArgument) {
    recreate(GRALLOC_USAGE_HW_TEXTURE, false, false);
    const uint64_t expected = GRALLOC_USAGE_HW_TEXTURE | GRALLOC_USAGE_HW_COMPOSER;
    EXPECT_EQ(expected, outputUsage());
    recomputeUsage(UINT64_MAX);
    EXPECT_EQ(expected, outputUsage());
}

TEST_F(VirtualDisplaySurfaceFenceTest, SecureVideoEncoderOutputAddsProtectedUsage) {
    recreate(GRALLOC_USAGE_HW_VIDEO_ENCODER, true, false);
    EXPECT_EQ(uint64_t(GRALLOC_USAGE_HW_VIDEO_ENCODER | GRALLOC_USAGE_HW_COMPOSER |
                       GRALLOC_USAGE_PROTECTED), outputUsage());
}

TEST_F(VirtualDisplaySurfaceFenceTest, ProtectionRequiresBothSecureDisplayAndEncoderSink) {
    recreate(GRALLOC_USAGE_HW_VIDEO_ENCODER, false, false);
    EXPECT_EQ(0u, outputUsage() & GRALLOC_USAGE_PROTECTED);
    recreate(GRALLOC_USAGE_HW_TEXTURE, true, false);
    EXPECT_EQ(0u, outputUsage() & GRALLOC_USAGE_PROTECTED);
}

TEST_F(VirtualDisplaySurfaceFenceTest, OutputUsageSignExtendsLegacySinkQuery) {
    recreate(static_cast<int32_t>(0x80000000u), false, false);
    EXPECT_EQ(0xffffffff80000800ULL, outputUsage());
}

TEST_F(VirtualDisplaySurfaceFenceTest, TwoSinkBuffersOptionSetsMaxDequeuedCount) {
    EXPECT_CALL(*sink, setMaxDequeuedBufferCount(2)).WillOnce(Return(NO_ERROR));
    recreate(0, false, true);
}

TEST_F(VirtualDisplaySurfaceFenceTest, LastDisplayOwnerCanBeDroppedInsideListenerCallback) {
    auto listener = connectListener();
    ASSERT_EQ(NO_ERROR, display->setSingleLayer(layer, buffer));
    wp<VirtualDisplaySurface> weak(display);
    layer->onNotify = [&] {
        display.clear();
        EXPECT_NE(nullptr, weak.promote().get());
    };
    listener->onBufferReleasedWithFence(Fence::NO_FENCE, buffer->getId(), false);
    EXPECT_EQ(1, layer->notifications);
    EXPECT_EQ(nullptr, weak.promote().get());
    layer->onNotify = {};
    listener->onBufferReleasedWithFence(Fence::NO_FENCE, buffer->getId(), false);
    EXPECT_EQ(1, layer->notifications);
}

TEST_F(VirtualDisplaySurfaceFenceTest, ConcurrentDisconnectWaitsForActiveListenerCallback) {
    using namespace std::chrono_literals;
    auto listener = connectListener();
    ASSERT_EQ(NO_ERROR, display->setSingleLayer(layer, buffer));
    std::promise<void> entered, allowed, disconnectStarted;
    auto enteredFuture = entered.get_future();
    auto allowedFuture = allowed.get_future();
    auto startedFuture = disconnectStarted.get_future();
    std::atomic<bool> callbackFinished{false};
    layer->onNotify = [&] {
        entered.set_value();
        ASSERT_EQ(std::future_status::ready, allowedFuture.wait_for(5s));
        callbackFinished = true;
    };
    auto release = std::async(std::launch::async, [&] {
        listener->onBufferReleasedWithFence(Fence::NO_FENCE, buffer->getId(), false);
    });
    EXPECT_EQ(std::future_status::ready, enteredFuture.wait_for(5s));
    auto transport = producer();
    EXPECT_CALL(*sink, disconnect(NATIVE_WINDOW_API_EGL, IGraphicBufferProducer::DisconnectMode::Api))
            .WillOnce([&](int, IGraphicBufferProducer::DisconnectMode) {
                EXPECT_TRUE(callbackFinished.load());
                return NO_ERROR;
            });
    auto disconnect = std::async(std::launch::async, [&] {
        disconnectStarted.set_value();
        return transport->disconnect(NATIVE_WINDOW_API_EGL);
    });
    EXPECT_EQ(std::future_status::ready, startedFuture.wait_for(5s));
    EXPECT_EQ(std::future_status::timeout, disconnect.wait_for(50ms));
    allowed.set_value();
    release.get();
    EXPECT_EQ(NO_ERROR, disconnect.get());
    EXPECT_EQ(1, layer->notifications);
    layer->onNotify = {};
    listener->onBufferReleasedWithFence(Fence::NO_FENCE, buffer->getId(), false);
    EXPECT_EQ(1, layer->notifications);
}

TEST_F(VirtualDisplaySurfaceFenceTest, SnapshotCallbacksCanRegisterWithoutChangingSnapshot) {
    ASSERT_EQ(NO_ERROR, display->setSingleLayer(layer, buffer));
    sp<GraphicBuffer> nextBuffer = new GraphicBuffer;
    layer->onNotify = [&] { EXPECT_EQ(NO_ERROR, display->setSingleLayer(layer, nextBuffer)); };
    EXPECT_EQ(NO_ERROR, display->clearAllSingleLayerFence());
    EXPECT_EQ(1, layer->notifications);
    layer->onNotify = {};
    EXPECT_EQ(NO_ERROR, display->handleSingleLayerFence(Fence::NO_FENCE, nextBuffer->getId(), false));
    EXPECT_EQ(2, layer->notifications);
}
} // namespace android
