// Copyright 2026 Picomisu contributors
// SPDX-License-Identifier: Apache-2.0
// Lifetime and locking of the factory-exact FreezeManager self registry.
// Factory behaviour: records are never freed, a repeated key keeps the first
// record, and callbacks run under the registry lock (so a callback must not
// register or unregister on the same thread; that deadlocks as on the factory).
#include <binder/FreezeManager.h>
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
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
TEST_F(FreezeRegistryTest, CallableIsRetainedAfterUnregisterAsOnFactory) {
    int key;
    auto owner = std::make_shared<int>(1);
    std::weak_ptr<int> weak = owner;
    manager->registerSelfUnFreezeListener(&key, [owner](const void*) {}, nullptr, false);
    owner.reset();
    manager->unRegisterSelfUnFreezeListener(&key);
    EXPECT_FALSE(weak.expired());
}
TEST_F(FreezeRegistryTest, RepeatedKeyKeepsFirstRecordAndRetainsSecond) {
    int key, first = 0, second = 0;
    auto secondOwner = std::make_shared<int>(2);
    std::weak_ptr<int> weakSecond = secondOwner;
    manager->registerSelfUnFreezeListener(&key, [&](const void*) { ++first; }, nullptr, false);
    manager->registerSelfUnFreezeListener(&key, [&, secondOwner](const void*) { ++second; },
                                          nullptr, false);
    secondOwner.reset();
    emit();
    EXPECT_EQ(1, first);
    EXPECT_EQ(0, second);
    EXPECT_FALSE(weakSecond.expired());
    manager->unRegisterSelfUnFreezeListener(&key);
    emit();
    EXPECT_EQ(1, first);
}
TEST_F(FreezeRegistryTest, ArgumentIsPassedAndEmptyCallableIsSkipped) {
    int key1, key2, value = 7;
    const void* seen = nullptr;
    manager->registerSelfUnFreezeListener(&key1, [&](const void* argument) { seen = argument; },
                                          &value, false);
    manager->registerSelfUnFreezeListener(&key2, {}, nullptr, false);
    emit();
    EXPECT_EQ(&value, seen);
    manager->unRegisterSelfUnFreezeListener(&key1);
    manager->unRegisterSelfUnFreezeListener(&key2);
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
