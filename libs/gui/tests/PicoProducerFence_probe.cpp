// Copyright 2026 Picomisu contributors
// SPDX-License-Identifier: Apache-2.0
// Run the same binary with source and factory libgui. No BufferQueue allocation,
// graphic buffers, GPU work, system writes or service replacement.
#include <binder/Binder.h>
#include <binder/Parcel.h>
#include <gui/IProducerListener.h>
#include <unistd.h>

#include <array>
#include <cinttypes>
#include <cstdio>
#include <string>

using namespace android;

namespace {
constexpr uint32_t kReleaseFence = IBinder::FIRST_CALL_TRANSACTION + 3;

class Recorder : public BnProducerListener {
public:
    void onBufferReleased() override { ++released; }
    void onBuffersDiscarded(const std::vector<int32_t>&) override { ++discarded; }
    void onBufferReleasedWithFence(const sp<Fence>& fence, uint64_t id, bool flag) override {
        ++calls;
        lastFence = fence;
        lastId = id;
        lastFlag = flag;
    }
    int calls = 0, released = 0, discarded = 0;
    uint64_t lastId = 0;
    bool lastFlag = false;
    sp<Fence> lastFence;
};

class Relay : public BBinder {
public:
    explicit Relay(const sp<IBinder>& target) : mTarget(target) {}
    uint32_t code = 0, flags = 0;
    std::string bytes;
protected:
    status_t onTransact(uint32_t transaction, const Parcel& data, Parcel* reply,
                        uint32_t transactionFlags) override {
        code = transaction;
        flags = transactionFlags;
        bytes.clear();
        if (!data.hasFileDescriptors()) {
            static const char digits[] = "0123456789abcdef";
            const uint8_t* buffer = data.data();
            for (size_t i = 0; i < data.dataSize(); ++i) {
                bytes += digits[buffer[i] >> 4];
                bytes += digits[buffer[i] & 15];
            }
        }
        return mTarget->transact(transaction, data, reply, transactionFlags);
    }
private:
    sp<IBinder> mTarget;
};

bool check(bool value, const char* label) {
    if (!value) fprintf(stderr, "Failed producer fence fixture: %s\n", label);
    return value;
}
}

int main() {
    sp<Recorder> recorder = new Recorder;
    sp<Relay> relay = new Relay(IInterface::asBinder(recorder));
    sp<IProducerListener> proxy = interface_cast<IProducerListener>(relay);
    const uint64_t ids[]{0, 0x100000001ULL, 0x80000000deadbeefULL, UINT64_MAX};
    for (uint64_t id : ids) {
        for (bool flag : {false, true}) {
            int count = recorder->calls;
            proxy->onBufferReleasedWithFence(Fence::NO_FENCE, id, flag);
            if (!check(recorder->calls == count + 1 && recorder->lastId == id &&
                    recorder->lastFlag == flag && recorder->lastFence != nullptr &&
                    !recorder->lastFence->isValid() && relay->code == kReleaseFence &&
                    relay->flags == IBinder::FLAG_ONEWAY, "no-FD proxy")) return 1;
            printf("fence-wire id=%016" PRIx64 " flag=%d code=%u flags=%u bytes=%s\n",
                   id, flag, relay->code, relay->flags, relay->bytes.c_str());
        }
    }

    Parcel data, reply;
    if (!check(data.writeInterfaceToken(recorder->getInterfaceDescriptor()) == NO_ERROR &&
               data.write(*Fence::NO_FENCE) == NO_ERROR &&
               data.writeUint64(0xfedcba9876543210ULL) == NO_ERROR &&
               data.writeBool(true) == NO_ERROR, "manual packet construction")) return 1;
    if (!check(IInterface::asBinder(recorder)->transact(kReleaseFence, data, &reply,
                IBinder::FLAG_ONEWAY) == NO_ERROR && recorder->lastId == 0xfedcba9876543210ULL &&
                recorder->lastFlag, "manual server packet")) return 1;
    puts("fence-wire manual-server=1");

    int fds[2];
    if (!check(pipe(fds) == 0, "pipe")) return 1;
    sp<Fence> sent = new Fence(fds[0]);
    proxy->onBufferReleasedWithFence(sent, 0xffff000012345678ULL, true);
    bool received = recorder->lastFence != nullptr && recorder->lastFence->isValid() &&
                    recorder->lastFence->get() != sent->get() &&
                    recorder->lastId == 0xffff000012345678ULL && recorder->lastFlag;
    char before = 'Z', after = 0;
    received = received && write(fds[1], &before, 1) == 1 &&
               read(recorder->lastFence->get(), &after, 1) == 1 && before == after;
    close(fds[1]);
    if (!check(received, "FD ownership/transport (pipe, not GPU fence)")) return 1;
    puts("fence-wire descriptor-transport=1");

    sp<VirtualDisplayProducerListener> display = new VirtualDisplayProducerListener;
    int calls = 0;
    // A large capture also exercises heap-backed std::function copying in factory code.
    std::array<uint64_t, 16> capture{};
    capture.back() = 0x123456789abcdef0ULL;
    display->setCallback([capture, &calls](const sp<Fence>& fence, uint64_t id, bool flag) {
        if (fence == Fence::NO_FENCE && id == capture.back() && flag) ++calls;
        return -456;
    });
    display->onBufferReleasedWithFence(Fence::NO_FENCE, capture.back(), true);
    if (!check(calls == 1 && !display->needsReleaseNotify(), "virtual-display callback")) return 1;
    display->setCallback([&](const sp<Fence>&, uint64_t, bool) { ++calls; return 0; });
    display->onBufferReleasedWithFence(Fence::NO_FENCE, 2, false);
    display->setCallback({});
    display->onBufferReleasedWithFence(Fence::NO_FENCE, 3, false);
    display->onBufferReleased();
    if (!check(calls == 2, "virtual-display replacement/clear")) return 1;
    printf("fence-wire display-callback=1 bn-bytes=%zu display-bytes=%zu\n",
           sizeof(BnProducerListener), sizeof(VirtualDisplayProducerListener));
    puts("fence-probe passed=12");
    return 0;
}
