// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_clobber.h"
#include "common/signal_context.h"
#include "../../src/platform/platform.h"
#include "bbport_timeline.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#ifndef _WIN32
#include <sys/mman.h>
#endif
#ifndef _WIN32
#include <ucontext.h>
#endif
#include <unistd.h>

namespace BbClobber {
namespace {
constexpr std::size_t Page = 4096;

std::uint64_t NowNs() {
    return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count());
}

const char* SourceName(std::uint32_t source) {
    switch (source) {
    case Eop: return "EOP fence";
    case Eos: return "EOS fence";
    case EosGds: return "EOS GDS store";
    case ReleaseMem: return "RELEASE_MEM fence";
    case WriteData: return "WRITE_DATA";
    case ComputeWriteData: return "compute WRITE_DATA";
    case DmaFill: return "DMA fill";
    case DmaCopy: return "DMA copy";
    case BufferDownload: return "buffer download";
    case ImageDownload: return "image download";
    case ConstRam: return "DUMP_CONST_RAM";
    default: return "?";
    }
}

// Submitted command buffers (SubmitGfx order); the game reuses their memory every frame.
struct Range {
    std::uint64_t seq, base, bytes, ns;
};
std::array<Range, 256> ranges;
std::atomic<std::uint64_t> ranges_head{0};
std::atomic<std::uint64_t> decoding{0};

// The port's writes into guest memory.
struct Write {
    std::uint64_t address, value, ns, decoding;
    std::uint32_t bytes, source, tid, pad;
};
std::array<Write, 16384> writes;
std::atomic<std::uint64_t> writes_head{0};
std::atomic<std::uint64_t> hits{0};

// The port's last write per label address (direct-mapped by address): a slot that holds what
// the port last wrote there was not rebuilt by the game (GuardLabel).
struct LastWrite {
    std::atomic<std::uint64_t> address{0}, value{0}, ns{0};
};
std::array<LastWrite, 4096> last_writes;
LastWrite& LastWriteSlot(std::uint64_t address) {
    return last_writes[(address >> 2) % last_writes.size()];
}

// The last label GuardLabel dropped (ReportLongWait).
std::atomic<std::uint64_t> dropped_address{0}, dropped_value{0}, dropped_ns{0};

// BB_COPY_PROTECT: the read-only copies.
struct Protected {
    std::atomic<std::uint64_t> base{0}, bytes{0}, seq{0};
};
std::array<Protected, 64> protected_copies;

std::size_t RoundUp(std::size_t bytes) {
    return (bytes + Page - 1) & ~(Page - 1);
}
} // namespace

std::int64_t FirstBadHeader(const std::uint32_t* words, std::uint64_t dwords) {
    // ufc3: diagnostic for SacredFire issue #2 (a packet walk per submission, twice: ~1.4 % of
    // the GPU thread). Off unless BB_CLOBBER_CHECK=1.
    static const bool enabled = [] {
        const char* e = std::getenv("BB_CLOBBER_CHECK");
        return e && e[0] == '1';
    }();
    if (!enabled) {
        return -1;
    }
    std::uint64_t at = 0;
    while (at < dwords) {
        const std::uint32_t header = words[at];
        switch (header >> 30) {
        case 3:
            at += ((header >> 16) & 0x3fff) + 2;
            break;
        case 2:
            ++at;
            break;
        default:
            return std::int64_t(at);
        }
    }
    return -1;
}

void NoteSubmit(std::uint64_t seq, const void* guest_dcb, std::uint64_t bytes) {
    ranges[ranges_head.fetch_add(1, std::memory_order_relaxed) % ranges.size()] =
        Range{seq, reinterpret_cast<std::uint64_t>(guest_dcb), bytes, NowNs()};
    static const bool test = [] {
        const char* env = std::getenv("BB_ISSUE2_TEST");
        return env && env[0] == '1';
    }();
    if (test) {
        AskForReport();
    }
}

void NoteDecoding(std::uint64_t seq) {
    decoding.store(seq, std::memory_order_relaxed);
}

namespace {
void NoteLastWrite(std::uint64_t address, std::uint64_t value, std::uint64_t now) {
    LastWrite& last = LastWriteSlot(address);
    last.address.store(address, std::memory_order_relaxed);
    last.value.store(value, std::memory_order_relaxed);
    last.ns.store(now, std::memory_order_release);
}
bool IsFence(Source source) {
    return source == Eop || source == Eos || source == ReleaseMem;
}
} // namespace

