// Copyright 2026 Picomisu contributors
// SPDX-License-Identifier: Apache-2.0
#include <gui/BufferItem.h>
#include <system/window.h>

#include <cstdint>
#include <cstdio>
#include <limits>
#include <new>
#include <vector>

using namespace android;

int main() {
    // The examined factory ARM64 constructor writes through byte 215. Use
    // spare aligned storage so this probe does not rely on sizeof at allocation.
    for (int sample = 0; sample < 102; ++sample) {
        alignas(64) unsigned char storage[4096] = {};
        BufferItem* item = new (storage) BufferItem;
        if (sample) {
            item->mCrop = Rect(-sample, sample, sample + 2, sample + 3);
            item->mTransform = static_cast<uint32_t>(sample * 17);
            item->mScalingMode = NATIVE_WINDOW_SCALING_MODE_FREEZE;
            item->mTimestamp = std::numeric_limits<int64_t>::min() + sample;
            item->mIsAutoTimestamp = sample & 1;
            item->mDataSpace = static_cast<android_dataspace>(sample * 257);
            item->mFrameNumber = std::numeric_limits<uint64_t>::max() - sample;
            item->mSlot = sample - 50;
            item->mIsDroppable = sample & 2;
            item->mAcquireCalled = sample & 4;
            item->mTransformToDisplayInverse = sample & 8;
            item->mAutoRefresh = sample & 16;
            item->mQueuedBuffer = sample & 32;
            item->mIsStale = sample & 64;
            item->mApi = NATIVE_WINDOW_API_CPU;
            item->mSurfaceDamage = Region(Rect(0, 0, sample, sample + 1));
        }
        const auto& flattened = static_cast<const Flattenable<BufferItem>&>(*item);
        size_t length = flattened.getFlattenedSize();
        if (length > 4096 || flattened.getFdCount() != 0) return 2;
        std::vector<uint64_t> words((length + 7) / 8, 0);
        void* cursor = words.data();
        size_t remaining = length, fdCount = 0;
        int* fds = nullptr;
        status_t result = flattened.flatten(cursor, remaining, fds, fdCount);
        if (result != NO_ERROR) return 3;
        std::printf("wire %d %zu ", sample, length);
        const auto* bytes = reinterpret_cast<const unsigned char*>(words.data());
        for (size_t offset = 0; offset < length; ++offset) std::printf("%02x", bytes[offset]);
        std::printf("\n");
        item->~BufferItem();
    }
    return 0;
}
