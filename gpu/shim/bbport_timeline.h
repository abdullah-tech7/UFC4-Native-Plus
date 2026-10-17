// SPDX-License-Identifier: GPL-2.0-or-later
// bbport BB_TIMELINE=<file>: a timeline of the frame's way to the GPU (guest submissions and
// waits, decoding, Vulkan submissions, GPU completion), to see where the GPU waits for work.
// Recording starts BB_TIMELINE_START seconds (default 30) after the first event and stops when
// the buffer is full (~15 s of play); the file is then written once: "ns event arg arg2" per line.
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>

#include "../../src/platform/platform.h"

// SacredFire issue #2: every event also goes into a small ring that is always on (RecentRing),
// which the late-label guard dumps when it trips: the guest's submissions, frame ends, flips
// and waits, and the GPU thread's decoding, drains and idle signals around the late label.
namespace BbTimeline {

enum Event : std::uint32_t {
    GuestSubmit,       ///< arg: queue (0 graphics), arg2: submission number
    GuestSubmitDone,   ///< arg: frame
    GuestWaitBegin,    ///< the guest waits for the previous frame
    GuestWaitEnd,
    DecodeBegin,       ///< arg: queue, arg2: decoded submissions so far
    DecodeEnd,
    PipeTask,          ///< arg: 0 signals flush, 1 frame retired, 2 idle signal, 3 end of frame
    VulkanSubmit,      ///< arg: tick, arg2: 1 on the recording thread
    GpuDone,           ///< arg: tick the GPU has reached
    FrameRetired,      ///< arg: frame
    DecoderIdleBegin,  ///< the GPU command thread has nothing to decode
    DecoderIdleEnd,
    // SacredFire issue #2 (the recent ring):
    GuestSubmitGfx,      ///< arg: graphics submission seq (the clobber diagnostics' numbering), arg2: guest dcb
    GuestIdleWaitBegin,  ///< a submission or sceGnmSubmitDone waits for the GPU-idle lock
    GuestIdleWaitEnd,    ///< arg2: ns waited
    GuestLockSet,        ///< sceGnmSubmitDone: arg 1 when the lock was set (GPU not idle), 0 when idle
    GuestSubmitDoneStub, ///< sceGnmRequestFlipAndSubmitDone (stubbed: returns at once)
    GuestFlip,           ///< arg: 0 sceVideoOutSubmitFlip, 1 EOP flip registered, 2 EOP flip reached; arg2: buffer
    FlipDone,            ///< the frame presented, arg2: buffer
    GpuIdleSignal,       ///< the GPU thread signals GPU idle (arg: 1) or finds new work (0)
    DrainBegin,          ///< the GPU thread waits for the draw recording thread; arg: reason
    DrainEnd,            ///< arg2: ns waited
    GuardTrip,           ///< the late-label guard dropped a write; arg: packet seq, arg2: address
};

inline const char* EventName(std::uint32_t event) {
    switch (event) {
    case GuestSubmit: return "guest submit";
    case GuestSubmitDone: return "guest sceGnmSubmitDone";
    case GuestWaitBegin: return "guest waits for the previous frame";
    case GuestWaitEnd: return "guest wait ends";
    case DecodeBegin: return "decode begin";
    case DecodeEnd: return "decode end";
    case PipeTask: return "pipe task";
    case VulkanSubmit: return "vulkan submit";
    case GpuDone: return "gpu done";
    case FrameRetired: return "frame retired";
    case DecoderIdleBegin: return "decoder idle";
    case DecoderIdleEnd: return "decoder resumes";
    case GuestSubmitGfx: return "guest submit gfx seq";
    case GuestIdleWaitBegin: return "guest waits for GPU idle";
    case GuestIdleWaitEnd: return "guest idle wait ends";
    case GuestLockSet: return "guest sceGnmSubmitDone lock";
    case GuestSubmitDoneStub: return "guest sceGnmRequestFlipAndSubmitDone (stub)";
    case GuestFlip: return "guest flip";
    case FlipDone: return "flip presented";
    case GpuIdleSignal: return "gpu idle signal";
    case DrainBegin: return "drain begin";
    case DrainEnd: return "drain end";
    case GuardTrip: return "LATE LABEL GUARDED";
    default: return "?";
    }
}

struct Recent {
    std::int64_t ns;
    std::uint64_t arg, arg2;
    std::uint32_t event, tid;
};
inline constexpr std::uint64_t RecentCapacity = 8192;
inline std::array<Recent, RecentCapacity>& RecentRing() {
    static std::array<Recent, RecentCapacity> ring{};
    return ring;
}
inline std::atomic<std::uint64_t>& RecentNext() {
    static std::atomic<std::uint64_t> next{0};
    return next;
}

/// The recent events from `since_ns` on, oldest first, on `out` (the late-label guard).
inline void DumpRecent(std::int64_t since_ns, std::FILE* out) {
    auto& ring = RecentRing();
    const std::uint64_t head = RecentNext().load(std::memory_order_acquire);
    const std::uint64_t count = head < RecentCapacity ? head : RecentCapacity;
    unsigned printed = 0;
    for (std::uint64_t i = 0; i < count; ++i) {
        const Recent& e = ring[(head - count + i) % RecentCapacity];
        if (e.ns < since_ns || e.event == 0 && e.ns == 0) {
            continue;
        }
        std::fprintf(out, "    %+10.3f ms  tid %-6u %s %llu %llu\n", (e.ns - since_ns) / 1e6, e.tid,
                     EventName(e.event), (unsigned long long)e.arg, (unsigned long long)e.arg2);
        if (++printed >= 400) {
            std::fprintf(out, "    ... (more)\n");
            break;
        }
    }
    if (printed == 0) {
        std::fprintf(out, "    (no events remembered from that time)\n");
    }
}

struct Entry {
    std::uint64_t ns;
    std::uint32_t event;
    std::uint32_t arg;
    std::uint64_t arg2;
};

inline constexpr std::uint64_t Capacity = 400000;

struct State {
    const char* path = std::getenv("BB_TIMELINE");
    std::int64_t start_ns = [] {
        const char* env = std::getenv("BB_TIMELINE_START");
        return std::int64_t(env ? std::atof(env) * 1e9 : 30e9);
    }();
    std::atomic<std::int64_t> first_ns{0};
    std::atomic<std::uint64_t> next{0};
    std::atomic<bool> written{false};
    std::unique_ptr<Entry[]> entries = path ? std::make_unique<Entry[]>(Capacity) : nullptr;
};

inline State& Get() {
    static State state;
    return state;
}

inline std::int64_t Now() {
    return std::chrono::steady_clock::now().time_since_epoch().count();
}

inline void Write(State& s) {
    if (s.written.exchange(true)) {
        return;
    }
    if (FILE* f = std::fopen(s.path, "w")) {
        for (std::uint64_t i = 0; i < Capacity; ++i) {
            const auto& e = s.entries[i];
            std::fprintf(f, "%llu %u %u %llu\n", (unsigned long long)e.ns, e.event, e.arg,
                         (unsigned long long)e.arg2);
        }
        std::fclose(f);
        std::printf("Timeline: %llu events written to %s\n", (unsigned long long)Capacity, s.path);
    }
}

inline void Note(Event event, std::uint64_t arg = 0, std::uint64_t arg2 = 0) {
    auto& s = Get();
    const std::int64_t now = Now();
    {
        static thread_local const std::uint32_t tid = platform_thread_id();
        const std::uint64_t index = RecentNext().fetch_add(1, std::memory_order_acq_rel);
        RecentRing()[index % RecentCapacity] = {now, arg, arg2, std::uint32_t(event), tid};
    }
    if (!s.path) {
        return;
    }
    std::int64_t first = s.first_ns.load(std::memory_order_relaxed);
    if (first == 0) {
        s.first_ns.compare_exchange_strong(first, now);
        first = s.first_ns.load(std::memory_order_relaxed);
    }
    if (now - first < s.start_ns) {
        return;
    }
    const std::uint64_t index = s.next.fetch_add(1, std::memory_order_relaxed);
    if (index < Capacity) {
        s.entries[index] = {std::uint64_t(now), std::uint32_t(event), std::uint32_t(arg), arg2};
        if (index == Capacity - 1) {
            Write(s);
        }
    }
}

} // namespace BbTimeline
