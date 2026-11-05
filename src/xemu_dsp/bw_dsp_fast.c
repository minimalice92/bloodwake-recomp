/* SPDX-License-Identifier: GPL-2.0-or-later (adapted from xemu's DSP; see NOTICE.md) */
/*
 * bw_dsp_fast.c - the translated engine for xemu's DSP56300 (lane au-dsp).
 *
 * xemu's interpreter (interp/dsp_cpu.c, from xemu v0.8.139, itself adapted
 * from Hatari's DSP M56001 emulation; GPL-2.0-or-later) fetches each P word,
 * looks its routine up, decodes its fields at run time and then runs the
 * generic PC and interrupt bookkeeping. This engine keeps a cache of
 * pre-decoded instructions, one per P address, built in memory from the
 * DSP's own P memory the first time an address runs (call A6), and runs
 * them through handlers that do the same thing with the decoding done once.
 *
 * Exactness is by construction where possible: this file includes the
 * interpreter's own source (with its five extern functions renamed so the
 * two copies link side by side), so every handler can use the interpreter's
 * own helpers (the 56-bit arithmetic, the condition codes, the address
 * updates, the stack, the memory routines), and an instruction no handler
 * covers runs through the interpreter's own routine. BW_DSP_LOCKSTEP
 * (bw_dsp.c) compares the whole state with the interpreter's every frame.
 *
 * The cache needs no invalidation hooks: each entry keeps the P word(s) it
 * was decoded from, and is used only while P still holds them. So the data
 * the EP keeps in P (a counter, a flag, a DMA descriptor) costs nothing, and
 * code that DMA brings in (the GP's effects, the EP's overlays) is decoded
 * the first time it runs.
 */

/* The interpreter's source, a second copy with its own names. */
#define dsp56k_reset_cpu bwf_dsp56k_reset_cpu
#define dsp56k_execute_instruction bwf_dsp56k_execute_instruction
#define dsp56k_read_memory bwf_dsp56k_read_memory
#define dsp56k_write_memory bwf_dsp56k_write_memory
#define dsp56k_add_interrupt bwf_dsp56k_add_interrupt

/* Its one P write (write_memory's stl_le_p), seen: during this engine's
 * frames a write that changes a P word one of its blocks was built from
 * moves the P generation, and only such a write (so data kept in P, and the
 * EP's overlay DMA'd in again unchanged each period, cost no revalidation).
 * Per thread; NULL outside the frames. */
#include "qemu/bswap.h"
#ifdef _MSC_VER
#define BWF_TLS __declspec(thread)
#else
#define BWF_TLS __thread
#endif
static BWF_TLS uint32_t *s_pw_gen;
static BWF_TLS const uint8_t *s_pw_map;
static BWF_TLS const uint32_t *s_pw_pram;

static void bwf_stl_le_p(void *p, uint32_t v)
{
    if (s_pw_gen) {
        size_t a = (size_t)((const uint32_t *)p - s_pw_pram);
        uint32_t old;
        memcpy(&old, p, 4);
        if (old != v && (a >= 4096 || s_pw_map[a]))
            (*s_pw_gen)++;              /* (the EP rewrites its overlay, unchanged, each period) */
    }
    memcpy(p, &v, 4);
}
#define stl_le_p bwf_stl_le_p
#include "interp/dsp_cpu.c"
#undef stl_le_p

#include "dsp.h"
#include "bw_dsp_fast.h"
#include "bw_dsp_engine.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#ifdef _MSC_VER
#define BWF_INLINE static __forceinline
#else
#define BWF_INLINE static inline __attribute__((always_inline))
#endif

typedef struct FOp FOp;
typedef void (*fop_fn)(dsp_core_t *c, const FOp *op);
typedef int (*fop_body)(dsp_core_t *c, const FOp *op);

/* One decoded instruction. */
struct FOp {
    uint32_t w0, w1;            /* the P words it was decoded from (w1 when deps 2) */
    fop_fn fn;                  /* its handler; NULL: the interpreter's step */
    fop_body body;              /* its body, for blocks (NULL: ends a block) */
    emu_func_t emu;             /* the interpreter's routine (generic handlers) */
    emu_func_t alu;             /* parallel moves: the ALU routine (NULL: move) */
    emu_func_t alu_slow;        /* the interpreter's own, for wide A2/B2 (see run_alu) */
    uint16_t addr;              /* its P address */
    uint32_t mask;              /* a destination register's width mask */
    uint32_t imm;               /* an immediate, address or displacement */
    uint8_t deps;               /* P words the entry was decoded from: 1 or 2 */
    uint8_t len, cyc;           /* static length and cycles (bodies, branches) */
    uint8_t inblock;            /* its body may run inside a block */
    uint8_t pwrite;             /* its handler may write P (movem, movep p:ea) */
    uint8_t kind;               /* for the statistics */
    uint8_t r1, r2;             /* registers */
    uint8_t ea1, ea2;           /* effective-address modes (mode << 3 | reg) */
    uint8_t space;              /* 0 X, 1 Y */
    uint8_t cc;                 /* condition code / bit number */
};

enum {
    K_GENERIC, K_LIGHT, K_SLOW,
    K_PM2NOP, K_PM2REG, K_PM3, K_PM5, K_PM8,
    K_MOVE_DISP, K_BRANCH, K_BITBRANCH, K_NOP, K_RTS, K_BSR, K_IMM_ALU,
    K_COUNT
};
static const char *const s_kind_name[K_COUNT] = {
    "generic", "light", "interp-step",
    "pm2-alu", "pm2-reg", "pm3-imm", "pm5-move", "pm8-move2",
    "move-disp", "branch", "bit-branch", "nop", "rts", "bsr", "imm-alu",
};

#define BWF_PSIZE DSP_PRAM_SIZE
#define BWF_NOWORD 0xFFFFFFFFu  /* never a 24-bit P word: forces a decode */
#define BWF_LF (1u << DSP_SR_LF)

/* Parallel-move forms, for the profile (pm_kind). */
enum { PMK_0, PMK_1, PMK_2NOP, PMK_2UPD, PMK_2_2, PMK_3, PMK_4X, PMK_5, PMK_8, PMK_COUNT };
static const char *const s_pmk_name[PMK_COUNT] = {
    "pm0", "pm1", "pm2-nop", "pm2-update", "pm2-reg", "pm3-imm", "pm4-long", "pm5-xy", "pm8-xy2",
};

static int pm_kind(uint32_t op)
{
    switch ((op >> 20) & 0xF) {
    case 0: return PMK_0;
    case 1: return PMK_1;
    case 2:
        if ((op & 0xffff00) == 0x200000) return PMK_2NOP;
        if ((op & 0xffe000) == 0x204000) return PMK_2UPD;
        if ((op & 0xfc0000) == 0x200000) return PMK_2_2;
        return PMK_3;
    case 3: return PMK_3;
    case 4: case 5: case 6: case 7:
        if ((op & 0xf40000) == 0x400000) return PMK_4X;
        return PMK_5;
    default: return PMK_8;
    }
}

struct BwFast {
    int is_gp;
    int stats;                      /* count instructions (BW_DSP_FASTSTAT) */
    int prof;                       /* BW_DSP_FASTSTAT=3: the profile */
    uint32_t prof_pm[PMK_COUNT][256];
    uint32_t prof_np[ARRAY_SIZE(nonparallel_opcodes) + 1];
    FOp ops[BWF_PSIZE];
    uint64_t n_fast, n_slow, n_decode;
    uint64_t kind_n[K_COUNT];
    uint64_t n_blocks, n_block_ops, n_builds;
    /* blocks: by start address; their decoded instructions and P words in
     * two pools, flushed together when full */
    struct Block *blk;
    FOp *pool;
    uint32_t *wpool;
    uint32_t pool_n, wpool_n;
    uint8_t boundary[BWF_PSIZE];    /* a DO loop ends just before this address */
    /* P's generation: bumped by every write to P that can happen (the DMA,
     * the instructions that write P, the interpreter's step, and from outside
     * through bwf_p_changed); a block checks its words again only when it
     * moved since it last did */
    uint32_t gen;
    struct Jit *jit;                /* the block JIT (NULL: off, BW_DSP_JIT=0) */
    void *ctab[BWF_PSIZE];          /* block linking: each address's fragment (or the exit) */
    uint32_t ctab_gen;              /* the P generation ctab's fragments were checked at */
    int chain_off;                  /* BW_DSP_JIT_CHAIN=0 */
    uint64_t n_chain_calls, n_chain_runs, n_chain_frags;   /* bench counters */
    int jit_full;                   /* its region filled: drop everything at the next turn */
    uint64_t n_jit;                 /* blocks compiled */
    void *dma_opaque;
    dsp_dma_mem_read_func dma_read;
    dsp_dma_mem_write_func dma_write;
    void (*wp_orig)(dsp_core_t *core, uint32_t address, uint32_t value);  /* during a frame */
    int fast_dma_off;               /* BW_DSP_FASTDMA=0 */
    uint8_t *dma_buf;               /* the inline DMA's buffer */
    uint32_t dma_buf_size;
    uint8_t code_map[BWF_PSIZE];    /* P words some block was built from (until a flush) */
    int64_t t_io;                   /* BW_DSP_TIME: QPC ticks in the DMA's system-memory callbacks */
    int time_io;
};

void bwf_p_changed(BwFast *f)
{
    f->gen++;
}

/* BW_DSP_TIME: QPC ticks spent in the DMA's system-memory callbacks (the
 * bridge's scratch and FIFOs) since the last call. */
int64_t bwf_take_io_time(BwFast *f)
{
    int64_t t = f->t_io;
    f->t_io = 0;
    return t;
}

/* Installed in the DSP's DMA for the frame: a write to P bumps the generation. */
/* The C backend's DMA memory callbacks (static in dsp_c.c), learnt from a
 * DSP made for the purpose (tables_init). */
static dsp_dma_mem_read_func s_c_dma_read;
static dsp_dma_mem_write_func s_c_dma_write;

static uint32_t dma_rd(void *opaque, int space, uint32_t addr)
{
    BwFast *f = (BwFast *)opaque;
    return f->dma_read(f->dma_opaque, space, addr);
}

static void dma_wr(void *opaque, int space, uint32_t addr, uint32_t value)
{
    BwFast *f = (BwFast *)opaque;
    if (space == DSP_SPACE_P)
        f->gen++;
    f->dma_write(f->dma_opaque, space, addr, value);
}

/* The same, when the DMA's memory callbacks were the C backend's (they are
 * dsp56k_read_memory / write_memory on the core): X and Y inline. */
static uint32_t dma_rd_x(dsp_core_t *c, uint32_t space, uint32_t a);
static void dma_wr_x(dsp_core_t *c, uint32_t space, uint32_t a, uint32_t v);

static uint32_t dma_rd_fast(void *opaque, int space, uint32_t addr)
{
    BwFast *f = (BwFast *)opaque;
    dsp_core_t *c = (dsp_core_t *)f->dma_opaque;
    if (space == DSP_SPACE_X || space == DSP_SPACE_Y)
        return dma_rd_x(c, (uint32_t)space, addr);
    return dsp56k_read_memory(c, space, addr);
}

static void dma_wr_fast(void *opaque, int space, uint32_t addr, uint32_t value)
{
    BwFast *f = (BwFast *)opaque;
    dsp_core_t *c = (dsp_core_t *)f->dma_opaque;
    if (space == DSP_SPACE_X || space == DSP_SPACE_Y) {
        dma_wr_x(c, (uint32_t)space, addr, value);
        return;
    }
    dsp56k_write_memory(c, space, addr, value);     /* P: the write hook */
}

/* ---- decoding helpers ----------------------------------------------------- */

static int s_tables_ready;
static void alu_desc_init(void);

/* The interpreter's opcode table in this copy fills on its first reset. */
static void tables_init(void)
{
    if (!s_tables_ready) {
        dsp_core_t *tmp = (dsp_core_t *)calloc(1, sizeof(dsp_core_t));
        DSPState *d;
        if (tmp) {
            dsp56k_reset_cpu(tmp);
            free(tmp);
        }
        alu_desc_init();
        d = dsp_init(NULL, NULL, NULL, false);
        if (d) {
            s_c_dma_read = d->dma.mem_read;
            s_c_dma_write = d->dma.mem_write;
            dsp_destroy(d);
        }
        s_tables_ready = 1;
    }
}

/* The interpreter's lookup, without its assert: NULL when nothing matches. */
static const OpcodeEntry *np_lookup(uint32_t op)
{
    int i;
    for (i = 0; i < (int)ARRAY_SIZE(nonparallel_opcodes); i++) {
        if ((op & nonparallel_matches[i][0]) == nonparallel_matches[i][1]) {
            if (nonparallel_opcodes[i].match_func && !nonparallel_opcodes[i].match_func(op))
                continue;
            return &nonparallel_opcodes[i];
        }
    }
    return NULL;
}

/* ---- the ALU, in registers --------------------------------------------------
 *
 * The interpreter's ALU routines work on 3-word arrays and mask them in
 * place (x &= 0xFFFFFF), which MSVC compiles to a byte store followed by a
 * full reload: a store-forwarding stall on every operation, the main cost of
 * both engines (dsp_bench's profile). These are the same routines computed
 * on 56-bit values in a register, with one store per register. They rely on
 * what every writer of the registers keeps: A2/B2 8 bits, the others 24, SR
 * 16. bwf_selftest_alu checks each against the interpreter's on random
 * states; one that differs is not used (fast_alu[] entry NULL). */

#define M56 0x00FFFFFFFFFFFFFFull
#define SR_C (1u << DSP_SR_C)
#define SR_V (1u << DSP_SR_V)
#define SR_Z (1u << DSP_SR_Z)
#define SR_N (1u << DSP_SR_N)
#define SR_U (1u << DSP_SR_U)
#define SR_E (1u << DSP_SR_E)
#define SR_L (1u << DSP_SR_L)

BWF_INLINE uint64_t acc_get(const dsp_core_t *c, int k)
{
    return ((uint64_t)c->registers[DSP_REG_A2 + k] << 48) |
           ((uint64_t)c->registers[DSP_REG_A1 + k] << 24) | c->registers[DSP_REG_A0 + k];
}

BWF_INLINE void acc_put(dsp_core_t *c, int k, uint64_t v)
{
    c->registers[DSP_REG_A2 + k] = (uint32_t)(v >> 48) & 0xFF;
    c->registers[DSP_REG_A1 + k] = (uint32_t)(v >> 24) & 0xFFFFFF;
    c->registers[DSP_REG_A0 + k] = (uint32_t)v & 0xFFFFFF;
}

/* A 24-bit register as an ALU source: bits 47-24, sign in 55-48. */
BWF_INLINE uint64_t src24(uint32_t r)
{
    return ((uint64_t)(r & (1 << 23) ? 0xFF : 0) << 48) | ((uint64_t)r << 24);
}

/* X or Y (hi:lo) as an ALU source. */
BWF_INLINE uint64_t src48(uint32_t hi, uint32_t lo)
{
    return ((uint64_t)(hi & (1 << 23) ? 0xFF : 0) << 48) | ((uint64_t)hi << 24) | lo;
}

/* dsp_add56: d + s; *nsr = L|V|C */
BWF_INLINE uint64_t add56(uint64_t s, uint64_t d, uint32_t *nsr)
{
    uint64_t r = s + d;
    uint32_t carry = (uint32_t)(r >> 56) & 1, ov;
    r &= M56;
    ov = (uint32_t)((((s ^ r) & (d ^ r)) >> 55) & 1);
    *nsr = (ov << DSP_SR_L) | (ov << DSP_SR_V) | carry;
    return r;
}

/* dsp_sub56: d - s; *nsr = L|V|C */
BWF_INLINE uint64_t sub56(uint64_t s, uint64_t d, uint32_t *nsr)
{
    uint64_t r = (d - s) & M56;
    uint32_t carry = d < s, ov = (uint32_t)((((s ^ d) & (r ^ d)) >> 55) & 1);
    *nsr = (ov << DSP_SR_L) | (ov << DSP_SR_V) | carry;
    return r;
}

/* dsp_abs56 */
BWF_INLINE uint64_t abs56(uint64_t v)
{
    return (v >> 55) ? (0 - v) & M56 : v;
}

/* dsp_asl56(d, 1) */
BWF_INLINE uint64_t asl56(uint64_t v, uint32_t *nsr)
{
    uint64_t r = (v << 1) & M56;
    uint32_t carry = (uint32_t)(v >> 55) & 1;
    uint32_t vv = (uint32_t)(((v >> 55) ^ (r >> 55)) & 1);
    *nsr = (carry << DSP_SR_L) | (vv << DSP_SR_V) | carry;
    return r;
}

/* dsp_asr56(d, 1): a logical shift of the 56-bit value, as xemu does */
BWF_INLINE uint64_t asr56(uint64_t v, uint32_t *nsr)
{
    *nsr = (uint32_t)v & 1;
    return v >> 1;
}

/* dsp_mul56: s1 * s2 as signed fractions, doubled; negated for SIGN_MINUS */
BWF_INLINE uint64_t mul56(uint32_t a, uint32_t b, int minus)
{
    int64_t p = (int64_t)(int32_t)(a << 8) * (int64_t)(int32_t)(b << 8);   /* << 16 */
    p >>= 15;                                                               /* << 1 */
    if (minus)
        p = -p;
    return (uint64_t)p & M56;
}

/* dsp_rnd56 */
BWF_INLINE uint64_t rnd56(uint32_t sr, uint64_t v)
{
    uint32_t d0, d1, d2;
    if (sr & (1u << DSP_SR_S0)) {
        v = (v + (1ull << 24)) & M56;
        d0 = (uint32_t)(v >> 48);
        d1 = (uint32_t)(v >> 24) & 0xFFFFFF;
        d2 = (uint32_t)v & 0xFFFFFF;
        if (d2 == 0 && (d1 & 1) == 0)
            d1 &= 0xfffffc;
        d1 &= 0xfffffe;
        d2 = 0;
    } else if (sr & (1u << DSP_SR_S1)) {
        v = (v + (1ull << 22)) & M56;
        d0 = (uint32_t)(v >> 48);
        d1 = (uint32_t)(v >> 24) & 0xFFFFFF;
        d2 = (uint32_t)v & 0xFFFFFF;
        if ((d2 & 0x7fffff) == 0)
            d2 = 0;
        d2 &= 0x800000;
    } else {
        v = (v + (1ull << 23)) & M56;
        d0 = (uint32_t)(v >> 48);
        d1 = (uint32_t)(v >> 24) & 0xFFFFFF;
        d2 = (uint32_t)v & 0xFFFFFF;
        if (d2 == 0)
            d1 &= 0xfffffe;
        d2 = 0;
    }
    return ((uint64_t)d0 << 48) | ((uint64_t)d1 << 24) | d2;
}

/* emu_ccr_update_e_u_n_z on a 56-bit value: SR with E, U, N, Z */
BWF_INLINE uint32_t ccr(uint32_t sr, uint64_t r)
{
    uint32_t e;
    sr &= 0xFFFFu - (SR_E | SR_U | SR_N | SR_Z);
    switch ((sr >> DSP_SR_S0) & 3) {
    case 0:
        e = (uint32_t)(r >> 47) & 0x1FF;
        if (e != 0 && e != 0x1FF)
            sr |= SR_E;
        e = (uint32_t)(r >> 46) & 3;
        if (e == 0 || e == 3)
            sr |= SR_U;
        break;
    case 1:
        e = (uint32_t)(r >> 48) & 0xFF;
        if (e != 0 && e != 0xFF)
            sr |= SR_E;
        e = (uint32_t)(r >> 47) & 3;
        if (e == 0 || e == 3)
            sr |= SR_U;
        break;
    case 2:
        e = (uint32_t)(r >> 46) & 0x3FF;
        if (e != 0 && e != 0x3FF)
            sr |= SR_E;
        e = (uint32_t)(r >> 45) & 3;
        if (e == 0 || e == 3)
            sr |= SR_U;
        break;
    default:
        return sr;
    }
    if (r == 0)
        sr |= SR_Z;
    sr |= (uint32_t)(r >> 52) & 8;      /* bit 55 to N (bit 3) */
    return sr;
}

