/* ufc3 online (UFC-25): the deterministic rcp/rsqrt patch (src/runtime_fp.c).
 * Small guest-like routines (System V, AT&T-free Intel syntax below) are copied into a buffer,
 * patched as the loader patches the eboot, run, and their results compared with the rule in C
 * (runtime_fp_rcp/runtime_fp_rsqrt): every operand form, the lanes and upper bits each form
 * keeps, registers, flags, the red zone, displaced instructions (RIP-relative, a jump), on edge
 * values and a wide sample of bit patterns, under MXCSR 0x1f80 and 0x9fc0. The rule itself is
 * checked against exact arithmetic and against this CPU's own rcpps/rsqrtps.
 * Arguments: --all also runs all 2^32 patterns through the 8-lane stubs (about a minute). */
#include "runtime.h"
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

__asm__(".text\n.intel_syntax noprefix\n.globl fp_test_begin\nfp_test_begin:\n"
        /* ymm reg: out = rcp(in[0..7]) */
        "t_ymm_reg: vmovups ymm9, [rdi]\n vrcpps ymm3, ymm9\n vmovups [rsi], ymm3\n ret\n"
        /* ymm memory with an index: out = rsqrt(in[0..7]) */
        "t_ymm_mem: mov rax, 1\n vrsqrtps ymm0, [rdi+rax*8-8]\n vmovups [rsi], ymm0\n ret\n"
        /* xmm memory (VEX.128 zeroes 255:128): out = rsqrt(in[0..3]), 0 x4 */
        "t_xmm_mem: vmovups ymm12, [rdx]\n vrsqrtps xmm12, [rdi]\n vmovups [rsi], ymm12\n ret\n"
        /* legacy reg, xmm8 (keeps 255:128): out = rcp(in[0..3]), aux[4..7] */
        "t_legacy_reg: vmovups ymm8, [rdx]\n movups xmm1, [rdi]\n rcpps xmm8, xmm1\n vmovups [rsi], ymm8\n ret\n"
        /* legacy memory: out = rsqrt(in[0..3]), aux[4..7] */
        "t_legacy_mem: vmovups ymm10, [rdx]\n rsqrtps xmm10, [rdi]\n vmovups [rsi], ymm10\n ret\n"
        /* legacy scalar memory: out = rcp(in[1]), aux[1..7] */
        "t_ss_mem: vmovups ymm5, [rdx]\n rcpss xmm5, [rdi+4]\n vmovups [rsi], ymm5\n ret\n"
        /* legacy scalar reg: out = rsqrt(in[0]), aux[1..7] */
        "t_ss_reg: vmovups ymm5, [rdx]\n movups xmm14, [rdi]\n rsqrtss xmm5, xmm14\n vmovups [rsi], ymm5\n ret\n"
        /* VEX scalar reg: out = rcp(in[0]), aux[1..3], 0 x4 */
        "t_vss_reg: vmovups ymm7, [rdx]\n movups xmm11, [rdi]\n vmovups ymm2, [rdi]\n vrcpss xmm2, xmm7, xmm11\n vmovups [rsi], ymm2\n ret\n"
        /* VEX scalar memory: out = rsqrt(in[2]), aux[1..3], 0 x4 */
        "t_vss_mem: vmovups ymm3, [rdx]\n vmovups ymm14, [rdi]\n vrsqrtss xmm14, xmm3, [rdi+8]\n vmovups [rsi], ymm14\n ret\n"
        /* adjacent sites (the second is displaced into the first's stub): rcp(in[0..3]), rsqrt(aux[0..3]) */
        "t_pair: vmovups ymm2, [rdi]\n vmovups ymm4, [rdx]\n vrcpps xmm1, xmm2\n vrsqrtps xmm3, xmm4\n"
        " vmovups [rsi], xmm1\n vmovups [rsi+16], xmm3\n ret\n"
        /* RIP-relative source, and a RIP-relative instruction displaced: rcp(k), k */
        "t_rip: vrcpps xmm4, [rip+t_k]\n vmovups [rsi], xmm4\n vmovups xmm6, [rdi]\n vrsqrtps xmm6, xmm6\n vmovups xmm5, [rip+t_k]\n"
        " vmovups [rsi+16], xmm5\n ret\n"
        /* a jump displaced with its flags: 1 when rdi < rsi, else 2 */
        "t_branch: cmp rdi, rsi\n vrsqrtps xmm1, xmm1\n jb 1f\n mov eax, 2\n ret\n1: mov eax, 1\n ret\n"
        /* preserved: ymm0-6 (the stubs' scratch), rcx r8 r11, the red zone, CF; out[0..6] ymm,
         * then rcx r8 r11, [rsp-8], [rsp-128], CF */
        "t_preserve: vmovups ymm0, [rdx]\n vmovups ymm1, [rdx+32]\n vmovups ymm2, [rdx+64]\n vmovups ymm3, [rdx+96]\n"
        " vmovups ymm4, [rdx+128]\n vmovups ymm5, [rdx+160]\n vmovups ymm6, [rdx+192]\n vmovups ymm8, [rdi]\n"
        " mov rcx, 0x1111\n mov r8, 0x2222\n mov r11, 0x3333\n mov [rsp-8], rdi\n mov [rsp-128], rsi\n stc\n"
        " vrcpps ymm7, ymm8\n mov rax, [rsp-8]\n setc r9b\n"
        " vmovups [rsi], ymm0\n vmovups [rsi+32], ymm1\n vmovups [rsi+64], ymm2\n vmovups [rsi+96], ymm3\n"
        " vmovups [rsi+128], ymm4\n vmovups [rsi+160], ymm5\n vmovups [rsi+192], ymm6\n"
        " mov [rsi+224], rcx\n mov [rsi+232], r8\n mov [rsi+240], r11\n mov [rsi+248], rax\n mov rax, [rsp-128]\n mov [rsi+256], rax\n"
        " mov [rsi+264], r9b\n ret\n"
        /* timing: edi iterations of an 8-lane rsqrt */
        "t_loop: vmovups ymm0, [rsi]\n2: vrsqrtps ymm1, ymm0\n dec edi\n jnz 2b\n vmovups [rsi], ymm1\n vzeroupper\n ret\n"
        ".p2align 4\nt_k: .long 0x40400000, 0x80000000, 0x00000001, 0x7f800001\n"
        ".globl fp_test_end\nfp_test_end:\n"
        ".globl fp_test_offsets\nfp_test_offsets:\n"
        ".quad t_ymm_reg-fp_test_begin, t_ymm_mem-fp_test_begin, t_xmm_mem-fp_test_begin, t_legacy_reg-fp_test_begin\n"
        ".quad t_legacy_mem-fp_test_begin, t_ss_mem-fp_test_begin, t_ss_reg-fp_test_begin, t_vss_reg-fp_test_begin\n"
        ".quad t_vss_mem-fp_test_begin, t_pair-fp_test_begin, t_rip-fp_test_begin, t_branch-fp_test_begin\n"
        ".quad t_preserve-fp_test_begin, t_loop-fp_test_begin\n"
        ".att_syntax prefix\n");
