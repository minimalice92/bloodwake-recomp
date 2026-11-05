/* SPDX-License-Identifier: GPL-2.0-or-later (part of the DSP engine; see NOTICE.md) */
/*
 * bw_dsp_x64.inc - a small x64 instruction encoder for the DSP JIT
 * (bw_dsp_fast.c includes it). Only what the JIT emits: moves, loads and
 * stores relative to a base register (with an optional index), the integer
 * ALU, shifts, multiplies, setcc/cmovcc, jumps and calls. Every encoding is
 * the plain one from the Intel manual; nothing clever.
 */

enum {
    X_RAX, X_RCX, X_RDX, X_RBX, X_RSP, X_RBP, X_RSI, X_RDI,
    X_R8, X_R9, X_R10, X_R11, X_R12, X_R13, X_R14, X_R15
};

/* Condition codes (the low nibble of Jcc / SETcc / CMOVcc). */
enum {
    CC_O = 0, CC_NO = 1, CC_B = 2, CC_AE = 3, CC_E = 4, CC_NE = 5, CC_BE = 6, CC_A = 7,
    CC_S = 8, CC_NS = 9, CC_L = 12, CC_GE = 13, CC_LE = 14, CC_G = 15
};

/* ALU group (opcode = op * 8 + 1 for r/m <- reg, + 3 for reg <- r/m, /op for imm). */
enum { A_ADD = 0, A_OR = 1, A_ADC = 2, A_SBB = 3, A_AND = 4, A_SUB = 5, A_XOR = 6, A_CMP = 7 };
/* Shift group (/op). */
enum { S_SHL = 4, S_SHR = 5, S_SAR = 7 };

typedef struct {
    uint8_t *p, *end;
    int bad;
} Xe;

static void xb(Xe *x, uint8_t b)
{
    if (x->p < x->end)
        *x->p++ = b;
    else
        x->bad = 1;
}

static void xd(Xe *x, uint32_t v)
{
    xb(x, (uint8_t)v);
    xb(x, (uint8_t)(v >> 8));
    xb(x, (uint8_t)(v >> 16));
    xb(x, (uint8_t)(v >> 24));
}

static void xq(Xe *x, uint64_t v)
{
    xd(x, (uint32_t)v);
    xd(x, (uint32_t)(v >> 32));
}

/* REX prefix: w 64-bit, reg / index / base the registers in ModRM.reg,
 * SIB.index and ModRM.rm (or SIB.base). force: emit it even when empty (for
 * the byte registers spl, bpl, sil, dil). */
static void xrex(Xe *x, int w, int reg, int index, int base, int force)
{
    uint8_t r = (uint8_t)(0x40 | (w ? 8 : 0) | ((reg & 8) ? 4 : 0) | ((index & 8) ? 2 : 0) |
                          ((base & 8) ? 1 : 0));
    if (r != 0x40 || force)
        xb(x, r);
}

static void xmod_rr(Xe *x, int reg, int rm)
{
    xb(x, (uint8_t)(0xC0 | ((reg & 7) << 3) | (rm & 7)));
}

/* [base + disp] */
static void xmod_m(Xe *x, int reg, int base, int32_t disp)
{
    int mod = (disp == 0 && (base & 7) != 5) ? 0 : (disp >= -128 && disp <= 127) ? 1 : 2;
    xb(x, (uint8_t)((mod << 6) | ((reg & 7) << 3) | (base & 7)));
    if ((base & 7) == 4)
        xb(x, 0x24);
    if (mod == 1)
        xb(x, (uint8_t)disp);
    else if (mod == 2)
        xd(x, (uint32_t)disp);
}

/* [base + index * scale + disp] (index never rsp) */
static void xmod_sib(Xe *x, int reg, int base, int index, int scale, int32_t disp)
{
    int ss = scale == 1 ? 0 : scale == 2 ? 1 : scale == 4 ? 2 : 3;
    int mod = (disp == 0 && (base & 7) != 5) ? 0 : (disp >= -128 && disp <= 127) ? 1 : 2;
    xb(x, (uint8_t)((mod << 6) | ((reg & 7) << 3) | 4));
    xb(x, (uint8_t)((ss << 6) | ((index & 7) << 3) | (base & 7)));
    if (mod == 1)
        xb(x, (uint8_t)disp);
    else if (mod == 2)
        xd(x, (uint32_t)disp);
}

/* mov dst, [base + disp] (w: 64-bit) */
static void x_load(Xe *x, int w, int dst, int base, int32_t disp)
{
    xrex(x, w, dst, 0, base, 0);
    xb(x, 0x8B);
    xmod_m(x, dst, base, disp);
}

/* mov [base + disp], src */
static void x_store(Xe *x, int w, int base, int32_t disp, int src)
{
    xrex(x, w, src, 0, base, 0);
    xb(x, 0x89);
    xmod_m(x, src, base, disp);
}

