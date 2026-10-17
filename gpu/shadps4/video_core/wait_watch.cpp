// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/wait_watch.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace VideoCore::WaitWatch {
namespace {

constexpr u64 ReportNs = 5'000'000'000ull;

struct Slot {
    std::atomic<u64> tid{0};
    const char* what = nullptr;
    const Holder* holder = nullptr;
    u64 site = 0;
    std::atomic<u64> since{0};
    std::atomic<bool> reported{false};
};
std::array<Slot, 64> slots;

u64 NowNs() {
    return u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
                   .count());
}

// Sites print as offsets into the module that holds this code (llvm-symbolizer on its debug exe).
u64 ModuleBase() {
    static const u64 base = [] {
#ifdef _WIN32
        return u64(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)));
#else
        Dl_info info{};
        return dladdr(reinterpret_cast<void*>(&NowNs), &info) ? u64(uintptr_t(info.dli_fbase))
                                                               : u64{0};
#endif
    }();
    return base;
}

u64 Offset(u64 site) {
    const u64 base = ModuleBase();
    return site >= base ? site - base : site;
}

void PrintWaits(u64 now) {
    for (const auto& slot : slots) {
        const u64 tid = slot.tid.load(std::memory_order_acquire);
        if (tid == 0) {
            continue;
        }
        const u64 since = slot.since.load(std::memory_order_acquire);
        std::fprintf(stderr, "  thread %llu waits for %s for %.1f s (at +%#llx)",
                     (unsigned long long)tid, slot.what ? slot.what : "?",
                     since && now > since ? (now - since) / 1e9 : 0.0,
                     (unsigned long long)Offset(slot.site));
        if (const Holder* h = slot.holder) {
            const u64 owner = h->tid.load(std::memory_order_relaxed);
            const u32 shared = h->shared.load(std::memory_order_relaxed);
            if (owner) {
                std::fprintf(stderr, "; held by thread %llu (taken at +%#llx)",
                             (unsigned long long)owner,
                             (unsigned long long)Offset(h->site.load(std::memory_order_relaxed)));
            }
            if (shared) {
                std::fprintf(stderr, "; %u shared holders (last: thread %llu at +%#llx)", shared,
                             (unsigned long long)h->shared_tid.load(std::memory_order_relaxed),
                             (unsigned long long)Offset(
                                 h->shared_site.load(std::memory_order_relaxed)));
            }
            if (!owner && !shared) {
                std::fprintf(stderr, "; no holder recorded");
            }
        }
        std::fprintf(stderr, "\n");
    }
}

void Watch() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const u64 now = NowNs();
        bool fresh = false;
        for (auto& slot : slots) {
            const u64 since = slot.since.load(std::memory_order_acquire);
            if (slot.tid.load(std::memory_order_acquire) != 0 && since != 0 &&
                now - since > ReportNs && !slot.reported.exchange(true)) {
                fresh = true;
            }
        }
        if (fresh) {
            std::fprintf(stderr, "Wait over 5 s (UFC-48; sites are offsets into the executable, "
                                 "module base %#llx); waits in progress:\n",
                         (unsigned long long)ModuleBase());
            PrintWaits(now);
            std::fflush(stderr);
        }
    }
}

} // namespace

u64 ThreadId() {
#ifdef _WIN32
    return GetCurrentThreadId();
#else
    return u64(syscall(SYS_gettid));
#endif
}

void Start() {
    static std::once_flag once;
    std::call_once(once, [] {
        ModuleBase();
        std::thread{Watch}.detach();
    });
}

int Begin(const char* what, const Holder* holder, u64 site) {
    const u64 tid = ThreadId();
    for (int i = 0; i < int(slots.size()); ++i) {
        u64 expected = 0;
        if (slots[i].tid.compare_exchange_strong(expected, tid, std::memory_order_acq_rel)) {
            auto& slot = slots[i];
            slot.what = what;
            slot.holder = holder;
            slot.site = site;
            slot.reported.store(false, std::memory_order_relaxed);
            slot.since.store(NowNs(), std::memory_order_release);
            return i;
        }
    }
    return -1;
}

void End(int index) {
    if (index < 0) {
        return;
    }
    auto& slot = slots[index];
    if (slot.reported.load(std::memory_order_acquire)) {
        const u64 since = slot.since.load(std::memory_order_relaxed);
        std::fprintf(stderr, "Wait over 5 s ended: thread %llu waited %.1f s for %s (UFC-48)\n",
                     (unsigned long long)slot.tid.load(std::memory_order_relaxed),
                     (NowNs() - since) / 1e9, slot.what ? slot.what : "?");
        std::fflush(stderr);
    }
    slot.since.store(0, std::memory_order_relaxed);
    slot.tid.store(0, std::memory_order_release);
}

} // namespace VideoCore::WaitWatch
