// Copyright 2026 Picomisu contributors
// SPDX-License-Identifier: Apache-2.0
// Local fake freeze service; synthetic buffers, no GPU or process freezing.
#include <binder/FreezeManager.h>
#include <gui/BufferItem.h>
#include <gui/BufferQueueConsumer.h>
#include <gui/BufferQueueCore.h>
#include <gui/BufferQueueProducer.h>
#include <gui/IConsumerListener.h>
#include <gui/IProducerListener.h>
#include <system/window.h>
#include <cstdio>
#include <new>
#include <utility>
#include <unistd.h>
#include "PicoFreezeRegistryFake.h"
using namespace android;
namespace { sp<picomisu::ServiceManager> fakeManager; }
namespace android { sp<IServiceManager> defaultServiceManager() { return fakeManager; } }
namespace {
template<class T, size_t Reserve, class... Args>
sp<T> padded(Args&&... args) {
    static_assert(sizeof(T) <= Reserve, "Insufficient object storage");
    return ::new (::operator new(Reserve)) T(std::forward<Args>(args)...);
}
struct Item {
    BufferItem* p = ::new (::operator new(1024)) BufferItem;
    ~Item() { p->~BufferItem(); ::operator delete(p); }
};
class Idle : public BnConsumerListener {
public:
    void onFrameAvailable(const BufferItem&) override {}
    void onBuffersReleased() override {}
    void onSidebandStreamChanged() override {}
};
sp<GraphicBuffer> buffer() {
    auto b = padded<GraphicBuffer, 512>();
    b->width = b->height = b->stride = b->layerCount = 1;
    b->format = HAL_PIXEL_FORMAT_RGBA_8888;
    return b;
}
struct Queue {
    sp<BufferQueueCore> core = padded<BufferQueueCore, 8192>();
    sp<BufferQueueProducer> producer = padded<BufferQueueProducer, 1024>(core);
    sp<BufferQueueConsumer> consumer = padded<BufferQueueConsumer, 1024>(core);
    int acquired = -1, held = -1;
    sp<GraphicBuffer> heldBuffer;
    bool attach(int* slot) {
        auto b = buffer();
        return producer->BufferQueueProducer::attachCachedBuffer(slot, b, b->getId()) == NO_ERROR;
    }
    bool queue(int slot) {
        sp<GraphicBuffer> b;
        if (producer->BufferQueueProducer::requestBuffer(slot, &b) != NO_ERROR || !b) return false;
        IGraphicBufferProducer::QueueBufferInput input(0, false, HAL_DATASPACE_UNKNOWN,
                Rect(1, 1), NATIVE_WINDOW_SCALING_MODE_FREEZE, 0, Fence::NO_FENCE);
        IGraphicBufferProducer::QueueBufferOutput output;
        return producer->BufferQueueProducer::queueBuffer(slot, input, &output) == NO_ERROR;
    }
    bool prepare() {
        IGraphicBufferProducer::QueueBufferOutput output;
        if (consumer->BufferQueueConsumer::connect(new Idle, false) != NO_ERROR ||
            producer->BufferQueueProducer::connect(nullptr, NATIVE_WINDOW_API_CPU, false, &output) != NO_ERROR ||
            producer->BufferQueueProducer::setMaxDequeuedBufferCount(1) != NO_ERROR ||
            producer->BufferQueueProducer::allowAllocation(false) != NO_ERROR ||
            !attach(&acquired) || !queue(acquired)) return false;
        Item item;
        return consumer->BufferQueueConsumer::acquireBuffer(item.p, 0) == NO_ERROR && attach(&held) &&
               producer->BufferQueueProducer::requestBuffer(held, &heldBuffer) == NO_ERROR;
    }
    status_t extra(int* slot) {
        auto b = buffer();
        return producer->BufferQueueProducer::attachCachedBuffer(slot, b, b->getId());
    }
    void listen() { sp<IGraphicBufferProducer> p = producer; p->listenFreezeSelf(); }
    bool release(int slot, uint64_t frame) {
        return consumer->BufferQueueConsumer::releaseBuffer(slot, frame, EGL_NO_DISPLAY,
                EGL_NO_SYNC_KHR, Fence::NO_FENCE) == NO_ERROR;
    }
};
bool check(bool ok, const char* label) {
    if (ok) printf("producer-freeze %s=1\n", label);
    else fprintf(stderr, "producer-freeze fixture failed: %s\n", label);
    return ok;
}
}
int main() {
    fakeManager = new picomisu::ServiceManager;
    sp<picomisu::FreezeService> service = new picomisu::FreezeService;
    fakeManager->service = IInterface::asBinder(service);
    FreezeManager* manager = FreezeManager::getInstance();
    auto selected = manager->getService();
    if (!check(selected && IInterface::asBinder(selected) == fakeManager->service &&
               fakeManager->lookups == 1 && !fakeManager->wrongName, "fake-service-preflight")) return 1;
    {
        Queue q;
        if (!q.prepare()) return 1;
        const size_t before = service->registrations.size();
        q.listen();
        if (!check(service->registrations.size() == before + 1 &&
                   service->registrations.back().pid == getpid() &&
                   !service->registrations.back().flag, "virtual-self-registration")) return 1;
        q.listen();
        if (!check(service->registrations.size() == before + 1,
                   "repeat-listen-keeps-process-registration")) return 1;
        auto event = service->latest();
        event->onUnFreeze(getpid() + 100000);
        int extra = -1;
        if (!check(q.extra(&extra) == INVALID_OPERATION && extra == -1,
                   "wrong-pid-retains-dequeue-limit")) return 1;
        event->onUnFreeze(getpid());
        if (q.extra(&extra) != NO_ERROR) return 1;
        sp<GraphicBuffer> oldSlot, newSlot;
        const status_t oldStatus = q.producer->BufferQueueProducer::requestBuffer(q.held, &oldSlot);
        const status_t newStatus = q.producer->BufferQueueProducer::requestBuffer(extra, &newSlot);
        if (!check(extra != q.acquired && newStatus == NO_ERROR && newSlot != q.heldBuffer &&
                   (extra == q.held ? oldStatus == NO_ERROR && oldSlot == newSlot : oldStatus == BAD_VALUE) &&
                   q.release(q.acquired, 1), "thaw-reclaims-held-slot-preserves-consumer")) return 1;
        int another = -1;
        if (!check(q.extra(&another) == INVALID_OPERATION && another == -1,
                   "single-event-consumed")) return 1;
    }
    {
        Queue q;
        if (!q.prepare()) return 1;
        q.listen();
        service->latest()->onUnFreeze(getpid());
        if (!q.release(q.acquired, 1) || !q.queue(q.held)) return 1;
        Item item;
        if (q.consumer->BufferQueueConsumer::acquireBuffer(item.p, 0) != NO_ERROR ||
            !q.release(q.held, 2)) return 1;
        int held = -1, another = -1;
        if (!q.attach(&held)) return 1;
        if (!check(q.extra(&another) == INVALID_OPERATION && another == -1,
                   "successful-queue-clears-event")) return 1;
    }
    {
        Queue q;
        if (!q.prepare() || q.producer->BufferQueueProducer::setMaxDequeuedBufferCount(2) != NO_ERROR) return 1;
        int last = -1;
        sp<GraphicBuffer> lastBuffer;
        if (!q.attach(&last) || last <= q.held ||
            q.producer->BufferQueueProducer::requestBuffer(last, &lastBuffer) != NO_ERROR) return 1;
        q.listen();
        service->latest()->onUnFreeze(getpid());
        int extra = -1;
        if (q.extra(&extra) != NO_ERROR) return 1;
        sp<GraphicBuffer> retained, replaced;
        if (!check(q.producer->BufferQueueProducer::requestBuffer(q.held, &retained) == NO_ERROR &&
                   retained == q.heldBuffer && extra != q.held && extra != q.acquired &&
                   q.producer->BufferQueueProducer::requestBuffer(extra, &replaced) == NO_ERROR &&
                   replaced != lastBuffer && q.release(q.acquired, 1),
                   "last-producer-slot-only-is-reclaimed")) return 1;
    }
    {
        Queue q;
        q.listen();
        auto event = service->latest();
        const void* address = q.producer.get();
        wp<BufferQueueProducer> weak = q.producer;
        q.producer.clear();
        int delivered = 0;
        auto count = [&delivered](const void*) { ++delivered; };
        manager->registerSelfUnFreezeListener(const_cast<void*>(address), count, nullptr, false);
        event->onUnFreeze(getpid());
        manager->unRegisterSelfUnFreezeListener(const_cast<void*>(address));
        if (!check(weak.promote() == nullptr && delivered == 1,
                   "destructor-removes-self-key")) return 1;
    }
    puts("producer-freeze-probe passed=9");
    return 0;
}
