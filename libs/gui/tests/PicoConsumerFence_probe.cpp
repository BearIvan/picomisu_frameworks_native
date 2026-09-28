// Copyright 2026 Picomisu contributors
// SPDX-License-Identifier: Apache-2.0
// Factory/source comparison of local ready and release paths, without GPU allocation.
#include <binder/Binder.h>
#include <gui/BufferItem.h>
#include <gui/BufferQueueConsumer.h>
#include <gui/BufferQueueCore.h>
#include <gui/BufferQueueProducer.h>
#include <gui/IProducerListener.h>
#include <gui/IConsumerListener.h>
#include <system/window.h>
#include <unistd.h>

#include <cstdio>
#include <new>
#include <utility>

using namespace android;
namespace {
// Constructors and destructors are supplied by the selected libgui/libui.
// Padding avoids allocating a factory object using a source-private sizeof.
template<class T, size_t Reserve, class... Args>
sp<T> padded(Args&&... args) {
    static_assert(sizeof(T) <= Reserve, "Insufficient source object storage");
    return ::new (::operator new(Reserve)) T(std::forward<Args>(args)...);
}
template<class T, size_t Reserve>
class PaddedValue {
public:
    template<class... Args> explicit PaddedValue(Args&&... args) {
        static_assert(sizeof(T) <= Reserve, "Insufficient value storage");
        ::new (storage) T(std::forward<Args>(args)...);
    }
    ~PaddedValue() { get()->~T(); }
    T* get() { return reinterpret_cast<T*>(storage); }
private:
    alignas(T) unsigned char storage[Reserve];
};

class Idle : public BnConsumerListener {
public:
    void onDisconnect() override { events.push_back(1); }
    void onFrameAvailable(const BufferItem&) override { events.push_back(2); }
    void onFrameReplaced(const BufferItem&) override { events.push_back(3); }
    void onBuffersReleased() override { events.push_back(4); }
    void onSidebandStreamChanged() override { events.push_back(5); }
    std::vector<int> events;
};
class Listener : public BnProducerListener {
public:
    bool needsReleaseNotify() override { return false; }
    void onBufferReleased() override { ++ordinary; }
    void onBufferReleasedWithFence(const sp<Fence>& fence, uint64_t id, bool flag) override {
        ++calls;
        lastFence = fence;
        lastId = id;
        lastFlag = flag;
    }
    int calls = 0, ordinary = 0;
    uint64_t lastId = 0;
    bool lastFlag = true;
    sp<Fence> lastFence;
};
class Relay : public BBinder {
public:
    int calls = 0;
protected:
    status_t onTransact(uint32_t, const Parcel&, Parcel*, uint32_t) override {
        ++calls;
        return UNKNOWN_TRANSACTION;
    }
};

struct Queue {
    sp<BufferQueueCore> core = padded<BufferQueueCore, 8192>();
    sp<BufferQueueConsumer> consumer = padded<BufferQueueConsumer, 1024>(core);
    sp<BufferQueueProducer> producer = padded<BufferQueueProducer, 1024>(core);
    sp<Listener> listener = new Listener;
    sp<Idle> receiver = new Idle;
    bool connect() {
        IGraphicBufferProducer::QueueBufferOutput output;
        return consumer->BufferQueueConsumer::connect(receiver, false) == NO_ERROR &&
               producer->BufferQueueProducer::connect(listener, NATIVE_WINDOW_API_CPU, false, &output) == NO_ERROR &&
               producer->BufferQueueProducer::allowAllocation(false) == NO_ERROR;
    }
};

sp<GraphicBuffer> buffer(uint64_t usage) {
    sp<GraphicBuffer> b = padded<GraphicBuffer, 512>();
    b->width = b->height = b->stride = b->layerCount = 1;
    b->format = HAL_PIXEL_FORMAT_RGBA_8888;
    b->usage = usage;
    return b;
}
bool check(bool result, const char* label) {
    if (!result) fprintf(stderr, "Consumer fence fixture failed: %s\n", label);
    return result;
}
}

