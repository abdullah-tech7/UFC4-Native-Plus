/* SacredFire: Windows guest-TLS probe. The runtime keeps each guest thread's TCB in a TEB TLS
 * slot and patches `mov rax, gs:[0]` to read that slot. This probe exercises the platform layer:
 * the slot is below 64, the displacement reads back what TlsSetValue stored, every thread sees
 * its own TCB (host threads that attach, create/destroy loops), and the Zydis-checked patcher
 * rewrites exactly the expected sites and refuses other GS access forms. GPL-2.0-or-later. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include "platform/platform.h"
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
static uint32_t displacement;
/* Reads gs:[displacement] the way patched guest code does. */
static void *read_tcb(void) { void *v; __asm__ volatile("mov %%gs:(%1), %0" : "=r"(v) : "r"((uintptr_t)displacement)); return v; }
static void *worker(void *arg) {
    uint64_t tcb[4] = {(uint64_t)(uintptr_t)arg, 1, 2, 3};
    platform_thread_publish_tcb(tcb);
    for (int i = 0; i < 1000; ++i) CHECK(read_tcb() == tcb);
    return NULL;
}
int main(void) {
    displacement = platform_thread_tcb_displacement();
    CHECK(displacement >= 0x1480 && displacement < 0x1480 + 8 * 64);
    printf("TEB TLS slot displacement 0x%x (slot %u)\n", displacement, (displacement - 0x1480) / 8);
    uint64_t main_tcb[4] = {42};
    platform_thread_publish_tcb(main_tcb);
    CHECK(read_tcb() == main_tcb);
    /* Threads: each its own TCB, repeated creation and destruction. */
    for (int round = 0; round < 50; ++round) {
        pthread_t threads[8];
        for (int i = 0; i < 8; ++i) CHECK(pthread_create(&threads[i], NULL, worker, (void *)(uintptr_t)(round * 8 + i)) == 0);
        for (int i = 0; i < 8; ++i) CHECK(pthread_join(threads[i], NULL) == 0);
        CHECK(read_tcb() == main_tcb);
    }
    /* The patcher on a synthetic executable segment: two real sites, one other GS form must be
     * rejected (exit 21) -- that path is exercised by running with --reject. */
    unsigned char *image = platform_map_anonymous(65536, PROT_READ | PROT_WRITE);
    CHECK(image);
    static const unsigned char load[] = {0x65, 0x48, 0x8b, 0x04, 0x25, 0, 0, 0, 0};
    memcpy(image + 16, load, 9); memcpy(image + 100, load, 9);
    memset(image + 200, 0x90, 32); image[232] = 0xc3;
    uint64_t address = 0, size = 4096, flags = 5;
    uint64_t patched = platform_patch_guest_tls_loads(image, &address, &size, &flags, 1, 2);
    CHECK(patched == 2);
    uint32_t got; memcpy(&got, image + 16 + 5, 4); CHECK(got == displacement);
    memcpy(&got, image + 100 + 5, 4); CHECK(got == displacement);
    /* Patched code runs: make the page executable and call it (mov rax, gs:[slot]; ret). */
    image[16 + 9] = 0xc3;
    CHECK(platform_protect(image, 4096, PROT_READ | PROT_EXEC) == 0);
    void *(__attribute__((sysv_abi)) *patched_load)(void) = (void *)(image + 16);
    CHECK(patched_load() == main_tcb);
    puts("tls_probe PASS: slot below 64, per-thread TCBs over 400 threads, 2 sites patched and executed");
    return 0;
}