void NoteWrite(std::uint64_t address, std::uint64_t value, std::uint32_t bytes, Source source) {
    if (bytes == 0 || address == 0) {
        return;
    }
    // ufc3 (UFC-47): the last write per address is what GuardLabel tells the port's own label
    // writes by, so it is kept whatever BB_CLOBBER_CHECK says. A fence label is noted by
    // GuardLabel itself once the write is let through: noted here, before the guard, it was the
    // write being judged, and a label the port bumps 2 -> 3 inside the buffer read as rebuilt.
    const std::uint64_t now = NowNs();
    if (!IsFence(source)) {
        NoteLastWrite(address, value, now);
    }
    // ufc3: the guest write ring and its reports are part of the issue-#2 diagnostics.
    static const bool enabled = [] {
        const char* e = std::getenv("BB_CLOBBER_CHECK");
        return e && e[0] == '1';
    }();
    if (!enabled) {
        return;
    }
    static thread_local const std::uint32_t tid = static_cast<std::uint32_t>(gettid());
    const std::uint64_t decoding_now = decoding.load(std::memory_order_relaxed);
    writes[writes_head.fetch_add(1, std::memory_order_relaxed) % writes.size()] =
        Write{address, value, now, decoding_now, bytes, source, tid, 0};
    // The newest submitted buffer the write lands in.
    const Range* hit = nullptr;
    const std::uint64_t head = ranges_head.load(std::memory_order_relaxed);
    const std::uint64_t count = std::min<std::uint64_t>(head, ranges.size());
    for (std::uint64_t i = 0; i < count; ++i) {
        const Range& r = ranges[(head - 1 - i) % ranges.size()];
        if (r.bytes != 0 && address < r.base + r.bytes && address + bytes > r.base &&
            (!hit || r.seq > hit->seq)) {
            hit = &r;
        }
    }
    if (!hit) {
        return;
    }
    const std::uint64_t n = hits.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n > 24 && n % 500 != 0) {
        return;
    }
    const char* where = hit->seq == decoding_now ? "the buffer being decoded"
                        : hit->seq > decoding_now
                            ? "a buffer submitted after the one being decoded, not decoded yet"
                            : "an older buffer, not resubmitted since";
    const bool late = hit->seq > decoding_now;
    char thread[32] = "?";
    platform_thread_get_name(thread, sizeof(thread));
    std::fprintf(stderr,
                 "Guest write check: %s of %u bytes, value %#llx, to %#llx lands inside submission "
                 "%llu's command buffer at +%#llx (%s; decoding submission %llu; submitted %.2f ms "
                 "ago; thread %s; hit %llu)\n",
                 SourceName(source), bytes, (unsigned long long)value, (unsigned long long)address,
                 (unsigned long long)hit->seq, (unsigned long long)(address - hit->base), where,
                 (unsigned long long)decoding_now, (now - hit->ns) / 1e6, thread,
                 (unsigned long long)n);
    if (late) {
        AskForReport();
    }
}

namespace {
// A plausible PM4 type-3 header: type 3, a known opcode range, the reserved bits clear (the
// predicate and shader-type bits may be set). A fence slot holds 0, 1, a counter or a clock:
// none of these look like one.
bool LooksLikeType3Header(std::uint32_t word) {
    const std::uint32_t opcode = (word >> 8) & 0xff;
    return (word >> 30) == 3 && opcode >= 0x10 && opcode <= 0xaf && (word & 0xfc) == 0;
}
} // namespace

