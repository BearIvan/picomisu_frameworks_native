/*
 * Copyright 2026 The Picomisu Project
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

// Exercise actual source/factory methods with local time and caller-ID replies.
// The OEM query is intercepted; no transaction is sent to a VR service.
#include <binder/IPCThreadState.h>
#include <gui/SurfaceClient.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <climits>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

using android::SurfaceClient;
static_assert(sizeof(SurfaceClient::Frame) == 168);
static_assert(offsetof(SurfaceClient::Frame, callingTid) == 128);
static_assert(offsetof(SurfaceClient::Frame, frameNumber) == 132);
static_assert(offsetof(SurfaceClient::Frame, timestamps) == 136);
static_assert(sizeof(SurfaceClient) == (sizeof(void*) == 8 ? 20256 : 20216));

static int queryStatus = 0;
static int32_t queryReply = 4321;
static int queryCalls = 0;
static int clockCalls = 0;
static nsecs_t timestamp = 0;
static bool queryShapeValid = true;
static int fixtures = 0;

extern "C" __attribute__((visibility("default"))) int ioctl(int fd, int request, ...) {
    va_list args;
    va_start(args, request);
    void* argument = va_arg(args, void*);
    va_end(args);
    if (static_cast<uint32_t>(request) != 0x8004621fU) {
        return static_cast<int>(syscall(SYS_ioctl, fd, request, argument));
    }
    ++queryCalls;
    auto* caller = static_cast<int32_t*>(argument);
    queryShapeValid &= fd >= 0 && caller && *caller == 0;
    if (caller) *caller = queryReply;
    if (queryStatus < 0) errno = EIO;
    return queryStatus;
}

extern "C" __attribute__((visibility("default"))) nsecs_t systemTime(int clock) {
    if (clock != SYSTEM_TIME_MONOTONIC) std::abort();
    ++clockCalls;
    return timestamp;
}

static void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "SurfaceClient fixture failed: %s\n", message);
        std::exit(1);
    }
}

struct ClientStorage {
    alignas(SurfaceClient) std::array<uint8_t, sizeof(SurfaceClient) + 64> bytes;
    SurfaceClient* client;
    ClientStorage() {
        bytes.fill(0xa5);
        client = new (bytes.data()) SurfaceClient();
    }
    ~ClientStorage() {
        client->~SurfaceClient();
        checkGuard();
    }
    void checkGuard() const {
        for (size_t i = sizeof(SurfaceClient); i < bytes.size(); ++i) {
            require(bytes[i] == 0xa5, "object guard");
        }
    }
};

static void expectFrame(ClientStorage& storage, SurfaceClient::Frame* frame, size_t slot,
                        const std::string& name, int frameNumber, pid_t caller,
                        const std::array<nsecs_t, 4>& times) {
    require(reinterpret_cast<uint8_t*>(frame) == storage.bytes.data() + slot * 168,
            "returned frame address");
    SurfaceClient::Frame expected{};
    std::strncpy(expected.name, name.c_str(), 127);
    expected.callingTid = caller;
    expected.frameNumber = frameNumber;
    for (size_t i = 0; i < 4; ++i) expected.timestamps[i] = times[i];
    require(std::memcmp(frame, &expected, 168) == 0, "frame bytes");
    storage.checkGuard();
}

static void printFrame(const std::string& label, const ClientStorage& storage,
                       const SurfaceClient::Frame* frame) {
    ++fixtures;
    const auto* bytes = reinterpret_cast<const uint8_t*>(frame);
    std::printf("surface-client %s slot=%zu record=", label.c_str(),
                static_cast<size_t>((bytes - storage.bytes.data()) / 168));
    for (size_t i = 0; i < 168; ++i) std::printf("%02x", bytes[i]);
    std::putchar('\n');
}

static void setStages(SurfaceClient* client, const std::array<nsecs_t, 4>& values) {
    for (int stage = 0; stage < 4; ++stage) {
        timestamp = values[stage];
        client->setFrameItem(stage);
    }
}

int main() {
    auto* ipc = android::IPCThreadState::self();
    const std::array<std::pair<int, int32_t>, 8> replies = {{{0, 0}, {0, 4321},
            {0, INT32_MIN}, {0, INT32_MAX}, {1, 123}, {-1, 987}, {-5, 987}, {0, -1}}};
    for (size_t i = 0; i < replies.size(); ++i) {
        queryStatus = replies[i].first;
        queryReply = replies[i].second;
        const auto result = ipc->getCallingTid();
        require(result == (queryStatus < 0 ? -1 : queryReply), "caller query result");
        require(queryShapeValid && queryCalls == static_cast<int>(i + 1), "caller query ABI");
        std::printf("calling-tid case=%zu status=%d result=%d request=8004621f initial=0 fd-valid=1\n",
                    i, queryStatus, result);
    }

    queryStatus = 0;
    queryReply = 4321;
    queryCalls = clockCalls = 0;
    {
        ClientStorage storage;
        auto* client = storage.client;
        uint64_t current;
        size_t count;
        pid_t caller;
        std::memcpy(&current, storage.bytes.data() + 20160, sizeof(current));
        std::memcpy(&count, storage.bytes.data() + 20168, sizeof(count));
        std::memcpy(&caller, storage.bytes.data() + (sizeof(void*) == 8 ? 20248 : 20212), sizeof(caller));
        require(current == UINT64_MAX && count == 0 && caller == 0, "constructor state");
        require(client->findCurrentFrame(0) == nullptr, "empty lookup");
        ++fixtures;
        std::puts("surface-client constructor index=ffffffffffffffff count=0 tid=0 empty=1");

        const std::array<nsecs_t, 4> boundaryTimes = {INT64_MIN, -1, 0, INT64_MAX};
        setStages(client, boundaryTimes);
        client->addFrame(android::String8(""), INT32_MIN);
        auto* frame = client->findCurrentFrame(INT32_MIN);
        expectFrame(storage, frame, 0, "", INT32_MIN, 4321, boundaryTimes);
        require(queryCalls == 1 && clockCalls == 4, "cached caller and stages");
        printFrame("stages", storage, frame);
        for (int stage : {-1, 4, INT32_MIN, INT32_MAX}) {
            timestamp = 999;
            client->setFrameItem(stage);
        }
        client->addFrame(android::String8("invalid-stages"), 1234);
        frame = client->findCurrentFrame(1234);
        expectFrame(storage, frame, 1, "invalid-stages", 1234, 4321, boundaryTimes);
        require(queryCalls == 1 && clockCalls == 8, "invalid stage still samples clock");
        printFrame("invalid-stages", storage, frame);

        const std::vector<std::string> names = {"", "s", std::string(126, 'r'),
                std::string(127, 'r'), std::string(128, 'r'), std::string(200, 'r'),
                std::string(160, '\xd0')};
        for (size_t i = 0; i < names.size(); ++i) {
            client->addFrame(android::String8(names[i].c_str()), 100 + i);
            frame = client->findCurrentFrame(100 + i);
            expectFrame(storage, frame, 2 + i, names[i], 100 + i, 4321, boundaryTimes);
            printFrame("name-" + std::to_string(i), storage, frame);
        }
        require(client->findCurrentFrame(INT32_MIN) == nullptr, "eight-frame lookup limit");

        queryReply = 8888;
        for (int i = 0; i < 130; ++i) {
            const std::array<nsecs_t, 4> times = {i * 10, i * 10 + 1, i * 10 + 2, i * 10 + 3};
            setStages(client, times);
            client->addFrame(android::String8("S"), i % 17);
            frame = client->findCurrentFrame(i % 17);
            expectFrame(storage, frame, (9 + i) % 120, "S", i % 17, 4321, times);
            if (i >= 7) {
                require(reinterpret_cast<uint8_t*>(client->findCurrentFrame((i - 7) % 17)) ==
                                storage.bytes.data() + ((9 + i - 7) % 120) * 168,
                        "oldest searched frame");
            }
            if (i >= 8) require(client->findCurrentFrame((i - 8) % 17) == nullptr,
                                "older stored frame is outside the search window");
            require(client->findCurrentFrame(-123456) == nullptr, "missing frame");
            if (i == 0 || i == 1 || i == 6 || i == 7 || i == 8 || i == 110 ||
                    i == 111 || i == 119 || i == 120 || i == 129) {
                printFrame("ring-" + std::to_string(i), storage, frame);
            }
        }
        require(queryCalls == 1 && clockCalls == 528, "caller cached across ring rollover");
    }

    queryCalls = clockCalls = 0;
    queryReply = 0;
    {
        ClientStorage storage;
        timestamp = 11;
        storage.client->setFrameItem(0);
        storage.client->setFrameItem(0);
        queryReply = 4321;
        for (int i = 1; i < 4; ++i) storage.client->setFrameItem(i);
        require(queryCalls == 3 && clockCalls == 5, "zero caller retried");
        storage.client->addFrame(android::String8("zero-tid"), -777);
        auto* frame = storage.client->findCurrentFrame(-777);
        expectFrame(storage, frame, 0, "zero-tid", -777, 4321, {11, 11, 11, 11});
        printFrame("zero-retry", storage, frame);
    }
    queryCalls = clockCalls = 0;
    queryStatus = -1;
    {
        ClientStorage storage;
        timestamp = 22;
        storage.client->setFrameItem(0);
        queryStatus = 0;
        for (int i = 1; i < 4; ++i) storage.client->setFrameItem(i);
        require(queryCalls == 1 && clockCalls == 4, "failed caller cached as minus one");
        storage.client->addFrame(android::String8("failed-tid"), -888);
        auto* frame = storage.client->findCurrentFrame(-888);
        expectFrame(storage, frame, 0, "failed-tid", -888, -1, {22, 22, 22, 22});
        printFrame("failure-cache", storage, frame);
    }
    require(queryShapeValid && fixtures == 22, "complete fixture set");
    std::printf("surface-client-probe passed=%d calling-tid=8\n", fixtures);
    return 0;
}