/* mov dword [base + disp], imm32 */
static void x_store_i(Xe *x, int base, int32_t disp, uint32_t imm)
{
    xrex(x, 0, 0, 0, base, 0);
    xb(x, 0xC7);
    xmod_m(x, 0, base, disp);
    xd(x, imm);
}

/* lea dst64, [rip + disp32]: returns the disp32 to patch (x_patch) */
static uint8_t *x_lea_rip(Xe *x, int dst)
{
    xrex(x, 1, dst, 0, 0, 0);
    xb(x, 0x8D);
    xb(x, (uint8_t)(0x05 | ((dst & 7) << 3)));
    xd(x, 0);
    return x->p - 4;
}

/* jmp reg64 */
static void x_jmp_r(Xe *x, int reg)
{
    xrex(x, 0, 0, 0, reg, 0);
    xb(x, 0xFF);
    xb(x, (uint8_t)(0xE0 | (reg & 7)));
}

/* mov word [base + disp], imm16 */
static void x_store16_i(Xe *x, int base, int32_t disp, uint32_t imm)
{
    xb(x, 0x66);
    xrex(x, 0, 0, 0, base, 0);
    xb(x, 0xC7);
    xmod_m(x, 0, base, disp);
    xb(x, (uint8_t)(imm & 0xFF));
    xb(x, (uint8_t)((imm >> 8) & 0xFF));
}

/* mov dst, [base + index * scale + disp] (32-bit) */
static void x_load_idx(Xe *x, int dst, int base, int index, int scale, int32_t disp)
{
    xrex(x, 0, dst, index, base, 0);
    xb(x, 0x8B);
    xmod_sib(x, dst, base, index, scale, disp);
}

/* mov [base + index * scale + disp], src (32-bit) */
static void x_store_idx(Xe *x, int base, int index, int scale, int32_t disp, int src)
{
    xrex(x, 0, src, index, base, 0);
    xb(x, 0x89);
    xmod_sib(x, src, base, index, scale, disp);
}

static void x_mov(Xe *x, int w, int dst, int src)
{
    if (dst == src && w)
        return;
    xrex(x, w, src, 0, dst, 0);
    xb(x, 0x89);
    xmod_rr(x, src, dst);
}

/* mov dst32, imm32 (zero-extends) */
static void x_movi(Xe *x, int dst, uint32_t imm)
{
    xrex(x, 0, 0, 0, dst, 0);
    xb(x, (uint8_t)(0xB8 | (dst & 7)));
    xd(x, imm);
}

/* mov dst64, imm64 */
static void x_movi64(Xe *x, int dst, uint64_t imm)
{
    if (imm <= 0xFFFFFFFFull) {
        x_movi(x, dst, (uint32_t)imm);
        return;
    }
    xrex(x, 1, 0, 0, dst, 0);
    xb(x, (uint8_t)(0xB8 | (dst & 7)));
    xq(x, imm);
}

static void x_alu(Xe *x, int op, int w, int dst, int src)
{
    xrex(x, w, src, 0, dst, 0);
    xb(x, (uint8_t)(op * 8 + 1));
    xmod_rr(x, src, dst);
}

/* op dst, [base + disp] */
static void x_alu_m(Xe *x, int op, int w, int dst, int base, int32_t disp)
{
    xrex(x, w, dst, 0, base, 0);
    xb(x, (uint8_t)(op * 8 + 3));
    xmod_m(x, dst, base, disp);
}

/* op dst, imm32 (sign-extended for 64-bit) */
static void x_alui(Xe *x, int op, int w, int dst, int32_t imm)
{
    xrex(x, w, 0, 0, dst, 0);
    if (imm >= -128 && imm <= 127) {
        xb(x, 0x83);
        xmod_rr(x, op, dst);
        xb(x, (uint8_t)imm);
    } else {
        xb(x, 0x81);
        xmod_rr(x, op, dst);
        xd(x, (uint32_t)imm);
    }
}

/* op dword [base + disp], imm32 */
static void x_alui_m(Xe *x, int op, int base, int32_t disp, int32_t imm)
{
    xrex(x, 0, 0, 0, base, 0);
    if (imm >= -128 && imm <= 127) {
        xb(x, 0x83);
        xmod_m(x, op, base, disp);
        xb(x, (uint8_t)imm);
    } else {
        xb(x, 0x81);
        xmod_m(x, op, base, disp);
        xd(x, (uint32_t)imm);
    }
}

static void x_shift(Xe *x, int op, int w, int dst, int n)
{
    xrex(x, w, 0, 0, dst, 0);
    if (n == 1) {
        xb(x, 0xD1);
        xmod_rr(x, op, dst);
    } else {
        xb(x, 0xC1);
        xmod_rr(x, op, dst);
        xb(x, (uint8_t)n);
    }
}

