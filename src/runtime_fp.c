/* ufc3 online (UFC-25): guest floating point that gives the same bits on every x86-64 CPU.
 *
 * Online play is lockstep: each peer simulates both fighters and the game compares a CRC of
 * their state every frame. The eboot has ~2300 approximate reciprocals (rcpps, rsqrtps, rcpss,
 * rsqrtss and their VEX forms); Intel and AMD, and each microarchitecture, return different
 * bits for them (only |error| <= 1.5*2^-12 is specified), so two peers with different CPUs
 * drift apart in a few frames (~/ufcCOntext/ONLINE-DETERMINISM.md, P1).
 *
 * With BB_DETERMINISTIC_FP (default: on when BB_ONLINE_SERVER is set, off offline) every one
 * of them in the guest image is replaced, at load time, by a jump to a stub that computes:
 *
 *   rcp(x)   = R(fl(1 / x))            rsqrt(x) = R(fl(1 / fl(sqrt(x))))
 *
 * fl() is IEEE-754 single precision (divps/sqrtps, round to nearest: bit-exact on every
 * CPU) and R() rounds the significand to 12 fraction bits, ties to even (AMD's output format;
 * error <= 2^-13 + 2^-24, within the instruction's specification). Special inputs as the
 * hardware gives them (measured on Zen 3; Intel's SDM says the same), whatever MXCSR holds:
 *   - zero or denormal input (treated as zero): rcp/rsqrt = signed infinity;
 *   - rcp of |x| > 2^126 (a tiny result): signed zero; rcp(inf) = signed zero;
 *   - rsqrt(+inf) = +0; rsqrt of a negative number or -inf = the default NaN 0xffc00000;
 *   - NaN input: the same NaN, quieted.
 * The Jaguar's own tables are not public, so this is not the console's result: it is a fixed
 * rule, the same on every PC (Linux and Windows), so the peers agree with each other.
 *
 * The same switch sets each guest thread's MXCSR to the PS4's 0x9fc0 (exceptions masked,
 * flush-to-zero, denormals-are-zero) and the x87 control word to 0x037f, as Orbis (and
 * shadPS4) do; without it threads keep the host's 0x1f80. That changes results with
 * denormals (closer to the console, but different from playing offline without it).
 *
 * The patch: a linear sweep of each executable segment with Zydis finds the instructions and
 * every direct branch target (relative jumps and calls, RIP-relative addresses into code and
 * the image's absolute pointers). An instruction shorter than the 5-byte `jmp rel32` takes the
 * next instructions with it into the stub (copied, RIP-relative displacements adjusted);
 * that is refused when one of those is a branch target or changes control flow, and the
 * site then keeps the CPU's instruction (counted in the log). */
#include "runtime.h"
#include <Zydis/Zydis.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int runtime_fp_deterministic(void) {
    static int value = -1;
    if (value < 0) {
        const char *forced = getenv("BB_DETERMINISTIC_FP"), *server = getenv("BB_ONLINE_SERVER");
        value = forced && *forced ? forced[0] == '1' : server && *server;
    }
    return value;
}

/* Orbis' floating-point environment, set on every thread that runs guest code (attach). */
void runtime_fp_thread_setup(void) {
    if (!runtime_fp_deterministic()) return;
    uint32_t mxcsr = 0x9fc0;
    uint16_t control = 0x037f;
    __asm__ volatile("ldmxcsr %0\n\tfldcw %1" : : "m"(mxcsr), "m"(control));
}

/* The rule in C (tests/test_deterministic_fp.c checks the stubs against it). */
static uint32_t round_significand(uint32_t bits) { return (bits + 0x3ff + (bits >> 11 & 1)) & ~UINT32_C(0x7ff); }
static float as_float(uint32_t bits) { float f; memcpy(&f, &bits, 4); return f; }
static uint32_t as_bits(float f) { uint32_t bits; memcpy(&bits, &f, 4); return bits; }
uint32_t runtime_fp_rcp(uint32_t x) {
    const uint32_t a = x & 0x7fffffff, sign = x & 0x80000000;
    if (a > 0x7f800000) return x | 0x00400000;
    if (a < 0x00800000) return sign | 0x7f800000;
    if (a > 0x7e800000) return sign;
    volatile float one = 1.0f; /* the division happens at run time, under the caller's MXCSR */
    return round_significand(as_bits(one / as_float(x)));
}
uint32_t runtime_fp_rsqrt(uint32_t x) {
    const uint32_t a = x & 0x7fffffff, sign = x & 0x80000000;
    if (a > 0x7f800000) return x | 0x00400000;
    if (a < 0x00800000) return sign | 0x7f800000;
    if (sign) return 0xffc00000;
    if (a == 0x7f800000) return 0;
    volatile float one = 1.0f;
    float root;
    __asm__("sqrtss %1, %0" : "=x"(root) : "x"(as_float(x)));
    return round_significand(as_bits(one / root));
}