bool GuardLabel(std::uint64_t address, std::uint64_t value, std::uint32_t bytes, Source source,
                std::uint64_t packet_seq, const std::uint32_t* at_submit) {
    if (address == 0 || (bytes != 4 && bytes != 8) || (address & 3) != 0) {
        return false;
    }
    // Inside a submitted command buffer? (The newest submission whose range holds it.)
    const Range* hit = nullptr;
    const std::uint64_t head = ranges_head.load(std::memory_order_relaxed);
    const std::uint64_t count = std::min<std::uint64_t>(head, ranges.size());
    for (std::uint64_t i = 0; i < count; ++i) {
        const Range& r = ranges[(head - 1 - i) % ranges.size()];
        if (r.bytes != 0 && address >= r.base && address + bytes <= r.base + r.bytes &&
            (!hit || r.seq > hit->seq)) {
            hit = &r;
        }
    }
    if (!hit) {
        return false;
    }
    std::uint32_t now_words[2] = {0, 0};
    std::memcpy(now_words, reinterpret_cast<const void*>(address), bytes);
    const std::uint32_t dwords = bytes / 4;
    bool rebuilt = false;
    char why[160];
    if (at_submit) {
        // Changed since the submission, by someone other than the port?
        if (std::memcmp(now_words, at_submit, bytes) != 0) {
            const LastWrite& last = LastWriteSlot(address);
            std::uint64_t ours = 0;
            const bool port_wrote =
                last.ns.load(std::memory_order_acquire) >= hit->ns &&
                last.address.load(std::memory_order_relaxed) == address &&
                (ours = last.value.load(std::memory_order_relaxed), true) &&
                std::memcmp(now_words, &ours, bytes) == 0;
            if (!port_wrote) {
                rebuilt = true;
                std::snprintf(why, sizeof(why), "the slot held %08x%s at submit and holds %08x%s now",
                              at_submit[0], dwords == 2 ? " .." : "", now_words[0],
                              dwords == 2 ? " .." : "");
            }
        }
    } else {
        const bool header = LooksLikeType3Header(now_words[0]) ||
                            (dwords == 2 && LooksLikeType3Header(now_words[1]));
        if (header) {
            rebuilt = true;
            std::snprintf(why, sizeof(why), "the slot now holds the packet header %08x",
                          LooksLikeType3Header(now_words[0]) ? now_words[0] : now_words[1]);
        }
    }
    if (!rebuilt) {
        // The write goes ahead: the slot now holds the port's value (the next label to it, a
        // counter's next step, is told from a rebuilt slot by this).
        NoteLastWrite(address, value, NowNs());
        return false;
    }
    dropped_address.store(address, std::memory_order_relaxed);
    dropped_value.store(value, std::memory_order_relaxed);
    dropped_ns.store(NowNs(), std::memory_order_relaxed);
    static std::atomic<std::uint64_t> guarded{0};
    const std::uint64_t n = guarded.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 16 || n % 100 == 0) {
        const std::uint64_t now = NowNs();
        char thread[32] = "?";
        platform_thread_get_name(thread, sizeof(thread));
        std::fprintf(stderr,
                     "Late label guarded: %s of %u bytes, value %#llx, to %#llx from submission %llu "
                     "lands at +%#llx of submission %llu's command buffer, which the game has rebuilt "
                     "(%s; submitted %.2f ms ago; decoding submission %llu; thread %s; guarded "
                     "%llu): the write is dropped\n",
                     SourceName(source), bytes, (unsigned long long)value,
                     (unsigned long long)address, (unsigned long long)packet_seq,
                     (unsigned long long)(address - hit->base), (unsigned long long)hit->seq, why,
                     (now - hit->ns) / 1e6,
                     (unsigned long long)decoding.load(std::memory_order_relaxed), thread,
                     (unsigned long long)n);
        void* frames[24];
        const int depth = platform_backtrace(frames, 24);
        for (int i = 1; i < depth; ++i) {
            char where[256];
            platform_describe_address(reinterpret_cast<uintptr_t>(frames[i]), where, sizeof(where));
            std::fprintf(stderr, "    #%d %s\n", i - 1, where);
        }
        if (n <= 4) {
            // What the guest and the GPU thread did from 50 ms before the buffer's submission:
            // its submissions and frame ends, flips, idle waits and the lock, the decoder's
            // progress, drains and idle signals, up to this write (times from that point).
            BbTimeline::Note(BbTimeline::GuardTrip, packet_seq, address);
            std::fprintf(stderr, "  events from 50 ms before submission %llu was submitted (tid %u "
                         "writes now):\n", (unsigned long long)hit->seq, platform_thread_id());
            BbTimeline::DumpRecent(std::int64_t(hit->ns) - 50'000'000, stderr);
        }
        std::fflush(stderr);
        AskForReport();
    }
    return true;
}

void ReportLongWait(std::uint64_t address, std::uint32_t function, std::uint32_t ref,
                    std::uint32_t mask, unsigned seconds) {
    static std::atomic<bool> reported{false};
    if (reported.exchange(true)) {
        return;
    }
    static const char* const functions[] = {"always", "<", "<=", "==", "!=", ">=", ">", "?"};
    const std::uint32_t now = *reinterpret_cast<const volatile std::uint32_t*>(address);
    std::fprintf(stderr,
                 "GPU wait not met for %u s: WAIT_REG_MEM on %#llx, which holds %#x, waits for "
                 "(value & %#x) %s %#x; decoding submission %llu; the game is frozen while this "
                 "lasts (UFC-47)\n",
                 seconds, (unsigned long long)address, now, mask, functions[function & 7], ref,
                 (unsigned long long)decoding.load(std::memory_order_relaxed));
    const std::uint64_t last = dropped_address.load(std::memory_order_relaxed);
    if (last != 0) {
        std::fprintf(stderr,
                     "  last label the late-label guard dropped: value %#llx to %#llx, %.2f s ago%s\n",
                     (unsigned long long)dropped_value.load(std::memory_order_relaxed),
                     (unsigned long long)last,
                     (NowNs() - dropped_ns.load(std::memory_order_relaxed)) / 1e9,
                     last == address ? " (the address this wait reads)" : "");
    } else {
        std::fprintf(stderr, "  the late-label guard has dropped no label\n");
    }
    std::fflush(stderr);
}