extern const unsigned char fp_test_begin[], fp_test_end[];
extern const uint64_t fp_test_offsets[];
enum { YMM_REG, YMM_MEM, XMM_MEM, LEGACY_REG, LEGACY_MEM, SS_MEM, SS_REG, VSS_REG, VSS_MEM, PAIR, RIP, BRANCH, PRESERVE, LOOP, ROUTINES };
typedef uint64_t (ABI *Routine)(const void *in, void *out, const void *aux);

static int failures;
#define CHECK(c, ...) do { if (!(c)) { if (failures++ < 20) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

static unsigned char *buffer, *stub_area;
static size_t stub_size;
static void *near_map(size_t bytes) {
    if (bytes > stub_size) return NULL;
    return stub_area;
}
static Routine patched[ROUTINES], native[ROUTINES];
static void load(void) {
    const size_t code = (size_t)(fp_test_end - fp_test_begin), page = platform_page_size();
    const size_t area = 2 * ((code + page - 1) / page * page) + (stub_size = 1 << 20);
    if (!(buffer = platform_map_anonymous(area, PROT_READ | PROT_WRITE))) { puts("FAIL: no memory"); exit(1); }
    unsigned char *copy = buffer + (code + page - 1) / page * page;
    stub_area = copy + (code + page - 1) / page * page;
    memcpy(buffer, fp_test_begin, code);
    memcpy(copy, fp_test_begin, code);
    const uint64_t address = 0, size = code, flags = 5;
    RuntimeFpStats stats;
    const uint64_t n = runtime_fp_patch_image(buffer, code, &address, &size, &flags, 1, NULL, 0, near_map, &stats);
    /* 16 sites, all patched (the pair gets one stub). */
    CHECK(n == 16 && stats.found == 16 && stats.unpatched == 0, "patched %" PRIu64 " of %" PRIu64 " (%" PRIu64 " kept)", n, stats.found, stats.unpatched);
    if (platform_protect(buffer, (size_t)(copy - buffer) * 2, PROT_READ | PROT_EXEC)) { puts("FAIL: protect"); exit(1); }
    for (int i = 0; i < ROUTINES; ++i) {
        patched[i] = (Routine)(buffer + fp_test_offsets[i]);
        native[i] = (Routine)(copy + fp_test_offsets[i]);
    }
    printf("patched %" PRIu64 " sites (%" PRIu64 " with displaced instructions), %" PRIu64 " bytes of stubs\n", n, stats.displaced, stats.stub_bytes);
}

