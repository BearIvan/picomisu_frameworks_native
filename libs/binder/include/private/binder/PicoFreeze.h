// Copyright 2026 Picomisu contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <utils/Errors.h>
#include <cstdint>
namespace android {
// PICO 10: command is followed by an int32 PID despite its _IO size bits.
constexpr uint32_t PICO_BR_FROZEN_REPLY = 0x7212;
constexpr status_t PICO_FROZEN_TRANSACTION = UNKNOWN_ERROR + 9;
// Transaction flag asking the PICO driver to report a frozen target (kernel TF_REPORT_FROZEN).
constexpr uint32_t PICO_TF_REPORT_FROZEN = 0x80;
// PICO/Smartisan binder ioctls; the size bits are those of the factory libbinder requests.
constexpr unsigned long PICO_BINDER_GET_SERVER_PIDS = 0xc030620eUL;  // _IOWR('b', 14, 48 bytes)
constexpr unsigned long PICO_BINDER_GET_CLIENT_PIDS = 0xc030620fUL;  // _IOWR('b', 15, 48 bytes)
constexpr unsigned long PICO_BINDER_GET_TARGET_CALLEE_PID = 0x8004621eUL;  // _IOR('b', 30, __s32)
constexpr unsigned long PICO_BINDER_FREEZE_PID = 0x40046222UL;  // _IOW('b', 34, __s32)
}