/* The stubs' arithmetic. Called from a site stub with the operand in a 32-byte slot at [rsp+8]
 * (the stub has stepped below the guest's 128-byte red zone); computed in place. They change
 * no general register and no flag (only SIMD instructions), and restore the full 256 bits of
 * the xmm registers they use. Packed: 4 or 8 lanes; scalar: lane 0 only. */
#define FP_SAVE                                                                                    \
    "lea rsp, [rsp-224]\n vmovdqu [rsp], ymm0\n vmovdqu [rsp+32], ymm1\n vmovdqu [rsp+64], ymm2\n"  \
    "vmovdqu [rsp+96], ymm3\n vmovdqu [rsp+128], ymm4\n vmovdqu [rsp+160], ymm5\n vmovdqu [rsp+192], ymm6\n"
#define FP_RESTORE                                                                                 \
    "vmovdqu ymm0, [rsp]\n vmovdqu ymm1, [rsp+32]\n vmovdqu ymm2, [rsp+64]\n vmovdqu ymm3, [rsp+96]\n" \
    "vmovdqu ymm4, [rsp+128]\n vmovdqu ymm5, [rsp+160]\n vmovdqu ymm6, [rsp+192]\n lea rsp, [rsp+224]\n ret\n"
/* xmm0: x. xmm1 |x|, xmm2 sign, xmm3 |x| >= smallest normal, xmm4 lanes computed, xmm5 result. */
#define FP_CLASSIFY                                                                                \
    "vpand xmm1, xmm0, [rip+bb_fp_k+0]\n vpand xmm2, xmm0, [rip+bb_fp_k+16]\n"                       \
    "vpcmpgtd xmm3, xmm1, [rip+bb_fp_k+32]\n"
/* R(): the significand rounded to 12 fraction bits, ties to even (xmm5; xmm6 scratch). */
#define FP_ROUND                                                                                   \
    "vpsrld xmm6, xmm5, 11\n vpand xmm6, xmm6, [rip+bb_fp_k+112]\n vpaddd xmm5, xmm5, xmm6\n"         \
    "vpaddd xmm5, xmm5, [rip+bb_fp_k+128]\n vpand xmm5, xmm5, [rip+bb_fp_k+144]\n"
/* xmm2: the special-input results so far; NaN inputs give themselves quieted, then the
 * computed lanes (xmm4) take xmm5. Result in xmm0. */
#define FP_FINISH                                                                                  \
    "vpcmpgtd xmm3, xmm1, [rip+bb_fp_k+64]\n vpor xmm0, xmm0, [rip+bb_fp_k+80]\n"                    \
    "vblendvps xmm2, xmm2, xmm0, xmm3\n vblendvps xmm0, xmm2, xmm5, xmm4\n"
#define FP_RCP                                                                                     \
    FP_CLASSIFY                                                                                    \
    "vmovdqa xmm4, [rip+bb_fp_k+48]\n vpcmpgtd xmm4, xmm4, xmm1\n vpand xmm4, xmm4, xmm3\n"          \
    "vmovaps xmm6, [rip+bb_fp_k+96]\n vblendvps xmm5, xmm6, xmm0, xmm4\n vdivps xmm5, xmm6, xmm5\n"   \
    FP_ROUND                                                                                       \
    "vpor xmm6, xmm2, [rip+bb_fp_k+64]\n vblendvps xmm2, xmm6, xmm2, xmm3\n"                         \
    FP_FINISH
#define FP_RSQRT                                                                                   \
    FP_CLASSIFY                                                                                    \
    "vmovdqa xmm4, [rip+bb_fp_k+64]\n vpcmpgtd xmm4, xmm4, xmm0\n vpcmpgtd xmm6, xmm0, [rip+bb_fp_k+32]\n" \
    "vpand xmm4, xmm4, xmm6\n"                                                                     \
    "vmovaps xmm6, [rip+bb_fp_k+96]\n vblendvps xmm5, xmm6, xmm0, xmm4\n vsqrtps xmm5, xmm5\n"        \
    "vdivps xmm5, xmm6, xmm5\n"                                                                    \
    FP_ROUND                                                                                       \
    "vpsrad xmm6, xmm0, 31\n vpand xmm6, xmm6, [rip+bb_fp_k+160]\n vpor xmm2, xmm2, [rip+bb_fp_k+64]\n" \
    "vblendvps xmm2, xmm2, xmm6, xmm3\n"                                                           \
    FP_FINISH