int main() {
    sp<Relay> relay = new Relay;
    sp<IGraphicBufferConsumer> proxy = interface_cast<IGraphicBufferConsumer>(relay);
    if (!check(proxy->notifyFenceReady(Fence::NO_FENCE, UINT64_MAX, 0) == NO_ERROR &&
               relay->calls == 0, "local proxy default")) return 1;
    puts("consumer-fence proxy-default-no-transaction=1");
    {
        Queue q;
        if (!check(q.connect(), "ready queue connect")) return 1;
        sp<GraphicBuffer> b = buffer(0x200000000ULL);
        int slot = -1;
        if (!check(q.consumer->BufferQueueConsumer::attachBuffer(&slot, b) == NO_ERROR,
                   "ready attach")) return 1;
        int fds[2];
        if (!check(pipe(fds) == 0, "pipe")) return 1;
        sp<Fence> ready = new Fence(fds[0]);
        if (!check(q.consumer->notifyFenceReady(ready, b->getId(), slot) == NO_ERROR &&
                   q.consumer->BufferQueueConsumer::releaseBuffer(slot, 0, Fence::NO_FENCE, EGL_NO_DISPLAY, EGL_NO_SYNC_KHR) == NO_ERROR,
                   "notify and release")) { close(fds[1]); return 1; }
        int dequeued = -1;
        sp<Fence> received;
        status_t result = q.producer->BufferQueueProducer::dequeueBuffer(&dequeued, &received,
                1, 1, HAL_PIXEL_FORMAT_RGBA_8888, 0, nullptr, nullptr);
        bool valid = result == IGraphicBufferProducer::BUFFER_NEEDS_REALLOCATION &&
                     dequeued == slot && received == ready &&
                     (b->getUsage() & 0xf00000000ULL) == 0;
        char sent = 'F', got = 0;
        valid = valid && write(fds[1], &sent, 1) == 1 && read(received->get(), &got, 1) == 1 && sent == got;
        close(fds[1]);
        if (!check(valid, "local ready fence consumed, pipe FD retained")) return 1;
        if (!check(q.producer->BufferQueueProducer::cancelBuffer(dequeued, Fence::NO_FENCE) == NO_ERROR,
                   "cancel fixture")) return 1;
        puts("consumer-fence ready-dequeue-descriptor-and-usage=1");
    }
    {
        Queue q;
        if (!check(q.connect(), "release queue connect")) return 1;
        sp<GraphicBuffer> b = buffer(0x100000000ULL);
        int slot = -1;
        if (!check(q.consumer->BufferQueueConsumer::attachBuffer(&slot, b) == NO_ERROR,
                   "release attach")) return 1;
        sp<Fence> fence = new Fence;
        if (!check(q.consumer->BufferQueueConsumer::releaseBuffer(slot, 0, fence, EGL_NO_DISPLAY, EGL_NO_SYNC_KHR) == NO_ERROR &&
                   q.listener->calls == 1 && q.listener->ordinary == 0 &&
                   q.listener->lastFence == fence && q.listener->lastId == b->getId() &&
                   !q.listener->lastFlag &&
                   q.consumer->BufferQueueConsumer::releaseBuffer(slot, 0, fence, EGL_NO_DISPLAY, EGL_NO_SYNC_KHR) == BAD_VALUE,
                   "special detach and callback")) return 1;
        puts("consumer-fence special-release-detach-callback=1");
    }
    {
        Queue q;
        if (!check(q.connect() && q.producer->BufferQueueProducer::setAsyncMode(true) == NO_ERROR,
                   "replacement queue connect")) return 1;
        sp<GraphicBuffer> first = buffer(0x100000000ULL), second = buffer(0);
        int firstSlot = -1, secondSlot = -1, thirdSlot = -1;
        PaddedValue<IGraphicBufferProducer::QueueBufferInput, 1024> firstInput(0, false, HAL_DATASPACE_UNKNOWN,
                Rect::EMPTY_RECT, NATIVE_WINDOW_SCALING_MODE_FREEZE, 0, Fence::NO_FENCE);
        PaddedValue<IGraphicBufferProducer::QueueBufferInput, 1024> secondInput(0, false, HAL_DATASPACE_UNKNOWN,
                Rect::EMPTY_RECT, NATIVE_WINDOW_SCALING_MODE_FREEZE, 0, Fence::NO_FENCE);
        PaddedValue<IGraphicBufferProducer::QueueBufferOutput, 1024> output;
        if (!check(q.producer->BufferQueueProducer::attachBuffer(&firstSlot, first) == NO_ERROR &&
                   q.producer->BufferQueueProducer::queueBuffer(firstSlot, *firstInput.get(), output.get()) == NO_ERROR &&
                   q.producer->BufferQueueProducer::attachBuffer(&secondSlot, second) == NO_ERROR &&
                   q.producer->BufferQueueProducer::queueBuffer(secondSlot, *secondInput.get(), output.get()) == NO_ERROR &&
                   output.get()->bufferReplaced && q.listener->calls == 1 && q.listener->lastFlag &&
                   q.listener->lastId == first->getId() && q.listener->lastFence == Fence::NO_FENCE &&
                   q.receiver->events == std::vector<int>{2, 3} &&
                   q.producer->BufferQueueProducer::attachBuffer(&thirdSlot, buffer(0)) == NO_ERROR &&
                   thirdSlot == firstSlot, "queued replacement")) return 1;
        puts("consumer-fence queued-special-replacement=1");
    }
    {
        // Check the existing native producer disconnect path in both libraries.
        Queue q;
        if (!check(q.connect() && q.producer->BufferQueueProducer::disconnect(NATIVE_WINDOW_API_CPU) == NO_ERROR &&
                   q.receiver->events == std::vector<int>{4, 1}, "existing onDisconnect")) return 1;
        puts("consumer-fence existing-disconnect-order=1");
    }
    puts("consumer-probe passed=5");
    return 0;
}