void DumpWritesInto(std::uint64_t address, std::uint64_t bytes) {
    const std::uint64_t now = NowNs();
    const std::uint64_t head = writes_head.load(std::memory_order_relaxed);
    const std::uint64_t count = std::min<std::uint64_t>(head, writes.size());
    unsigned listed = 0;
    for (std::uint64_t i = 0; i < count && listed < 8; ++i) {
        const Write& w = writes[(head - 1 - i) % writes.size()];
        if (w.address < address + bytes && w.address + w.bytes > address) {
            std::fprintf(stderr,
                         "  port write into it: %s of %u bytes, value %#llx, to %#llx, %.2f ms ago, "
                         "while decoding submission %llu (thread %u)\n",
                         SourceName(w.source), w.bytes, (unsigned long long)w.value,
                         (unsigned long long)w.address, (now - w.ns) / 1e6,
                         (unsigned long long)w.decoding, w.tid);
            ++listed;
        }
    }
    if (listed == 0) {
        std::fprintf(stderr, "  no remembered port write into it (%llu writes remembered)\n",
                     (unsigned long long)count);
    }
}

void AskForReport() {
    static std::atomic<bool> asked{false};
    if (asked.exchange(true)) {
        return;
    }
    std::fflush(stdout);
    std::fprintf(stderr,
                 "\nSacredFire issue #2: the known \"clobbered dword\" bug just happened (a dword of a "
                 "submitted command buffer turned into 1); the lines above are its diagnostics.\n"
                 "SacredFire issue #2: please attach this whole log to "
                 "https://github.com/T3CHNOLOG1C/SacredFire/issues/2 (launcher: Log page, \"Export "
                 "log...\") so that the writer can be found and fixed.\n\n");
    std::fflush(stderr);
}

bool ProtectCopies() {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_COPY_PROTECT");
        return env && env[0] == '1';
    }();
    return enabled;
}

void* AllocCopy(std::size_t bytes) {
    if (!ProtectCopies()) {
        return std::malloc(bytes);
    }
#ifdef _WIN32
    return bb_win_aligned_alloc(Page, RoundUp(bytes));
#else
    return std::aligned_alloc(Page, RoundUp(bytes));
#endif
}

void FreeCopy(void* storage, std::size_t bytes) {
    if (ProtectCopies()) {
        mprotect(storage, RoundUp(bytes), PROT_READ | PROT_WRITE);
        for (Protected& p : protected_copies) {
            if (p.base.load(std::memory_order_relaxed) == reinterpret_cast<std::uint64_t>(storage)) {
                p.base.store(0, std::memory_order_relaxed);
            }
        }
    }
#ifdef _WIN32
    if (ProtectCopies()) { bb_win_aligned_free(storage); return; }
#endif
    std::free(storage);
}

void ProtectCopy(std::uint64_t seq, const void* storage, std::size_t bytes) {
    if (!ProtectCopies() || bytes == 0) {
        return;
    }
    for (Protected& p : protected_copies) {
        std::uint64_t expected = 0;
        if (p.base.compare_exchange_strong(expected, reinterpret_cast<std::uint64_t>(storage),
                                           std::memory_order_relaxed)) {
            p.bytes.store(RoundUp(bytes), std::memory_order_relaxed);
            p.seq.store(seq, std::memory_order_relaxed);
            break;
        }
    }
    if (mprotect(const_cast<void*>(storage), RoundUp(bytes), PROT_READ) != 0) {
        static std::atomic<bool> once{false};
        if (!once.exchange(true)) {
            std::perror("BB_COPY_PROTECT: mprotect");
        }
    }
}

bool OnFault(void* ucontext, std::uint64_t address) {
    if (!ProtectCopies()) {
        return false;
    }
    for (const Protected& p : protected_copies) {
        const std::uint64_t base = p.base.load(std::memory_order_relaxed);
        const std::uint64_t bytes = p.bytes.load(std::memory_order_relaxed);
        if (base != 0 && address >= base && address < base + bytes) {
            const std::uint64_t rip = Common::GetFaultRegister(ucontext, Common::FaultRegister::Rip);
            char thread[32] = "?";
            pthread_getname_np(pthread_self(), thread, sizeof(thread));
            char line[256];
            const int n = std::snprintf(
                line, sizeof(line),
                "Clobber trap: a write to the read-only copy of submission %llu at +%#llx (host "
                "%#llx; copy of %llu bytes at %#llx) from RIP %#llx, thread %s\n",
                (unsigned long long)p.seq.load(std::memory_order_relaxed),
                (unsigned long long)(address - base), (unsigned long long)address,
                (unsigned long long)bytes, (unsigned long long)base,
                (unsigned long long)rip, thread);
            if (n > 0) {
                const ssize_t written = write(2, line, std::min<std::size_t>(n, sizeof(line) - 1));
                (void)written;
            }
            AskForReport();
            return true;
        }
    }
    return false;
}
} // namespace BbClobber