#define FP_PACKED(name, body)                                                                      \
    ".globl " #name "4\n.p2align 4\n" #name "4:\n" FP_SAVE "vmovdqu xmm0, [rsp+232]\n" body          \
    "vmovdqu [rsp+232], xmm0\n" FP_RESTORE                                                         \
    ".globl " #name "8\n.p2align 4\n" #name "8:\n" FP_SAVE "vmovdqu xmm0, [rsp+232]\n" body          \
    "vmovdqu [rsp+232], xmm0\n vmovdqu xmm0, [rsp+248]\n" body "vmovdqu [rsp+248], xmm0\n" FP_RESTORE \
    ".globl " #name "1\n.p2align 4\n" #name "1:\n" FP_SAVE "vmovdqu xmm0, [rsp+232]\n" body          \
    "vmovd [rsp+232], xmm0\n" FP_RESTORE
__asm__(".text\n.intel_syntax noprefix\n"
        ".p2align 4\nbb_fp_k:\n"
        ".long 0x7fffffff,0x7fffffff,0x7fffffff,0x7fffffff\n" /* +0   |x| */
        ".long 0x80000000,0x80000000,0x80000000,0x80000000\n" /* +16  sign */
        ".long 0x007fffff,0x007fffff,0x007fffff,0x007fffff\n" /* +32  below: zero or denormal */
        ".long 0x7e800001,0x7e800001,0x7e800001,0x7e800001\n" /* +48  rcp: below, 1/x is normal */
        ".long 0x7f800000,0x7f800000,0x7f800000,0x7f800000\n" /* +64  infinity; above: NaN */
        ".long 0x00400000,0x00400000,0x00400000,0x00400000\n" /* +80  quiet bit */
        ".long 0x3f800000,0x3f800000,0x3f800000,0x3f800000\n" /* +96  1.0f */
        ".long 1,1,1,1\n"                                     /* +112 */
        ".long 0x3ff,0x3ff,0x3ff,0x3ff\n"                     /* +128 */
        ".long 0xfffff800,0xfffff800,0xfffff800,0xfffff800\n" /* +144 12 fraction bits */
        ".long 0xffc00000,0xffc00000,0xffc00000,0xffc00000\n" /* +160 default NaN */
        FP_PACKED(bb_fp_rcp, FP_RCP) FP_PACKED(bb_fp_rsqrt, FP_RSQRT)
        ".att_syntax prefix\n");
void bb_fp_rcp4(void), bb_fp_rcp8(void), bb_fp_rcp1(void), bb_fp_rsqrt4(void), bb_fp_rsqrt8(void), bb_fp_rsqrt1(void);

/* ---- the patch ---- */
enum { JMP = 5, MAX_STUB = 384 };
/* Where an instruction's operand forms put it: legacy packed, VEX packed 128/256, scalar. */
enum { LEGACY, VEX128, VEX256 };
typedef struct {
    int rsqrt, scalar, form;
    int dst, merge;     /* xmm numbers: destination; VEX scalar: the source of lanes 1-3 */
    int src_reg;        /* the source register, or -1 for memory */
    int index, base;    /* memory: GPR numbers (-1 none), base -2 = RIP */
    const unsigned char *modrm; size_t modrm_bytes; /* ModRM, SIB and displacement as encoded */
    int disp_at;        /* offset of a RIP-relative disp32 within those bytes, or -1 */
    uint64_t target;    /* RIP-relative: the image offset addressed */
} Site;

