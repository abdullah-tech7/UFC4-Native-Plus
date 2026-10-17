/* SacredFire: Linux implementation of platform.h. Behaviour-preserving: each function does what
 * the runtime files did inline before the Windows port (docs/sacredfire/13-windows-port.md). */
#define _GNU_SOURCE
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <ftw.h>
#include <limits.h>
#include <malloc.h>
#include <sched.h>
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <asm/prctl.h>

/* ---- memory ---- */
size_t platform_page_size(void) { static size_t size; if (!size) size = (size_t)sysconf(_SC_PAGESIZE); return size; }
void *platform_map_fixed(uintptr_t address, size_t size, int prot) {
    void *p = mmap((void *)address, size, prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}
void *platform_map_anonymous(size_t size, int prot) {
    void *p = mmap(NULL, size, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}
void platform_unmap(void *address, size_t size) { munmap(address, size); }
int platform_protect(void *address, size_t size, int prot) { return mprotect(address, size, prot); }
void *platform_aligned_alloc(size_t alignment, size_t size) { return aligned_alloc(alignment, (size + alignment - 1) & ~(alignment - 1)); }
void platform_aligned_free(void *p) { free(p); }

/* ---- threads ---- */
void platform_thread_publish_tcb(void *tcb) {
    if (syscall(SYS_arch_prctl, ARCH_SET_GS, (unsigned long)tcb)) { perror("STOP: arch_prctl(ARCH_SET_GS)"); exit(21); }
}
/* runtime_memory.c: below 1 TiB. Weak: the file tests link without the memory runtime. */
void *runtime_low_map(size_t size, int prot) __attribute__((weak));
void platform_thread_stack(pthread_attr_t *attr, size_t bytes) {
    /* Stacks below 1 TiB as on PS4; guest code may pack stack addresses. */
    void *memory = runtime_low_map ? runtime_low_map(bytes, PROT_READ | PROT_WRITE) : NULL;
    if (memory) pthread_attr_setstack(attr, memory, bytes);
    else pthread_attr_setstacksize(attr, bytes);
}
uint32_t platform_thread_id(void) { return (uint32_t)syscall(SYS_gettid); }
void platform_thread_set_name(const char *name) {
    /* Linux thread names hold 15 characters; longer ones would be rejected. */
    char host[16] = {0};
    memcpy(host, name, strnlen(name, sizeof(host) - 1));
    pthread_setname_np(pthread_self(), host);
}
void platform_thread_get_name(char *out, size_t size) {
    if (size) { out[0] = '?'; out[size > 1 ? 1 : 0] = 0; }
    if (size >= 16) pthread_getname_np(pthread_self(), out, size);
}
uintptr_t platform_thread_stack_top(void) {
    pthread_attr_t attr; void *base = NULL; size_t size = 0;
    if (pthread_getattr_np(pthread_self(), &attr)) return 0;
    pthread_attr_getstack(&attr, &base, &size); pthread_attr_destroy(&attr);
    return (uintptr_t)base + size;
}
int platform_current_cpu(void) { int c = sched_getcpu(); return c < 0 ? 0 : c; }

/* ---- time ---- */
static uint64_t ns_of(clockid_t id) { struct timespec t; clock_gettime(id, &t); return (uint64_t)t.tv_sec * 1000000000 + (uint64_t)t.tv_nsec; }
uint64_t platform_monotonic_ns(void) { return ns_of(CLOCK_MONOTONIC); }
uint64_t platform_realtime_ns(void) { return ns_of(CLOCK_REALTIME); }
clockid_t platform_wait_clock(void) { return CLOCK_MONOTONIC; }
void platform_cond_init_waitclock(pthread_cond_t *cond) {
    pthread_condattr_t a; pthread_condattr_init(&a); pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init(cond, &a); pthread_condattr_destroy(&a);
}
int platform_sleep_ns(uint64_t ns) {
    struct timespec t = {.tv_sec = (time_t)(ns / 1000000000), .tv_nsec = (long)(ns % 1000000000)};
    while (nanosleep(&t, &t)) if (errno != EINTR) return errno;
    return 0;
}
void platform_sleep_until_ns(uint64_t deadline_ns) {
    struct timespec t = {(time_t)(deadline_ns / 1000000000u), (long)(deadline_ns % 1000000000u)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL)) {}
}
long platform_utc_offset(time_t t) { struct tm local; if (!localtime_r(&t, &local)) return 0; return local.tm_gmtoff; }
int platform_is_dst(time_t t) { struct tm local; if (!localtime_r(&t, &local)) return 0; return local.tm_isdst > 0; }
int platform_random(void *buffer, size_t size) {
    unsigned char *p = buffer;
    while (size) {
        ssize_t n = getrandom(p, size, 0);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        p += n; size -= (size_t)n;
    }
    return 0;
}
int platform_cpu_times(int who, int64_t *user_us, int64_t *kernel_us) {
    struct rusage r;
    if (getrusage(who ? RUSAGE_THREAD : RUSAGE_SELF, &r)) return -1;
    *user_us = (int64_t)r.ru_utime.tv_sec * 1000000 + r.ru_utime.tv_usec;
    *kernel_us = (int64_t)r.ru_stime.tv_sec * 1000000 + r.ru_stime.tv_usec;
    return 0;
}

/* ---- files ---- */
static void convert(const struct stat *s, PlatformStat *g) {
    memset(g, 0, sizeof(*g));
    g->size = s->st_size; g->blocks = s->st_blocks; g->blksize = (uint32_t)s->st_blksize;
    g->mode = (uint32_t)s->st_mode; g->dev = (uint32_t)s->st_dev; g->ino = (uint32_t)s->st_ino; g->nlink = (uint32_t)s->st_nlink;
    g->atim = s->st_atim; g->mtim = s->st_mtim; g->ctim = s->st_ctim;
    g->is_dir = S_ISDIR(s->st_mode); g->is_reg = S_ISREG(s->st_mode); g->is_lnk = S_ISLNK(s->st_mode);
}
int platform_stat(const char *path, PlatformStat *out) { struct stat s; if (stat(path, &s)) return -errno; convert(&s, out); return 0; }
int platform_fstat(int fd, PlatformStat *out) { struct stat s; if (fstat(fd, &s)) return -errno; convert(&s, out); return 0; }
int platform_open(const char *path, int flags, int mode) {
    int r;
    switch (flags & 3) { case 0: r = O_RDONLY; break; case 1: r = O_WRONLY; break; default: r = O_RDWR; }
    if (flags & 0x4) r |= O_NONBLOCK;
    if (flags & 0x8) r |= O_APPEND;
    if (flags & 0x80) r |= O_SYNC;
    if (flags & 0x200) r |= O_CREAT;
    if (flags & 0x400) r |= O_TRUNC;
    if (flags & 0x800) r |= O_EXCL;
    if (flags & 0x20000) r |= O_DIRECTORY;
    int fd = open(path, r | O_CLOEXEC, mode ? mode : 0644);
    return fd < 0 ? -errno : fd;
}
int platform_close(int fd) { return close(fd) ? -errno : 0; }
ssize_t platform_read(int fd, void *buffer, size_t size) { return read(fd, buffer, size); }
ssize_t platform_write(int fd, const void *buffer, size_t size) { return write(fd, buffer, size); }
ssize_t platform_pread(int fd, void *buffer, size_t size, int64_t offset) { return pread(fd, buffer, size, offset); }
ssize_t platform_pwrite(int fd, const void *buffer, size_t size, int64_t offset) { return pwrite(fd, buffer, size, offset); }
int64_t platform_lseek(int fd, int64_t offset, int whence) { return lseek(fd, offset, whence); }
int platform_fsync(int fd) { return fsync(fd) ? -errno : 0; }
int platform_ftruncate(int fd, int64_t length) { return ftruncate(fd, length) ? -errno : 0; }
int platform_truncate(const char *path, int64_t length) { return truncate(path, length) ? -errno : 0; }
int platform_rename(const char *from, const char *to) { return rename(from, to) ? -errno : 0; }
int platform_unlink(const char *path) { return unlink(path) ? -errno : 0; }
int platform_mkdir(const char *path, int mode) { return mkdir(path, mode ? (mode_t)mode : 0755) ? -errno : 0; }
int platform_rmdir(const char *path) { return rmdir(path) ? -errno : 0; }
int platform_access(const char *path, int mode) { return access(path, mode & 7) ? -errno : 0; }
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
        unsigned char type = e->d_type;
        if (type == DT_LNK || type == DT_UNKNOWN) {
            struct stat entry;
            if (!fstatat(dirfd(d), e->d_name, &entry, 0))
                type = S_ISDIR(entry.st_mode) ? DT_DIR : S_ISREG(entry.st_mode) ? DT_REG : type;
        }
        l->types[l->count] = type == DT_DIR ? 4 : type == DT_REG ? 8 : type == DT_LNK ? 10 : 0;
        ++l->count; bytes += n;
    }
    closedir(d);
    return l;
}
void platform_free_listing(PlatformListing *l) { if (l) { free(l->names); free(l->offsets); free(l->types); free(l); } }
static int remove_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw) {
    (void)st; (void)flag; (void)ftw; return remove(path);
}
int platform_remove_tree(const char *root, const char *path) {
    size_t n = strlen(root);
    if (!n || strncmp(path, root, n) || (path[n] != '/' && path[n] != 0) || !path[n]) return -1; /* outside the owned root, or the root itself */
    struct stat st;
    if (lstat(path, &st)) return 0;
    return nftw(path, remove_entry, 16, FTW_DEPTH | FTW_PHYS) ? -1 : 0;
}
char *platform_realpath(const char *path, char *out) { return realpath(path, out); }

