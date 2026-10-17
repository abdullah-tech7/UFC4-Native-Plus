/* SacredFire: the host services the runtime and loader need, with a Linux and a Windows
 * implementation (platform_posix.c, platform_win.c; docs/sacredfire/13-windows-port.md).
 * Guest-facing logic stays in src/runtime_*.c; this is the only place that knows the host OS.
 * Nothing here is guest-visible: guest layouts and error numbers belong to the runtime files. */
#ifndef BB_PLATFORM_H
#define BB_PLATFORM_H
#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include <pthread.h>
#include <sys/types.h> /* ssize_t */
#ifdef _WIN32
/* Page protection bits as POSIX names them; mapped to PAGE_* in platform_win.c. */
#ifndef PROT_NONE
#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#endif
#ifndef CLOCK_REALTIME_COARSE
#define CLOCK_REALTIME_COARSE CLOCK_REALTIME
#endif
#ifndef PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
#define PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP PTHREAD_RECURSIVE_MUTEX_INITIALIZER /* winpthreads' spelling */
#endif
/* Recovery point for faults in speculative guest reads: setjmp/longjmp that saves every Win64
 * callee-saved register (rbx rbp rdi rsi r12-r15, xmm6-15, MXCSR, x87 control word) and does
 * no SEH unwinding: guest frames have no unwind data, and the vectored handler enters
 * bb_longjmp directly by rewriting the faulting thread's context. */
