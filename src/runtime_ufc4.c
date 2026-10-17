// SPDX-License-Identifier: GPL-2.0-or-later
/* UFC 4 additions, linked normally in a full build or into the measured donor's
 * identified call sites by tools/link_runtime_extension.py. No generic success stubs.
 * PftFlag mirrors shadPS4 app_content.cpp's explicitly logged compatibility stub.
 * It does not mount DLC, return entitlements, or alter license data.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <pthread.h>

#define GUEST __attribute__((sysv_abi))
#define U4_TIME_ABI GUEST
#include "ufc4_time.h"
#include "ufc4_file_diag.c"
extern uintptr_t runtime_kernel_resolve(const char *name);
uintptr_t ufc4_kernel_resolve(const char *name) {
    uintptr_t diagnostic=u4_file_diagnostic(name);
    if (diagnostic) return diagnostic;
    if (!strcmp(name,"0NTHN1NKONI#libkernel")) return (uintptr_t)u4_localtime_to_utc;
    if (!strcmp(name,"-o5uEDpN+oY#libkernel")) return (uintptr_t)u4_utc_to_localtime;
    return runtime_kernel_resolve(name);
}
uintptr_t ufc4_kernel_lookup(const void *table,size_t count,const char *name) {
    (void)table; (void)count; return ufc4_kernel_resolve(name);
}
#include "ufc4_savedata.c"
#include "ufc4_dialog.c"
#ifdef _WIN32
#include "ufc4_net.c"
#else
extern uintptr_t runtime_services_resolve(const char *name);
uintptr_t ufc4_services_resolve(const char *name) {
    uintptr_t dialog=u4_dialog_resolve(name);return dialog ? dialog : runtime_services_resolve(name);
}
#endif
extern uintptr_t bbgpu_resolve(const char *name);
extern uint64_t platform_monotonic_ns(void);
extern int platform_sleep_ns(uint64_t ns);
extern uint32_t platform_thread_id(void);
#ifdef UFC4_DONOR_EXTENSION
#if !defined(UFC4_EXTENSION_TEST) && !defined(UFC4_DONOR_V015)
#include "ufc4_texture_sync.c"
#include "ufc4_texture_probe.c"
#include "ufc4_gpu_labels.c"
#endif
#ifdef UFC4_DONOR_V015
extern void original_attach(void *) __asm__("attach.llvm.3127064991155529722");
/* v0.1.5 adds an optional online provider before its services table. UFC4 is
 * explicitly offline: leave all imports to the existing offline services.
 */
uintptr_t ufc4_offline_resolver(const char *name) { (void)name;return 0; }
#else
extern void original_attach(void *) __asm__("attach.llvm.350147966661975831");
#endif

static struct { _Atomic uintptr_t context; _Atomic int ready; } ready_threads[4096];
void ufc4_thread_attach(void *context) {
    original_attach(context);
    /* The first ScePthread word is the thread identifier EAThread reads. The
     * donor kept a TCB pointer there; its TCB is already held in the TEB slot. */
    atomic_store_explicit((_Atomic uintptr_t *)context, platform_thread_id(), memory_order_release);
    for (unsigned i=0;i<4096;i++)
        if (atomic_load_explicit(&ready_threads[i].context,memory_order_acquire)==(uintptr_t)context) {
            atomic_store_explicit(&ready_threads[i].ready,1,memory_order_release);break;
        }
}

/* EAThread reads the first ScePthread word immediately after creation as its
 * ThreadId. In the donor that word is the TCB, assigned by host_start::attach.
 * Publishing an empty word makes EAThread report failure and free its live
 * DirtyThread context. Wait for attachment before returning to the creator.
 * Only the native pthread_create call inside runtime_thread.c::create is routed
 * here. Renderer worker threads retain their existing creation path.
 */
int ufc4_host_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                           void *(*entry)(void *), void *context) {
    if (!context) return pthread_create(thread,attr,entry,context);
    unsigned slot=4096;
    for (unsigned i=0;i<4096;i++) {
        uintptr_t empty=0;
        if (atomic_compare_exchange_strong(&ready_threads[i].context,&empty,(uintptr_t)context)) {
            slot=i;atomic_store(&ready_threads[i].ready,0);break;
        }
    }
    if (slot==4096) { fputs("STOP: UFC4 concurrent thread registry is full\n",stderr);exit(21); }
    int result = pthread_create(thread, attr, entry, context);
    if (result) { atomic_store(&ready_threads[slot].context,0);return result; }
    const uint64_t deadline = platform_monotonic_ns() + UINT64_C(5000000000);
    for (;;) {
        if (atomic_load_explicit(&ready_threads[slot].ready,memory_order_acquire)) {
            atomic_store_explicit(&ready_threads[slot].context,0,memory_order_release);return 0;
        }
        if (platform_monotonic_ns() >= deadline) {
            fputs("STOP: UFC4 guest thread did not publish its ID within 5 seconds\n", stderr);
            exit(21);
        }
        platform_sleep_ns(1000);
    }
}
#endif

static GUEST int32_t ufc4_pft_flag(uint32_t *flag) {
    if (!flag) return (int32_t)0x80d90002;
    *flag = 0;
    static _Atomic unsigned calls;
    if (atomic_fetch_add_explicit(&calls, 1, memory_order_relaxed) < 4)
        puts("UFC4: sceAppContentGetPftFlag compatibility provider (offline flag 0)");
    return 0;
}

/* This runtime runs a PS4 executable on PC, not a PS5/Prospero executable. */
static GUEST int32_t ufc4_is_prospero(void) { return 0; }

uintptr_t ufc4_bbgpu_fallback(const char *name) {
    if (!strcmp(name, "xmhnAoxN3Wk#libSceAppContentPft"))
        return (uintptr_t)ufc4_pft_flag;
    if (!strcmp(name, "mpxAdqW7dKY#libkernel_cpumode_platform"))
        return (uintptr_t)ufc4_is_prospero;
    return bbgpu_resolve(name);
}
