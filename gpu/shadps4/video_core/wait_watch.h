// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// ufc3 (UFC-48): a wait on one of the port's own locks or readbacks that lasts over 5 s is
// reported in the default log, with every other wait in progress and each lock's holder. A
// freeze left no line at all (Career's fighter editor on Windows, with write windows): a deadlock
// or a fault retried forever looked the same. Waits are noted only when they block (a try first),
// in a fixed table a watchdog thread scans once a second; async-signal safe (fault handlers).

#include <atomic>
#include <mutex>
#include <shared_mutex>
#include "common/types.h"

namespace VideoCore::WaitWatch {

/// The calling thread's id as the log prints it elsewhere (gettid / GetCurrentThreadId).
u64 ThreadId();

/// Who holds a watched lock (exclusive), and where it was taken (a return address).
struct Holder {
    const char* name;
    std::atomic<u64> tid{0}, site{0};
    std::atomic<u32> shared{0};
    std::atomic<u64> shared_tid{0}, shared_site{0};
    explicit Holder(const char* name_) : name{name_} {}
};

/// Starts the watchdog thread (once; not from a fault handler).
void Start();

/// A wait of the calling thread; `holder` may be null (a readback, a GPU wait).
int Begin(const char* what, const Holder* holder, u64 site);
void End(int slot);

struct Scope {
    int slot;
    explicit Scope(const char* what, const Holder* holder = nullptr)
        : slot{Begin(what, holder, u64(__builtin_return_address(0)))} {}
    ~Scope() {
        End(slot);
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

/// std::mutex whose blocking waits are watched.
class Mutex {
public:
    explicit Mutex(const char* name) : holder{name} {}
    __attribute__((noinline)) void lock() {
        const u64 site = u64(__builtin_return_address(0));
        if (!m.try_lock()) {
            const int slot = Begin(holder.name, &holder, site);
            m.lock();
            End(slot);
        }
        holder.tid.store(ThreadId(), std::memory_order_relaxed);
        holder.site.store(site, std::memory_order_relaxed);
    }
    bool try_lock() {
        if (!m.try_lock()) {
            return false;
        }
        holder.tid.store(ThreadId(), std::memory_order_relaxed);
        holder.site.store(u64(__builtin_return_address(0)), std::memory_order_relaxed);
        return true;
    }
    void unlock() {
        holder.tid.store(0, std::memory_order_relaxed);
        m.unlock();
    }

private:
    std::mutex m;
    Holder holder;
};

/// std::shared_mutex whose blocking waits are watched (exclusive holder; shared: a count and
/// the last thread to take it).
class SharedMutex {
public:
    explicit SharedMutex(const char* name) : holder{name} {}
    __attribute__((noinline)) void lock() {
        const u64 site = u64(__builtin_return_address(0));
        if (!m.try_lock()) {
            const int slot = Begin(holder.name, &holder, site);
            m.lock();
            End(slot);
        }
        holder.tid.store(ThreadId(), std::memory_order_relaxed);
        holder.site.store(site, std::memory_order_relaxed);
    }
    bool try_lock() {
        if (!m.try_lock()) {
            return false;
        }
        holder.tid.store(ThreadId(), std::memory_order_relaxed);
        return true;
    }
    void unlock() {
        holder.tid.store(0, std::memory_order_relaxed);
        m.unlock();
    }
    __attribute__((noinline)) void lock_shared() {
        const u64 site = u64(__builtin_return_address(0));
        if (!m.try_lock_shared()) {
            const int slot = Begin(holder.name, &holder, site);
            m.lock_shared();
            End(slot);
        }
        holder.shared.fetch_add(1, std::memory_order_relaxed);
        holder.shared_tid.store(ThreadId(), std::memory_order_relaxed);
        holder.shared_site.store(site, std::memory_order_relaxed);
    }
    bool try_lock_shared() {
        if (!m.try_lock_shared()) {
            return false;
        }
        holder.shared.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    void unlock_shared() {
        holder.shared.fetch_sub(1, std::memory_order_relaxed);
        m.unlock_shared();
    }

private:
    std::shared_mutex m;
    Holder holder;
};

} // namespace VideoCore::WaitWatch