static int xmm_number(ZydisRegister r) {
    if (r >= ZYDIS_REGISTER_XMM0 && r <= ZYDIS_REGISTER_XMM15) return r - ZYDIS_REGISTER_XMM0;
    if (r >= ZYDIS_REGISTER_YMM0 && r <= ZYDIS_REGISTER_YMM15) return r - ZYDIS_REGISTER_YMM0;
    return -1;
}
static int gpr_number(ZydisRegister r) {
    if (r == ZYDIS_REGISTER_NONE) return -1;
    if (r == ZYDIS_REGISTER_RIP) return -2;
    if (r >= ZYDIS_REGISTER_RAX && r <= ZYDIS_REGISTER_R15) return r - ZYDIS_REGISTER_RAX;
    return -3;
}
/* Recognizes the eight instructions in the forms this file handles; 0 for anything else. */
static int describe(const ZydisDecodedInstruction *in, const ZydisDecodedOperand *op, const unsigned char *code, uint64_t offset, Site *s) {
    memset(s, 0, sizeof(*s));
    switch (in->mnemonic) {
    case ZYDIS_MNEMONIC_RCPPS: case ZYDIS_MNEMONIC_VRCPPS: break;
    case ZYDIS_MNEMONIC_RSQRTPS: case ZYDIS_MNEMONIC_VRSQRTPS: s->rsqrt = 1; break;
    case ZYDIS_MNEMONIC_RCPSS: case ZYDIS_MNEMONIC_VRCPSS: s->scalar = 1; break;
    case ZYDIS_MNEMONIC_RSQRTSS: case ZYDIS_MNEMONIC_VRSQRTSS: s->scalar = s->rsqrt = 1; break;
    default: return 0;
    }
    const int vex = in->encoding == ZYDIS_INSTRUCTION_ENCODING_VEX;
    if (!vex && in->encoding != ZYDIS_INSTRUCTION_ENCODING_LEGACY) return 0;
    s->form = !vex ? LEGACY : in->avx.vector_length == 256 ? VEX256 : VEX128;
    if (s->scalar && s->form == VEX256) s->form = VEX128; /* VEX.LIG */
    const int src = vex && s->scalar ? 2 : 1;
    if (op[0].type != ZYDIS_OPERAND_TYPE_REGISTER || (s->dst = xmm_number(op[0].reg.value)) < 0) return 0;
    s->merge = s->dst;
    if (vex && s->scalar && (op[1].type != ZYDIS_OPERAND_TYPE_REGISTER || (s->merge = xmm_number(op[1].reg.value)) < 0)) return 0;
    s->src_reg = -1;
    if (op[src].type == ZYDIS_OPERAND_TYPE_REGISTER) {
        if ((s->src_reg = xmm_number(op[src].reg.value)) < 0) return 0;
        return 1;
    }
    if (op[src].type != ZYDIS_OPERAND_TYPE_MEMORY || in->address_width != 64 ||
        (op[src].mem.segment != ZYDIS_REGISTER_DS && op[src].mem.segment != ZYDIS_REGISTER_SS) ||
        (in->attributes & ZYDIS_ATTRIB_HAS_SEGMENT))
        return 0;
    s->base = gpr_number(op[src].mem.base); s->index = gpr_number(op[src].mem.index);
    if (s->base == -3 || s->index < -1) return 0;
    s->modrm = code + in->raw.modrm.offset;
    s->modrm_bytes = in->length - in->raw.modrm.offset; /* no immediate: ModRM, SIB, displacement */
    s->disp_at = -1;
    if (s->base == -2) {
        if (in->raw.disp.size != 32) return 0;
        s->disp_at = in->raw.disp.offset - in->raw.modrm.offset;
        s->target = offset + in->length + (uint64_t)in->raw.disp.value;
    }
    return 1;
}

static int rel32(const unsigned char *from_end, const void *to, int32_t *out) {
    const intptr_t d = (intptr_t)((const unsigned char *)to - from_end);
    if (d < INT32_MIN || d > INT32_MAX) return 0;
    *out = (int32_t)d;
    return 1;
}
/* "opcode xmm(reg), r/m" with the site's source operand as r/m; VEX is always the 3-byte form.
 * pp: 0 none, 1 66, 2 F3; map: 1 0F, 3 0F3A. Returns the end, or NULL when a RIP-relative
 * displacement no longer reaches. */
