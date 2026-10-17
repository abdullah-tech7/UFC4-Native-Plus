// SPDX-License-Identifier: GPL-2.0-or-later
/* PS4 conversion ABI, checked against shadPS4 kernel/time.h (2026-10-15).
 * The second local-to-UTC argument is numeric, never an output pointer. */
#include <time.h>
extern long platform_utc_offset(time_t value);
extern int platform_is_dst(time_t value);
typedef struct { int32_t offset_minutes, dst_minutes; } U4Timezone;
typedef struct { int64_t time; int32_t offset_seconds, dst_seconds; } U4Timesec;
static U4_TIME_ABI int32_t u4_localtime_to_utc(int64_t local, int64_t mode,
        int64_t *utc, U4Timezone *zone, int32_t *dst_seconds) {
    (void)mode;
    if (!zone) return (int32_t)0x80020016;
    const int32_t offset=(int32_t)platform_utc_offset((time_t)local);
    const int32_t dst=platform_is_dst((time_t)local)>0 ? 3600 : 0;
    zone->offset_minutes=(offset-dst)/60;
    zone->dst_minutes=dst/60;
    if (utc) *utc=local-offset;
    if (dst_seconds) *dst_seconds=dst;
    return 0;
}
static U4_TIME_ABI int32_t u4_utc_to_localtime(int64_t utc, int64_t *local,
        U4Timesec *detail, uint64_t *dst_seconds) {
    const int32_t offset=(int32_t)platform_utc_offset((time_t)utc);
    const int32_t dst=platform_is_dst((time_t)utc)>0 ? 3600 : 0;
    if (local) *local=utc+offset;
    if (detail) { detail->time=utc; detail->offset_seconds=offset-dst; detail->dst_seconds=dst; }
    if (dst_seconds) *dst_seconds=(uint64_t)dst;
    return 0;
}