/* imul dst, src (signed) */
static void x_imul(Xe *x, int w, int dst, int src)
{
    xrex(x, w, dst, 0, src, 0);
    xb(x, 0x0F);
    xb(x, 0xAF);
    xmod_rr(x, dst, src);
}

/* imul dst, src, imm8 (sign-extended) */
static void x_imul_i8(Xe *x, int w, int dst, int src, int imm)
{
    xrex(x, w, dst, 0, src, 0);
    xb(x, 0x6B);
    xmod_rr(x, dst, src);
    xb(x, (uint8_t)imm);
}

static void x_test(Xe *x, int w, int a, int b)
{
    xrex(x, w, b, 0, a, 0);
    xb(x, 0x85);
    xmod_rr(x, b, a);
}

/* test dst, imm32 */
static void x_testi(Xe *x, int w, int dst, uint32_t imm)
{
    xrex(x, w, 0, 0, dst, 0);
    xb(x, 0xF7);
    xmod_rr(x, 0, dst);
    xd(x, imm);
}

/* setcc dst8 (then the caller zero-extends) */
static void x_setcc(Xe *x, int cc, int dst)
{
    xrex(x, 0, 0, 0, dst, dst >= 4 && dst < 8);
    xb(x, 0x0F);
    xb(x, (uint8_t)(0x90 | cc));
    xmod_rr(x, 0, dst);
}

/* movzx dst32, src8 */
static void x_movzx8(Xe *x, int dst, int src)
{
    xrex(x, 0, dst, 0, src, src >= 4 && src < 8);
    xb(x, 0x0F);
    xb(x, 0xB6);
    xmod_rr(x, dst, src);
}

/* movzx dst32, src16 */
static void x_movzx16(Xe *x, int dst, int src)
{
    xrex(x, 0, dst, 0, src, 0);
    xb(x, 0x0F);
    xb(x, 0xB7);
    xmod_rr(x, dst, src);
}

/* movsxd dst64, src32 */
static void x_movsxd(Xe *x, int dst, int src)
{
    xrex(x, 1, dst, 0, src, 0);
    xb(x, 0x63);
    xmod_rr(x, dst, src);
}

/* cmovcc dst, src */
static void x_cmov(Xe *x, int cc, int w, int dst, int src)
{
    xrex(x, w, dst, 0, src, 0);
    xb(x, 0x0F);
    xb(x, (uint8_t)(0x40 | cc));
    xmod_rr(x, dst, src);
}

static void x_neg(Xe *x, int w, int dst)
{
    xrex(x, w, 0, 0, dst, 0);
    xb(x, 0xF7);
    xmod_rr(x, 3, dst);
}

static void x_not(Xe *x, int w, int dst)
{
    xrex(x, w, 0, 0, dst, 0);
    xb(x, 0xF7);
    xmod_rr(x, 2, dst);
}

/* lea dst, [base + disp] */
static void x_lea(Xe *x, int w, int dst, int base, int32_t disp)
{
    xrex(x, w, dst, 0, base, 0);
    xb(x, 0x8D);
    xmod_m(x, dst, base, disp);
}

static void x_push(Xe *x, int r)
{
    xrex(x, 0, 0, 0, r, 0);
    xb(x, (uint8_t)(0x50 | (r & 7)));
}

static void x_pop(Xe *x, int r)
{
    xrex(x, 0, 0, 0, r, 0);
    xb(x, (uint8_t)(0x58 | (r & 7)));
}

/* jcc rel32 / jmp rel32: returns where the rel32 goes, for x_patch */
static uint8_t *x_jcc(Xe *x, int cc)
{
    xb(x, 0x0F);
    xb(x, (uint8_t)(0x80 | cc));
    xd(x, 0);
    return x->p - 4;
}

static uint8_t *x_jmp(Xe *x)
{
    xb(x, 0xE9);
    xd(x, 0);
    return x->p - 4;
}

/* Point a jcc / jmp at `target`. */
static void x_patch(uint8_t *rel, const uint8_t *target)
{
    int32_t d = (int32_t)(target - (rel + 4));
    memcpy(rel, &d, 4);
}

/* call target: rel32 when it reaches, else through rax */
static void x_call(Xe *x, const void *target)
{
    int64_t rel = (int64_t)(intptr_t)target - (int64_t)(intptr_t)(x->p + 5);
    if (rel >= INT32_MIN && rel <= INT32_MAX) {
        xb(x, 0xE8);
        xd(x, (uint32_t)(int32_t)rel);
    } else {
        x_movi64(x, X_RAX, (uint64_t)(uintptr_t)target);
        xb(x, 0xFF);
        xb(x, 0xD0);
    }
}

static void x_ret(Xe *x)
{
    xb(x, 0xC3);
}
