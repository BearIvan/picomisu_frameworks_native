// Copyright 2026 Picomisu contributors
// SPDX-License-Identifier: Apache-2.0
// Factory/Source libbinder parity fixture (chunk 6). The same binary runs against the
// factory libbinder and against the Source build; the printed lines must be equal.
// Observation only: BINDER_WRITE_READ and the PICO freeze ioctls are intercepted, the
// service manager is a local fake, and no process is frozen.
#include <binder/Binder.h>
#include <binder/IInterface.h>
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <binder/ISceneInfoManager.h>
#include <binder/Parcel.h>
#include <binder/ProcessState.h>
#include <binder/SceneData.h>
#include <binder/SceneInfoManager.h>
#include <private/binder/PicoFreeze.h>
#include <linux/android/binder.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
using namespace android;
namespace {
std::vector<int32_t> responses;
std::vector<uint32_t> transactionFlags;
int exchanges = 0;
unsigned long lastRequest = 0;
int32_t lastWords[12];
int nextResult = 0;
int nextErrno = 0;
bool interceptFreeze = false;

std::string hex(const void* data, size_t size) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; i++) {
        out += digits[bytes[i] >> 4];
        out += digits[bytes[i] & 15];
    }
    return out;
}

void recordCommands(const binder_write_read* transfer) {
    const auto* cursor = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(transfer->write_buffer));
    const uint8_t* end = cursor + transfer->write_size;
    while (cursor + sizeof(uint32_t) <= end) {
        uint32_t command;
        std::memcpy(&command, cursor, sizeof(command));
        cursor += sizeof(command);
        if (command == BC_TRANSACTION && cursor + sizeof(binder_transaction_data) <= end) {
            binder_transaction_data data;
            std::memcpy(&data, cursor, sizeof(data));
            transactionFlags.push_back(data.flags);
        }
        cursor += _IOC_SIZE(command);
    }
}
} // namespace

extern "C" int ioctl(int fd, int request, ...) {
    va_list ap; va_start(ap, request); void* argument = va_arg(ap, void*); va_end(ap);
    const unsigned long code = static_cast<uint32_t>(request);
    if (interceptFreeze && (code == PICO_BINDER_GET_SERVER_PIDS || code == PICO_BINDER_GET_CLIENT_PIDS ||
                            code == PICO_BINDER_GET_TARGET_CALLEE_PID || code == PICO_BINDER_FREEZE_PID)) {
        lastRequest = code;
        const size_t size = _IOC_SIZE(code) > 16 ? 48 : (code == PICO_BINDER_FREEZE_PID ? 16 : 12);
        std::memset(lastWords, 0, sizeof(lastWords));
        std::memcpy(lastWords, argument, size);
        if (code == PICO_BINDER_GET_SERVER_PIDS || code == PICO_BINDER_GET_CLIENT_PIDS) {
            auto* pids = static_cast<int32_t*>(argument);
            pids[0] = 101; pids[1] = 202; pids[10] = 2;
        } else if (code == PICO_BINDER_GET_TARGET_CALLEE_PID) {
            static_cast<int32_t*>(argument)[2] = 77;
        }
        if (nextErrno) { errno = nextErrno; return -1; }
        return nextResult;
    }
    if (code != static_cast<uint32_t>(BINDER_WRITE_READ)) {
        return static_cast<int>(syscall(SYS_ioctl, fd, request, argument));
    }
    auto* transfer = static_cast<binder_write_read*>(argument);
    ++exchanges;
    recordCommands(transfer);
    transfer->write_consumed = transfer->write_size;
    transfer->read_consumed = 0;
    if (transfer->read_size) {
        if (responses.empty() || responses.size() * sizeof(int32_t) > transfer->read_size) {
            errno = EIO; return -1;
        }
        std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(transfer->read_buffer)),
                    responses.data(), responses.size() * sizeof(int32_t));
        transfer->read_consumed = responses.size() * sizeof(int32_t);
        responses.clear();
    }
    return 0;
}