/* emu_pm_read_accu24: an accumulator read as 24 bits, limited (sets L) */
BWF_INLINE uint32_t accu24(dsp_core_t *c, uint32_t k)
{
    uint32_t a2 = c->registers[DSP_REG_A2 + k], value = (a2 << 24) + c->registers[DSP_REG_A1 + k];
    switch ((c->registers[DSP_REG_SR] >> DSP_SR_S0) & 3) {
    case 1:
        value >>= 1;
        break;
    case 2:
        value <<= 1;
        value |= (c->registers[DSP_REG_A0 + k] >> 23) & 1;
        break;
    }
    value &= BITMASK(24);
    if (a2 == 0 && value <= 0x007fffff)
        return value;
    if (a2 == 0xff && value >= 0x00800000)
        return value;
    c->registers[DSP_REG_SR] |= SR_L;
    return a2 & (1 << 7) ? 0x00800000 : 0x007fffff;
}

/* -- the families (named as the interpreter's: D 0 = A, 1 = B) -- */

#define DSRC_ACC(k) acc_get(c, (k))
#define DSRC_X src48(c->registers[DSP_REG_X1], c->registers[DSP_REG_X0])
#define DSRC_Y src48(c->registers[DSP_REG_Y1], c->registers[DSP_REG_Y0])
#define DSRC_R(r) src24(c->registers[r])

#define F_ADDSUB(NAME, OP, D, SRC)                                                      \
    static void NAME(dsp_core_t *c)                                                     \
    {                                                                                   \
        uint32_t nsr, sr;                                                               \
        uint64_t r = OP(SRC, acc_get(c, D), &nsr);                                      \
        acc_put(c, D, r);                                                               \
        sr = ccr(c->registers[DSP_REG_SR], r);                                          \
        c->registers[DSP_REG_SR] = (sr & (0xFFFFu - (SR_V | SR_C))) | nsr;               \
    }
#define F_CMP(NAME, D, SRC)                                                             \
    static void NAME(dsp_core_t *c)                                                     \
    {                                                                                   \
        uint32_t nsr, sr;                                                               \
        uint64_t r = sub56(SRC, acc_get(c, D), &nsr);                                   \
        sr = ccr(c->registers[DSP_REG_SR], r);                                          \
        c->registers[DSP_REG_SR] = (sr & (0xFFFFu - (SR_V | SR_C))) | nsr;               \
    }
#define F_CMPM(NAME, D, SRC)                                                            \
    static void NAME(dsp_core_t *c)                                                     \
    {                                                                                   \
        uint32_t nsr, sr;                                                               \
        uint64_t r = sub56(abs56(SRC), abs56(acc_get(c, D)), &nsr);                     \
        sr = ccr(c->registers[DSP_REG_SR], r);                                          \
        c->registers[DSP_REG_SR] = (sr & (0xFFFFu - (SR_V | SR_C))) | nsr;               \
    }
/* tfr from a 24-bit register: A0 = 0, A1 = r, A2 = its sign */
#define F_TFR_R(NAME, D, R)                                                             \
    static void NAME(dsp_core_t *c)                                                     \
    {                                                                                   \
        uint32_t v = c->registers[R];                                                   \
        c->registers[DSP_REG_A0 + (D)] = 0;                                             \
        c->registers[DSP_REG_A1 + (D)] = v;                                             \
        c->registers[DSP_REG_A2 + (D)] = v & (1 << 23) ? 0xff : 0x0;                    \
    }
#define F_LOGIC(NAME, D, R, EXPR)                                                       \
    static void NAME(dsp_core_t *c)                                                     \
    {                                                                                   \
        uint32_t a = c->registers[DSP_REG_A1 + (D)], s = c->registers[R];               \
        uint32_t sr = c->registers[DSP_REG_SR];                                         \
        (void)s;                                                                        \
        a = (EXPR);                                                                     \
        c->registers[DSP_REG_A1 + (D)] = a;                                             \
        sr &= 0xFFFFu - (SR_N | SR_Z | SR_V);                                           \
        sr |= ((a >> 23) & 1) << DSP_SR_N;                                              \
        sr |= (uint32_t)(a == 0) << DSP_SR_Z;                                           \
        c->registers[DSP_REG_SR] = sr;                                                  \
    }

/* mpy / mpyr / mac / macr. KIND 0 mpy, 1 mpyr, 2 mac, 3 macr */
#define F_MUL(NAME, KIND, MINUS, R1, R2, D)                                             \
    static void NAME(dsp_core_t *c)                                                     \
    {                                                                                   \
        uint32_t nsr = 0, sr;                                                           \
        uint64_t r = mul56(c->registers[R1], c->registers[R2], MINUS);                  \
        if ((KIND) >= 2)                                                                \
            r = add56(r, acc_get(c, D), &nsr);                                          \
        if ((KIND) & 1)                                                                 \
            r = rnd56(c->registers[DSP_REG_SR], r);                                     \
        acc_put(c, D, r);                                                               \
        sr = ccr(c->registers[DSP_REG_SR], r) & (0xFFFFu - SR_V);                       \
        if ((KIND) >= 2)                                                                \
            sr |= nsr & 0xfe;                                                           \
        c->registers[DSP_REG_SR] = sr;                                                  \
    }

F_ADDSUB(fa_add_b_a, add56, 0, DSRC_ACC(1))
F_ADDSUB(fa_add_a_b, add56, 1, DSRC_ACC(0))
F_ADDSUB(fa_add_x_a, add56, 0, DSRC_X)
F_ADDSUB(fa_add_x_b, add56, 1, DSRC_X)
F_ADDSUB(fa_add_y_a, add56, 0, DSRC_Y)
F_ADDSUB(fa_add_y_b, add56, 1, DSRC_Y)
F_ADDSUB(fa_add_x0_a, add56, 0, DSRC_R(DSP_REG_X0))
F_ADDSUB(fa_add_x0_b, add56, 1, DSRC_R(DSP_REG_X0))
F_ADDSUB(fa_add_y0_a, add56, 0, DSRC_R(DSP_REG_Y0))
F_ADDSUB(fa_add_y0_b, add56, 1, DSRC_R(DSP_REG_Y0))
F_ADDSUB(fa_add_x1_a, add56, 0, DSRC_R(DSP_REG_X1))
F_ADDSUB(fa_add_x1_b, add56, 1, DSRC_R(DSP_REG_X1))
F_ADDSUB(fa_add_y1_a, add56, 0, DSRC_R(DSP_REG_Y1))
F_ADDSUB(fa_add_y1_b, add56, 1, DSRC_R(DSP_REG_Y1))
F_ADDSUB(fa_sub_b_a, sub56, 0, DSRC_ACC(1))
F_ADDSUB(fa_sub_a_b, sub56, 1, DSRC_ACC(0))
F_ADDSUB(fa_sub_x_a, sub56, 0, DSRC_X)
F_ADDSUB(fa_sub_x_b, sub56, 1, DSRC_X)
F_ADDSUB(fa_sub_y_a, sub56, 0, DSRC_Y)
F_ADDSUB(fa_sub_y_b, sub56, 1, DSRC_Y)
F_ADDSUB(fa_sub_x0_a, sub56, 0, DSRC_R(DSP_REG_X0))
F_ADDSUB(fa_sub_x0_b, sub56, 1, DSRC_R(DSP_REG_X0))
F_ADDSUB(fa_sub_y0_a, sub56, 0, DSRC_R(DSP_REG_Y0))
F_ADDSUB(fa_sub_y0_b, sub56, 1, DSRC_R(DSP_REG_Y0))
F_ADDSUB(fa_sub_x1_a, sub56, 0, DSRC_R(DSP_REG_X1))
F_ADDSUB(fa_sub_x1_b, sub56, 1, DSRC_R(DSP_REG_X1))
F_ADDSUB(fa_sub_y1_a, sub56, 0, DSRC_R(DSP_REG_Y1))
F_ADDSUB(fa_sub_y1_b, sub56, 1, DSRC_R(DSP_REG_Y1))
F_CMP(fa_cmp_b_a, 0, DSRC_ACC(1))
F_CMP(fa_cmp_a_b, 1, DSRC_ACC(0))
F_CMP(fa_cmp_x0_a, 0, DSRC_R(DSP_REG_X0))
F_CMP(fa_cmp_x0_b, 1, DSRC_R(DSP_REG_X0))
F_CMP(fa_cmp_y0_a, 0, DSRC_R(DSP_REG_Y0))
F_CMP(fa_cmp_y0_b, 1, DSRC_R(DSP_REG_Y0))
F_CMP(fa_cmp_x1_a, 0, DSRC_R(DSP_REG_X1))
F_CMP(fa_cmp_x1_b, 1, DSRC_R(DSP_REG_X1))
F_CMP(fa_cmp_y1_a, 0, DSRC_R(DSP_REG_Y1))
F_CMP(fa_cmp_y1_b, 1, DSRC_R(DSP_REG_Y1))
F_CMPM(fa_cmpm_b_a, 0, DSRC_ACC(1))
F_CMPM(fa_cmpm_a_b, 1, DSRC_ACC(0))
F_CMPM(fa_cmpm_x0_a, 0, DSRC_R(DSP_REG_X0))
F_CMPM(fa_cmpm_x0_b, 1, DSRC_R(DSP_REG_X0))
F_CMPM(fa_cmpm_y0_a, 0, DSRC_R(DSP_REG_Y0))
F_CMPM(fa_cmpm_y0_b, 1, DSRC_R(DSP_REG_Y0))
F_CMPM(fa_cmpm_x1_a, 0, DSRC_R(DSP_REG_X1))
F_CMPM(fa_cmpm_x1_b, 1, DSRC_R(DSP_REG_X1))
F_CMPM(fa_cmpm_y1_a, 0, DSRC_R(DSP_REG_Y1))
F_CMPM(fa_cmpm_y1_b, 1, DSRC_R(DSP_REG_Y1))
F_TFR_R(fa_tfr_x0_a, 0, DSP_REG_X0)
F_TFR_R(fa_tfr_x0_b, 1, DSP_REG_X0)
F_TFR_R(fa_tfr_y0_a, 0, DSP_REG_Y0)
F_TFR_R(fa_tfr_y0_b, 1, DSP_REG_Y0)
F_TFR_R(fa_tfr_x1_a, 0, DSP_REG_X1)
F_TFR_R(fa_tfr_x1_b, 1, DSP_REG_X1)
F_TFR_R(fa_tfr_y1_a, 0, DSP_REG_Y1)
F_TFR_R(fa_tfr_y1_b, 1, DSP_REG_Y1)
F_LOGIC(fa_and_x0_a, 0, DSP_REG_X0, a & s)
F_LOGIC(fa_and_x0_b, 1, DSP_REG_X0, a & s)
F_LOGIC(fa_and_y0_a, 0, DSP_REG_Y0, a & s)
F_LOGIC(fa_and_y0_b, 1, DSP_REG_Y0, a & s)
F_LOGIC(fa_and_x1_a, 0, DSP_REG_X1, a & s)
F_LOGIC(fa_and_x1_b, 1, DSP_REG_X1, a & s)
F_LOGIC(fa_and_y1_a, 0, DSP_REG_Y1, a & s)
F_LOGIC(fa_and_y1_b, 1, DSP_REG_Y1, a & s)
F_LOGIC(fa_or_x0_a, 0, DSP_REG_X0, (a | s) & BITMASK(24))
F_LOGIC(fa_or_x0_b, 1, DSP_REG_X0, (a | s) & BITMASK(24))
F_LOGIC(fa_or_y0_a, 0, DSP_REG_Y0, (a | s) & BITMASK(24))
F_LOGIC(fa_or_y0_b, 1, DSP_REG_Y0, (a | s) & BITMASK(24))
F_LOGIC(fa_or_x1_a, 0, DSP_REG_X1, (a | s) & BITMASK(24))
F_LOGIC(fa_or_x1_b, 1, DSP_REG_X1, (a | s) & BITMASK(24))
F_LOGIC(fa_or_y1_a, 0, DSP_REG_Y1, (a | s) & BITMASK(24))
F_LOGIC(fa_or_y1_b, 1, DSP_REG_Y1, (a | s) & BITMASK(24))
F_LOGIC(fa_eor_x0_a, 0, DSP_REG_X0, (a ^ s) & BITMASK(24))
F_LOGIC(fa_eor_x0_b, 1, DSP_REG_X0, (a ^ s) & BITMASK(24))
F_LOGIC(fa_eor_y0_a, 0, DSP_REG_Y0, (a ^ s) & BITMASK(24))
F_LOGIC(fa_eor_y0_b, 1, DSP_REG_Y0, (a ^ s) & BITMASK(24))
F_LOGIC(fa_eor_x1_a, 0, DSP_REG_X1, (a ^ s) & BITMASK(24))
F_LOGIC(fa_eor_x1_b, 1, DSP_REG_X1, (a ^ s) & BITMASK(24))
F_LOGIC(fa_eor_y1_a, 0, DSP_REG_Y1, (a ^ s) & BITMASK(24))
F_LOGIC(fa_eor_y1_b, 1, DSP_REG_Y1, (a ^ s) & BITMASK(24))
F_LOGIC(fa_not_a, 0, DSP_REG_X0, (~a) & BITMASK(24))
F_LOGIC(fa_not_b, 1, DSP_REG_X0, (~a) & BITMASK(24))

static void fa_tfr_b_a(dsp_core_t *c)
{
    c->registers[DSP_REG_A0] = c->registers[DSP_REG_B0];
    c->registers[DSP_REG_A1] = c->registers[DSP_REG_B1];
    c->registers[DSP_REG_A2] = c->registers[DSP_REG_B2];
}

static void fa_tfr_a_b(dsp_core_t *c)
{
    c->registers[DSP_REG_B0] = c->registers[DSP_REG_A0];
    c->registers[DSP_REG_B1] = c->registers[DSP_REG_A1];
    c->registers[DSP_REG_B2] = c->registers[DSP_REG_A2];
}

#define F_TST(NAME, D)                                                                  \
    static void NAME(dsp_core_t *c)                                                     \
    {                                                                                   \
        c->registers[DSP_REG_SR] = ccr(c->registers[DSP_REG_SR], acc_get(c, D)) &        \
                                   (0xFFFFu - SR_V);                                    \
    }
F_TST(fa_tst_a, 0)
F_TST(fa_tst_b, 1)

#define F_CLR(NAME, D)                                                                  \
    static void NAME(dsp_core_t *c)                                                     \
    {                                                                                   \
        uint32_t sr = c->registers[DSP_REG_SR];                                         \
        c->registers[DSP_REG_A2 + (D)] = 0;                                             \
        c->registers[DSP_REG_A1 + (D)] = 0;                                             \
        c->registers[DSP_REG_A0 + (D)] = 0;                                             \
        sr &= 0xFFFFu - (SR_E | SR_N | SR_V);                                           \
        c->registers[DSP_REG_SR] = sr | SR_U | SR_Z;                                    \
    }
F_CLR(fa_clr_a, 0)
F_CLR(fa_clr_b, 1)

/* asl / asr: SR &= ~(C|V); |= the shift's L|V|C; then E, U, N, Z */
#define F_SHIFT(NAME, D, OP)                                                            \
    static void NAME(dsp_core_t *c)                                                     \
    {                                                                                   \
        uint32_t nsr, sr = c->registers[DSP_REG_SR];                                    \
        uint64_t r = OP(acc_get(c, D), &nsr);                                           \
        acc_put(c, D, r);                                                               \
        sr = (sr & (0xFFFFu - (SR_C | SR_V))) | nsr;                                     \
        c->registers[DSP_REG_SR] = ccr(sr, r);                                          \
    }
F_SHIFT(fa_asl_a, 0, asl56)
F_SHIFT(fa_asl_b, 1, asl56)
F_SHIFT(fa_asr_a, 0, asr56)
F_SHIFT(fa_asr_b, 1, asr56)

/* lsl / lsr / rol / ror on A1 */
#define F_A1SHIFT(NAME, D, KIND)                                                        \
    static void NAME(dsp_core_t *c)                                                     \
    {                                                                                   \
        uint32_t a = c->registers[DSP_REG_A1 + (D)], carry, n;                          \
        uint32_t sr = c->registers[DSP_REG_SR];                                         \
        if ((KIND) == 0) {          /* lsl */                                           \
            carry = (a >> 23) & 1;                                                      \
            a = (a << 1) & BITMASK(24);                                                 \
            n = (a >> 23) & 1;                                                          \
        } else if ((KIND) == 1) {   /* lsr */                                           \
            carry = a & 1;                                                              \
            a >>= 1;                                                                    \
            n = 0;                                                                      \
        } else if ((KIND) == 2) {   /* rol */                                           \
            carry = (a >> 23) & 1;                                                      \
            a = ((a << 1) | carry) & BITMASK(24);                                       \
            n = (a >> 23) & 1;                                                          \
        } else {                    /* ror */                                           \
            carry = a & 1;                                                              \
            a = (a >> 1) | (carry << 23);                                               \
            n = carry;                                                                  \
        }                                                                               \
        c->registers[DSP_REG_A1 + (D)] = a;                                             \
        sr &= 0xFFFFu - (SR_C | SR_N | SR_Z | SR_V);                                    \
        sr |= carry | (n << DSP_SR_N) | ((uint32_t)(a == 0) << DSP_SR_Z);               \
        c->registers[DSP_REG_SR] = sr;                                                  \
    }
F_A1SHIFT(fa_lsl_a, 0, 0)
F_A1SHIFT(fa_lsl_b, 1, 0)
F_A1SHIFT(fa_lsr_a, 0, 1)
F_A1SHIFT(fa_lsr_b, 1, 1)
F_A1SHIFT(fa_rol_a, 0, 2)
F_A1SHIFT(fa_rol_b, 1, 2)
F_A1SHIFT(fa_ror_a, 0, 3)
F_A1SHIFT(fa_ror_b, 1, 3)

/* abs / neg: V and L when the value was the most negative one */
#define F_ABSNEG(NAME, D, NEG)                                                          \
    static void NAME(dsp_core_t *c)                                                     \
    {                                                                                   \
        uint64_t v = acc_get(c, D), r;                                                  \
        uint32_t ov = v == 0x0080000000000000ull, sr = c->registers[DSP_REG_SR];        \
        r = (NEG) ? (0 - v) & M56 : abs56(v);                                           \
        acc_put(c, D, r);                                                               \
        sr = (sr & (0xFFFFu - SR_V)) | (ov << DSP_SR_L) | (ov << DSP_SR_V);             \
        c->registers[DSP_REG_SR] = ccr(sr, r);                                          \
    }
F_ABSNEG(fa_abs_a, 0, 0)
F_ABSNEG(fa_abs_b, 1, 0)
F_ABSNEG(fa_neg_a, 0, 1)
F_ABSNEG(fa_neg_b, 1, 1)

/* rnd */
#define F_RND(NAME, D)                                                                  \
    static void NAME(dsp_core_t *c)                                                     \
    {                                                                                   \
        uint64_t r = rnd56(c->registers[DSP_REG_SR], acc_get(c, D));                    \
        acc_put(c, D, r);                                                               \
        c->registers[DSP_REG_SR] = ccr(c->registers[DSP_REG_SR], r);                    \
    }
F_RND(fa_rnd_a, 0)
F_RND(fa_rnd_b, 1)

/* addl / addr / subl / subr: D shifted (asl / xemu's logical asr), then +- S */
#define F_LR(NAME, D, SHIFT, OP)                                                        \
    static void NAME(dsp_core_t *c)                                                     \
    {                                                                                   \
        uint32_t nsr, nsr2, sr;                                                         \
        uint64_t r = SHIFT(acc_get(c, D), &nsr);                                        \
        r = OP(acc_get(c, 1 - (D)), r, &nsr2);                                          \
        nsr |= nsr2;                                                                    \
        acc_put(c, D, r);                                                               \
        sr = ccr(c->registers[DSP_REG_SR], r);                                          \
        c->registers[DSP_REG_SR] = (sr & (0xFFFFu - (SR_V | SR_C))) | nsr;               \
    }
