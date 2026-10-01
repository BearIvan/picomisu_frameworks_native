/*
 * Copyright 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <atomic>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <sys/time.h>

#define LOG_TAG "BufferQueueProducer"
#define ATRACE_TAG ATRACE_TAG_GRAPHICS
//#define LOG_NDEBUG 0

#if DEBUG_ONLY_CODE
#define VALIDATE_CONSISTENCY() do { mCore->validateConsistencyLocked(); } while (0)
#else
#define VALIDATE_CONSISTENCY()
#endif

#define EGL_EGLEXT_PROTOTYPES

#include <binder/IPCThreadState.h>
#include <binder/FreezeManager.h>
#include <cutils/properties.h>
#include <gui/BufferItem.h>
#include <gui/BufferQueueCore.h>
#include <gui/BufferQueueProducer.h>
#include <gui/GLConsumer.h>
#include <gui/IConsumerListener.h>
#include <gui/IProducerListener.h>
#include <private/gui/BufferQueueThreadState.h>

#include <utils/Log.h>
#include <utils/Trace.h>

#include <system/window.h>

namespace android {

static constexpr uint32_t BQ_LAYER_COUNT = 1;

BufferQueueProducer::BufferQueueProducer(const sp<BufferQueueCore>& core,
        bool consumerIsSurfaceFlinger) :
    mCore(core),
    mSlots(core->mSlots),
    mConsumerName(),
    mStickyTransform(0),
    mConsumerIsSurfaceFlinger(consumerIsSurfaceFlinger),
    mLastQueueBufferFence(Fence::NO_FENCE),
    mLastQueuedTransform(0),
    mCallbackMutex(),
    mNextCallbackTicket(0),
    mCurrentCallbackTicket(0),
    mCallbackCondition(),
    mDequeueTimeout(-1),
    mDequeueWaitingForAllocation(false),
    mAutoBufferCountEnable(property_get_bool("persist.sys.autobuffercountenable", true)),
    mBufferReleaseFrame(property_get_int32("persist.sys.bufferelease.frame", 50)) {}

BufferQueueProducer::~BufferQueueProducer() {
    FreezeManager::getInstance()->unRegisterSelfUnFreezeListener(this);
}

status_t BufferQueueProducer::requestBuffer(int slot, sp<GraphicBuffer>* buf) {
    ATRACE_CALL();
    BQ_LOGV("requestBuffer: slot %d", slot);
    BufferQueueCore::MutexLock lock(mCore->mMutex);

    if (mCore->mIsAbandoned) {
        BQ_LOGE("requestBuffer: BufferQueue has been abandoned");
        return NO_INIT;
    }

    if (mCore->mConnectedApi == BufferQueueCore::NO_CONNECTED_API) {
        BQ_LOGE("requestBuffer: BufferQueue has no connected producer");
        return NO_INIT;
    }

    if (slot < 0 || slot >= BufferQueueDefs::NUM_BUFFER_SLOTS) {
        BQ_LOGE("requestBuffer: slot index %d out of range [0, %d)",
                slot, BufferQueueDefs::NUM_BUFFER_SLOTS);
        return BAD_VALUE;
    } else if (!mSlots[slot].mBufferState.isDequeued()) {
        BQ_LOGE("requestBuffer: slot %d is not owned by the producer "
                "(state = %s)", slot, mSlots[slot].mBufferState.string());
        return BAD_VALUE;
    }

    mSlots[slot].mRequestBufferCalled = true;
    *buf = mSlots[slot].mGraphicBuffer;
    return NO_ERROR;
}

status_t BufferQueueProducer::setMaxDequeuedBufferCount(
        int maxDequeuedBuffers) {
    ATRACE_CALL();
    BQ_LOGV("setMaxDequeuedBufferCount: maxDequeuedBuffers = %d",
            maxDequeuedBuffers);

    sp<IConsumerListener> listener;
    { // Autolock scope
        BufferQueueCore::MutexLock lock(mCore->mMutex);
        if (maxDequeuedBuffers == -2 && mConsumerIsSurfaceFlinger) {
            mPicoSingleLayerDequeues = true;
            return NO_ERROR;
        }
        // Match PICO's 32-bit count arithmetic, including the private consumer
        // allowance. It uses this marker before the normal quota validation.
        uint32_t adjusted = static_cast<uint32_t>(maxDequeuedBuffers) +
                static_cast<uint32_t>(mPicoSingleLayerDequeues);
        if (mCore->mHasPicoConsumer) {
            adjusted += 2;
            maxDequeuedBuffers = std::max(1, static_cast<int32_t>(adjusted));
        } else {
            maxDequeuedBuffers = static_cast<int32_t>(adjusted);
        }
        mCore->waitWhileAllocatingLocked(&mCore->mMutex);

        if (mCore->mIsAbandoned) {
            BQ_LOGE("setMaxDequeuedBufferCount: BufferQueue has been "
                    "abandoned");
            return NO_INIT;
        }

        if (maxDequeuedBuffers == mCore->mMaxDequeuedBufferCount) {
            return NO_ERROR;
        }

        // The new maxDequeuedBuffer count should not be violated by the number
        // of currently dequeued buffers
        int dequeuedCount = 0;
        for (int s : mCore->mActiveBuffers) {
            if (mSlots[s].mBufferState.isDequeued()) {
                dequeuedCount++;
            }
        }
        if (dequeuedCount > maxDequeuedBuffers) {
            BQ_LOGE("setMaxDequeuedBufferCount: the requested maxDequeuedBuffer"
                    "count (%d) exceeds the current dequeued buffer count (%d)",
                    maxDequeuedBuffers, dequeuedCount);
            return BAD_VALUE;
        }

        int bufferCount = mCore->getMinUndequeuedBufferCountLocked();
        bufferCount += maxDequeuedBuffers;

        if (bufferCount > BufferQueueDefs::NUM_BUFFER_SLOTS) {
            BQ_LOGE("setMaxDequeuedBufferCount: bufferCount %d too large "
                    "(max %d)", bufferCount, BufferQueueDefs::NUM_BUFFER_SLOTS);
            return BAD_VALUE;
        }

        const int minBufferSlots = mCore->getMinMaxBufferCountLocked();
        if (bufferCount < minBufferSlots) {
            BQ_LOGE("setMaxDequeuedBufferCount: requested buffer count %d is "
                    "less than minimum %d", bufferCount, minBufferSlots);
            return BAD_VALUE;
        }

        if (bufferCount > mCore->mMaxBufferCount) {
            BQ_LOGE("setMaxDequeuedBufferCount: %d dequeued buffers would "
                    "exceed the maxBufferCount (%d) (maxAcquired %d async %d "
                    "mDequeuedBufferCannotBlock %d)", maxDequeuedBuffers,
                    mCore->mMaxBufferCount, mCore->mMaxAcquiredBufferCount,
                    mCore->mAsyncMode, mCore->mDequeueBufferCannotBlock);
            return BAD_VALUE;
        }

        int delta = maxDequeuedBuffers - mCore->mMaxDequeuedBufferCount;
        if (!mCore->adjustAvailableSlotsLocked(delta)) {
            return BAD_VALUE;
        }
        mCore->mMaxDequeuedBufferCount = maxDequeuedBuffers;
        VALIDATE_CONSISTENCY();
        if (delta < 0) {
            listener = mCore->mConsumerListener;
        }
        pthread_cond_broadcast(&mCore->mDequeueCondition);
    } // Autolock scope

    // Call back without lock held
    if (listener != nullptr) {
        listener->onBuffersReleased();
    }

    return NO_ERROR;
}

status_t BufferQueueProducer::setAsyncMode(bool async) {
    ATRACE_CALL();
    BQ_LOGV("setAsyncMode: async = %d", async);

    sp<IConsumerListener> listener;
    { // Autolock scope
        BufferQueueCore::MutexLock lock(mCore->mMutex);
        mCore->waitWhileAllocatingLocked(&mCore->mMutex);

        if (mCore->mIsAbandoned) {
            BQ_LOGE("setAsyncMode: BufferQueue has been abandoned");
            return NO_INIT;
        }

        if (async == mCore->mAsyncMode) {
            return NO_ERROR;
        }

        if ((mCore->mMaxAcquiredBufferCount + mCore->mMaxDequeuedBufferCount +
                (async || mCore->mDequeueBufferCannotBlock ? 1 : 0)) >
                mCore->mMaxBufferCount) {
            BQ_LOGE("setAsyncMode(%d): this call would cause the "
                    "maxBufferCount (%d) to be exceeded (maxAcquired %d "
                    "maxDequeued %d mDequeueBufferCannotBlock %d)", async,
                    mCore->mMaxBufferCount, mCore->mMaxAcquiredBufferCount,
                    mCore->mMaxDequeuedBufferCount,
                    mCore->mDequeueBufferCannotBlock);
            return BAD_VALUE;
        }

        int delta = mCore->getMaxBufferCountLocked(async,
                mCore->mDequeueBufferCannotBlock, mCore->mMaxBufferCount)
                - mCore->getMaxBufferCountLocked();

        if (!mCore->adjustAvailableSlotsLocked(delta)) {
            BQ_LOGE("setAsyncMode: BufferQueue failed to adjust the number of "
                    "available slots. Delta = %d", delta);
            return BAD_VALUE;
        }
        mCore->mAsyncMode = async;
        VALIDATE_CONSISTENCY();
        pthread_cond_broadcast(&mCore->mDequeueCondition);
        if (delta < 0) {
            listener = mCore->mConsumerListener;
        }
    } // Autolock scope

    // Call back without lock held
    if (listener != nullptr) {
        listener->onBuffersReleased();
    }
    return NO_ERROR;
}

int BufferQueueProducer::getFreeBufferLocked() const {
    if (mCore->mFreeBuffers.empty()) {
        return BufferQueueCore::INVALID_BUFFER_SLOT;
    }
    auto selected = mCore->mFreeBuffers.begin();
    const sp<GraphicBuffer>& buffer = mSlots[*selected].mGraphicBuffer;
    if (mPicoFenceReadyMode && buffer != nullptr &&
            (buffer->getUsage() & 0xf00000000ULL) == 0x200000000ULL &&
            !mSlots[*selected].mPicoFenceReady) {
        auto ready = std::find_if(std::next(selected), mCore->mFreeBuffers.end(),
                [this](int slot) { return mSlots[slot].mPicoFenceReady; });
        if (ready != mCore->mFreeBuffers.end()) selected = ready;
    }
    int slot = *selected;
    mCore->mFreeBuffers.erase(selected);
    return slot;
}

int BufferQueueProducer::getFreeSlotLocked() const {
    if (mCore->mFreeSlots.empty()) {
        return BufferQueueCore::INVALID_BUFFER_SLOT;
    }
    int slot = *(mCore->mFreeSlots.begin());
    mCore->mFreeSlots.erase(slot);
    return slot;
}

status_t BufferQueueProducer::waitForFreeSlotThenRelock(FreeSlotCaller caller,
        pthread_mutex_t* mutex, int* found, bool* outBufferReleased) const {
    auto callerString = (caller == FreeSlotCaller::Dequeue) ?
            "dequeueBuffer" : "attachBuffer";
    bool tryAgain = true;
    while (tryAgain) {
        if (mCore->mIsAbandoned) {
            BQ_LOGE("%s: BufferQueue has been abandoned", callerString);
            return NO_INIT;
        }

        int dequeuedCount = 0;
        int acquiredCount = 0;
        for (int s : mCore->mActiveBuffers) {
            if (mSlots[s].mBufferState.isDequeued()) {
                ++dequeuedCount;
            }
            if (mSlots[s].mBufferState.isAcquired()) {
                ++acquiredCount;
            }
        }

        // Producers are not allowed to dequeue more than
        // mMaxDequeuedBufferCount buffers.
        // This check is only done if a buffer has already been queued
        if (mCore->mBufferHasBeenQueued &&
                dequeuedCount >= mCore->mMaxDequeuedBufferCount) {
            // Suppress error logs when timeout is non-negative. The factory logs this
            // before it checks for a pending self unfreeze.
            if (mDequeueTimeout < 0) {
                BQ_LOGE("%s: attempting to exceed the max dequeued buffer "
                        "count (%d)", callerString, mCore->mMaxDequeuedBufferCount);
            }
            if (!mPicoUnfreezePending->exchange(false, std::memory_order_relaxed)) {
                return INVALID_OPERATION;
            }
            // PICO cancels the last producer-owned active slot after unfreeze.
            // Consumer-only slots are excluded by the dequeue-count predicate.
            int recovered = BufferQueueCore::INVALID_BUFFER_SLOT;
            for (int slot : mCore->mActiveBuffers) {
                if (mSlots[slot].mBufferState.isDequeued()) recovered = slot;
            }
            if (recovered != BufferQueueCore::INVALID_BUFFER_SLOT) {
                mSlots[recovered].mBufferState.cancel();
                mCore->mActiveBuffers.erase(recovered);
                mCore->mFreeBuffers.push_back(recovered);
                mSlots[recovered].mFence = Fence::NO_FENCE;
            }
        }

        *found = BufferQueueCore::INVALID_BUFFER_SLOT;

        // If we disconnect and reconnect quickly, we can be in a state where
        // our slots are empty but we have many buffers in the queue. This can
        // cause us to run out of memory if we outrun the consumer. Wait here if
        // it looks like we have too many buffers queued up.
        const int maxBufferCount = mCore->getMaxBufferCountLocked();
        bool tooManyBuffers = mCore->mQueue.size()
                            > static_cast<size_t>(maxBufferCount);
        if (tooManyBuffers) {
            BQ_LOGV("%s: queue size is %zu, waiting", callerString,
                    mCore->mQueue.size());
        } else {
            // If in shared buffer mode and a shared buffer exists, always
            // return it.
            if (mCore->mSharedBufferMode && mCore->mSharedBufferSlot !=
                    BufferQueueCore::INVALID_BUFFER_SLOT) {
                *found = mCore->mSharedBufferSlot;
            } else {
                // PICO: in fence-ready mode, dequeue prefers a new slot while the queue is
                // below its maximum buffer count, so it does not wait on a buffer whose
                // ready fence has not arrived yet. (The factory does not check
                // mAllowAllocation here; without it dequeueBuffer would dereference the
                // empty slot's null buffer.)
                const bool picoPreferFreeSlot = caller == FreeSlotCaller::Dequeue &&
                        mPicoFenceReadyMode && mCore->mAllowAllocation &&
                        mCore->getMaxBufferCountLocked() >
                                static_cast<int>(mCore->mFreeBuffers.size() +
                                                 mCore->mActiveBuffers.size());
                if (caller == FreeSlotCaller::Dequeue && !picoPreferFreeSlot) {
                    // If we're calling this from dequeue, prefer free buffers
                    int slot = getFreeBufferLocked();
                    if (slot != BufferQueueCore::INVALID_BUFFER_SLOT) {
                        *found = slot;
                    } else if (mCore->mAllowAllocation) {
                        *found = getFreeSlotLocked();
                    }
                } else {
                    // If we're calling this from attach, prefer free slots
                    int slot = getFreeSlotLocked();
                    if (slot != BufferQueueCore::INVALID_BUFFER_SLOT) {
                        *found = slot;
                    } else {
                        *found = getFreeBufferLocked();
                    }
                }
            }
        }

        // If no buffer is found, or if the queue has too many buffers
        // outstanding, wait for a buffer to be acquired or released, or for the
        // max buffer count to change.
        tryAgain = (*found == BufferQueueCore::INVALID_BUFFER_SLOT) ||
                   tooManyBuffers;

        // PICO autobuffercount: a SurfaceFlinger consumer that did not ask for the extra
        // single-layer dequeue slot gets one idle buffer freed every mBufferReleaseFrame
        // dequeues while it holds at least three buffers, so the buffer count shrinks back
        // after a burst.
        if (mConsumerIsSurfaceFlinger && !mPicoSingleLayerDequeues &&
                *found != BufferQueueCore::INVALID_BUFFER_SLOT && outBufferReleased != nullptr &&
                mAutoBufferCountEnable) {
            const size_t freeBuffers = mCore->mFreeBuffers.size();
            if (freeBuffers >= 1 && freeBuffers + mCore->mActiveBuffers.size() >= 3) {
                if (mAutoBufferReleaseCounter++ >= mBufferReleaseFrame) {
                    const int slot = mCore->mFreeBuffers.front();
                    if (mSlots[slot].mGraphicBuffer != nullptr) {
                        mCore->clearBufferSlotLocked(slot);
                        mCore->mFreeBuffers.pop_front();
                        mCore->mFreeSlots.insert(slot);
                        *outBufferReleased = true;
                        BQ_LOGI("waitForFreeSlotThenRelock: release buffer callingpid = %d",
                                IPCThreadState::self()->getCallingPid());
                    }
                    mAutoBufferReleaseCounter = 0;
                }
            } else if (freeBuffers == 0) {
                mAutoBufferReleaseCounter = 0;
            }
        }

        if (tryAgain) {
            // Return an error if we're in non-blocking mode (producer and
            // consumer are controlled by the application).
            // However, the consumer is allowed to briefly acquire an extra
            // buffer (which could cause us to have to wait here), which is
            // okay, since it is only used to implement an atomic acquire +
            // release (e.g., in GLConsumer::updateTexImage())
            if ((mCore->mDequeueBufferCannotBlock || mCore->mAsyncMode) &&
                    (acquiredCount <= mCore->mMaxAcquiredBufferCount)) {
                return WOULD_BLOCK;
            }
            if (mDequeueTimeout >= 0) {
                // Factory: absolute CLOCK_REALTIME deadline from gettimeofday().
                constexpr nsecs_t kNsPerSec = 1000000000;
                struct timeval now;
                gettimeofday(&now, nullptr);
                struct timespec deadline;
                deadline.tv_sec = now.tv_sec + static_cast<int>(mDequeueTimeout / kNsPerSec);
                deadline.tv_nsec = now.tv_usec * 1000 + mDequeueTimeout % kNsPerSec;
                if (deadline.tv_nsec > kNsPerSec - 1) {
                    deadline.tv_nsec -= kNsPerSec;
                    deadline.tv_sec += 1;
                }
                if (pthread_cond_timedwait(&mCore->mDequeueCondition, mutex, &deadline) ==
                        ETIMEDOUT) {
                    return TIMED_OUT;
                }
            } else {
                pthread_cond_wait(&mCore->mDequeueCondition, mutex);
            }
        }
    } // while (tryAgain)

    return NO_ERROR;
}

status_t BufferQueueProducer::dequeueBuffer(int* outSlot, sp<android::Fence>* outFence,
                                            uint32_t width, uint32_t height, PixelFormat format,
                                            uint64_t usage, uint64_t* outBufferAge,
                                            FrameEventHistoryDelta* outTimestamps) {
    ATRACE_CALL();
    { // Autolock scope
        BufferQueueCore::MutexLock lock(mCore->mMutex);
        mConsumerName = mCore->mConsumerName;

        if (mCore->mIsAbandoned) {
            BQ_LOGE("dequeueBuffer: BufferQueue has been abandoned");
            return NO_INIT;
        }

        if (mCore->mConnectedApi == BufferQueueCore::NO_CONNECTED_API) {
            BQ_LOGE("dequeueBuffer: BufferQueue has no connected producer");
            return NO_INIT;
        }
    } // Autolock scope

    BQ_LOGV("dequeueBuffer: w=%u h=%u format=%#x, usage=%#" PRIx64, width, height, format, usage);

    if ((width && !height) || (!width && height)) {
        BQ_LOGE("dequeueBuffer: invalid size: w=%u h=%u", width, height);
        return BAD_VALUE;
    }

    status_t returnFlags = NO_ERROR;
    EGLDisplay eglDisplay = EGL_NO_DISPLAY;
    EGLSyncKHR eglFence = EGL_NO_SYNC_KHR;
    bool attachedByConsumer = false;
    // PICO: set when waitForFreeSlotThenRelock freed an idle buffer (autobuffercount).
    bool bufferReleased = false;

    { // Autolock scope
        BufferQueueCore::MutexLock lock(mCore->mMutex);

        // If we don't have a free buffer, but we are currently allocating, we wait until allocation
        // is finished such that we don't allocate in parallel.
        if (mCore->mFreeBuffers.empty() && mCore->mIsAllocating) {
            mDequeueWaitingForAllocation = true;
            mCore->waitWhileAllocatingLocked(&mCore->mMutex);
            mDequeueWaitingForAllocation = false;
            pthread_cond_broadcast(&mDequeueWaitingForAllocationCondition);
        }

        if (format == 0) {
            format = mCore->mDefaultBufferFormat;
        }

        // Enable the usage bits the consumer requested
        usage |= mCore->mConsumerUsageBits;

        const bool useDefaultSize = !width && !height;
        if (useDefaultSize) {
            width = mCore->mDefaultWidth;
            height = mCore->mDefaultHeight;
        }

        int found = BufferItem::INVALID_BUFFER_SLOT;
        while (found == BufferItem::INVALID_BUFFER_SLOT) {
            status_t status = waitForFreeSlotThenRelock(FreeSlotCaller::Dequeue,
                                                        &mCore->mMutex, &found,
                                                        &bufferReleased);
            if (status != NO_ERROR) {
                return status;
            }

            // This should not happen
            if (found == BufferQueueCore::INVALID_BUFFER_SLOT) {
                BQ_LOGE("dequeueBuffer: no available buffer slots");
                return -EBUSY;
            }

            const sp<GraphicBuffer>& buffer(mSlots[found].mGraphicBuffer);

            // If we are not allowed to allocate new buffers,
            // waitForFreeSlotThenRelock must have returned a slot containing a
            // buffer. If this buffer would require reallocation to meet the
            // requested attributes, we free it and attempt to get another one.
            if (!mCore->mAllowAllocation) {
                if (buffer->needsReallocation(width, height, format, BQ_LAYER_COUNT, usage)) {
                    if (mCore->mSharedBufferSlot == found) {
                        BQ_LOGE("dequeueBuffer: cannot re-allocate a sharedbuffer");
                        return BAD_VALUE;
                    }
                    mCore->mFreeSlots.insert(found);
                    mCore->clearBufferSlotLocked(found);
                    found = BufferItem::INVALID_BUFFER_SLOT;
                    continue;
                }
            }
        }

        const sp<GraphicBuffer>& buffer(mSlots[found].mGraphicBuffer);
        if (mCore->mSharedBufferSlot == found &&
                buffer->needsReallocation(width, height, format, BQ_LAYER_COUNT, usage)) {
            BQ_LOGE("dequeueBuffer: cannot re-allocate a shared"
                    "buffer");

            return BAD_VALUE;
        }

        if (mCore->mSharedBufferSlot != found) {
            mCore->mActiveBuffers.insert(found);
        }
        *outSlot = found;
        ATRACE_BUFFER_INDEX(found);

        attachedByConsumer = mSlots[found].mNeedsReallocation;
        mSlots[found].mNeedsReallocation = false;

        mSlots[found].mBufferState.dequeue();

        if ((buffer == nullptr) ||
                buffer->needsReallocation(width, height, format, BQ_LAYER_COUNT, usage))
        {
            mSlots[found].mAcquireCalled = false;
            mSlots[found].mGraphicBuffer = nullptr;
            mSlots[found].mRequestBufferCalled = false;
            mSlots[found].mEglDisplay = EGL_NO_DISPLAY;
            mSlots[found].mEglFence = EGL_NO_SYNC_KHR;
            mSlots[found].mFence = Fence::NO_FENCE;
            mCore->mBufferAge = 0;
            mCore->mIsAllocating = true;

            returnFlags |= BUFFER_NEEDS_REALLOCATION;
        } else {
            // We add 1 because that will be the frame number when this buffer
            // is queued
            mCore->mBufferAge = mCore->mFrameCounter + 1 - mSlots[found].mFrameNumber;
        }

        BQ_LOGV("dequeueBuffer: setting buffer age to %" PRIu64,
                mCore->mBufferAge);

        if (CC_UNLIKELY(mSlots[found].mFence == nullptr)) {
            BQ_LOGE("dequeueBuffer: about to return a NULL fence - "
                    "slot=%d w=%d h=%d format=%u",
                    found, buffer->width, buffer->height, buffer->format);
        }

        eglDisplay = mSlots[found].mEglDisplay;
        eglFence = mSlots[found].mEglFence;
        // Don't return a fence in shared buffer mode, except for the first
        // frame.
        *outFence = (mCore->mSharedBufferMode &&
                mCore->mSharedBufferSlot == found) ?
                Fence::NO_FENCE : mSlots[found].mFence;
        mSlots[found].mEglFence = EGL_NO_SYNC_KHR;
        mSlots[found].mFence = Fence::NO_FENCE;

        // If shared buffer mode has just been enabled, cache the slot of the
        // first buffer that is dequeued and mark it as the shared buffer.
        if (mCore->mSharedBufferMode && mCore->mSharedBufferSlot ==
                BufferQueueCore::INVALID_BUFFER_SLOT) {
            mCore->mSharedBufferSlot = found;
            mSlots[found].mBufferState.mShared = true;
        }
        waitForFenceReadyBufferLocked(found, outFence);
    } // Autolock scope

    // PICO: let the consumer drop its reference to the buffer freed by autobuffercount.
    if (bufferReleased) {
        sp<IConsumerListener> listener;
        {
            BufferQueueCore::MutexLock lock(mCore->mMutex);
            listener = mCore->mConsumerListener;
        }
        if (listener != nullptr) {
            listener->onBuffersReleased();
        }
    }

    if (returnFlags & BUFFER_NEEDS_REALLOCATION) {
        BQ_LOGV("dequeueBuffer: allocating a new buffer for slot %d", *outSlot);
        sp<GraphicBuffer> graphicBuffer = new GraphicBuffer(
                width, height, format, BQ_LAYER_COUNT, usage,
                {mConsumerName.string(), mConsumerName.size()});

        status_t error = graphicBuffer->initCheck();

        { // Autolock scope
            BufferQueueCore::MutexLock lock(mCore->mMutex);

            if (error == NO_ERROR && !mCore->mIsAbandoned) {
                graphicBuffer->setGenerationNumber(mCore->mGenerationNumber);
                mSlots[*outSlot].mGraphicBuffer = graphicBuffer;
            }

            mCore->mIsAllocating = false;
            pthread_cond_broadcast(&mCore->mIsAllocatingCondition);

            if (error != NO_ERROR) {
                mCore->mFreeSlots.insert(*outSlot);
                mCore->clearBufferSlotLocked(*outSlot);
                BQ_LOGE("dequeueBuffer: createGraphicBuffer failed");
                return error;
            }

            if (mCore->mIsAbandoned) {
                mCore->mFreeSlots.insert(*outSlot);
                mCore->clearBufferSlotLocked(*outSlot);
                BQ_LOGE("dequeueBuffer: BufferQueue has been abandoned");
                return NO_INIT;
            }

            VALIDATE_CONSISTENCY();
        } // Autolock scope
    }

    if (attachedByConsumer) {
        returnFlags |= BUFFER_NEEDS_REALLOCATION;
    }

    if (eglFence != EGL_NO_SYNC_KHR) {
        EGLint result = eglClientWaitSyncKHR(eglDisplay, eglFence, 0,
                1000000000);
        // If something goes wrong, log the error, but return the buffer without
        // synchronizing access to it. It's too late at this point to abort the
        // dequeue operation.
        if (result == EGL_FALSE) {
            BQ_LOGE("dequeueBuffer: error %#x waiting for fence",
                    eglGetError());
        } else if (result == EGL_TIMEOUT_EXPIRED_KHR) {
            BQ_LOGE("dequeueBuffer: timeout waiting for fence");
        }
        eglDestroySyncKHR(eglDisplay, eglFence);
    }

    BQ_LOGV("dequeueBuffer: returning slot=%d/%" PRIu64 " buf=%p flags=%#x",
            *outSlot,
            mSlots[*outSlot].mFrameNumber,
            mSlots[*outSlot].mGraphicBuffer->handle, returnFlags);

    if (outBufferAge) {
        *outBufferAge = mCore->mBufferAge;
    }
    addAndGetFrameTimestamps(nullptr, outTimestamps);

    return returnFlags;
}

// Caller owns mCore->mMutex. The factory waits once for 13,880,000 ns.
void BufferQueueProducer::waitForFenceReadyBufferLocked(int slot, sp<Fence>* outFence) {
    if (slot < 0 || slot >= BufferQueueDefs::NUM_BUFFER_SLOTS || outFence == nullptr) return;
    sp<GraphicBuffer> buffer = mSlots[slot].mGraphicBuffer;
    constexpr uint64_t mask = 0xf00000000ULL;
    if (buffer == nullptr || (buffer->getUsage() & mask) != 0x200000000ULL) return;
    mPicoFenceReadyMode = true;
    // Factory markers for SurfaceFlinger's single-layer path: 0x4 = the ready fence only
    // arrived while dequeue waited ("hit"), 0x8 = the 13.88 ms wait timed out. A buffer
    // that was ready at once, or a wake-up without a ready fence ("missed"), gets none.
    bool waitHit = false;
    bool timeout = false;
    if (mSlots[slot].mPicoFenceReady) {
        *outFence = mSlots[slot].mPicoReadyFence;
        mSlots[slot].mPicoFenceReady = false;
        mSlots[slot].mPicoReadyFence = Fence::NO_FENCE;
    } else {
        // Factory: the CLOCK_REALTIME deadline is gettimeofday() + 13,880,000 ns with no
        // tv_nsec carry. When tv_usec >= 986120 the deadline is invalid, the wait returns
        // EINVAL at once and the wake-up path below runs (as in the factory).
        struct timeval now;
        gettimeofday(&now, nullptr);
        struct timespec deadline;
        deadline.tv_sec = now.tv_sec;
        deadline.tv_nsec = now.tv_usec * 1000 + 13880000;
        const nsecs_t waitStart = systemTime(SYSTEM_TIME_MONOTONIC);
        timeout = pthread_cond_timedwait(&mCore->mPicoFenceCondition, &mCore->mMutex,
                                         &deadline) == ETIMEDOUT;
        buffer = mSlots[slot].mGraphicBuffer;
        if (buffer == nullptr) return;
        if (timeout) {
            ALOGD("dequeueBuffer wait fence ready 13.8ms timeout! slot %d, id %" PRIu64
                  " usage %" PRIu64 " delta %" PRId64,
                  slot, buffer->getId(), buffer->getUsage(),
                  systemTime(SYSTEM_TIME_MONOTONIC) - waitStart);
        } else if (mSlots[slot].mPicoFenceReady) {
            String8 freeStates;
            for (int freeSlot : mCore->mFreeBuffers) {
                freeStates.appendFormat("[%d,rdy:%d] ", freeSlot,
                                        mSlots[freeSlot].mPicoFenceReady);
            }
            ALOGD("dequeueBuffer wait fence hit(%s)! slot %d, id %" PRIu64 " %s  delta %" PRId64,
                  freeStates.string(), slot, buffer->getId(), mCore->mConsumerName.string(),
                  systemTime(SYSTEM_TIME_MONOTONIC) - waitStart);
            *outFence = mSlots[slot].mPicoReadyFence;
            mSlots[slot].mPicoFenceReady = false;
            mSlots[slot].mPicoReadyFence = Fence::NO_FENCE;
            waitHit = true;
        } else {
            ALOGD("dequeueBuffer wait fence missed! slot %d, id %" PRIu64, slot,
                  buffer->getId());
        }
    }
    uint64_t usage = buffer->getUsage() & ~mask;
    if (waitHit) {
        usage |= 0x400000000ULL;
    } else if (timeout) {
        usage |= 0x800000000ULL;
    }
    buffer->usage = usage;
}

status_t BufferQueueProducer::detachBuffer(int slot) {
    ATRACE_CALL();
    ATRACE_BUFFER_INDEX(slot);
    BQ_LOGV("detachBuffer: slot %d", slot);

    sp<IConsumerListener> listener;
    {
        BufferQueueCore::MutexLock lock(mCore->mMutex);

        if (mCore->mIsAbandoned) {
            BQ_LOGE("detachBuffer: BufferQueue has been abandoned");
            return NO_INIT;
        }

        if (mCore->mConnectedApi == BufferQueueCore::NO_CONNECTED_API) {
            BQ_LOGE("detachBuffer: BufferQueue has no connected producer");
            return NO_INIT;
        }

        if (mCore->mSharedBufferMode || mCore->mSharedBufferSlot == slot) {
            BQ_LOGE("detachBuffer: cannot detach a buffer in shared buffer mode");
            return BAD_VALUE;
        }

        if (slot < 0 || slot >= BufferQueueDefs::NUM_BUFFER_SLOTS) {
            BQ_LOGE("detachBuffer: slot index %d out of range [0, %d)",
                    slot, BufferQueueDefs::NUM_BUFFER_SLOTS);
            return BAD_VALUE;
        } else if (!mSlots[slot].mBufferState.isDequeued()) {
            BQ_LOGE("detachBuffer: slot %d is not owned by the producer "
                    "(state = %s)", slot, mSlots[slot].mBufferState.string());
            return BAD_VALUE;
        } else if (!mSlots[slot].mRequestBufferCalled) {
            BQ_LOGE("detachBuffer: buffer in slot %d has not been requested",
                    slot);
            return BAD_VALUE;
        }

        mSlots[slot].mBufferState.detachProducer();
        mCore->mActiveBuffers.erase(slot);
        mCore->mFreeSlots.insert(slot);
        mCore->clearBufferSlotLocked(slot);
        pthread_cond_broadcast(&mCore->mDequeueCondition);
        VALIDATE_CONSISTENCY();
        listener = mCore->mConsumerListener;
    }

    if (listener != nullptr) {
        listener->onBuffersReleased();
    }

    return NO_ERROR;
}

status_t BufferQueueProducer::detachNextBuffer(sp<GraphicBuffer>* outBuffer,
        sp<Fence>* outFence) {
    ATRACE_CALL();

    if (outBuffer == nullptr) {
        BQ_LOGE("detachNextBuffer: outBuffer must not be NULL");
        return BAD_VALUE;
    } else if (outFence == nullptr) {
        BQ_LOGE("detachNextBuffer: outFence must not be NULL");
        return BAD_VALUE;
    }

    sp<IConsumerListener> listener;
    {
        BufferQueueCore::MutexLock lock(mCore->mMutex);

        if (mCore->mIsAbandoned) {
            BQ_LOGE("detachNextBuffer: BufferQueue has been abandoned");
            return NO_INIT;
        }

        if (mCore->mConnectedApi == BufferQueueCore::NO_CONNECTED_API) {
            BQ_LOGE("detachNextBuffer: BufferQueue has no connected producer");
            return NO_INIT;
        }

        if (mCore->mSharedBufferMode) {
            BQ_LOGE("detachNextBuffer: cannot detach a buffer in shared buffer "
                    "mode");
            return BAD_VALUE;
        }

        mCore->waitWhileAllocatingLocked(&mCore->mMutex);

        if (mCore->mFreeBuffers.empty()) {
            return NO_MEMORY;
        }

        int found = mCore->mFreeBuffers.front();
        mCore->mFreeBuffers.remove(found);
        mCore->mFreeSlots.insert(found);

        BQ_LOGV("detachNextBuffer detached slot %d", found);

        *outBuffer = mSlots[found].mGraphicBuffer;
        *outFence = mSlots[found].mFence;
        mCore->clearBufferSlotLocked(found);
        VALIDATE_CONSISTENCY();
        listener = mCore->mConsumerListener;
    }

    if (listener != nullptr) {
        listener->onBuffersReleased();
    }

    return NO_ERROR;
}

status_t BufferQueueProducer::attachBuffer(int* outSlot,
        const sp<android::GraphicBuffer>& buffer) {
    ATRACE_CALL();

    if (outSlot == nullptr) {
        BQ_LOGE("attachBuffer: outSlot must not be NULL");
        return BAD_VALUE;
    } else if (buffer == nullptr) {
        BQ_LOGE("attachBuffer: cannot attach NULL buffer");
        return BAD_VALUE;
    }

    BufferQueueCore::MutexLock lock(mCore->mMutex);

    if (mCore->mIsAbandoned) {
        BQ_LOGE("attachBuffer: BufferQueue has been abandoned");
        return NO_INIT;
    }

    if (mCore->mConnectedApi == BufferQueueCore::NO_CONNECTED_API) {
        BQ_LOGE("attachBuffer: BufferQueue has no connected producer");
        return NO_INIT;
    }

    if (mCore->mSharedBufferMode) {
        BQ_LOGE("attachBuffer: cannot attach a buffer in shared buffer mode");
        return BAD_VALUE;
    }

    if (buffer->getGenerationNumber() != mCore->mGenerationNumber) {
        BQ_LOGE("attachBuffer: generation number mismatch [buffer %u] "
                "[queue %u]", buffer->getGenerationNumber(),
                mCore->mGenerationNumber);
        return BAD_VALUE;
    }

    mCore->waitWhileAllocatingLocked(&mCore->mMutex);

    status_t returnFlags = NO_ERROR;
    int found;
    status_t status = waitForFreeSlotThenRelock(FreeSlotCaller::Attach, &mCore->mMutex, &found);
    if (status != NO_ERROR) {
        BQ_LOGE("attachBuffer: waitForFreeSlotThenRelock fail");
        return status;
    }

    // This should not happen
    if (found == BufferQueueCore::INVALID_BUFFER_SLOT) {
        BQ_LOGE("attachBuffer: no available buffer slots");
        return -EBUSY;
    }

    *outSlot = found;
    ATRACE_BUFFER_INDEX(*outSlot);
    BQ_LOGV("attachBuffer: returning slot %d flags=%#x",
            *outSlot, returnFlags);

    mSlots[*outSlot].mGraphicBuffer = buffer;
    mSlots[*outSlot].mBufferState.attachProducer();
    mSlots[*outSlot].mEglFence = EGL_NO_SYNC_KHR;
    mSlots[*outSlot].mFence = Fence::NO_FENCE;
    mSlots[*outSlot].mRequestBufferCalled = true;
    mSlots[*outSlot].mAcquireCalled = false;
    mSlots[*outSlot].mNeedsReallocation = false;
    mCore->mActiveBuffers.insert(found);
    VALIDATE_CONSISTENCY();

    return returnFlags;
}

namespace {
// Factory uses a process-wide access counter (a plain uint64_t). Atomic ordering also
// covers accesses protected by different BufferQueueCore mutexes.
std::atomic<uint64_t> sBufferCacheAccess{0};
}

uint64_t BufferQueueProducer::getCounter() {
    return sBufferCacheAccess.fetch_add(1, std::memory_order_relaxed);
}

void BufferQueueProducer::clearAllCachedBuffersLocked() {
    mBufferCache.clear();
}

void BufferQueueProducer::dumpBuffers() {
    ALOGD("===start dump cached buffers===");
    for (auto entry : mBufferCache) {
        ALOGD("[%" PRIu64 ", sp %p, counter %" PRIu64 "]", entry.first,
              entry.second.first.get(), entry.second.second);
    }
    ALOGD("===end dump cached buffers===");
}

status_t BufferQueueProducer::queryBufferLocked(uint64_t id) {
    return mBufferCache.count(id) ? ALREADY_EXISTS : NAME_NOT_FOUND;
}

void BufferQueueProducer::fetchBufferLocked(uint64_t id, sp<GraphicBuffer>* buffer) {
    const auto entry = mBufferCache.find(id);
    if (entry == mBufferCache.end()) {
        buffer->clear();
        return;
    }
    entry->second.second = getCounter();
    *buffer = entry->second.first;
}

void BufferQueueProducer::evictBuffer() {
    if (mBufferCache.empty()) return;
    auto oldest = mBufferCache.begin();
    for (auto entry = mBufferCache.begin(); entry != mBufferCache.end(); ++entry) {
        if (entry->second.second < oldest->second.second) oldest = entry;
    }
    mBufferCache.erase(oldest);
}

void BufferQueueProducer::cacheBufferLocked(const sp<GraphicBuffer>& buffer) {
    // Factory evicts before checking whether the inserted ID already exists.
    if (mBufferCache.size() >= 5) evictBuffer();
    mBufferCache[buffer->getId()] = {buffer, getCounter()};
}

status_t BufferQueueProducer::attachCachedBuffer(int* outSlot, const sp<GraphicBuffer>& buffer,
                                                uint64_t id) {
    ATRACE_CALL();
    if (outSlot == nullptr) {
        BQ_LOGE("attachCachedBuffer: outSlot must not be NULL");
        return BAD_VALUE;
    }
    if (buffer == nullptr && id == 0) {
        BQ_LOGE("attachCachedBuffer: buffer and id are both NULL");
        return BAD_VALUE;
    }
    BufferQueueCore::MutexLock lock(mCore->mMutex);
    if (mCore->mIsAbandoned) {
        BQ_LOGE("attachCachedBuffer: BufferQueue has been abandoned");
        return NO_INIT;
    }
    if (mCore->mConnectedApi == BufferQueueCore::NO_CONNECTED_API) {
        BQ_LOGE("attachCachedBuffer: BufferQueue has no connected producer");
        return NO_INIT;
    }
    if (mCore->mSharedBufferMode) {
        BQ_LOGE("attachCachedBuffer: cannot attach a buffer in shared buffer mode");
        return BAD_VALUE;
    }
    // An id that is not cached fails without a log, as in the factory.
    if (id != 0 && buffer == nullptr && queryBufferLocked(id) == NAME_NOT_FOUND) {
        return NAME_NOT_FOUND;
    }
    mCore->waitWhileAllocatingLocked(&mCore->mMutex);
    int found;
    status_t status = waitForFreeSlotThenRelock(FreeSlotCaller::Attach, &mCore->mMutex, &found);
    if (status != NO_ERROR) {
        BQ_LOGE("attachCachedBuffer: waitForFreeSlotThenRelock fail %d", status);
        return status;
    }
    if (found == BufferQueueCore::INVALID_BUFFER_SLOT) {
        BQ_LOGE("attachCachedBuffer: no available buffer slots");
        return -EBUSY;
    }
    *outSlot = found;
    ATRACE_BUFFER_INDEX(*outSlot);
    if (id != 0) {
        if (buffer) {
            cacheBufferLocked(buffer);
            mSlots[found].mGraphicBuffer = buffer;
        } else {
            fetchBufferLocked(id, &mSlots[found].mGraphicBuffer);
        }
    }
    // PICO's zero-ID path preserves the selected slot's existing buffer.
    // Unlike ordinary attachBuffer, this entry point has no generation check.
    mSlots[found].mBufferState.attachProducer();
    mSlots[found].mEglFence = EGL_NO_SYNC_KHR;
    mSlots[found].mFence = Fence::NO_FENCE;
    mSlots[found].mRequestBufferCalled = true;
    mSlots[found].mAcquireCalled = false;
    mSlots[found].mNeedsReallocation = false;
    mCore->mActiveBuffers.insert(found);
    VALIDATE_CONSISTENCY();
    return NO_ERROR;
}

status_t BufferQueueProducer::queueBuffer(int slot,
        const QueueBufferInput &input, QueueBufferOutput *output) {
    ATRACE_CALL();
    ATRACE_BUFFER_INDEX(slot);

    int64_t requestedPresentTimestamp;
    bool isAutoTimestamp;
    android_dataspace dataSpace;
    Rect crop(Rect::EMPTY_RECT);
    int scalingMode;
    uint32_t transform;
    uint32_t stickyTransform;
    sp<Fence> acquireFence;
    bool getFrameTimestamps = false;
    input.deflate(&requestedPresentTimestamp, &isAutoTimestamp, &dataSpace,
            &crop, &scalingMode, &transform, &acquireFence, &stickyTransform,
            &getFrameTimestamps);
    const Region& surfaceDamage = input.getSurfaceDamage();
    const HdrMetadata& hdrMetadata = input.getHdrMetadata();

    if (acquireFence == nullptr) {
        BQ_LOGE("queueBuffer: fence is NULL");
        return BAD_VALUE;
    }

    auto acquireFenceTime = std::make_shared<FenceTime>(acquireFence);

    switch (scalingMode) {
        case NATIVE_WINDOW_SCALING_MODE_FREEZE:
        case NATIVE_WINDOW_SCALING_MODE_SCALE_TO_WINDOW:
        case NATIVE_WINDOW_SCALING_MODE_SCALE_CROP:
        case NATIVE_WINDOW_SCALING_MODE_NO_SCALE_CROP:
            break;
        default:
            BQ_LOGE("queueBuffer: unknown scaling mode %d", scalingMode);
            return BAD_VALUE;
    }

    sp<IConsumerListener> frameAvailableListener;
    sp<IConsumerListener> frameReplacedListener;
    sp<IProducerListener> droppedBufferListener;
    uint64_t droppedBufferId = 0;
    int callbackTicket = 0;
    uint64_t currentFrameNumber = 0;
    BufferItem item;
    bool picoConsumer = false;
    { // Autolock scope
        BufferQueueCore::MutexLock lock(mCore->mMutex);

        if (mCore->mIsAbandoned) {
            BQ_LOGE("queueBuffer: BufferQueue has been abandoned");
            return NO_INIT;
        }

        if (mCore->mConnectedApi == BufferQueueCore::NO_CONNECTED_API) {
            BQ_LOGE("queueBuffer: BufferQueue has no connected producer");
            return NO_INIT;
        }

        if (slot < 0 || slot >= BufferQueueDefs::NUM_BUFFER_SLOTS) {
            BQ_LOGE("queueBuffer: slot index %d out of range [0, %d)",
                    slot, BufferQueueDefs::NUM_BUFFER_SLOTS);
            return BAD_VALUE;
        } else if (!mSlots[slot].mBufferState.isDequeued()) {
            BQ_LOGE("queueBuffer: slot %d is not owned by the producer "
                    "(state = %s)", slot, mSlots[slot].mBufferState.string());
            return BAD_VALUE;
        } else if (!mSlots[slot].mRequestBufferCalled) {
            BQ_LOGE("queueBuffer: slot %d was queued without requesting "
                    "a buffer", slot);
            return BAD_VALUE;
        }

        // If shared buffer mode has just been enabled, cache the slot of the
        // first buffer that is queued and mark it as the shared buffer.
        if (mCore->mSharedBufferMode && mCore->mSharedBufferSlot ==
                BufferQueueCore::INVALID_BUFFER_SLOT) {
            mCore->mSharedBufferSlot = slot;
            mSlots[slot].mBufferState.mShared = true;
        }

        BQ_LOGV("queueBuffer: slot=%d/%" PRIu64 " time=%" PRIu64 " dataSpace=%d"
                " validHdrMetadataTypes=0x%x crop=[%d,%d,%d,%d] transform=%#x scale=%s",
                slot, mCore->mFrameCounter + 1, requestedPresentTimestamp, dataSpace,
                hdrMetadata.validTypes, crop.left, crop.top, crop.right, crop.bottom,
                transform,
                BufferItem::scalingModeName(static_cast<uint32_t>(scalingMode)));

        const sp<GraphicBuffer>& graphicBuffer(mSlots[slot].mGraphicBuffer);
        Rect bufferRect(graphicBuffer->getWidth(), graphicBuffer->getHeight());
        Rect croppedRect(Rect::EMPTY_RECT);
        crop.intersect(bufferRect, &croppedRect);
        if (croppedRect != crop) {
            BQ_LOGE("queueBuffer: crop rect is not contained within the "
                    "buffer in slot %d", slot);
            return BAD_VALUE;
        }

        // Override UNKNOWN dataspace with consumer default
        if (dataSpace == HAL_DATASPACE_UNKNOWN) {
            dataSpace = mCore->mDefaultBufferDataSpace;
        }

        mSlots[slot].mFence = acquireFence;
        mSlots[slot].mBufferState.queue();

        // Increment the frame counter and store a local version of it
        // for use outside the lock on mCore->mMutex.
        ++mCore->mFrameCounter;
        currentFrameNumber = mCore->mFrameCounter;
        mSlots[slot].mFrameNumber = currentFrameNumber;

        item.mAcquireCalled = mSlots[slot].mAcquireCalled;
        item.mGraphicBuffer = mSlots[slot].mGraphicBuffer;
        item.mCrop = crop;
        item.mTransform = transform &
                ~static_cast<uint32_t>(NATIVE_WINDOW_TRANSFORM_INVERSE_DISPLAY);
        item.mTransformToDisplayInverse =
                (transform & NATIVE_WINDOW_TRANSFORM_INVERSE_DISPLAY) != 0;
        item.mScalingMode = static_cast<uint32_t>(scalingMode);
        item.mTimestamp = requestedPresentTimestamp;
        item.mIsAutoTimestamp = isAutoTimestamp;
        item.mDataSpace = dataSpace;
        item.mHdrMetadata = hdrMetadata;
        item.mFrameNumber = currentFrameNumber;
        item.mSlot = slot;
        item.mFence = acquireFence;
        item.mFenceTime = acquireFenceTime;
        item.mIsDroppable = mCore->mAsyncMode ||
                (mConsumerIsSurfaceFlinger && mCore->mQueueBufferCanDrop) ||
                (mCore->mLegacyBufferDrop && mCore->mQueueBufferCanDrop) ||
                (mCore->mSharedBufferMode && mCore->mSharedBufferSlot == slot);
        item.mSurfaceDamage = surfaceDamage;
        item.mQueuedBuffer = true;
        item.mAutoRefresh = mCore->mSharedBufferMode && mCore->mAutoRefresh;
        item.mApi = mCore->mConnectedApi;

        mStickyTransform = stickyTransform;

        // Cache the shared buffer data so that the BufferItem can be recreated.
        if (mCore->mSharedBufferMode) {
            mCore->mSharedBufferCache.crop = crop;
            mCore->mSharedBufferCache.transform = transform;
            mCore->mSharedBufferCache.scalingMode = static_cast<uint32_t>(
                    scalingMode);
            mCore->mSharedBufferCache.dataspace = dataSpace;
        }

        output->bufferReplaced = false;
        if (mCore->mQueue.empty()) {
            // When the queue is empty, we can ignore mDequeueBufferCannotBlock
            // and simply queue this buffer
            mCore->mQueue.push_back(item);
            frameAvailableListener = mCore->mConsumerListener;
        } else {
            // When the queue is not empty, we need to look at the last buffer
            // in the queue to see if we need to replace it
            const BufferItem& last = mCore->mQueue.itemAt(
                    mCore->mQueue.size() - 1);
            if (last.mIsDroppable) {

                if (!last.mIsStale) {
                    mSlots[last.mSlot].mBufferState.freeQueued();

                    const sp<GraphicBuffer>& dropped = mSlots[last.mSlot].mGraphicBuffer;
                    if (dropped != nullptr &&
                            (dropped->getUsage() & 0xf00000000ULL) == 0x100000000ULL) {
                        droppedBufferId = dropped->getId();
                        droppedBufferListener = mCore->mConnectedProducerListener;
                        mCore->mActiveBuffers.erase(last.mSlot);
                        mCore->mFreeSlots.insert(last.mSlot);
                        mCore->clearBufferSlotLocked(last.mSlot);
                        output->bufferReplaced = true;
                    } else {
                        // After leaving shared buffer mode, the shared buffer will
                        // still be around. Mark it as no longer shared if this
                        // operation causes it to be free.
                        if (!mCore->mSharedBufferMode &&
                                mSlots[last.mSlot].mBufferState.isFree()) {
                            mSlots[last.mSlot].mBufferState.mShared = false;
                        }
                        // Don't put the shared buffer on the free list.
                        if (!mSlots[last.mSlot].mBufferState.isShared()) {
                            mCore->mActiveBuffers.erase(last.mSlot);
                            mCore->mFreeBuffers.push_back(last.mSlot);
                            output->bufferReplaced = true;
                        }
                    }
                }

                // Make sure to merge the damage rect from the frame we're about
                // to drop into the new frame's damage rect.
                if (last.mSurfaceDamage.bounds() == Rect::INVALID_RECT ||
                    item.mSurfaceDamage.bounds() == Rect::INVALID_RECT) {
                    item.mSurfaceDamage = Region::INVALID_REGION;
                } else {
                    item.mSurfaceDamage |= last.mSurfaceDamage;
                }

                // Overwrite the droppable buffer with the incoming one
                mCore->mQueue.editItemAt(mCore->mQueue.size() - 1) = item;
                frameReplacedListener = mCore->mConsumerListener;
            } else {
                mCore->mQueue.push_back(item);
                frameAvailableListener = mCore->mConsumerListener;
            }
        }

        mCore->mBufferHasBeenQueued = true;
        pthread_cond_broadcast(&mCore->mDequeueCondition);
        mCore->mLastQueuedSlot = slot;

        output->width = mCore->mDefaultWidth;
        output->height = mCore->mDefaultHeight;
        output->transformHint = mCore->mTransformHint;
        output->numPendingBuffers = static_cast<uint32_t>(mCore->mQueue.size());
        output->nextFrameNumber = mCore->mFrameCounter + 1;

        ATRACE_INT(mCore->mConsumerName.string(),
                static_cast<int32_t>(mCore->mQueue.size()));
        mCore->mOccupancyTracker.registerOccupancyChange(mCore->mQueue.size());

        // Take a ticket for the callback functions
        callbackTicket = mNextCallbackTicket++;
        picoConsumer = mCore->mHasPicoConsumer;

        VALIDATE_CONSISTENCY();
    } // Autolock scope

    // The factory reports the dropped PICO buffer right after the queue mutex is
    // released, before the GraphicBuffer clear below and before the callback-ticket
    // wait and the consumer frame callback, with neither mutex held. The payload is
    // NO_FENCE, not the old acquire fence.
    if (droppedBufferListener != nullptr) {
        droppedBufferListener->onBufferReleasedWithFence(Fence::NO_FENCE,
                                                         droppedBufferId, true);
    }

    // It is okay not to clear the GraphicBuffer when the consumer is SurfaceFlinger because
    // it is guaranteed that the BufferQueue is inside SurfaceFlinger's process and
    // there will be no Binder call
    if (!mConsumerIsSurfaceFlinger && !picoConsumer) {
        item.mGraphicBuffer.clear();
    }

    // Call back without the main BufferQueue lock held, but with the callback
    // lock held so we can ensure that callbacks occur in order

    int connectedApi;
    sp<Fence> lastQueuedFence;

    { // scope for the lock
        std::unique_lock<std::mutex> lock(mCallbackMutex);
        while (callbackTicket != mCurrentCallbackTicket) {
            mCallbackCondition.wait(lock);
        }

        if (frameAvailableListener != nullptr) {
            frameAvailableListener->onFrameAvailable(item);
        } else if (frameReplacedListener != nullptr) {
            frameReplacedListener->onFrameReplaced(item);
        }

        connectedApi = mCore->mConnectedApi;
        lastQueuedFence = std::move(mLastQueueBufferFence);

        mLastQueueBufferFence = std::move(acquireFence);
        mLastQueuedCrop = item.mCrop;
        mLastQueuedTransform = item.mTransform;

        ++mCurrentCallbackTicket;
        mCallbackCondition.notify_all();
    }

    // PICO keeps the reference through the listener callback, then
    // releases this local copy before frame timestamp processing.
    if (picoConsumer) {
        item.mGraphicBuffer.clear();
    }

    // Update and get FrameEventHistory.
    nsecs_t postedTime = systemTime(SYSTEM_TIME_MONOTONIC);
    NewFrameEventsEntry newFrameEventsEntry = {
        currentFrameNumber,
        postedTime,
        requestedPresentTimestamp,
        std::move(acquireFenceTime)
    };
    addAndGetFrameTimestamps(&newFrameEventsEntry,
            getFrameTimestamps ? &output->frameTimestamps : nullptr);

    // Wait without lock held
    if (connectedApi == NATIVE_WINDOW_API_EGL) {
        // Waiting here allows for two full buffers to be queued but not a
        // third. In the event that frames take varying time, this makes a
        // small trade-off in favor of latency rather than throughput.
        lastQueuedFence->waitForever("Throttling EGL Production");
    }

    mPicoUnfreezePending->store(false, std::memory_order_relaxed);
    return NO_ERROR;
}

status_t BufferQueueProducer::cancelBuffer(int slot, const sp<Fence>& fence) {
    ATRACE_CALL();
    BQ_LOGV("cancelBuffer: slot %d", slot);
    BufferQueueCore::MutexLock lock(mCore->mMutex);

    if (mCore->mIsAbandoned) {
        BQ_LOGE("cancelBuffer: BufferQueue has been abandoned");
        return NO_INIT;
    }

    if (mCore->mConnectedApi == BufferQueueCore::NO_CONNECTED_API) {
        BQ_LOGE("cancelBuffer: BufferQueue has no connected producer");
        return NO_INIT;
    }

    if (mCore->mSharedBufferMode) {
        BQ_LOGE("cancelBuffer: cannot cancel a buffer in shared buffer mode");
        return BAD_VALUE;
    }

    if (slot < 0 || slot >= BufferQueueDefs::NUM_BUFFER_SLOTS) {
        BQ_LOGE("cancelBuffer: slot index %d out of range [0, %d)",
                slot, BufferQueueDefs::NUM_BUFFER_SLOTS);
        return BAD_VALUE;
    } else if (!mSlots[slot].mBufferState.isDequeued()) {
        BQ_LOGE("cancelBuffer: slot %d is not owned by the producer "
                "(state = %s)", slot, mSlots[slot].mBufferState.string());
        return BAD_VALUE;
    } else if (fence == nullptr) {
        BQ_LOGE("cancelBuffer: fence is NULL");
        return BAD_VALUE;
    }

    mSlots[slot].mBufferState.cancel();

    // After leaving shared buffer mode, the shared buffer will still be around.
    // Mark it as no longer shared if this operation causes it to be free.
    if (!mCore->mSharedBufferMode && mSlots[slot].mBufferState.isFree()) {
        mSlots[slot].mBufferState.mShared = false;
    }

    // Don't put the shared buffer on the free list.
    if (!mSlots[slot].mBufferState.isShared()) {
        mCore->mActiveBuffers.erase(slot);
        mCore->mFreeBuffers.push_back(slot);
    }

    mSlots[slot].mFence = fence;
    pthread_cond_broadcast(&mCore->mDequeueCondition);
    VALIDATE_CONSISTENCY();

    return NO_ERROR;
}

int BufferQueueProducer::query(int what, int *outValue) {
    ATRACE_CALL();
    BufferQueueCore::MutexLock lock(mCore->mMutex);

    if (outValue == nullptr) {
        BQ_LOGE("query: outValue was NULL");
        return BAD_VALUE;
    }

    if (mCore->mIsAbandoned) {
        BQ_LOGE("query: BufferQueue has been abandoned");
        return NO_INIT;
    }

    int value;
    switch (what) {
        case 10000: // PICO QUERY_SET_PVR_STATUS: an input/output query.
            value = *outValue;
            mCore->mPicoVrStatus = value;
            break;
        case NATIVE_WINDOW_WIDTH:
            value = static_cast<int32_t>(mCore->mDefaultWidth);
            break;
        case NATIVE_WINDOW_HEIGHT:
            value = static_cast<int32_t>(mCore->mDefaultHeight);
            break;
        case NATIVE_WINDOW_FORMAT:
            value = static_cast<int32_t>(mCore->mDefaultBufferFormat);
            break;
        case NATIVE_WINDOW_LAYER_COUNT:
            // All BufferQueue buffers have a single layer.
            value = BQ_LAYER_COUNT;
            break;
        case NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS:
            value = mCore->getMinUndequeuedBufferCountLocked();
            break;
        case NATIVE_WINDOW_STICKY_TRANSFORM:
            value = static_cast<int32_t>(mStickyTransform);
            break;
        case NATIVE_WINDOW_CONSUMER_RUNNING_BEHIND:
            value = (mCore->mQueue.size() > 1);
            break;
        case NATIVE_WINDOW_CONSUMER_USAGE_BITS:
            // deprecated; higher 32 bits are truncated
            value = static_cast<int32_t>(mCore->mConsumerUsageBits);
            break;
        case NATIVE_WINDOW_DEFAULT_DATASPACE:
            value = static_cast<int32_t>(mCore->mDefaultBufferDataSpace);
            break;
        case NATIVE_WINDOW_BUFFER_AGE:
            if (mCore->mBufferAge > INT32_MAX) {
                value = 0;
            } else {
                value = static_cast<int32_t>(mCore->mBufferAge);
            }
            break;
        case NATIVE_WINDOW_CONSUMER_IS_PROTECTED:
            value = static_cast<int32_t>(mCore->mConsumerIsProtected);
            break;
        case NATIVE_WINDOW_MAX_BUFFER_COUNT:
            value = static_cast<int32_t>(mCore->mMaxBufferCount);
            break;
        default:
            return BAD_VALUE;
    }

    BQ_LOGV("query: %d? %d", what, value);
    *outValue = value;
    return NO_ERROR;
}

status_t BufferQueueProducer::connect(const sp<IProducerListener>& listener,
        int api, bool producerControlledByApp, QueueBufferOutput *output) {
    ATRACE_CALL();
    BufferQueueCore::MutexLock lock(mCore->mMutex);
    mConsumerName = mCore->mConsumerName;
    // Factory: logged for every producer whose consumer is not SurfaceFlinger, under the lock.
    if (!mConsumerIsSurfaceFlinger) {
        BQ_LOGI("connect: api %d, w*h = %d * %d", api, mCore->mDefaultWidth,
                mCore->mDefaultHeight);
    }
    BQ_LOGV("connect: api=%d producerControlledByApp=%s", api,
            producerControlledByApp ? "true" : "false");

    if (mCore->mIsAbandoned) {
        BQ_LOGE("connect: BufferQueue has been abandoned");
        return NO_INIT;
    }

    if (mCore->mConsumerListener == nullptr) {
        BQ_LOGE("connect: BufferQueue has no consumer");
        return NO_INIT;
    }

    if (output == nullptr) {
        BQ_LOGE("connect: output was NULL");
        return BAD_VALUE;
    }

    if (mCore->mConnectedApi != BufferQueueCore::NO_CONNECTED_API) {
        BQ_LOGE("connect: already connected (cur=%d req=%d)",
                mCore->mConnectedApi, api);
        return BAD_VALUE;
    }

    int delta = mCore->getMaxBufferCountLocked(mCore->mAsyncMode,
            mDequeueTimeout < 0 ?
            mCore->mConsumerControlledByApp && producerControlledByApp : false,
            mCore->mMaxBufferCount) -
            mCore->getMaxBufferCountLocked();
    if (!mCore->adjustAvailableSlotsLocked(delta)) {
        BQ_LOGE("connect: BufferQueue failed to adjust the number of available "
                "slots. Delta = %d", delta);
        return BAD_VALUE;
    }

    int status = NO_ERROR;
    switch (api) {
        case NATIVE_WINDOW_API_EGL:
        case NATIVE_WINDOW_API_CPU:
        case NATIVE_WINDOW_API_MEDIA:
        case NATIVE_WINDOW_API_CAMERA:
            mCore->mConnectedApi = api;

            output->width = mCore->mDefaultWidth;
            output->height = mCore->mDefaultHeight;
            output->transformHint = mCore->mTransformHint;
            output->numPendingBuffers =
                    static_cast<uint32_t>(mCore->mQueue.size());
            output->nextFrameNumber = mCore->mFrameCounter + 1;
            output->bufferReplaced = false;

            if (listener != nullptr) {
                // Set up a death notification so that we can disconnect
                // automatically if the remote producer dies
                if (IInterface::asBinder(listener)->remoteBinder() != nullptr) {
                    status = IInterface::asBinder(listener)->linkToDeath(
                            static_cast<IBinder::DeathRecipient*>(this));
                    if (status != NO_ERROR) {
                        BQ_LOGE("connect: linkToDeath failed: %s (%d)",
                                strerror(-status), status);
                    }
                    mCore->mLinkedToDeath = listener;
                }
                mCore->mConnectedProducerListener = listener;
                mCore->mBufferReleasedCbEnabled = listener->needsReleaseNotify();
            }
            break;
        default:
            BQ_LOGE("connect: unknown API %d", api);
            status = BAD_VALUE;
            break;
    }
    mCore->mConnectedPid = BufferQueueThreadState::getCallingPid();
    mCore->mBufferHasBeenQueued = false;
    mCore->mDequeueBufferCannotBlock = false;
    mCore->mQueueBufferCanDrop = false;
    mCore->mLegacyBufferDrop = true;
    if (mCore->mConsumerControlledByApp && producerControlledByApp) {
        mCore->mDequeueBufferCannotBlock = mDequeueTimeout < 0;
        mCore->mQueueBufferCanDrop = mDequeueTimeout <= 0;
    }

    mCore->mAllowAllocation = true;
    VALIDATE_CONSISTENCY();
    return status;
}

status_t BufferQueueProducer::disconnect(int api, DisconnectMode mode) {
    ATRACE_CALL();
    // Factory: logged for every producer whose consumer is not SurfaceFlinger, before the
    // queue mutex is taken.
    if (!mConsumerIsSurfaceFlinger) {
        BQ_LOGI("disconnect: api %d, w*h = %d * %d", api, mCore->mDefaultWidth,
                mCore->mDefaultHeight);
    }

    int status = NO_ERROR;
    sp<IConsumerListener> listener;
    { // Autolock scope
        BufferQueueCore::MutexLock lock(mCore->mMutex);

        if (mode == DisconnectMode::AllLocal) {
            if (BufferQueueThreadState::getCallingPid() != mCore->mConnectedPid) {
                return NO_ERROR;
            }
            api = BufferQueueCore::CURRENTLY_CONNECTED_API;
        }

        mCore->waitWhileAllocatingLocked(&mCore->mMutex);

        if (mCore->mIsAbandoned) {
            // It's not really an error to disconnect after the surface has
            // been abandoned; it should just be a no-op.
            return NO_ERROR;
        }

        if (api == BufferQueueCore::CURRENTLY_CONNECTED_API) {
            if (mCore->mConnectedApi == NATIVE_WINDOW_API_MEDIA) {
                ALOGD("About to force-disconnect API_MEDIA, mode=%d", mode);
            }
            api = mCore->mConnectedApi;
            // If we're asked to disconnect the currently connected api but
            // nobody is connected, it's not really an error.
            if (api == BufferQueueCore::NO_CONNECTED_API) {
                return NO_ERROR;
            }
        }

        switch (api) {
            case NATIVE_WINDOW_API_EGL:
            case NATIVE_WINDOW_API_CPU:
            case NATIVE_WINDOW_API_MEDIA:
            case NATIVE_WINDOW_API_CAMERA:
                if (mCore->mConnectedApi == api) {
                    // PICO (factory 0x79538..0x797dc): first drop the active slots that
                    // hold a cached-attach buffer (usage type 0x1 in bits 32-35), then the
                    // queued items whose slot still holds such a buffer.
                    for (auto it = mCore->mActiveBuffers.begin();
                            it != mCore->mActiveBuffers.end();) {
                        const int s = *it;
                        const sp<GraphicBuffer>& active = mSlots[s].mGraphicBuffer;
                        if (active != nullptr &&
                                (active->getUsage() & 0xf00000000ULL) == 0x100000000ULL) {
                            mCore->mFreeSlots.insert(s);
                            mCore->clearBufferSlotLocked(s);
                            it = mCore->mActiveBuffers.erase(it);
                        } else {
                            ++it;
                        }
                    }
                    for (auto it = mCore->mQueue.begin(); it != mCore->mQueue.end();) {
                        const sp<GraphicBuffer>& queued = mSlots[it->mSlot].mGraphicBuffer;
                        if (queued != nullptr &&
                                (queued->getUsage() & 0xf00000000ULL) == 0x100000000ULL) {
                            it = mCore->mQueue.erase(it);
                        } else {
                            ++it;
                        }
                    }
                    mCore->freeAllBuffersLocked();

                    // Remove our death notification callback if we have one
                    if (mCore->mLinkedToDeath != nullptr) {
                        sp<IBinder> token =
                                IInterface::asBinder(mCore->mLinkedToDeath);
                        // This can fail if we're here because of the death
                        // notification, but we just ignore it
                        token->unlinkToDeath(
                                static_cast<IBinder::DeathRecipient*>(this));
                    }
                    mCore->mSharedBufferSlot =
                            BufferQueueCore::INVALID_BUFFER_SLOT;
                    mCore->mLinkedToDeath = nullptr;
                    mCore->mConnectedProducerListener = nullptr;
                    mCore->mConnectedApi = BufferQueueCore::NO_CONNECTED_API;
                    mCore->mConnectedPid = -1;
                    mCore->mSidebandStream.clear();
                    pthread_cond_broadcast(&mCore->mDequeueCondition);
                    pthread_cond_broadcast(&mCore->mPicoFenceCondition);
                    clearAllCachedBuffersLocked();
                    listener = mCore->mConsumerListener;
                } else if (mCore->mConnectedApi == BufferQueueCore::NO_CONNECTED_API) {
                    BQ_LOGE("disconnect: not connected (req=%d)", api);
                    status = NO_INIT;
                } else {
                    BQ_LOGE("disconnect: still connected to another API "
                            "(cur=%d req=%d)", mCore->mConnectedApi, api);
                    status = BAD_VALUE;
                }
                break;
            default:
                BQ_LOGE("disconnect: unknown API %d", api);
                status = BAD_VALUE;
                break;
        }
    } // Autolock scope

    // Call back without lock held
    if (listener != nullptr) {
        listener->onBuffersReleased();
        listener->onDisconnect();
    }

    return status;
}

status_t BufferQueueProducer::setSidebandStream(const sp<NativeHandle>& stream) {
    sp<IConsumerListener> listener;
    { // Autolock scope
        BufferQueueCore::MutexLock _l(mCore->mMutex);
        mCore->mSidebandStream = stream;
        listener = mCore->mConsumerListener;
    } // Autolock scope

    if (listener != nullptr) {
        listener->onSidebandStreamChanged();
    }
    return NO_ERROR;
}

void BufferQueueProducer::allocateBuffers(uint32_t width, uint32_t height,
        PixelFormat format, uint64_t usage) {
    ATRACE_CALL();
    while (true) {
        size_t newBufferCount = 0;
        uint32_t allocWidth = 0;
        uint32_t allocHeight = 0;
        PixelFormat allocFormat = PIXEL_FORMAT_UNKNOWN;
        uint64_t allocUsage = 0;
        std::string allocName;
        { // Autolock scope
            BufferQueueCore::MutexLock lock(mCore->mMutex);
            mCore->waitWhileAllocatingLocked(&mCore->mMutex);

            if (!mCore->mAllowAllocation) {
                BQ_LOGE("allocateBuffers: allocation is not allowed for this "
                        "BufferQueue");
                return;
            }

            // Only allocate one buffer at a time to reduce risks of overlapping an allocation from
            // both allocateBuffers and dequeueBuffer.
            newBufferCount = mCore->mFreeSlots.empty() ? 0 : 1;
            if (newBufferCount == 0) {
                return;
            }

            allocWidth = width > 0 ? width : mCore->mDefaultWidth;
            allocHeight = height > 0 ? height : mCore->mDefaultHeight;
            allocFormat = format != 0 ? format : mCore->mDefaultBufferFormat;
            allocUsage = usage | mCore->mConsumerUsageBits;
            allocName.assign(mCore->mConsumerName.string(), mCore->mConsumerName.size());

            mCore->mIsAllocating = true;
        } // Autolock scope

        Vector<sp<GraphicBuffer>> buffers;
        for (size_t i = 0; i < newBufferCount; ++i) {
            sp<GraphicBuffer> graphicBuffer = new GraphicBuffer(
                    allocWidth, allocHeight, allocFormat, BQ_LAYER_COUNT,
                    allocUsage, allocName);

            status_t result = graphicBuffer->initCheck();

            if (result != NO_ERROR) {
                BQ_LOGE("allocateBuffers: failed to allocate buffer (%u x %u, format"
                        " %u, usage %#" PRIx64 ")", width, height, format, usage);
                BufferQueueCore::MutexLock lock(mCore->mMutex);
                mCore->mIsAllocating = false;
                pthread_cond_broadcast(&mCore->mIsAllocatingCondition);
                return;
            }
            buffers.push_back(graphicBuffer);
        }

        { // Autolock scope
            BufferQueueCore::MutexLock lock(mCore->mMutex);
            uint32_t checkWidth = width > 0 ? width : mCore->mDefaultWidth;
            uint32_t checkHeight = height > 0 ? height : mCore->mDefaultHeight;
            PixelFormat checkFormat = format != 0 ?
                    format : mCore->mDefaultBufferFormat;
            uint64_t checkUsage = usage | mCore->mConsumerUsageBits;
            if (checkWidth != allocWidth || checkHeight != allocHeight ||
                checkFormat != allocFormat || checkUsage != allocUsage) {
                // Something changed while we released the lock. Retry.
                BQ_LOGV("allocateBuffers: size/format/usage changed while allocating. Retrying.");
                mCore->mIsAllocating = false;
                pthread_cond_broadcast(&mCore->mIsAllocatingCondition);
                continue;
            }

            for (size_t i = 0; i < newBufferCount; ++i) {
                if (mCore->mFreeSlots.empty()) {
                    BQ_LOGV("allocateBuffers: a slot was occupied while "
                            "allocating. Dropping allocated buffer.");
                    continue;
                }
                auto slot = mCore->mFreeSlots.begin();
                mCore->clearBufferSlotLocked(*slot); // Clean up the slot first
                mSlots[*slot].mGraphicBuffer = buffers[i];
                mSlots[*slot].mFence = Fence::NO_FENCE;

                // freeBufferLocked puts this slot on the free slots list. Since
                // we then attached a buffer, move the slot to free buffer list.
                mCore->mFreeBuffers.push_front(*slot);

                BQ_LOGV("allocateBuffers: allocated a new buffer in slot %d",
                        *slot);

                // Make sure the erase is done after all uses of the slot
                // iterator since it will be invalid after this point.
                mCore->mFreeSlots.erase(slot);
            }

            mCore->mIsAllocating = false;
            pthread_cond_broadcast(&mCore->mIsAllocatingCondition);
            VALIDATE_CONSISTENCY();

            // If dequeue is waiting for to allocate a buffer, release the lock until it's not
            // waiting anymore so it can use the buffer we just allocated.
            while (mDequeueWaitingForAllocation) {
                pthread_cond_wait(&mDequeueWaitingForAllocationCondition, &mCore->mMutex);
            }
        } // Autolock scope
    }
}

status_t BufferQueueProducer::allowAllocation(bool allow) {
    ATRACE_CALL();
    BQ_LOGV("allowAllocation: %s", allow ? "true" : "false");

    BufferQueueCore::MutexLock lock(mCore->mMutex);
    mCore->mAllowAllocation = allow;
    return NO_ERROR;
}

status_t BufferQueueProducer::setGenerationNumber(uint32_t generationNumber) {
    ATRACE_CALL();
    BQ_LOGV("setGenerationNumber: %u", generationNumber);

    BufferQueueCore::MutexLock lock(mCore->mMutex);
    mCore->mGenerationNumber = generationNumber;
    return NO_ERROR;
}

String8 BufferQueueProducer::getConsumerName() const {
    ATRACE_CALL();
    BufferQueueCore::MutexLock lock(mCore->mMutex);
    BQ_LOGV("getConsumerName: %s", mConsumerName.string());
    return mConsumerName;
}

status_t BufferQueueProducer::setSharedBufferMode(bool sharedBufferMode) {
    ATRACE_CALL();
    BQ_LOGV("setSharedBufferMode: %d", sharedBufferMode);

    BufferQueueCore::MutexLock lock(mCore->mMutex);
    if (!sharedBufferMode) {
        mCore->mSharedBufferSlot = BufferQueueCore::INVALID_BUFFER_SLOT;
    }
    mCore->mSharedBufferMode = sharedBufferMode;
    return NO_ERROR;
}

status_t BufferQueueProducer::setAutoRefresh(bool autoRefresh) {
    ATRACE_CALL();
    BQ_LOGV("setAutoRefresh: %d", autoRefresh);

    BufferQueueCore::MutexLock lock(mCore->mMutex);

    mCore->mAutoRefresh = autoRefresh;
    return NO_ERROR;
}

status_t BufferQueueProducer::setDequeueTimeout(nsecs_t timeout) {
    ATRACE_CALL();
    BQ_LOGV("setDequeueTimeout: %" PRId64, timeout);

    BufferQueueCore::MutexLock lock(mCore->mMutex);
    bool dequeueBufferCannotBlock =
            timeout >= 0 ? false : mCore->mDequeueBufferCannotBlock;
    int delta = mCore->getMaxBufferCountLocked(mCore->mAsyncMode, dequeueBufferCannotBlock,
            mCore->mMaxBufferCount) - mCore->getMaxBufferCountLocked();
    if (!mCore->adjustAvailableSlotsLocked(delta)) {
        BQ_LOGE("setDequeueTimeout: BufferQueue failed to adjust the number of "
                "available slots. Delta = %d", delta);
        return BAD_VALUE;
    }

    mDequeueTimeout = timeout;
    mCore->mDequeueBufferCannotBlock = dequeueBufferCannotBlock;
    if (timeout > 0) {
        mCore->mQueueBufferCanDrop = false;
    }

    VALIDATE_CONSISTENCY();
    return NO_ERROR;
}

status_t BufferQueueProducer::setLegacyBufferDrop(bool drop) {
    ATRACE_CALL();
    BQ_LOGV("setLegacyBufferDrop: drop = %d", drop);

    BufferQueueCore::MutexLock lock(mCore->mMutex);
    mCore->mLegacyBufferDrop = drop;
    return NO_ERROR;
}

status_t BufferQueueProducer::getLastQueuedBuffer(sp<GraphicBuffer>* outBuffer,
        sp<Fence>* outFence, float outTransformMatrix[16]) {
    ATRACE_CALL();
    BQ_LOGV("getLastQueuedBuffer");

    BufferQueueCore::MutexLock lock(mCore->mMutex);
    if (mCore->mLastQueuedSlot == BufferItem::INVALID_BUFFER_SLOT) {
        *outBuffer = nullptr;
        *outFence = Fence::NO_FENCE;
        return NO_ERROR;
    }

    *outBuffer = mSlots[mCore->mLastQueuedSlot].mGraphicBuffer;
    *outFence = mLastQueueBufferFence;

    // Currently only SurfaceFlinger internally ever changes
    // GLConsumer's filtering mode, so we just use 'true' here as
    // this is slightly specialized for the current client of this API,
    // which does want filtering.
    GLConsumer::computeTransformMatrix(outTransformMatrix,
            mSlots[mCore->mLastQueuedSlot].mGraphicBuffer, mLastQueuedCrop,
            mLastQueuedTransform, true /* filter */);

    return NO_ERROR;
}

