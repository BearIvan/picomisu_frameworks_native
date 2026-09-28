// Copyright 2026 Picomisu contributors
// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <renderengine/mock/RenderEngine.h>
#include <ui/GraphicBuffer.h>
#include <unistd.h>
#include "BufferLayerConsumer.h"
#include "mock/gui/MockGraphicBufferConsumer.h"

namespace android {
using testing::_;
using testing::NiceMock;
using testing::Return;

class BufferLayerConsumerFenceTest : public testing::Test {
protected:
    void SetUp() override {
        consumer = new NiceMock<mock::GraphicBufferConsumer>;
        tracked = new BufferLayerConsumer(consumer, engine, 0, nullptr);
        buffer = new GraphicBuffer;
        int fds[2];
        ASSERT_EQ(0, pipe(fds));
        close(fds[1]);
        fence = new Fence(fds[0]);
    }
    void seedPending() {
        tracked->mPendingRelease.isPending = true;
        tracked->mPendingRelease.currentTexture = slot;
        tracked->mPendingRelease.graphicBuffer = buffer;
        tracked->mSlots[slot].mGraphicBuffer = buffer;
        // Exercise first-fence insertion. A pipe is not a GPU sync fence and
        // must not be used to assert sync-file merge/timestamp semantics.
        tracked->mSlots[slot].mFence.clear();
    }
    sp<Fence> pendingFence() { return tracked->mSlots[slot].mFence; }
    static constexpr int slot = 3;
    NiceMock<renderengine::mock::RenderEngine> engine;
    sp<NiceMock<mock::GraphicBufferConsumer>> consumer;
    sp<BufferLayerConsumer> tracked;
    sp<GraphicBuffer> buffer;
    sp<Fence> fence;
};

TEST_F(BufferLayerConsumerFenceTest, ForwardsBufferIdAndNullAsZeroWithoutPendingRelease) {
    EXPECT_CALL(*consumer, notifyFenceReady(fence, buffer->getId(), slot)).WillOnce(Return(NO_ERROR));
    tracked->notifyFenceReady(fence, buffer, slot);
    EXPECT_CALL(*consumer, notifyFenceReady(fence, 0, slot)).WillOnce(Return(NO_ERROR));
    tracked->notifyFenceReady(fence, nullptr, slot);
}

TEST_F(BufferLayerConsumerFenceTest, MatchingPendingBufferReceivesReleaseFence) {
    seedPending();
    EXPECT_CALL(*consumer, notifyFenceReady(fence, buffer->getId(), slot)).WillOnce(Return(NO_ERROR));
    tracked->notifyFenceReady(fence, buffer, slot);
    EXPECT_EQ(fence.get(), pendingFence().get());
}

TEST_F(BufferLayerConsumerFenceTest, DifferentBufferIdDoesNotChangePendingFence) {
    seedPending();
    sp<GraphicBuffer> other = new GraphicBuffer;
    EXPECT_CALL(*consumer, notifyFenceReady(fence, other->getId(), slot)).WillOnce(Return(NO_ERROR));
    tracked->notifyFenceReady(fence, other, slot);
    EXPECT_EQ(nullptr, pendingFence().get());
}

TEST_F(BufferLayerConsumerFenceTest, DifferentSlotDoesNotChangePendingFence) {
    seedPending();
    EXPECT_CALL(*consumer, notifyFenceReady(fence, buffer->getId(), slot + 1)).WillOnce(Return(NO_ERROR));
    tracked->notifyFenceReady(fence, buffer, slot + 1);
    EXPECT_EQ(nullptr, pendingFence().get());
}

TEST_F(BufferLayerConsumerFenceTest, LateNotificationAfterAbandonDoesNotCallConsumer) {
    tracked->abandon();
    EXPECT_CALL(*consumer, notifyFenceReady(_, _, _)).Times(0);
    tracked->notifyFenceReady(fence, buffer, slot);
}
} // namespace android
