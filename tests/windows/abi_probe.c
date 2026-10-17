/* SacredFire: Windows x86-64 ABI probe (tests/windows). Built by `ninja windows-tests`
 * (gpu/CMakeLists.txt), run on Windows or under Wine as a diagnostic. GPL-2.0-or-later. */
/* Cross-toolchain probe: host (MS x64) -> guest (SysV) -> host callbacks, integer/FP/aggregate
 * arguments, register preservation. Compiled for Windows x86-64 with llvm-mingw. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define ABI __attribute__((sysv_abi))
typedef struct { uint64_t a, b; } Pair;           /* 16 bytes: SysV returns in rax:rdx */
typedef struct { double x, y; } DPair;            /* SysV returns in xmm0:xmm1 */
typedef struct { uint64_t a, b, c; } Triple;      /* memory class */
typedef int64_t (ABI *GuestFn)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, double, double);
typedef void (ABI *Callback)(void *);
/* "Guest" side: a SysV function that calls back into the host through a SysV pointer. */
static ABI int64_t guest_sum(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e, int64_t f, int64_t g, double h, double i) {
    return a + 2*b + 3*c + 4*d + 5*e + 6*f + 7*g + (int64_t)(h*10) + (int64_t)(i*100);
}
static ABI Pair guest_pair(uint64_t a, uint64_t b) { Pair p = {a ^ 0x1111, b ^ 0x2222}; return p; }
static ABI DPair guest_dpair(double a, double b) { DPair p = {a * 2, b * 3}; return p; }
static ABI Triple guest_triple(Triple t) { Triple r = {t.c, t.b, t.a}; return r; }
static int cb_hits;
static ABI void host_callback(void *arg) { cb_hits += *(int *)arg; }
static ABI void guest_nested(Callback cb, void *arg, int depth) { cb(arg); if (depth) guest_nested(cb, arg, depth - 1); }
/* Callee-saved registers of the *host* ABI (rbx rbp rdi rsi r12-r15, xmm6-15) must survive a
 * SysV call: SysV may clobber rdi/rsi/xmm6-15, so clang must save them around the call. */
static ABI void guest_clobber(void) {
    __asm__ volatile("mov $0x1234, %%rdi\n mov $0x5678, %%rsi\n pxor %%xmm6,%%xmm6\n pxor %%xmm7,%%xmm7\n pxor %%xmm15,%%xmm15" ::: "rdi", "rsi", "xmm6", "xmm7", "xmm15", "memory");
}
int main(void) {
    int ok = 1;
    GuestFn f = guest_sum;
    int64_t r = f(1, 2, 3, 4, 5, 6, 7, 0.5, 0.25);
    ok &= r == 1+4+9+16+25+36+49+5+25; printf("sum %lld %s\n", (long long)r, ok ? "ok" : "BAD");
    Pair p = guest_pair(0xaaaa, 0xbbbb); ok &= p.a == (0xaaaa ^ 0x1111) && p.b == (0xbbbb ^ 0x2222);
    DPair d = guest_dpair(1.5, 2.0); ok &= d.x == 3.0 && d.y == 6.0;
    Triple t = {1, 2, 3}, u = guest_triple(t); ok &= u.a == 3 && u.b == 2 && u.c == 1;
    int v = 3; guest_nested(host_callback, &v, 4); ok &= cb_hits == 15;
    volatile double keep6 = 6.5, keep7 = 7.5, keep15 = 15.5; register double x6 = keep6, x7 = keep7, x15 = keep15;
    guest_clobber();
    ok &= x6 == 6.5 && x7 == 7.5 && x15 == 15.5;
    printf("aggregates/callbacks/preservation %s\n", ok ? "PASS" : "FAIL");
    _Static_assert(sizeof(long) == 4, "LLP64: long is 32-bit on Windows");
    return ok ? 0 : 1;
}