static float f(uint32_t bits) { float v; memcpy(&v, &bits, 4); return v; }
static uint32_t bits_of(float v) { uint32_t b; memcpy(&b, &v, 4); return b; }
static uint32_t lane(const void *p, int i) { uint32_t v; memcpy(&v, (const unsigned char *)p + 4 * i, 4); return v; }

/* One input vector through every routine. */
static void check_vector(const uint32_t in[8], const uint32_t aux[8]) {
    uint32_t out[8];
    patched[YMM_REG](in, out, aux);
    for (int i = 0; i < 8; ++i) CHECK(out[i] == runtime_fp_rcp(in[i]), "ymm rcp(%08x) = %08x, rule %08x", in[i], out[i], runtime_fp_rcp(in[i]));
    patched[YMM_MEM](in, out, aux);
    for (int i = 0; i < 8; ++i) CHECK(out[i] == runtime_fp_rsqrt(in[i]), "ymm rsqrt(%08x) = %08x, rule %08x", in[i], out[i], runtime_fp_rsqrt(in[i]));
    patched[XMM_MEM](in, out, aux);
    for (int i = 0; i < 8; ++i) CHECK(out[i] == (i < 4 ? runtime_fp_rsqrt(in[i]) : 0), "xmm rsqrt lane %d of %08x: %08x", i, in[i], out[i]);
    patched[LEGACY_REG](in, out, aux);
    for (int i = 0; i < 8; ++i) CHECK(out[i] == (i < 4 ? runtime_fp_rcp(in[i]) : aux[i]), "legacy rcp lane %d of %08x: %08x", i, in[i], out[i]);
    patched[LEGACY_MEM](in, out, aux);
    for (int i = 0; i < 8; ++i) CHECK(out[i] == (i < 4 ? runtime_fp_rsqrt(in[i]) : aux[i]), "legacy rsqrt lane %d of %08x: %08x", i, in[i], out[i]);
    patched[SS_MEM](in, out, aux);
    for (int i = 0; i < 8; ++i) CHECK(out[i] == (i ? aux[i] : runtime_fp_rcp(in[1])), "rcpss mem lane %d: %08x", i, out[i]);
    patched[SS_REG](in, out, aux);
    for (int i = 0; i < 8; ++i) CHECK(out[i] == (i ? aux[i] : runtime_fp_rsqrt(in[0])), "rsqrtss reg lane %d: %08x", i, out[i]);
    patched[VSS_REG](in, out, aux);
    for (int i = 0; i < 8; ++i) CHECK(out[i] == (!i ? runtime_fp_rcp(in[0]) : i < 4 ? aux[i] : 0), "vrcpss reg lane %d: %08x", i, out[i]);
    patched[VSS_MEM](in, out, aux);
    for (int i = 0; i < 8; ++i) CHECK(out[i] == (!i ? runtime_fp_rsqrt(in[2]) : i < 4 ? aux[i] : 0), "vrsqrtss mem lane %d: %08x", i, out[i]);
    patched[PAIR](in, out, aux);
    for (int i = 0; i < 8; ++i)
        CHECK(out[i] == (i < 4 ? runtime_fp_rcp(in[i]) : runtime_fp_rsqrt(aux[i - 4])), "pair lane %d: %08x", i, out[i]);
}

