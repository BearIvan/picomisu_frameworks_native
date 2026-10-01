// Copyright 2026 Picomisu contributors
// SPDX-License-Identifier: Apache-2.0
#include <binder/Binder.h>
#include <binder/Parcel.h>
#include <gui/IProducerListener.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <limits>
#include <thread>

namespace android {
namespace {

constexpr uint32_t kReleaseFence = IBinder::FIRST_CALL_TRANSACTION + 3;

class FenceRecorder : public BnProducerListener {
public:
    void onBufferReleased() override { ++released; }
    void onBuffersDiscarded(const std::vector<int32_t>&) override { ++discarded; }
    void onBufferReleasedWithFence(const sp<Fence>& fence, uint64_t bufferId, bool flag) override {
        ++fences;
        lastFence = fence;
        lastId = bufferId;
        lastFlag = flag;
    }
    int released = 0, discarded = 0, fences = 0;
    uint64_t lastId = 0;
    bool lastFlag = false;
    sp<Fence> lastFence;
};

class FenceRelay : public BBinder {
public:
    explicit FenceRelay(const sp<IBinder>& target) : mTarget(target) {}
    int calls = 0;
    uint32_t lastCode = 0, lastFlags = 0;
protected:
    status_t onTransact(uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags) override {
        ++calls;
        lastCode = code;
        lastFlags = flags;
        return mTarget->transact(code, data, reply, flags);
    }
private:
    sp<IBinder> mTarget;
};

class Pipe {
public:
    Pipe() { if (pipe(fds) != 0) fds[0] = fds[1] = -1; }
    ~Pipe() { for (int fd : fds) if (fd >= 0) close(fd); }
    int releaseRead() { int fd = fds[0]; fds[0] = -1; return fd; }
    int fds[2]{-1, -1};
};

TEST(ProducerFenceTest, BinderPreservesUint64IdFlagAndOneWayCode) {
    sp<FenceRecorder> recorder = new FenceRecorder;
    sp<FenceRelay> relay = new FenceRelay(IInterface::asBinder(recorder));
    sp<IProducerListener> proxy = interface_cast<IProducerListener>(relay);
    const uint64_t ids[]{0, 0x100000001ULL, 0x80000000deadbeefULL,
                         std::numeric_limits<uint64_t>::max()};
    for (uint64_t id : ids) {
        for (bool flag : {false, true}) {
            proxy->onBufferReleasedWithFence(Fence::NO_FENCE, id, flag);
            EXPECT_EQ(id, recorder->lastId);
            EXPECT_EQ(flag, recorder->lastFlag);
            ASSERT_NE(nullptr, recorder->lastFence.get());
            EXPECT_FALSE(recorder->lastFence->isValid());
            EXPECT_EQ(kReleaseFence, relay->lastCode);
            EXPECT_EQ(IBinder::FLAG_ONEWAY, relay->lastFlags);
        }
    }
    EXPECT_EQ(8, recorder->fences);
}

TEST(ProducerFenceTest, BinderDuplicatesAndTransfersFileDescriptor) {
    // A pipe exercises FD ownership and payload transport, not GPU synchronization.
    Pipe pipe;
    ASSERT_GE(pipe.fds[0], 0);
    sp<Fence> sent = new Fence(pipe.releaseRead());
    sp<FenceRecorder> recorder = new FenceRecorder;
    sp<FenceRelay> relay = new FenceRelay(IInterface::asBinder(recorder));
    sp<IProducerListener> proxy = interface_cast<IProducerListener>(relay);
    proxy->onBufferReleasedWithFence(sent, 0xffff000012345678ULL, true);
    ASSERT_EQ(1, recorder->fences);
    ASSERT_NE(nullptr, recorder->lastFence.get());
    ASSERT_TRUE(recorder->lastFence->isValid());
    EXPECT_NE(sent->get(), recorder->lastFence->get());
    char sentByte = 'P', receivedByte = 0;
    ASSERT_EQ(1, write(pipe.fds[1], &sentByte, 1));
    ASSERT_EQ(1, read(recorder->lastFence->get(), &receivedByte, 1));
    EXPECT_EQ(sentByte, receivedByte);
    sent.clear();
    EXPECT_TRUE(recorder->lastFence->isValid());
}

TEST(ProducerFenceTest, ServerRejectsWrongInterface) {
    sp<FenceRecorder> recorder = new FenceRecorder;
    Parcel data, reply;
    ASSERT_EQ(NO_ERROR, data.writeInterfaceToken(String16("wrong.interface")));
    ASSERT_EQ(NO_ERROR, data.write(*Fence::NO_FENCE));
    ASSERT_EQ(NO_ERROR, data.writeUint64(1));
    ASSERT_EQ(NO_ERROR, data.writeBool(false));
    EXPECT_EQ(PERMISSION_DENIED, IInterface::asBinder(recorder)->transact(kReleaseFence, data, &reply));
    EXPECT_EQ(0, recorder->fences);
}

TEST(ProducerFenceTest, ServerDeliversTruncatedPayloadAndReturnsIdReadStatus) {
    // Factory: the fence and flag read statuses are ignored, the callback always runs and
    // the transaction returns the status of the uint64 id read (a missing flag reads false).
    sp<FenceRecorder> recorder = new FenceRecorder;
    for (int stage = 0; stage < 4; ++stage) {
        Parcel data, reply;
        ASSERT_EQ(NO_ERROR, data.writeInterfaceToken(recorder->getInterfaceDescriptor()));
        if (stage >= 1) { ASSERT_EQ(NO_ERROR, data.write(*Fence::NO_FENCE)); }
        if (stage == 2) { ASSERT_EQ(NO_ERROR, data.writeUint32(123)); }
        if (stage == 3) { ASSERT_EQ(NO_ERROR, data.writeUint64(0xfeedbeef12345678ULL)); }
        const status_t result =
                IInterface::asBinder(recorder)->transact(kReleaseFence, data, &reply);
        if (stage == 3) {
            EXPECT_EQ(NO_ERROR, result);
            EXPECT_EQ(0xfeedbeef12345678ULL, recorder->lastId);
        } else {
            EXPECT_NE(NO_ERROR, result);
            EXPECT_EQ(0u, recorder->lastId);
        }
        EXPECT_FALSE(recorder->lastFlag);
        EXPECT_EQ(stage + 1, recorder->fences);
    }
}

TEST(ProducerFenceTest, ServerIgnoresMalformedFenceDescriptorCount) {
    sp<FenceRecorder> recorder = new FenceRecorder;
    Parcel data, reply;
    ASSERT_EQ(NO_ERROR, data.writeInterfaceToken(recorder->getInterfaceDescriptor()));
    // Parcel flattened length + FD count, followed by Fence's own FD count.
    ASSERT_EQ(NO_ERROR, data.writeInt32(4));
    ASSERT_EQ(NO_ERROR, data.writeInt32(0));
    ASSERT_EQ(NO_ERROR, data.writeUint32(1));
    ASSERT_EQ(NO_ERROR, data.writeUint64(1));
    ASSERT_EQ(NO_ERROR, data.writeBool(false));
    // Factory: the failed fence unflatten is ignored; the callback gets an invalid fence.
    EXPECT_EQ(NO_ERROR, IInterface::asBinder(recorder)->transact(kReleaseFence, data, &reply));
    EXPECT_EQ(1, recorder->fences);
    EXPECT_EQ(1u, recorder->lastId);
    ASSERT_NE(nullptr, recorder->lastFence.get());
    EXPECT_FALSE(recorder->lastFence->isValid());
}

TEST(ProducerFenceTest, DefaultFenceCallbackDoesNotReplaceOrdinaryRelease) {
    sp<DummyProducerListener> listener = new DummyProducerListener;
    sp<FenceRelay> relay = new FenceRelay(IInterface::asBinder(listener));
    sp<IProducerListener> proxy = interface_cast<IProducerListener>(relay);
    proxy->onBufferReleasedWithFence(Fence::NO_FENCE, 7, true);
    EXPECT_EQ(kReleaseFence, relay->lastCode);
    EXPECT_FALSE(proxy->needsReleaseNotify());
}

TEST(ProducerFenceTest, VirtualDisplayCallbackPreservesArgumentsAndIgnoresIntResult) {
    sp<VirtualDisplayProducerListener> listener = new VirtualDisplayProducerListener;
    int calls = 0;
    sp<Fence> received;
    uint64_t receivedId = 0;
    bool receivedFlag = false;
    listener->setCallback([&](const sp<Fence>& fence, uint64_t id, bool flag) {
        ++calls;
        received = fence;
        receivedId = id;
        receivedFlag = flag;
        return -567;
    });
    listener->onBufferReleasedWithFence(Fence::NO_FENCE, 0x8000000100000002ULL, true);
    EXPECT_EQ(1, calls);
    EXPECT_EQ(Fence::NO_FENCE.get(), received.get());
    EXPECT_EQ(0x8000000100000002ULL, receivedId);
    EXPECT_TRUE(receivedFlag);
    EXPECT_FALSE(listener->needsReleaseNotify());
    listener->onBufferReleased();
    EXPECT_EQ(1, calls);
}

TEST(ProducerFenceTest, VirtualDisplayCallbackCanBeReplacedAndCleared) {
    sp<VirtualDisplayProducerListener> listener = new VirtualDisplayProducerListener;
    int first = 0, second = 0;
    listener->onBufferReleasedWithFence(Fence::NO_FENCE, 1, false);
    listener->setCallback([&](const sp<Fence>&, uint64_t, bool) { ++first; return 0; });
    listener->onBufferReleasedWithFence(Fence::NO_FENCE, 2, false);
    listener->setCallback([&](const sp<Fence>&, uint64_t, bool) { ++second; return 0; });
    listener->onBufferReleasedWithFence(Fence::NO_FENCE, 3, false);
    listener->setCallback({});
    listener->onBufferReleasedWithFence(Fence::NO_FENCE, 4, false);
    EXPECT_EQ(1, first);
    EXPECT_EQ(1, second);
}

TEST(ProducerFenceTest, VirtualDisplaySynchronizesCallbackReplacementAndDelivery) {
    sp<VirtualDisplayProducerListener> listener = new VirtualDisplayProducerListener;
    std::atomic<int> calls{0};
    auto callback = [&](const sp<Fence>&, uint64_t, bool) { ++calls; return 0; };
    listener->setCallback(callback);
    std::thread replace([&] { for (int i = 0; i < 500; ++i) listener->setCallback(callback); });
    std::thread deliver([&] {
        for (int i = 0; i < 500; ++i) listener->onBufferReleasedWithFence(Fence::NO_FENCE, i, false);
    });
    replace.join();
    deliver.join();
    EXPECT_EQ(500, calls.load());
}

} // namespace
} // namespace android