static unsigned char *encode(unsigned char *p, const Site *s, const unsigned char *image, int vex, int L, int pp,
                             int map, int vvvv, uint8_t opcode, int imm) {
    const int reg = s->dst, rm_reg = s->src_reg;
    const int R = reg >> 3, X = rm_reg < 0 && s->index >= 0 ? s->index >> 3 : 0,
              B = rm_reg >= 0 ? rm_reg >> 3 : s->base >= 0 ? s->base >> 3 : 0;
    if (vex) {
        *p++ = 0xc4;
        *p++ = (uint8_t)((!R) << 7 | (!X) << 6 | (!B) << 5 | map);
        *p++ = (uint8_t)((~vvvv & 15) << 3 | L << 2 | pp);
    } else {
        if (pp == 1) *p++ = 0x66;
        if (pp == 2) *p++ = 0xf3;
        if (R | X | B) *p++ = (uint8_t)(0x40 | R << 2 | X << 1 | B);
        *p++ = 0x0f;
        if (map == 3) *p++ = 0x3a;
    }
    *p++ = opcode;
    if (rm_reg >= 0) *p++ = (uint8_t)(0xc0 | (reg & 7) << 3 | (rm_reg & 7));
    else {
        unsigned char *m = p;
        memcpy(p, s->modrm, s->modrm_bytes);
        m[0] = (uint8_t)((m[0] & 0xc7) | (reg & 7) << 3);
        p += s->modrm_bytes;
        if (imm >= 0) *p++ = (uint8_t)imm;
        if (s->disp_at >= 0) {
            int32_t d;
            if (!rel32(p, image + s->target, &d)) return NULL;
            memcpy(m + s->disp_at, &d, 4);
        }
        return p;
    }
    if (imm >= 0) *p++ = (uint8_t)imm;
    return p;
}
/* movups/vmovups [rsp] <-> xmm/ymm `reg` (store 0x11, load 0x10). */
static unsigned char *slot_move(unsigned char *p, int vex, int L, int reg, uint8_t opcode) {
    if (vex) { *p++ = 0xc5; *p++ = (uint8_t)((!(reg >> 3)) << 7 | 0x78 | L << 2); }
    else if (reg >> 3) *p++ = 0x44;
    if (!vex) *p++ = 0x0f;
    *p++ = opcode; *p++ = (uint8_t)(0x04 | (reg & 7) << 3); *p++ = 0x24;
    return p;
}
static unsigned char *lea_rsp(unsigned char *p, int32_t by) {
    *p++ = 0x48; *p++ = 0x8d; *p++ = 0xa4; *p++ = 0x24; memcpy(p, &by, 4);
    return p + 4;
}
/* The site's result into its destination register, as the instruction leaves it: the source
 * is loaded into the destination with the same operand (movups/movss/insertps, keeping the
 * lanes and upper bits the instruction keeps), then computed in place by a core routine. */
static unsigned char *emit_site(unsigned char *p, const Site *s, const unsigned char *image, unsigned char *const *cores) {
    const int vex = s->form != LEGACY, L = s->form == VEX256;
    if (s->src_reg == s->dst && s->merge == s->dst) {} /* already there; the VEX reload below zeroes the upper bits */
    else if (!s->scalar) p = encode(p, s, image, vex, L, 0, 1, 0, 0x10, -1);            /* (v)movups dst, src */
    else if (s->src_reg >= 0) p = encode(p, s, image, vex, 0, 2, 1, s->merge, 0x10, -1); /* (v)movss dst, [merge,] src */
    else p = encode(p, s, image, vex, 0, 1, 3, s->merge, 0x21, 0);                 /* (v)insertps dst, [merge,] m32, 0 */
    if (!p) return NULL;
    p = lea_rsp(p, -160); /* below the red zone: [rsp] is a 32-byte slot */
    p = slot_move(p, vex, L, s->dst, 0x11);
    const int core = s->rsqrt * 3 + (s->scalar ? 2 : L);
    int32_t d;
    p[0] = 0xff; p[1] = 0x15; /* call [rip+d]: the core's address in the table at the area's start */
    if (!rel32(p + 6, cores + core, &d)) return NULL;
    memcpy(p + 2, &d, 4);
    p += 6;
    p = slot_move(p, vex, L, s->dst, 0x10);
    return lea_rsp(p, 160);
}

typedef struct {
    unsigned char *image;
    const uint64_t *seg_address, *seg_size, *seg_flags;
    uint64_t segments;
    unsigned char *targets; /* bitmap: image offsets some instruction or pointer goes to */
} Scan;
static int is_target(const Scan *c, uint64_t offset) { return c->targets[offset >> 3] >> (offset & 7) & 1; }
static void mark(Scan *c, uint64_t offset, uint64_t limit) {
    if (offset < limit) c->targets[offset >> 3] |= (unsigned char)(1u << (offset & 7));
}
/* An instruction that can run from the stub unchanged (RIP-relative operands are adjusted). */
static int movable(const ZydisDecodedInstruction *in) {
    if (in->attributes & ZYDIS_ATTRIB_IS_RELATIVE && !(in->raw.modrm.mod == 0 && in->raw.modrm.rm == 5)) return 0;
    switch (in->meta.category) {
    case ZYDIS_CATEGORY_COND_BR: case ZYDIS_CATEGORY_UNCOND_BR: case ZYDIS_CATEGORY_CALL: case ZYDIS_CATEGORY_RET:
    case ZYDIS_CATEGORY_SYSCALL: case ZYDIS_CATEGORY_INTERRUPT: case ZYDIS_CATEGORY_SYSTEM: return 0;
    default: return in->mnemonic != ZYDIS_MNEMONIC_INVALID;
    }
}

/* The functions the unwind tables describe: each .eh_frame_hdr in the image (one per linked
 * module; found by its usual encodings, then checked) lists every FDE, and each FDE gives one
 * function's [start, end). Sweeping those, from each function's first byte, keeps the decoder
 * aligned and off the read-only data the PS4 linker places in the executable segment. */
