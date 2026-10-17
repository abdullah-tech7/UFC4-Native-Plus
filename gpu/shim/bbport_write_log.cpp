// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_clobber.h"
#include "bbport_write_log.h"
#include "bbport_free_check.h"
#include "common/signal_context.h"
#include "video_core/page_manager.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifndef _WIN32
#include <ucontext.h>
#endif
#include <unistd.h>
#include <x86intrin.h>

namespace BbWriteLog {
namespace {
struct Entry {
    std::uint64_t address, size, first, tsc;
    std::uint32_t source, tid;
};
constexpr std::size_t Size = 1 << 20;
std::array<Entry, Size> ring;
std::atomic<std::uint64_t> head{0};
// Writes that contained the suspicious qword (see Note), with the exact address.
std::array<Entry, 256> hits;
std::atomic<std::uint64_t> hits_head{0};
constexpr std::uint64_t Pattern = 0x0000005300000000ull;

void Push(std::array<Entry, Size>& r, std::atomic<std::uint64_t>& h, const Entry& e) {
    r[h.fetch_add(1, std::memory_order_relaxed) % r.size()] = e;
}
} // namespace

void Record(std::uint64_t address, const void* data, std::uint64_t size, Source source);

int Mode() {
    static const int mode = [] {
        const char* env = std::getenv("BB_WRITE_LOG");
        return env ? std::atoi(env) : 0;
    }();
    return mode;
}

bool Enabled() {
    return Mode() == 1;
}

void NoteIntent(std::uint64_t address, const void* data, std::uint64_t size, Source source) {
    if (Mode() == 2) {
        Record(address, data, size, source);
    }
}

void Note(std::uint64_t address, const void* data, std::uint64_t size, Source source) {
    if (Enabled()) {
        Record(address, data, size, source);
    }
}

void Record(std::uint64_t address, const void* data, std::uint64_t size, Source source) {
    static thread_local const std::uint32_t tid = static_cast<std::uint32_t>(gettid());
    Entry e{address, size, 0, __rdtsc(), source, tid};
    std::memcpy(&e.first, data, size < 8 ? size : 8);
    Push(ring, head, e);
    // Only small writes are scanned: scanning downloads of megabytes delays them enough to hide
    // the race this log is for.
    if (size > 64) {
        return;
    }
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::uint64_t at = (8 - (address & 7)) & 7; at + 8 <= size; at += 8) {
        std::uint64_t v;
        std::memcpy(&v, bytes + at, 8);
        if (v == Pattern) {
            Entry hit = e;
            hit.address = address + at;
            hit.first = v;
            hits[hits_head.fetch_add(1, std::memory_order_relaxed) % hits.size()] = hit;
        }
    }
}
void DumpRange(std::uint64_t address, std::uint64_t size) {
    if (Mode() == 0) {
        std::fprintf(stderr, "Write log: off (BB_WRITE_LOG=1 lists the writes into this range)\n");
        return;
    }
    const char* sources[] = {"backing",      "WriteData",           "fence",
                             "EOP (decoded)", "WriteData (decoded)", "EOS (decoded)", "DmaData"};
    const std::uint64_t now = __rdtsc(), n = head.load();
    int shown = 0;
    for (std::uint64_t i = n; i-- > (n > Size ? n - Size : 0) && shown < 64;) {
        const Entry& e = ring[i % Size];
        if (e.address < address + size && address < e.address + e.size) {
            std::fprintf(stderr,
                         "Write log: into range %s %#llx +%llu first %#llx tid %u, %.3f s before\n",
                         e.source < 7 ? sources[e.source] : "?", (unsigned long long)e.address,
                         (unsigned long long)e.size, (unsigned long long)e.first, e.tid,
                         double(now - e.tsc) / 3.0e9);
            ++shown;
        }
    }
    std::fprintf(stderr, "Write log: %d of %llu logged writes in %#llx +%llu\n", shown,
                 (unsigned long long)n, (unsigned long long)address, (unsigned long long)size);
}
} // namespace BbWriteLog

