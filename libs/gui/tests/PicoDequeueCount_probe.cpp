// Copyright 2026 Picomisu contributors
// SPDX-License-Identifier: Apache-2.0
// Compare public count/status behavior with PICO; no buffers, GPU or real services.
#include <binder/FreezeManager.h>
#include <gui/BufferQueueConsumer.h>
#include <gui/BufferQueueCore.h>
#include <gui/BufferQueueProducer.h>
#include <gui/IConsumerListener.h>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <utility>
#include "PicoFreezeRegistryFake.h"
using namespace android;
namespace { sp<picomisu::ServiceManager> fakeManager; }
namespace android { sp<IServiceManager> defaultServiceManager() { return fakeManager; } }
namespace {
void require(bool ok, const char* label) {
    if (!ok) { std::fprintf(stderr, "dequeue-count fixture failed: %s\n", label); std::exit(1); }
}
template<class T, size_t Reserve, class... Args>
sp<T> padded(Args&&... args) {
    static_assert(sizeof(T) <= Reserve, "Insufficient object storage");
    return ::new (::operator new(Reserve)) T(std::forward<Args>(args)...);
}
class Idle : public BnConsumerListener {
public:
    void onFrameAvailable(const BufferItem&) override {}
    void onBuffersReleased() override {}
    void onSidebandStreamChanged() override {}
};
struct Queue {
    sp<BufferQueueCore> core = padded<BufferQueueCore, 8192>();
    sp<BufferQueueProducer> producer;
    sp<BufferQueueConsumer> consumer = padded<BufferQueueConsumer, 1024>(core);
    explicit Queue(bool sf) : producer(padded<BufferQueueProducer, 1024>(core, sf)) {}
    void markPico() {
        Parcel data, reply;
        require(data.writeInterfaceToken(IGraphicBufferConsumer::descriptor) == NO_ERROR &&
                data.writeInt32(7) == NO_ERROR && data.writeInt32(0) == NO_ERROR, "consumer packet");
        require(IInterface::asBinder(consumer)->transact(10000, data, &reply) == NO_ERROR,
                "consumer marker");
    }
    int count() {
        String8 dump;
        require(consumer->BufferQueueConsumer::dumpState(String8(), &dump) == NO_ERROR, "dump");
        const char* marker = std::strstr(dump.string(), "mMaxDequeuedBufferCount=");
        require(marker != nullptr, "count in public dump");
        return static_cast<int>(std::strtol(marker + std::strlen("mMaxDequeuedBufferCount="), nullptr, 10));
    }
};
}
int main() {
    fakeManager = new picomisu::ServiceManager;
    sp<picomisu::FreezeService> service = new picomisu::FreezeService;
    fakeManager->service = IInterface::asBinder(service);
    auto selected = FreezeManager::getInstance()->getService();
    require(selected && IInterface::asBinder(selected) == fakeManager->service &&
            !fakeManager->wrongName, "fake service");
    int fixtures = 0;
    const int requests[]{-3, -2, -1, 0, 1, 2, 3, 61, 62, 63, 64, INT_MIN, INT_MAX};
    for (bool sf : {false, true}) for (bool pico : {false, true}) {
        for (bool arm : {false, true}) for (int request : requests) {
            Queue q(sf);
            if (pico) q.markPico();
            int armed = arm ? q.producer->BufferQueueProducer::setMaxDequeuedBufferCount(-2) : INT_MIN;
            int before = q.count();
            status_t status = q.producer->BufferQueueProducer::setMaxDequeuedBufferCount(request);
            std::printf("dequeue-count sf=%d pico=%d arm=%d request=%d arm_status=%d before=%d status=%d after=%d\n",
                        sf, pico, arm, request, armed, before, status, q.count());
            ++fixtures;
        }
        Queue q(sf);
        if (pico) q.markPico();
        require(q.consumer->BufferQueueConsumer::connect(new Idle, false) == NO_ERROR, "connect before abandon");
        require(q.consumer->BufferQueueConsumer::disconnect() == NO_ERROR, "abandon");
        int status = q.producer->BufferQueueProducer::setMaxDequeuedBufferCount(-2);
        int ordinary = q.producer->BufferQueueProducer::setMaxDequeuedBufferCount(1);
        std::printf("dequeue-count abandoned sf=%d pico=%d command=%d ordinary=%d\n", sf, pico, status, ordinary);
        ++fixtures;
    }
    require(fixtures == 108, "fixture total");
    std::printf("dequeue-count-probe passed=%d\n", fixtures);
}
