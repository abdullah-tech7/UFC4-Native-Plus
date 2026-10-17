#ifndef BB_RUNTIME_H
#define BB_RUNTIME_H
#include <time.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "platform/platform.h"
/* Recovery point for speculative guest memory reads on this thread (the fault handler of
 * platform.h resumes it; sigjmp_buf on Linux, bb_jmp_buf on Windows). */
extern __thread bb_fault_jmp_buf *runtime_fault_recover;
/* Restarts the game (in-game settings menu, render resolution change). */
void runtime_restart(void);
/* The guest's functions and callbacks use the System V AMD64 calling convention on every host;
 * on Windows (Microsoft x64 ABI) the attribute makes clang emit the SysV convention for them. */
#define ABI __attribute__((sysv_abi))
typedef void (ABI *GuestCallback)(void);
void runtime_start(uint64_t capabilities);
uintptr_t runtime_resolve(const char *name, int is_data);
void runtime_report(void);
void runtime_finalize(void *dso);
uintptr_t runtime_mutex_resolve(const char *name);
void runtime_mutex_report(void);
uintptr_t runtime_memory_resolve(const char *name);
void runtime_memory_report(void);
int runtime_memory_is_mapped(uintptr_t address, uint64_t size);
/* The CPU is about to write the range outside guest code (a file read): tells the GPU side. */
void runtime_memory_note_write(uintptr_t address, uint64_t size);
void runtime_memory_note_cpu_write(uintptr_t address, uint64_t size);
/* bbport (frame stats): a guest thread was blocked `ns` in the runtime (0 cond, 1 mutex, 2 sema, 3 sleep). */
void runtime_wait_note(int kind, uint64_t ns);
void runtime_wait_report(double frames);
void runtime_guest_call_sites(uint64_t out[3]);
/* bbport: times the game's heap asked for more memory (posix_mmap): a sign it leaks. */
uint64_t runtime_heap_growths(void);
static inline uint64_t runtime_wait_clock(void) { return platform_monotonic_ns(); }
const char *runtime_import_name(const char *name);
uintptr_t runtime_rwlock_resolve(const char *name);
void runtime_rwlock_report(void);
void runtime_set_libc_tls(const void *data, uint64_t filesz, uint64_t memsz);
void runtime_set_module_tls(uint64_t module, const void *data, uint64_t filesz, uint64_t memsz);
void runtime_set_procparam(void *param);
void **runtime_application_heap_api(void);
uintptr_t runtime_thread_resolve(const char *name);
void runtime_thread_report(void);
void runtime_set_main_tls(const void *data, uint64_t filesz, uint64_t memsz, uint64_t align);
void runtime_thread_attach_main(void);
/* ufc3 online (UFC-25, runtime_fp.c): BB_DETERMINISTIC_FP (default: on with BB_ONLINE_SERVER).
 * The guest's approximate reciprocals are patched to a rule that is the same on every CPU, and
 * guest threads get the PS4's MXCSR (0x9fc0) and x87 control word (0x037f). */
int runtime_fp_deterministic(void);
void runtime_fp_thread_setup(void);
uint32_t runtime_fp_rcp(uint32_t x);
uint32_t runtime_fp_rsqrt(uint32_t x);
typedef struct { uint64_t functions, found, patched, unpatched, displaced, stub_bytes; } RuntimeFpStats;
/* Patches every (v)rcpps/(v)rsqrtps/(v)rcpss/(v)rsqrtss in the executable segments; `pointers`
 * are image offsets of code addresses stored in data (they are never displaced). `map_near`
 * gives writable memory within 2 GiB of the image; it is made executable here. */
uint64_t runtime_fp_patch_image(unsigned char *image, uint64_t size, const uint64_t *seg_address, const uint64_t *seg_size,
                                const uint64_t *seg_flags, uint64_t segments, const uint64_t *pointers, uint64_t pointer_count,
                                void *(*map_near)(size_t), RuntimeFpStats *stats);