F_LR(fa_addl_b_a, 0, asl56, add56)
F_LR(fa_addl_a_b, 1, asl56, add56)
F_LR(fa_addr_b_a, 0, asr56, add56)
F_LR(fa_addr_a_b, 1, asr56, add56)
F_LR(fa_subl_a, 0, asl56, sub56)
F_LR(fa_subl_b, 1, asl56, sub56)
F_LR(fa_subr_a, 0, asr56, sub56)
F_LR(fa_subr_b, 1, asr56, sub56)

/* adc / sbc: +- S, then +- 1 more when C was set */
#define F_CARRY(NAME, D, OP, SRC)                                                       \
    static void NAME(dsp_core_t *c)                                                     \
    {                                                                                   \
        uint32_t nsr, nsr2, sr, cur = c->registers[DSP_REG_SR] & SR_C;                  \
        uint64_t r = OP(SRC, acc_get(c, D), &nsr);                                      \
        if (cur) {                                                                      \
            r = OP(1, r, &nsr2);                                                        \
            nsr |= nsr2;                                                                \
        }                                                                               \
        acc_put(c, D, r);                                                               \
        sr = ccr(c->registers[DSP_REG_SR], r);                                          \
        c->registers[DSP_REG_SR] = (sr & (0xFFFFu - (SR_V | SR_C))) | nsr;               \
    }
F_CARRY(fa_adc_x_a, 0, add56, DSRC_X)
F_CARRY(fa_adc_x_b, 1, add56, DSRC_X)
F_CARRY(fa_adc_y_a, 0, add56, DSRC_Y)
F_CARRY(fa_adc_y_b, 1, add56, DSRC_Y)
F_CARRY(fa_sbc_x_a, 0, sub56, DSRC_X)
F_CARRY(fa_sbc_x_b, 1, sub56, DSRC_X)
F_CARRY(fa_sbc_y_a, 0, sub56, DSRC_Y)
F_CARRY(fa_sbc_y_b, 1, sub56, DSRC_Y)

/* max a,b: when A - B <= 0, B gets A (with A2 and A0 crossed, as xemu has it) */
static void fa_max(dsp_core_t *c)
{
    uint32_t nsr, pass;
    uint64_t r = sub56(acc_get(c, 1), acc_get(c, 0), &nsr);
    (void)nsr;
    pass = (uint32_t)(r >> 55) || r == 0;
    if (pass) {
        c->registers[DSP_REG_B0] = c->registers[DSP_REG_A2];
        c->registers[DSP_REG_B1] = c->registers[DSP_REG_A1];
        c->registers[DSP_REG_B2] = c->registers[DSP_REG_A0];
    }
    c->registers[DSP_REG_SR] = (c->registers[DSP_REG_SR] & (0xFFFFu - SR_C)) | pass;
}

