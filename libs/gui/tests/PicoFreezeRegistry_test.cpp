// Copyright 2026 Picomisu contributors
// SPDX-License-Identifier: Apache-2.0
#include <binder/FreezeManager.h>
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <future>
#include <unistd.h>
#include "PicoFreezeRegistryFake.h"
using namespace android;
namespace { sp<picomisu::ServiceManager> fakeManager; }
namespace android { sp<IServiceManager> defaultServiceManager() { return fakeManager; } }
namespace {
class FreezeRegistryTest : public testing::Test {
protected:
    void SetUp() override {
        if (!fakeManager) fakeManager = new picomisu::ServiceManager;
        service = new picomisu::FreezeService;
        fakeManager->service = IInterface::asBinder(service);
        manager = FreezeManager::getInstance();
        ASSERT_EQ(fakeManager->service, IInterface::asBinder(manager->getService()));
    }
    void TearDown() override { service->alive = false; }
    void emit() { service->latest()->onUnFreeze(getpid()); }
    FreezeManager* manager;
    sp<picomisu::FreezeService> service;
};
TEST_F(FreezeRegistryTest, CallableOwnershipIsReleasedOnUnregister) {
    int key;
    auto owner = std::make_shared<int>(1);
    std::weak_ptr<int> weak = owner;
    manager->registerSelfUnFreezeListener(&key, [owner](const void*) {}, nullptr, false);
    owner.reset();
    EXPECT_FALSE(weak.expired());
    manager->unRegisterSelfUnFreezeListener(&key);
    EXPECT_TRUE(weak.expired());
}
TEST_F(FreezeRegistryTest, CallbackCanRemoveItselfWithoutLateInvocation) {
    int key, calls = 0;
    manager->registerSelfUnFreezeListener(&key, [&](const void*) {
        ++calls;
        manager->unRegisterSelfUnFreezeListener(&key);
    }, nullptr, false);
    emit(); emit();
    EXPECT_EQ(1, calls);
}
TEST_F(FreezeRegistryTest, RemovingAnotherRecordSkipsItInCurrentSnapshot) {
    int keys[2], first = 0, second = 0;
    manager->registerSelfUnFreezeListener(&keys[0], [&](const void*) {
        ++first;
        manager->unRegisterSelfUnFreezeListener(&keys[1]);
    }, nullptr, false);
    manager->registerSelfUnFreezeListener(&keys[1], [&](const void*) { ++second; }, nullptr, false);
    emit();
    EXPECT_EQ(1, first);
    EXPECT_EQ(0, second);
    manager->unRegisterSelfUnFreezeListener(&keys[0]);
}
TEST_F(FreezeRegistryTest, ConcurrentUnregisterWaitsForActiveCallback) {
    using namespace std::chrono_literals;
    int key;
    std::atomic<int> calls{0};
    std::promise<void> entered, allowed, removing;
    auto enteredFuture = entered.get_future();
    auto allowedFuture = allowed.get_future();
    auto removingFuture = removing.get_future();
    manager->registerSelfUnFreezeListener(&key, [&](const void*) {
        ++calls;
        entered.set_value();
        EXPECT_EQ(std::future_status::ready, allowedFuture.wait_for(5s));
    }, nullptr, false);
    auto callback = std::async(std::launch::async, [&] { emit(); });
    EXPECT_EQ(std::future_status::ready, enteredFuture.wait_for(5s));
    auto remove = std::async(std::launch::async, [&] {
        removing.set_value();
        manager->unRegisterSelfUnFreezeListener(&key);
    });
    EXPECT_EQ(std::future_status::ready, removingFuture.wait_for(5s));
    EXPECT_EQ(std::future_status::timeout, remove.wait_for(50ms));
    allowed.set_value();
    callback.get(); remove.get();
    emit();
    EXPECT_EQ(1, calls.load());
}
} // namespace
