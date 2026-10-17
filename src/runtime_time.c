/* Explicit guest timeval layout; host pointers never escape this boundary. */
#define _DEFAULT_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { int64_t seconds, microseconds; } GuestTimeval;
typedef struct { int32_t minuteswest, dsttime; } GuestTimezone;
static ABI int guest_gettimeofday(GuestTimeval *value,GuestTimezone *zone) {
    const uint64_t now=platform_realtime_ns();
    if (value) { value->seconds=(int64_t)(now/1000000000u); value->microseconds=(int64_t)(now%1000000000u/1000u); }
    if (zone) { const time_t t=(time_t)(now/1000000000u); zone->minuteswest=(int32_t)(-platform_utc_offset(t)/60); zone->dsttime=platform_is_dst(t); }
    return 0;
}
uintptr_t runtime_time_resolve(const char *name) {
    if (runtime_nid_equals(name,"n88vx3C5nW8#libScePosix")) return (uintptr_t)guest_gettimeofday;
    return 0;
}
