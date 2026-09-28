// Copyright 2026 Picomisu contributors
// SPDX-License-Identifier: Apache-2.0

#include <binder/Binder.h>
#include <binder/Parcel.h>
#include <gui/BufferQueueCore.h>
#include <gui/BufferQueueProducer.h>
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

} // namespace

class PicoBufferQueueStatusTest : public testing::Test {
protected:
    void SetUp() override {
        core = new BufferQueueCore;
        producer = new BufferQueueProducer(core);
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
    sp<BufferQueueCore> core;
    sp<BufferQueueProducer> producer;
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

} // namespace android