namespace {
class Recorder : public BBinder {
public:
    int calls = 0;
    int32_t replyValue = 1;
    std::string log;
    status_t onTransact(uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags) override {
        ++calls;
        log = "code=" + std::to_string(code) + " flags=" + std::to_string(flags) + " data=" +
              hex(data.data(), data.dataSize());
        reply->writeNoException();
        reply->writeInt32(replyValue);
        return NO_ERROR;
    }
};

class FakeServiceManager : public BnInterface<IServiceManager> {
public:
    sp<IBinder> service;
    mutable int gets = 0, checks = 0;
    mutable std::string names;
    sp<IBinder> getService(const String16& name) const override {
        ++gets; names += String8(name).string(); names += ";";
        return service;
    }
    sp<IBinder> checkService(const String16& name) const override {
        ++checks; names += String8(name).string(); names += ";";
        return service;
    }
    status_t addService(const String16&, const sp<IBinder>&, bool, int) override { return INVALID_OPERATION; }
    Vector<String16> listServices(int) override { return {}; }
};
sp<FakeServiceManager> fakeManager;
} // namespace
namespace android { sp<IServiceManager> defaultServiceManager() { return fakeManager; } }

namespace {
class ListenerInterface : public IInterface {
public:
    explicit ListenerInterface(IBinder* binder) : mBinder(binder) {}
protected:
    IBinder* onAsBinder() override { return mBinder; }
private:
    IBinder* mBinder;
};
// Local binder answering the IProducerListener descriptor like a BnProducerListener.
class FakeListener : public BBinder {
public:
    sp<IInterface> queryLocalInterface(const String16& descriptor) override {
        if (descriptor == String16("android.gui.IProducerListener")) return new ListenerInterface(this);
        return nullptr;
    }
};

void line(const std::string& text) { printf("binder-parity %s\n", text.c_str()); }

long binderMappingSize() {
    FILE* maps = fopen("/proc/self/maps", "re");
    if (!maps) return -1;
    char buffer[512];
    long size = 0;
    while (fgets(buffer, sizeof(buffer), maps)) {
        if (!strstr(buffer, "/dev/binder") && !strstr(buffer, "/dev/vndbinder")) continue;
        unsigned long start, end;
        if (sscanf(buffer, "%lx-%lx", &start, &end) == 2) size += static_cast<long>(end - start);
    }
    fclose(maps);
    return size;
}

void processVm(const char* label, int which) {
    fflush(stdout);
    pid_t child = fork();
    if (child == 0) {
        sp<ProcessState> state;
        if (which == 0) state = ProcessState::self();
        if (which == 1) state = ProcessState::selfForSystemServer();
        if (which == 2) state = ProcessState::selfForRuntime();
        if (which == 3) { ProcessState::self(); state = ProcessState::selfForSystemServer(); }
        printf("binder-parity process-vm %s pages=%ld\n", label, binderMappingSize() / getpagesize());
        fflush(stdout);
        _exit(0);
    }
    int status = 0;
    waitpid(child, &status, 0);
}

void sceneData() {
    SceneData data;
    std::string r;
    r += std::to_string(data.setInt32(String16("a"), 1));
    r += std::to_string(data.setInt64(String16("b"), 0x100000002LL));
    r += std::to_string(data.setFloat(String16("c"), 1.5f));
    r += std::to_string(data.setString(String16("s"), String16("x")));
    r += std::to_string(data.setString(String16("s"), String16("y")));
    r += std::to_string(data.setInt32(String16("a"), 3));
    r += std::to_string(data.setData(String16("blob"), 9, "0123456789", 10));
    int32_t i32 = 0; int64_t i64 = 0; float f = 0;
    r += std::to_string(data.findInt32(String16("a"), &i32)) + ":" + std::to_string(i32);
    r += std::to_string(data.findInt64(String16("a"), &i64));
    r += std::to_string(data.findInt64(String16("b"), &i64)) + ":" + std::to_string(i64);
    r += std::to_string(data.findFloat(String16("c"), &f)) + ":" + std::to_string(f);
    r += std::to_string(data.hasData(String16("s"))) + std::to_string(data.hasData(String16("zz")));
    uint32_t type = 0; const void* ptr = nullptr; size_t size = 0;
    r += std::to_string(data.findData(String16("blob"), &type, &ptr, &size)) + ":" +
         std::to_string(type) + ":" + std::to_string(size);
    line("scene-data ops=" + r);
    Parcel parcel;
    status_t status = data.writeToParcel(parcel);
    line("scene-data parcel status=" + std::to_string(status) + " bytes=" +
         hex(parcel.data(), parcel.dataSize()));
    SceneData copy(data);
    SceneData assigned;
    assigned.setInt32(String16("old"), 5);
    assigned = copy;
    r = std::to_string(copy.remove(String16("s"))) + std::to_string(copy.remove(String16("a"))) +
        std::to_string(copy.remove(String16("missing"))) + std::to_string(assigned.hasData(String16("old")));
    Parcel after;
    copy.writeToParcel(after);
    Parcel assignedParcel;
    assigned.writeToParcel(assignedParcel);
    line("scene-data copy removes=" + r + " copy=" + hex(after.data(), after.dataSize()) +
         " assigned=" + hex(assignedParcel.data(), assignedParcel.dataSize()));
    copy.clear();
    Parcel cleared;
    copy.writeToParcel(cleared);
    line("scene-data cleared=" + hex(cleared.data(), cleared.dataSize()));
}

void sceneProxy() {
    sp<Recorder> recorder = new Recorder;
    sp<ISceneInfoManager> proxy = interface_cast<ISceneInfoManager>(sp<IBinder>(recorder));
    SceneData data;
    data.setInt32(String16("k"), 42);
    data.setString(String16("t"), String16("v"));
    bool result = proxy->reportDataInfo(5, 6, data);
    line("scene-proxy descriptor=" + std::string(String8(ISceneInfoManager::descriptor).string()) +
         " result=" + std::to_string(result) + " " + recorder->log);
    recorder->replyValue = 0;
    line("scene-proxy zero-reply=" + std::to_string(proxy->reportDataInfo(1, 2, data)));
}

void sceneManager() {
    sp<Recorder> recorder = new Recorder;
    fakeManager->service = recorder;
    SceneInfoManager* manager = SceneInfoManager::getInstance();
    SceneData data;
    data.setInt64(String16("when"), 7);
    bool first = manager->reportDataInfo(7, 8, data);
    bool second = manager->reportDataInfo(9, 10, data);
    line("scene-manager singleton=" + std::to_string(manager == SceneInfoManager::getInstance()) +
         " results=" + std::to_string(first) + std::to_string(second) + " calls=" +
         std::to_string(recorder->calls) + " gets=" + std::to_string(fakeManager->gets) + " checks=" +
         std::to_string(fakeManager->checks) + " names=" + fakeManager->names + " last=" + recorder->log);
    fakeManager->service.clear();
    fakeManager->names.clear();
    SceneInfoManager unavailable;
    sp<ISceneInfoManager> service = unavailable.getService();
    line("scene-manager unavailable service=" + std::to_string(service == nullptr) + " report=" +
         std::to_string(unavailable.reportDataInfo(1, 1, data)) + " names=" + fakeManager->names);
}

void flatten() {
    sp<IBinder> listener = new FakeListener;
    sp<IBinder> plain = new BBinder;
    for (auto& entry : {std::make_pair("listener", listener), std::make_pair("plain", plain)}) {
        Parcel parcel;
        parcel.writeStrongBinder(entry.second);
        flat_binder_object object;
        std::memcpy(&object, parcel.data(), sizeof(object));
        char text[96];
        snprintf(text, sizeof(text), "flatten %s type=%x flags=%x", entry.first, object.hdr.type, object.flags);
        line(text);
    }
}

void string16Inplace() {
    for (bool terminated : {true, false}) {
        Parcel parcel;
        parcel.writeInt32(3);
        char16_t chars[4] = {u'a', u'b', u'c', terminated ? u'\0' : u'd'};
        parcel.write(chars, sizeof(chars));
        parcel.setDataPosition(0);
        size_t length = 99;
        const char16_t* text = parcel.readString16Inplace(&length);
        line(std::string("string16-inplace terminated=") + (terminated ? "1" : "0") + " result=" +
             std::to_string(text != nullptr) + " length=" + std::to_string(length));
    }
}

void frozenTransact() {
    sp<IBinder> proxy = ProcessState::self()->getStrongProxyForHandle(4242);
    struct Step { const char* label; uint32_t flags; std::vector<int32_t> words; };
    const Step steps[] = {
        {"frozen-plain", 0, {int32_t(PICO_BR_FROZEN_REPLY), 555}},
        {"frozen-requested", PICO_TF_REPORT_FROZEN, {int32_t(PICO_BR_FROZEN_REPLY), 556}},
        {"frozen-oneway", IBinder::FLAG_ONEWAY, {int32_t(BR_TRANSACTION_COMPLETE)}},
        {"dead", 0, {int32_t(BR_DEAD_REPLY)}},
        {"after-dead", 0, {}},
    };
    for (const auto& step : steps) {
        responses = step.words;
        transactionFlags.clear();
        const int before = exchanges;
        Parcel data, reply;
        data.writeInt32(1);
        status_t status = proxy->transact(IBinder::FIRST_CALL_TRANSACTION, data, &reply, step.flags);
        std::string flags;
        for (uint32_t value : transactionFlags) flags += std::to_string(value) + ",";
        char text[160];
        snprintf(text, sizeof(text), "transact %s status=%d exchanged=%d written-flags=%s last-frozen=%d",
                 step.label, status, exchanges > before, flags.c_str(),
                 IPCThreadState::self()->getLastFrozenPid());
        line(text);
        responses.clear();
    }
}

void freezeIoctls() {
    interceptFreeze = true;
    auto* thread = IPCThreadState::self();
    binder_remote_pids pids;
    std::memset(&pids, 0, sizeof(pids));
    pids.pid = 1234;
    nextResult = 0; nextErrno = 0;
    status_t server = thread->getBinderServerPids(&pids);
    char text[200];
    snprintf(text, sizeof(text), "pids server=%d request=%lx in-pid=%d out=%d,%d count=%d",
             server, lastRequest, lastWords[11], pids.pids[0], pids.pids[1], pids.count);
    line(text);
    lastRequest = 0;
    status_t clientNull = thread->getBinderClientPids(nullptr);
    nextResult = 3;
    status_t client = thread->getBinderClientPids(&pids);
    snprintf(text, sizeof(text), "pids client-null=%d touched=%lx client=%d request=%lx",
             clientNull, 0UL, client, lastRequest);
    line(text);
    nextResult = 0;
    pid_t callee = thread->getTargetCalleePid(11, 22);
    snprintf(text, sizeof(text), "callee result=%d request=%lx words=%d,%d,%d", callee, lastRequest,
             lastWords[0], lastWords[1], lastWords[2]);
    line(text);
    nextErrno = EINVAL;
    pid_t calleeError = thread->getTargetCalleePid(1, 2);
    status_t freezeError = thread->setPidFreeze(33, true, 2);
    snprintf(text, sizeof(text), "freeze error callee=%d freeze=%d request=%lx words=%d,%d,%d,%d",
             calleeError, freezeError, lastRequest, lastWords[0], lastWords[1], lastWords[2], lastWords[3]);
    line(text);
    nextErrno = 0; nextResult = 5;
    status_t freezeOk = thread->setPidFreeze(-10000, false, 0);
    snprintf(text, sizeof(text), "freeze ok=%d words=%d,%d,%d,%d", freezeOk, lastWords[0],
             lastWords[1], lastWords[2], lastWords[3]);
    line(text);
    interceptFreeze = false;
}
} // namespace

int main() {
    processVm("self", 0);
    processVm("system-server", 1);
    processVm("runtime", 2);
    processVm("self-then-system-server", 3);
    fakeManager = new FakeServiceManager;
    sceneData();
    sceneProxy();
    sceneManager();
    string16Inplace();
    flatten();
    frozenTransact();
    freezeIoctls();
    puts("binder-parity-probe completed=1");
    return 0;
}