void BufferQueueProducer::getFrameTimestamps(FrameEventHistoryDelta* outDelta) {
    addAndGetFrameTimestamps(nullptr, outDelta);
}

void BufferQueueProducer::addAndGetFrameTimestamps(
        const NewFrameEventsEntry* newTimestamps,
        FrameEventHistoryDelta* outDelta) {
    if (newTimestamps == nullptr && outDelta == nullptr) {
        return;
    }

    ATRACE_CALL();
    BQ_LOGV("addAndGetFrameTimestamps");
    sp<IConsumerListener> listener;
    {
        BufferQueueCore::MutexLock lock(mCore->mMutex);
        listener = mCore->mConsumerListener;
    }
    if (listener != nullptr) {
        listener->addAndGetFrameTimestamps(newTimestamps, outDelta);
    }
}

void BufferQueueProducer::binderDied(const wp<android::IBinder>& /* who */) {
    // If we're here, it means that a producer we were connected to died.
    // We're guaranteed that we are still connected to it because we remove
    // this callback upon disconnect. It's therefore safe to read mConnectedApi
    // without synchronization here.
    int api = mCore->mConnectedApi;
    disconnect(api);
}

status_t BufferQueueProducer::getUniqueId(uint64_t* outId) const {
    BQ_LOGV("getUniqueId");

    *outId = mCore->mUniqueId;
    return NO_ERROR;
}

status_t BufferQueueProducer::getConsumerUsage(uint64_t* outUsage) const {
    BQ_LOGV("getConsumerUsage");

    BufferQueueCore::MutexLock lock(mCore->mMutex);
    *outUsage = mCore->mConsumerUsageBits;
    return NO_ERROR;
}

void BufferQueueProducer::listenFreezeSelf() {
    const auto pending = mPicoUnfreezePending;
    FreezeManager* manager = FreezeManager::getInstance();
    if (manager) {
        manager->registerSelfUnFreezeListener(this,
                [pending](const void* name) {
                    // Factory: the callback argument is the unfrozen name (a C string).
                    ALOGW("unFreeze Callback  %s", static_cast<const char*>(name));
                    pending->store(true, std::memory_order_relaxed);
                },
                nullptr, false);
    }
}

} // namespace android
