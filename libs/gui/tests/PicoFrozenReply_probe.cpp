// Copyright 2026 Picomisu contributors
// SPDX-License-Identifier: Apache-2.0
// All BINDER_WRITE_READ exchanges are intercepted; no real Binder transactions.
#include <binder/IPCThreadState.h>
#include <binder/Parcel.h>
#include <private/binder/PicoFreeze.h>
#include <linux/android/binder.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <climits>
#include <vector>
using namespace android;
namespace {
std::vector<int32_t> responses;
int exchanges = 0;
}
extern "C" int ioctl(int fd, int request, ...) {
    va_list ap; va_start(ap, request); void* argument = va_arg(ap, void*); va_end(ap);
    if (static_cast<uint32_t>(request) != static_cast<uint32_t>(BINDER_WRITE_READ)) {
        return static_cast<int>(syscall(SYS_ioctl, fd, request, argument));
    }
    auto* transfer = static_cast<binder_write_read*>(argument);
    ++exchanges;
    transfer->write_consumed = transfer->write_size;
    transfer->read_consumed = 0;
    if (transfer->read_size) {
        if (responses.empty() || responses.size() * sizeof(int32_t) > transfer->read_size) {
            errno = EIO; return -1;
        }
        auto* destination = reinterpret_cast<void*>(static_cast<uintptr_t>(transfer->read_buffer));
        std::memcpy(destination, responses.data(), responses.size() * sizeof(int32_t));
        transfer->read_consumed = responses.size() * sizeof(int32_t);
        responses.clear();
    }
    return 0;
}
// Factory and source export this method; only its access qualifier is private.
extern "C" status_t selectedWait(IPCThreadState*, Parcel*, status_t*)
        asm("_ZN7android14IPCThreadState15waitForResponseEPNS_6ParcelEPi");
namespace {
bool check(bool ok, const char* label) {
    if (ok) printf("frozen-reply %s=1\n", label);
    else fprintf(stderr, "frozen-reply fixture failed: %s\n", label);
    return ok;
}
status_t wait(IPCThreadState* thread, std::initializer_list<int32_t> words,
              Parcel* reply = nullptr, status_t* acquire = nullptr) {
    responses.assign(words);
    const int before = exchanges;
    status_t result = selectedWait(thread, reply, acquire);
    if (exchanges <= before || !responses.empty()) return UNKNOWN_ERROR;
    return result;
}
}
int main() {
    auto* thread = IPCThreadState::self();
    if (!check(thread->getLastFrozenPid() == 0, "initial-pid-zero")) return 1;
    if (!check(wait(thread, {int32_t(PICO_BR_FROZEN_REPLY), 1234}) == PICO_FROZEN_TRANSACTION &&
               thread->getLastFrozenPid() == 1234, "frozen-pid-and-error")) return 1;
    Parcel reply;
    status_t acquire = NO_ERROR;
    if (!check(wait(thread, {int32_t(PICO_BR_FROZEN_REPLY), INT_MIN}, &reply, &acquire) == PICO_FROZEN_TRANSACTION &&
               thread->getLastFrozenPid() == INT_MIN && reply.errorCheck() == PICO_FROZEN_TRANSACTION &&
               acquire == PICO_FROZEN_TRANSACTION, "reply-acquire-and-signed-pid")) return 1;
    if (!check(wait(thread, {int32_t(PICO_BR_FROZEN_REPLY), INT_MAX}) == PICO_FROZEN_TRANSACTION &&
               thread->getLastFrozenPid() == INT_MAX, "pid-overwrite")) return 1;
    if (!check(wait(thread, {int32_t(BR_TRANSACTION_COMPLETE)}) == NO_ERROR &&
               thread->getLastFrozenPid() == INT_MAX, "success-retains-last-pid")) return 1;
    if (!check(wait(thread, {int32_t(BR_DEAD_REPLY)}) == DEAD_OBJECT &&
               thread->getLastFrozenPid() == INT_MAX, "dead-reply-retains-last-pid")) return 1;
    if (!check(wait(thread, {int32_t(BR_FAILED_REPLY)}) == FAILED_TRANSACTION &&
               thread->getLastFrozenPid() == INT_MAX, "failed-reply-retains-last-pid")) return 1;
    if (!check(wait(thread, {int32_t(PICO_BR_FROZEN_REPLY)}) == PICO_FROZEN_TRANSACTION &&
               thread->getLastFrozenPid() == 0, "legacy-short-pid-is-zero")) return 1;
    if (!check(wait(thread, {int32_t(PICO_BR_FROZEN_REPLY), 99}) == PICO_FROZEN_TRANSACTION &&
               thread->getLastFrozenPid() == 99, "non-identity-pid")) return 1;
    const int64_t token = thread->clearCallingIdentity();
    thread->restoreCallingIdentity(token);
    if (!check(thread->getLastFrozenPid() == 99, "identity-change-retains-frozen-pid")) return 1;
    puts("frozen-reply-probe passed=10");
    return 0;
}