typedef struct { uint64_t start, end; } Range;
static int in_executable(const Scan *c, uint64_t start, uint64_t end) {
    for (uint64_t g = 0; g < c->segments; ++g)
        if ((c->seg_flags[g] & 1) && start >= c->seg_address[g] && end <= c->seg_address[g] + c->seg_size[g] && start < end) return 1;
    return 0;
}
/* A 4-byte DWARF pointer (sdata4/udata4), PC- or data-relative to image offsets; 0 otherwise. */
static int read_pointer(const unsigned char *image, uint64_t size, uint64_t *at, uint8_t encoding, uint64_t data, int apply, uint64_t *out) {
    uint32_t u;
    if ((encoding & 0x07) != 0x03 || *at + 4 > size) return 0;
    memcpy(&u, image + *at, 4);
    uint64_t value = encoding & 0x08 ? (uint64_t)(int64_t)(int32_t)u : u;
    if (apply) {
        if ((encoding & 0x70) == 0x10) value += *at;
        else if ((encoding & 0x70) == 0x30) value += data;
        else return 0;
    }
    *at += 4; *out = value;
    return 1;
}
static uint64_t uleb(const unsigned char *image, uint64_t size, uint64_t *at) {
    uint64_t value = 0;
    for (int shift = 0; *at < size && shift < 64; shift += 7) {
        const unsigned char b = image[(*at)++];
        value |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) break;
    }
    return value;
}
/* The pointer encoding of the FDEs that use the CIE at `cie` ('R' augmentation), or -1. */
static int fde_encoding(const unsigned char *image, uint64_t size, uint64_t cie) {
    uint32_t length, id;
    if (cie + 9 > size) return -1;
    memcpy(&length, image + cie, 4); memcpy(&id, image + cie + 4, 4);
    if (length == 0xffffffff || id != 0 || cie + 4 + length > size) return -1;
    const uint64_t end = cie + 4 + length;
    uint64_t at = cie + 8;
    const int version = image[at++];
    const char *augmentation = (const char *)image + at;
    const char *nul = memchr(augmentation, 0, (size_t)(end - at));
    if (!nul) return -1;
    const size_t n = (size_t)(nul - augmentation);
    at += n + 1;
    if (augmentation[0] != 'z') return augmentation[0] ? -1 : 0x00;
    uleb(image, end, &at); uleb(image, end, &at); /* code and data alignment */
    if (version == 1) ++at; else uleb(image, end, &at);
    uleb(image, end, &at); /* augmentation data length */
    for (size_t i = 1; i < n && at < end; ++i) {
        if (augmentation[i] == 'R') return image[at];
        if (augmentation[i] == 'L') ++at;
        else if (augmentation[i] == 'P') {
            const uint8_t encoding = image[at++];
            const int bytes[] = {8, 0, 2, 4, 8, 0, 0, 0, 8, 0, 2, 4, 8};
            if ((encoding & 0x0f) > 12 || !bytes[encoding & 0x0f]) return -1;
            at += (uint64_t)bytes[encoding & 0x0f];
        } else if (augmentation[i] != 'S' && augmentation[i] != 'B') return -1;
    }
    return -1;
}
static int by_start(const void *a, const void *b) {
    const Range *x = a, *y = b;
    return x->start < y->start ? -1 : x->start > y->start;
}
static Range *unwind_ranges(const Scan *c, uint64_t size, uint64_t *count) {
    Range *ranges = NULL;
    uint64_t n = 0, capacity = 0;
    for (uint64_t g = 0; g < c->segments; ++g) {
        const uint64_t begin = c->seg_address[g], end = c->seg_address[g] + c->seg_size[g];
        for (uint64_t hdr = begin; hdr + 12 <= end; hdr += 4) {
            /* version 1, eh_frame_ptr pcrel|sdata4, fde_count udata4, table datarel|sdata4 */
            if (memcmp(c->image + hdr, "\x01\x1b\x03\x3b", 4)) continue;
            uint32_t fdes;
            memcpy(&fdes, c->image + hdr + 8, 4);
            if (!fdes || (uint64_t)fdes > (end - hdr - 12) / 8) continue;
            uint64_t found = 0, previous = 0;
            for (uint64_t i = 0; i < fdes; ++i) {
                uint64_t at = hdr + 12 + 8 * i, location, fde, start, length;
                if (!read_pointer(c->image, size, &at, 0x3b, hdr, 1, &location) ||
                    !read_pointer(c->image, size, &at, 0x3b, hdr, 1, &fde) || location < previous || fde + 12 > size) break;
                previous = location;
                uint32_t cie_offset;
                memcpy(&cie_offset, c->image + fde + 4, 4);
                if (cie_offset > fde + 4) break;
                const int encoding = fde_encoding(c->image, size, fde + 4 - cie_offset);
                at = fde + 8;
                if (encoding < 0 || !read_pointer(c->image, size, &at, (uint8_t)encoding, hdr, 1, &start) ||
                    !read_pointer(c->image, size, &at, (uint8_t)(encoding & 0x0f), 0, 0, &length) || start != location)
                    break;
                if (!in_executable(c, start, start + length)) continue;
                if (n == capacity) {
                    Range *grown = realloc(ranges, (capacity = capacity ? capacity * 2 : 65536) * sizeof(*ranges));
                    if (!grown) { free(ranges); *count = 0; return NULL; }
                    ranges = grown;
                }
                ranges[n++] = (Range){start, start + length};
                ++found;
            }
            if (found) hdr += 12 + 8 * (uint64_t)fdes - 4;
        }
    }
    if (n) qsort(ranges, n, sizeof(*ranges), by_start);
    for (uint64_t i = 1; i < n; ++i)
        if (ranges[i].start < ranges[i - 1].end) ranges[i].start = ranges[i - 1].end; /* never overlap */
    *count = n;
    return ranges;
}
/* jcc or jmp with a rel8/rel32 target (not jrcxz or loop, which have no rel32 form). */
static int relative_jump(const ZydisDecodedInstruction *in) {
    if ((in->meta.category != ZYDIS_CATEGORY_COND_BR && in->meta.category != ZYDIS_CATEGORY_UNCOND_BR) || !in->raw.imm[0].is_relative)
        return 0;
    if (in->opcode_map == ZYDIS_OPCODE_MAP_DEFAULT) return (in->opcode >= 0x70 && in->opcode <= 0x7f) || in->opcode == 0xeb || in->opcode == 0xe9;
    return in->opcode_map == ZYDIS_OPCODE_MAP_0F && in->opcode >= 0x80 && in->opcode <= 0x8f;
}

