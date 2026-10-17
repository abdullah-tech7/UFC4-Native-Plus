/* SacredFire: Windows x86-64 implementation of platform.h (llvm-mingw, UCRT, winpthreads).
 * Mechanisms adapted from Supermedo/bloodborne_pc at ec8340a9 (GPL-2.0-or-later: src/compat_win.c,
 * src/runtime_memory.c, src/runtime_thread.c, src/probe.c), reworked for the current runtime:
 *   - guest TCB in a TEB TLS slot (gs:[0x1480 + 8*slot]), the loads patched after a Zydis decode;
 *   - guest memory as a pagefile-backed section, the PS4 user range reserved as a placeholder and
 *     replaced view by view (VirtualAlloc2, MapViewOfFile3, UnmapViewOfFile2);
 *   - a vectored exception handler with bb_setjmp/bb_longjmp recovery, a fatal-fault report and a
 *     watchdog that suspends every thread and prints its frame chain;
 *   - UTF-8 paths through the CRT (the executable's manifest selects the UTF-8 code page).
 * See docs/sacredfire/13-windows-port.md. */
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "platform.h"
#include <windows.h>
#include <mmsystem.h>
#include <bcrypt.h>
#include <tlhelp32.h>
#include <direct.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <io.h>
#include <limits.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <Zydis/Zydis.h>
/* mingw-w64 a3d9399 declares VirtualAlloc2/MapViewOfFile3/UnmapViewOfFile2 but not
 * CreateFileMapping2 (api-ms-win-core-memory-l1-1-7, Windows 10 1803; libonecore.a). */
WINBASEAPI HANDLE WINAPI CreateFileMapping2(HANDLE File, SECURITY_ATTRIBUTES *SecurityAttributes, ULONG DesiredAccess, ULONG PageProtection,
                                            ULONG AllocationAttributes, ULONG64 MaximumSize, PCWSTR Name, MEM_EXTENDED_PARAMETER *ExtendedParameters, ULONG ParameterCount);

/* bb_jmp_buf: rbx rbp rdi rsi r12-r15 (0-56), rsp after return (64), return address (72),
 * xmm6-15 (80-239), mxcsr (240), x87 control word (244). */
__asm__(".text\n"
        ".globl bb_setjmp\n"
        ".def bb_setjmp; .scl 2; .type 32; .endef\n"
        "bb_setjmp:\n"
        "    mov %rbx,0(%rcx)\n    mov %rbp,8(%rcx)\n    mov %rdi,16(%rcx)\n    mov %rsi,24(%rcx)\n"
        "    mov %r12,32(%rcx)\n    mov %r13,40(%rcx)\n    mov %r14,48(%rcx)\n    mov %r15,56(%rcx)\n"
        "    lea 8(%rsp),%rdx\n    mov %rdx,64(%rcx)\n    mov (%rsp),%rdx\n    mov %rdx,72(%rcx)\n"
        "    movups %xmm6,80(%rcx)\n    movups %xmm7,96(%rcx)\n    movups %xmm8,112(%rcx)\n"
        "    movups %xmm9,128(%rcx)\n    movups %xmm10,144(%rcx)\n    movups %xmm11,160(%rcx)\n"
        "    movups %xmm12,176(%rcx)\n    movups %xmm13,192(%rcx)\n    movups %xmm14,208(%rcx)\n"
        "    movups %xmm15,224(%rcx)\n    stmxcsr 240(%rcx)\n    fnstcw 244(%rcx)\n"
        "    xor %eax,%eax\n    ret\n"
        ".globl bb_longjmp\n"
        ".def bb_longjmp; .scl 2; .type 32; .endef\n"
        "bb_longjmp:\n"
        "    mov %edx,%eax\n    test %eax,%eax\n    jnz 1f\n    mov $1,%eax\n"
        "1:  mov 0(%rcx),%rbx\n    mov 8(%rcx),%rbp\n    mov 16(%rcx),%rdi\n    mov 24(%rcx),%rsi\n"
        "    mov 32(%rcx),%r12\n    mov 40(%rcx),%r13\n    mov 48(%rcx),%r14\n    mov 56(%rcx),%r15\n"
        "    movups 80(%rcx),%xmm6\n    movups 96(%rcx),%xmm7\n    movups 112(%rcx),%xmm8\n"
        "    movups 128(%rcx),%xmm9\n    movups 144(%rcx),%xmm10\n    movups 160(%rcx),%xmm11\n"
        "    movups 176(%rcx),%xmm12\n    movups 192(%rcx),%xmm13\n    movups 208(%rcx),%xmm14\n"
        "    movups 224(%rcx),%xmm15\n    ldmxcsr 240(%rcx)\n    fldcw 244(%rcx)\n"
        "    mov 64(%rcx),%rsp\n    jmp *72(%rcx)\n");

static int errno_from_win32(DWORD error) {
    switch (error) {
    case ERROR_SUCCESS: return 0;
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: case ERROR_INVALID_DRIVE:
    case ERROR_BAD_NETPATH: case ERROR_BAD_NET_NAME: case ERROR_INVALID_NAME: return ENOENT;
    case ERROR_ACCESS_DENIED: case ERROR_SHARING_VIOLATION: case ERROR_LOCK_VIOLATION:
    case ERROR_WRITE_PROTECT: return EACCES;
    case ERROR_FILE_EXISTS: case ERROR_ALREADY_EXISTS: return EEXIST;
    case ERROR_INVALID_HANDLE: return EBADF;
    case ERROR_NOT_ENOUGH_MEMORY: case ERROR_OUTOFMEMORY: case ERROR_COMMITMENT_LIMIT: return ENOMEM;
    case ERROR_DISK_FULL: case ERROR_HANDLE_DISK_FULL: return ENOSPC;
    case ERROR_DIR_NOT_EMPTY: return ENOTEMPTY;
    case ERROR_DIRECTORY: return ENOTDIR;
    case ERROR_FILENAME_EXCED_RANGE: case ERROR_BUFFER_OVERFLOW: return ENAMETOOLONG;
    case ERROR_TOO_MANY_OPEN_FILES: return EMFILE;
    case ERROR_NOACCESS: return EFAULT;
    case ERROR_NEGATIVE_SEEK: case ERROR_INVALID_PARAMETER: return EINVAL;
    case ERROR_INVALID_ADDRESS: return EINVAL;
    default: return EIO;
    }
}
int platform_last_errno(void) { return errno_from_win32(GetLastError()); }

