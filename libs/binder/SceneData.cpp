// Copyright 2026 Picomisu contributors
// SPDX-License-Identifier: Apache-2.0
// Reconstructed from the factory PICO OS 5.13.7 libbinder (SceneData).
#define LOG_TAG "SceneData"
#include <binder/SceneData.h>
#include <utils/Log.h>
#include <stdlib.h>
#include <string.h>
namespace android {

struct SceneData::SceneDataInternal {
    KeyedVector<String16, typed_data> mItems;
    KeyedVector<String16, String16> mStrings;
};

SceneData::SceneData()
    : mInternalData(new SceneDataInternal()) {
}

SceneData::SceneData(const SceneData& from)
    : mInternalData(new SceneDataInternal()) {
    *mInternalData = *from.mInternalData;
}

SceneData& SceneData::operator=(const SceneData& rhs) {
    *mInternalData = *rhs.mInternalData;
    return *this;
}

SceneData::~SceneData() {
    clear();
    delete mInternalData;
    mInternalData = nullptr;
}

void SceneData::clear() {
    mInternalData->mItems.clear();
    mInternalData->mStrings.clear();
}

bool SceneData::remove(String16 key) {
    ssize_t i = mInternalData->mItems.indexOfKey(key);
    ssize_t j = mInternalData->mStrings.indexOfKey(key);
    if (i < 0 && j < 0) {
        return false;
    }
    if (i >= 0) {
        mInternalData->mItems.removeItemsAt(i);
    } else if (j >= 0) {
        mInternalData->mStrings.removeItemsAt(j);
    }
    return true;
}

bool SceneData::setString(String16 key, String16 value) {
    return setStringData(key, value);
}

// An existing key keeps its value (factory behaviour); true means it existed.
bool SceneData::setStringData(String16 key, String16 value) {
    if (mInternalData->mStrings.indexOfKey(key) >= 0) {
        return true;
    }
    mInternalData->mStrings.add(key, value);
    return false;
}

bool SceneData::setInt32(String16 key, int32_t value) {
    return setData(key, mTypeInt32, &value, sizeof(value));
}

bool SceneData::setData(String16 key, uint32_t type, const void* data, size_t size) {
    bool overwrote_existing = true;
    ssize_t i = mInternalData->mItems.indexOfKey(key);
    if (i < 0) {
        typed_data item;
        i = mInternalData->mItems.add(key, item);
        overwrote_existing = false;
    }
    typed_data& item = mInternalData->mItems.editValueAt(i);
    item.setData(type, data, size);
    return overwrote_existing;
}

bool SceneData::setInt64(String16 key, int64_t value) {
    return setData(key, mTypeInt64, &value, sizeof(value));
}

bool SceneData::setFloat(String16 key, float value) {
    return setData(key, mTypeFloat, &value, sizeof(value));
}

bool SceneData::findInt32(String16 key, int32_t* value) const {
    uint32_t type = 0;
    const void* data;
    size_t size;
    if (!findData(key, &type, &data, &size) || type != mTypeInt32) {
        return false;
    }
    *value = *static_cast<const int32_t*>(data);
    return true;
}

bool SceneData::findData(String16 key, uint32_t* type, const void** data, size_t* size) const {
    ssize_t i = mInternalData->mItems.indexOfKey(key);
    if (i < 0) {
        return false;
    }
    const typed_data& item = mInternalData->mItems.valueAt(i);
    item.getData(type, data, size);
    return true;
}

bool SceneData::findInt64(String16 key, int64_t* value) const {
    uint32_t type = 0;
    const void* data;
    size_t size;
    if (!findData(key, &type, &data, &size) || type != mTypeInt64) {
        return false;
    }
    *value = *static_cast<const int64_t*>(data);
    return true;
}

bool SceneData::findFloat(String16 key, float* value) const {
    uint32_t type = 0;
    const void* data;
    size_t size;
    if (!findData(key, &type, &data, &size) || type != mTypeFloat) {
        return false;
    }
    *value = *static_cast<const float*>(data);
    return true;
}

void SceneData::typed_data::setData(uint32_t type, const void* data, size_t size) {
    clear();
    mType = type;
    void* dst = allocateStorage(size);
    if (dst) {
        memcpy(dst, data, size);
    }
}

void SceneData::typed_data::getData(uint32_t* type, const void** data, size_t* size) const {
    *type = mType;
    *size = mSize;
    *data = storage();
}

bool SceneData::hasData(String16 key) const {
    ssize_t i = mInternalData->mItems.indexOfKey(key);
    ssize_t j = mInternalData->mStrings.indexOfKey(key);
    return i >= 0 || j >= 0;
}

SceneData::typed_data::typed_data()
    : mType(0),
      mSize(0) {
}

SceneData::typed_data::~typed_data() {
    clear();
}

void SceneData::typed_data::clear() {
    freeStorage();
    mType = 0;
}

SceneData::typed_data::typed_data(const typed_data& from)
    : mType(from.mType),
      mSize(0) {
    void* dst = allocateStorage(from.mSize);
    if (dst) {
        memcpy(dst, from.storage(), mSize);
    }
}

void* SceneData::typed_data::allocateStorage(size_t size) {
    mSize = size;
    if (usesReservoir()) {
        return &u.reservoir;
    }
    u.ext_data = malloc(mSize);
    if (u.ext_data == nullptr) {
        ALOGE("Couldn't allocate %zu bytes for item", size);
        mSize = 0;
    }
    return u.ext_data;
}

SceneData::typed_data& SceneData::typed_data::operator=(const typed_data& from) {
    if (this != &from) {
        clear();
        mType = from.mType;
        void* dst = allocateStorage(from.mSize);
        if (dst) {
            memcpy(dst, from.storage(), mSize);
        }
    }
    return *this;
}

void SceneData::typed_data::freeStorage() {
    if (!usesReservoir()) {
        if (u.ext_data) {
            free(u.ext_data);
        }
        u.ext_data = nullptr;
    }
    mSize = 0;
}

status_t SceneData::writeToParcel(Parcel& parcel) {
    size_t numItems = mInternalData->mItems.size();
    status_t ret = parcel.writeUint32(uint32_t(numItems));
    if (ret) {
        return ret;
    }
    for (size_t i = 0; i < numItems; i++) {
        String16 key = mInternalData->mItems.keyAt(i);
        const typed_data& item = mInternalData->mItems.valueAt(i);
        uint32_t type;
        const void* data;
        size_t size;
        item.getData(&type, &data, &size);
        ret = parcel.writeString16(key);
        if (ret) {
            return ret;
        }
        ret = parcel.writeUint32(type);
        if (ret) {
            return ret;
        }
        ret = parcel.writeByteArray(size, static_cast<const uint8_t*>(data));
        if (ret) {
            return ret;
        }
    }
    size_t numStrings = mInternalData->mStrings.size();
    ret = parcel.writeUint32(uint32_t(numStrings));
    if (ret) {
        return ret;
    }
    for (size_t i = 0; i < numStrings; i++) {
        String16 key = mInternalData->mStrings.keyAt(i);
        ret = parcel.writeString16(key);
        if (ret) {
            return ret;
        }
        ret = parcel.writeString16(mInternalData->mStrings.valueAt(i));
        if (ret) {
            return ret;
        }
    }
    return NO_ERROR;
}

} // namespace android
