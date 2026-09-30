// Copyright 2026 Picomisu contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <binder/IInterface.h>
#include <binder/SceneData.h>
namespace android {
// Native client interface of the PICO "sceneinfo_service" (android.app.ISceneInfoManager).
// The factory libbinder has only the proxy; the service is implemented in Java.
class ISceneInfoManager : public IInterface {
public:
    DECLARE_META_INTERFACE(SceneInfoManager)
    virtual bool reportDataInfo(int type, int subType, SceneData data) = 0;
    enum { REPORT_DATA_INFO = IBinder::FIRST_CALL_TRANSACTION };
};
} // namespace android