/* ---- memory ---- */
static DWORD win_prot(int prot) {
    if (prot & PROT_EXEC) return (prot & PROT_WRITE) ? PAGE_EXECUTE_READWRITE : (prot & PROT_READ) ? PAGE_EXECUTE_READ : PAGE_EXECUTE;
    if (prot & PROT_WRITE) return PAGE_READWRITE;
    return (prot & PROT_READ) ? PAGE_READONLY : PAGE_NOACCESS;
}
size_t platform_page_size(void) { static size_t size; if (!size) { SYSTEM_INFO i; GetSystemInfo(&i); size = i.dwPageSize; } return size; }
void *platform_map_fixed(uintptr_t address, size_t size, int prot) {
    return VirtualAlloc((void *)address, size, MEM_RESERVE | MEM_COMMIT, win_prot(prot));
}
void *platform_map_anonymous(size_t size, int prot) { return VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, win_prot(prot)); }
void platform_unmap(void *address, size_t size) { (void)size; VirtualFree(address, 0, MEM_RELEASE); }
int platform_protect(void *address, size_t size, int prot) {
    DWORD old;
    if (!VirtualProtect(address, size, win_prot(prot), &old)) return -1;
    if (prot & PROT_EXEC) FlushInstructionCache(GetCurrentProcess(), address, size);
    return 0;
}
void *platform_aligned_alloc(size_t alignment, size_t size) { return _aligned_malloc(size, alignment); }
void platform_aligned_free(void *p) { _aligned_free(p); }

/* ---- guest memory pool: section views over a placeholder reservation ----
 * [lo, hi) is covered by regions, each one placeholder or one view of the section. Mapping a
 * range carves it out as a single placeholder first: views overlapping it are unmapped,
 * placeholders coalesced and split, and the parts of views outside the range mapped again with
 * the page protections they had (GPU page tracking included). */
typedef struct { uintptr_t start, end; uint64_t phys; int mapped; } HostRegion;
typedef struct { uintptr_t start, end; DWORD protect; } ProtRun;
static HostRegion *regions; static size_t region_count, region_capacity;
static HANDLE section;
static uintptr_t reserve_lo, reserve_hi;
static void win_fail(const char *what, uintptr_t address, uint64_t size) {
    fprintf(stderr, "STOP: %s failed at %p (+0x%" PRIx64 "), error %lu\n", what, (void *)address, size, GetLastError());
    exit(21);
}
static size_t region_index(uintptr_t a) { /* first region with end > a */
    size_t lo = 0, hi = region_count;
    while (lo < hi) { size_t mid = (lo + hi) / 2; if (regions[mid].end <= a) lo = mid + 1; else hi = mid; }
    return lo;
}
static void region_insert(size_t at, HostRegion r) {
    if (region_count == region_capacity) {
        size_t capacity = region_capacity ? region_capacity * 2 : 256;
        HostRegion *next = realloc(regions, capacity * sizeof(*regions));
        if (!next) { fputs("Out of memory (host regions)\n", stderr); exit(1); }
        regions = next; region_capacity = capacity;
    }
    memmove(regions + at + 1, regions + at, (region_count - at) * sizeof(*regions));
    regions[at] = r; ++region_count;
}
static void region_erase(size_t at, size_t n) { memmove(regions + at, regions + at + n, (region_count - at - n) * sizeof(*regions)); region_count -= n; }
void platform_pool_reserve_range(uintptr_t lo, uintptr_t hi) {
    if (region_count) return;
    if (!VirtualAlloc2(NULL, (void *)lo, hi - lo, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0)) {
        MEMORY_BASIC_INFORMATION m;
        fprintf(stderr, "STOP: reserving the PS4 address range %p-%p failed (error %lu).", (void *)lo, (void *)hi, GetLastError());
        for (uintptr_t at = lo; at < hi && VirtualQuery((void *)at, &m, sizeof(m)); at = (uintptr_t)m.BaseAddress + m.RegionSize)
            if (m.State != MEM_FREE) { fprintf(stderr, " Something is already mapped at %p (+0x%zx, state %#lx, type %#lx).", m.BaseAddress, m.RegionSize, m.State, m.Type); break; }
        fputs(" The game executable must be linked with --disable-high-entropy-va (docs/sacredfire/13-windows-port.md).\n", stderr);
        exit(21);
    }
    reserve_lo = lo; reserve_hi = hi;
    region_insert(0, (HostRegion){lo, hi, 0, 0});
}
/* A view of the section over the placeholder of region r. 0, or -1 with the placeholder kept
 * (a bad offset or an exhausted page file: the caller reports NO_MEMORY and the range reads as
 * unmapped, which is the state before any mapping there). */
static int view_map(const HostRegion *r, DWORD protect) {
    DWORD initial = protect == PAGE_NOACCESS ? PAGE_READWRITE : protect, old;
    if (!MapViewOfFile3(section, GetCurrentProcess(), (void *)r->start, r->phys, r->end - r->start, MEM_REPLACE_PLACEHOLDER, initial, NULL, 0)) {
        static int reported;
        if (reported++ < 4) fprintf(stderr, "Runtime: MapViewOfFile3 at %p (+0x%" PRIxPTR ", section offset 0x%" PRIx64 ") failed, error %lu\n", (void *)r->start, r->end - r->start, r->phys, GetLastError());
        return -1;
    }
    if (initial != protect && !VirtualProtect((void *)r->start, r->end - r->start, protect, &old))
        win_fail("VirtualProtect", r->start, r->end - r->start);
    return 0;
}
static size_t save_protections(uintptr_t start, uintptr_t end, ProtRun **runs, size_t count, size_t *capacity) {
    for (uintptr_t at = start; at < end;) {
        MEMORY_BASIC_INFORMATION m;
        if (!VirtualQuery((void *)at, &m, sizeof(m))) win_fail("VirtualQuery", at, 0);
        uintptr_t next = (uintptr_t)m.BaseAddress + m.RegionSize;
        if (next > end) next = end;
        if (count == *capacity) { *capacity = *capacity ? *capacity * 2 : 16; *runs = realloc(*runs, *capacity * sizeof(**runs)); if (!*runs) exit(1); }
        (*runs)[count++] = (ProtRun){at, next, m.Protect};
        at = next;
    }
    return count;
}
/* Leaves [a,b) as exactly one unmapped placeholder; returns its region index, or (size_t)-1 when
 * the range is not inside the reservation. */
