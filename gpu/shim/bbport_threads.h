// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: helper thread sizing. Counts follow the hardware threads this process may run on
// (the affinity mask, so `taskset` can emulate a Steam Deck), and speculative helpers run as
// SCHED_IDLE: they use cores the game leaves idle and never take time from its threads.

#pragma once

#include <algorithm>
#include <thread>
#ifdef _WIN32
#include <bit>
#else
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

#ifdef _WIN32
// sacredfire (Windows): the three Win32 calls, declared here so <windows.h> stays out of the
// renderer's translation units.
extern "C" {
__declspec(dllimport) int __stdcall GetProcessAffinityMask(void* process, unsigned long long* process_mask, unsigned long long* system_mask);
__declspec(dllimport) void* __stdcall GetCurrentProcess(void);
__declspec(dllimport) void* __stdcall GetCurrentThread(void);
__declspec(dllimport) int __stdcall SetThreadPriority(void* thread, int priority);
}
#endif

namespace BbThreads {

/// Hardware threads available to the process.
inline unsigned Available() {
#ifdef _WIN32
    unsigned long long process = 0, system = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &process, &system) && process) {
        return std::max(1, std::popcount(process));
    }
#else
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        return std::max(1, CPU_COUNT(&set));
    }
#endif
    return std::max(1u, std::thread::hardware_concurrency());
}

/// The calling thread only runs on otherwise idle cores (falls back to the lowest nice level).
inline void MakeBackground() {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), -15 /* THREAD_PRIORITY_IDLE */);
#else
    sched_param param{};
    if (sched_setscheduler(0, SCHED_IDLE, &param) != 0) {
        setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), 19);
    }
#endif
}

} // namespace BbThreads