int32_t *runtime_errno(void);
uintptr_t runtime_sema_resolve(const char *name);
uintptr_t runtime_eventflag_resolve(const char *name);
uintptr_t runtime_json2_resolve(const char *name);
void runtime_eventflag_report(void);
void runtime_sema_report(void);
unsigned runtime_sema_waiters(uint32_t id);
uintptr_t runtime_time_resolve(const char *name);
void runtime_content_configure(const uint32_t values[5]);
uintptr_t runtime_content_resolve(const char *name);
void runtime_content_report(void);
typedef struct { const char *name; void *function; } RuntimeExport;
#define RUNTIME_LOOKUP(table, nid) runtime_lookup(table, sizeof(table)/sizeof(*(table)), nid)
const char *runtime_symbol(const char *nid);
/* SacredFire: imports are named "NID#library" (link_modules.py); a pattern "NID" matches the NID from
 * any library, "NID#library" only from that library. The eboot's own index suffixes never reach here. */
static inline int runtime_nid_equals(const char *name, const char *pattern) {
    const char *h = strchr(name, '#'), *k = strchr(pattern, '#');
    size_t n = h ? (size_t)(h - name) : strlen(name), p = k ? (size_t)(k - pattern) : strlen(pattern);
    if (n != p || memcmp(name, pattern, n)) return 0;
    if (!k) return 1; /* any library */
    const char *library = h ? h + 1 : "", *want = k + 1;
    const char *end = strchr(library, '#');
    size_t l = end ? (size_t)(end - library) : strlen(library);
    return l == strlen(want) && !memcmp(library, want, l);
}
/* Loads "NID library symbol" lines generated for the prepared eboot (tools/gen_import_names.py). */
int runtime_load_import_names(const char *path);
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid);
uintptr_t runtime_kernel_resolve(const char *name);
/* SacredFire: a guest thread is ending: libc's thread destructors (_sceKernelSetThreadDtors) run first. */
void runtime_kernel_thread_exit(void);
uintptr_t runtime_file_resolve(const char *name);
uintptr_t runtime_services_resolve(const char *name);
/* ufc3: online play (BB_ONLINE_SERVER); 0 for every name when it is not set. */
uintptr_t runtime_net_resolve(const char *name);
void runtime_file_report(void);
void runtime_file_configure(const char *app0, const char *user);
int runtime_file_mount(const char *guest, const char *host);
void runtime_file_unmount(const char *guest);
int runtime_file_translate(const char *guest, char *out, size_t size);
int64_t runtime_file_open(const char *path, int flags, int mode);
int64_t runtime_file_close(int fd);
int64_t runtime_file_read(int fd, void *buffer, uint64_t size);
int64_t runtime_file_pread(int fd, void *buffer, uint64_t size, int64_t offset);
int64_t runtime_file_write(int fd, const void *buffer, uint64_t size);
int64_t runtime_file_lseek(int fd, int64_t offset, int whence);
int64_t runtime_file_stat(const char *path, void *guest_stat);
int64_t runtime_file_fstat(int fd, void *guest_stat);
int64_t runtime_file_getdents(int fd, char *buffer, uint64_t size, int64_t *base);
void runtime_thread_keys_cleanup(void);
/* Guest-visible errno values are FreeBSD's. */
int32_t runtime_guest_errno(int host_errno);
void *runtime_low_map(size_t size, int prot);
uintptr_t runtime_ajm_resolve(const char *name);
void runtime_ajm_report(void);
uintptr_t runtime_audio_resolve(const char *name);
void runtime_audio_report(void);
uintptr_t runtime_pad_resolve(const char *name);
void runtime_pad_report(void);
/* SacredFire: controller ports for split screen. Port 1 is the player's pad (keyboard too); ports
 * 2..4 take the gamepad named in bbport.ini (port.N=<GUID or part of the name>, written by the
 * in-game menu) and appear to the game as logged-in users 2..4 while that gamepad is connected. */
int runtime_pad_user_present(int user);
void runtime_services_user_change(int user, int login);
uintptr_t runtime_rtc_resolve(const char *name);
const char *runtime_file_user_dir(void);
void runtime_savedata_configure(const char *title);
uintptr_t runtime_savedata_resolve(const char *name);
void runtime_savedata_report(void);
void runtime_thread_attach_host(const char *name);
/* Before anything else allocates: Windows reserves the PS4 user range (no-op on Linux). */
void runtime_memory_reserve(void);
/* Before any driver loads: Windows claims the TEB TLS slot of the guest TCB (no-op on Linux). */
void runtime_thread_host_init(void);
#endif