/* ---- process and faults ---- */
int platform_safe_read(uintptr_t address, void *out, size_t size) {
    struct iovec local = {out, size}, remote = {(void *)address, size};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == (ssize_t)size;
}
void platform_describe_address(uintptr_t address, char *out, size_t size) {
    Dl_info where;
    if (dladdr((void *)address, &where) && where.dli_fname)
        snprintf(out, size, "%s+0x%lx (%s)", where.dli_fname, (unsigned long)(address - (uintptr_t)where.dli_fbase), where.dli_sname ? where.dli_sname : "?");
    else snprintf(out, size, "%p", (void *)address);
}
int platform_backtrace(void **frames, int max) { return backtrace(frames, max); }

static PlatformGpuFault gpu_fault;
static PlatformRecoverPoint recover_point;
static PlatformFaultExtra fault_extra;
static const unsigned char *guest_image;
static uint64_t guest_image_size;
static uintptr_t exe_base;
static void write_all(const char *text) { ssize_t written = write(2, text, strlen(text)); (void)written; }
static void fault(int sig, siginfo_t *info, void *context) {
    /* GPU page tracking (write-protected guest pages) is resolved first. */
    if (sig == SIGSEGV && gpu_fault && gpu_fault(context, info->si_addr)) return;
    /* A speculative guest memory read failed: resume its recovery point. */
    bb_fault_jmp_buf *recover = recover_point ? recover_point() : NULL;
    if ((sig == SIGSEGV || sig == SIGBUS) && recover) {
        sigset_t unblock;
        sigemptyset(&unblock); sigaddset(&unblock, sig);
        pthread_sigmask(SIG_UNBLOCK, &unblock, NULL);
        siglongjmp(*recover, 1);
    }
    /* The process is terminating: dladdr/snprintf are acceptable here. */
    ucontext_t *uc = context;
    uintptr_t rip = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
    char line[512], where[256];
    if (rip - (uintptr_t)guest_image < guest_image_size)
        snprintf(line, sizeof(line), "Guest fault (signal %d) at guest offset 0x%lx, address %p\n", sig, (unsigned long)(rip - (uintptr_t)guest_image), info->si_addr);
    else { platform_describe_address(rip, where, sizeof(where)); snprintf(line, sizeof(line), "Host fault (signal %d) in %s, address %p\n", sig, where, info->si_addr); }
    write_all(line);
    if (fault_extra) fault_extra(context);
    Dl_info dl;
    if (rip - (uintptr_t)guest_image >= guest_image_size && !(dladdr((void *)rip, &dl) && dl.dli_fname)) {
        /* Outside the image and any shared object (generated code, a freed mapping): the mapping
         * from /proc/self/maps, and the thread. */
        char thread[32] = "?";
        pthread_getname_np(pthread_self(), thread, sizeof(thread));
        snprintf(line, sizeof(line), "  thread %s; mapping of RIP: ", thread); write_all(line);
        FILE *maps = fopen("/proc/self/maps", "r");
        int found = 0;
        while (maps && fgets(line, sizeof(line), maps)) {
            unsigned long from, to;
            if (sscanf(line, "%lx-%lx", &from, &to) == 2 && rip >= from && rip < to) { write_all(line); found = 1; break; }
        }
        if (maps) fclose(maps);
        if (!found) write_all("none\n");
    }
    /* Host call chain (frames with unwind info; guest frames end it). */
    void *frames[32];
    int depth = backtrace(frames, 32);
    for (int i = 2; i < depth; ++i) {
        platform_describe_address((uintptr_t)frames[i], where, sizeof(where));
        snprintf(line, sizeof(line), "  #%d %s\n", i, where); write_all(line);
    }
    _exit(128 + sig);
}
static PlatformStepHandler step_handler;
static struct sigaction previous_trap;
static void step_trap(int sig, siginfo_t *info, void *context) {
    if (step_handler && step_handler(context)) return;
    if ((previous_trap.sa_flags & SA_SIGINFO) && previous_trap.sa_sigaction) { previous_trap.sa_sigaction(sig, info, context); return; }
    if (!(previous_trap.sa_flags & SA_SIGINFO) && previous_trap.sa_handler != SIG_DFL && previous_trap.sa_handler != SIG_IGN && previous_trap.sa_handler) { previous_trap.sa_handler(sig); return; }
    signal(SIGTRAP, SIG_DFL);
    raise(SIGTRAP);
}
void platform_install_step_handler(PlatformStepHandler handler) {
    if (step_handler) { step_handler = handler; return; }
    step_handler = handler;
    struct sigaction sa = {0}; sa.sa_sigaction = step_trap; sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTRAP, &sa, &previous_trap);
}
void platform_install_fault_handlers(PlatformGpuFault gpu, PlatformRecoverPoint recover, PlatformFaultExtra extra, const unsigned char *image, uint64_t image_size) {
    gpu_fault = gpu; recover_point = recover; fault_extra = extra; guest_image = image; guest_image_size = image_size;
    struct sigaction sa = {0}; sa.sa_sigaction = fault; sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGILL, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
    Dl_info self_info;
    if (dladdr((void *)platform_install_fault_handlers, &self_info)) exe_base = (uintptr_t)self_info.dli_fbase;
}
/* Watchdog: dump RIP and the rbp frame chain of every thread (guest offsets when inside the
 * image). Reads use process_vm_readv so bad frames cannot fault. */
