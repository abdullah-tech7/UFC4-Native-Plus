// SPDX-License-Identifier: GPL-2.0-or-later
// ufc3 (Linux release builds): glibc 2.43 gave acosf and sqrtf new symbol versions; the library
// is linked with --wrap so they bind to the old ones and the build runs on glibc 2.38 (Ubuntu
// 24.04, Debian 13, Fedora 39+, SteamOS) instead of only the build machine's. Built without LTO
// (CMakeLists.txt): .symver must reach the assembler of this file.
#if defined(__linux__) && defined(__x86_64__)
extern "C" {
float bb_acosf_old(float);
float bb_sqrtf_old(float);
__asm__(".symver bb_acosf_old,acosf@GLIBC_2.2.5");
__asm__(".symver bb_sqrtf_old,sqrtf@GLIBC_2.2.5");
float __wrap_acosf(float x) { return bb_acosf_old(x); }
float __wrap_sqrtf(float x) { return bb_sqrtf_old(x); }
}
#endif