static void check_fixed(void) {
    uint32_t out[8], in[8] = {0x40400000};
    static const uint32_t k[4] = {0x40400000, 0x80000000, 0x00000001, 0x7f800001};
    patched[RIP](in, out, NULL);
    for (int i = 0; i < 4; ++i) CHECK(out[i] == runtime_fp_rcp(k[i]) && out[4 + i] == k[i], "rip lane %d: %08x %08x", i, out[i], out[4 + i]);
    CHECK(patched[BRANCH]((void *)1, (void *)2, NULL) == 1 && patched[BRANCH]((void *)2, (void *)1, NULL) == 2, "displaced jb");
    uint32_t keep[56], result[68];
    for (int i = 0; i < 56; ++i) keep[i] = 0x9e3779b9u * (uint32_t)(i + 1);
    for (int i = 0; i < 8; ++i) in[i] = 0x3f800000 + (uint32_t)i * 0x123457;
    patched[PRESERVE](in, result, keep);
    CHECK(!memcmp(result, keep, 224), "ymm0-6 changed");
    uint64_t g[5];
    memcpy(g, result + 56, sizeof g);
    CHECK(g[0] == 0x1111 && g[1] == 0x2222 && g[2] == 0x3333, "rcx/r8/r11 changed");
    CHECK(g[3] == (uintptr_t)in && g[4] == (uintptr_t)result, "red zone changed: %" PRIx64 " %" PRIx64, g[3], g[4]);
    CHECK((result[66] & 0xff) == 1, "carry flag lost");
}

/* The rule: R(fl(1/x)), R(fl(1/fl(sqrt x))) within 2^-13 + 2^-23 of exact; specials as listed. */
static void check_rule(uint32_t x) {
    const uint32_t r = runtime_fp_rcp(x), s = runtime_fp_rsqrt(x), a = x & 0x7fffffff;
    const double v = f(x);
    CHECK(!(r & 0x7ff) || a > 0x7f800000, "rcp(%08x) = %08x has more than 12 fraction bits", x, r);
    CHECK(!(s & 0x7ff) || a > 0x7f800000 || s == 0xffc00000, "rsqrt(%08x) = %08x has more than 12 fraction bits", x, s);
    if (a > 0x7f800000) { CHECK(r == (x | 0x400000) && s == (x | 0x400000), "NaN %08x: %08x %08x", x, r, s); return; }
    if (a < 0x00800000) { CHECK(r == ((x & 0x80000000) | 0x7f800000) && s == r, "zero/denormal %08x: %08x %08x", x, r, s); return; }
    if (a > 0x7e800000) CHECK(r == (x & 0x80000000), "rcp(%08x) = %08x, not zero", x, r);
    else CHECK(fabs(f(r) - 1 / v) <= fabs(1 / v) * (0x1p-13 + 0x1p-23), "rcp(%08x) = %08x: too far from %a", x, r, 1 / v);
    if (x & 0x80000000) CHECK(s == 0xffc00000, "rsqrt(%08x) = %08x, not the default NaN", x, s);
    else if (a == 0x7f800000) CHECK(s == 0, "rsqrt(inf) = %08x", s);
    else CHECK(fabs(f(s) - 1 / sqrt(v)) <= 1 / sqrt(v) * (0x1p-13 + 0x1p-22), "rsqrt(%08x) = %08x: too far from %a", x, s, 1 / sqrt(v));
}
/* This CPU's rcpps/rsqrtps: the same special results, and within the specified error. */
static void check_hardware(uint32_t x) {
    float rf, sf;
    __asm__("rcpss %1, %0" : "=x"(rf) : "x"(f(x)));
    __asm__("rsqrtss %1, %0" : "=x"(sf) : "x"(f(x)));
    const uint32_t r = bits_of(rf), s = bits_of(sf), a = x & 0x7fffffff;
    const uint32_t rule_r = runtime_fp_rcp(x), rule_s = runtime_fp_rsqrt(x);
    if (a < 0x00800000 || a >= 0x7f800000) {
        CHECK(r == rule_r && s == rule_s, "special %08x: cpu %08x %08x, rule %08x %08x", x, r, s, rule_r, rule_s);
        return;
    }
    const double v = f(x);
    /* rcp: where results turn tiny each CPU decides on its own */
    if (a < 0x7e000000) CHECK(fabs(f(r) - f(rule_r)) <= fabs(1 / v) * 0x1.8p-11, "rcp(%08x): cpu %08x, rule %08x", x, r, rule_r);
    if (x & 0x80000000) CHECK(s == rule_s, "rsqrt(%08x): cpu %08x, rule %08x", x, s, rule_s);
    else CHECK(fabs(f(s) - f(rule_s)) <= 1 / sqrt(v) * 0x1.8p-11, "rsqrt(%08x): cpu %08x, rule %08x", x, s, rule_s);
}

static uint64_t state = 0x853c49e6748fea9bu;
static uint32_t random32(void) { state = state * 6364136223846793005u + 1442695040888963407u; return (uint32_t)(state >> 32); }

