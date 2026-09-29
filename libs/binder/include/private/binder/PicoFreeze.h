// Copyright 2026 Picomisu contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <utils/Errors.h>
#include <cstdint>
namespace android {
// PICO 10: command is followed by an int32 PID despite its _IO size bits.
constexpr uint32_t PICO_BR_FROZEN_REPLY = 0x7212;
constexpr status_t PICO_FROZEN_TRANSACTION = UNKNOWN_ERROR + 9;
}