/* mpy, mpyr, mac, macr for every pair and sign: generated */
#define F_MUL_SET(PR, R1, R2)                                                           \
    F_MUL(fa_mpy_p_##PR##_a, 0, 0, R1, R2, 0) F_MUL(fa_mpyr_p_##PR##_a, 1, 0, R1, R2, 0)  \
    F_MUL(fa_mac_p_##PR##_a, 2, 0, R1, R2, 0) F_MUL(fa_macr_p_##PR##_a, 3, 0, R1, R2, 0)  \
    F_MUL(fa_mpy_m_##PR##_a, 0, 1, R1, R2, 0) F_MUL(fa_mpyr_m_##PR##_a, 1, 1, R1, R2, 0)  \
    F_MUL(fa_mac_m_##PR##_a, 2, 1, R1, R2, 0) F_MUL(fa_macr_m_##PR##_a, 3, 1, R1, R2, 0)  \
    F_MUL(fa_mpy_p_##PR##_b, 0, 0, R1, R2, 1) F_MUL(fa_mpyr_p_##PR##_b, 1, 0, R1, R2, 1)  \
    F_MUL(fa_mac_p_##PR##_b, 2, 0, R1, R2, 1) F_MUL(fa_macr_p_##PR##_b, 3, 0, R1, R2, 1)  \
    F_MUL(fa_mpy_m_##PR##_b, 0, 1, R1, R2, 1) F_MUL(fa_mpyr_m_##PR##_b, 1, 1, R1, R2, 1)  \
    F_MUL(fa_mac_m_##PR##_b, 2, 1, R1, R2, 1) F_MUL(fa_macr_m_##PR##_b, 3, 1, R1, R2, 1)
F_MUL_SET(x0_x0, DSP_REG_X0, DSP_REG_X0)
F_MUL_SET(y0_y0, DSP_REG_Y0, DSP_REG_Y0)
F_MUL_SET(x1_x0, DSP_REG_X1, DSP_REG_X0)
F_MUL_SET(y1_y0, DSP_REG_Y1, DSP_REG_Y0)
F_MUL_SET(x0_y1, DSP_REG_X0, DSP_REG_Y1)
F_MUL_SET(y0_x0, DSP_REG_Y0, DSP_REG_X0)
F_MUL_SET(x1_y0, DSP_REG_X1, DSP_REG_Y0)
F_MUL_SET(y1_x1, DSP_REG_Y1, DSP_REG_X1)

/* xemu's emu_macr_p_x1_y0_b rounds an uninitialised array before it loads
 * B, then discards it: in effect a mac without rounding. So is this one. */
F_MUL(fa_macr_p_x1_y0_b_xemu, 2, 0, DSP_REG_X1, DSP_REG_Y0, 1)

#define MUL_ROW(PR, D)                                                                  \
    fa_mpy_p_##PR##_##D, fa_mpyr_p_##PR##_##D, fa_mac_p_##PR##_##D, fa_macr_p_##PR##_##D,  \
    fa_mpy_m_##PR##_##D, fa_mpyr_m_##PR##_##D, fa_mac_m_##PR##_##D, fa_macr_m_##PR##_##D

/* In the interpreter's opcodes_alu order; NULL: use the interpreter's own. */
static emu_func_t fast_alu[256] = {
    /* 0x00 - 0x3f */
    NULL, fa_tfr_b_a, fa_addr_b_a, fa_tst_a, NULL, fa_cmp_b_a, fa_subr_a, fa_cmpm_b_a,
    NULL, fa_tfr_a_b, fa_addr_a_b, fa_tst_b, NULL, fa_cmp_a_b, fa_subr_b, fa_cmpm_a_b,
    fa_add_b_a, fa_rnd_a, fa_addl_b_a, fa_clr_a, fa_sub_b_a, NULL, fa_subl_a, fa_not_a,
    fa_add_a_b, fa_rnd_b, fa_addl_a_b, fa_clr_b, fa_sub_a_b, fa_max, fa_subl_b, fa_not_b,
    fa_add_x_a, fa_adc_x_a, fa_asr_a, fa_lsr_a, fa_sub_x_a, fa_sbc_x_a, fa_abs_a, fa_ror_a,
    fa_add_x_b, fa_adc_x_b, fa_asr_b, fa_lsr_b, fa_sub_x_b, fa_sbc_x_b, fa_abs_b, fa_ror_b,
    fa_add_y_a, fa_adc_y_a, fa_asl_a, fa_lsl_a, fa_sub_y_a, fa_sbc_y_a, fa_neg_a, fa_rol_a,
    fa_add_y_b, fa_adc_y_b, fa_asl_b, fa_lsl_b, fa_sub_y_b, fa_sbc_y_b, fa_neg_b, fa_rol_b,
    /* 0x40 - 0x7f */
    fa_add_x0_a, fa_tfr_x0_a, fa_or_x0_a, fa_eor_x0_a, fa_sub_x0_a, fa_cmp_x0_a, fa_and_x0_a, fa_cmpm_x0_a,
    fa_add_x0_b, fa_tfr_x0_b, fa_or_x0_b, fa_eor_x0_b, fa_sub_x0_b, fa_cmp_x0_b, fa_and_x0_b, fa_cmpm_x0_b,
    fa_add_y0_a, fa_tfr_y0_a, fa_or_y0_a, fa_eor_y0_a, fa_sub_y0_a, fa_cmp_y0_a, fa_and_y0_a, fa_cmpm_y0_a,
    fa_add_y0_b, fa_tfr_y0_b, fa_or_y0_b, fa_eor_y0_b, fa_sub_y0_b, fa_cmp_y0_b, fa_and_y0_b, fa_cmpm_y0_b,
    fa_add_x1_a, fa_tfr_x1_a, fa_or_x1_a, fa_eor_x1_a, fa_sub_x1_a, fa_cmp_x1_a, fa_and_x1_a, fa_cmpm_x1_a,
    fa_add_x1_b, fa_tfr_x1_b, fa_or_x1_b, fa_eor_x1_b, fa_sub_x1_b, fa_cmp_x1_b, fa_and_x1_b, fa_cmpm_x1_b,
    fa_add_y1_a, fa_tfr_y1_a, fa_or_y1_a, fa_eor_y1_a, fa_sub_y1_a, fa_cmp_y1_a, fa_and_y1_a, fa_cmpm_y1_a,
    fa_add_y1_b, fa_tfr_y1_b, fa_or_y1_b, fa_eor_y1_b, fa_sub_y1_b, fa_cmp_y1_b, fa_and_y1_b, fa_cmpm_y1_b,
    /* 0x80 - 0xff */
    MUL_ROW(x0_x0, a), MUL_ROW(x0_x0, b), MUL_ROW(y0_y0, a), MUL_ROW(y0_y0, b),
    MUL_ROW(x1_x0, a), MUL_ROW(x1_x0, b), MUL_ROW(y1_y0, a), MUL_ROW(y1_y0, b),
    MUL_ROW(x0_y1, a), MUL_ROW(x0_y1, b), MUL_ROW(y0_x0, a), MUL_ROW(y0_x0, b),
    MUL_ROW(x1_y0, a), MUL_ROW(x1_y0, b), MUL_ROW(y1_x1, a), MUL_ROW(y1_x1, b),
};

/* The table's exceptions, where xemu's routine is not its family's. */
static void alu_quirks(void)
{
    static int done;
    int i;
    if (done)
        return;
    done = 1;
    for (i = 0; i < 256; i++)
        if (opcodes_alu[i] == emu_macr_p_x1_y0_b)
            fast_alu[i] = fa_macr_p_x1_y0_b_xemu;
}

/* ---- self-test: each fast routine against the interpreter's -------------- */

static uint64_t s_rng;
static int s_fz_wide;           /* random_regs: sometimes a wide A2 or B2 (handler tests) */

static uint32_t rnd32(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 7;
    s_rng ^= s_rng << 17;
    return (uint32_t)(s_rng >> 16);
}

/* A random valid register file: the widths every writer keeps, and values
 * biased to the edges (0, all ones, the sign bit, small numbers). */
static uint32_t edge24(void)
{
    switch (rnd32() & 7) {
    case 0: return 0;
    case 1: return 0xFFFFFF;
    case 2: return 0x800000;
    case 3: return 0x7FFFFF;
    case 4: return rnd32() & 0xF;
    case 5: return 0xFFFFF0 | (rnd32() & 0xF);
    default: return rnd32() & 0xFFFFFF;
    }
}

static void random_regs(dsp_core_t *c)
{
    int i;
    for (i = 0; i < DSP_REG_MAX; i++)
        c->registers[i] = rnd32() & BITMASK(registers_mask[i]);
    for (i = DSP_REG_X0; i <= DSP_REG_Y1; i++)
        c->registers[i] = edge24();
    c->registers[DSP_REG_A0] = edge24();
    c->registers[DSP_REG_B0] = edge24();
    c->registers[DSP_REG_A1] = edge24();
    c->registers[DSP_REG_B1] = edge24();
    c->registers[DSP_REG_A2] = (rnd32() & 3) ? (c->registers[DSP_REG_A1] & 0x800000 ? 0xFF : 0)
                                             : rnd32() & 0xFF;
    c->registers[DSP_REG_B2] = (rnd32() & 3) ? (c->registers[DSP_REG_B1] & 0x800000 ? 0xFF : 0)
                                             : rnd32() & 0xFF;
    c->registers[DSP_REG_SR] = rnd32() & 0xaf7f;
    if (s_fz_wide && (rnd32() & 7) == 0)
        c->registers[DSP_REG_A2 + (rnd32() & 1)] = rnd32() & 0xFFFFFF;   /* after a max */
}

/* Checks every fast routine on `iters` random states; drops any that ever
 * differs from the interpreter's (fast_alu[i] = NULL) and says which.
 * Returns how many were dropped. */
int bwf_selftest_alu(uint64_t seed, int iters, char *msg, size_t cap)
{
    dsp_core_t *a = (dsp_core_t *)calloc(1, sizeof(dsp_core_t));
    dsp_core_t *b = (dsp_core_t *)calloc(1, sizeof(dsp_core_t));
    int i, k, bad = 0;
    size_t n = 0;
    if (cap)
        msg[0] = 0;
    if (!a || !b) {
        free(a);
        free(b);
        return -1;
    }
    tables_init();
    alu_quirks();
    s_rng = seed | 1;
    for (i = 0; i < 256; i++) {
        if (!fast_alu[i])
            continue;
        for (k = 0; k < iters; k++) {
            random_regs(a);
            memcpy(b->registers, a->registers, sizeof a->registers);
            opcodes_alu[i](a);
            fast_alu[i](b);
            if (memcmp(a->registers, b->registers, sizeof a->registers)) {
                int r;
                for (r = 0; r < DSP_REG_MAX && a->registers[r] == b->registers[r]; r++)
                    ;
                if (n + 1 < cap)
                    n += (size_t)_snprintf(msg + n, cap - n - 1, "%s%s (register 0x%X: 0x%X, fast 0x%X)",
                                           n ? "; " : "", disasm_opcodes_alu[i], r,
                                           a->registers[r], b->registers[r]);
                fast_alu[i] = NULL;
                bad++;
                break;
            }
        }
    }
    if (cap)
        msg[cap - 1] = 0;
    free(a);
    free(b);
    return bad;
}

/* ---- the shared tail -------------------------------------------------------
 *
 * dsp_postexecute_update_pc and dsp_postexecute_interrupts as they run when
 * the instruction started in plain running (no REP in progress, nothing in
 * the interrupt pipeline: the only state the step hands to a handler) and is
 * not itself a REP: pc += cur_inst_len, then the DO loop's end check. An
 * interrupt can only have been raised by a stack push or pop (overflow,
 * underflow) or an illegal instruction; then the pipeline routine runs. */

static uint64_t s_n_loop_end;
static uint32_t s_exit_hist[4096][4];
static uint32_t s_fail_why[4096];
static uint64_t s_cbody_cyc;
static uint32_t s_fail_n[4096][2], s_fail_sr[4096], s_fail_m[4096][8];  /* a bench aid (BW_DSP_JIT_COUNT): chain exits by pc */

static void loop_end(dsp_core_t *c)
{
    uint32_t lc = (c->registers[DSP_REG_LC] - 1) & BITMASK(16);
    s_n_loop_end++;
    c->registers[DSP_REG_LC] = lc;     /* one store (not a byte store and a reload) */
    if (lc == 0) {
        uint32_t saved_pc, saved_sr;
        dsp_stack_pop(c, &saved_pc, &saved_sr);
        c->registers[DSP_REG_SR] &= 0x7f;
        c->registers[DSP_REG_SR] |= saved_sr & BWF_LF;
        dsp_stack_pop(c, &c->registers[DSP_REG_LA], &c->registers[DSP_REG_LC]);
    } else {
        c->pc = c->registers[DSP_REG_SSH];
    }
}

/* The tail of an instruction whose length and cycles the handler knows. */
BWF_INLINE void fin(dsp_core_t *c, uint32_t w0, uint32_t len, uint32_t cyc)
{
    uint32_t pc = c->pc + len;
    c->cur_inst = w0;
    c->cur_inst_len = len;
    c->instr_cycle = (uint16_t)cyc;
    c->pc = pc;
    if ((c->registers[DSP_REG_SR] & BWF_LF) && pc == c->registers[DSP_REG_LA] + 1) {
        loop_end(c);
        if (c->interrupt_counter)
            dsp_postexecute_interrupts(c);
    }
    c->num_inst += cyc;
}

/* The same tail when the instruction may have pushed or popped. */
BWF_INLINE void fin_stack(dsp_core_t *c, uint32_t w0, uint32_t len, uint32_t cyc)
{
    uint32_t pc = c->pc + len;
    c->cur_inst = w0;
    c->cur_inst_len = len;
    c->instr_cycle = (uint16_t)cyc;
    c->pc = pc;
    if ((c->registers[DSP_REG_SR] & BWF_LF) && pc == c->registers[DSP_REG_LA] + 1)
        loop_end(c);
    if (c->interrupt_counter)
        dsp_postexecute_interrupts(c);
    c->num_inst += cyc;
}

/* ---- memory and addressing, as the interpreter does them ----------------- */

/* dsp56k_read_memory / write_memory, with the plain RAM cases inline: X
 * below 0xC00 is xram (0xC00-0xFFF and 0x1400-0x17FF are the mix buffer,
 * 0xFFFF80 up the peripherals), Y below 0x800 is yram. Everything else goes
 * through the interpreter's own routine. */
BWF_INLINE uint32_t rd(dsp_core_t *c, uint32_t space, uint32_t a)
{
    if (space == DSP_SPACE_X) {
        if (a < 0xC00)
            return c->xram[a];
        if (a < 0x1000)                 /* X:0xC00 is the mix buffer too */
            return c->mixbuffer[a - 0xC00];
        if (a - DSP_MIXBUFFER_BASE < DSP_MIXBUFFER_SIZE)
            return c->mixbuffer[a - DSP_MIXBUFFER_BASE];
    } else if (a < DSP_YRAM_SIZE) {
        return c->yram[a];
    }
    return dsp56k_read_memory(c, space, a);
}

BWF_INLINE int wr(dsp_core_t *c, uint32_t space, uint32_t a, uint32_t v)
{
    if (space == DSP_SPACE_X) {
        if (a < 0xC00) {
            c->xram[a] = v;
            return 0;
        }
        if (a - DSP_MIXBUFFER_BASE < DSP_MIXBUFFER_SIZE) {
            c->mixbuffer[a - DSP_MIXBUFFER_BASE] = v;
            return 0;
        }
        if (a < 0x1000) {
            c->mixbuffer[a - 0xC00] = v;
            return 0;
        }
    } else if (a < DSP_YRAM_SIZE) {
        c->yram[a] = v;
        return 0;
    }
    dsp56k_write_memory(c, space, a, v);
    /* 1: a peripheral (X:0xFFFF80 up), which may halt the DSP or run a DMA */
    return space == DSP_SPACE_X && a >= DSP_PERIPH_BASE;
}

static uint32_t dma_rd_x(dsp_core_t *c, uint32_t space, uint32_t a)
{
    return rd(c, space, a);
}

static void dma_wr_x(dsp_core_t *c, uint32_t space, uint32_t a, uint32_t v)
{
    (void)wr(c, space, a, v);
}

/* ---- the DMA, inline -------------------------------------------------------
 *
 * The DSP starts its DMA by writing the control register (X:0xFFFFD6):
 * dsp_dma_write's DMA_CONTROL action, then dsp_dma_run over the block list,
 * a word at a time through the memory callbacks. During a frame of this
 * engine the same, with this core's X and Y read and written inline (rd, wr:
 * the interpreter's dsp56k_read_memory / write_memory). The system side (the
 * FIFOs, the scratch memory) goes through the same callbacks. Anything the
 * fast path doesn't take (a P space block, an unusual format or buffer, what
 * dsp_dma.c asserts) goes to dsp_dma_write from that block on: the actions
 * are idempotent and the list's state is s->next_block. */

#include "dsp_dma_regs.h"

static void fdma_circular(DSPDMAState *s, uint32_t scratch_base, uint32_t *scratch_offset,
                          uint32_t scratch_size, uint32_t transfer_size, uint8_t *buf, int dir)
{
    uint32_t buf_offset = 0;
    if (*scratch_offset >= scratch_size)
        *scratch_offset = 0;
    while (transfer_size > 0) {
        size_t bytes_until_wrap = scratch_size - *scratch_offset;
        size_t chunk = transfer_size < bytes_until_wrap ? transfer_size : bytes_until_wrap;
        s->scratch_rw(s->rw_opaque, &buf[buf_offset], scratch_base + *scratch_offset, chunk, dir);
        *scratch_offset += (uint32_t)chunk;
        if (*scratch_offset >= scratch_size)
            *scratch_offset = 0;
        transfer_size -= (uint32_t)chunk;
        buf_offset += (uint32_t)chunk;
    }
}

/* Returns 1 when the list ran to its end (or isn't running), 0 when the block
 * at s->next_block is for dsp_dma_run (nothing of it done here). */
static int fdma_run(BwFast *f, dsp_core_t *c, DSPDMAState *s)
{
    if (!(s->control & DMA_CONTROL_RUNNING) || (s->control & DMA_CONTROL_FROZEN))
        return 1;
    while (!(s->next_block & NODE_POINTER_EOL)) {
        uint32_t addr = s->next_block & NODE_POINTER_VAL, block_addr, w[7], k;
        uint32_t space, mem_space, mem_address, item_size, item_mask, count, buf_id, format;
        uint32_t channel_count, block_count, scratch_offset, scratch_base, scratch_size;
        uint32_t transfer_size, dsp_offset, control;
        int direction, interleave, writeback;
        uint8_t *buf;
        if (addr < 0x1800) {
            if (addr + 6 >= 0x1800)
                return 0;
            space = DSP_SPACE_X;
            block_addr = addr;
        } else if (addr < 0x2000) {
            if (addr + 6 >= 0x2000)
                return 0;
            space = DSP_SPACE_Y;
            block_addr = addr - 0x1800;
        } else {
            return 0;                           /* P, or out of range */
        }
        for (k = 0; k < 7; k++)
            w[k] = rd(c, space, block_addr + k);
        control = w[1];
        count = w[2];
        dsp_offset = w[3];
        scratch_offset = w[4];
        scratch_base = w[5];
        scratch_size = w[6] + 1;
        interleave = control & 1;
        direction = (control & NODE_CONTROL_DIRECTION) != 0;
        writeback = (control >> 4) & 1;
        buf_id = (control >> 5) & 0xF;
        format = (control >> 10) & 7;
        if (((control >> 2) & 3) || ((control >> 13) & 1))
            return 0;
        if (format == 1) {
            item_size = 2;
            item_mask = 0xFFFF;
        } else if (format == 2 || format == 6) {
            item_size = 4;
            item_mask = 0xFFFFFF;
        } else {
            return 0;
        }
        if (dsp_offset < 0x1800) {
            if (dsp_offset + count >= 0x1800)
                return 0;
            mem_space = DSP_SPACE_X;
            mem_address = dsp_offset;
        } else if (dsp_offset < 0x2000) {
            if (dsp_offset + count >= 0x2000)
                return 0;
            mem_space = DSP_SPACE_Y;
            mem_address = dsp_offset - 0x1800;
        } else {
            return 0;
        }
        if (direction ? !(buf_id <= 3 || buf_id == 0xE || buf_id == 0xF)
                      : (interleave || !(buf_id == 0xE || buf_id == 0xF)))
            return 0;
        channel_count = (count & 0xF) + 1;
        block_count = count >> 4;
        transfer_size = count * item_size;
        if (transfer_size > f->dma_buf_size) {
            uint8_t *nb = (uint8_t *)realloc(f->dma_buf, transfer_size);
            if (!nb)
                return 0;
            f->dma_buf = nb;
            f->dma_buf_size = transfer_size;
        }
        buf = f->dma_buf;
        /* the block is ours from here */
        s->next_block = w[0];
        if (s->next_block & NODE_POINTER_EOL)
            s->eol = true;
        if (direction) {
            if (interleave) {
                uint32_t i, ch;
                transfer_size = block_count * item_size * channel_count;
                if (transfer_size > f->dma_buf_size) {
                    uint8_t *nb = (uint8_t *)realloc(f->dma_buf, transfer_size);
                    if (!nb)
                        abort();
                    f->dma_buf = buf = nb;
                    f->dma_buf_size = transfer_size;
                }
                for (i = 0; i < block_count; i++)
                    for (ch = 0; ch < channel_count; ch++) {
                        uint32_t v = rd(c, mem_space, mem_address + ch * block_count + i);
                        if (item_size == 2) {
                            uint16_t h = (uint16_t)(v >> 8);
                            memcpy(buf + i * 2 * channel_count + ch * 2, &h, 2);
                        } else {
                            memcpy(buf + i * 4 * channel_count + ch * 4, &v, 4);
                        }
                    }
            } else {
                uint32_t i;
                if (item_size == 2) {
                    for (i = 0; i < count; i++) {
                        uint16_t h = (uint16_t)(rd(c, mem_space, mem_address + i) >> 8);
                        memcpy(buf + i * 2, &h, 2);
                    }
                } else {
                    for (i = 0; i < count; i++) {
                        uint32_t v = rd(c, mem_space, mem_address + i);
                        memcpy(buf + i * 4, &v, 4);
                    }
                }
            }
            {
                LARGE_INTEGER t0, t1;
                if (f->time_io)
                    QueryPerformanceCounter(&t0);
                if (buf_id <= 3)
                    s->fifo_rw(s->rw_opaque, buf, buf_id, transfer_size, 1);
                else if (buf_id == 0xE)
                    fdma_circular(s, scratch_base, &scratch_offset, scratch_size, transfer_size, buf, 1);
                else
                    s->scratch_rw(s->rw_opaque, buf, scratch_base + scratch_offset, transfer_size, 1);
                if (f->time_io) {
                    QueryPerformanceCounter(&t1);
                    f->t_io += t1.QuadPart - t0.QuadPart;
                }
            }
        } else {
            uint32_t i;
            LARGE_INTEGER t0, t1;
            if (f->time_io)
                QueryPerformanceCounter(&t0);
            if (buf_id == 0xE)
                fdma_circular(s, scratch_base, &scratch_offset, scratch_size, transfer_size, buf, 0);
            else
                s->scratch_rw(s->rw_opaque, buf, scratch_base + scratch_offset, transfer_size, 0);
            if (f->time_io) {
                QueryPerformanceCounter(&t1);
                f->t_io += t1.QuadPart - t0.QuadPart;
            }
            for (i = 0; i < count; i++) {
                uint32_t v;
                if (item_size == 2) {
                    uint16_t h;
                    memcpy(&h, buf + i * 2, 2);
                    v = (uint32_t)h << 8;
                } else {
                    memcpy(&v, buf + i * 4, 4);
                    v &= item_mask;
                }
                (void)wr(c, mem_space, mem_address + i, v);
            }
        }
        if (writeback)
            (void)wr(c, space, block_addr + 4, scratch_offset);
    }
    return 1;
}

/* The core's peripheral write during a frame: the DMA control register's
 * actions and run inline, all else the original. */
static void fdma_write_periph(dsp_core_t *core, uint32_t address, uint32_t value)
{
    DSPState *dsp = (DSPState *)core->opaque;
    BwFast *f = (BwFast *)dsp->dma.mem_opaque;
    DSPDMAState *s = &dsp->dma;
    uint32_t act = value & DMA_CONTROL_ACTION;
    if (address != 0xFFFFD6 || s->mem_read != dma_rd_fast || f->fast_dma_off ||
        act < DMA_CONTROL_ACTION_START || act > DMA_CONTROL_ACTION_UNFREEZE) {
        f->wp_orig(core, address, value);
        return;
    }
    switch (act) {
    case DMA_CONTROL_ACTION_START:
        s->control |= DMA_CONTROL_RUNNING;
        s->control &= ~DMA_CONTROL_STOPPED;
        s->dma_read_count = 0;
        break;
    case DMA_CONTROL_ACTION_STOP:
        s->control |= DMA_CONTROL_STOPPED;
        s->control &= ~DMA_CONTROL_RUNNING;
        break;
    case DMA_CONTROL_ACTION_FREEZE:
        s->control |= DMA_CONTROL_FROZEN;
        break;
    default:
        s->control &= ~DMA_CONTROL_FROZEN;
        break;
    }
    if (!fdma_run(f, core, s))
        f->wp_orig(core, address, value);       /* the rest: the action again, then dsp_dma_run */
}

/* emu_update_rn, with linear addressing (M = 0xFFFF) inline. */
BWF_INLINE void upd(dsp_core_t *c, uint32_t n, int16_t mod)
{
    if ((uint16_t)c->registers[DSP_REG_M0 + n] == 0xFFFF)
        c->registers[DSP_REG_R0 + n] = (uint16_t)(c->registers[DSP_REG_R0 + n] + (uint32_t)(int32_t)mod);
    else
        emu_update_rn(c, n, mod);
}

/* emu_calc_ea for modes 0-5 and 7 (6, the absolute address, is op->imm).
 * The cycle and length changes it makes are the handler's (fin). */
BWF_INLINE uint32_t ea_rn(dsp_core_t *c, uint32_t mode, uint32_t n)
{
    uint32_t a, cur;
    switch (mode) {
    case 0:
        a = c->registers[DSP_REG_R0 + n];
        upd(c, n, (int16_t)-c->registers[DSP_REG_N0 + n]);
        return a;
    case 1:
        a = c->registers[DSP_REG_R0 + n];
        upd(c, n, (int16_t)c->registers[DSP_REG_N0 + n]);
        return a;
    case 2:
        a = c->registers[DSP_REG_R0 + n];
        upd(c, n, -1);
        return a;
    case 3:
        a = c->registers[DSP_REG_R0 + n];
        upd(c, n, +1);
        return a;
    case 4:
        return c->registers[DSP_REG_R0 + n];
    case 5:
        cur = c->registers[DSP_REG_R0 + n];
        upd(c, n, (int16_t)c->registers[DSP_REG_N0 + n]);
        a = c->registers[DSP_REG_R0 + n];
        c->registers[DSP_REG_R0 + n] = cur;
        return a;
    default: /* 7 */
        upd(c, n, -1);
        return c->registers[DSP_REG_R0 + n];
    }
}

/* An accumulator set from a 24-bit move: A0 = 0, A1 = v, A2 = its sign. */
BWF_INLINE void set_acc(dsp_core_t *c, uint32_t k, uint32_t v)
{
    c->registers[DSP_REG_A0 + k] = 0;
    c->registers[DSP_REG_A1 + k] = v;
    c->registers[DSP_REG_A2 + k] = v & (1 << 23) ? 0xff : 0x0;
}

/* ---- handlers ------------------------------------------------------------- */

/* Exactly dsp56k_execute_instruction for one instruction, minus the fetch
 * and the table lookup (done once, at decode) and the trace hooks (off).
 * Any instruction, in any state. */
static void h_generic(dsp_core_t *c, const FOp *op)
{
    c->cur_inst = op->w0;
    c->cur_inst_len = 1;
    c->instr_cycle = 2;
    op->emu(c);
    dsp_postexecute_update_pc(c);
    dsp_postexecute_interrupts(c);
    c->num_inst += c->instr_cycle;
}

/* The interpreter's routine with the shared tail: any instruction but REP
 * (which starts a repeat: h_generic), from plain running. The routine sets
 * the length (0 for a jump taken) and the cycles. */
static void h_light(dsp_core_t *c, const FOp *op)
{
    c->cur_inst = op->w0;
    c->cur_inst_len = 1;
    c->instr_cycle = 2;
    op->emu(c);
    c->pc += c->cur_inst_len;
    if ((c->registers[DSP_REG_SR] & BWF_LF) && c->pc == c->registers[DSP_REG_LA] + 1)
        loop_end(c);
    if (c->interrupt_counter)
        dsp_postexecute_interrupts(c);
    c->num_inst += c->instr_cycle;
}

/* xemu's max writes A0 into B2, so A2 and B2 can hold more than 8 bits (and
 * tfr, tcc copy it on). The register-based routines assume 8; with a wide
 * one the interpreter's own routine runs instead. */
BWF_INLINE int acc_wide(const dsp_core_t *c)
{
    return (c->registers[DSP_REG_A2] | c->registers[DSP_REG_B2]) > 0xFF;
}

BWF_INLINE void run_alu(dsp_core_t *c, const FOp *op)
{
    if (acc_wide(c))
        op->alu_slow(c);
    else
        op->alu(c);
}

/* -- parallel moves (emu_pm_*): the move's reads, the ALU, the move's writes.
 * Each comes as a body (b_*: the instruction's own work, returning 1 when it
 * wrote a peripheral, which can halt the DSP or run a DMA) and a handler
 * (h_*: the body and the tail). Blocks run bodies back to back. */

/* 0010 0000 0000 0000 + ALU: no move */
static int b_pm2nop(dsp_core_t *c, const FOp *op)
{
    if (op->alu)
        run_alu(c, op);
    return 0;
}

/* emu_pm_2_2: 0010 00ee eeed dddd, register to register */
static int b_pm2reg(dsp_core_t *c, const FOp *op)
{
    uint32_t v, src = op->r1, dst = op->r2;
    if (src == DSP_REG_A || src == DSP_REG_B)
        v = accu24(c, (src) & 1);
    else
        v = c->registers[src];
    if (op->alu)
        run_alu(c, op);
    if (dst == DSP_REG_A || dst == DSP_REG_B)
        set_acc(c, dst - DSP_REG_A, v);
    else
        c->registers[dst] = v & op->mask;
    return 0;
}

/* emu_pm_3: 001d dddd iiii iiii, #xx to a register (op->imm already shifted) */
static int b_pm3(dsp_core_t *c, const FOp *op)
{
    uint32_t dst = op->r2;
    if (op->alu)
        run_alu(c, op);
    if (dst == DSP_REG_A || dst == DSP_REG_B)
        set_acc(c, dst - DSP_REG_A, op->imm);
    else
        c->registers[dst] = op->imm & op->mask;
    return 0;
}

/* emu_pm_5: one X or Y move, in templates by (address mode, space, direction,
 * accumulator). MODE 0-5 and 7 are the Rn modes, 6 an absolute address
 * (op->imm), 8 a short absolute (op->imm), 9 an immediate (#xxxxxx: op->imm
 * is the value; the interpreter's "write D" path). */
#define PM5_LEN(MODE) (((MODE) == 6 || (MODE) == 9) ? 2 : 1)
#define PM5_CYC(MODE) (((MODE) == 5 || (MODE) == 6 || (MODE) == 7 || (MODE) == 9) ? 4 : 2)

#define DEF_PM5(MODE, SPACE, W, ACC)                                                    \
    static int b_pm5_##MODE##_##SPACE##_##W##_##ACC(dsp_core_t *c, const FOp *op)       \
    {                                                                                   \
        uint32_t a, v;                                                                  \
        if ((MODE) == 6 || (MODE) == 8 || (MODE) == 9)                                  \
            a = op->imm;                                                                \
        else                                                                            \
            a = ea_rn(c, (MODE), op->ea1 & 7);                                          \
        if (W) {                                                                        \
            v = (MODE) == 9 ? a : rd(c, (SPACE), a);                                    \
        } else if (ACC) {                                                               \
            v = accu24(c, (op->r1) & 1);                                          \
        } else {                                                                        \
            v = c->registers[op->r1];                                                   \
        }                                                                               \
        if (op->alu)                                                                    \
            run_alu(c, op);                                                                 \
        if (W) {                                                                        \
            if (ACC)                                                                    \
                set_acc(c, op->r1 - DSP_REG_A, v);                                      \
            else                                                                        \
                c->registers[op->r1] = v & op->mask;                                    \
            return 0;                                                                   \
        }                                                                               \
        return wr(c, (SPACE), a, v);                                                    \
    }

#define DEF_PM5_MODE(MODE)                                                              \
    DEF_PM5(MODE, 0, 0, 0) DEF_PM5(MODE, 0, 0, 1) DEF_PM5(MODE, 0, 1, 0)                \
    DEF_PM5(MODE, 0, 1, 1) DEF_PM5(MODE, 1, 0, 0) DEF_PM5(MODE, 1, 0, 1)                \
    DEF_PM5(MODE, 1, 1, 0) DEF_PM5(MODE, 1, 1, 1)
DEF_PM5_MODE(0) DEF_PM5_MODE(1) DEF_PM5_MODE(2) DEF_PM5_MODE(3) DEF_PM5_MODE(4)
DEF_PM5_MODE(5) DEF_PM5_MODE(6) DEF_PM5_MODE(7) DEF_PM5_MODE(8) DEF_PM5_MODE(9)

#define PM5_ROW(MODE)                                                                   \
    { { { b_pm5_##MODE##_0_0_0, b_pm5_##MODE##_0_0_1 },                                 \
        { b_pm5_##MODE##_0_1_0, b_pm5_##MODE##_0_1_1 } },                               \
      { { b_pm5_##MODE##_1_0_0, b_pm5_##MODE##_1_0_1 },                                 \
        { b_pm5_##MODE##_1_1_0, b_pm5_##MODE##_1_1_1 } } }
static const fop_body s_pm5[10][2][2][2] = {  /* [mode][space][W][acc] */
    PM5_ROW(0), PM5_ROW(1), PM5_ROW(2), PM5_ROW(3), PM5_ROW(4),
    PM5_ROW(5), PM5_ROW(6), PM5_ROW(7), PM5_ROW(8), PM5_ROW(9),
};

/* emu_pm_8: an X move and a Y move (modes 1-4 only). */
static int b_pm8(dsp_core_t *c, const FOp *op)
{
    uint32_t xa, ya, v1, v2, w0 = op->w0;
    uint32_t n1 = op->r1, n2 = op->r2;
    int stop = 0;
    xa = ea_rn(c, op->ea1 >> 3, op->ea1 & 7);
    ya = ea_rn(c, op->ea2 >> 3, op->ea2 & 7);
    if (w0 & (1 << 15))
        v1 = rd(c, DSP_SPACE_X, xa);
    else if (n1 == DSP_REG_A || n1 == DSP_REG_B)
        v1 = accu24(c, (n1) & 1);
    else
        v1 = c->registers[n1];
    if (w0 & (1 << 22))
        v2 = rd(c, DSP_SPACE_Y, ya);
    else if (n2 == DSP_REG_A || n2 == DSP_REG_B)
        v2 = accu24(c, (n2) & 1);
    else
        v2 = c->registers[n2];
    if (op->alu)
        run_alu(c, op);
    if (w0 & (1 << 15)) {
        if (n1 == DSP_REG_A || n1 == DSP_REG_B)
            set_acc(c, n1 - DSP_REG_A, v1);
        else
            c->registers[n1] = v1;
    } else {
        stop |= wr(c, DSP_SPACE_X, xa, v1);
    }
    if (w0 & (1 << 22)) {
        if (n2 == DSP_REG_A || n2 == DSP_REG_B)
            set_acc(c, n2 - DSP_REG_A, v2);
        else
            c->registers[n2] = v2;
    } else {
        stop |= wr(c, DSP_SPACE_Y, ya, v2);
    }
    return stop;
}

/* -- non-parallel instructions --------------------------------------------- */

/* emu_move_xy_imm / emu_move_x_long: X or Y:(Rn + displacement) <-> R. The
 * register is never SP, SSH or SSL here (decode), so no stack effects. The
 * 24-bit address can reach the peripherals. */
static int b_move_disp(dsp_core_t *c, const FOp *op)
{
    uint32_t n = op->r1, v, a = (c->registers[op->r2] + op->imm) & BITMASK(24);
    if (!(op->cc & 1)) {
        if (n == DSP_REG_A || n == DSP_REG_B)
            v = accu24(c, (n) & 1);
        else
            v = c->registers[n];
        return wr(c, op->space, a, v);
    }
    v = rd(c, op->space, a);
    if (n == DSP_REG_A || n == DSP_REG_B)
        set_acc(c, n - DSP_REG_A, v);           /* dsp_write_reg's A and B */
    else if (n < DSP_REG_LCSAVE)
        c->registers[n] = v & op->mask;         /* its default: the width mask */
    else
        dsp_write_reg(c, n, v);                 /* SR, OMR, LA, LC: its own rules */
    return 0;
}

/* The interpreter's routine for this instruction, from inside a block (wide
 * A2/B2): it reads cur_inst and, for 2-word forms, P at pc + 1. The pc and
 * transient fields it leaves are set again by whoever ends the block. */
static void np_slow(dsp_core_t *c, const FOp *op)
{
    c->pc = op->addr;
    c->cur_inst = op->w0;
    c->cur_inst_len = 1;
    c->instr_cycle = 2;
    op->emu(c);
}

/* -- immediate ALU forms (emu_add_x, emu_sub_x, emu_cmp_*, emu_and_x,
 * emu_or_long, emu_asl_imm, emu_asr_imm), in registers like the ALU above.
 * op->imm the operand, op->r1 the destination (0 A, 1 B). */
static int b_add_x(dsp_core_t *c, const FOp *op)
{
    uint32_t nsr, sr;
    uint64_t r = add56(src24(op->imm), acc_get(c, op->r1), &nsr);
    if (acc_wide(c)) {
        np_slow(c, op);
        return 0;
    }
    acc_put(c, op->r1, r);
    sr = ccr(c->registers[DSP_REG_SR], r);
    c->registers[DSP_REG_SR] = (sr & (0xFFFFu - (SR_V | SR_C))) | nsr;
    return 0;
}

static int b_sub_x(dsp_core_t *c, const FOp *op)
{
    uint32_t nsr, sr;
    uint64_t r = sub56(src24(op->imm), acc_get(c, op->r1), &nsr);
    if (acc_wide(c)) {
        np_slow(c, op);
        return 0;
    }
    acc_put(c, op->r1, r);
    sr = ccr(c->registers[DSP_REG_SR], r);
    c->registers[DSP_REG_SR] = (sr & (0xFFFFu - (SR_V | SR_C))) | nsr;
    return 0;
}

/* cmp #xx / #xxxx: the operand at bits 47-24, not sign-extended (xemu) */
static int b_cmp_x(dsp_core_t *c, const FOp *op)
{
    uint32_t nsr, sr;
    uint64_t r = sub56((uint64_t)op->imm << 24, acc_get(c, op->r1), &nsr);
    if (acc_wide(c)) {
        np_slow(c, op);
        return 0;
    }
    sr = ccr(c->registers[DSP_REG_SR], r);
    c->registers[DSP_REG_SR] = (sr & (0xFFFFu - (SR_V | SR_C))) | nsr;
    return 0;
}

static int b_logic_x(dsp_core_t *c, const FOp *op)
{
    uint32_t a = c->registers[DSP_REG_A1 + op->r1], sr = c->registers[DSP_REG_SR];
    a = op->space ? (a | op->imm) : (a & op->imm);       /* space 1: or */
    c->registers[DSP_REG_A1 + op->r1] = a;
    sr &= 0xFFFFu - (SR_N | SR_Z | SR_V);
    sr |= ((a >> 23) & 1) << DSP_SR_N;
    sr |= (uint32_t)(a == 0) << DSP_SR_Z;
    c->registers[DSP_REG_SR] = sr;
    return 0;
}

/* asl / asr #ii, S, D for 1 <= ii <= 55 (dsp_asl56 / dsp_asr56 with n = ii):
 * op->r2 the source, op->r1 the destination, op->cc ii, op->space 1 = asr */
static int b_shift_x(dsp_core_t *c, const FOp *op)
{
    uint64_t v = acc_get(c, op->r2), r;
    uint32_t n = op->cc, nsr, sr = c->registers[DSP_REG_SR];
    if (acc_wide(c)) {
        np_slow(c, op);
        return 0;
    }
    if (op->space) {
        nsr = (uint32_t)(v >> (n - 1)) & 1;
        r = v >> n;
    } else {
        uint64_t sh = v << n;
        uint32_t carry = (uint32_t)(v >> (56 - n)) & 1;
        uint32_t lim = (v >> (56 - n)) != 0;
        uint32_t vv = (uint32_t)(((v >> 55) ^ (sh >> 55)) & 1);
        nsr = (lim << DSP_SR_L) | (vv << DSP_SR_V) | carry;
        r = sh & M56;
    }
    acc_put(c, op->r1, r);
    sr = (sr & (0xFFFFu - (SR_C | SR_V))) | nsr;
    c->registers[DSP_REG_SR] = ccr(sr, r);
    return 0;
}

static int b_nop(dsp_core_t *c, const FOp *op)
{
    (void)c;
    (void)op;
    return 0;
}

/* A body's handler: the body and the shared tail (static length, cycles). */
static void h_body(dsp_core_t *c, const FOp *op)
{
    (void)op->body(c, op);
    fin(c, op->w0, op->len, op->cyc);
}

/* bcc / bra (emu_bcc_*, emu_bra_*): op->imm the displacement, op->len the
 * length, op->cc the condition (16: always). No cycles added (xemu's TODO). */
static void h_branch(dsp_core_t *c, const FOp *op)
{
    if (op->cc == 16 || emu_calc_cc(c, op->cc)) {
        c->pc = (c->pc + op->imm) & BITMASK(24);
        fin(c, op->w0, 0, 2);
    } else {
        fin(c, op->w0, op->len, 2);
    }
}

/* brclr / brset on a register (emu_brclr_reg, emu_brset_reg): r1 the
 * register, cc the bit, space 1 for brset; +4 cycles; 2 words. */
static void h_brbit_reg(dsp_core_t *c, const FOp *op)
{
    uint32_t v, n = op->r1;
    if (n == DSP_REG_A || n == DSP_REG_B)
        v = accu24(c, (n) & 1);
    else
        v = c->registers[n];
    if (((v >> op->cc) & 1) == op->space) {
        c->pc = (c->pc + op->imm) & BITMASK(24);
        fin(c, op->w0, 0, 6);
    } else {
        fin(c, op->w0, 2, 6);
    }
}

/* emu_rts: pop the PC (the SR half is dropped), +2 cycles. */
static void h_rts(dsp_core_t *c, const FOp *op)
{
    uint32_t newpc = 0, newsr;
    dsp_stack_pop(c, &newpc, &newsr);
    c->pc = newpc;
    fin_stack(c, op->w0, 0, 4);
}

/* emu_bsr_long / emu_bsr_imm from plain running (interrupt_state NONE):
 * push PC + length and SR, branch; op->len the length, op->cyc the cycles. */
static void h_bsr(dsp_core_t *c, const FOp *op)
{
    dsp_stack_push(c, c->pc + op->len, c->registers[DSP_REG_SR], 0);
    c->pc = (c->pc + op->imm) & BITMASK(24);
    fin_stack(c, op->w0, 0, op->cyc);
}

/* ---- the cache ------------------------------------------------------------- */

static uint32_t sext(int bits, uint32_t v)
{
    return dsp_signextend(bits, v);
}

/* A register a fast handler may move: not the stack's (SP, SSH, SSL). */
static int plain_reg(uint32_t n)
{
    return n != DSP_REG_SP && n != DSP_REG_SSH && n != DSP_REG_SSL;
}

static void decode_pm(FOp *op, uint32_t w0, uint32_t w1, int have_w1)
{
    emu_func_t alu = opcodes_alu[w0 & 0xFF];
    op->emu = opcodes_parmove[(w0 >> 20) & 0xF];
    op->fn = h_light;
    op->kind = K_LIGHT;
    if (alu == emu_undefined)
        return;                                 /* the interpreter's own path */
    op->alu = alu == emu_move ? NULL : fast_alu[w0 & 0xFF] ? fast_alu[w0 & 0xFF] : alu;
    op->alu_slow = alu;
    switch (pm_kind(w0)) {
    case PMK_2NOP:
        op->body = b_pm2nop;
        op->len = 1;
        op->cyc = 2;
        op->fn = h_body;
        op->kind = K_PM2NOP;
        break;
    case PMK_2_2:
        op->r1 = (uint8_t)((w0 >> 13) & 31);
        op->r2 = (uint8_t)((w0 >> 8) & 31);
        op->mask = BITMASK(registers_mask[op->r2]);
        op->body = b_pm2reg;
        op->len = 1;
        op->cyc = 2;
        op->fn = h_body;
        op->kind = K_PM2REG;
        break;
    case PMK_3: {
        uint32_t dst = (w0 >> 16) & 31, v = (w0 >> 8) & 0xFF;
        switch (dst) {
        case DSP_REG_X0: case DSP_REG_X1: case DSP_REG_Y0: case DSP_REG_Y1:
        case DSP_REG_A: case DSP_REG_B:
            v <<= 16;
            break;
        }
        op->r2 = (uint8_t)dst;
        op->imm = v;
        op->mask = BITMASK(registers_mask[dst]);
        op->body = b_pm3;
        op->len = 1;
        op->cyc = 2;
        op->fn = h_body;
        op->kind = K_PM3;
        break;
    }
    case PMK_5: {
        uint32_t f = (w0 >> 8) & 63, mode, w = (w0 >> 15) & 1;
        uint32_t numreg = ((w0 >> 16) & 7) | ((w0 >> 17) & (3 << 3));
        uint32_t space = (w0 >> 19) & 1;
        if (w0 & (1 << 14)) {
            mode = f >> 3;
            if (mode == 6) {
                if (!have_w1)
                    return;
                op->imm = w1;
                op->deps = 2;
                if ((f & 7) && w)
                    mode = 9;                   /* #xxxxxx */
            }
        } else {
            mode = 8;
            op->imm = f;
        }
        op->ea1 = (uint8_t)f;
        op->r1 = (uint8_t)numreg;
        op->mask = BITMASK(registers_mask[numreg]);
        op->body = s_pm5[mode][space][w][numreg == DSP_REG_A || numreg == DSP_REG_B];
        op->len = (uint8_t)PM5_LEN(mode);
        op->cyc = (uint8_t)PM5_CYC(mode);
        op->fn = h_body;
        op->kind = K_PM5;
        break;
    }
    case PMK_8: {
        static const uint8_t r1t[4] = { DSP_REG_X0, DSP_REG_X1, DSP_REG_A, DSP_REG_B };
        static const uint8_t r2t[4] = { DSP_REG_Y0, DSP_REG_Y1, DSP_REG_A, DSP_REG_B };
        uint32_t ea1 = (w0 >> 8) & BITMASK(5), ea2;
        if ((ea1 >> 3) == 0)
            ea1 |= (1 << 5);
        ea2 = (w0 >> 13) & BITMASK(2);
        ea2 |= (w0 >> 17) & (BITMASK(2) << 3);
        if ((ea1 & (1 << 2)) == 0)
            ea2 |= 1 << 2;
        if ((ea2 >> 3) == 0)
            ea2 |= (1 << 5);
        op->ea1 = (uint8_t)ea1;
        op->ea2 = (uint8_t)ea2;
        op->r1 = r1t[(w0 >> 18) & 3];
        op->r2 = r2t[(w0 >> 16) & 3];
        op->body = b_pm8;
        op->len = 1;
        op->cyc = 2;
        op->fn = h_body;
        op->kind = K_PM8;
        break;
    }
    default:
        break;                                  /* pm_0, pm_1, pm_2 update, pm_4x */
    }
}

static void add_boundary(BwFast *f, uint32_t a);

static void decode_np(BwFast *f, uint32_t pc, FOp *op, const OpcodeEntry *e, uint32_t w0,
                      uint32_t w1, int have_w1)
{
    emu_func_t fn = e->emu_func;
    op->emu = fn ? fn : emu_undefined;
    op->fn = h_light;
    op->kind = K_LIGHT;
    /* writes P (movem S,p:ea; movep x:pp,p:ea): bit 15 clear. With it set
     * they read P (a movep then writes a peripheral, whose DMA bumps the
     * generation itself if it writes P) */
    op->pwrite = (fn == emu_movem_aa || fn == emu_movem_ea || fn == emu_movep_1) &&
                 !(w0 & (1u << 15));
    if (fn == emu_rep_aa || fn == emu_rep_imm || fn == emu_rep_ea || fn == emu_rep_reg) {
        op->fn = h_generic;                     /* starts a repeat */
        op->kind = K_GENERIC;
        return;
    }
    if (have_w1 && f) {
        /* A DO loop's last instruction ends a block: its end check (pc ==
         * LA + 1) then falls at the block's end. LA as emu_do_* / emu_dor_*
         * set it. */
        if (fn == emu_do_aa || fn == emu_do_imm || fn == emu_do_ea || fn == emu_do_reg) {
            add_boundary(f, ((w1 & BITMASK(16)) + 1) & (BWF_PSIZE - 1));
            op->deps = 2;                       /* its second word: checked with the op */
        } else if (fn == emu_dor_imm || fn == emu_dor_reg) {
            add_boundary(f, (((pc + w1) & BITMASK(16)) + 1) & (BWF_PSIZE - 1));
            op->deps = 2;
        }
    }
    if (fn == emu_move_x_imm || fn == emu_move_y_imm) {
        uint32_t xxx = (((w0 >> 11) & BITMASK(6)) << 1) + ((w0 >> 6) & 1);
        op->space = fn == emu_move_y_imm;
        op->cc = (uint8_t)((w0 >> 4) & 1);      /* W */
        op->r2 = (uint8_t)(DSP_REG_R0 + ((w0 >> 8) & BITMASK(3)));
        op->r1 = (uint8_t)(w0 & BITMASK(4));
        op->mask = BITMASK(registers_mask[op->r1]);
        op->imm = sext(7, xxx);
        op->body = b_move_disp;
        op->len = 1;
        op->cyc = 2;
        op->fn = h_body;
        op->kind = K_MOVE_DISP;
    } else if (fn == emu_move_x_long && have_w1 && plain_reg(w0 & BITMASK(6))) {
        op->space = DSP_SPACE_X;
        op->cc = (uint8_t)((w0 >> 6) & 1);
        op->r2 = (uint8_t)(DSP_REG_R0 + ((w0 >> 8) & BITMASK(3)));
        op->r1 = (uint8_t)(w0 & BITMASK(6));
        op->mask = BITMASK(registers_mask[op->r1]);
        op->imm = w1;
        op->deps = 2;
        op->body = b_move_disp;
        op->inblock = !(op->cc && (op->r1 == DSP_REG_SR || op->r1 == DSP_REG_LA ||
                                   op->r1 == DSP_REG_LC));
        op->len = 2;
        op->cyc = 2;
        op->fn = h_body;
        op->kind = K_MOVE_DISP;
    } else if ((fn == emu_bcc_long || fn == emu_bra_long) && have_w1) {
        op->cc = fn == emu_bra_long ? 16 : (uint8_t)(w0 & BITMASK(4));
        op->imm = w1;
        op->len = 2;
        op->deps = 2;
        op->fn = h_branch;
        op->kind = K_BRANCH;
    } else if (fn == emu_bcc_imm || fn == emu_bra_imm) {
        uint32_t xxx = (w0 & BITMASK(5)) + ((w0 & (BITMASK(4) << 6)) >> 1);
        op->cc = fn == emu_bra_imm ? 16 : (uint8_t)((w0 >> 12) & BITMASK(4));
        op->imm = sext(9, xxx);
        op->len = 1;
        op->fn = h_branch;
        op->kind = K_BRANCH;
    } else if ((fn == emu_brclr_reg || fn == emu_brset_reg) && have_w1) {
        op->r1 = (uint8_t)((w0 >> 8) & BITMASK(6));
        op->cc = (uint8_t)(w0 & BITMASK(5));
        op->space = fn == emu_brset_reg;
        op->imm = w1;
        op->deps = 2;
        op->fn = h_brbit_reg;
        op->kind = K_BITBRANCH;
    } else if (fn == emu_add_imm || fn == emu_sub_imm || fn == emu_cmp_imm || fn == emu_and_imm ||
               ((fn == emu_add_long || fn == emu_sub_long || fn == emu_cmp_long ||
                 fn == emu_and_long || fn == emu_or_long) && have_w1)) {
        int is_long = fn == emu_add_long || fn == emu_sub_long || fn == emu_cmp_long ||
                      fn == emu_and_long || fn == emu_or_long;
        op->imm = is_long ? w1 : (w0 >> 8) & BITMASK(6);
        op->r1 = (uint8_t)((w0 >> 3) & 1);
        op->space = fn == emu_or_long;
        op->body = (fn == emu_add_imm || fn == emu_add_long) ? b_add_x
                 : (fn == emu_sub_imm || fn == emu_sub_long) ? b_sub_x
                 : (fn == emu_cmp_imm || fn == emu_cmp_long) ? b_cmp_x : b_logic_x;
        op->len = is_long ? 2 : 1;
        op->cyc = 2;
        if (is_long)
            op->deps = 2;
        op->fn = h_body;
        op->kind = K_IMM_ALU;
    } else if ((fn == emu_asl_imm || fn == emu_asr_imm) && ((w0 >> 1) & 63) >= 1 &&
               ((w0 >> 1) & 63) <= 55) {
        op->cc = (uint8_t)((w0 >> 1) & 63);
        op->r2 = (uint8_t)((w0 >> 7) & 1);
        op->r1 = (uint8_t)(w0 & 1);
        op->space = fn == emu_asr_imm;
        op->body = b_shift_x;
        op->len = 1;
        op->cyc = 2;
        op->fn = h_body;
        op->kind = K_IMM_ALU;
    } else if (fn == emu_nop) {
        op->body = b_nop;
        op->len = 1;
        op->cyc = 2;
        op->fn = h_body;
        op->kind = K_NOP;
    } else if (fn == emu_rts) {
        op->fn = h_rts;
        op->kind = K_RTS;
    } else if (fn == emu_bsr_long && have_w1) {
        op->imm = w1;
        op->len = 2;
        op->cyc = 6;
        op->deps = 2;
        op->fn = h_bsr;
        op->kind = K_BSR;
    } else if (fn == emu_bsr_imm) {
        uint32_t xxx = (w0 & BITMASK(5)) + ((w0 & (BITMASK(4) << 6)) >> 1);
        op->imm = sext(9, xxx);
        op->len = 1;
        op->cyc = 4;
        op->fn = h_bsr;
        op->kind = K_BSR;
    }
}

static void decode_into(BwFast *f, dsp_core_t *c, uint32_t pc, FOp *op)
{
    uint32_t w0 = c->pram[pc];
    int have_w1 = pc + 1 < BWF_PSIZE;
    uint32_t w1 = have_w1 ? c->pram[pc + 1] : 0;

    memset(op, 0, sizeof *op);
    op->w0 = w0;
    op->addr = (uint16_t)pc;
    op->deps = 1;
    f->n_decode++;
    if (w0 >= 0x100000) {
        decode_pm(op, w0, w1, have_w1);
    } else {
        const OpcodeEntry *e = np_lookup(w0);
        if (!e) {
            op->fn = NULL;          /* the interpreter's step (it asserts) */
            op->kind = K_SLOW;
            return;
        }
        decode_np(f, pc, op, e, w0, w1, have_w1);
    }
    if (op->body && op->fn == h_body && op->len && !(op->kind == K_MOVE_DISP && op->len == 2))
        op->inblock = 1;
    if (op->deps == 2)
        op->w1 = w1;
}

static void decode(BwFast *f, dsp_core_t *c, uint32_t pc)
{
    decode_into(f, c, pc, &f->ops[pc]);
}

/* ---- blocks ------------------------------------------------------------------
 *
 * A block is a run of instructions from one start address: bodies (moves,
 * parallel moves with their ALU, nop) back to back, then one last instruction
 * of any kind with its whole handler. The interpreter's per-instruction
 * bookkeeping (pc, the DO loop's end check, the cycle chunks, the counters,
 * the transient fields) is done once for the bodies, which is exact when, at
 * the block's start:
 *   - the core is in plain running (as for any handler);
 *   - P still holds the words the block was decoded from;
 *   - no 1000-cycle chunk ends after any body (save_cycles stays > 0), so
 *     the cap and the chunk carry are only ever checked where they would be;
 *   - the DO loop's end (LA + 1, when LF is set) is not the address after a
 *     body (blocks end at every DO loop end seen, so it is the block's end or
 *     outside it);
 * and the bodies change none of it: they move only X, Y and the registers
 * below 0x20 (and never LF, LA, LC or the stack), and one that wrote a
 * peripheral (which may halt the DSP or run a DMA) stops the block right
 * after it, exactly as after an instruction. */

#define BLK_MAX 48
#define POOL_OPS 16384
#define POOL_WORDS 32768

typedef struct Block {
    uint16_t start, last, end;  /* first and last instruction; [start, end) words */
    uint16_t n;                 /* instructions (0: not built) */
    int body_cycles;            /* the cycles of all but the last */
    uint32_t pool, wpool;       /* its instructions and words */
    uint32_t gen;               /* P's generation when its words last matched */
    uint16_t end_pc;            /* the address after its last instruction */
    uint8_t loop_ok;            /* its last instruction is a body (a DO loop's own runner) */
    int total_cycles;           /* all of its instructions, when they are all bodies */
    uint32_t runs;              /* runs so far, until compiled */
    void *jit_s, *jit_l;        /* compiled: straight (bodies but the last), loop */
    void *frag;                 /* its fragment for chains (ctab's entry while valid) */
    uint32_t rmask_s, rmask_l;  /* the address registers each updates */
    uint32_t mode_s, mode_l;    /* the mode each was compiled for (jit_mode_of) */
    uint8_t tried_s, tried_l, tried_c;   /* compiling was tried (straight, loop, chain) */
    uint8_t seen;               /* how it has run: 1 straight, 2 as a DO loop */
    uint8_t frag_paths;         /* the paths its fragment has */
} Block;

static void jit_reset(struct Jit *j);

static void ctab_reset(BwFast *f);
static void ctab_clear(BwFast *f, uint32_t s);

static void flush_blocks(BwFast *f)
{
    memset(f->blk, 0, BWF_PSIZE * sizeof(Block));
    memset(f->code_map, 0, sizeof f->code_map);
    f->pool_n = f->wpool_n = 0;
    jit_reset(f->jit);
    ctab_reset(f);
    f->jit_full = 0;
}

/* A new DO loop end: blocks running across it are built again. */
static void add_boundary(BwFast *f, uint32_t a)
{
    uint32_t s;
    if (f->boundary[a])
        return;
    f->boundary[a] = 1;
    if (!f->blk)
        return;
    for (s = 0; s < BWF_PSIZE; s++) {
        Block *b = &f->blk[s];
        if (b->n && b->start < a && a <= b->last)
            b->n = 0;
    }
}

static Block *build_block(BwFast *f, dsp_core_t *c, uint32_t s)
{
    Block *b = &f->blk[s];
    uint32_t a = s, n = 0, cyc = 0, end = s, i;
    FOp *ops;

    if (f->pool_n + BLK_MAX > POOL_OPS || f->wpool_n + 2 * BLK_MAX + 2 > POOL_WORDS)
        flush_blocks(f);
    f->n_builds++;
    ops = &f->pool[f->pool_n];
    for (;;) {
        FOp *op = &ops[n];
        uint32_t next;
        decode_into(f, c, a, op);
        n++;
        if (a + op->deps > end)
            end = a + op->deps;
        if (!op->body || !op->inblock)
            break;                              /* ends the block: its handler */
        next = a + op->len;
        if (n == BLK_MAX || next >= BWF_PSIZE || f->boundary[next])
            break;                              /* a body ends it: h_body */
        cyc += op->cyc;
        a = next;
    }
    if (end > BWF_PSIZE)
        end = BWF_PSIZE;
    b->start = (uint16_t)s;
    b->last = (uint16_t)a;
    b->loop_ok = ops[n - 1].body && ops[n - 1].inblock && ops[n - 1].fn == h_body;
    b->end_pc = (uint16_t)(a + (b->loop_ok ? ops[n - 1].len : 0));
    b->total_cycles = (int)cyc + (b->loop_ok ? ops[n - 1].cyc : 0);
    b->end = (uint16_t)end;
    b->n = (uint16_t)n;
    b->body_cycles = (int)cyc;
    b->pool = f->pool_n;
    b->wpool = f->wpool_n;
    for (i = s; i < end; i++)
        f->wpool[f->wpool_n++] = c->pram[i];
    memset(f->code_map + s, 1, end - s);
    f->pool_n += n;
    b->gen = f->gen;
    b->runs = 0;
    b->jit_s = b->jit_l = NULL;
    b->tried_s = b->tried_l = b->tried_c = 0;
    b->seen = b->frag_paths = 0;
    b->frag = NULL;
    ctab_clear(f, s);
    return b;
}

/* The block at pc, built (again) when P no longer holds its words. */
BWF_INLINE Block *get_block(BwFast *f, dsp_core_t *c, uint32_t pc)
{
    Block *b = &f->blk[pc];
    if (b->n) {
        const uint32_t *w, *p;
        uint32_t i, k;
        if (b->gen == f->gen) {
            if (b->frag && f->ctab_gen == f->gen)
                f->ctab[pc] = b->frag;  /* (the table may have been emptied since) */
            return b;
        }
        w = &f->wpool[b->wpool];
        p = &c->pram[pc];
        k = b->end - pc;
        for (i = 0; i < k; i++)
            if (w[i] != p[i])
                break;
        if (i == k) {
            b->gen = f->gen;
            if (b->frag && f->ctab_gen == f->gen)
                f->ctab[pc] = b->frag;  /* checked again: its fragment back in the table */
            return b;
        }
    }
    return build_block(f, c, pc);
}

#include "bw_dsp_jit.inc"

/* What a compiled block assumes when it runs: SR's scaling mode 00 (its
 * limiting and condition codes are mode 0's) and linear addressing on the
 * address registers it updates. */
BWF_INLINE int jit_entry_ok(const dsp_core_t *c, uint32_t rmask, uint32_t mode)
{
    uint32_t r;
    if (((c->registers[DSP_REG_SR] >> DSP_SR_S0) & 3) != (mode & 3))
        return 0;                       /* the scaling mode it was compiled for */
    if (acc_wide(c))
        return 0;                       /* A2/B2 must fit the 56-bit form */
    for (r = 0; rmask; r++, rmask >>= 1)
        if ((rmask & 1) &&
            (uint16_t)c->registers[DSP_REG_M0 + r] != (((mode >> (8 + r)) & 1) ? 0 : 0xFFFF))
            return 0;
    return 1;
}

/* Compiles a block's variant after it ran often enough (loop: the whole DO
 * loop body; else the bodies before its last instruction). */
static void jit_maybe(BwFast *f, Block *b, int loop, const dsp_core_t *c)
{
    uint32_t rmask = 0, mode = 0;
    jit_fn fn = NULL;
    b->seen |= loop ? 2 : 1;
    if (!f->jit || b->runs < JIT_AFTER_RUNS)
        return;
    if (!f->chain_off && !b->tried_c) {
        /* the block's fragment for chains (with its loop path for a DO loop body) */
        void *entry;
        b->tried_c = 1;
        s_frag_counter = getenv("BW_DSP_JIT_COUNT") ? &f->n_chain_frags : NULL;
        entry = jit_compile_chain(f->jit, &f->pool[b->pool], b->n, b->start, b->last,
                                  b->body_cycles, &f->ctab_gen, &f->gen, f->ctab, b->loop_ok,
                                  b->total_cycles, b->end_pc, c,
                                  getenv("BW_DSP_JIT_BOTHPATHS") ? 3 : b->seen);
        if (entry) {
            b->frag = entry;
            b->frag_paths = (b->loop_ok && jit_body_ok(&f->pool[b->pool + b->n - 1])) ? b->seen : 1;
            if (b->gen == f->gen && f->ctab_gen == f->gen)
                f->ctab[b->start] = entry;
            f->jit->tag[f->jit->nfn - 1] = b->start | (1u << 16);
        } else if (f->jit->used + 4096 + b->n * 2800 > JIT_COLD_START ||
                   f->jit->cold_used + 1024 + b->n * 800 > JIT_BYTES || f->jit->nfn >= JIT_MAX_FUNCS) {
            f->jit_full = 1;            /* dropped with the blocks at the next turn */
            b->tried_c = 0;
            return;
        }
    }
    if (b->frag && (b->frag_paths & (loop ? 2 : 1)))
        return;                         /* its fragment covers it (and keeps the code compact) */
    if (loop ? b->tried_l : b->tried_s)
        return;
    if (loop)
        b->tried_l = 1;
    else
        b->tried_s = 1;
    if (loop || b->n >= 2)
        fn = jit_compile(f->jit, &f->pool[b->pool], b->n, loop, &rmask, c, &mode);
    if (fn) {
        f->n_jit++;
        f->jit->tag[f->jit->nfn - 1] = b->start | ((loop ? 3u : 2u) << 16);
        if (loop) {
            b->jit_l = (void *)fn;
            b->rmask_l = rmask;
            b->mode_l = mode;
        } else {
            b->jit_s = (void *)fn;
            b->rmask_s = rmask;
            b->mode_s = mode;
        }
    } else if ((loop || b->n >= 2) &&
               (f->jit->used + 4096 + b->n * 1400 > JIT_COLD_START || f->jit->nfn >= JIT_MAX_FUNCS)) {
        f->jit_full = 1;                /* dropped with the blocks at the next turn */
        if (loop)
            b->tried_l = 0;
        else
            b->tried_s = 0;
    }
}

static void ctab_clear(BwFast *f, uint32_t s)
{
    if (f->jit)
        f->ctab[s] = f->jit->exit_dispatch;
}

static void ctab_reset(BwFast *f)
{
    uint32_t i;
    if (!f->jit)
        return;
    for (i = 0; i < BWF_PSIZE; i++)
        f->ctab[i] = f->jit->exit_dispatch;
}

/* A body wrote a peripheral: the state right after it (op), the cycles of
 * the instructions before it in `cyc`. */
BWF_INLINE void after_stop(dsp_core_t *c, const FOp *op, uint32_t pc, int cyc, int *save, uint32_t *cc)
{
    cyc += op->cyc;
    c->pc = pc + op->len;
    c->cur_inst = op->w0;
    c->cur_inst_len = op->len;
    c->instr_cycle = op->cyc;
    c->num_inst += (uint32_t)cyc;
    *save -= cyc;
    *cc += (uint32_t)cyc;
}

/* A DO loop whose body is this block, from its first instruction to the
 * loop's last (all bodies), with LF set, LA + 1 the block's end and the loop
 * start (SSH) the block's start: the iterations back to back. Per iteration
 * what the interpreter does: the instructions, then at the last one's end
 * LC - 1, and either back to SSH or, at 0, the loop's end (two pops, SR's
 * mode bits cleared but LF from the pop). The bodies change none of LF, LA,
 * SSH, the stack or P, so only the cycle budget and a peripheral write can
 * stop it early. */
static void run_loop(BwFast *f, dsp_core_t *c, Block *b, int *psave, uint32_t *pcc)
{
    const FOp *ops = &f->pool[b->pool], *last = &ops[b->n - 1];
    int save = *psave, total = b->total_cycles;
    uint32_t cc = *pcc, n = b->n;
    b->runs++;
    jit_maybe(f, b, 1, c);
    if (b->jit_l && jit_entry_ok(c, b->rmask_l, b->mode_l)) {
        JitLoop L;
        int r;
        L.save = save;
        L.cc = cc;
        L.total = total;
        L.body_cycles = total - last->cyc;
        r = ((jit_fn)b->jit_l)(c, &L);
        save = L.save;
        cc = L.cc;
        if (r == JR_LOOP_END) {
            uint32_t saved_pc, saved_sr;
            s_n_loop_end++;
            c->pc = b->end_pc;
            dsp_stack_pop(c, &saved_pc, &saved_sr);
            c->registers[DSP_REG_SR] &= 0x7f;
            c->registers[DSP_REG_SR] |= saved_sr & BWF_LF;
            dsp_stack_pop(c, &c->registers[DSP_REG_LA], &c->registers[DSP_REG_LC]);
            if (c->interrupt_counter)
                dsp_postexecute_interrupts(c);
        } else if (r == JR_LOOP_BUDGET) {
            s_n_loop_end++;
            c->pc = c->registers[DSP_REG_SSH];
        } else {
            /* body i wrote a peripheral; save and cc are as at its iteration's start */
            uint32_t i = (uint32_t)r & 0xFFF, k, pc = b->start;
            int cyc = 0;
            if (i + 1 < n) {
                for (k = 0; k < i; k++) {
                    cyc += ops[k].cyc;
                    pc += ops[k].len;
                }
                after_stop(c, &ops[i], pc, cyc, &save, &cc);
                *psave = save;
                *pcc = cc;
                return;
            }
            /* the last one: the iteration's end, then out (as below) */
            {
                uint32_t lc;
                c->num_inst += (uint32_t)total;
                save -= total;
                cc += (uint32_t)total;
                lc = (c->registers[DSP_REG_LC] - 1) & BITMASK(16);
                c->registers[DSP_REG_LC] = lc;
                s_n_loop_end++;
                if (lc == 0) {
                    uint32_t saved_pc, saved_sr;
                    c->pc = b->end_pc;
                    dsp_stack_pop(c, &saved_pc, &saved_sr);
                    c->registers[DSP_REG_SR] &= 0x7f;
                    c->registers[DSP_REG_SR] |= saved_sr & BWF_LF;
                    dsp_stack_pop(c, &c->registers[DSP_REG_LA], &c->registers[DSP_REG_LC]);
                    if (c->interrupt_counter)
                        dsp_postexecute_interrupts(c);
                } else {
                    c->pc = c->registers[DSP_REG_SSH];
                }
            }
        }
        c->cur_inst = last->w0;
        c->cur_inst_len = last->len;
        c->instr_cycle = last->cyc;
        *psave = save;
        *pcc = cc;
        return;
    }
    for (;;) {
        const FOp *op = ops;
        uint32_t i, pc = b->start, lc;
        int cyc = 0, last_stop = 0;
        for (i = 0; i < n; i++, op++) {
            if (op->body(c, op)) {
                if (i + 1 < n) {
                    after_stop(c, op, pc, cyc, &save, &cc);
                    goto out;
                }
                last_stop = 1;          /* the last one: its tail (the loop's end), then out */
            }
            cyc += op->cyc;
            pc += op->len;
        }
        c->num_inst += (uint32_t)total;
        save -= total;
        cc += (uint32_t)total;
        lc = (c->registers[DSP_REG_LC] - 1) & BITMASK(16);
        c->registers[DSP_REG_LC] = lc;
        s_n_loop_end++;
        if (lc == 0) {
            uint32_t saved_pc, saved_sr;
            c->pc = b->end_pc;
            dsp_stack_pop(c, &saved_pc, &saved_sr);
            c->registers[DSP_REG_SR] &= 0x7f;
            c->registers[DSP_REG_SR] |= saved_sr & BWF_LF;
            dsp_stack_pop(c, &c->registers[DSP_REG_LA], &c->registers[DSP_REG_LC]);
            if (c->interrupt_counter)
                dsp_postexecute_interrupts(c);
            break;
        }
        c->pc = c->registers[DSP_REG_SSH];
        if (last_stop || c->is_idle || save <= 0 || save - (total - last->cyc) <= 0)
            break;
    }
    c->cur_inst = last->w0;
    c->cur_inst_len = last->len;
    c->instr_cycle = last->cyc;
out:
    *psave = save;
    *pcc = cc;
}

/* A compiled DO loop stopped in body i (a peripheral write, or a bail); save
 * and cc as at that iteration's start: the state after body i (i < n - 1), or
 * after the iteration's end (the last body), as run_loop does. */
static void loop_stop_c(BwFast *f, dsp_core_t *c, const Block *b, uint32_t i, int save_in,
                        uint32_t cc_in, int *psave, uint32_t *pcc)
{
    const FOp *ops = &f->pool[b->pool], *last = &ops[b->n - 1];
    int save = save_in, total = b->total_cycles;
    uint32_t cc = cc_in, n = b->n;
    if (i + 1 < n) {
        uint32_t k, pc = b->start;
        int cyc = 0;
        for (k = 0; k < i; k++) {
            cyc += ops[k].cyc;
            pc += ops[k].len;
        }
        after_stop(c, &ops[i], pc, cyc, &save, &cc);
    } else {
        uint32_t lc;
        c->num_inst += (uint32_t)total;
        save -= total;
        cc += (uint32_t)total;
        lc = (c->registers[DSP_REG_LC] - 1) & BITMASK(16);
        c->registers[DSP_REG_LC] = lc;
        s_n_loop_end++;
        if (lc == 0) {
            uint32_t saved_pc, saved_sr;
            c->pc = b->end_pc;
            dsp_stack_pop(c, &saved_pc, &saved_sr);
            c->registers[DSP_REG_SR] &= 0x7f;
            c->registers[DSP_REG_SR] |= saved_sr & BWF_LF;
            dsp_stack_pop(c, &c->registers[DSP_REG_LA], &c->registers[DSP_REG_LC]);
            if (c->interrupt_counter)
                dsp_postexecute_interrupts(c);
        } else {
            c->pc = c->registers[DSP_REG_SSH];
        }
        c->cur_inst = last->w0;
        c->cur_inst_len = last->len;
        c->instr_cycle = last->cyc;
    }
    *psave = save;
    *pcc = cc;
}

/* Runs a block whose conditions hold, with the frame loop's accounting for
 * every instruction it runs (save_cycles, the cycle count). When the last
 * instruction closes a DO loop iteration back to the block's start, and the
 * conditions still hold, it runs the block again in place (run_loop, when the
 * whole block is the loop's body). Returns with the state after the last
 * instruction run, as after one of the frame loop's. */
static void run_block(BwFast *f, dsp_core_t *c, Block *b, int *psave, uint32_t *pcc)
{
    int save = *psave;
    uint32_t cc = *pcc;
    for (;;) {
        const FOp *op = &f->pool[b->pool];
        uint32_t i, n1 = b->n - 1, pc = b->start, la1;
        int cyc = 0;
        if (b->loop_ok && (c->registers[DSP_REG_SR] & BWF_LF) &&
            c->registers[DSP_REG_LA] + 1 == b->end_pc && c->registers[DSP_REG_SSH] == b->start) {
            run_loop(f, c, b, &save, &cc);
            break;
        }
        b->runs++;
        jit_maybe(f, b, 0, c);
        if (b->jit_s && jit_entry_ok(c, b->rmask_s, b->mode_s)) {
            int r = ((jit_fn)b->jit_s)(c, NULL);
            if (r != JR_DONE) {
                uint32_t k = (uint32_t)r & 0xFFF;
                for (i = 0; i < k; i++, op++) {
                    cyc += op->cyc;
                    pc += op->len;
                }
                after_stop(c, op, pc, cyc, &save, &cc);
                goto out;
            }
            op += n1;
            cyc = b->body_cycles;
            pc = b->last;
        } else {
            for (i = 0; i < n1; i++, op++) {
                if (op->body(c, op)) {
                    after_stop(c, op, pc, cyc, &save, &cc);
                    goto out;
                }
                cyc += op->cyc;
                pc += op->len;
            }
            if (s_frag_counter)
                s_cbody_cyc += (uint32_t)cyc;   /* a bench aid: cycles in C bodies */
        }
        c->num_inst += (uint32_t)cyc;
        c->pc = pc;
        if (op->fn == h_body) {
            (void)op->body(c, op);
            fin(c, op->w0, op->len, op->cyc);
        } else if (op->fn) {
            op->fn(c, op);              /* a P write: the hook */
        } else {
            dsp56k_execute_instruction(c);
        }
        cyc += c->instr_cycle;
        save -= cyc;
        cc += (uint32_t)cyc;
        if (c->pc != b->start || c->is_idle || save - b->body_cycles <= 0 || b->gen != f->gen ||
            (c->loop_rep | c->interrupt_state | c->interrupt_counter))
            break;
        la1 = c->registers[DSP_REG_LA] + 1;
        if ((c->registers[DSP_REG_SR] & BWF_LF) && la1 > b->start && la1 <= b->last)
            break;
    }
out:
    *psave = save;
    *pcc = cc;
}

/* The instructions a REP repeats, when the repeated one is a body: each
 * repetition is one instruction for the interpreter (its own cycles, the
 * chunk check after it), with update_pc's REP branch: LC - 1, staying on
 * the instruction while it is above 0, then LC from LCSAVE and on. */
static void run_rep(BwFast *f, dsp_core_t *c, const FOp *op, int *psave, uint32_t *pcc)
{
    int save = *psave;
    uint32_t cc = *pcc, cyc = op->cyc;
    (void)f;
    for (;;) {
        int stop = op->body(c, op);
        uint32_t lc = (c->registers[DSP_REG_LC] - 1) & BITMASK(16), len;
        if (lc > 0) {
            c->registers[DSP_REG_LC] = lc;
            len = 0;
        } else {
            c->loop_rep = 0;
            c->registers[DSP_REG_LC] = c->registers[DSP_REG_LCSAVE];
            len = op->len;
        }
        c->pc += len;
        c->cur_inst = op->w0;
        c->cur_inst_len = len;
        c->instr_cycle = (uint16_t)cyc;
        if ((c->registers[DSP_REG_SR] & BWF_LF) && c->pc == c->registers[DSP_REG_LA] + 1)
            loop_end(c);
        if (!c->loop_rep && c->interrupt_counter)
            dsp_postexecute_interrupts(c);
        c->num_inst += cyc;
        save -= (int)cyc;
        cc += cyc;
        if (stop || c->is_idle || save <= 0 || !c->loop_rep)
            break;
    }
    *psave = save;
    *pcc = cc;
}

/* ---- the frame loop ------------------------------------------------------- */

BwFast *bwf_new(int is_gp)
{
    BwFast *f = (BwFast *)calloc(1, sizeof(BwFast));
    int i;
    if (!f)
        return NULL;
    tables_init();
    alu_quirks();
    f->is_gp = is_gp;
    for (i = 0; i < BWF_PSIZE; i++)
        f->ops[i].w0 = BWF_NOWORD;
    {
        const char *e = getenv("BW_DSP_FASTDMA");
        const char *t = getenv("BW_DSP_TIME");
        f->fast_dma_off = e && e[0] == '0';
        f->time_io = t && t[0] == '1';
    }
    f->jit = jit_wanted() ? jit_new() : NULL;
    if (f->jit)
        s_last_jit = f->jit;
    {
        const char *e = getenv("BW_DSP_JIT_CHAIN");
        f->chain_off = e && e[0] == '0';
    }
    ctab_reset(f);
    f->blk = (Block *)calloc(BWF_PSIZE, sizeof(Block));
    f->pool = (FOp *)calloc(POOL_OPS, sizeof(FOp));
    f->wpool = (uint32_t *)calloc(POOL_WORDS, sizeof(uint32_t));
    if (!f->blk || !f->pool || !f->wpool) {
        bwf_free(f);
        return NULL;
    }
    return f;
}

void bwf_free(BwFast *f)
{
    if (!f)
        return;
    jit_free(f->jit);
    free(f->dma_buf);
    free(f->blk);
    free(f->pool);
    free(f->wpool);
    free(f);
}

/* The cache entry for pc, decoded again if P no longer holds its words. */
BWF_INLINE const FOp *lookup(BwFast *f, dsp_core_t *c, uint32_t pc)
{
    FOp *op = &f->ops[pc];
    if (op->w0 != c->pram[pc] || (op->deps == 2 && op->w1 != c->pram[pc + 1]))
        decode(f, c, pc);
    return op;
}

/* The counting step (BW_DSP_FASTSTAT): the same, with the statistics. */
static void step_counted(BwFast *f, dsp_core_t *c)
{
    uint32_t pc = c->pc;
    if (pc < BWF_PSIZE && !(c->loop_rep | c->interrupt_state | c->interrupt_counter)) {
        const FOp *op = lookup(f, c, pc);
        if (op->fn) {
            if (f->prof) {
                if (op->w0 >= 0x100000) {
                    f->prof_pm[pm_kind(op->w0)][op->w0 & 0xFF]++;
                } else {
                    const OpcodeEntry *e = np_lookup(op->w0);
                    f->prof_np[e ? e - nonparallel_opcodes : ARRAY_SIZE(nonparallel_opcodes)]++;
                }
            }
            op->fn(c, op);
            f->n_fast++;
            f->kind_n[op->kind]++;
            return;
        }
    }
    dsp56k_execute_instruction(c);
    f->n_slow++;
    f->kind_n[K_SLOW]++;
}

/* One frame. The loops are bw_dsp_run_frame's and dsp_c_run's:
 *     do { dsp_run(1000) } while (!halt && cycles < max)
 *     dsp_run: save_cycles += 1000; if (save_cycles <= 0) return;
 *              while (save_cycles > 0) { one instruction;
 *                  save_cycles -= instr_cycle; cycle_count += instr_cycle;
 *                  if (halt) break; }
 * One instruction is the cached handler when the cache entry still matches
 * P and the core is in plain running (no REP in progress, nothing in the
 * interrupt pipeline), else the interpreter's own step. */
uint32_t bwf_run_frame(BwFast *f, DSPState *dsp, uint32_t max_cycles)
{
    dsp_core_t *c = (dsp_core_t *)dsp->backend;
    int save;
    uint32_t cc;

    dsp_start_frame(dsp);
    dsp_set_halt_requested(dsp, false);
    dsp_set_cycle_count(dsp, 0);

    save = dsp->save_cycles;
    cc = 0;
    if (dsp->dma.mem_write != dma_wr && dsp->dma.mem_write != dma_wr_fast) {
        f->dma_opaque = dsp->dma.mem_opaque;
        f->dma_read = dsp->dma.mem_read;
        f->dma_write = dsp->dma.mem_write;
        dsp->dma.mem_opaque = f;
        /* the C backend's own callbacks, on this core: the inline versions */
        if (f->dma_opaque == (void *)c && s_c_dma_read && f->dma_read == s_c_dma_read &&
            f->dma_write == s_c_dma_write) {
            dsp->dma.mem_read = dma_rd_fast;
            dsp->dma.mem_write = dma_wr_fast;
        } else {
            dsp->dma.mem_read = dma_rd;
            dsp->dma.mem_write = dma_wr;
        }
    }
    if (c->write_peripheral != fdma_write_periph) {
        f->wp_orig = c->write_peripheral;
        c->write_peripheral = fdma_write_periph;
    }
    s_pw_gen = &f->gen;
    s_pw_map = f->code_map;
    s_pw_pram = c->pram;
    for (;;) {
        save += 1000;
        while (save > 0) {
            uint32_t pc = c->pc, ic;
            if (f->stats) {
                step_counted(f, c);
                f->gen++;
            } else if (pc < BWF_PSIZE && !(c->loop_rep | c->interrupt_state | c->interrupt_counter)) {
                Block *b;
                if (f->jit_full)
                    flush_blocks(f);
                if (f->jit && f->ctab_gen != f->gen) {
                    /* P changed under some block: no fragment runs until its
                     * block is checked again (get_block puts it back) */
                    ctab_reset(f);
                    f->ctab_gen = f->gen;
                }
                if (f->jit && f->ctab[pc] != (void *)f->jit->exit_dispatch) {
                    JitRun R;
                    R.save = save;
                    R.cc = cc;
                    R.code = 0;
                    R.block_pc = 0;
                    R.entry = f->ctab[pc];
                    f->n_chain_calls++;
                    (void)f->jit->tramp(c, &R);
                    if (s_frag_counter && c->pc < BWF_PSIZE) {
                        /* a bench aid: where chains end, and why */
                        uint32_t why = (R.code & 0xF000) == JC_DISPATCH
                                           ? (f->ctab[c->pc] == (void *)f->jit->exit_dispatch ? 0 : 1)
                                           : (R.code & 0xF000) == JC_CHUNK ? 2 : 3;
                        s_exit_hist[c->pc][why]++;
                        if (why == 1) {
                            const Block *fb = &f->blk[c->pc];
                            uint32_t la1f = c->registers[DSP_REG_LA] + 1, m = 0;
                            if (fb->gen != f->gen)
                                m |= 1;
                            if (acc_wide(c))
                                m |= 2;
                            if (c->registers[DSP_REG_SR] & ((1u << DSP_SR_S0) | (1u << DSP_SR_S1)))
                                m |= 4;
                            if (!jit_entry_ok(c, fb->rmask_s | fb->rmask_l, 0))
                                m |= 8;
                            if ((c->registers[DSP_REG_SR] & BWF_LF) && la1f > c->pc && la1f <= fb->last)
                                m |= 16;
                            if (R.save - fb->body_cycles <= 0)
                                m |= 32;
                            s_fail_why[c->pc] |= m;
                            if (m & 4)
                                s_fail_n[c->pc][0]++;
                            if (m & 8)
                                s_fail_n[c->pc][1]++;
                            s_fail_sr[c->pc] = c->registers[DSP_REG_SR];
                            {
                                uint32_t rm = fb->rmask_s | fb->rmask_l, q;
                                for (q = 0; q < 8; q++)
                                    if ((rm >> q) & 1)
                                        s_fail_m[c->pc][q] = c->registers[DSP_REG_M0 + q];
                            }
                        }
                    }
                    if (R.save != save)
                        f->n_chain_runs++;
                    if ((R.code & 0xF000) == JR_LSTOP) {
                        loop_stop_c(f, c, &f->blk[R.block_pc], R.code & 0xFFF, R.save, R.cc,
                                    &save, &cc);
                        if (c->is_idle)
                            break;
                        continue;
                    }
                    if ((R.code & 0xF000) == JR_STOP) {
                        /* body i of the block at R.block_pc wrote a peripheral (or bailed) */
                        const Block *sb = &f->blk[R.block_pc];
                        const FOp *sop = &f->pool[sb->pool];
                        uint32_t si = R.code & 0xFFF, k, spc = R.block_pc;
                        int scyc = 0;
                        save = R.save;
                        cc = R.cc;
                        for (k = 0; k < si; k++) {
                            scyc += sop[k].cyc;
                            spc += sop[k].len;
                        }
                        after_stop(c, &sop[si], spc, scyc, &save, &cc);
                        if (c->is_idle)
                            break;
                        continue;
                    }
                    if (R.save != save) {
                        /* at least one block ran: carry on from where it stopped */
                        save = R.save;
                        cc = R.cc;
                        if (c->is_idle)
                            break;
                        continue;
                    }
                    /* the first block's conditions failed: C runs it */
                }
                b = get_block(f, c, pc);
                uint32_t la1 = c->registers[DSP_REG_LA] + 1;
                if (save - b->body_cycles > 0 &&
                    !((c->registers[DSP_REG_SR] & BWF_LF) && la1 > pc && la1 <= b->last)) {
                    run_block(f, c, b, &save, &cc);
                    if (c->is_idle)
                        break;
                    continue;
                } else {
                    const FOp *op = lookup(f, c, pc);
                    if (op->fn)
                        op->fn(c, op);          /* a P write: the hook */
                    else
                        dsp56k_execute_instruction(c);
                }
            } else if (pc < BWF_PSIZE && c->loop_rep && !c->pc_on_rep &&
                       !(c->interrupt_state | c->interrupt_counter) &&
                       lookup(f, c, pc)->body && lookup(f, c, pc)->inblock) {
                run_rep(f, c, lookup(f, c, pc), &save, &cc);
                if (c->is_idle)
                    break;
                continue;
            } else {
                dsp56k_execute_instruction(c);
            }
            ic = c->instr_cycle;
            save -= (int)ic;
            cc += ic;
            if (c->is_idle)
                break;
        }
        if (c->is_idle || cc >= max_cycles)
            break;
    }
    dsp->dma.mem_opaque = f->dma_opaque;
    dsp->dma.mem_read = f->dma_read;
    dsp->dma.mem_write = f->dma_write;
    c->write_peripheral = f->wp_orig;
    s_pw_gen = NULL;
    dsp->save_cycles = save;
    c->cycle_count = cc;
    return cc;
}

void bwf_take_stats(BwFast *f, uint64_t *fast, uint64_t *slow, uint64_t *decodes)
{
    *fast = f->n_fast;
    *slow = f->n_slow;
    *decodes = f->n_decode;
    f->n_fast = f->n_slow = f->n_decode = 0;
}

void bwf_set_profile(BwFast *f, int on)
{
    f->prof = on >= 2;
    f->stats = on >= 1;
}

/* The top entries of the profile: form and mnemonic (xemu's names), counts. */
void bwf_profile_report(BwFast *f, char *buf, size_t cap, int top)
{
    size_t n = 0;
    int t;
    if (cap)
        buf[0] = 0;
    for (t = 0; t < top; t++) {
        uint32_t best = 0, *bp = NULL;
        const char *a = "", *b = "";
        int k, i;
        for (k = 0; k < PMK_COUNT; k++)
            for (i = 0; i < 256; i++)
                if (f->prof_pm[k][i] > best) {
                    best = f->prof_pm[k][i];
                    bp = &f->prof_pm[k][i];
                    a = s_pmk_name[k];
                    b = disasm_opcodes_alu[i];
                }
        for (i = 0; i <= (int)ARRAY_SIZE(nonparallel_opcodes); i++)
            if (f->prof_np[i] > best) {
                best = f->prof_np[i];
                bp = &f->prof_np[i];
                a = "np";
                b = i < (int)ARRAY_SIZE(nonparallel_opcodes) ? nonparallel_opcodes[i].name : "?";
            }
        if (!bp || n + 1 >= cap)
            break;
        n += (size_t)_snprintf(buf + n, cap - n - 1, "%s%s %s:%u", n ? " | " : "", a, b, best);
        *bp = 0;
    }
    memset(f->prof_pm, 0, sizeof f->prof_pm);
    memset(f->prof_np, 0, sizeof f->prof_np);
    if (cap)
        buf[cap - 1] = 0;
}

void bwf_kind_report(BwFast *f, char *buf, size_t cap)
{
    size_t n = 0;
    int k;
    if (cap)
        buf[0] = 0;
    for (k = 0; k < K_COUNT && n + 1 < cap; k++) {
        if (!f->kind_n[k])
            continue;
        n += (size_t)_snprintf(buf + n, cap - n - 1, "%s%s:%llu", n ? " " : "", s_kind_name[k],
                               (unsigned long long)f->kind_n[k]);
        f->kind_n[k] = 0;
    }
    if (cap)
        buf[cap - 1] = 0;
}

/* ---- as an engine (bw_dsp_engine.h), for the offline replayer ------------ */

typedef struct {
    BwDsp *b;
    BwFast *f;
} RecompEngine;

static void *recomp_attach(BwDsp *b)
{
    RecompEngine *e = (RecompEngine *)calloc(1, sizeof *e);
    if (!e)
        return NULL;
    e->b = b;
    e->f = bwf_new(bw_dsp_xemu(b)->is_gp);
    if (!e->f) {
        free(e);
        return NULL;
    }
    return e;
}

static void recomp_detach(void *h)
{
    RecompEngine *e = (RecompEngine *)h;
    if (e) {
        bwf_free(e->f);
        free(e);
    }
}

static uint32_t recomp_run_frame(void *h, uint32_t max_cycles)
{
    RecompEngine *e = (RecompEngine *)h;
    return bwf_run_frame(e->f, bw_dsp_xemu(e->b), max_cycles);
}

/* The state was replaced or poked from outside: P may have changed. */
static void recomp_state_in(void *h)
{
    bwf_p_changed(((RecompEngine *)h)->f);
}

const BwDspEngine bw_dsp_engine_recomp = {
    "recomp", recomp_attach, recomp_detach, recomp_state_in, NULL, recomp_run_frame
};

/* Counters for the bench (dsp_bench): blocks run, instructions in them, block
 * builds, DO loop ends. */
/* The bench's profiler: which compiled block a code address is in (tag: the
 * block's start, kind << 16: 1 chain fragment, 2 straight, 3 loop). */
int bwf_jit_where(uint64_t rip, uint32_t *tag, uint32_t *off, const uint8_t **code, uint32_t *len)
{
    Jit *j = s_last_jit;
    uint32_t i;
    if (!j || rip < (uint64_t)(uintptr_t)j->base || rip >= (uint64_t)(uintptr_t)j->base + JIT_BYTES)
        return 0;
    for (i = 0; i < j->nfn; i++) {
        uint64_t b0 = (uint64_t)(uintptr_t)j->base + j->rtf[i].BeginAddress;
        uint64_t b1 = (uint64_t)(uintptr_t)j->base + j->rtf[i].EndAddress;
        if (rip >= b0 && rip < b1) {
            *tag = i < j->base_nfn ? 0 : j->tag[i];
            if (((*tag >> 16) & 3) == 1) {
                /* a fragment: 4 entry + load, 5 bodies, 6 store, 7 handler and on, 8 loop path */
                uint32_t o = (uint32_t)(rip - b0), *sc = j->sec[i], k2;
                k2 = (sc[3] && o >= sc[3]) ? 8 : o < sc[4] ? 4 : o < sc[0] ? 9 : o < sc[1] ? 5 : o < sc[2] ? 6 : 7;
                *tag = (*tag & 0xFFFF) | (k2 << 16);
            }
            *off = (uint32_t)(rip - b0);
            *code = (const uint8_t *)(uintptr_t)b0;
            *len = (uint32_t)(b1 - b0);
            return 1;
        }
    }
    return 0;
}

void bwf_block_stats(BwFast *f, uint64_t out[4])
{
    out[0] = f->n_blocks;
    out[1] = f->n_block_ops;
    out[2] = f->n_chain_calls;
    out[3] = f->n_chain_frags;
    out[0] = f->n_chain_runs;
    out[1] = f->n_jit;
    f->n_blocks = f->n_block_ops = f->n_builds = 0;
    f->n_chain_calls = f->n_chain_runs = f->n_chain_frags = 0;
    s_n_loop_end = 0;
    if (s_frag_counter) {
        if (f->jit) {
            /* the fragments' bytes by section: entry+load, bodies, store, tail and cold, loop path */
            uint64_t z[6] = { 0 };
            uint32_t q;
            for (q = f->jit->base_nfn; q < f->jit->nfn; q++) {
                const uint32_t *sc = f->jit->sec[q];
                uint32_t len = f->jit->rtf[q].EndAddress - f->jit->rtf[q].BeginAddress;
                if (((f->jit->tag[q] >> 16) & 3) != 1) {
                    z[5] += len;
                    continue;
                }
                z[0] += sc[0];
                z[1] += sc[1] - sc[0];
                z[2] += sc[2] - sc[1];
                z[3] += (sc[3] ? sc[3] : len) - sc[2];
                z[4] += sc[3] ? len - sc[3] : 0;
            }
            fprintf(stderr, "  fragment bytes: entry %llu, bodies %llu, store %llu, tail+cold %llu, loop path %llu; other functions %llu\n",
                    (unsigned long long)z[0], (unsigned long long)z[1], (unsigned long long)z[2],
                    (unsigned long long)z[3], (unsigned long long)z[4], (unsigned long long)z[5]);
        }
        fprintf(stderr, "  cycles in C bodies (straight blocks): %llu; P generation %u; JIT %u bytes in %u functions\n",
                (unsigned long long)s_cbody_cyc, f->gen, f->jit ? f->jit->used : 0, f->jit ? f->jit->nfn : 0);
        s_cbody_cyc = 0;
    }
    if (s_frag_counter) {
        /* the top chain exits: pc, no fragment / conditions failed / chunk / stop */
        uint32_t k, pc, best;
        for (k = 0; k < 30; k++) {
            uint64_t bn = 0;
            best = 0;
            for (pc = 0; pc < 4096; pc++) {
                uint64_t t = (uint64_t)s_exit_hist[pc][0] + s_exit_hist[pc][1] + s_exit_hist[pc][2] +
                             s_exit_hist[pc][3];
                if (t > bn) {
                    bn = t;
                    best = pc;
                }
            }
            if (!bn)
                break;
            {
                const Block *b = &f->blk[best];
                fprintf(stderr, "  exit pc %04X: nofrag %u fail %u chunk %u stop %u; block n %u loop_ok %d tried %d/%d jit %d/%d w0 %06X\n",
                        best, s_exit_hist[best][0], s_exit_hist[best][1], s_exit_hist[best][2],
                        s_exit_hist[best][3], b->n, b->loop_ok, b->tried_s, b->tried_l,
                        b->jit_s != NULL, b->jit_l != NULL,
                        b->n ? f->pool[b->pool].w0 : 0);
                fprintf(stderr, "      fail why %02X (1 gen 2 wide 4 scale 8 M 16 LA 32 budget)\n", s_fail_why[best]);
                if (s_fail_why[best]) {
                    uint32_t q;
                    fprintf(stderr, "      scale %u M %u sr %06X M:", s_fail_n[best][0], s_fail_n[best][1], s_fail_sr[best]);
                    for (q = 0; q < 8; q++)
                        fprintf(stderr, " %04X", s_fail_m[best][q]);
                    fprintf(stderr, " ops:");
                    for (q = 0; q < b->n; q++)
                        fprintf(stderr, " %06X/%d", f->pool[b->pool + q].w0, f->pool[b->pool + q].kind);
                    fputc(10, stderr);
                }
            }
            memset(s_exit_hist[best], 0, sizeof s_exit_hist[best]);
        }
        memset(s_exit_hist, 0, sizeof s_exit_hist);
        memset(s_fail_why, 0, sizeof s_fail_why);
    }
}

/* ---- self-test: every specialised handler against the interpreter's step ----
 *
 * Random instruction words, decoded; those that get a handler of this file's
 * own (not the interpreter's routine) run from a random state, beside the
 * interpreter's own step from the same state, and the whole core is compared
 * (registers, stack, X/Y/P/mix memories, pc, the transient fields, the
 * counters). The states keep what every writer of the registers keeps, and
 * the address registers' modes are drawn to cover linear, modulo and
 * bit-reversed addressing. Peripherals answer 0 and ignore writes in both. */

static uint32_t fz_periph_rd(dsp_core_t *c, uint32_t a)
{
    (void)c;
    (void)a;
    return 0;
}

static void fz_periph_wr(dsp_core_t *c, uint32_t a, uint32_t v)
{
    (void)c;
    (void)a;
    (void)v;
}

static void fz_state(dsp_core_t *c)
{
    int i;
    s_fz_wide = 1;
    random_regs(c);
    s_fz_wide = 0;
    for (i = 0; i < 8; i++) {
        uint32_t k = rnd32() & 15;
        c->registers[DSP_REG_M0 + i] = k < 8 ? 0xFFFF : k < 12 ? (rnd32() & 0xFF) + 1
                                       : k < 13 ? 0 : rnd32() & 0xFFFF;
        /* addresses kept below 0x3000, so even a word the interpreter writes past
         * xram or yram stays inside the core (both engines write it alike) */
        c->registers[DSP_REG_R0 + i] = (rnd32() & 3) ? rnd32() & 0x7FF : rnd32() % 0x17E0;
        c->registers[DSP_REG_N0 + i] = (rnd32() & 1) ? rnd32() & 0xF : (0x10000 - (rnd32() & 0xF)) & 0xFFFF;
    }
    c->registers[DSP_REG_SR] = rnd32() & 0xaf7f & ~(1u << DSP_SR_LF);   /* LF: half the time */
    if (rnd32() & 1)
        c->registers[DSP_REG_SR] |= 1u << DSP_SR_LF;
    c->registers[DSP_REG_LA] = rnd32() & 0xFFFF;
    c->registers[DSP_REG_LC] = rnd32() & 0xFFFF;
    c->registers[DSP_REG_SP] = (rnd32() % 14) + 1;
    for (i = 0; i < 16; i++) {
        c->stack[0][i] = rnd32() & 0xFFFF;
        c->stack[1][i] = rnd32() & 0xFFFF;
    }
    c->registers[DSP_REG_SSH] = c->stack[0][c->registers[DSP_REG_SP] & 15];
    c->registers[DSP_REG_SSL] = c->stack[1][c->registers[DSP_REG_SP] & 15];
    for (i = 0; i < DSP_XRAM_SIZE; i++)
        c->xram[i] = rnd32() & 0xFFFFFF;
    for (i = 0; i < DSP_YRAM_SIZE; i++)
        c->yram[i] = rnd32() & 0xFFFFFF;
    for (i = 0; i < DSP_MIXBUFFER_SIZE; i++)
        c->mixbuffer[i] = rnd32() & 0xFFFFFF;
    for (i = 0; i < DSP_PRAM_SIZE; i++)
        c->pram[i] = rnd32() & 0xFFFFFF;
    memset(c->periph, 0, sizeof c->periph);
    memset((void *)c->pram_opcache, 0, sizeof c->pram_opcache);
    c->pc = rnd32() & 0x7FF;
    c->loop_rep = c->pc_on_rep = 0;
    c->interrupt_state = DSP_INTERRUPT_NONE;
    c->interrupt_counter = 0;
    c->interrupt_pipeline_count = 0;
    for (i = 0; i < 4; i++) {
        c->interrupt_ipl[i] = 3;
        c->interrupt_is_pending[i] = 0;
    }
    c->is_idle = false;
    c->cycle_count = 0;
    c->num_inst = rnd32();
    c->read_peripheral = fz_periph_rd;
    c->write_peripheral = fz_periph_wr;
}

/* A word for one of the forms this file handles itself. */
static uint32_t fz_word(void)
{
    static const OpcodeEntry *ents[32];
    static int nent = -1;
    if (nent < 0) {
        static const emu_func_t want[] = {
            emu_move_x_imm, emu_move_y_imm, emu_move_x_long, emu_bcc_long, emu_bra_long,
            emu_bcc_imm, emu_bra_imm, emu_brclr_reg, emu_brset_reg, emu_nop, emu_rts,
            emu_bsr_long, emu_bsr_imm, emu_add_imm, emu_add_long, emu_sub_imm, emu_sub_long,
            emu_cmp_imm, emu_cmp_long, emu_and_imm, emu_and_long, emu_or_long, emu_asl_imm,
            emu_asr_imm, emu_rep_imm, emu_rep_reg, emu_dor_imm, emu_do_imm,
        };
        int i, k;
        nent = 0;
        for (i = 0; i < (int)ARRAY_SIZE(nonparallel_opcodes); i++)
            for (k = 0; k < (int)ARRAY_SIZE(want); k++)
                if (nonparallel_opcodes[i].emu_func == want[k] && nent < 32)
                    ents[nent++] = &nonparallel_opcodes[i];
    }
    if ((rnd32() & 3) || !nent)
        return 0x100000 | (rnd32() & 0xFFFFF) | ((rnd32() & 0xF) << 20);   /* parallel */
    {
        const OpcodeEntry *e = ents[rnd32() % nent];
        const char *t = e->template;
        uint32_t w = 0;
        int j;
        for (j = 0; j < 24; j++) {
            w <<= 1;
            w |= t[j] == '1' ? 1 : t[j] == '0' ? 0 : rnd32() & 1;
        }
        return w;
    }
}

#define FZ_CMP(field) if (a->field != b->field) { what = #field; goto diff; }

/* Returns how many cases differed (msg: the first few). */
int bwf_selftest_ops(uint64_t seed, int iters, char *msg, size_t cap, int *tested)
{
    dsp_core_t *a = (dsp_core_t *)calloc(1, sizeof(dsp_core_t));
    dsp_core_t *b = (dsp_core_t *)calloc(1, sizeof(dsp_core_t));
    BwFast *f = bwf_new(1);
    int it, bad = 0, n_tested = 0;
    size_t n = 0;
    if (cap)
        msg[0] = 0;
    if (!a || !b || !f) {
        free(a);
        free(b);
        bwf_free(f);
        return -1;
    }
    s_rng = seed | 1;
    for (it = 0; it < iters; it++) {
        uint32_t pc, w;
        FOp op;
        const char *what = NULL;
        int i;
        fz_state(a);
        pc = a->pc;
        w = fz_word();
        a->pram[pc] = w;
        if (rnd32() & 3)
            a->pram[pc + 1] = rnd32() & 0x1FFF;     /* an address or displacement in range */
        memset(&op, 0, sizeof op);
        decode_into(f, a, pc, &op);
        if (!op.fn || op.fn == h_light || op.fn == h_generic)
            continue;
        if (op.body == b_move_disp) {
            uint32_t ea = (a->registers[op.r2] + op.imm) & BITMASK(24);
            if (ea >= 0x17E0)
                continue;                       /* past the core, or a peripheral */
        } else if (op.deps == 2 && op.imm >= 0x17E0 && op.fn != h_branch && op.fn != h_brbit_reg &&
                   op.fn != h_bsr) {
            continue;                           /* an absolute address past the core */
        }
        n_tested++;
        memcpy(b, a, sizeof *a);
        dsp56k_execute_instruction(a);
        op.fn(b, &op);
        FZ_CMP(pc);
        FZ_CMP(cur_inst);
        FZ_CMP(cur_inst_len);
        FZ_CMP(instr_cycle);
        FZ_CMP(num_inst);
        FZ_CMP(loop_rep);
        FZ_CMP(pc_on_rep);
        FZ_CMP(interrupt_state);
        FZ_CMP(interrupt_counter);
        FZ_CMP(is_idle);
        for (i = 0; i < DSP_REG_MAX; i++)
            if (a->registers[i] != b->registers[i]) {
                what = registers_name[i];
                goto diff;
            }
        if (memcmp(a->stack, b->stack, sizeof a->stack)) { what = "stack"; goto diff; }
        if (memcmp(a->xram, b->xram, sizeof a->xram)) { what = "X"; goto diff; }
        if (memcmp(a->yram, b->yram, sizeof a->yram)) { what = "Y"; goto diff; }
        if (memcmp(a->pram, b->pram, sizeof a->pram)) { what = "P"; goto diff; }
        if (memcmp(a->mixbuffer, b->mixbuffer, sizeof a->mixbuffer)) { what = "mix"; goto diff; }
        continue;
    diff:
        if (bad < 6 && n + 1 < cap)
            n += (size_t)_snprintf(msg + n, cap - n - 1, "%sword 0x%06X (%s): %s differs",
                                   n ? "; " : "", w, s_kind_name[op.kind], what);
        bad++;
    }
    if (cap)
        msg[cap - 1] = 0;
    if (tested)
        *tested = n_tested;
    free(a);
    free(b);
    bwf_free(f);
    return bad;
}

/* ---- self-test: the JIT against the C bodies --------------------------------
 *
 * Random blocks of 1-6 bodies (real instruction words), from random states in
 * the JIT's own preconditions (scaling mode 00, linear addressing); half run
 * as straight blocks, half as DO loops of 1-4 iterations with a random cycle
 * budget. The native code and a C reference (run_loop's own steps) run from
 * the same state; return codes, counters and the whole core are compared. */

/* bail_iter / bail_i: the JIT bailed out after body bail_i of iteration
 * bail_iter (a max left B2 wide): the reference stops there too. */
static int jit_ref(dsp_core_t *c, const FOp *ops, uint32_t n, int loop, JitLoop *L, int bail_iter,
                   int bail_i)
{
    uint32_t i;
    int iter = 0;
    if (!loop) {
        for (i = 0; i + 1 < n; i++)
            if (ops[i].body(c, &ops[i]) || (int)i == bail_i)
                return JR_STOP | (int)i;
        return JR_DONE;
    }
    for (;; iter++) {
        uint32_t lc;
        for (i = 0; i < n; i++)
            if (ops[i].body(c, &ops[i]) || (iter == bail_iter && (int)i == bail_i))
                return JR_STOP | (int)i;
        c->num_inst += (uint32_t)L->total;
        L->save -= L->total;
        L->cc += (uint32_t)L->total;
        lc = (c->registers[DSP_REG_LC] - 1) & BITMASK(16);
        c->registers[DSP_REG_LC] = lc;
        if (lc == 0)
            return JR_LOOP_END;
        if (L->save - L->body_cycles <= 0)
            return JR_LOOP_BUDGET;
    }
}

int bwf_selftest_jit(uint64_t seed, int iters, char *msg, size_t cap, int *tested)
{
    /* each core with 1 MB of zeroes after it: a word loaded into an address
     * register can send a read far past the core, as in the interpreter; both
     * copies then read the same zeroes */
#define FZ_PAD (1u << 20)
    dsp_core_t *a = (dsp_core_t *)calloc(1, sizeof(dsp_core_t) + FZ_PAD);
    dsp_core_t *b = (dsp_core_t *)calloc(1, sizeof(dsp_core_t) + FZ_PAD);
    BwFast *f = bwf_new(1);
    int it, bad = 0, n_tested = 0;
    size_t nm = 0;
    if (cap)
        msg[0] = 0;
    if (!a || !b || !f || !f->jit) {
        free(a);
        free(b);
        bwf_free(f);
        return -1;
    }
    s_rng = seed | 1;
    for (it = 0; it < iters; it++) {
        FOp ops[8];
        uint32_t n = 1 + rnd32() % 6, pc, i, k, rmask = 0;
        int loop = rnd32() & 1, ra, rb, r;
        {
            /* test knobs: BW_JITFZ_N (fixed length), BW_JITFZ_LOOP (0 or 1) */
            const char *en = getenv("BW_JITFZ_N"), *el = getenv("BW_JITFZ_LOOP");
            if (en && *en)
                n = (uint32_t)atoi(en);
            if (el && *el)
                loop = atoi(el);
        }
        JitLoop La, Lb;
        jit_fn fn;
        const char *what = NULL;
        fz_state(a);
        a->registers[DSP_REG_SR] &= ~((1u << DSP_SR_S0) | (1u << DSP_SR_S1));
        if (rnd32() & 1)
            a->registers[DSP_REG_SR] |= 1u << DSP_SR_S1;           /* scaling up */
        for (k = 0; k < 8; k++)         /* above the displacements' reach below 0 */
            a->registers[DSP_REG_R0 + k] = 0x100 + a->registers[DSP_REG_R0 + k] % 0x16E0;
        for (k = 0; k < 8; k++)
            a->registers[DSP_REG_M0 + k] = (rnd32() & 3) ? 0xFFFF : 0;  /* linear or bit-reversed */
        pc = 0x100 + (rnd32() & 0x3FF);
        a->pc = pc;
        for (i = 0; i < n; i++) {
            int tries;
            for (tries = 0; tries < 200; tries++) {
                a->pram[pc + i] = fz_word();
                a->pram[pc + i + 1] = rnd32() & 0xFFF;     /* small: no 24-bit wrap */
                memset(&ops[i], 0, sizeof ops[i]);
                decode_into(f, a, pc + i, &ops[i]);
                if (ops[i].body && ops[i].inblock && ops[i].len == 1) {
                    /* no word from memory into an address or offset register (it
                     * would send later accesses past any memory): only #xx */
                    const FOp *o = &ops[i];
                    uint32_t dst = 0;
                    if (o->body == b_pm2reg)
                        dst = o->r2;
                    else if (o->body == b_move_disp && (o->cc & 1))
                        dst = o->r1;
                    else if (o->w0 >= 0x100000 && pm_kind(o->w0) == PMK_5 && ((o->w0 >> 15) & 1))
                        dst = o->r1;
                    if (!(dst >= DSP_REG_R0 && dst <= DSP_REG_N7))
                        break;
                }
            }
            if (tries == 200)
                break;
        }
        if (i < n)
            continue;
        for (i = 0; i < n; i++)
            if (ops[i].body == b_move_disp &&
                ((a->registers[ops[i].r2] + ops[i].imm) & BITMASK(24)) >= 0x17E0)
                break;
        if (i < n)
            continue;
        if (!loop) {
            ops[n] = ops[n - 1];            /* the last one isn't run by a straight block */
            n++;
        }
        if (f->jit->used > JIT_COLD_START / 2)
            jit_reset(f->jit);
        {
            uint32_t mode = 0;
            fn = jit_compile(f->jit, ops, n, loop, &rmask, a, &mode);
            if (fn && !jit_entry_ok(a, rmask, mode))
                fn = NULL;
        }
        if (!fn)
            continue;
        memset(&La, 0, sizeof La);
        La.total = La.body_cycles = 0;
        for (i = 0; i < n; i++)
            La.total += ops[i].cyc;
        La.body_cycles = La.total - ops[n - 1].cyc;
        La.save = 1 + (int)(rnd32() % (uint32_t)(La.total * 5 + 1));
        La.cc = rnd32();
        if (loop) {
            a->registers[DSP_REG_LC] = 1 + rnd32() % 4;
            if (La.save - La.body_cycles <= 0)
                La.save = La.body_cycles + 1;
        }
        Lb = La;
        memset((uint8_t *)a + sizeof *a, 0, FZ_PAD);
        memcpy(b, a, sizeof *a + FZ_PAD);
        n_tested++;
        {
            static uintptr_t fault_at;
            __try {
                rb = fn(b, loop ? &Lb : NULL);
            } __except (fault_at = (uintptr_t)(GetExceptionInformation())->ExceptionRecord->ExceptionAddress,
                        EXCEPTION_EXECUTE_HANDLER) {
                char words[120];
                size_t wn = 0;
                for (i = 0; i < n && wn + 10 < sizeof words; i++)
                    wn += (size_t)_snprintf(words + wn, sizeof words - wn - 1, " %06X", ops[i].w0);
                words[sizeof words - 1] = 0;
                {
                    int ref_faults = 0;
                    __try {
                        (void)jit_ref(a, ops, n, loop, &La, -1, -1);
                    } __except (EXCEPTION_EXECUTE_HANDLER) {
                        ref_faults = 1;
                    }
                    fprintf(stderr, "JIT fault at %p (code %p..%p; exe offset %llX) loop %d, the"
                                    " reference %s:%s\n", (void *)fault_at, (void *)fn,
                            (void *)(f->jit->base + f->jit->used),
                            (unsigned long long)(fault_at - (uintptr_t)GetModuleHandleA(NULL)), loop,
                            ref_faults ? "faults too" : "DOES NOT fault", words);
                }
                continue;
            }
        }
        {
            /* where the JIT stopped, for a bail (the reference has no bail of its own) */
            int bi = (rb & JR_STOP) == JR_STOP && rb < JR_LOOP_END ? (rb & 0xFFF) : -1;
            int biter = loop && La.total ? (La.save - Lb.save) / La.total : 0;
            __try {
                ra = jit_ref(a, ops, n, loop, &La, biter, bi);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                fprintf(stderr, "reference fault (skipped)\n");
                continue;
            }
        }
        r = 0;
        if (ra != rb) { what = "return code"; r = 1; }
        else if (loop && (La.save != Lb.save || La.cc != Lb.cc)) { what = "save/cc"; r = 1; }
        else {
            for (k = 0; k < DSP_REG_MAX; k++)
                if (a->registers[k] != b->registers[k]) {
                    static char rbuf[64];
                    _snprintf(rbuf, sizeof rbuf - 1, "%s ref 0x%X jit 0x%X", registers_name[k],
                              a->registers[k], b->registers[k]);
                    what = rbuf;
                    r = 1;
                    break;
                }
            if (!r && a->num_inst != b->num_inst) { what = "num_inst"; r = 1; }
            {
                static char wbuf[96];
                const uint32_t *ma = NULL, *mb = NULL;
                uint32_t nwords = 0, q;
                const char *nm2 = NULL;
                if (!r && memcmp(a->xram, b->xram, sizeof a->xram)) { ma = a->xram; mb = b->xram; nwords = DSP_XRAM_SIZE; nm2 = "X"; }
                else if (!r && memcmp(a->yram, b->yram, sizeof a->yram)) { ma = a->yram; mb = b->yram; nwords = DSP_YRAM_SIZE; nm2 = "Y"; }
                else if (!r && memcmp(a->mixbuffer, b->mixbuffer, sizeof a->mixbuffer)) { ma = a->mixbuffer; mb = b->mixbuffer; nwords = DSP_MIXBUFFER_SIZE; nm2 = "mix"; }
                if (ma) {
                    for (q = 0; q < nwords && ma[q] == mb[q]; q++)
                        ;
                    _snprintf(wbuf, sizeof wbuf - 1, "%s[0x%X] ref 0x%X jit 0x%X", nm2, q, ma[q], mb[q]);
                    what = wbuf;
                    r = 1;
                }
            }
            if (!r && memcmp(a->stack, b->stack, sizeof a->stack)) { what = "stack"; r = 1; }
        }
        if (r) {
            if (bad < 6 && nm + 1 < cap) {
                char words[120];
                size_t wn = 0;
                for (i = 0; i < n && wn + 10 < sizeof words; i++)
                    wn += (size_t)_snprintf(words + wn, sizeof words - wn - 1, " %06X", ops[i].w0);
                words[sizeof words - 1] = 0;
                nm += (size_t)_snprintf(msg + nm, cap - nm - 1, "%s%s, %s:%s (ref %X jit %X)",
                                        nm ? "; " : "", loop ? "loop" : "straight", what, words,
                                        ra, rb);
            }
            bad++;
        }
    }
    if (cap)
        msg[cap - 1] = 0;
    if (tested)
        *tested = n_tested;
    free(a);
    free(b);
    bwf_free(f);
    return bad;
}
