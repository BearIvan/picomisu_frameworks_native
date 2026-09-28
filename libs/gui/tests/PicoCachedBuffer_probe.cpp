// Copyright 2026 Picomisu contributors
// SPDX-License-Identifier: Apache-2.0
// Public-operation comparison with the selected source/factory libraries.
#include <gui/BufferQueueConsumer.h>
#include <gui/BufferQueueCore.h>
#include <gui/BufferQueueProducer.h>
#include <gui/IConsumerListener.h>
#include <gui/IProducerListener.h>
#include <system/window.h>
#include <cstdio>
#include <new>
#include <utility>
#include <vector>
using namespace android;
namespace {
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
    sp<BufferQueueProducer> producer = padded<BufferQueueProducer, 1024>(core);
    sp<BufferQueueConsumer> consumer = padded<BufferQueueConsumer, 1024>(core);
    bool connect() {
        IGraphicBufferProducer::QueueBufferOutput output;
        return consumer->BufferQueueConsumer::connect(new Idle, false) == NO_ERROR &&
               producer->BufferQueueProducer::connect(nullptr, NATIVE_WINDOW_API_CPU, false, &output) == NO_ERROR &&
               producer->BufferQueueProducer::allowAllocation(false) == NO_ERROR;
    }
    bool attach(const sp<GraphicBuffer>& b, uint64_t id, bool expectBuffer = true) {
        int slot = -1;
        if (producer->BufferQueueProducer::attachCachedBuffer(&slot, b, id) != NO_ERROR) return false;
        sp<GraphicBuffer> got;
        if (producer->BufferQueueProducer::requestBuffer(slot, &got) != NO_ERROR) return false;
        bool valid = expectBuffer ? got != nullptr && (!b || got == b) : got == nullptr;
        return producer->BufferQueueProducer::detachBuffer(slot) == NO_ERROR && valid;
    }
};
sp<GraphicBuffer> buffer() {
    sp<GraphicBuffer> b = padded<GraphicBuffer, 512>();
    b->width = b->height = b->stride = b->layerCount = 1;
    b->format = HAL_PIXEL_FORMAT_RGBA_8888;
    return b;
}
bool check(bool good, const char* label) {
    if (!good) fprintf(stderr, "cached-buffer fixture failed: %s\n", label);
    else printf("cached-buffer %s=1\n", label);
    return good;
}
}
int main() {
    {
        Queue q;
        sp<GraphicBuffer> b = buffer();
        int slot = -1;
        if (!check(q.producer->BufferQueueProducer::attachCachedBuffer(nullptr, b, b->getId()) == BAD_VALUE &&
                   q.producer->BufferQueueProducer::attachCachedBuffer(&slot, nullptr, 0) == BAD_VALUE && slot == -1,
                   "invalid-arguments")) return 1;
        if (!check(q.producer->BufferQueueProducer::attachCachedBuffer(&slot, b, b->getId()) == NO_INIT,
                   "unconnected")) return 1;
    }
    {
        Queue q;
        if (!q.connect()) return 1;
        int slot = -1;
        if (!check(q.producer->BufferQueueProducer::attachCachedBuffer(&slot, nullptr, UINT64_MAX) == NAME_NOT_FOUND && slot == -1,
                   "unknown-id-preserves-slot")) return 1;
        auto b = buffer();
        if (!check(q.attach(b, b->getId()) && q.attach(nullptr, b->getId()),
                   "register-detach-fetch-same-buffer")) return 1;
    }
    {
        Queue q;
        if (!q.connect()) return 1;
        auto b = buffer();
        if (!check(q.attach(b, UINT64_MAX) &&
                   q.producer->BufferQueueProducer::attachCachedBuffer(nullptr, nullptr, b->getId()) == BAD_VALUE &&
                   q.attach(nullptr, b->getId()), "register-by-actual-buffer-id")) return 1;
        int slot = -1;
        if (q.producer->BufferQueueProducer::attachCachedBuffer(&slot, nullptr, UINT64_MAX) != NAME_NOT_FOUND) return 1;
    }
    {
        Queue q;
        if (!q.connect()) return 1;
        auto b = buffer();
        if (q.producer->BufferQueueProducer::setGenerationNumber(123) != NO_ERROR) return 1;
        if (!check(q.attach(b, b->getId()), "cached-entry-has-no-generation-check")) return 1;
    }
    {
        Queue q;
        if (!q.connect()) return 1;
        if (!check(q.attach(buffer(), 0, false), "zero-id-does-not-replace-empty-slot")) return 1;
    }
    {
        Queue q;
        if (!q.connect()) return 1;
        std::vector<sp<GraphicBuffer>> bs;
        for (int i = 0; i < 5; ++i) {
            bs.push_back(buffer());
            if (!q.attach(bs.back(), bs.back()->getId())) return 1;
        }
        if (!q.attach(nullptr, bs[0]->getId())) return 1;
        auto sixth = buffer();
        if (!q.attach(sixth, sixth->getId())) return 1;
        int slot = -1;
        if (!check(q.producer->BufferQueueProducer::attachCachedBuffer(&slot, nullptr, bs[1]->getId()) == NAME_NOT_FOUND &&
                   q.attach(nullptr, bs[0]->getId()), "five-entry-lru-fetch-refreshes")) return 1;
    }
    {
        Queue q;
        if (!q.connect()) return 1;
        std::vector<sp<GraphicBuffer>> bs;
        for (int i = 0; i < 5; ++i) {
            bs.push_back(buffer());
            if (!q.attach(bs.back(), bs.back()->getId())) return 1;
        }
        if (!q.attach(bs[4], bs[4]->getId())) return 1;
        int slot = -1;
        if (!check(q.producer->BufferQueueProducer::attachCachedBuffer(&slot, nullptr, bs[0]->getId()) == NAME_NOT_FOUND &&
                   q.attach(nullptr, bs[1]->getId()), "evicts-before-existing-id-update")) return 1;
    }
    {
        Queue q;
        if (!q.connect()) return 1;
        auto b = buffer();
        if (!q.attach(b, b->getId())) return 1;
        if (q.producer->BufferQueueProducer::disconnect(NATIVE_WINDOW_API_CPU) != NO_ERROR) return 1;
        IGraphicBufferProducer::QueueBufferOutput output;
        if (q.producer->BufferQueueProducer::connect(nullptr, NATIVE_WINDOW_API_CPU, false, &output) != NO_ERROR) return 1;
        int slot = -1;
        const status_t status = q.producer->BufferQueueProducer::attachCachedBuffer(&slot, nullptr, b->getId());
        if (status != NAME_NOT_FOUND) return 1;
        printf("cached-buffer reconnect-status=%d\n", status);
        if (status == NO_ERROR && q.producer->BufferQueueProducer::detachBuffer(slot) != NO_ERROR) return 1;
    }
    return 0;
}