uint64_t runtime_fp_patch_image(unsigned char *image, uint64_t size, const uint64_t *seg_address, const uint64_t *seg_size,
                                const uint64_t *seg_flags, uint64_t segments, const uint64_t *pointers, uint64_t pointer_count,
                                void *(*map_near)(size_t), RuntimeFpStats *stats) {
    memset(stats, 0, sizeof(*stats));
    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    Scan c = {image, seg_address, seg_size, seg_flags, segments, calloc(size / 8 + 1, 1)};
    uint64_t *sites = NULL, count = 0, capacity = 0, range_count = 0;
    if (!c.targets) return 0;
    for (uint64_t i = 0; i < pointer_count; ++i) mark(&c, pointers[i], size);
    Range *ranges = unwind_ranges(&c, size, &range_count);
    if (!range_count) { /* no unwind tables (a test image): whole executable segments */
        free(ranges);
        if (!(ranges = calloc(segments ? segments : 1, sizeof(*ranges)))) { free(c.targets); return 0; }
        for (uint64_t g = 0; g < segments; ++g)
            if (seg_flags[g] & 1) ranges[range_count++] = (Range){seg_address[g], seg_address[g] + seg_size[g]};
    } else stats->functions = range_count;
    /* Pass 1: the sites and every direct target. */
    for (uint64_t r = 0; r < range_count; ++r) {
        for (uint64_t at = ranges[r].start, end = ranges[r].end; at < end;) {
            ZydisDecodedInstruction in;
            if (!ZYAN_SUCCESS(ZydisDecoderDecodeInstruction(&decoder, NULL, image + at, end - at, &in))) { ++at; continue; }
            const uint64_t next = at + in.length;
            if (in.raw.imm[0].is_relative) mark(&c, next + (uint64_t)in.raw.imm[0].value.s, size);
            if (in.raw.modrm.offset && in.raw.modrm.mod == 0 && in.raw.modrm.rm == 5 && in.raw.disp.size == 32)
                mark(&c, next + (uint64_t)in.raw.disp.value, size);
            switch (in.mnemonic) {
            case ZYDIS_MNEMONIC_RCPPS: case ZYDIS_MNEMONIC_VRCPPS: case ZYDIS_MNEMONIC_RSQRTPS: case ZYDIS_MNEMONIC_VRSQRTPS:
            case ZYDIS_MNEMONIC_RCPSS: case ZYDIS_MNEMONIC_VRCPSS: case ZYDIS_MNEMONIC_RSQRTSS: case ZYDIS_MNEMONIC_VRSQRTSS:
                if (count == capacity) {
                    uint64_t *grown = realloc(sites, (capacity = capacity ? capacity * 2 : 4096) * 2 * sizeof(*sites));
                    if (!grown) { free(sites); free(ranges); free(c.targets); return 0; }
                    sites = grown;
                }
                sites[2 * count] = at; sites[2 * count++ + 1] = end;
                break;
            default: break;
            }
            at = next;
        }
    }
    free(ranges);
    stats->found = count;
    const size_t table = 6 * sizeof(void *), area_size = table + (size_t)count * MAX_STUB;
    unsigned char *area = count ? map_near(area_size) : NULL;
    if (!area) { stats->unpatched = count; free(sites); free(c.targets); return 0; }
    unsigned char **cores = (unsigned char **)area;
    cores[0] = (unsigned char *)bb_fp_rcp4; cores[1] = (unsigned char *)bb_fp_rcp8; cores[2] = (unsigned char *)bb_fp_rcp1;
    cores[3] = (unsigned char *)bb_fp_rsqrt4; cores[4] = (unsigned char *)bb_fp_rsqrt8; cores[5] = (unsigned char *)bb_fp_rsqrt1;
    unsigned char *p = area + table;
    uint64_t covered_until = 0;
    /* Pass 2: a stub per site; the instructions it displaces go with it. */
    for (uint64_t i = 0; i < count; ++i) {
        const uint64_t at = sites[2 * i], end = sites[2 * i + 1]; /* displaced code stays in the function */
        if (at < covered_until) continue; /* emulated inside the previous site's stub */
        unsigned char *stub = p, *q = p;
        uint64_t length = 0, emulated = 0;
        int ok = 1;
        while (ok && length < JMP) {
            ZydisDecodedInstruction in;
            ZydisDecodedOperand op[ZYDIS_MAX_OPERAND_COUNT];
            const uint64_t here = at + length;
            if (here >= end || (length && is_target(&c, here)) ||
                !ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, image + here, end - here, &in, op))) { ok = 0; break; }
            Site s;
            if (describe(&in, op, image + here, here, &s)) {
                if (!(q = emit_site(q, &s, image, cores))) ok = 0;
                ++emulated;
            } else if (length && length + in.length >= JMP && relative_jump(&in)) {
                /* The last displaced instruction may be a jump (jcc/jmp, not jrcxz/loop): rel32 to
                 * the same target; the flags it tests are the guest's (no stub changes them). */
                const unsigned char *target = image + here + in.length + in.raw.imm[0].value.s;
                if (in.opcode == 0xeb || in.opcode == 0xe9) *q++ = 0xe9;
                else { *q++ = 0x0f; *q++ = (uint8_t)(0x80 | (in.opcode & 0x0f)); }
                int32_t d = 0;
                if (!rel32(q + 4, target, &d)) ok = 0;
                memcpy(q, &d, 4);
                q += 4;
            } else if (!length || !movable(&in)) ok = 0;
            else {
                memcpy(q, image + here, in.length);
                if (in.raw.modrm.offset && in.raw.modrm.mod == 0 && in.raw.modrm.rm == 5) { /* RIP-relative */
                    int32_t d;
                    if (!rel32(q + in.length, image + here + in.length + in.raw.disp.value, &d)) ok = 0;
                    else memcpy(q + in.raw.disp.offset, &d, 4);
                }
                q += in.length;
            }
            length += in.length;
        }
        int32_t back = 0, to_stub = 0;
        if (ok) {
            q[0] = 0xe9;
            ok = rel32(q + 5, image + at + length, &back) && rel32(image + at + JMP, stub, &to_stub);
        }
        if (!ok) {
            if (stats->unpatched++ < 8) printf("Deterministic FP: the instruction at image offset 0x%" PRIx64 " is kept (the next ones cannot move)\n", at);
            continue;
        }
        memcpy(q + 1, &back, 4);
        p = q + 5;
        image[at] = 0xe9;
        memcpy(image + at + 1, &to_stub, 4);
        memset(image + at + JMP, 0xcc, length - JMP); /* never reached: none of it is a target */
        stats->patched += emulated;
        stats->displaced += length > JMP || emulated > 1 ? 1 : 0;
        covered_until = at + length;
    }
    stats->stub_bytes = (uint64_t)(p - area);
    if (platform_protect(area, area_size, PROT_READ | PROT_EXEC)) {
        fputs("STOP: deterministic FP: cannot make the stubs executable\n", stderr);
        exit(21);
    }
    free(sites); free(c.targets);
    return stats->patched;
}
