// Copyright 2026 Picomisu contributors
// SPDX-License-Identifier: Apache-2.0
#include "PicoSingleLayerComposition.h"
#include "Layer.h"
#include <compositionengine/mock/RenderSurface.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <ui/GraphicBuffer.h>
#include <fcntl.h>
#include <unistd.h>
namespace android {
using testing::_;
using testing::Return;
class PicoSingleLayerCompositionTest : public testing::Test {
protected:
    testing::StrictMock<compositionengine::mock::RenderSurface> surface;
    bool fallback = false;
    sp<Layer> owner;
    base::unique_fd ready;
    std::vector<renderengine::LayerSettings> settings{1};
    sp<GraphicBuffer> buffer = new GraphicBuffer;
    void SetUp() override {
        buffer->usage = 0x100000000ULL | 0x20;
        settings[0].source.buffer.buffer = buffer;
        settings[0].source.buffer.fence = Fence::NO_FENCE;
    }
    PicoSingleLayerAction invoke(bool enabled = true) {
        return preparePicoSingleLayerComposition(surface, enabled, fallback, settings, owner, &ready);
    }
};
TEST_F(PicoSingleLayerCompositionTest, GateDisabledPreservesBufferTag) {
    EXPECT_EQ(PicoSingleLayerAction::Render, invoke(false));
    EXPECT_EQ(0x100000020ULL, buffer->usage);
}
TEST_F(PicoSingleLayerCompositionTest, StickyFallbackPreservesBufferTag) {
    fallback = true;
    EXPECT_EQ(PicoSingleLayerAction::Render, invoke());
    EXPECT_EQ(0x100000020ULL, buffer->usage);
}
TEST_F(PicoSingleLayerCompositionTest, EmptyAndMultipleLayersRenderNormally) {
    settings.clear();
    EXPECT_EQ(PicoSingleLayerAction::Render, invoke());
    settings.resize(2);
    settings[0].source.buffer.buffer = buffer;
    EXPECT_EQ(PicoSingleLayerAction::Render, invoke());
    EXPECT_EQ(0x100000020ULL, buffer->usage);
}
TEST_F(PicoSingleLayerCompositionTest, SolidColorUsesNormalRenderer) {
    settings[0].source.buffer.buffer.clear();
    EXPECT_EQ(PicoSingleLayerAction::Render, invoke());
    EXPECT_FALSE(fallback);
}
TEST_F(PicoSingleLayerCompositionTest, MarkedBuffersTripFallbackAndClearTag) {
    for (uint64_t tag : {0x400000000ULL, 0x800000000ULL}) {
        fallback = false;
        buffer->usage = tag | 0x20;
        EXPECT_EQ(PicoSingleLayerAction::Render, invoke());
        EXPECT_TRUE(fallback);
        EXPECT_EQ(0x20ULL, buffer->usage);
    }
}
TEST_F(PicoSingleLayerCompositionTest, MultiLayerFlagPreventsAttach) {
    EXPECT_CALL(surface, getMultiLayerFlag()).WillOnce(Return(true));
    EXPECT_EQ(PicoSingleLayerAction::Render, invoke());
    EXPECT_EQ(0x20ULL, buffer->usage);
    EXPECT_FALSE(fallback);
}
TEST_F(PicoSingleLayerCompositionTest, SuccessfulAttachTracksBufferAndAcquireFence) {
    int descriptors[2];
    ASSERT_EQ(0, pipe(descriptors));
    base::unique_fd writer(descriptors[1]);
    settings[0].source.buffer.fence = new Fence(descriptors[0]);
    EXPECT_CALL(surface, getMultiLayerFlag()).WillOnce(Return(false));
    EXPECT_CALL(surface, attachBuffer(_)).WillOnce(Return(NO_ERROR));
    EXPECT_CALL(surface, setMultiLayerFlag(false));
    EXPECT_CALL(surface, setSingleLayer(owner, buffer)).WillOnce(Return(NO_ERROR));
    EXPECT_EQ(PicoSingleLayerAction::Attached, invoke());
    ASSERT_GE(ready.get(), 0);
    EXPECT_NE(descriptors[0], ready.get());
    EXPECT_GE(fcntl(descriptors[0], F_GETFD), 0);
    EXPECT_EQ(0x20ULL, buffer->usage);
    EXPECT_FALSE(fallback);
}
TEST_F(PicoSingleLayerCompositionTest, RepeatedBufferSkipsFrame) {
    EXPECT_CALL(surface, getMultiLayerFlag()).WillOnce(Return(false));
    EXPECT_CALL(surface, attachBuffer(_)).WillOnce(Return(ALREADY_EXISTS));
    EXPECT_EQ(PicoSingleLayerAction::SkipFrame, invoke());
    EXPECT_FALSE(fallback);
    EXPECT_EQ(-1, ready.get());
}
TEST_F(PicoSingleLayerCompositionTest, NoInitRendersWithoutLatchingFallback) {
    EXPECT_CALL(surface, getMultiLayerFlag()).WillOnce(Return(false));
    EXPECT_CALL(surface, attachBuffer(_)).WillOnce(Return(NO_INIT));
    EXPECT_EQ(PicoSingleLayerAction::RenderAndTrack, invoke());
    EXPECT_FALSE(fallback);
}
TEST_F(PicoSingleLayerCompositionTest, OtherAttachFailureLatchesFallback) {
    EXPECT_CALL(surface, getMultiLayerFlag()).WillOnce(Return(false));
    EXPECT_CALL(surface, attachBuffer(_)).WillOnce(Return(BAD_VALUE));
    EXPECT_EQ(PicoSingleLayerAction::RenderAndTrack, invoke());
    EXPECT_TRUE(fallback);
}
TEST_F(PicoSingleLayerCompositionTest, RenderedFallbackTracksOutputBuffer) {
    sp<GraphicBuffer> output = new GraphicBuffer;
    EXPECT_CALL(surface, setMultiLayerFlag(false));
    EXPECT_CALL(surface, setSingleLayer(owner, output)).WillOnce(Return(NO_ERROR));
    finishPicoSingleLayerRender(surface, owner, output, Fence::NO_FENCE, &ready);
    EXPECT_EQ(-1, ready.get());
}
}