static size_t carve_host(uintptr_t a, uintptr_t b) {
    if (a < reserve_lo || b > reserve_hi || a >= b) return (size_t)-1;
    size_t first = region_index(a), last = first;
    if (first == region_count || regions[first].start > a) return (size_t)-1;
    while (last < region_count && regions[last].start < b) ++last;
    if (regions[last - 1].end < b) return (size_t)-1;
    uintptr_t lo = regions[first].start, hi = regions[last - 1].end;
    HostRegion left = regions[first], right = regions[last - 1];
    int keep_left = left.mapped && lo < a, keep_right = right.mapped && b < hi;
    if (first == last - 1 && !left.mapped && lo == a && hi == b) return first;
    ProtRun *runs = NULL; size_t nruns = 0, capacity = 0;
    if (keep_left) nruns = save_protections(lo, a, &runs, nruns, &capacity);
    if (keep_right) nruns = save_protections(b, hi, &runs, nruns, &capacity);
    for (size_t i = first; i < last; ++i) if (regions[i].mapped) {
        if (!UnmapViewOfFile2(GetCurrentProcess(), (void *)regions[i].start, MEM_PRESERVE_PLACEHOLDER))
            win_fail("UnmapViewOfFile2", regions[i].start, regions[i].end - regions[i].start);
    }
    if (last - first > 1 && !VirtualFree((void *)lo, hi - lo, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS))
        win_fail("coalescing placeholders", lo, hi - lo);
    region_erase(first, last - first);
    region_insert(first, (HostRegion){lo, hi, 0, 0});
    size_t at = first;
    if (lo < a) {
        if (!VirtualFree((void *)lo, a - lo, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) win_fail("splitting a placeholder", lo, a - lo);
        regions[at].end = a;
        region_insert(at + 1, (HostRegion){a, hi, 0, 0});
        if (keep_left) { regions[at].phys = left.phys; regions[at].mapped = 1; if (view_map(&regions[at], PAGE_READWRITE)) win_fail("remapping the kept part of a view", regions[at].start, a - lo); }
        ++at;
    }
    if (b < hi) {
        if (!VirtualFree((void *)a, b - a, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) win_fail("splitting a placeholder", a, b - a);
        regions[at].end = b;
        region_insert(at + 1, (HostRegion){b, hi, 0, 0});
        if (keep_right) { regions[at + 1].phys = right.phys + (b - right.start); regions[at + 1].mapped = 1; if (view_map(&regions[at + 1], PAGE_READWRITE)) win_fail("remapping the kept part of a view", b, hi - b); }
    }
    for (size_t i = 0; i < nruns; ++i) {
        DWORD old;
        if (runs[i].protect != PAGE_READWRITE && !VirtualProtect((void *)runs[i].start, runs[i].end - runs[i].start, runs[i].protect, &old))
            win_fail("restoring page protection", runs[i].start, runs[i].end - runs[i].start);
    }
    free(runs);
    return at;
}
int platform_pool_create(uint64_t bytes, unsigned char **backing) {
    if (section) return 0;
    section = CreateFileMapping2(INVALID_HANDLE_VALUE, NULL, FILE_MAP_ALL_ACCESS, PAGE_EXECUTE_READWRITE, SEC_COMMIT, bytes, NULL, NULL, 0);
    if (!section) {
        fprintf(stderr, "Runtime: cannot create %" PRIu64 " MiB of guest memory (error %lu); is the page file large enough?\n", bytes >> 20, GetLastError());
        return -1;
    }
    *backing = MapViewOfFile3(section, GetCurrentProcess(), NULL, 0, bytes, 0, PAGE_READWRITE, NULL, 0);
    return *backing ? 0 : -1;
}
int platform_pool_map(uintptr_t address, uint64_t size, int prot, int reserved, uint64_t phys) {
    size_t i = carve_host(address, address + size);
    if (i == (size_t)-1) return -1;
    if (reserved) return 0;
    regions[i].phys = phys; regions[i].mapped = 1;
    if (view_map(&regions[i], win_prot(prot))) { regions[i].mapped = 0; regions[i].phys = 0; return -1; }
    return 0;
}
int platform_pool_unmap(uintptr_t address, uint64_t size) { return carve_host(address, address + size) == (size_t)-1 ? -1 : 0; }
int platform_pool_protect(uintptr_t address, uint64_t size, int prot) {
    DWORD protect = win_prot(prot), old;
    uintptr_t end = address + size;
    for (size_t i = region_index(address); i < region_count && regions[i].start < end; ++i) {
        if (!regions[i].mapped) continue;
        uintptr_t a = regions[i].start > address ? regions[i].start : address, b = regions[i].end < end ? regions[i].end : end;
        if (!VirtualProtect((void *)a, b - a, protect, &old)) return -1;
    }
    return 0;
}
void platform_pool_discard(unsigned char *backing, uint64_t phys, uint64_t size) { memset(backing + phys, 0, size); }

/* ---- threads ---- */
/* TEB TlsSlots[64] start at gs:0x1480; slots 64+ live in TlsExpansionSlots and cannot be used. */
static DWORD tls_slot = TLS_OUT_OF_INDEXES;
uint32_t platform_thread_tcb_displacement(void) {
    if (tls_slot == TLS_OUT_OF_INDEXES) {
        tls_slot = TlsAlloc();
        if (tls_slot >= 64) { fprintf(stderr, "STOP: no TEB TLS slot below 64 for the guest TCB (got %lu); the slot must be claimed before any driver loads\n", tls_slot); exit(21); }
        /* The TEB layout this relies on: TlsSlots at 0x1480 (Windows x64, every version since XP). */
        _Static_assert(0x1480 == 0x1480, "TEB.TlsSlots offset");
    }
    return 0x1480 + 8 * (uint32_t)tls_slot;
}
void platform_thread_publish_tcb(void *tcb) {
    platform_thread_tcb_displacement();
    if (!TlsSetValue(tls_slot, tcb)) { fprintf(stderr, "STOP: TlsSetValue failed (%lu)\n", GetLastError()); exit(21); }
    /* The patched loads read the slot through GS; check the two agree once per thread. */
    void *seen; __asm__ volatile("mov %%gs:(%1), %0" : "=r"(seen) : "r"((uintptr_t)(0x1480 + 8 * tls_slot)));
    if (seen != tcb) { fputs("STOP: TEB TlsSlots layout differs from the expected 0x1480 + 8*slot\n", stderr); exit(21); }
}
/* The one guest TLS access form the preparation scripts leave: `mov rax, gs:[0]`
 * (65 48 8b 04 25 00 00 00 00). Every match is decoded with Zydis and must be that instruction;
 * other GS/FS-segment memory accesses in the image stop the loader with their offsets. */
uint64_t platform_patch_guest_tls_loads(unsigned char *image, const uint64_t *seg_address, const uint64_t *seg_size,
                                        const uint64_t *seg_flags, uint64_t segments, int64_t expected) {
    static const unsigned char load[] = {0x65, 0x48, 0x8b, 0x04, 0x25, 0, 0, 0, 0};
    const uint32_t displacement = platform_thread_tcb_displacement();
    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    uint64_t patched = 0, rejected = 0;
    for (uint64_t s = 0; s < segments; ++s) {
        if (!(seg_flags[s] & 1) || seg_size[s] < sizeof(load)) continue;
        unsigned char *base = image + seg_address[s], *end = base + seg_size[s] - sizeof(load);
        for (unsigned char *at = base; at <= end; ++at) {
            if (at[0] != 0x65 && at[0] != 0x64) continue;
            /* Any segment-prefixed `mov r64, [disp32]` (48 8b /r with mod=00 rm=100 SIB base=101). */
            if (!(at[1] == 0x48 && at[2] == 0x8b && (at[3] & 0xc7) == 0x04 && at[4] == 0x25)) continue;
            ZydisDecodedInstruction instruction;
            ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
            if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, at, sizeof(load), &instruction, operands))) {
                fprintf(stderr, "STOP: guest TLS access at image offset 0x%" PRIxPTR " does not decode\n", (uintptr_t)(at - image)); exit(21);
            }
            const int is_tcb_load = instruction.mnemonic == ZYDIS_MNEMONIC_MOV && instruction.length == sizeof(load) &&
                operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER && operands[0].reg.value == ZYDIS_REGISTER_RAX &&
                operands[1].type == ZYDIS_OPERAND_TYPE_MEMORY && operands[1].mem.segment == ZYDIS_REGISTER_GS &&
                operands[1].mem.base == ZYDIS_REGISTER_NONE && operands[1].mem.index == ZYDIS_REGISTER_NONE &&
                operands[1].mem.disp.value == 0 && !memcmp(at, load, sizeof(load));
            if (is_tcb_load) { memcpy(at + 5, &displacement, 4); ++patched; at += sizeof(load) - 1; continue; }
            /* A byte scan also meets 0x64/0x65 that end another instruction (UFC 3: `jne +0x64`
             * then `mov rdi, [0x10]`, EA's assert trap). Only a [0] load is a TCB access; the Linux
             * linker leaves every other form alone too, so they stay as they are here. */
            if (at[5] | at[6] | at[7] | at[8]) continue;
            if (rejected++ < 8)
                fprintf(stderr, "STOP: unsupported guest TLS access form at image offset 0x%" PRIxPTR " (%02x %02x %02x %02x %02x ...): only `mov rax, gs:[0]` is handled\n",
                        (uintptr_t)(at - image), at[0], at[1], at[2], at[3], at[4]);
        }
    }
    if (rejected) { fprintf(stderr, "STOP: %" PRIu64 " unsupported TLS access sites\n", rejected); exit(21); }
    if (expected >= 0 && (uint64_t)expected != patched) {
        fprintf(stderr, "STOP: patched %" PRIu64 " guest TCB loads but the preparation step recorded %" PRId64 " (link.json fs_loads_patched): the scans disagree\n", patched, expected);
        exit(21);
    }
    return patched;
}
void platform_thread_stack(pthread_attr_t *attr, size_t bytes) {
    /* Windows places the stack itself (low: no high-entropy ASLR, so below 1 TiB) and commits all
     * of it (winpthreads passes the size as the commit size): guest code has no stack probes and
     * would step over a guard page. */
    pthread_attr_setstacksize(attr, bytes);
}
uint32_t platform_thread_id(void) { return (uint32_t)GetCurrentThreadId(); }
void platform_thread_set_name(const char *name) {
    wchar_t wide[64];
    int n = MultiByteToWideChar(CP_UTF8, 0, name, -1, wide, 63);
    wide[n > 0 ? n : 0] = 0;
    SetThreadDescription(GetCurrentThread(), wide);
    pthread_setname_np(pthread_self(), name); /* winpthreads keeps it for pthread_getname_np */
    /* As the GoW3R port: the threads on the frame's critical path (the game's main, simulation
     * and render threads, audio) run above normal so job pools and loaders cannot preempt them
     * on a loaded laptop; everything else keeps the default. */
    static const char *const critical[] = {"Main", "main", "Sim", "Render", "Audio", "audio", "Sound"};
    for (size_t i = 0; i < sizeof(critical) / sizeof(critical[0]); ++i)
        if (strstr(name, critical[i])) { SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL); break; }
}
void platform_thread_get_name(char *out, size_t size) {
    if (size) { out[0] = '?'; out[size > 1 ? 1 : 0] = 0; }
    if (size >= 16 && pthread_getname_np(pthread_self(), out, size)) { out[0] = '?'; out[1] = 0; }
}
uintptr_t platform_thread_stack_top(void) { return (uintptr_t)((const NT_TIB *)NtCurrentTeb())->StackBase; }
int platform_current_cpu(void) { return (int)GetCurrentProcessorNumber(); }

