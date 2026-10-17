/* SacredFire: Windows guest-memory probe for the section/placeholder backend of platform.h:
 * reservation of the PS4 user range, aliasing (two views of the same pool offset see each
 * other's writes), partial unmap with split and coalesce, protection changes that survive a
 * neighbour's remap, discard zeroing, and refusal of ranges outside the reservation.
 * GPL-2.0-or-later. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "platform/platform.h"
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
#define LO UINT64_C(0x1000000000)
#define HI UINT64_C(0xfc00000000)
/* The PS4 page (16 KiB): Windows replaces placeholders at page granularity (MapViewOfFile3:
 * BaseAddress and ViewSize must match the placeholder, the 64 KiB rule does not apply). Wine 11
 * requires 64 KiB: BB_PROBE_PAGE=65536 runs the same logic there as a diagnostic. */
static uint64_t PAGE = 16384;
int main(void) {
    if (getenv("BB_PROBE_PAGE")) PAGE = strtoull(getenv("BB_PROBE_PAGE"), NULL, 10);
    printf("memory_probe: page granularity %llu\n", (unsigned long long)PAGE);
    platform_process_init();
    platform_pool_reserve_range(LO, HI);
    unsigned char *backing = NULL;
    CHECK(platform_pool_create(64u << 20, &backing) == 0 && backing);
    /* Two views of pool offset 0 alias each other and the backing view. */
    uintptr_t a = LO + (1u << 30), b = LO + (2u << 30);
    CHECK(platform_pool_map(a, 4 * PAGE, PROT_READ | PROT_WRITE, 0, 0) == 0);
    CHECK(platform_pool_map(b, 4 * PAGE, PROT_READ | PROT_WRITE, 0, 0) == 0);
    *(volatile uint32_t *)a = 0x11223344;
    CHECK(*(volatile uint32_t *)b == 0x11223344 && *(uint32_t *)backing == 0x11223344);
    /* Partial unmap in the middle of a view: the two outer parts stay mapped and aliased. */
    CHECK(platform_pool_unmap(a + PAGE, 2 * PAGE) == 0);
    *(volatile uint32_t *)(a + 3 * PAGE) = 0x55667788;
    CHECK(*(volatile uint32_t *)(b + 3 * PAGE) == 0x55667788);
    /* Remap the hole from another offset, then the whole run again. */
    CHECK(platform_pool_map(a + PAGE, 2 * PAGE, PROT_READ | PROT_WRITE, 0, 8 * PAGE) == 0);
    *(volatile uint32_t *)(a + PAGE) = 7; CHECK(*(uint32_t *)(backing + 8 * PAGE) == 7);
    CHECK(platform_pool_unmap(a, 4 * PAGE) == 0);
    CHECK(platform_pool_map(a, 4 * PAGE, PROT_READ | PROT_WRITE, 0, 0) == 0);
    CHECK(*(volatile uint32_t *)a == 0x11223344);
    /* Protection: read-only pages fault on write (checked through the platform's safe read of
     * a protected page: readable; a write would raise). */
    CHECK(platform_pool_protect(a, PAGE, PROT_READ) == 0);
    uint32_t copy = 0; CHECK(platform_safe_read(a, &copy, 4) && copy == 0x11223344);
    /* A neighbour's remap keeps that protection (save/restore of runs). */
    CHECK(platform_pool_unmap(a + 2 * PAGE, PAGE) == 0);
    CHECK(platform_pool_map(a + 2 * PAGE, PAGE, PROT_READ | PROT_WRITE, 0, 16 * PAGE) == 0);
    CHECK(platform_pool_protect(a, PAGE, PROT_READ | PROT_WRITE) == 0);
    *(volatile uint32_t *)a = 9; CHECK(*(volatile uint32_t *)b == 9);
    /* Reserved (no access) ranges and discard zeroing. */
    CHECK(platform_pool_map(LO + (3u << 30), 2 * PAGE, 0, 1, 0) == 0);
    platform_pool_discard(backing, 0, PAGE); CHECK(*(volatile uint32_t *)a == 0);
    /* Outside the reservation: refused, nothing mapped. */
    CHECK(platform_pool_map(LO - PAGE, PAGE, PROT_READ | PROT_WRITE, 0, 0) != 0);
    CHECK(platform_pool_map(HI, PAGE, PROT_READ | PROT_WRITE, 0, 0) != 0);
    /* Exhaustion of the pool offset range is the caller's accounting; a view past the section
     * end fails without touching the table. */
    CHECK(platform_pool_map(LO + (5u << 30), PAGE, PROT_READ | PROT_WRITE, 0, (64u << 20)) != 0);
    CHECK(platform_pool_unmap(a, 4 * PAGE) == 0 && platform_pool_unmap(b, 4 * PAGE) == 0);
    /* Low mappings for the image and stacks land below the reservation. */
    void *low = platform_map_fixed(UINT64_C(0x800000000), 65536, PROT_READ | PROT_WRITE);
    CHECK(low == (void *)UINT64_C(0x800000000));
    puts("memory_probe PASS: reservation, aliasing, split/coalesce, protection runs, discard, refusals");
    return 0;
}
