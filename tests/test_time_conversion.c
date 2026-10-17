/* SPDX-License-Identifier: GPL-2.0-or-later
 * Regression: PS4 libc passes a DST selector as argument two, not a pointer.
 * Link with --gc-sections to exercise only these static ABI entry points. */
#include "../src/runtime_kernel.c"
#include <assert.h>

uint64_t platform_monotonic_ns(void) { return 0; }
long platform_utc_offset(time_t t) { (void)t; return -10800; }
int platform_is_dst(time_t t) { (void)t; return 1; }

int main(void) {
    typedef int32_t (ABI *FromLocal)(int64_t, int64_t, int64_t *, GuestTimezone *, int32_t *);
    typedef int32_t (ABI *ToLocal)(int64_t, int64_t *, GuestTimesec *, uint64_t *);
    FromLocal from_local = localtime_to_utc;
    ToLocal to_local = utc_to_localtime;
    const int64_t utc = 1700000000, local = utc - 10800;
    int64_t converted = -1;
    GuestTimezone tz = {0}; int32_t dst = -1;
    /* This selector=1 call dereferenced address 1 before the signature fix. */
    assert(from_local(local, 1, &converted, &tz, &dst) == 0);
    assert(converted == utc && tz.minuteswest == 180 && dst == 3600);
    assert((uint32_t)from_local(local, -1, NULL, &tz, &dst) == 0x80020016);
    struct { uint64_t before; GuestTimesec out; uint64_t after; } st = {
        .before = UINT64_MAX, .out = {-1, UINT32_MAX, UINT32_MAX}, .after = UINT64_MAX};
    uint64_t dst64 = UINT64_MAX;
    assert(to_local(utc, &converted, &st.out, &dst64) == 0);
    assert(converted == local && st.out.t == utc);
    assert((int32_t)st.out.west_sec == -10800 && st.out.dst_sec == 3600 && dst64 == 3600);
    assert(st.before == UINT64_MAX && st.after == UINT64_MAX);
    puts("PS4 time conversion ABI checks OK");
    return 0;
}