/* ---- time ---- */
uint64_t platform_monotonic_ns(void) {
    static LARGE_INTEGER frequency;
    if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    const uint64_t seconds = (uint64_t)now.QuadPart / (uint64_t)frequency.QuadPart, rest = (uint64_t)now.QuadPart % (uint64_t)frequency.QuadPart;
    return seconds * 1000000000u + rest * 1000000000u / (uint64_t)frequency.QuadPart;
}
uint64_t platform_realtime_ns(void) {
    FILETIME ft; GetSystemTimePreciseAsFileTime(&ft);
    const uint64_t ticks = ((uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime) - UINT64_C(116444736000000000); /* 1601 -> 1970, 100 ns */
    return ticks * 100;
}
/* winpthreads condition variables take CLOCK_REALTIME deadlines only (setclock(MONOTONIC) is
 * EINVAL); the relative wait is computed from the deadline right before each wait. */
clockid_t platform_wait_clock(void) { return CLOCK_REALTIME; }
void platform_cond_init_waitclock(pthread_cond_t *cond) { pthread_cond_init(cond, NULL); }
/* One high-resolution timer per thread (Windows 10 1803+); a plain waitable timer before. */
static __thread HANDLE sleep_timer;
/* As the GoW3R port: a high-resolution waitable timer (Windows 10 1803+, wakes within ~0.5 ms)
 * for all but the last 100 us, which are spun, so short guest sleeps are not stretched to the
 * scheduler tick. */
int platform_sleep_ns(uint64_t ns) {
    enum { SPIN_NS = 100000 };
    if (!ns) { SwitchToThread(); return 0; }
    const uint64_t deadline = platform_monotonic_ns() + ns;
    if (ns > SPIN_NS) {
        if (!sleep_timer) {
            sleep_timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
            if (!sleep_timer) sleep_timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
        }
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)((ns - SPIN_NS) / 100); /* relative, 100 ns units */
        if (sleep_timer && due.QuadPart < 0 && SetWaitableTimer(sleep_timer, &due, 0, NULL, NULL, FALSE)) WaitForSingleObject(sleep_timer, INFINITE);
        else Sleep((DWORD)((ns - SPIN_NS) / 1000000));
    }
    while (platform_monotonic_ns() < deadline) YieldProcessor();
    return 0;
}
void platform_sleep_until_ns(uint64_t deadline_ns) {
    for (uint64_t now = platform_monotonic_ns(); now < deadline_ns; now = platform_monotonic_ns()) platform_sleep_ns(deadline_ns - now);
}
long platform_utc_offset(time_t t) {
    struct tm local, utc;
    if (localtime_s(&local, &t) || gmtime_s(&utc, &t)) return 0;
    /* mktime interprets utc as local time: the difference is the zone offset (with DST). */
    utc.tm_isdst = local.tm_isdst;
    time_t as_local = mktime(&utc);
    return as_local == (time_t)-1 ? 0 : (long)(t - as_local);
}
int platform_is_dst(time_t t) { struct tm local; if (localtime_s(&local, &t)) return 0; return local.tm_isdst > 0; }
int platform_random(void *buffer, size_t size) {
    while (size) {
        ULONG chunk = size > 0x10000000u ? 0x10000000u : (ULONG)size;
        if (BCryptGenRandom(NULL, buffer, chunk, BCRYPT_USE_SYSTEM_PREFERRED_RNG)) return -1;
        buffer = (char *)buffer + chunk; size -= chunk;
    }
    return 0;
}
int platform_cpu_times(int who, int64_t *user_us, int64_t *kernel_us) {
    FILETIME created, exited, kernel, user;
    BOOL ok = who ? GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)
                  : GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    if (!ok) return -1;
    *user_us = (int64_t)(((uint64_t)user.dwHighDateTime << 32 | user.dwLowDateTime) / 10);
    *kernel_us = (int64_t)(((uint64_t)kernel.dwHighDateTime << 32 | kernel.dwLowDateTime) / 10);
    return 0;
}