static void run_sample(void) {
    static const uint32_t edges[] = {0, 0x80000000, 1, 0x80000001, 0x007fffff, 0x807fffff, 0x00800000, 0x80800000,
        0x00800001, 0x3f800000, 0xbf800000, 0x3f7fffff, 0x3f800001, 0x40400000, 0x40800000, 0x7e7fffff, 0x7e800000,
        0x7e800001, 0xfe800000, 0xfe800001, 0x7effffff, 0x7f7fffff, 0xff7fffff, 0x7f800000, 0xff800000, 0x7f800001,
        0xff800001, 0x7fbfffff, 0x7fc00000, 0xffc00000, 0x7fffffff, 0xffffffff, 0x3f7ff800, 0x3f7ffc00, 0x3f7ffbff,
        0x3f7ffc01, 0x5f000000, 0x1f800000, 0x00000400, 0x00400000, 0x7e7ff000, 0x7e801000, 0x3eaaaaab};
    const size_t n = sizeof(edges) / sizeof(*edges);
    uint32_t in[8], aux[8];
    for (size_t i = 0; i < n; ++i) {
        for (int k = 0; k < 8; ++k) { in[k] = edges[(i + (size_t)k) % n]; aux[k] = edges[(i * 7 + (size_t)k) % n]; }
        check_vector(in, aux);
        check_rule(edges[i]); check_hardware(edges[i]);
    }
    /* Random patterns, then every exponent with random significands. */
    for (int i = 0; i < 1 << 18; ++i) {
        for (int k = 0; k < 8; ++k) { in[k] = random32(); aux[k] = random32(); }
        if (i & 1) for (int k = 0; k < 8; ++k) in[k] = (in[k] & 0x807fffff) | (uint32_t)((i >> 1) & 0xff) << 23;
        check_vector(in, aux);
        check_rule(in[0]); check_hardware(in[0]);
    }
}

int main(int argc, char **argv) {
    unsigned int saved;
    __asm__ volatile("stmxcsr %0" : "=m"(saved));
    load();
    check_fixed();
    run_sample();
    unsigned int ps4 = 0x9fc0;
    __asm__ volatile("ldmxcsr %0" : : "m"(ps4));
    state = 0x853c49e6748fea9bu;
    run_sample(); /* the same results with FTZ/DAZ: the stubs do not depend on MXCSR */
    __asm__ volatile("ldmxcsr %0" : : "m"(saved));
    if (argc > 1 && !strcmp(argv[1], "--all")) {
        uint32_t in[8], out[8];
        for (uint64_t x = 0; x < UINT64_C(1) << 32; x += 8) {
            for (int k = 0; k < 8; ++k) in[k] = (uint32_t)(x + (uint64_t)k);
            patched[YMM_REG](in, out, NULL);
            for (int k = 0; k < 8; ++k) CHECK(out[k] == runtime_fp_rcp(in[k]), "rcp(%08x) = %08x", in[k], out[k]);
            patched[YMM_MEM](in, out, NULL);
            for (int k = 0; k < 8; ++k) CHECK(out[k] == runtime_fp_rsqrt(in[k]), "rsqrt(%08x) = %08x", in[k], out[k]);
            check_rule(in[0]);
        }
        puts("all 2^32 patterns checked");
    }
    /* Cost of a patched 8-lane rsqrt against the CPU's own (a loop of nothing else). */
    uint32_t v[8] = {0x40400000, 0x40400000, 0x40400000, 0x40400000, 0x40400000, 0x40400000, 0x40400000, 0x40400000};
    const int iterations = 2000000;
    uint64_t t0 = platform_monotonic_ns();
    native[LOOP]((void *)(uintptr_t)iterations, v, NULL);
    uint64_t t1 = platform_monotonic_ns();
    for (int k = 0; k < 8; ++k) v[k] = 0x40400000;
    patched[LOOP]((void *)(uintptr_t)iterations, v, NULL);
    uint64_t t2 = platform_monotonic_ns();
    CHECK(lane(v, 7) == runtime_fp_rsqrt(0x40400000), "loop result %08x", lane(v, 7));
    printf("vrsqrtps ymm: %.1f ns native, %.1f ns patched\n", (double)(t1 - t0) / iterations, (double)(t2 - t1) / iterations);
    if (failures) { printf("%d failures\n", failures); return 1; }
    puts("deterministic FP: all checks passed");
    return 0;
}
