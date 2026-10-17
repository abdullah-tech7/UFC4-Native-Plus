// SPDX-License-Identifier: GPL-2.0-or-later
// SacredFire (Windows): implementations of bb_win_compat.h (the only GPU-library file that
// includes <windows.h> besides the vendored shadPS4 Windows paths).
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <io.h>
#include "bb_win_compat.h"

extern "C" {

int bb_win_gettid(void) { return static_cast<int>(GetCurrentThreadId()); }
int bb_win_getpid(void) { return static_cast<int>(GetCurrentProcessId()); }
long bb_win_sysconf(int name) {
    if (name == _SC_PAGESIZE) { SYSTEM_INFO info; GetSystemInfo(&info); return static_cast<long>(info.dwPageSize); }
    return -1;
}
ssize_t bb_win_process_vm_readv(int pid, const struct iovec* local, unsigned long liovcnt, const struct iovec* remote, unsigned long riovcnt, unsigned long flags) {
    (void)pid; (void)flags;
    if (liovcnt != 1 || riovcnt != 1) return -1;
    SIZE_T got = 0;
    const size_t n = local->iov_len < remote->iov_len ? local->iov_len : remote->iov_len;
    if (!ReadProcessMemory(GetCurrentProcess(), remote->iov_base, local->iov_base, n, &got)) return got ? static_cast<ssize_t>(got) : -1;
    return static_cast<ssize_t>(got);
}
static DWORD win_prot(int prot) {
    if (prot & PROT_EXEC) return (prot & PROT_WRITE) ? PAGE_EXECUTE_READWRITE : (prot & PROT_READ) ? PAGE_EXECUTE_READ : PAGE_EXECUTE;
    if (prot & PROT_WRITE) return PAGE_READWRITE;
    return (prot & PROT_READ) ? PAGE_READONLY : PAGE_NOACCESS;
}
int bb_win_mprotect(void* address, size_t size, int prot) {
    DWORD old;
    if (!VirtualProtect(address, size, win_prot(prot), &old)) return -1;
    if (prot & PROT_EXEC) FlushInstructionCache(GetCurrentProcess(), address, size);
    return 0;
}
// Anonymous private mappings only; MAP_FIXED_NOREPLACE places exactly at `address` or fails.
void* bb_win_mmap(void* address, size_t size, int prot, int flags, int fd, int64_t offset) {
    if (fd != -1 || offset != 0 || !(flags & MAP_ANONYMOUS)) return MAP_FAILED;
    void* p = VirtualAlloc((flags & MAP_FIXED_NOREPLACE) ? address : nullptr, size, MEM_RESERVE | MEM_COMMIT, win_prot(prot));
    return p ? p : MAP_FAILED;
}
int bb_win_munmap(void* address, size_t size) { (void)size; return VirtualFree(address, 0, MEM_RELEASE) ? 0 : -1; }
int bb_win_getrusage(int who, struct rusage* usage) {
    FILETIME created, exited, kernel, user;
    const BOOL ok = who == RUSAGE_THREAD ? GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)
                                         : GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    if (!ok) return -1;
    const auto us = [](FILETIME t) { return ((static_cast<unsigned long long>(t.dwHighDateTime) << 32) | t.dwLowDateTime) / 10; };
    std::memset(usage, 0, sizeof(*usage));
    usage->ru_utime.tv_sec = static_cast<long>(us(user) / 1000000); usage->ru_utime.tv_usec = static_cast<long>(us(user) % 1000000);
    usage->ru_stime.tv_sec = static_cast<long>(us(kernel) / 1000000); usage->ru_stime.tv_usec = static_cast<long>(us(kernel) % 1000000);
    return 0;
}
int bb_win_dladdr(const void* address, Dl_info* info) {
    static thread_local char name[MAX_PATH];
    HMODULE module = nullptr;
    std::memset(info, 0, sizeof(*info));
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(address), &module) || !module) return 0;
    GetModuleFileNameA(module, name, sizeof(name));
    info->dli_fname = name;
    info->dli_fbase = module;
    return 1;
}
int bb_win_backtrace(void** frames, int size) { return static_cast<int>(CaptureStackBackTrace(0, static_cast<DWORD>(size), frames, nullptr)); }
void bb_win_backtrace_symbols_fd(void* const* frames, int size, int fd) {
    for (int i = 0; i < size; ++i) {
        Dl_info info;
        char line[MAX_PATH + 32];
        int n;
        if (bb_win_dladdr(frames[i], &info)) {
            const char* base = std::strrchr(info.dli_fname, '\\');
            n = std::snprintf(line, sizeof(line), "%s+0x%llx\n", base ? base + 1 : info.dli_fname,
                              static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(frames[i]) - reinterpret_cast<uintptr_t>(info.dli_fbase)));
        } else {
            n = std::snprintf(line, sizeof(line), "%p\n", frames[i]);
        }
        if (n > 0) _write(fd, line, static_cast<unsigned>(n));
    }
}
int bb_win_setenv(const char* name, const char* value, int overwrite) {
    if (!overwrite && std::getenv(name)) return 0;
    return _putenv_s(name, value) ? -1 : 0;
}
void* bb_win_aligned_alloc(size_t alignment, size_t size) { return _aligned_malloc(size, alignment); }
void bb_win_aligned_free(void* p) { _aligned_free(p); }
char* bb_win_strcasestr(const char* haystack, const char* needle) {
    const size_t n = std::strlen(needle);
    for (const char* p = haystack; *p; ++p) {
        size_t i = 0;
        while (i < n && p[i] && std::tolower(static_cast<unsigned char>(p[i])) == std::tolower(static_cast<unsigned char>(needle[i]))) ++i;
        if (i == n) return const_cast<char*>(p);
    }
    return n ? nullptr : const_cast<char*>(haystack);
}

} // extern "C"
#endif