/* ---- files ----
 * Directories cannot be opened as CRT descriptors: platform_open hands out descriptors from
 * DIR_FD_BASE up that stand for a directory path (stat, close; reads fail with EISDIR). */
#define DIR_FDS 64
#define DIR_FD_BASE 0x40000000
static char *dir_paths[DIR_FDS];
static CRITICAL_SECTION dir_lock;
static INIT_ONCE dir_lock_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK init_dir_lock(PINIT_ONCE once, PVOID p, PVOID *c) { (void)once; (void)p; (void)c; InitializeCriticalSection(&dir_lock); return TRUE; }
static const char *dir_path(int fd) { return fd >= DIR_FD_BASE && fd < DIR_FD_BASE + DIR_FDS ? dir_paths[fd - DIR_FD_BASE] : NULL; }
static struct timespec filetime_to_timespec(FILETIME ft) {
    const uint64_t ticks = ((uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime);
    const int64_t unix_ticks = (int64_t)ticks - INT64_C(116444736000000000);
    struct timespec t = {(time_t)(unix_ticks / 10000000), (long)(unix_ticks % 10000000) * 100};
    return t;
}
static void convert_attributes(DWORD attributes, uint64_t size, FILETIME atime, FILETIME mtime, FILETIME ctime, PlatformStat *g) {
    memset(g, 0, sizeof(*g));
    g->is_dir = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    g->is_lnk = (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    g->is_reg = !g->is_dir && !g->is_lnk;
    /* FreeBSD mode bits: directories rwxr-xr-x, files rw-r--r-- (read-only files r--r--r--). */
    g->mode = g->is_dir ? 0040755 : (attributes & FILE_ATTRIBUTE_READONLY) ? 0100444 : 0100644;
    g->size = g->is_dir ? 0 : (int64_t)size;
    g->blocks = (g->size + 511) / 512; g->blksize = 4096; g->nlink = 1;
    g->atim = filetime_to_timespec(atime); g->mtim = filetime_to_timespec(mtime); g->ctim = filetime_to_timespec(ctime);
}
int platform_stat(const char *path, PlatformStat *out) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &a)) return -errno_from_win32(GetLastError());
    convert_attributes(a.dwFileAttributes, (uint64_t)a.nFileSizeHigh << 32 | a.nFileSizeLow, a.ftLastAccessTime, a.ftLastWriteTime, a.ftCreationTime, out);
    return 0;
}
int platform_fstat(int fd, PlatformStat *out) {
    const char *dir = dir_path(fd);
    if (dir) return platform_stat(dir, out);
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) return -EBADF;
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(h, &info)) return -errno_from_win32(GetLastError());
    convert_attributes(info.dwFileAttributes, (uint64_t)info.nFileSizeHigh << 32 | info.nFileSizeLow, info.ftLastAccessTime, info.ftLastWriteTime, info.ftCreationTime, out);
    out->nlink = info.nNumberOfLinks; out->ino = info.nFileIndexLow; out->dev = info.dwVolumeSerialNumber;
    return 0;
}
int platform_open(const char *path, int flags, int mode) {
    (void)mode;
    PlatformStat s;
    const int exists = platform_stat(path, &s) == 0;
    if (exists && s.is_dir) {
        if (flags & 3) return -EISDIR;
        InitOnceExecuteOnce(&dir_lock_once, init_dir_lock, NULL, NULL);
        EnterCriticalSection(&dir_lock);
        int fd = -EMFILE;
        for (int i = 0; i < DIR_FDS; ++i) if (!dir_paths[i]) { dir_paths[i] = _strdup(path); fd = dir_paths[i] ? DIR_FD_BASE + i : -ENOMEM; break; }
        LeaveCriticalSection(&dir_lock);
        return fd;
    }
    if (flags & 0x20000) return -(exists ? ENOTDIR : ENOENT);
    int r;
    switch (flags & 3) { case 0: r = _O_RDONLY; break; case 1: r = _O_WRONLY; break; default: r = _O_RDWR; }
    if (flags & 0x8) r |= _O_APPEND;
    if (flags & 0x200) r |= _O_CREAT;
    if (flags & 0x400) r |= _O_TRUNC;
    if (flags & 0x800) r |= _O_EXCL;
    /* 0x4 nonblock and 0x80 sync have no CRT equivalent; writes are flushed by platform_fsync. */
    int fd = _open(path, r | _O_BINARY | _O_NOINHERIT, _S_IREAD | _S_IWRITE);
    return fd < 0 ? -errno : fd;
}
int platform_close(int fd) {
    if (dir_path(fd)) {
        EnterCriticalSection(&dir_lock);
        free(dir_paths[fd - DIR_FD_BASE]); dir_paths[fd - DIR_FD_BASE] = NULL;
        LeaveCriticalSection(&dir_lock);
        return 0;
    }
    return _close(fd) ? -errno : 0;
}
/* The CRT reads and writes at most INT_MAX bytes per call. */
ssize_t platform_read(int fd, void *buffer, size_t size) {
    if (dir_path(fd)) { errno = EISDIR; return -1; }
    int n = _read(fd, buffer, (unsigned)(size > 0x7ffff000 ? 0x7ffff000 : size));
    return n;
}
ssize_t platform_write(int fd, const void *buffer, size_t size) {
    if (dir_path(fd)) { errno = EISDIR; return -1; }
    return _write(fd, buffer, (unsigned)(size > 0x7ffff000 ? 0x7ffff000 : size));
}
/* Positioned I/O on a synchronous handle moves the file pointer: it is restored afterwards.
 * A concurrent read() on the same descriptor would race, as with a shared offset anyway. */
