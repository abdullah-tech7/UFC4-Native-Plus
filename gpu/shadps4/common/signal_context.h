// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Common {

void* GetXmmPointer(void* ctx, u8 index);

void* GetRip(void* ctx);

void IncrementRip(void* ctx, u64 length);

bool IsWriteError(void* ctx);

// bbport: general registers of the faulting thread, the same on every host (x86-64 only).
enum class FaultRegister : u8 { Rax, Rbx, Rcx, Rdx, Rsi, Rdi, Rbp, Rsp, R8, R9, R10, R11, R12, R13, R14, R15, Rip };
u64 GetFaultRegister(void* ctx, FaultRegister which);
/// bbport: sets or clears the trap flag (single step) in a fault context.
void SetTrapFlag(void* ctx, bool on);

} // namespace Common