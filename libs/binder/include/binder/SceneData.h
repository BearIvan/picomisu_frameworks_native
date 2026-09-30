// Copyright 2026 Picomisu contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <binder/Parcel.h>
#include <utils/KeyedVector.h>
#include <utils/String16.h>
#include <stdint.h>
namespace android {
// Typed key/value scene report sent to the PICO "sceneinfo_service", reconstructed
// from the factory PICO OS 5.13.7 libbinder. The storage follows MetaDataBase with
// String16 keys plus a separate String16 value table.
class SceneData {
public:
    SceneData();
    SceneData(const SceneData& from);
    SceneData& operator=(const SceneData& rhs);
    virtual ~SceneData();

    void clear();
    bool remove(String16 key);
    bool setString(String16 key, String16 value);
    bool setStringData(String16 key, String16 value);
    bool setInt32(String16 key, int32_t value);
    bool setInt64(String16 key, int64_t value);
    bool setFloat(String16 key, float value);
    bool findInt32(String16 key, int32_t* value) const;
    bool findInt64(String16 key, int64_t* value) const;
    bool findFloat(String16 key, float* value) const;
    bool setData(String16 key, uint32_t type, const void* data, size_t size);
    bool findData(String16 key, uint32_t* type, const void** data, size_t* size) const;
    bool hasData(String16 key) const;
    status_t writeToParcel(Parcel& parcel);

    struct typed_data {
        typed_data();
        ~typed_data();
        typed_data(const typed_data&);
        typed_data& operator=(const typed_data&);
        void clear();
        void setData(uint32_t type, const void* data, size_t size);
        void getData(uint32_t* type, const void** data, size_t* size) const;
    private:
        uint32_t mType;
        size_t mSize;
        union {
            void* ext_data;
            float reservoir;
        } u;
        bool usesReservoir() const { return mSize <= sizeof(u.reservoir); }
        void* allocateStorage(size_t size);
        void freeStorage();
        void* storage() { return usesReservoir() ? &u.reservoir : u.ext_data; }
        const void* storage() const { return usesReservoir() ? &u.reservoir : u.ext_data; }
    };

private:
    // Type tags stored in every object (factory values).
    uint32_t mTypeInt32 = 1;
    uint32_t mTypeInt64 = 2;
    uint32_t mTypeFloat = 3;
    struct SceneDataInternal;
    SceneDataInternal* mInternalData;
};
} // namespace android