static ssize_t positioned(int fd, void *buffer, size_t size, int64_t offset, int write) {
    if (dir_path(fd)) { errno = EISDIR; return -1; }
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    if (offset < 0) { errno = EINVAL; return -1; }
    LARGE_INTEGER zero = {{0}}, saved;
    if (!SetFilePointerEx(h, zero, &saved, FILE_CURRENT)) { errno = errno_from_win32(GetLastError()); return -1; }
    size_t done = 0;
    while (done < size) {
        DWORD chunk = size - done > 0x40000000u ? 0x40000000u : (DWORD)(size - done), n = 0;
        OVERLAPPED at = {0};
        uint64_t position = (uint64_t)offset + done;
        at.Offset = (DWORD)position; at.OffsetHigh = (DWORD)(position >> 32);
        BOOL ok = write ? WriteFile(h, (const char *)buffer + done, chunk, &n, &at) : ReadFile(h, (char *)buffer + done, chunk, &n, &at);
        if (!ok) {
            DWORD e = GetLastError();
            if (e == ERROR_HANDLE_EOF) break;
            SetFilePointerEx(h, saved, NULL, FILE_BEGIN);
            if (done) return (ssize_t)done;
            errno = errno_from_win32(e);
            return -1;
        }
        done += n;
        if (n < chunk) break;
    }
    SetFilePointerEx(h, saved, NULL, FILE_BEGIN);
    return (ssize_t)done;
}
ssize_t platform_pread(int fd, void *buffer, size_t size, int64_t offset) { return positioned(fd, buffer, size, offset, 0); }
ssize_t platform_pwrite(int fd, const void *buffer, size_t size, int64_t offset) { return positioned(fd, (void *)buffer, size, offset, 1); }
int64_t platform_lseek(int fd, int64_t offset, int whence) { if (dir_path(fd)) { errno = EISDIR; return -1; } return _lseeki64(fd, offset, whence); }
int platform_fsync(int fd) { if (dir_path(fd)) return 0; return _commit(fd) ? -errno : 0; }
int platform_ftruncate(int fd, int64_t length) { if (dir_path(fd)) return -EISDIR; errno_t e = _chsize_s(fd, length); return e ? -e : 0; }
int platform_truncate(const char *path, int64_t length) {
    int fd = _open(path, _O_RDWR | _O_BINARY | _O_NOINHERIT);
    if (fd < 0) return -errno;
    errno_t e = _chsize_s(fd, length);
    _close(fd);
    return e ? -e : 0;
}
int platform_rename(const char *from, const char *to) {
    /* POSIX rename replaces an existing destination; the CRT's fails. MOVEFILE_WRITE_THROUGH:
     * the rename is on disk when the call returns (saves: the old file or the new one, never none). */
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ? 0 : -errno_from_win32(GetLastError());
}
int platform_unlink(const char *path) {
    if (DeleteFileA(path)) return 0;
    DWORD e = GetLastError();
    if (e == ERROR_ACCESS_DENIED) { /* read-only attribute, as POSIX unlink ignores it */
        DWORD a = GetFileAttributesA(path);
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY) && SetFileAttributesA(path, a & ~FILE_ATTRIBUTE_READONLY) && DeleteFileA(path)) return 0;
        e = GetLastError();
    }
    return -errno_from_win32(e);
}
int platform_mkdir(const char *path, int mode) { (void)mode; return _mkdir(path) ? -errno : 0; }
int platform_rmdir(const char *path) { return _rmdir(path) ? -errno : 0; }
int platform_access(const char *path, int mode) { return _access(path, mode & 6) ? -errno : 0; /* X_OK is invalid for the CRT */ }
PlatformListing *platform_list_directory(const char *path) {
    DIR *d = opendir(path);
    if (!d) return NULL;
    PlatformListing *l = calloc(1, sizeof(*l));
    size_t capacity = 0, bytes = 0, cap_names = 0;
    struct dirent *e;
    while (l && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        size_t n = strlen(e->d_name) + 1;
        if (l->count == capacity) {
            capacity = capacity ? capacity * 2 : 64;
            l->offsets = realloc(l->offsets, capacity * sizeof(size_t));
            l->types = realloc(l->types, capacity);
        }
        if (bytes + n > cap_names) { cap_names = (bytes + n) * 2; l->names = realloc(l->names, cap_names); }
        if (!l->offsets || !l->types || !l->names) { fputs("Out of memory listing directory\n", stderr); exit(1); }
        memcpy(l->names + bytes, e->d_name, n);
        l->offsets[l->count] = bytes;
        /* MinGW's dirent has no d_type. */
        char child[PATH_MAX]; PlatformStat entry;
        snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
        l->types[l->count] = platform_stat(child, &entry) ? 0 : entry.is_dir ? 4 : entry.is_reg ? 8 : entry.is_lnk ? 10 : 0;
        ++l->count; bytes += n;
    }
    closedir(d);
    return l;
}
void platform_free_listing(PlatformListing *l) { if (l) { free(l->names); free(l->offsets); free(l->types); free(l); } }
static int remove_tree_unchecked(const char *path) {
    DWORD attributes = GetFileAttributesA(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) return 0;
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        if (attributes & FILE_ATTRIBUTE_READONLY) SetFileAttributesA(path, attributes & ~FILE_ATTRIBUTE_READONLY);
        if (attributes & FILE_ATTRIBUTE_DIRECTORY) return RemoveDirectoryA(path) ? 0 : -1; /* a junction: not followed */
        return DeleteFileA(path) ? 0 : -1;
    }
    char pattern[PATH_MAX];
    snprintf(pattern, sizeof(pattern), "%s\\*", path);
    WIN32_FIND_DATAA entry;
    HANDLE find = FindFirstFileA(pattern, &entry);
    int result = 0;
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(entry.cFileName, ".") || !strcmp(entry.cFileName, "..")) continue;
            char child[PATH_MAX];
            if ((size_t)snprintf(child, sizeof(child), "%s\\%s", path, entry.cFileName) >= sizeof(child)) { result = -1; continue; }
            if (remove_tree_unchecked(child)) result = -1;
        } while (FindNextFileA(find, &entry));
        FindClose(find);
    }
    if (!RemoveDirectoryA(path)) result = -1;
    return result;
}
int platform_remove_tree(const char *root, const char *path) {
    /* Containment: the resolved path lies strictly inside the resolved root. */
    char full_root[PATH_MAX], full_path[PATH_MAX];
    if (!_fullpath(full_root, root, sizeof(full_root)) || !_fullpath(full_path, path, sizeof(full_path))) return -1;
    size_t n = strlen(full_root);
    while (n && (full_root[n - 1] == '\\' || full_root[n - 1] == '/')) full_root[--n] = 0;
    if (!n || _strnicmp(full_path, full_root, n) || (full_path[n] != '\\' && full_path[n] != '/') || !full_path[n + 1]) return -1;
    return remove_tree_unchecked(full_path);
}
char *platform_realpath(const char *path, char *out) { return GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES ? NULL : _fullpath(out, path, PATH_MAX); }