static void write_hex(char *out, uint64_t v) {
    const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) out[i] = digits[(v >> (60 - i * 4)) & 15];
}
static void dump_frames(ucontext_t *uc) {
    char line[] = "  tid=0000000000000000 rip=0000000000000000 image-relative=0000000000000000 host-relative=0000000000000000\n";
    uintptr_t rip = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP], rbp = (uintptr_t)uc->uc_mcontext.gregs[REG_RBP];
    uint64_t tid = (uint64_t)gettid();
    /* First argument register: the lock address when a thread waits on a futex. */
    char arg[] = "  tid=0000000000000000 rdi=0000000000000000\n";
    write_hex(arg + 6, tid); write_hex(arg + 27, (uint64_t)uc->uc_mcontext.gregs[REG_RDI]);
    write_all(arg);
    for (int depth = 0; depth < 24; ++depth) {
        write_hex(line + 6, tid); write_hex(line + 27, rip); write_hex(line + 59, rip - (uintptr_t)guest_image); write_hex(line + 90, rip - exe_base);
        write_all(line);
        uintptr_t frame[2];
        if (!rbp || !platform_safe_read(rbp, frame, sizeof(frame))) break;
        if (frame[0] <= rbp) break;
        rbp = frame[0]; rip = frame[1];
    }
}
static void thread_dump(int sig, siginfo_t *info, void *context) { (void)sig; (void)info; dump_frames(context); }
static void watchdog(int sig, siginfo_t *info, void *context) {
    (void)info;
    write_all("STOP: watchdog timeout; thread stacks:\n");
    dump_frames(context);
    int dir = open("/proc/self/task", O_RDONLY | O_DIRECTORY);
    char buffer[4096];
    long n;
    pid_t self = gettid();
    while (dir >= 0 && (n = syscall(SYS_getdents64, dir, buffer, sizeof(buffer))) > 0)
        for (long at = 0; at < n;) {
            struct { uint64_t ino; int64_t off; unsigned short reclen; unsigned char type; char name[]; } *d = (void *)(buffer + at);
            pid_t tid = (pid_t)strtol(d->name, NULL, 10);
            if (tid > 0 && tid != self) { syscall(SYS_tgkill, getpid(), tid, SIGUSR2); usleep(20000); }
            at += d->reclen;
        }
    usleep(100000);
    _exit(128 + sig);
}
void platform_watchdog_start(unsigned seconds, const unsigned char *image) {
    guest_image = image;
    struct sigaction dump = {0}; dump.sa_sigaction = thread_dump; dump.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&dump.sa_mask); sigaction(SIGUSR2, &dump, NULL);
    struct sigaction alarm_action = {0}; alarm_action.sa_sigaction = watchdog; alarm_action.sa_flags = SA_SIGINFO;
    sigemptyset(&alarm_action.sa_mask); sigaction(SIGALRM, &alarm_action, NULL);
    alarm(seconds); /* 0 disables the watchdog */
}
/* enter_on_stack(entry, arg0, arg1, stack_top): call entry(arg0,arg1) on a new stack. */
void enter_on_stack(void *entry, void *arg0, void *arg1, void *top);
__asm__(".text\n.globl enter_on_stack\nenter_on_stack:\n"
        " push %rbp\n mov %rsp,%rbp\n and $-16,%rcx\n mov %rcx,%rsp\n"
        " mov %rdi,%rax\n mov %rsi,%rdi\n mov %rdx,%rsi\n call *%rax\n"
        " mov %rbp,%rsp\n pop %rbp\n ret\n");
void platform_enter_guest_main(void *entry, void *arg0, void *arg1) {
    /* The guest main thread runs on a stack below 1 TiB like PS4 stacks. */
    enum { MAIN_STACK = 8 * 1024 * 1024 };
    unsigned char *stack = runtime_low_map ? runtime_low_map(MAIN_STACK, PROT_READ | PROT_WRITE) : NULL;
    if (!stack) { fputs("ERROR: cannot allocate guest main stack\n", stderr); exit(1); }
    enter_on_stack(entry, arg0, arg1, stack + MAIN_STACK - 64);
}
void platform_restart(void) {
    puts("Runtime: restarting through run.sh");
    syscall(SYS_close_range, 3u, ~0u, 0u);
    execlp("bash", "bash", "run.sh", (char *)NULL);
    perror("runtime_restart: exec");
    _exit(1);
}
void platform_process_init(void) {
    /* Keep host heap objects handed to the guest (thread handles, TLS) in the
       non-PIE brk heap, i.e. below 1 TiB: the guest packs pointers into 40 bits. */
    mallopt(M_ARENA_MAX, 1);
    mallopt(M_MMAP_THRESHOLD, 32 * 1024 * 1024);
}
int platform_last_errno(void) { return errno; }