extern "C" void bbgpu_dump_guest_writes(void* ucontext) {
    VideoCore::PageManager::DumpWindowFault(ucontext);
    using namespace BbWriteLog;
    using Common::FaultRegister;
    const auto reg = [&](FaultRegister r) { return Common::GetFaultRegister(ucontext, r); };
    BbFreeCheck::DumpAtFault(reg(FaultRegister::Rax), reg(FaultRegister::R14));
    if (Mode() == 0) {
        return;
    }
    const std::uint64_t regs[] = {reg(FaultRegister::Rax), reg(FaultRegister::Rbx), reg(FaultRegister::Rcx), reg(FaultRegister::Rdx),
                                  reg(FaultRegister::Rsi), reg(FaultRegister::Rdi), reg(FaultRegister::R14), reg(FaultRegister::R15),
                                  reg(FaultRegister::R12), reg(FaultRegister::R13)};
    const char* names[] = {"rax", "rbx", "rcx", "rdx", "rsi", "rdi", "r14", "r15", "r12", "r13"};
    for (int i = 0; i < 10; ++i) {
        std::fprintf(stderr, "Write log: %s=%#llx\n", names[i], (unsigned long long)regs[i]);
    }
    const char* sources[] = {"backing",      "WriteData",           "fence",
                             "EOP (decoded)", "WriteData (decoded)", "EOS (decoded)", "DmaData"};
    const std::uint64_t now = __rdtsc();
    const auto print = [&](const Entry& e, const char* what) {
        std::fprintf(stderr,
                     "Write log: %s %s %#llx +%llu first %#llx tid %u, %.3f s before the fault\n",
                     what, e.source < 7 ? sources[e.source] : "?", (unsigned long long)e.address,
                     (unsigned long long)e.size, (unsigned long long)e.first, e.tid,
                     double(now - e.tsc) / 3.0e9);
    };
    const std::uint64_t nh = hits_head.load();
    for (std::uint64_t i = nh > hits.size() ? nh - hits.size() : 0; i < nh; ++i) {
        print(hits[i % hits.size()], "pattern");
    }
    // The block the guest read (rax) and all logged writes into it or its neighbours.
    const std::uint64_t block = regs[0];
    for (std::uint64_t at = block - 0x30; at < block + 0x60; at += 8) {
        std::uint64_t value = 0;
        std::memcpy(&value, reinterpret_cast<const void*>(at), 8);
        std::fprintf(stderr, "Write log: [%#llx] = %#llx\n", (unsigned long long)at,
                     (unsigned long long)value);
    }
    {
        const std::uint64_t n = head.load();
        int shown = 0;
        for (std::uint64_t i = n; i-- > (n > Size ? n - Size : 0) && shown < 200;) {
            const Entry& e = ring[i % Size];
            if (e.address + e.size > block - 0x30 && e.address < block + 0x60) {
                print(e, "in block");
                ++shown;
            }
        }
        shown = 0;
        for (std::uint64_t i = n; i-- > (n > Size ? n - Size : 0) && shown < 40;) {
            const Entry& e = ring[i % Size];
            if ((e.first & 0xffffffffull) == 0x53 || (e.first >> 32) == 0x53) {
                print(e, "value 0x53");
                ++shown;
            }
        }
    }
    // Writes that cover the chunk header the guest read (rax..rax+0x40) or the registers.
    const std::uint64_t n = head.load();
    int shown = 0;
    for (std::uint64_t i = n; i-- > (n > Size ? n - Size : 0) && shown < 64;) {
        const Entry& e = ring[i % Size];
        bool near = false;
        for (const auto r : regs) {
            near |= r + 0x1000 > e.address && r < e.address + e.size + 0x1000;
        }
        if (near) {
            print(e, "near");
            ++shown;
        }
    }
    // ufc3: writes whose data holds the value found in a register (the corrupted pointer), and
    // the remembered port writes (BbClobber ring) into the slot the game read it from.
    shown = 0;
    for (std::uint64_t i = n; i-- > (n > Size ? n - Size : 0) && shown < 32;) {
        const Entry& e = ring[i % Size];
        for (const auto r : regs) {
            if (r > 0x10000 && r < 0x100000000ull && ((e.first & 0xffffffffull) == r || (e.first >> 32) == r)) {
                print(e, "holds a register value");
                ++shown;
                break;
            }
        }
    }
    std::fprintf(stderr, "Write log: port writes (clobber ring) into [r14+0x18]:\n");
    BbClobber::DumpWritesInto(reg(FaultRegister::R14) + 0x18, 8);
    std::fprintf(stderr, "Write log: %llu writes logged\n", (unsigned long long)n);
}
