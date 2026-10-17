// SPDX-License-Identifier: GPL-2.0-or-later
// SacredFire (Windows): the POSIX names the bbport parts of the GPU library use for diagnostics
// and thread identity, so those files compile unchanged on Windows. Force-included into every
// GPU translation unit on Windows only (gpu/CMakeLists.txt); kept free of <windows.h> so its
// macros reach no translation unit (the implementations are in bb_win_compat.cpp).
// Anything that needs a Linux kernel facility (userfaultfd, SIGTRAP single-stepping, dma-buf,
// /proc) stays behind `#ifdef __linux__` in its own file and reports itself as unavailable.
#pragma once
#ifdef _WIN32
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif
// <unistd.h>, <sys/syscall.h>
int bb_win_gettid(void);
int bb_win_getpid(void);
long bb_win_sysconf(int name);
#define _SC_PAGESIZE 30
#define gettid bb_win_gettid
#define getpid bb_win_getpid
#define sysconf bb_win_sysconf
// <sys/uio.h> process_vm_readv: the one use is reading this process's memory without faulting.
struct iovec { void* iov_base; size_t iov_len; };
ssize_t bb_win_process_vm_readv(int pid, const struct iovec* local, unsigned long liovcnt, const struct iovec* remote, unsigned long riovcnt, unsigned long flags);
#define process_vm_readv bb_win_process_vm_readv
// <sys/mman.h>: protection changes and anonymous mappings (gnm hook stubs near the image).
#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define MAP_PRIVATE 2
#define MAP_ANONYMOUS 0x20
#define MAP_FIXED_NOREPLACE 0x100000
#define MAP_FAILED ((void*)-1)
int bb_win_mprotect(void* address, size_t size, int prot);
void* bb_win_mmap(void* address, size_t size, int prot, int flags, int fd, int64_t offset);
int bb_win_munmap(void* address, size_t size);
#define mprotect bb_win_mprotect
#define mmap bb_win_mmap
#define munmap bb_win_munmap
// <sys/resource.h> getrusage: CPU times (and no fault or context-switch counts: they read 0).
struct bb_timeval { long tv_sec; long tv_usec; };
struct rusage { struct bb_timeval ru_utime, ru_stime; long ru_minflt, ru_majflt, ru_nvcsw, ru_nivcsw; };
#define RUSAGE_SELF 0
#define RUSAGE_THREAD 1
int bb_win_getrusage(int who, struct rusage* usage);
#define getrusage bb_win_getrusage
// <dlfcn.h> dladdr: module name and base for an address (no symbol names on Windows).
typedef struct { const char* dli_fname; void* dli_fbase; const char* dli_sname; void* dli_saddr; } Dl_info;
int bb_win_dladdr(const void* address, Dl_info* info);
#define dladdr bb_win_dladdr
// <execinfo.h>
int bb_win_backtrace(void** frames, int size);
void bb_win_backtrace_symbols_fd(void* const* frames, int size, int fd);
#define backtrace bb_win_backtrace
#define backtrace_symbols_fd bb_win_backtrace_symbols_fd
// <stdlib.h> setenv (the CRT has _putenv_s); page-aligned allocations (UCRT has no aligned_alloc).
int bb_win_setenv(const char* name, const char* value, int overwrite);
#define setenv bb_win_setenv
void* bb_win_aligned_alloc(size_t alignment, size_t size);
void bb_win_aligned_free(void* p);
// <strings.h>
char* bb_win_strcasestr(const char* haystack, const char* needle);
#define strcasestr bb_win_strcasestr
// pthread_getcpuclockid: winpthreads declares it; CPU-time clocks per thread are read through
// GetThreadTimes by bb_win_thread_cpu_ns when a clock id of a thread is asked for.
#ifdef __cplusplus
}
#endif
#endif