/* ---- process and faults ---- */
int platform_safe_read(uintptr_t address, void *out, size_t size) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), (const void *)address, out, size, &got) && got == size;
}
void platform_describe_address(uintptr_t address, char *out, size_t size) {
    HMODULE module = NULL;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)address, &module) && module) {
        GetModuleFileNameA(module, name, sizeof(name));
        const char *base = strrchr(name, '\\');
        snprintf(out, size, "%s+0x%" PRIxPTR, base ? base + 1 : name, address - (uintptr_t)module);
        return;
    }
    snprintf(out, size, "%p", (void *)address);
}
int platform_backtrace(void **frames, int max) { return (int)CaptureStackBackTrace(0, (DWORD)max, frames, NULL); }

static PlatformGpuFault gpu_fault;
static PlatformRecoverPoint recover_point;
static PlatformFaultExtra fault_extra;
static const unsigned char *guest_image;
static uint64_t guest_image_size;
static void describe_rip(char *out, size_t size, uintptr_t address) {
    if (guest_image && address - (uintptr_t)guest_image < guest_image_size) snprintf(out, size, "guest offset 0x%" PRIxPTR, address - (uintptr_t)guest_image);
    else platform_describe_address(address, out, size);
}
static LONG fatal_reported;
static LONG fatal_exception(EXCEPTION_POINTERS *info) {
    if (InterlockedExchange(&fatal_reported, 1)) { Sleep(INFINITE); }
    const EXCEPTION_RECORD *record = info->ExceptionRecord;
    const CONTEXT *c = info->ContextRecord;
    char where[MAX_PATH + 64], thread[32] = "?";
    platform_thread_get_name(thread, sizeof(thread));
    describe_rip(where, sizeof(where), (uintptr_t)c->Rip);
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2)
        fprintf(stderr, "%s fault: %s access to %p at %s (thread %lu %s)\n",
                guest_image && c->Rip - (uintptr_t)guest_image < guest_image_size ? "Guest" : "Host",
                record->ExceptionInformation[0] == 1 ? "write" : record->ExceptionInformation[0] == 8 ? "execute" : "read",
                (void *)record->ExceptionInformation[1], where, GetCurrentThreadId(), thread);
    else
        fprintf(stderr, "Fault: exception 0x%08lx at %s (thread %lu %s)\n", record->ExceptionCode, where, GetCurrentThreadId(), thread);
    fprintf(stderr, "  rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx\n  rsi=%016llx rdi=%016llx rbp=%016llx rsp=%016llx\n",
            c->Rax, c->Rbx, c->Rcx, c->Rdx, c->Rsi, c->Rdi, c->Rbp, c->Rsp);
    if (fault_extra) fault_extra(info);
    /* rbp chain (guest and host code keep frame pointers). */
    uintptr_t rbp = (uintptr_t)c->Rbp;
    for (int depth = 0; depth < 24 && rbp; ++depth) {
        uintptr_t frame[2];
        if (!platform_safe_read(rbp, frame, sizeof(frame)) || frame[0] <= rbp) break;
        describe_rip(where, sizeof(where), frame[1]);
        fprintf(stderr, "  #%d %s\n", depth, where);
        rbp = frame[0];
    }
    fflush(NULL);
    TerminateProcess(GetCurrentProcess(), 3);
    return EXCEPTION_CONTINUE_SEARCH;
}
static PlatformStepHandler step_handler;
void platform_install_step_handler(PlatformStepHandler handler) { step_handler = handler; }
static LONG CALLBACK vectored_fault(EXCEPTION_POINTERS *info) {
    const EXCEPTION_RECORD *record = info->ExceptionRecord;
    DWORD code = record->ExceptionCode;
    if (code == EXCEPTION_SINGLE_STEP && step_handler && step_handler(info)) return EXCEPTION_CONTINUE_EXECUTION;
    if (code == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
        /* GPU page tracking (write-protected guest pages) is resolved first. */
        if (gpu_fault && gpu_fault(info, (void *)record->ExceptionInformation[1])) return EXCEPTION_CONTINUE_EXECUTION;
        bb_fault_jmp_buf *recover = recover_point ? recover_point() : NULL;
        if (recover) {
            /* A speculative guest memory read (GPU draw preparation) faulted: the thread resumes
             * in bb_longjmp, outside the exception dispatcher, back at its recovery point. */
            CONTEXT *c = info->ContextRecord;
            c->Rsp = ((c->Rsp - 128) & ~(DWORD64)15) - 40;
            c->Rcx = (DWORD64)(uintptr_t)*recover;
            c->Rdx = 1;
            c->Rip = (DWORD64)(uintptr_t)bb_longjmp;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION || code == EXCEPTION_PRIV_INSTRUCTION ||
        code == EXCEPTION_STACK_OVERFLOW || code == EXCEPTION_INT_DIVIDE_BY_ZERO || code == EXCEPTION_DATATYPE_MISALIGNMENT ||
        code == EXCEPTION_IN_PAGE_ERROR) {
        /* Faults in guest code, the loader or the runtime end the process with a report; drivers
         * and system libraries may handle their own exceptions (continue the search). */
        uintptr_t rip = (uintptr_t)info->ContextRecord->Rip;
        HMODULE self = GetModuleHandleA(NULL), module = NULL;
        int ours = (guest_image && rip - (uintptr_t)guest_image < guest_image_size) ||
                   (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)rip, &module) && module == self) ||
                   !module;
        if (ours) return fatal_exception(info);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
static LONG WINAPI unhandled_fault(EXCEPTION_POINTERS *info) { return fatal_exception(info); }
void platform_install_fault_handlers(PlatformGpuFault gpu, PlatformRecoverPoint recover, PlatformFaultExtra extra, const unsigned char *image, uint64_t image_size) {
    gpu_fault = gpu; recover_point = recover; fault_extra = extra; guest_image = image; guest_image_size = image_size;
    static int installed;
    if (installed) return;
    installed = 1;
    AddVectoredExceptionHandler(1, vectored_fault);
    SetUnhandledExceptionFilter(unhandled_fault);
}
/* Watchdog: a thread that, after the timeout, suspends every other thread of the process and
 * prints its RIP and rbp chain (guest offsets inside the image), then exits with 142 (Linux's
 * 128+SIGALRM, so logs read the same). */
static unsigned watchdog_seconds;
static void dump_thread(DWORD tid, const CONTEXT *c) {
    char where[MAX_PATH + 64];
    describe_rip(where, sizeof(where), (uintptr_t)c->Rip);
    fprintf(stderr, "  tid=%lu rip=%016llx (%s) rdi=%016llx\n", tid, c->Rip, where, c->Rdi);
    uintptr_t rbp = (uintptr_t)c->Rbp;
    for (int depth = 0; depth < 24 && rbp; ++depth) {
        uintptr_t frame[2];
        if (!platform_safe_read(rbp, frame, sizeof(frame)) || frame[0] <= rbp) break;
        describe_rip(where, sizeof(where), frame[1]);
        fprintf(stderr, "    #%d %s\n", depth, where);
        rbp = frame[0];
    }
}
static DWORD WINAPI watchdog_main(LPVOID p) {
    (void)p;
    Sleep(watchdog_seconds * 1000u);
    fputs("STOP: watchdog timeout; thread stacks:\n", stderr);
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 entry = {.dwSize = sizeof(entry)};
    const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    if (snapshot != INVALID_HANDLE_VALUE && Thread32First(snapshot, &entry)) do {
        if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == self) continue;
        HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
        if (!t) continue;
        if (SuspendThread(t) != (DWORD)-1) {
            CONTEXT c = {.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER};
            if (GetThreadContext(t, &c)) dump_thread(entry.th32ThreadID, &c);
            ResumeThread(t);
        }
        CloseHandle(t);
    } while (Thread32Next(snapshot, &entry));
    if (snapshot != INVALID_HANDLE_VALUE) CloseHandle(snapshot);
    fflush(NULL);
    TerminateProcess(GetCurrentProcess(), 142);
    return 0;
}
void platform_watchdog_start(unsigned seconds, const unsigned char *image) {
    guest_image = image;
    if (!seconds) return; /* 0 disables the watchdog */
    watchdog_seconds = seconds;
    HANDLE t = CreateThread(NULL, 0, watchdog_main, NULL, 0, NULL);
    if (t) CloseHandle(t);
}
void platform_enter_guest_main(void *entry, void *arg0, void *arg1) {
    /* The process stack: 16 MiB reserved and committed by the linker (--stack 16777216,16777216),
     * below 1 TiB without high-entropy ASLR; guest code has no stack probes. */
    typedef void (__attribute__((sysv_abi)) *Entry)(void *, void *);
    ((Entry)entry)(arg0, arg1);
}
void platform_restart(void) {
    /* The launcher sets BB_RESTART_COMMAND: its own command line, started again (new patches). */
    const char *command = getenv("BB_RESTART_COMMAND");
    puts("Runtime: restarting through the launcher");
    if (!command || !*command) { fputs("runtime_restart: BB_RESTART_COMMAND is not set (started without the launcher); exiting instead\n", stderr); _exit(1); }
    char *line = _strdup(command);
    /* Same output handles: a launcher reading the game's log keeps reading the new launch. */
    STARTUPINFOA startup = {.cb = sizeof(startup), .dwFlags = STARTF_USESTDHANDLES,
                            .hStdInput = GetStdHandle(STD_INPUT_HANDLE), .hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE), .hStdError = GetStdHandle(STD_ERROR_HANDLE)};
    PROCESS_INFORMATION process;
    if (!line || !CreateProcessA(NULL, line, NULL, NULL, TRUE, 0, NULL, NULL, &startup, &process)) {
        fprintf(stderr, "runtime_restart: CreateProcess failed (%lu)\n", GetLastError()); _exit(1);
    }
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    /* The new launch waits for this process's GPU device to go away (launcher --after PID). */
    TerminateProcess(GetCurrentProcess(), 0);
    _exit(0);
}
void platform_process_init(void) {
    /* As the GoW3R port: 1 ms scheduler tick for every wait of the process, above-normal
     * priority, and no execution-speed throttling (EcoQoS would park the game on efficiency
     * cores or low clocks on a laptop when its window is not in front). */
    timeBeginPeriod(1);
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
    PROCESS_POWER_THROTTLING_STATE throttling = {.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION,
                                                 .ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED, .StateMask = 0};
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttling, sizeof(throttling));
    /* Heap objects handed to the guest come from the CRT heap, which lies below 4 GiB without
     * high-entropy ASLR (the 40-bit pointer packing of the guest is satisfied). Checked once. */
    void *probe = malloc(16);
    if ((uintptr_t)probe >= (UINT64_C(1) << 40)) { fprintf(stderr, "STOP: the heap is above 1 TiB (%p): the executable was linked with high-entropy ASLR\n", probe); exit(21); }
    free(probe);
}
#endif