typedef unsigned long long bb_jmp_buf[32];
#ifdef __cplusplus
extern "C" {
#endif
__attribute__((returns_twice)) int bb_setjmp(bb_jmp_buf buffer);
__attribute__((noreturn)) void bb_longjmp(bb_jmp_buf buffer, int value);
#ifdef __cplusplus
}
#endif
typedef bb_jmp_buf bb_fault_jmp_buf;
#define bb_fault_setjmp(buffer) bb_setjmp(buffer)
#else
#include <setjmp.h>
#include <sys/mman.h>
typedef sigjmp_buf bb_fault_jmp_buf;
#define bb_fault_setjmp(buffer) sigsetjmp(buffer, 0)
#endif
#ifdef __cplusplus
extern "C" {
#endif

/* ---- virtual memory (host side; the guest pool backend is below) ---- */
size_t platform_page_size(void);
/* Anonymous memory exactly at `address` (a multiple of the allocation granularity, 64 KiB on
 * Windows), committed, or NULL when the range is taken. */
void *platform_map_fixed(uintptr_t address, size_t size, int prot);
/* Anonymous memory anywhere, or NULL. */
void *platform_map_anonymous(size_t size, int prot);
void platform_unmap(void *address, size_t size);
/* 0 on success. Executable pages get their instruction cache flushed on Windows. */
int platform_protect(void *address, size_t size, int prot);
/* Memory handed to the guest must be allocated 64-byte aligned; freed with platform_aligned_free. */
void *platform_aligned_alloc(size_t alignment, size_t size);
void platform_aligned_free(void *p);

/* ---- guest memory pool (direct + flexible memory): one sparse backing object, views of it at
 * guest addresses. Linux: memfd + mmap (runtime_memory.c keeps the dma-buf chunk code); Windows:
 * a pagefile-backed section and the PS4 user range reserved as a placeholder (platform_win.c). */
#ifdef _WIN32
/* Reserves [lo, hi) so no host allocation lands there; call first thing in main. Exits on failure. */
void platform_pool_reserve_range(uintptr_t lo, uintptr_t hi);
/* Creates the section of `bytes` and a host-private view of all of it (*backing). 0 or -1. */
int platform_pool_create(uint64_t bytes, unsigned char **backing);
/* A view of pool offset `phys` at `address` (prot bits), or a reserved (no access, no storage)
 * range when `reserved`. The range must lie inside the reserved user range. 0 or -1. */
int platform_pool_map(uintptr_t address, uint64_t size, int prot, int reserved, uint64_t phys);
/* Back to a placeholder; views partly inside the range are split and the parts outside kept. */
int platform_pool_unmap(uintptr_t address, uint64_t size);
int platform_pool_protect(uintptr_t address, uint64_t size, int prot);
/* Released pool pages read as zero again (the memfd punches a hole; the section is cleared). */
void platform_pool_discard(unsigned char *backing, uint64_t phys, uint64_t size);
#endif

/* ---- threads ---- */
/* Makes `tcb` the guest TCB of the calling thread: what `mov rax, gs:[0]` (as prepared by
 * link_modules.py, patched on Windows) loads. Exits with a message on failure. */
void platform_thread_publish_tcb(void *tcb);
#ifdef _WIN32
/* The TEB displacement guest TCB loads are patched to (0x1480 + 8*slot). Claims the slot on the
 * first call; must run before any DLL (Vulkan drivers) can take the low 64 slots. */
uint32_t platform_thread_tcb_displacement(void);
/* Patches every `mov rax, gs:[0]` in the executable segments of the image to the TCB slot after
 * decoding each site with Zydis; the count must match `expected` (link.json fs_loads_patched)
 * or the loader stops. Returns the count. */
uint64_t platform_patch_guest_tls_loads(unsigned char *image, const uint64_t *segment_address,
                                        const uint64_t *segment_size, const uint64_t *segment_flags,
                                        uint64_t segments, int64_t expected);
#endif
/* Sets the host thread's stack for pthread_create: `bytes` of it, fully committed, below 1 TiB
 * (guest code packs pointers into 40 bits and has no stack probes). */
void platform_thread_stack(pthread_attr_t *attr, size_t bytes);
uint32_t platform_thread_id(void);
/* Names the calling host thread (debugger, thread dumps); truncated as the host requires. */
void platform_thread_set_name(const char *name);
/* The calling thread's name into `out` (at least 16 bytes), "?" when unknown. */
void platform_thread_get_name(char *out, size_t size);
/* The highest address of the calling thread's stack (for scanning it for return addresses). */
uintptr_t platform_thread_stack_top(void);
int platform_current_cpu(void);

/* ---- time ---- */
uint64_t platform_monotonic_ns(void);
uint64_t platform_realtime_ns(void);
/* The clock the pthread condition variables of this host can wait on with a deadline
 * (CLOCK_MONOTONIC on Linux; CLOCK_REALTIME on winpthreads, which rejects monotonic). */
clockid_t platform_wait_clock(void);
/* A condition variable whose timed waits take deadlines on platform_wait_clock(). */
void platform_cond_init_waitclock(pthread_cond_t *cond);
/* Sleeps `ns` (sub-millisecond on Windows through a high-resolution waitable timer); 0 on
 * success, else an errno (EINTR when interrupted on Linux, the caller retries). */
int platform_sleep_ns(uint64_t ns);
/* Sleeps until platform_monotonic_ns() reaches `deadline_ns`. */
void platform_sleep_until_ns(uint64_t deadline_ns);
/* Seconds east of UTC at `t`, with daylight saving (struct tm has no tm_gmtoff on Windows). */
long platform_utc_offset(time_t t);
/* 1 when daylight saving time is in effect at `t`. */
int platform_is_dst(time_t t);
/* Cryptographic random bytes; 0 or -1. */
int platform_random(void *buffer, size_t size);
/* User and kernel CPU time in microseconds of the process (who 0) or calling thread (who 1). */
int platform_cpu_times(int who, int64_t *user_us, int64_t *kernel_us);

/* ---- files (paths are UTF-8; on Windows the executable's manifest selects the UTF-8 code page) ---- */
typedef struct {
    int64_t size, blocks;
    uint32_t mode, blksize, dev, ino, nlink; /* mode: POSIX bits (S_IFDIR 0040000, S_IFREG 0100000) */
    struct timespec atim, mtim, ctim;
    int is_dir, is_reg, is_lnk;
} PlatformStat;
int platform_stat(const char *path, PlatformStat *out);   /* 0 or -errno */
int platform_fstat(int fd, PlatformStat *out);            /* 0 or -errno */
/* Guest open flags (FreeBSD bits: 3 access, 0x4 nonblock, 0x8 append, 0x80 sync, 0x200 creat,
 * 0x400 trunc, 0x800 excl, 0x20000 directory) to a host descriptor; a directory may be opened
 * read-only (Windows gives it a listing-only descriptor, platform_fd_is_dir). Returns the
 * descriptor or -errno. */
int platform_open(const char *path, int guest_flags, int mode);
int platform_close(int fd);
ssize_t platform_read(int fd, void *buffer, size_t size);      /* -1 and errno as read() */
ssize_t platform_write(int fd, const void *buffer, size_t size);
ssize_t platform_pread(int fd, void *buffer, size_t size, int64_t offset);
ssize_t platform_pwrite(int fd, const void *buffer, size_t size, int64_t offset);
int64_t platform_lseek(int fd, int64_t offset, int whence);   /* -1 and errno */
int platform_fsync(int fd);                                   /* 0 or -errno */
int platform_ftruncate(int fd, int64_t length);               /* 0 or -errno */
int platform_truncate(const char *path, int64_t length);      /* 0 or -errno */
/* POSIX rename: replaces an existing destination file. 0 or -errno. */
int platform_rename(const char *from, const char *to);
int platform_unlink(const char *path);                        /* 0 or -errno */
int platform_mkdir(const char *path, int mode);               /* 0 or -errno (EEXIST when present) */
int platform_rmdir(const char *path);                         /* 0 or -errno */
/* mode bits: 4 read, 2 write, 1 execute (ignored on Windows). 0 or -errno. */
int platform_access(const char *path, int mode);
/* Directory listing: entries other than "." and "..", each name NUL-terminated in `names`,
 * `types` as FreeBSD d_type (4 directory, 8 regular file, 10 link, 0 unknown). */
typedef struct { char *names; size_t count, *offsets; unsigned char *types; } PlatformListing;
PlatformListing *platform_list_directory(const char *path); /* NULL and errno when it fails */
void platform_free_listing(PlatformListing *listing);
/* Removes a directory tree. Refuses (returns -1, nothing removed) unless `path` lies inside
 * `root`: the caller names the directory it owns. 0 when gone or never there. */
int platform_remove_tree(const char *root, const char *path);
/* Canonical absolute path into `out` (PATH_MAX bytes), or NULL. */
char *platform_realpath(const char *path, char *out);
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* ---- process and faults ---- */
/* Copies `size` bytes from `address` without faulting; 1 when all were readable. */
int platform_safe_read(uintptr_t address, void *out, size_t size);
/* "module+0xoffset (symbol)" for a host address, or the bare address. */
void platform_describe_address(uintptr_t address, char *out, size_t size);
/* The host call chain (frames with unwind data) into `frames`; returns the count. */
int platform_backtrace(void **frames, int max);
/* Fault handling. `gpu` resolves GPU page-tracking faults (context, address) and returns 1 when
 * handled; `recover` returns the thread's recovery point or NULL. The handler then reports a
 * fatal fault (RIP described, registers, rbp chain) and ends the process with 128+signal (Linux)
 * or 3 (Windows). `image`/`image_size`: the guest image, for "guest offset" reports. */
typedef int (*PlatformGpuFault)(void *context, void *address);
typedef bb_fault_jmp_buf *(*PlatformRecoverPoint)(void);
typedef void (*PlatformFaultExtra)(void *context);
void platform_install_fault_handlers(PlatformGpuFault gpu, PlatformRecoverPoint recover,
                                     PlatformFaultExtra extra, const unsigned char *image, uint64_t image_size);
/* ufc3 (BB_WRITE_STEP): the handler of single-step traps (Linux SIGTRAP, Windows
 * EXCEPTION_SINGLE_STEP); it returns nonzero for its own steps, others go on as before. */
typedef int (*PlatformStepHandler)(void *context);
void platform_install_step_handler(PlatformStepHandler handler);
/* Watchdog: after `seconds` (0: never) dumps every thread's RIP and rbp chain and exits 142. */
void platform_watchdog_start(unsigned seconds, const unsigned char *image);
/* Runs the guest's main entry: on Linux on a fresh low-address stack, on Windows on the process
 * stack (16 MiB, committed by the linker). entry(arg0, arg1) with the SysV ABI. */
void platform_enter_guest_main(void *entry, void *arg0, void *arg1);
/* Restarts the game: Linux re-executes run.sh; Windows runs BB_RESTART_COMMAND (set by the
 * launcher) and ends this process. Does not return. */
__attribute__((noreturn)) void platform_restart(void);
/* Host tuning before anything else runs (Linux: glibc heap below 1 TiB; Windows: nothing). */
void platform_process_init(void);
/* errno for the last host error (Windows: GetLastError mapped; Linux: errno). */
int platform_last_errno(void);
#ifdef __cplusplus
}
#endif
#endif
