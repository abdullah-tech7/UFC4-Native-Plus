/* SPDX-License-Identifier: GPL-2.0-or-later
 * ufc3 (Linux release): glibc 2.43 gave these float math functions new symbol versions; the
 * bundled SDL3 is linked with --wrap=<name> and this object so it runs on glibc 2.38. */
#if defined(__linux__) && defined(__x86_64__)
float bb_acosf_old(float); float bb_asinf_old(float); float bb_sqrtf_old(float);
float bb_log10f_old(float); float bb_atan2f_old(float, float);
__asm__(".symver bb_acosf_old,acosf@GLIBC_2.2.5");
__asm__(".symver bb_asinf_old,asinf@GLIBC_2.2.5");
__asm__(".symver bb_sqrtf_old,sqrtf@GLIBC_2.2.5");
__asm__(".symver bb_log10f_old,log10f@GLIBC_2.2.5");
__asm__(".symver bb_atan2f_old,atan2f@GLIBC_2.2.5");
float __wrap_acosf(float x) { return bb_acosf_old(x); }
float __wrap_asinf(float x) { return bb_asinf_old(x); }
float __wrap_sqrtf(float x) { return bb_sqrtf_old(x); }
float __wrap_log10f(float x) { return bb_log10f_old(x); }
float __wrap_atan2f(float y, float x) { return bb_atan2f_old(y, x); }
#endif
