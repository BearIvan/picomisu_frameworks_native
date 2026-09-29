// Copyright 2026 The Picomisu Project
// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <binder/Parcel.h>
#include <gui/IGraphicBufferProducer.h>
#include <compositionengine/DisplaySurface.h>
#include <cutils/properties.h>
#include <dlfcn.h>
#include <cstring>
#include <cstdlib>
#include "ColorLayer.h"
#include "MonitoredProducer.h"
#include "TestableSurfaceFlinger.h"
#include "TestableScheduler.h"
#include "mock/MockMessageQueue.h"
#include "mock/gui/MockGraphicBufferProducer.h"

// Keep every Source monitor created by this unit executable disconnected from
// real VR services. This interposition changes no system property.
extern "C" __attribute__((visibility("default"))) int32_t property_get_int32(
        const char* key, int32_t fallback) {
    if (!std::strcmp(key,"sys.boot_completed")) return 0;
    using Getter=int32_t(*)(const char*,int32_t);
    static auto real=reinterpret_cast<Getter>(dlsym(RTLD_NEXT,"property_get_int32"));
    if (!real) std::abort();
    return real(key,fallback);
}

namespace android {
using testing::_;
using testing::NiceMock;
using testing::Return;

class PicoMonitoredProducerTest : public testing::Test {
protected:
    void SetUp() override {
        auto queue=std::make_unique<NiceMock<mock::MessageQueue>>();
        ON_CALL(*queue,postMessage(_, _)).WillByDefault(Return(NO_ERROR));
        flinger.mutableEventQueue()=std::move(queue);
        flinger.mutableScheduler()=std::make_unique<TestableScheduler>(flinger.mutableRefreshRateConfigs());
        layer=new ColorLayer(LayerCreationArgs(flinger.mFlinger.get(),sp<Client>(),
                    String8("pico-test-layer"),16,16,0,LayerMetadata()));
        producer=new NiceMock<mock::GraphicBufferProducer>;
        wrapped=new MonitoredProducer(producer,flinger.mFlinger,layer);
    }
    void TearDown() override { wrapped.clear();producer.clear();layer.clear(); }
    TestableSurfaceFlinger flinger;
    sp<ColorLayer> layer;
    sp<NiceMock<mock::GraphicBufferProducer>> producer;
    sp<MonitoredProducer> wrapped;
};

TEST_F(PicoMonitoredProducerTest, SuccessfulDequeueForwardsAndRecordsLayerDuration) {
    int slot=-1;sp<Fence> fence;uint64_t age=0;
    EXPECT_CALL(*producer,dequeueBuffer(&slot,&fence,16,16,PIXEL_FORMAT_RGBA_8888,0,&age,nullptr))
        .WillOnce(Return(NO_ERROR));
    EXPECT_EQ(NO_ERROR,wrapped->dequeueBuffer(&slot,&fence,16,16,PIXEL_FORMAT_RGBA_8888,0,&age,nullptr));
    EXPECT_GE(layer->getPicoDequeueDuration(),0);
    EXPECT_EQ(nullptr,wrapped->getSurfaceClient()->findCurrentFrame(0));
}
TEST_F(PicoMonitoredProducerTest, FailedDequeuePreservesPreviousLayerDuration) {
    layer->setPicoDequeueDuration(12345);
    int slot=-1;sp<Fence> fence;
    EXPECT_CALL(*producer,dequeueBuffer(_,_,_,_,_,_,_,_)).WillOnce(Return(BAD_VALUE));
    EXPECT_EQ(BAD_VALUE,wrapped->dequeueBuffer(&slot,&fence,16,16,PIXEL_FORMAT_RGBA_8888,0,nullptr,nullptr));
    EXPECT_EQ(12345,layer->getPicoDequeueDuration());
}
TEST_F(PicoMonitoredProducerTest, QueueRecordsSlotAndLayerNameOnSuccessAndError) {
    int slot=-1;sp<Fence> fence;
    EXPECT_CALL(*producer,dequeueBuffer(_,_,_,_,_,_,_,_)).WillOnce(Return(NO_ERROR));
    wrapped->dequeueBuffer(&slot,&fence,16,16,PIXEL_FORMAT_RGBA_8888,0,nullptr,nullptr);
    IGraphicBufferProducer::QueueBufferInput input(0,false,HAL_DATASPACE_UNKNOWN,Rect(),
            NATIVE_WINDOW_SCALING_MODE_FREEZE,0,Fence::NO_FENCE);
    IGraphicBufferProducer::QueueBufferOutput output;
    EXPECT_CALL(*producer,queueBuffer(3,_,&output)).WillOnce(Return(NO_ERROR));
    EXPECT_EQ(NO_ERROR,wrapped->queueBuffer(3,input,&output));
    auto* frame=wrapped->getSurfaceClient()->findCurrentFrame(3);
    ASSERT_NE(nullptr,frame);
    EXPECT_STREQ("pico-test-layer",frame->name);
    EXPECT_EQ(3,frame->frameNumber);
    EXPECT_LE(frame->timestamps[0],frame->timestamps[1]);
    EXPECT_LE(frame->timestamps[1],frame->timestamps[2]);
    EXPECT_LE(frame->timestamps[2],frame->timestamps[3]);
    EXPECT_CALL(*producer,queueBuffer(4,_,&output)).WillOnce(Return(BAD_VALUE));
    EXPECT_EQ(BAD_VALUE,wrapped->queueBuffer(4,input,&output));
    frame=wrapped->getSurfaceClient()->findCurrentFrame(4);
    ASSERT_NE(nullptr,frame);
    EXPECT_EQ(4,frame->frameNumber);
}
TEST_F(PicoMonitoredProducerTest, DisplayModeTransactionRequiresProducerInterface) {
    Parcel data,reply;
    data.writeInterfaceToken(IGraphicBufferProducer::descriptor);
    data.writeInt32(0);
    EXPECT_EQ(NO_ERROR,wrapped->transact(1110,data,&reply,0));
    Parcel invalid;
    invalid.writeInterfaceToken(String16("invalid"));
    invalid.writeInt32(0);
    EXPECT_EQ(PERMISSION_DENIED,wrapped->transact(1110,invalid,&reply,0));
}
TEST_F(PicoMonitoredProducerTest, UnknownTransactionKeepsBaseResult) {
    Parcel data,reply;
    data.writeInterfaceToken(IGraphicBufferProducer::descriptor);
    EXPECT_EQ(UNKNOWN_TRANSACTION,wrapped->transact(1111,data,&reply,0));
}
} // namespace android
