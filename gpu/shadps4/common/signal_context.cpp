// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/arch.h"
#include "common/signal_context.h"

#ifdef _WIN32
#include <windows.h>
#elif defined(__FreeBSD__)
#include <machine/npx.h>
#include <sys/ucontext.h>
#else
#include <sys/ucontext.h>
#endif

namespace Common {

void* GetRip(void* ctx) {
#if defined(_WIN32)
    return (void*)((EXCEPTION_POINTERS*)ctx)->ContextRecord->Rip;
#elif defined(__APPLE__) && defined(ARCH_X86_64)
    return (void*)((ucontext_t*)ctx)->uc_mcontext->__ss.__rip;
#elif defined(__APPLE__) && defined(ARCH_ARM64)
    return (void*)((ucontext_t*)ctx)->uc_mcontext->__ss.__pc;
#elif defined(__FreeBSD__)
    return (void*)((ucontext_t*)ctx)->uc_mcontext.mc_rip;
#elif defined(ARCH_X86_64)
    return (void*)((ucontext_t*)ctx)->uc_mcontext.gregs[REG_RIP];
#else
#error "Unsupported architecture"
#endif
}

bool IsWriteError(void* ctx) {
#if defined(_WIN32)
    return ((EXCEPTION_POINTERS*)ctx)->ExceptionRecord->ExceptionInformation[0] == 1;
#elif defined(__APPLE__) && defined(ARCH_X86_64)
    return ((ucontext_t*)ctx)->uc_mcontext->__es.__err & 0x2;
#elif defined(__APPLE__) && defined(ARCH_ARM64)
    return ((ucontext_t*)ctx)->uc_mcontext->__es.__esr & 0x40;
#elif defined(__FreeBSD__) && defined(ARCH_X86_64)
    return ((ucontext_t*)ctx)->uc_mcontext.mc_err & 0x2;
#elif defined(ARCH_X86_64)
    return ((ucontext_t*)ctx)->uc_mcontext.gregs[REG_ERR] & 0x2;
#else
#error "Unsupported architecture"
#endif
}

u64 GetFaultRegister(void* ctx, FaultRegister which) {
#if defined(_WIN32)
    const CONTEXT* c = ((EXCEPTION_POINTERS*)ctx)->ContextRecord;
    switch (which) {
    case FaultRegister::Rax: return c->Rax; case FaultRegister::Rbx: return c->Rbx; case FaultRegister::Rcx: return c->Rcx;
    case FaultRegister::Rdx: return c->Rdx; case FaultRegister::Rsi: return c->Rsi; case FaultRegister::Rdi: return c->Rdi;
    case FaultRegister::Rbp: return c->Rbp; case FaultRegister::Rsp: return c->Rsp; case FaultRegister::R8: return c->R8;
    case FaultRegister::R9: return c->R9; case FaultRegister::R10: return c->R10; case FaultRegister::R11: return c->R11;
    case FaultRegister::R12: return c->R12; case FaultRegister::R13: return c->R13; case FaultRegister::R14: return c->R14;
    case FaultRegister::R15: return c->R15; case FaultRegister::Rip: return c->Rip;
    }
    return 0;
#elif defined(__linux__) && defined(ARCH_X86_64)
    static constexpr int index[] = {REG_RAX, REG_RBX, REG_RCX, REG_RDX, REG_RSI, REG_RDI, REG_RBP, REG_RSP,
                                    REG_R8, REG_R9, REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15, REG_RIP};
    return u64(((ucontext_t*)ctx)->uc_mcontext.gregs[index[static_cast<u8>(which)]]);
#else
    (void)ctx; (void)which;
    return 0;
#endif
}

void SetTrapFlag(void* ctx, bool on) {
    constexpr u64 TrapFlag = 0x100;
#if defined(_WIN32)
    auto& flags = ((EXCEPTION_POINTERS*)ctx)->ContextRecord->EFlags;
    flags = on ? (flags | DWORD(TrapFlag)) : (flags & ~DWORD(TrapFlag));
#elif defined(__linux__) && defined(ARCH_X86_64)
    auto& flags = ((ucontext_t*)ctx)->uc_mcontext.gregs[REG_EFL];
    flags = on ? (flags | greg_t(TrapFlag)) : (flags & ~greg_t(TrapFlag));
#else
    (void)ctx; (void)on;
#endif
}

} // namespace Common
