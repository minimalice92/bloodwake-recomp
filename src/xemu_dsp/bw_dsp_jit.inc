/* SPDX-License-Identifier: GPL-2.0-or-later (adapted from xemu's DSP; see NOTICE.md) */
/*
 * bw_dsp_jit.inc - the block JIT (x64), included by bw_dsp_fast.c.
 *
 * A block that runs often becomes native code: the moves, the address
 * updates and the common ALU operations inline, with the DSP registers the
 * block uses held in host registers from its start to its exits, and a DO
 * loop whose body is the block compiled as a native loop. Everything else is
 * the C of bw_dsp_fast.c, called from the native code. The semantics are
 * those of the C bodies (themselves xemu's), and the JIT's self-test checks
 * the native code against them on random blocks and states.
 *
 * Guardrails: the code region is never writable and executable at once
 * (written read-write, then made read-execute and the instruction cache
 * flushed); unwind information is registered for the whole region, so an
 * exception inside native code reaches the bridge's guard
 * (BW_DSP_JIT_FAULT=n plants one in the n-th compiled block, to prove it);
 * the region is bounded and dropped with the blocks when full;
 * BW_DSP_JIT=0 runs the decode cache alone.
 *
 * What a compiled block assumes, checked before it runs (jit_entry_ok, or
 * a fragment's own entry): plain running and the block's own conditions (as
 * for any block), the scaling mode it was compiled for (00 or S1's scaling
 * up), each address register it updates in the mode it was compiled for
 * (linear, M = 0xFFFF, or bit-reversed, M = 0), and A2/B2 in the 56-bit form.
 * A body that would change one of those (a move to M, SR, OMR, LA or the
 * stack's registers) isn't compiled.
 *
 * Block linking: each block also becomes a fragment that runs in the
 * trampoline's frame. A fragment checks its block's conditions, runs the
 * bodies, then its last instruction (natively for branches, bit branches and
 * bodies; else the C handler), does the frame loop's accounting, and jumps
 * through a per-address table straight to the next fragment. A DO loop body's
 * fragment also has a native loop path. Anything else returns to C at an
 * instruction boundary.
 *
 * Flags only where read: an ALU result's E, U, N and Z are computed when
 * something reads SR (a move from SR, a condition, C code, a stop, the end
 * of the bodies), not at each operation; C, V and L at each, as xemu.
 */

#include "bw_dsp_x64.inc"

#define JIT_BYTES (2u << 20)            /* per engine */
#define JIT_COLD_START (JIT_BYTES - (512u << 10))   /* the last 512 KB: fragments' cold code */
#define JIT_MAX_FUNCS 16384
#define JIT_AFTER_RUNS 4                /* compile a block after this many runs */
#define JIT_FRAME 88                    /* sub rsp: 32 shadow + 7 slots, aligned */
#define SLOT(k) (32 + 8 * (k))          /* the frame's slots: 0-3 moves, 4 ctx, 5 flags, 6 bitrev */

typedef int (*jit_fn)(dsp_core_t *c, void *ctx);

typedef struct Jit {
    uint8_t *base;                      /* the region; its first 64 bytes: UNWIND_INFO */
    uint32_t used;
    RUNTIME_FUNCTION rtf[JIT_MAX_FUNCS];
    uint32_t nfn;
    int registered;
    long fault_after;                   /* BW_DSP_JIT_FAULT (a test aid) */
    jit_fn tramp;                       /* block linking: the trampoline */
    uint8_t *exit_dispatch, *epilogue;  /* its shared exits */
    uint32_t base_used, base_nfn;       /* what jit_reset keeps (the trampoline) */
    uint32_t tag[JIT_MAX_FUNCS];        /* each function's block (a bench aid) */
    uint32_t sec[JIT_MAX_FUNCS][5];
    uint8_t *flush[2][2];               /* the flag flush, shared: [SR in memory][scaling up] */
    uint32_t cold_used;                 /* the cold area's end (from JIT_COLD_START) */
    RUNTIME_FUNCTION rtf_cold;          /* the whole cold area: the fragments' frame */
    long fault_cold_after;              /* BW_DSP_JIT_FAULT_COLD (a test aid) */     /* a fragment's sections: bodies, store, handler, loop (bench) */
} Jit;

static Jit *s_last_jit;                 /* the bench's profiler: the last JIT made */

static int s_jit_env = -1;

static int jit_wanted(void)
{
    if (s_jit_env < 0) {
        const char *e = getenv("BW_DSP_JIT");
        s_jit_env = !(e && e[0] == '0');
    }
    return s_jit_env;
}

/* ---- the region ------------------------------------------------------------ */

static PRUNTIME_FUNCTION CALLBACK jit_lookup(DWORD64 pc, PVOID ctx)
{
    Jit *j = (Jit *)ctx;
    DWORD off = (DWORD)(pc - (DWORD64)j->base);
    uint32_t lo = 0, hi = j->nfn;
    if (off >= JIT_COLD_START)
        return off < j->cold_used ? &j->rtf_cold : NULL;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (j->rtf[mid].EndAddress <= off)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < j->nfn && j->rtf[lo].BeginAddress <= off && off < j->rtf[lo].EndAddress)
        return &j->rtf[lo];
    return NULL;
}

/* Within reach of rel32 calls to `target` when possible. */
static uint8_t *jit_alloc(const void *target)
{
    uintptr_t t = (uintptr_t)target & ~(uintptr_t)0xFFFF, step;
    for (step = 0x10000000; step < 0x60000000; step += 0x10000) {
        void *p;
        if (t > step) {
            p = VirtualAlloc((void *)(t - step), JIT_BYTES, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (p)
                return (uint8_t *)p;
        }
        p = VirtualAlloc((void *)(t + step), JIT_BYTES, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (p)
            return (uint8_t *)p;
    }
    return (uint8_t *)VirtualAlloc(NULL, JIT_BYTES, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}

/* The prologue every compiled function has (the unwind info describes it):
 *   push rbx, rbp, rsi, rdi, r12, r13, r14, r15; sub rsp, JIT_FRAME */
static int jit_build_tramp(Jit *jt);
static void jit_free(Jit *j);

static Jit *jit_new(void)
{
    static const uint8_t unwind[] = {
        0x01, 16, 9, 0,                 /* version 1, prologue 16 bytes, 9 codes */
        16, 0xA2,                       /* at 16: UWOP_ALLOC_SMALL, (88 - 8) / 8 = 10 */
        12, 0xF0,                       /* at 12: push r15 */
        10, 0xE0,                       /* at 10: push r14 */
        8, 0xD0,                        /* at 8: push r13 */
        6, 0xC0,                        /* at 6: push r12 */
        4, 0x70,                        /* at 4: push rdi */
        3, 0x60,                        /* at 3: push rsi */
        2, 0x50,                        /* at 2: push rbp */
        1, 0x30,                        /* at 1: push rbx */
        0, 0,                           /* to an even count */
    };
    /* at +32: the same frame from inside its body (block-linking fragments run
     * in the trampoline's frame): no prologue, every code already done */
    static const uint8_t unwind_body[] = {
        0x01, 0, 9, 0,
        0, 0xA2, 0, 0xF0, 0, 0xE0, 0, 0xD0, 0, 0xC0, 0, 0x70, 0, 0x60, 0, 0x50, 0, 0x30,
        0, 0,
    };
    Jit *j = (Jit *)calloc(1, sizeof(Jit));
    DWORD old;
    if (!j)
        return NULL;
    j->base = jit_alloc((const void *)&h_body);
    if (!j->base) {
        free(j);
        return NULL;
    }
    memcpy(j->base, unwind, sizeof unwind);
    memcpy(j->base + 32, unwind_body, sizeof unwind_body);
    j->used = 64;
    VirtualProtect(j->base, JIT_BYTES, PAGE_EXECUTE_READ, &old);
    j->registered = RtlInstallFunctionTableCallback((DWORD64)j->base | 3, (DWORD64)j->base,
                                                    JIT_BYTES, jit_lookup, j, NULL);
    if (!j->registered) {
        /* no unwind information: no JIT (an exception could not be caught) */
        VirtualFree(j->base, 0, MEM_RELEASE);
        free(j);
        return NULL;
    }
    {
        const char *e = getenv("BW_DSP_JIT_FAULT");
        const char *ec = getenv("BW_DSP_JIT_FAULT_COLD");
        j->fault_after = e && *e ? atol(e) : 0;
        j->fault_cold_after = ec && *ec ? atol(ec) : 0;
    }
    j->cold_used = JIT_COLD_START;
    j->rtf_cold.BeginAddress = JIT_COLD_START;
    j->rtf_cold.EndAddress = JIT_COLD_START;
    j->rtf_cold.UnwindData = 32;
    if (!jit_build_tramp(j)) {
        jit_free(j);
        return NULL;
    }
    return j;
}

static void jit_free(Jit *j)
{
    if (!j)
        return;
    if (j->registered)
        RtlDeleteFunctionTable((PRUNTIME_FUNCTION)((DWORD64)j->base | 3));
    VirtualFree(j->base, 0, MEM_RELEASE);
    free(j);
}

static void jit_reset(Jit *j)
{
    if (j) {
        j->used = j->base_used;
        j->nfn = j->base_nfn;
        j->cold_used = JIT_COLD_START;
        j->rtf_cold.EndAddress = JIT_COLD_START;
    }
}

/* BW_DSP_JIT_FAULT=n: the n-th compiled block calls this first; its first
 * call faults, inside native code's callee, to prove the guard catches an
 * exception that unwinds through JIT frames. */
static volatile int s_fault_armed = 1;

static void jit_fault_probe(void)
{
    if (s_fault_armed) {
        s_fault_armed = 0;
        fprintf(stderr, "[DSP-JIT] BW_DSP_JIT_FAULT: faulting inside a JIT-compiled block (a test)\n");
        *(volatile int *)(uintptr_t)8 = 1;
    }
}

/* ---- what the native code calls -------------------------------------------- */

static uint32_t jit_rd_slow(dsp_core_t *c, uint32_t space, uint32_t a)
{
    return dsp56k_read_memory(c, space, a);
}

/* Returns 1 for a peripheral (X:0xFFFF80 up): the block stops after it. */
static int jit_wr_slow(dsp_core_t *c, uint32_t space, uint32_t a, uint32_t v)
{
    dsp56k_write_memory(c, space, a, v);
    return space == DSP_SPACE_X && a >= DSP_PERIPH_BASE;
}

/* ---- the compiler ------------------------------------------------------------ */

/* The DSP registers the JIT can hold in host registers. A and B as 56-bit
 * values (A2:A1:A0), the others as they are stored. */
enum { V_X0, V_X1, V_Y0, V_Y1, V_A, V_B, V_R0, V_SR = V_R0 + 8, V_LC, V_COUNT };

static int v_of_reg(uint32_t r)
{
    switch (r) {
    case DSP_REG_X0: return V_X0;
    case DSP_REG_X1: return V_X1;
    case DSP_REG_Y0: return V_Y0;
    case DSP_REG_Y1: return V_Y1;
    case DSP_REG_SR: return V_SR;
    case DSP_REG_LC: return V_LC;
    }
    if (r >= DSP_REG_R0 && r <= DSP_REG_R7)
        return V_R0 + (int)(r - DSP_REG_R0);
    return -1;
}

static const int s_vreg[V_COUNT] = {   /* registers[] index (A, B: by parts) */
    DSP_REG_X0, DSP_REG_X1, DSP_REG_Y0, DSP_REG_Y1, -1, -1,
    DSP_REG_R0, DSP_REG_R0 + 1, DSP_REG_R0 + 2, DSP_REG_R0 + 3,
    DSP_REG_R0 + 4, DSP_REG_R0 + 5, DSP_REG_R0 + 6, DSP_REG_R0 + 7,
    DSP_REG_SR, DSP_REG_LC,
};

#define POOL_N 7
static const int s_pool[POOL_N] = { X_RSI, X_RDI, X_RBP, X_R12, X_R13, X_R14, X_R15 };

#define OFF_REG(r) ((int32_t)(offsetof(dsp_core_t, registers) + 4 * (r)))
#define OFF_X ((int32_t)offsetof(dsp_core_t, xram))
#define OFF_Y ((int32_t)offsetof(dsp_core_t, yram))
#define OFF_MIX ((int32_t)offsetof(dsp_core_t, mixbuffer))
#define OFF_NUMINST ((int32_t)offsetof(dsp_core_t, num_inst))

/* ALU routines the JIT inlines: family and operands, from the table's
 * layout (the self-test checks each against the C). */
enum { AF_NONE, AF_ADD, AF_SUB, AF_CMP, AF_TFR, AF_TST, AF_CLR, AF_MUL, AF_ABS };
enum { SRC_ACC, SRC_X, SRC_Y, SRC_X0, SRC_Y0, SRC_X1, SRC_Y1 };
typedef struct { uint8_t fam, d, src, minus, kind, r1, r2; } AluDesc;
static AluDesc s_alu[256];

static void alu_desc_init(void)
{
    static const uint8_t pair[8][2] = {
        { V_X0, V_X0 }, { V_Y0, V_Y0 }, { V_X1, V_X0 }, { V_Y1, V_Y0 },
        { V_X0, V_Y1 }, { V_Y0, V_X0 }, { V_X1, V_Y0 }, { V_Y1, V_X1 },
    };
    static const uint8_t src4[4] = { SRC_X0, SRC_Y0, SRC_X1, SRC_Y1 };
    int i;
    memset(s_alu, 0, sizeof s_alu);
    for (i = 0x80; i < 0x100; i++) {
        AluDesc *a = &s_alu[i];
        a->fam = AF_MUL;
        a->r1 = pair[(i >> 4) & 7][0];
        a->r2 = pair[(i >> 4) & 7][1];
        a->d = (i >> 3) & 1;
        a->minus = (i >> 2) & 1;
        a->kind = i & 3;
        if (opcodes_alu[i] == emu_macr_p_x1_y0_b)
            a->kind = 2;                /* xemu's macr +x1,y0,b is a mac */
    }
    for (i = 0x40; i < 0x80; i++) {
        AluDesc *a = &s_alu[i];
        int op = i & 7;
        a->src = src4[(i >> 4) & 3];
        a->d = (i >> 3) & 1;
        a->fam = op == 0 ? AF_ADD : op == 1 ? AF_TFR : op == 4 ? AF_SUB : op == 5 ? AF_CMP : AF_NONE;
    }
    s_alu[0x10] = (AluDesc){ AF_ADD, 0, SRC_ACC };
    s_alu[0x18] = (AluDesc){ AF_ADD, 1, SRC_ACC };
    s_alu[0x14] = (AluDesc){ AF_SUB, 0, SRC_ACC };
    s_alu[0x1c] = (AluDesc){ AF_SUB, 1, SRC_ACC };
    s_alu[0x20] = (AluDesc){ AF_ADD, 0, SRC_X };
    s_alu[0x28] = (AluDesc){ AF_ADD, 1, SRC_X };
    s_alu[0x30] = (AluDesc){ AF_ADD, 0, SRC_Y };
    s_alu[0x38] = (AluDesc){ AF_ADD, 1, SRC_Y };
    s_alu[0x24] = (AluDesc){ AF_SUB, 0, SRC_X };
    s_alu[0x2c] = (AluDesc){ AF_SUB, 1, SRC_X };
    s_alu[0x34] = (AluDesc){ AF_SUB, 0, SRC_Y };
    s_alu[0x3c] = (AluDesc){ AF_SUB, 1, SRC_Y };
    s_alu[0x05] = (AluDesc){ AF_CMP, 0, SRC_ACC };
    s_alu[0x0d] = (AluDesc){ AF_CMP, 1, SRC_ACC };
    s_alu[0x01] = (AluDesc){ AF_TFR, 0, SRC_ACC };
    s_alu[0x09] = (AluDesc){ AF_TFR, 1, SRC_ACC };
    s_alu[0x03] = (AluDesc){ AF_TST, 0 };
    s_alu[0x0b] = (AluDesc){ AF_TST, 1 };
    s_alu[0x13] = (AluDesc){ AF_CLR, 0 };
    s_alu[0x1b] = (AluDesc){ AF_CLR, 1 };
    s_alu[0x26] = (AluDesc){ AF_ABS, 0 };
    s_alu[0x2e] = (AluDesc){ AF_ABS, 1 };
}

/* JIT kinds of a body. */
enum { JK_CALL, JK_PM5, JK_PM2REG, JK_PM3, JK_PM2NOP, JK_PM8, JK_MOVEDISP, JK_NOP,
       JK_ADDX, JK_SUBX, JK_CMPX, JK_LOGICX, JK_SHIFTX };

typedef struct {
    Xe x;
    int8_t loc[V_COUNT];                /* host register, or -1: in memory */
    uint32_t used;                      /* V registers the block touches */
    int no_acc[2];                      /* A or B touched by part: not held */
    uint8_t **stops;                    /* jumps to the stop exits */
    uint32_t *stop_index;
    uint32_t nstops;
    uint8_t **bails;                    /* jumps to the bail exits (no store: memory is current) */
    uint32_t *bail_index;
    uint32_t nbails;
    uint8_t *exit_jumps[4];
    int pend;                           /* SR's E, U, N, Z not yet computed: from SLOT_PR */
    int8_t spare[4];                    /* pool registers free for e_save (or -1) */
    int scale;                          /* the scaling mode compiled for: 0, or 2 (S1, up) */
    uint32_t bitrev;                    /* the address registers compiled as bit-reversed (M = 0) */
    struct ColdRec *cold;               /* memory accesses' slow paths, emitted out of line */
    uint32_t ncold;
    struct Jit *jt;                     /* the region (its shared code) */
    Xe xc;                              /* the fragment's cold code (swapped in by cold_swap) */
} Jc;

/* Emit into the cold area (and back): slow paths and exits, away from the
 * hot code, so a frame's hot code packs into fewer cache lines and pages. */
static void cold_swap(Jc *j)
{
    Xe t = j->x;
    j->x = j->xc;
    j->xc = t;
}

#define STOP_FLUSH 0x8000u            /* a stop exit that computes the pending flags first */

/* A memory access's slow path (the mix buffer, past RAM, a peripheral), kept
 * out of the hot code: emitted after the path's code, jumping back. */
typedef struct ColdRec {
    uint8_t wr, space, pend;
    int8_t dst, ar, vr;
    uint32_t si;
    uint8_t *jmp, *ret;
} ColdRec;
#define COLD_MAX (BLK_MAX * 4 + 16)

/* The mode a block is compiled for, from the core when it is compiled: the
 * scaling mode (00 or S1) in bits 0-1, and for each address register the
 * block updates, bit 8 + r when it is bit-reversed (M = 0) rather than linear
 * (M = 0xFFFF). Other modes (S0, modulo): 0, not compiled. */
static int jit_mode_of(const dsp_core_t *c, uint32_t rmask, uint32_t *mode)
{
    uint32_t scale = (c->registers[DSP_REG_SR] >> DSP_SR_S0) & 3, r, m = 0;
    if (scale == 1 || scale == 3)
        return 0;
    m = scale;
    for (r = 0; r < 8; r++)
        if (rmask & (1u << r)) {
            uint16_t mr = (uint16_t)c->registers[DSP_REG_M0 + r];
            if (mr == 0)
                m |= 1u << (8 + r);
            else if (mr != 0xFFFF)
                return 0;
        }
    *mode = m;
    return 1;
}

/* emu_update_rn_bitreverse's arithmetic on R's and N's values (M = 0: every
 * update, whatever its modifier, is this reversed-carry increment). */
static uint32_t jit_bitrev(uint32_t r_reg, uint32_t nval)
{
    int revbits, i;
    uint32_t value;
    for (revbits = 0; revbits < 16; revbits++)
        if (nval & (1 << revbits))
            break;
    revbits++;
    value = r_reg & (BITMASK(16) - BITMASK(revbits));
    for (i = 0; i < revbits; i++)
        if (r_reg & (1 << i))
            value |= 1 << (revbits - i - 1);
    value++;
    value &= BITMASK(revbits);
    r_reg &= (BITMASK(16) - BITMASK(revbits));
    r_reg |= value;
    value = r_reg & (BITMASK(16) - BITMASK(revbits));
    for (i = 0; i < revbits; i++)
        if (r_reg & (1 << i))
            value |= 1 << (revbits - i - 1);
    return value;
}

static int jk_of(const FOp *op)
{
    /* max a,b can leave B2 wide (xemu writes A0 into it): the whole body in C,
     * then a check that bails out of the block if it did */
    if (op->alu && (op->w0 & 0xFF) == 0x1D)
        return JK_CALL;
    if (op->body == b_nop)
        return JK_NOP;
    if (op->body == b_add_x)
        return JK_ADDX;
    if (op->body == b_sub_x)
        return JK_SUBX;
    if (op->body == b_cmp_x)
        return JK_CMPX;
    if (op->body == b_logic_x)
        return JK_LOGICX;
    if (op->body == b_shift_x)
        return JK_SHIFTX;
    if (op->body == b_pm2nop)
        return JK_PM2NOP;
    if (op->body == b_pm2reg)
        return JK_PM2REG;
    if (op->body == b_pm3)
        return JK_PM3;
    if (op->body == b_pm8)
        return JK_PM8;
    if (op->body == b_move_disp)
        /* the JIT's register write covers A, B and the registers below 0x20 */
        return ((op->cc & 1) && !(op->r1 == DSP_REG_A || op->r1 == DSP_REG_B || op->r1 < 0x20))
               ? JK_CALL : JK_MOVEDISP;
    if (op->w0 >= 0x100000 && pm_kind(op->w0) == PMK_5) {
        uint32_t w0 = op->w0, f = (w0 >> 8) & 63;
        /* an absolute peripheral address: the C does it */
        if ((w0 & (1 << 14)) && (f >> 3) == 6 && op->imm >= DSP_PERIPH_BASE)
            return JK_CALL;
        return JK_PM5;
    }
    return JK_CALL;
}

/* pm5's decoded mode (as decode_pm chose the template) */
static uint32_t pm5_mode(const FOp *op)
{
    uint32_t w0 = op->w0, f = (w0 >> 8) & 63, w = (w0 >> 15) & 1;
    if (!(w0 & (1 << 14)))
        return 8;
    if ((f >> 3) == 6)
        return ((f & 7) && w) ? 9 : 6;
    return f >> 3;
}

/* An ALU routine inlined (else called). */
static int alu_inline(const FOp *op)
{
    return op->alu && s_alu[op->w0 & 0xFF].fam != AF_NONE;
}

/* Note a register a body reads or writes. */
static void use_reg(Jc *j, uint32_t r)
{
    int v = v_of_reg(r);
    if (r == DSP_REG_A || r == DSP_REG_B) {
        j->used |= 1u << (V_A + (r - DSP_REG_A));
        return;
    }
    if (r == DSP_REG_A0 || r == DSP_REG_A1 || r == DSP_REG_A2)
        j->no_acc[0] = 1;
    if (r == DSP_REG_B0 || r == DSP_REG_B1 || r == DSP_REG_B2)
        j->no_acc[1] = 1;
    if (v >= 0)
        j->used |= 1u << v;
}

static void use_alu(Jc *j, const FOp *op)
{
    const AluDesc *a;
    if (!op->alu)
        return;
    j->used |= 1u << V_SR;
    if (!alu_inline(op)) {
        /* called: it may touch A, B, X, Y and SR in memory */
        j->used |= (1u << V_A) | (1u << V_B) | (1u << V_X0) | (1u << V_X1) | (1u << V_Y0) |
                   (1u << V_Y1);
        return;
    }
    a = &s_alu[op->w0 & 0xFF];
    j->used |= 1u << (V_A + a->d);
    if (a->fam == AF_MUL) {
        j->used |= (1u << a->r1) | (1u << a->r2);
    } else if (a->fam == AF_ADD || a->fam == AF_SUB || a->fam == AF_CMP || a->fam == AF_TFR) {
        switch (a->src) {
        case SRC_ACC: j->used |= 1u << (V_A + 1 - a->d); break;
        case SRC_X: j->used |= (1u << V_X0) | (1u << V_X1); break;
        case SRC_Y: j->used |= (1u << V_Y0) | (1u << V_Y1); break;
        case SRC_X0: j->used |= 1u << V_X0; break;
        case SRC_Y0: j->used |= 1u << V_Y0; break;
        case SRC_X1: j->used |= 1u << V_X1; break;
        case SRC_Y1: j->used |= 1u << V_Y1; break;
        }
    }
}

/* ---- loading and storing the held registers -------------------------------- */

/* scratch registers: rax rcx rdx r8 r9 r10 r11 */
#define T0 X_RAX
#define T1 X_RCX
#define T2 X_RDX
#define T3 X_R8
#define T4 X_R9
#define T5 X_R10
#define T6 X_R11

/* acc = (A2 << 48) | (A1 << 24) | A0, into dst (64-bit) */
static void e_acc_load_mem(Jc *j, int dst, int k)
{
    Xe *x = &j->x;
    x_load(x, 0, dst, X_RBX, OFF_REG(DSP_REG_A2 + k));
    x_shift(x, S_SHL, 1, dst, 48);
    x_load(x, 0, T6, X_RBX, OFF_REG(DSP_REG_A1 + k));
    x_shift(x, S_SHL, 1, T6, 24);
    x_alu(x, A_OR, 1, dst, T6);
    x_load(x, 0, T6, X_RBX, OFF_REG(DSP_REG_A0 + k));
    x_alu(x, A_OR, 1, dst, T6);
}

/* A2 = acc >> 48 & 0xFF, A1 = acc >> 24 & 0xFFFFFF, A0 = acc & 0xFFFFFF */
static void e_acc_store_mem(Jc *j, int k, int src)
{
    Xe *x = &j->x;
    x_mov(x, 1, T6, src);
    x_alui(x, A_AND, 0, T6, 0xFFFFFF);
    x_store(x, 0, X_RBX, OFF_REG(DSP_REG_A0 + k), T6);
    x_mov(x, 1, T6, src);
    x_shift(x, S_SHR, 1, T6, 24);
    x_alui(x, A_AND, 0, T6, 0xFFFFFF);
    x_store(x, 0, X_RBX, OFF_REG(DSP_REG_A1 + k), T6);
    x_mov(x, 1, T6, src);
    x_shift(x, S_SHR, 1, T6, 48);
    x_alui(x, A_AND, 0, T6, 0xFF);
    x_store(x, 0, X_RBX, OFF_REG(DSP_REG_A2 + k), T6);
}

static void e_load_held(Jc *j)
{
    int v;
    for (v = 0; v < V_COUNT; v++) {
        if (j->loc[v] < 0)
            continue;
        if (v == V_A || v == V_B)
            e_acc_load_mem(j, j->loc[v], v - V_A);
        else
            x_load(&j->x, 0, j->loc[v], X_RBX, OFF_REG(s_vreg[v]));
    }
}

static void e_store_held(Jc *j)
{
    int v;
    for (v = 0; v < V_COUNT; v++) {
        if (j->loc[v] < 0)
            continue;
        if (v == V_A || v == V_B)
            e_acc_store_mem(j, v - V_A, j->loc[v]);
        else
            x_store(&j->x, 0, X_RBX, OFF_REG(s_vreg[v]), j->loc[v]);
    }
}

/* A V register's value in a host register: its own, or `t` loaded. */
static int e_get(Jc *j, int v, int t)
{
    if (j->loc[v] >= 0)
        return j->loc[v];
    if (v == V_A || v == V_B)
        e_acc_load_mem(j, t, v - V_A);
    else
        x_load(&j->x, 0, t, X_RBX, OFF_REG(s_vreg[v]));
    return t;
}

static void e_set(Jc *j, int v, int src)
{
    if (j->loc[v] >= 0) {
        if (j->loc[v] != src)
            x_mov(&j->x, (v == V_A || v == V_B), j->loc[v], src);
        return;
    }
    if (v == V_A || v == V_B)
        e_acc_store_mem(j, v - V_A, src);
    else
        x_store(&j->x, 0, X_RBX, OFF_REG(s_vreg[v]), src);
}

static void e_flush_ccr(Jc *j);
static void e_pend_ccr(Jc *j, int r);

/* Any register by index (not a held one's part). */
static void e_reg_load(Jc *j, int dst, uint32_t r)
{
    int v = v_of_reg(r);
    if (r == DSP_REG_SR)
        e_flush_ccr(j);
    if (v >= 0 && j->loc[v] >= 0)
        x_mov(&j->x, 0, dst, j->loc[v]);
    else
        x_load(&j->x, 0, dst, X_RBX, OFF_REG(r));
}

static void e_reg_store(Jc *j, uint32_t r, int src)
{
    int v = v_of_reg(r);
    if (v >= 0 && j->loc[v] >= 0) {
        if (j->loc[v] != src)
            x_mov(&j->x, 0, j->loc[v], src);
    }
    else
        x_store(&j->x, 0, X_RBX, OFF_REG(r), src);
    if (r == DSP_REG_SR)
        j->pend = 0;                    /* all of SR written */
}

/* ---- arithmetic pieces ------------------------------------------------------- */

#define M56_HI 0x00FFFFFF               /* M56 = (M56_HI << 32) | 0xFFFFFFFF */

/* dst64 &= M56 */
static void e_mask56(Jc *j, int dst)
{
    x_shift(&j->x, S_SHL, 1, dst, 8);
    x_shift(&j->x, S_SHR, 1, dst, 8);
}

/* dst64 = sign-extended 24-bit src32, shifted to bits 47-24, masked to 56 */
static void e_src24(Jc *j, int dst, int src)
{
    Xe *x = &j->x;
    /* src's 24 bits (the rest zero) at 63-40, arithmetic to 47-24, masked */
    x_mov(x, 0, dst, src);
    x_shift(x, S_SHL, 1, dst, 40);
    x_shift(x, S_SAR, 1, dst, 16);
    e_mask56(j, dst);
}

/* dst64 = hi:lo as a 48-bit value sign-extended to 56 (src48) */
static void e_src48(Jc *j, int dst, int hi, int lo)
{
    Xe *x = &j->x;
    e_src24(j, dst, hi);
    x_alu(x, A_OR, 1, dst, lo);
}

/* The flags of a 56-bit add r = s + d (r before masking in `r`): r masked,
 * nsr = L|V|C. Uses T6. s, d, r distinct. */
static void e_add56_flags_c(Jc *j, int r, int s, int d, int nsr, int want_c)
{
    Xe *x = &j->x;
    /* carry = bit 56 of the unmasked sum */
    if (want_c) {
        x_mov(x, 1, nsr, r);
        x_shift(x, S_SHR, 1, nsr, 56);
        x_alui(x, A_AND, 0, nsr, 1);
    } else {
        x_movi(x, nsr, 0);
    }
    e_mask56(j, r);
    /* ov = ((s ^ r) & (d ^ r)) >> 55 & 1 */
    x_mov(x, 1, T6, s);
    x_alu(x, A_XOR, 1, T6, r);
    x_mov(x, 1, s, d);                  /* s is clobbered */
    x_alu(x, A_XOR, 1, s, r);           /* d ^ r */
    x_alu(x, A_AND, 1, T6, s);
    x_shift(x, S_SHR, 1, T6, 55);
    x_alui(x, A_AND, 0, T6, 1);
    /* nsr |= ov << L | ov << V */
    x_imul_i8(x, 0, T6, T6, (1 << DSP_SR_L) | (1 << DSP_SR_V));
    x_alu(x, A_OR, 0, nsr, T6);
}

static void e_add56_flags(Jc *j, int r, int s, int d, int nsr)
{
    e_add56_flags_c(j, r, s, d, nsr, 1);
}

/* r = (d - s) & M56; nsr = L|V|C (C: d < s). s, d, r distinct; s clobbered. */
static void e_sub56(Jc *j, int r, int s, int d, int nsr)
{
    Xe *x = &j->x;
    x_alu(x, A_CMP, 1, d, s);
    x_setcc(x, CC_B, nsr);
    x_movzx8(x, nsr, nsr);
    x_mov(x, 1, r, d);
    x_alu(x, A_SUB, 1, r, s);
    e_mask56(j, r);
    /* ov = ((s ^ d) & (r ^ d)) >> 55 & 1 */
    x_alu(x, A_XOR, 1, s, d);
    x_mov(x, 1, T6, r);
    x_alu(x, A_XOR, 1, T6, d);
    x_alu(x, A_AND, 1, T6, s);
    x_shift(x, S_SHR, 1, T6, 55);
    x_alui(x, A_AND, 0, T6, 1);
    x_imul_i8(x, 0, T6, T6, (1 << DSP_SR_L) | (1 << DSP_SR_V));
    x_alu(x, A_OR, 0, nsr, T6);
}

/* sr (32-bit, in place): E, U, N, Z from r (56-bit) in scaling mode 0.
 * Uses T6 and `t`. */
static void e_ccr(Jc *j, int sr, int r, int t)
{
    Xe *x = &j->x;
    int up = j->scale == 2;             /* scaling up: E from 55-46, U from 46 and 45 */
    x_alui(x, A_AND, 0, sr, (int32_t)(0xFFFFu - (SR_E | SR_U | SR_N | SR_Z)));
    /* E: bits 55-47 not all equal: ((r >> 47) + 1) & 0x1FF > 1 */
    x_mov(x, 1, t, r);
    x_shift(x, S_SHR, 1, t, up ? 46 : 47);
    x_alui(x, A_ADD, 0, t, 1);
    x_alui(x, A_AND, 0, t, up ? 0x3FF : 0x1FF);
    x_alui(x, A_CMP, 0, t, 1);
    x_setcc(x, CC_A, t);
    x_movzx8(x, t, t);
    x_shift(x, S_SHL, 0, t, DSP_SR_E);
    x_alu(x, A_OR, 0, sr, t);
    /* U: bits 47 and 46 equal */
    x_mov(x, 1, t, r);
    x_shift(x, S_SHR, 1, t, up ? 45 : 46);
    x_mov(x, 0, T6, t);
    x_shift(x, S_SHR, 0, T6, 1);
    x_alu(x, A_XOR, 0, t, T6);
    x_alui(x, A_AND, 0, t, 1);
    x_alui(x, A_XOR, 0, t, 1);
    x_shift(x, S_SHL, 0, t, DSP_SR_U);
    x_alu(x, A_OR, 0, sr, t);
    /* Z */
    x_test(x, 1, r, r);
    x_setcc(x, CC_E, t);
    x_movzx8(x, t, t);
    x_shift(x, S_SHL, 0, t, DSP_SR_Z);
    x_alu(x, A_OR, 0, sr, t);
    /* N: bit 55 to bit 3 */
    x_mov(x, 1, t, r);
    x_shift(x, S_SHR, 1, t, 52);
    x_alui(x, A_AND, 0, t, 8);
    x_alu(x, A_OR, 0, sr, t);
}

/* dst64 = mul56(a, b, minus): a, b 24-bit in 32-bit registers */
static void e_mul56(Jc *j, int dst, int a, int b, int minus)
{
    Xe *x = &j->x;
    x_mov(x, 0, dst, a);
    x_shift(x, S_SHL, 0, dst, 8);
    x_movsxd(x, dst, dst);
    x_mov(x, 0, T6, b);
    x_shift(x, S_SHL, 0, T6, 8);
    x_movsxd(x, T6, T6);
    x_imul(x, 1, dst, T6);
    x_shift(x, S_SAR, 1, dst, 15);
    if (minus)
        x_neg(x, 1, dst);
    e_mask56(j, dst);
}

/* rnd56 in scaling mode 0: v += 1 << 23; if low 24 bits are 0, clear bit 24;
 * low 24 bits cleared. Uses T6. */
static void e_rnd56(Jc *j, int v)
{
    Xe *x = &j->x;
    if (j->scale == 2) {
        /* scaling up (dsp_rnd56's S1): + 1 << 22; A0 = 0 when its low 23 bits
         * are, else A0 &= 0x800000 */
        uint8_t *nz, *done;
        x_alui(x, A_ADD, 1, v, 0x400000);
        e_mask56(j, v);
        x_testi(x, 0, v, 0x7FFFFF);
        nz = x_jcc(x, CC_NE);
        x_movi64(x, T6, ~(uint64_t)0xFFFFFF);
        x_alu(x, A_AND, 1, v, T6);
        done = x_jmp(x);
        x_patch(nz, x->p);
        x_movi64(x, T6, ~(uint64_t)0x7FFFFF);
        x_alu(x, A_AND, 1, v, T6);
        x_patch(done, x->p);
        return;
    }
    x_alui(x, A_ADD, 1, v, 0x800000);
    e_mask56(j, v);
    x_movi64(x, T6, ~(uint64_t)(1u << 24));
    x_testi(x, 0, v, 0xFFFFFF);
    {
        uint8_t *skip = x_jcc(x, CC_NE);
        x_alu(x, A_AND, 1, v, T6);
        x_patch(skip, x->p);
    }
    x_movi64(x, T6, ~(uint64_t)0xFFFFFF);
    x_alu(x, A_AND, 1, v, T6);
}

/* accu24 in scaling mode 0 into dst (32-bit); may set L in SR. acc: the 56-bit
 * value in a register. Uses T6. */
static void e_accu24(Jc *j, int dst, int acc, int sr_v)
{
    Xe *x = &j->x;
    uint8_t *ok1, *ok2, *pos, *done;
    /* value = (A2 << 24 + A1) & 0xFFFFFF = (acc >> 24) & 0xFFFFFF; a2 = acc >> 48.
     * Scaling up: ((A2 << 24 + A1) << 1 | A0 >> 23) & 0xFFFFFF = (acc >> 23) & 0xFFFFFF */
    x_mov(x, 1, dst, acc);
    x_shift(x, S_SHR, 1, dst, j->scale == 2 ? 23 : 24);
    x_alui(x, A_AND, 0, dst, 0xFFFFFF);
    x_mov(x, 1, T6, acc);
    x_shift(x, S_SHR, 1, T6, 48);
    /* a2 == 0 && value <= 0x7fffff: as is */
    x_test(x, 0, T6, T6);
    {
        uint8_t *ne = x_jcc(x, CC_NE);
        x_alui(x, A_CMP, 0, dst, 0x7FFFFF);
        ok1 = x_jcc(x, CC_BE);
        x_patch(ne, x->p);
    }
    /* a2 == 0xff && value >= 0x800000: as is */
    x_alui(x, A_CMP, 0, T6, 0xFF);
    {
        uint8_t *ne = x_jcc(x, CC_NE);
        x_alui(x, A_CMP, 0, dst, 0x800000);
        ok2 = x_jcc(x, CC_AE);
        x_patch(ne, x->p);
    }
    /* limited: L, and the extreme of the sign */
    if (j->loc[sr_v] >= 0)
        x_alui(x, A_OR, 0, j->loc[sr_v], (int32_t)SR_L);
    else
        x_alui_m(x, A_OR, X_RBX, OFF_REG(DSP_REG_SR), (int32_t)SR_L);
    x_testi(x, 0, T6, 0x80);
    pos = x_jcc(x, CC_E);
    x_movi(x, dst, 0x800000);
    done = x_jmp(x);
    x_patch(pos, x->p);
    x_movi(x, dst, 0x7FFFFF);
    x_patch(done, x->p);
    x_patch(ok1, x->p);
    x_patch(ok2, x->p);
}

/* acc form of set_acc(v): ((int32)(v << 8) >> 8) << 24, masked */
static void e_acc_from24(Jc *j, int dst, int v)
{
    e_src24(j, dst, v);
}

/* ---- the ALU, inline ----------------------------------------------------------- */

/* The 56-bit source of add/sub/cmp/tfr into dst. */
static void e_alu_src(Jc *j, const AluDesc *a, int dst)
{
    int h, l;
    switch (a->src) {
    case SRC_ACC:
        h = e_get(j, V_A + 1 - a->d, dst);
        x_mov(&j->x, 1, dst, h);
        return;
    case SRC_X:
        h = e_get(j, V_X1, T4);
        l = e_get(j, V_X0, T5);
        e_src48(j, dst, h, l);
        return;
    case SRC_Y:
        h = e_get(j, V_Y1, T4);
        l = e_get(j, V_Y0, T5);
        e_src48(j, dst, h, l);
        return;
    default: {
        static const int vv[] = { 0, 0, 0, V_X0, V_Y0, V_X1, V_Y1 };
        h = e_get(j, vv[a->src], T4);
        e_src24(j, dst, h);
        return;
    }
    }
}

/* Emits the ALU routine of op (inline when it can). Registers: T0-T6 free. */
static void e_alu(Jc *j, const FOp *op)
{
    Xe *x = &j->x;
    const AluDesc *a = &s_alu[op->w0 & 0xFF];
    int acc, sr;
    if (!op->alu)
        return;
    if (!alu_inline(op)) {
        /* the C routine on the registers in memory */
        e_flush_ccr(j);
        e_store_held(j);
        x_mov(x, 1, X_RCX, X_RBX);
        x_call(x, (const void *)op->alu);
        e_load_held(j);
        return;
    }
    switch (a->fam) {
    case AF_ADD:
    case AF_SUB:
        /* F_ADDSUB: r = d +- s; acc = r; sr = ccr(sr, r) & ~(V|C) | nsr */
        e_alu_src(j, a, T1);
        acc = e_get(j, V_A + a->d, T2);
        if (a->fam == AF_ADD) {
            x_mov(x, 1, T0, acc);
            x_alu(x, A_ADD, 1, T0, T1);
            e_add56_flags(j, T0, T1, acc == T2 ? T2 : acc, T3);
        } else {
            e_sub56(j, T0, T1, acc, T3);
        }
        e_set(j, V_A + a->d, T0);
        sr = e_get(j, V_SR, T4);
        e_pend_ccr(j, T0);
        x_alui(x, A_AND, 0, sr, (int32_t)(0xFFFFu - (SR_V | SR_C)));
        x_alu(x, A_OR, 0, sr, T3);
        e_set(j, V_SR, sr);
        return;
    case AF_CMP:
        e_alu_src(j, a, T1);
        acc = e_get(j, V_A + a->d, T2);
        e_sub56(j, T0, T1, acc, T3);
        sr = e_get(j, V_SR, T4);
        e_pend_ccr(j, T0);
        x_alui(x, A_AND, 0, sr, (int32_t)(0xFFFFu - (SR_V | SR_C)));
        x_alu(x, A_OR, 0, sr, T3);
        e_set(j, V_SR, sr);
        return;
    case AF_TFR:
        /* tfr b,a / a,b: the other accumulator's parts as they are;
         * tfr reg: A0 = 0, A1 = reg, A2 = sign (= src24) */
        e_alu_src(j, a, T1);
        e_set(j, V_A + a->d, T1);
        return;
    case AF_TST:
        acc = e_get(j, V_A + a->d, T2);
        sr = e_get(j, V_SR, T4);
        e_pend_ccr(j, acc);
        x_alui(x, A_AND, 0, sr, (int32_t)(0xFFFFu - SR_V));
        e_set(j, V_SR, sr);
        return;
    case AF_CLR:
        x_movi(x, T0, 0);
        e_set(j, V_A + a->d, T0);
        sr = e_get(j, V_SR, T4);
        x_alui(x, A_AND, 0, sr, (int32_t)(0xFFFFu - (SR_E | SR_N | SR_V)));
        x_alui(x, A_OR, 0, sr, (int32_t)(SR_U | SR_Z));
        e_set(j, V_SR, sr);
        j->pend = 0;                    /* E, U, N, Z all written */
        return;
    case AF_ABS: {
        /* ov = v == 0x0080000000000000; r = v >> 55 ? (0 - v) & M56 : v */
        uint8_t *pos;
        acc = e_get(j, V_A + a->d, T2);
        x_mov(x, 1, T0, acc);
        x_movi64(x, T6, 0x0080000000000000ull);
        x_alu(x, A_CMP, 1, T0, T6);
        x_setcc(x, CC_E, T3);
        x_movzx8(x, T3, T3);
        x_test(x, 1, T0, T6);
        pos = x_jcc(x, CC_E);
        x_neg(x, 1, T0);
        e_mask56(j, T0);
        x_patch(pos, x->p);
        e_set(j, V_A + a->d, T0);
        sr = e_get(j, V_SR, T4);
        x_alui(x, A_AND, 0, sr, (int32_t)(0xFFFFu - SR_V));
        x_mov(x, 0, T1, T3);
        x_shift(x, S_SHL, 0, T1, DSP_SR_L);
        x_alu(x, A_OR, 0, sr, T1);
        x_shift(x, S_SHL, 0, T3, DSP_SR_V);
        x_alu(x, A_OR, 0, sr, T3);
        e_pend_ccr(j, T0);
        e_set(j, V_SR, sr);
        return;
    }
    case AF_MUL: {
        /* F_MUL: r = mul56; mac: r = add56(r, acc); mpyr/macr: rnd; acc = r;
         * sr = ccr(sr, r) & ~V; mac: sr |= nsr & 0xfe */
        int r1 = e_get(j, a->r1, T4), r2 = e_get(j, a->r2, T5);
        e_mul56(j, T0, r1, r2, a->minus);
        if (a->kind >= 2) {
            acc = e_get(j, V_A + a->d, T2);
            x_mov(x, 1, T1, T0);
            x_alu(x, A_ADD, 1, T0, acc);
            e_add56_flags_c(j, T0, T1, acc, T3, 0);     /* mac keeps C */
        }
        if (a->kind & 1)
            e_rnd56(j, T0);
        e_set(j, V_A + a->d, T0);
        sr = e_get(j, V_SR, T4);
        e_pend_ccr(j, T0);
        x_alui(x, A_AND, 0, sr, (int32_t)(0xFFFFu - SR_V));
        if (a->kind >= 2)
            x_alu(x, A_OR, 0, sr, T3);  /* L, V (no C) */
        e_set(j, V_SR, sr);
        return;
    }
    }
}

/* ---- memory ------------------------------------------------------------------- */

/* dst = mem[space][a] for an address in `ar` (32-bit). Uses T6 and a slow call
 * (out of line: e_cold). */
static void e_rd_dyn(Jc *j, int dst, int space, int ar)
{
    Xe *x = &j->x;
    ColdRec *r = &j->cold[j->ncold++];
    x_alui(x, A_CMP, 0, ar, space == DSP_SPACE_X ? 0xC00 : DSP_YRAM_SIZE);
    r->jmp = x_jcc(x, CC_AE);
    x_load_idx(x, dst, X_RBX, ar, 4, space == DSP_SPACE_X ? OFF_X : OFF_Y);
    r->ret = x->p;
    r->wr = 0;
    r->space = (uint8_t)space;
    r->dst = (int8_t)dst;
    r->ar = (int8_t)ar;
}

/* mem[space][a] = v: a in r10, v in r9 (the slow call's own argument).
 * A slow write that reached a peripheral jumps to a stop exit (stop index
 * `si`): the held registers are stored there. Out of line: e_cold. */
static void e_wr_dyn(Jc *j, int space, int ar, int vr, uint32_t si)
{
    Xe *x = &j->x;
    ColdRec *r = &j->cold[j->ncold++];
    x_alui(x, A_CMP, 0, ar, space == DSP_SPACE_X ? 0xC00 : DSP_YRAM_SIZE);
    r->jmp = x_jcc(x, CC_AE);
    x_store_idx(x, X_RBX, ar, 4, space == DSP_SPACE_X ? OFF_X : OFF_Y, vr);
    r->ret = x->p;
    r->wr = 1;
    r->space = (uint8_t)space;
    r->ar = (int8_t)ar;
    r->vr = (int8_t)vr;
    r->si = si;
    r->pend = (uint8_t)j->pend;
}

/* The slow paths recorded since the last call, each jumping back. */
static void e_cold(Jc *j)
{
    Xe *x = &j->x;
    uint32_t k;
    int pend = j->pend;
    for (k = 0; k < j->ncold; k++) {
        ColdRec *r = &j->cold[k];
        int ar = r->ar;
        x_patch(r->jmp, x->p);
        if (!r->wr) {
            int dst = r->dst;
            if (r->space == DSP_SPACE_X) {
                /* the mix buffer: X:0xC00-0xFFF and X:0x1400-0x17FF */
                uint8_t *mix1, *mix2;
                x_mov(x, 0, T6, ar);
                x_alui(x, A_SUB, 0, T6, 0xC00);
                x_alui(x, A_CMP, 0, T6, 0x400);
                mix1 = x_jcc(x, CC_B);
                x_mov(x, 0, T6, ar);
                x_alui(x, A_SUB, 0, T6, DSP_MIXBUFFER_BASE);
                x_alui(x, A_CMP, 0, T6, DSP_MIXBUFFER_SIZE);
                mix2 = x_jcc(x, CC_B);
                x_mov(x, 1, X_R8, ar);
                x_movi(x, X_RDX, DSP_SPACE_X);
                x_mov(x, 1, X_RCX, X_RBX);
                x_call(x, (const void *)jit_rd_slow);
                x_mov(x, 0, dst, X_RAX);
                x_patch(x_jmp(x), r->ret);
                x_patch(mix1, x->p);
                x_patch(mix2, x->p);
                x_load_idx(x, dst, X_RBX, T6, 4, OFF_MIX);
            } else {
                x_mov(x, 1, X_R8, ar);
                x_movi(x, X_RDX, DSP_SPACE_Y);
                x_mov(x, 1, X_RCX, X_RBX);
                x_call(x, (const void *)jit_rd_slow);
                x_mov(x, 0, dst, X_RAX);
            }
            x_patch(x_jmp(x), r->ret);
            continue;
        }
        if (r->space == DSP_SPACE_X) {
            uint8_t *mix1, *mix2;
            int vr = r->vr;
            x_mov(x, 0, X_RAX, ar);
            x_alui(x, A_SUB, 0, X_RAX, 0xC00);
            x_alui(x, A_CMP, 0, X_RAX, 0x400);
            mix1 = x_jcc(x, CC_B);
            x_mov(x, 0, X_RAX, ar);
            x_alui(x, A_SUB, 0, X_RAX, DSP_MIXBUFFER_BASE);
            x_alui(x, A_CMP, 0, X_RAX, DSP_MIXBUFFER_SIZE);
            mix2 = x_jcc(x, CC_B);
            x_mov(x, 0, X_R9, vr);
            x_mov(x, 0, X_R8, ar);
            x_movi(x, X_RDX, DSP_SPACE_X);
            x_mov(x, 1, X_RCX, X_RBX);
            x_call(x, (const void *)jit_wr_slow);
            x_test(x, 0, X_RAX, X_RAX);
            /* the stop's state has its flags: STOP_FLUSH, done at the exit */
            j->stops[j->nstops] = x_jcc(x, CC_NE);
            j->stop_index[j->nstops++] = r->si | (r->pend ? STOP_FLUSH : 0);
            x_patch(x_jmp(x), r->ret);
            x_patch(mix1, x->p);
            x_patch(mix2, x->p);
            x_store_idx(x, X_RBX, X_RAX, 4, OFF_MIX, vr);
        } else {
            x_mov(x, 0, X_R9, r->vr);
            x_mov(x, 0, X_R8, ar);
            x_movi(x, X_RDX, DSP_SPACE_Y);
            x_mov(x, 1, X_RCX, X_RBX);
            x_call(x, (const void *)jit_wr_slow);
        }
        x_patch(x_jmp(x), r->ret);
    }
    j->ncold = 0;
    j->pend = pend;
}

/* ---- the bodies ------------------------------------------------------------------- */

/* emu_calc_ea modes 0-5, 7 with linear addressing: the address into `dst`,
 * the register updated. */
static void e_ea(Jc *j, int dst, uint32_t mode, uint32_t n)
{
    Xe *x = &j->x;
    int v = V_R0 + (int)n, r = e_get(j, v, T5);
    if (j->bitrev & (1u << n)) {
        /* M = 0: an update is jit_bitrev (in C: the caller-saved registers go) */
        if (mode == 4) {
            x_mov(x, 0, dst, r);
            return;
        }
        if (mode != 5 && mode != 7) {
            x_mov(x, 0, dst, r);
            x_store(x, 0, X_RSP, SLOT(6), dst);
        }
        x_mov(x, 0, X_RCX, r);
        x_load(x, 0, X_RDX, X_RBX, OFF_REG(DSP_REG_N0 + n));
        x_call(x, (const void *)jit_bitrev);
        if (mode == 5) {
            x_mov(x, 0, dst, X_RAX);            /* (Rx+Nx): the address only */
            return;
        }
        e_set(j, v, X_RAX);
        if (mode == 7)
            x_mov(x, 0, dst, X_RAX);            /* -(Rx): the updated one */
        else
            x_load(x, 0, dst, X_RSP, SLOT(6));
        return;
    }
    switch (mode) {
    case 4:
        x_mov(x, 0, dst, r);
        return;
    case 2:
    case 3:
        x_mov(x, 0, dst, r);
        x_alui(x, mode == 3 ? A_ADD : A_SUB, 0, r, 1);
        x_movzx16(x, r, r);
        e_set(j, v, r);
        return;
    case 0:
    case 1:
        x_mov(x, 0, dst, r);
        x_alu_m(x, mode == 1 ? A_ADD : A_SUB, 0, r, X_RBX, OFF_REG(DSP_REG_N0 + n));
        x_movzx16(x, r, r);
        e_set(j, v, r);
        return;
    case 5:
        x_mov(x, 0, dst, r);
        x_alu_m(x, A_ADD, 0, dst, X_RBX, OFF_REG(DSP_REG_N0 + n));
        x_movzx16(x, dst, dst);
        return;
    default: /* 7 */
        x_alui(x, A_SUB, 0, r, 1);
        x_movzx16(x, r, r);
        e_set(j, v, r);
        x_mov(x, 0, dst, r);
        return;
    }
}

/* the register write of a move: A/B as set_acc, others masked */
static void e_move_to_reg(Jc *j, uint32_t numreg, int vr, uint32_t mask)
{
    if (numreg == DSP_REG_A || numreg == DSP_REG_B) {
        e_acc_from24(j, T0, vr);
        e_set(j, V_A + (numreg - DSP_REG_A), T0);
        return;
    }
    if (mask != 0xFFFFFFFFu)
        x_alui(&j->x, A_AND, 0, vr, (int32_t)mask);
    e_reg_store(j, numreg, vr);
}

/* the register read of a move: A/B limited, others as they are */
static void e_move_from_reg(Jc *j, int dst, uint32_t numreg)
{
    if (numreg == DSP_REG_A || numreg == DSP_REG_B) {
        int acc = e_get(j, V_A + (numreg - DSP_REG_A), T0);
        e_accu24(j, dst, acc, V_SR);
        return;
    }
    e_reg_load(j, dst, numreg);
}

/* Stack slots for values that live across the ALU (it may call C). */
#define SLOT_PR SLOT(5)                 /* the last flag-setting result (56-bit) */

/* A move's value or address kept across the ALU: in a pool register the block
 * doesn't hold (callee-saved, so C calls keep it), else in a stack slot. */
static void e_save(Jc *j, int k, int src)
{
    if (j->spare[k] >= 0)
        x_mov(&j->x, 0, j->spare[k], src);
    else
        x_store(&j->x, 0, X_RSP, SLOT(k), src);
}

static void e_restore(Jc *j, int dst, int k)
{
    if (j->spare[k] >= 0)
        x_mov(&j->x, 0, dst, j->spare[k]);
    else
        x_load(&j->x, 0, dst, X_RSP, SLOT(k));
}

/* Flags only where read: an ALU result's E, U, N and Z (functions of the
 * result alone in scaling mode 00) are computed when something reads SR (a
 * move from SR, C code, a stop, the end of the bodies), not at each ALU op.
 * Uses T0, T1, T2, T4, T6 (T2: the shared code's way back). */
static void e_flush_ccr(Jc *j)
{
    int sr;
    if (!j->pend)
        return;
    if (j->jt && j->jt->flush[0][0] && (j->loc[V_SR] == X_RSI || j->loc[V_SR] < 0)) {
        /* the shared one (SR in esi or in memory), back through rdx */
        uint8_t *thunk = j->jt->flush[j->loc[V_SR] < 0][j->scale == 2], *disp;
        disp = x_lea_rip(&j->x, X_RDX);
        x_patch(x_jmp(&j->x), thunk);
        x_patch(disp, j->x.p);
        j->pend = 0;
        return;
    }
    x_load(&j->x, 1, T0, X_RSP, SLOT_PR);
    sr = e_get(j, V_SR, T4);
    e_ccr(j, sr, T0, T1);
    e_set(j, V_SR, sr);
    j->pend = 0;
}

/* The pending result: r's E, U, N, Z will be SR's. */
static void e_pend_ccr(Jc *j, int r)
{
    x_store(&j->x, 1, X_RSP, SLOT_PR, r);
    j->pend = 1;
}

static void e_body(Jc *j, const FOp *op, uint32_t si)
{
    Xe *x = &j->x;
    switch (jk_of(op)) {
    case JK_NOP:
        return;
    case JK_PM2NOP:
        e_alu(j, op);
        return;
    case JK_PM2REG:
        e_move_from_reg(j, T3, op->r1);
        e_save(j, 0, T3);
        e_alu(j, op);
        e_restore(j, T3, 0);
        e_move_to_reg(j, op->r2, T3, op->mask);
        return;
    case JK_PM3:
        e_alu(j, op);
        x_movi(x, T3, op->imm);
        e_move_to_reg(j, op->r2, T3, op->mask);
        return;
    case JK_PM5: {
        uint32_t mode = pm5_mode(op), w = (op->w0 >> 15) & 1, space = (op->w0 >> 19) & 1;
        int alu = op->alu != NULL;      /* else nothing between the read and the write */
        if (mode == 6 || mode == 8 || mode == 9)
            x_movi(x, T3, op->imm);
        else
            e_ea(j, T3, mode, op->ea1 & 7);
        if (!w && alu)
            e_save(j, 1, T3);              /* the address */
        if (w) {
            if (mode == 9)
                x_mov(x, 0, T2, T3);
            else
                e_rd_dyn(j, T2, (int)space, T3);
        } else {
            e_move_from_reg(j, T2, op->r1);
        }
        if (alu) {
            e_save(j, 0, T2);              /* the value */
            e_alu(j, op);
            e_restore(j, T2, 0);
        }
        if (w) {
            e_move_to_reg(j, op->r1, T2, op->mask);
        } else {
            if (alu)
                e_restore(j, X_R10, 1);
            else
                x_mov(x, 0, X_R10, T3);
            x_mov(x, 0, X_R9, T2);
            e_wr_dyn(j, (int)space, X_R10, X_R9, si);
        }
        return;
    }
    case JK_PM8: {
        uint32_t w0 = op->w0, n1 = op->r1, n2 = op->r2;
        e_ea(j, T3, op->ea1 >> 3, op->ea1 & 7);
        e_save(j, 1, T3);
        e_ea(j, T3, op->ea2 >> 3, op->ea2 & 7);
        e_save(j, 2, T3);
        if (w0 & (1 << 15)) {
            e_restore(j, T3, 1);
            e_rd_dyn(j, T2, DSP_SPACE_X, T3);
        } else {
            e_move_from_reg(j, T2, n1);
        }
        e_save(j, 0, T2);
        if (w0 & (1 << 22)) {
            e_restore(j, T3, 2);
            e_rd_dyn(j, T2, DSP_SPACE_Y, T3);
        } else {
            e_move_from_reg(j, T2, n2);
        }
        e_save(j, 3, T2);
        e_alu(j, op);
        e_restore(j, T2, 0);
        if (w0 & (1 << 15)) {
            if (n1 == DSP_REG_A || n1 == DSP_REG_B)
                e_move_to_reg(j, n1, T2, 0xFFFFFFFFu);
            else
                e_reg_store(j, n1, T2);
        } else {
            e_restore(j, X_R10, 1);
            x_mov(x, 0, X_R9, T2);
            e_wr_dyn(j, DSP_SPACE_X, X_R10, X_R9, si);
        }
        e_restore(j, T2, 3);
        if (w0 & (1 << 22)) {
            if (n2 == DSP_REG_A || n2 == DSP_REG_B)
                e_move_to_reg(j, n2, T2, 0xFFFFFFFFu);
            else
                e_reg_store(j, n2, T2);
        } else {
            e_restore(j, X_R10, 2);
            x_mov(x, 0, X_R9, T2);
            e_wr_dyn(j, DSP_SPACE_Y, X_R10, X_R9, si);
        }
        return;
    }
    case JK_ADDX:
    case JK_SUBX:
    case JK_CMPX: {
        /* b_add_x / b_sub_x / b_cmp_x: add56 / sub56 with a constant operand
         * (add, sub: src24(imm); cmp: imm << 24, as xemu), flags as the ALU's */
        int acc, sr, k = jk_of(op);
        uint64_t sv;
        if (k == JK_CMPX) {
            sv = (uint64_t)op->imm << 24;
        } else {
            int64_t t = (int64_t)(int32_t)(op->imm << 8) >> 8;
            sv = ((uint64_t)t << 24) & 0x00FFFFFFFFFFFFFFull;
        }
        x_movi64(x, T1, sv);
        acc = e_get(j, V_A + (int)op->r1, T2);
        if (k == JK_ADDX) {
            x_mov(x, 1, T0, acc);
            x_alu(x, A_ADD, 1, T0, T1);
            e_add56_flags(j, T0, T1, acc, T3);
        } else {
            e_sub56(j, T0, T1, acc, T3);
        }
        if (k != JK_CMPX)
            e_set(j, V_A + (int)op->r1, T0);
        sr = e_get(j, V_SR, T4);
        e_pend_ccr(j, T0);
        x_alui(x, A_AND, 0, sr, (int32_t)(0xFFFFu - (SR_V | SR_C)));
        x_alu(x, A_OR, 0, sr, T3);
        e_set(j, V_SR, sr);
        return;
    }
    case JK_LOGICX: {
        /* b_logic_x: A1 &= / |= imm; N, Z from it, V cleared (E, U kept) */
        int acc, sr;
        e_flush_ccr(j);
        acc = e_get(j, V_A + (int)op->r1, T2);
        x_mov(x, 1, T0, acc);
        x_shift(x, S_SHR, 1, T0, 24);
        x_alui(x, A_AND, 0, T0, 0xFFFFFF);
        x_alui(x, op->space ? A_OR : A_AND, 0, T0, (int32_t)op->imm);
        x_alui(x, A_AND, 0, T0, 0xFFFFFF);
        x_movi64(x, T1, ~(0xFFFFFFull << 24));
        x_alu(x, A_AND, 1, T1, acc);
        x_mov(x, 0, T3, T0);
        x_shift(x, S_SHL, 1, T3, 24);
        x_alu(x, A_OR, 1, T1, T3);
        e_set(j, V_A + (int)op->r1, T1);
        sr = e_get(j, V_SR, T4);
        x_alui(x, A_AND, 0, sr, (int32_t)(0xFFFFu - (SR_N | SR_Z | SR_V)));
        x_mov(x, 0, T3, T0);
        x_shift(x, S_SHR, 0, T3, 20);
        x_alui(x, A_AND, 0, T3, SR_N);
        x_alu(x, A_OR, 0, sr, T3);
        x_test(x, 0, T0, T0);
        x_setcc(x, CC_E, T3);
        x_movzx8(x, T3, T3);
        x_shift(x, S_SHL, 0, T3, DSP_SR_Z);
        x_alu(x, A_OR, 0, sr, T3);
        e_set(j, V_SR, sr);
        return;
    }
    case JK_SHIFTX: {
        /* b_shift_x: asr #n (logical, as xemu) or asl #n, 1 <= n <= 55 */
        int v, sr;
        uint32_t n = op->cc;
        v = e_get(j, V_A + (int)op->r2, T2);
        if (op->space) {
            x_mov(x, 1, T3, v);
            if (n > 1)
                x_shift(x, S_SHR, 1, T3, (int)(n - 1));
            x_alui(x, A_AND, 0, T3, 1);               /* C */
            x_mov(x, 1, T0, v);
            x_shift(x, S_SHR, 1, T0, (int)n);
        } else {
            x_mov(x, 1, T0, v);
            x_shift(x, S_SHL, 1, T0, (int)n);         /* sh (64-bit) */
            x_mov(x, 1, T1, v);
            x_shift(x, S_SHR, 1, T1, (int)(56 - n));  /* the bits shifted out */
            x_mov(x, 0, T3, T1);
            x_alui(x, A_AND, 0, T3, 1);               /* C */
            x_test(x, 1, T1, T1);
            x_setcc(x, CC_NE, T1);
            x_movzx8(x, T1, T1);
            x_shift(x, S_SHL, 0, T1, DSP_SR_L);
            x_alu(x, A_OR, 0, T3, T1);                /* L */
            x_mov(x, 1, T1, v);
            x_alu(x, A_XOR, 1, T1, T0);
            x_shift(x, S_SHR, 1, T1, 55);
            x_alui(x, A_AND, 0, T1, 1);
            x_shift(x, S_SHL, 0, T1, DSP_SR_V);
            x_alu(x, A_OR, 0, T3, T1);                /* V */
            e_mask56(j, T0);
        }
        e_set(j, V_A + (int)op->r1, T0);
        sr = e_get(j, V_SR, T4);
        x_alui(x, A_AND, 0, sr, (int32_t)(0xFFFFu - (SR_C | SR_V)));
        x_alu(x, A_OR, 0, sr, T3);
        e_pend_ccr(j, T0);
        e_set(j, V_SR, sr);
        return;
    }
    case JK_MOVEDISP: {
        /* a = (R + imm) & 0xFFFFFF */
        uint32_t n = op->r1;
        int r = e_get(j, V_R0 + (op->r2 - DSP_REG_R0), T0);
        x_mov(x, 0, X_R10, r);
        x_alui(x, A_ADD, 0, X_R10, (int32_t)op->imm);
        x_alui(x, A_AND, 0, X_R10, 0xFFFFFF);
        if (!(op->cc & 1)) {
            e_move_from_reg(j, X_R9, n);
            e_wr_dyn(j, op->space, X_R10, X_R9, si);
        } else {
            e_rd_dyn(j, T2, op->space, X_R10);
            /* b_move_disp: A/B set_acc; below LCSAVE: masked; (others: not JIT'd) */
            e_move_to_reg(j, n, T2, op->mask);
        }
        return;
    }
    default: {
        /* any other body: the C, on the registers in memory */
        e_flush_ccr(j);
        e_store_held(j);
        x_mov(x, 1, X_RCX, X_RBX);
        x_movi64(x, X_RDX, (uint64_t)(uintptr_t)op);
        x_call(x, (const void *)op->body);
        x_store(x, 0, X_RSP, SLOT(0), X_RAX);
        /* A2 or B2 wide now (max): out, without storing the held copies */
        x_load(x, 0, X_RAX, X_RBX, OFF_REG(DSP_REG_A2));
        x_alu_m(x, A_OR, 0, X_RAX, X_RBX, OFF_REG(DSP_REG_B2));
        x_alui(x, A_CMP, 0, X_RAX, 0xFF);
        j->bails[j->nbails] = x_jcc(x, CC_A);
        j->bail_index[j->nbails++] = si;
        e_load_held(j);
        x_load(x, 0, X_RAX, X_RSP, SLOT(0));
        x_test(x, 0, X_RAX, X_RAX);
        j->stops[j->nstops] = x_jcc(x, CC_NE);
        j->stop_index[j->nstops++] = si;
        return;
    }
    }
}

/* What a compiled block takes as fixed while it runs: the addressing modes
 * (M0-M7), the scaling mode and the loop state (SR, LA), and OMR and the
 * stack's registers. A body that writes one of them isn't compiled. */
static int jit_ctl_write(uint32_t r)
{
    return (r >= DSP_REG_M0 && r <= DSP_REG_M7) || (r >= DSP_REG_SR && r <= DSP_REG_LA);
}

/* Whether a body can be compiled at all (else the block isn't). */
static int jit_body_ok(const FOp *op)
{
    if (!op->body || !op->inblock)
        return 0;
    if (op->body == b_pm2reg || op->body == b_pm3)
        return !jit_ctl_write(op->r2);
    if (op->body == b_move_disp)
        return !((op->cc & 1) && jit_ctl_write(op->r1));
    if (op->w0 >= 0x100000 && pm_kind(op->w0) == PMK_5 && ((op->w0 >> 15) & 1))
        return !jit_ctl_write(op->r1);
    return 1;
}

/* The registers a body uses (for the host-register choice), and the address
 * registers it updates (for the linear-addressing check). */
static void jit_scan(Jc *j, const FOp *op, uint32_t *rmask)
{
    switch (jk_of(op)) {
    case JK_PM2NOP:
        use_alu(j, op);
        break;
    case JK_PM2REG:
        use_reg(j, op->r1);
        use_reg(j, op->r2);
        use_alu(j, op);
        break;
    case JK_PM3:
        use_reg(j, op->r2);
        use_alu(j, op);
        break;
    case JK_PM5: {
        uint32_t mode = pm5_mode(op);
        if (mode < 6 || mode == 7) {
            j->used |= 1u << (V_R0 + (op->ea1 & 7));
            *rmask |= 1u << (op->ea1 & 7);
        }
        use_reg(j, op->r1);
        if (op->r1 == DSP_REG_A || op->r1 == DSP_REG_B)
            j->used |= 1u << V_SR;
        use_alu(j, op);
        break;
    }
    case JK_PM8:
        j->used |= (1u << (V_R0 + (op->ea1 & 7))) | (1u << (V_R0 + (op->ea2 & 7)));
        *rmask |= (1u << (op->ea1 & 7)) | (1u << (op->ea2 & 7));
        use_reg(j, op->r1);
        use_reg(j, op->r2);
        j->used |= 1u << V_SR;
        use_alu(j, op);
        break;
    case JK_ADDX:
    case JK_SUBX:
    case JK_CMPX:
    case JK_LOGICX:
        j->used |= (1u << (V_A + op->r1)) | (1u << V_SR);
        break;
    case JK_SHIFTX:
        j->used |= (1u << (V_A + op->r1)) | (1u << (V_A + op->r2)) | (1u << V_SR);
        break;
    case JK_MOVEDISP:
        j->used |= 1u << (V_R0 + (op->r2 - DSP_REG_R0));
        use_reg(j, op->r1);
        if (op->r1 == DSP_REG_A || op->r1 == DSP_REG_B)
            j->used |= 1u << V_SR;
        break;
    default:
        break;
    }
}

static int body_may_stop(const FOp *op);

/* Whether an inline body writes all of SR's E, U, N and Z. */
static int ccr_full_write(const FOp *op)
{
    int k = jk_of(op);
    if (k == JK_ADDX || k == JK_SUBX || k == JK_CMPX || k == JK_SHIFTX)
        return 1;
    if (k == JK_CALL || k == JK_LOGICX || k == JK_MOVEDISP || k == JK_NOP || !op->alu ||
        !alu_inline(op))
        return 0;
    switch (s_alu[op->w0 & 0xFF].fam) {
    case AF_ADD: case AF_SUB: case AF_CMP: case AF_TST: case AF_MUL: case AF_ABS: case AF_CLR:
        return 1;
    default:
        return 0;
    }
}

/* In a DO loop body, whether E, U, N and Z may stay pending across the back
 * edge: a body writes all four inline before anything reads SR or can stop,
 * so the stale SR an iteration then starts with is never seen. */
static int loop_pend_ok(const FOp *ops, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        const FOp *op = &ops[i];
        int k = jk_of(op);
        if (k == JK_CALL || k == JK_LOGICX || (op->alu && !alu_inline(op)))
            return 0;                                   /* C, or a flush */
        if ((k == JK_PM2REG && op->r1 == DSP_REG_SR) ||
            (k == JK_PM5 && !((op->w0 >> 15) & 1) && op->r1 == DSP_REG_SR) ||
            (k == JK_MOVEDISP && !(op->cc & 1) && op->r1 == DSP_REG_SR))
            return 0;                                   /* a move from SR */
        if (ccr_full_write(op))
            return 1;
        if (body_may_stop(op))
            return 0;
    }
    return 0;
}

/* Loop mode's context (run_loop's locals). */
typedef struct {
    int save;
    uint32_t cc;
    int total;              /* the cycles of one iteration */
    int body_cycles;        /* all but the last */
} JitLoop;

enum { JR_DONE = 0, JR_STOP = 0x1000, JR_LOOP_END = 0x2000, JR_LOOP_BUDGET = 0x3000 };

/* Compiles a block: STRAIGHT (loop 0) runs bodies 0..n-2 and returns 0, or
 * JR_STOP | i after body i wrote a peripheral; LOOP (loop 1) runs whole
 * iterations of all n bodies with run_loop's accounting (ctx: JitLoop) and
 * returns JR_LOOP_END (LC reached 0 after an iteration), JR_LOOP_BUDGET (the
 * next iteration doesn't fit the cycle budget, or the last body wrote a
 * peripheral) or JR_STOP | i (body i < n - 1 wrote a peripheral; save and cc
 * as at that iteration's start). rmask: the address registers it updates,
 * whose M must be 0xFFFF when it runs. NULL when it can't. */
static jit_fn jit_compile(Jit *jt, const FOp *ops, uint32_t n, int loop, uint32_t *rmask_out,
                          const dsp_core_t *c, uint32_t *mode_out)
{
    Jc j;
    uint8_t *start, *top;
    uint8_t *stops[BLK_MAX * 4 + 8];
    uint32_t stop_index[BLK_MAX * 4 + 8];
    uint8_t *bails[BLK_MAX + 8];
    uint32_t bail_index[BLK_MAX + 8];
    ColdRec cold[COLD_MAX];
    uint32_t i, nb = loop ? n : n - 1, rmask = 0, need;
    int k, lpend, pend_end;
    DWORD old;

    if (!nb)
        return NULL;
    for (i = 0; i < nb; i++)
        if (!jit_body_ok(&ops[i]))
            return NULL;
    memset(&j, 0, sizeof j);
    j.stops = stops;
    j.stop_index = stop_index;
    j.bails = bails;
    j.bail_index = bail_index;
    j.cold = cold;
    j.jt = jt;
    for (k = 0; k < V_COUNT; k++)
        j.loc[k] = -1;
    for (i = 0; i < nb; i++)
        jit_scan(&j, &ops[i], &rmask);
    if (!jit_mode_of(c, rmask, mode_out))
        return NULL;
    j.scale = (int)(*mode_out & 3);
    j.bitrev = *mode_out >> 8;
    if (loop)
        j.used |= 1u << V_LC;
    if (j.no_acc[0])
        j.used &= ~(1u << V_A);
    if (j.no_acc[1])
        j.used &= ~(1u << V_B);
    {
        /* the pool in a fixed order of preference: SR, A, B, the R's, X/Y, LC */
        static const int pref[V_COUNT] = { V_SR, V_A, V_B, V_R0, V_R0 + 4, V_R0 + 1, V_R0 + 5,
                                           V_X0, V_Y0, V_X1, V_Y1, V_R0 + 2, V_R0 + 3, V_R0 + 6,
                                           V_R0 + 7, V_LC };
        int p = 0;
        for (k = 0; k < V_COUNT && p < POOL_N; k++)
            if (j.used & (1u << pref[k]))
                j.loc[pref[k]] = (int8_t)s_pool[p++];
        for (k = 0; k < 4; k++)
            j.spare[k] = p < POOL_N ? (int8_t)s_pool[p++] : -1;
    }

    need = 4096 + nb * 1400;
    if (jt->nfn >= JIT_MAX_FUNCS || jt->used + need > JIT_COLD_START)
        return NULL;
    start = jt->base + jt->used;
    if (!VirtualProtect(start, need, PAGE_READWRITE, &old))
        return NULL;
    j.x.p = start;
    j.x.end = start + need;
    /* prologue (as the unwind info says) */
    x_push(&j.x, X_RBX);
    x_push(&j.x, X_RBP);
    x_push(&j.x, X_RSI);
    x_push(&j.x, X_RDI);
    x_push(&j.x, X_R12);
    x_push(&j.x, X_R13);
    x_push(&j.x, X_R14);
    x_push(&j.x, X_R15);
    x_alui(&j.x, A_SUB, 1, X_RSP, JIT_FRAME);
    x_mov(&j.x, 1, X_RBX, X_RCX);
    x_store(&j.x, 1, X_RSP, SLOT(4), X_RDX);            /* ctx */
    if (jt->fault_after && --jt->fault_after == 0)
        x_call(&j.x, (const void *)jit_fault_probe);
    e_load_held(&j);
    top = j.x.p;
    for (i = 0; i < nb; i++)
        e_body(&j, &ops[i], i);
    lpend = loop && !getenv("BW_DSP_JIT_NOLPEND") && loop_pend_ok(ops, n);
    if (!lpend)
        e_flush_ccr(&j);                /* else at the loop's exits */
    pend_end = j.pend;
    if (loop) {
        /* the iteration's end: num_inst, save, cc; LC - 1; LC 0: the loop's
         * end; else the budget for the next one */
        uint8_t *end0, *budget;
        int lc;
        x_load(&j.x, 1, X_RDX, X_RSP, SLOT(4));
        {
            /* num_inst += total */
            x_load(&j.x, 0, T0, X_RDX, (int32_t)offsetof(JitLoop, total));
            x_alu_m(&j.x, A_ADD, 0, T0, X_RBX, OFF_NUMINST);
            x_store(&j.x, 0, X_RBX, OFF_NUMINST, T0);
            /* save -= total; cc += total */
            x_load(&j.x, 0, T0, X_RDX, (int32_t)offsetof(JitLoop, total));
            x_load(&j.x, 0, T1, X_RDX, (int32_t)offsetof(JitLoop, save));
            x_alu(&j.x, A_SUB, 0, T1, T0);
            x_store(&j.x, 0, X_RDX, (int32_t)offsetof(JitLoop, save), T1);
            x_alu_m(&j.x, A_ADD, 0, T0, X_RDX, (int32_t)offsetof(JitLoop, cc));
            x_store(&j.x, 0, X_RDX, (int32_t)offsetof(JitLoop, cc), T0);
        }
        lc = e_get(&j, V_LC, T3);
        x_alui(&j.x, A_SUB, 0, lc, 1);
        x_movzx16(&j.x, lc, lc);
        e_set(&j, V_LC, lc);
        x_test(&j.x, 0, lc, lc);
        end0 = x_jcc(&j.x, CC_E);
        /* next iteration fits: save - body_cycles > 0 */
        x_alu_m(&j.x, A_SUB, 0, T1, X_RDX, (int32_t)offsetof(JitLoop, body_cycles));
        budget = x_jcc(&j.x, CC_LE);
        {
            uint8_t *back = x_jmp(&j.x);
            x_patch(back, top);
        }
        x_patch(end0, j.x.p);
        j.pend = pend_end;
        e_flush_ccr(&j);
        e_store_held(&j);
        x_movi(&j.x, X_RAX, JR_LOOP_END);
        j.exit_jumps[0] = x_jmp(&j.x);
        x_patch(budget, j.x.p);
        j.pend = pend_end;
        e_flush_ccr(&j);
        e_store_held(&j);
        x_movi(&j.x, X_RAX, JR_LOOP_BUDGET);
        j.exit_jumps[1] = x_jmp(&j.x);
    } else {
        e_store_held(&j);
        x_movi(&j.x, X_RAX, JR_DONE);
        j.exit_jumps[0] = x_jmp(&j.x);
    }
    e_cold(&j);
    /* the stop exits: after body i wrote a peripheral. In loop mode the last
     * body's stop runs the iteration's end first (as run_loop does). */
    for (i = 0; i < j.nstops; i++) {
        x_patch(j.stops[i], j.x.p);
        if (j.stop_index[i] & STOP_FLUSH) {
            j.pend = 1;
            e_flush_ccr(&j);
        }
        e_store_held(&j);
        x_movi(&j.x, X_RAX, JR_STOP | (j.stop_index[i] & 0xFFF));
        j.stops[i] = x_jmp(&j.x);
    }
    for (i = 0; i < j.nbails; i++) {
        x_patch(j.bails[i], j.x.p);
        x_movi(&j.x, X_RAX, JR_STOP | j.bail_index[i]);
        j.bails[i] = x_jmp(&j.x);
    }
    /* the epilogue */
    {
        uint8_t *epi = j.x.p;
        for (i = 0; i < j.nbails; i++)
            x_patch(j.bails[i], epi);
        if (j.exit_jumps[0])
            x_patch(j.exit_jumps[0], epi);
        if (j.exit_jumps[1])
            x_patch(j.exit_jumps[1], epi);
        for (i = 0; i < j.nstops; i++)
            x_patch(j.stops[i], epi);
        x_alui(&j.x, A_ADD, 1, X_RSP, JIT_FRAME);
        x_pop(&j.x, X_R15);
        x_pop(&j.x, X_R14);
        x_pop(&j.x, X_R13);
        x_pop(&j.x, X_R12);
        x_pop(&j.x, X_RDI);
        x_pop(&j.x, X_RSI);
        x_pop(&j.x, X_RBP);
        x_pop(&j.x, X_RBX);
        x_ret(&j.x);
    }
    VirtualProtect(start, need, PAGE_EXECUTE_READ, &old);
    if (j.x.bad)
        return NULL;
    FlushInstructionCache(GetCurrentProcess(), start, (SIZE_T)(j.x.p - start));
    jt->rtf[jt->nfn].BeginAddress = (DWORD)(start - jt->base);
    jt->rtf[jt->nfn].EndAddress = (DWORD)(j.x.p - jt->base);
    jt->rtf[jt->nfn].UnwindData = 0;
    jt->nfn++;
    jt->used = (uint32_t)(j.x.p - jt->base + 15) & ~15u;
    *rmask_out = rmask;
    return (jit_fn)(void *)start;
}

/* ---- block linking ------------------------------------------------------------
 *
 * Blocks compiled as fragments that run one after another without returning to
 * C: one call to the trampoline sets up a frame (the same prologue as every
 * compiled function) and jumps to the first block's fragment; each fragment
 * checks, natively, the same conditions the frame loop checks before it runs
 * a block, runs its bodies, then its last instruction through that
 * instruction's own C handler (so every kind can end a fragment), does the
 * frame loop's accounting, and jumps to the fragment of the block at the new
 * pc through a table (ctab, one entry per P address, reset whenever its block
 * is rebuilt or dropped). Anything a fragment can't take (a condition that
 * fails, a 1000-cycle chunk ending, a halt, a REP or interrupt, a pc out of
 * range, a block not compiled) returns to C, at an instruction boundary,
 * which carries on exactly as the frame loop would. */

typedef struct {
    int save;
    uint32_t cc;
    uint32_t code;              /* JC_*, or JR_STOP | i */
    uint32_t block_pc;          /* a stop: the block's start */
    void *entry;                /* the fragment to start at */
} JitRun;

enum { JC_DISPATCH = 0x4000, JC_CHUNK = 0x5000, JR_LSTOP = 0x6000 };

/* A loop fragment's loop ended (LC reached 0 after an iteration): run_loop's
 * lc == 0 path, then the transient fields of the loop's last instruction. */
static void jit_loop_end_c(dsp_core_t *c, const FOp *last, uint32_t end_pc)
{
    uint32_t saved_pc, saved_sr;
    c->pc = end_pc;
    dsp_stack_pop(c, &saved_pc, &saved_sr);
    c->registers[DSP_REG_SR] &= 0x7f;
    c->registers[DSP_REG_SR] |= saved_sr & BWF_LF;
    dsp_stack_pop(c, &c->registers[DSP_REG_LA], &c->registers[DSP_REG_LC]);
    if (c->interrupt_counter)
        dsp_postexecute_interrupts(c);
    c->cur_inst = last->w0;
    c->cur_inst_len = last->len;
    c->instr_cycle = last->cyc;
}

/* A loop fragment's next iteration doesn't fit the chunk: back at the loop's
 * start (SSH) with the last instruction's transient fields. */
static void jit_loop_budget_c(dsp_core_t *c, const FOp *last)
{
    c->pc = c->registers[DSP_REG_SSH];
    c->cur_inst = last->w0;
    c->cur_inst_len = last->len;
    c->instr_cycle = last->cyc;
}

/* jmp [base + disp] */
static void x_jmp_m(Xe *x, int base, int32_t disp)
{
    xrex(x, 0, 0, 0, base, 0);
    xb(x, 0xFF);
    xmod_m(x, 4, base, disp);
}

/* jmp [base + index * 8] */
static void x_jmp_tab(Xe *x, int base, int index)
{
    xrex(x, 0, 0, index, base, 0);
    xb(x, 0xFF);
    xmod_sib(x, 4, base, index, 8, 0);
}

/* movzx dst32, byte [base + disp] */
static void x_load8(Xe *x, int dst, int base, int32_t disp)
{
    xrex(x, 0, dst, 0, base, 0);
    xb(x, 0x0F);
    xb(x, 0xB6);
    xmod_m(x, dst, base, disp);
}

/* movzx dst32, word [base + disp] */
static void x_load16(Xe *x, int dst, int base, int32_t disp)
{
    xrex(x, 0, dst, 0, base, 0);
    xb(x, 0x0F);
    xb(x, 0xB7);
    xmod_m(x, dst, base, disp);
}

#define RUN_OFF(field) ((int32_t)offsetof(JitRun, field))

/* Builds the trampoline at the region's start (after the unwind info): the
 * prologue, then jmp [run->entry]; the shared exits; the epilogue. */
static int jit_build_tramp(Jit *jt)
{
    Xe x;
    uint8_t *start = jt->base + jt->used, *epi_jmp;
    DWORD old;
    if (!VirtualProtect(start, 512, PAGE_READWRITE, &old))
        return 0;
    x.p = start;
    x.end = start + 512;
    x.bad = 0;
    x_push(&x, X_RBX);
    x_push(&x, X_RBP);
    x_push(&x, X_RSI);
    x_push(&x, X_RDI);
    x_push(&x, X_R12);
    x_push(&x, X_R13);
    x_push(&x, X_R14);
    x_push(&x, X_R15);
    x_alui(&x, A_SUB, 1, X_RSP, JIT_FRAME);
    x_mov(&x, 1, X_RBX, X_RCX);
    x_store(&x, 1, X_RSP, SLOT(4), X_RDX);
    x_jmp_m(&x, X_RDX, RUN_OFF(entry));
    /* the dispatch exit: a fragment found nothing to run at c->pc */
    jt->exit_dispatch = x.p;
    x_load(&x, 1, X_RDX, X_RSP, SLOT(4));
    x_store_i(&x, X_RDX, RUN_OFF(code), JC_DISPATCH);
    epi_jmp = x_jmp(&x);
    /* the epilogue: eax = run->code */
    jt->epilogue = x.p;
    x_patch(epi_jmp, x.p);
    x_load(&x, 1, X_RDX, X_RSP, SLOT(4));
    x_load(&x, 0, X_RAX, X_RDX, RUN_OFF(code));
    x_alui(&x, A_ADD, 1, X_RSP, JIT_FRAME);
    x_pop(&x, X_R15);
    x_pop(&x, X_R14);
    x_pop(&x, X_R13);
    x_pop(&x, X_R12);
    x_pop(&x, X_RDI);
    x_pop(&x, X_RSI);
    x_pop(&x, X_RBP);
    x_pop(&x, X_RBX);
    x_ret(&x);
    VirtualProtect(start, 512, PAGE_EXECUTE_READ, &old);
    if (x.bad)
        return 0;
    FlushInstructionCache(GetCurrentProcess(), start, (SIZE_T)(x.p - start));
    jt->rtf[jt->nfn].BeginAddress = (DWORD)(start - jt->base);
    jt->rtf[jt->nfn].EndAddress = (DWORD)(x.p - jt->base);
    jt->rtf[jt->nfn].UnwindData = 0;
    jt->nfn++;
    jt->tramp = (jit_fn)(void *)start;
    jt->used = (uint32_t)(x.p - jt->base + 15) & ~15u;
    {
        /* the flag flush, shared by the fragments: entered by a jump with the
         * way back in rdx, in their frame (the body's unwind info) */
        Jc fj;
        uint8_t *fs = jt->base + jt->used;
        int mem, up;
        if (!VirtualProtect(fs, 1024, PAGE_READWRITE, &old))
            return 0;
        memset(&fj, 0, sizeof fj);
        fj.x.p = fs;
        fj.x.end = fs + 1024;
        for (mem = 0; mem < 2; mem++)
            for (up = 0; up < 2; up++) {
                jt->flush[mem][up] = fj.x.p;
                fj.scale = up ? 2 : 0;
                x_load(&fj.x, 1, X_RAX, X_RSP, SLOT_PR);
                if (mem) {
                    x_load(&fj.x, 0, X_R9, X_RBX, OFF_REG(DSP_REG_SR));
                    e_ccr(&fj, X_R9, X_RAX, X_RCX);
                    x_store(&fj.x, 0, X_RBX, OFF_REG(DSP_REG_SR), X_R9);
                } else {
                    e_ccr(&fj, X_RSI, X_RAX, X_RCX);
                }
                x_jmp_r(&fj.x, X_RDX);
            }
        VirtualProtect(fs, 1024, PAGE_EXECUTE_READ, &old);
        if (fj.x.bad)
            return 0;
        FlushInstructionCache(GetCurrentProcess(), fs, (SIZE_T)(fj.x.p - fs));
        jt->rtf[jt->nfn].BeginAddress = (DWORD)(fs - jt->base);
        jt->rtf[jt->nfn].EndAddress = (DWORD)(fj.x.p - jt->base);
        jt->rtf[jt->nfn].UnwindData = 32;
        jt->nfn++;
        jt->used = (uint32_t)(fj.x.p - jt->base + 15) & ~15u;
    }
    jt->base_used = jt->used;
    jt->base_nfn = jt->nfn;
    return 1;
}

/* A jump to the dispatch exit (c->pc is where C carries on). */
static void e_to_dispatch(Jc *j, Jit *jt)
{
    uint8_t *jmp = x_jmp(&j->x);
    x_patch(jmp, jt->exit_dispatch);
}

/* Compiles block B (ops[0..n-1], starting at `start`, its last at `last`) as a
 * fragment. gen_addr / fgen_addr: the table's and the engine's P generation
 * (after calling C the fragment uses the table only while they agree);
 * body_cycles: the cycles of all but the last; ctab: the table it dispatches
 * through. Returns the entry. */
static uint64_t *s_frag_counter;      /* a bench counter (fragments that ran) */

/* fin's DO loop end, from a native terminator: loop_end, then the interrupt
 * pipeline if something is pending (as fin). */
static void jit_fin_end(dsp_core_t *c)
{
    loop_end(c);
    if (c->interrupt_counter)
        dsp_postexecute_interrupts(c);
}

/* Whether a body can stop its block (a peripheral write, a call into C). */
static int body_may_stop(const FOp *op)
{
    switch (jk_of(op)) {
    case JK_CALL:
        return 1;
    case JK_PM5:
        return !((op->w0 >> 15) & 1) && ((op->w0 >> 19) & 1) == DSP_SPACE_X;
    case JK_PM8:
        return !(op->w0 & (1 << 15));
    case JK_MOVEDISP:
        return !(op->cc & 1) && op->space == DSP_SPACE_X;
    default:
        return 0;
    }
}

/* A block's last instruction done natively: 1 h_branch (bra, bcc), 2
 * h_brbit_reg on a register other than A or B, 3 a body (h_body) that can't
 * stop, 4 do #n / 5 dor #n (emu_do_imm, emu_dor_imm), 6 bsr, 7 rts (the stack
 * natively while it can't overflow or underflow, else the handler); 0: its
 * handler is called. */
static int term_kind(const FOp *t)
{
    if (t->fn == h_branch)
        return 1;
    if (t->fn == h_brbit_reg && t->r1 != DSP_REG_A && t->r1 != DSP_REG_B)
        return 2;
    if (t->fn == h_body && jit_body_ok(t) && !body_may_stop(t))
        return 3;
    if (t->fn == h_light && t->emu == emu_do_imm && t->deps == 2)
        return 4;
    if (t->fn == h_light && t->emu == emu_dor_imm && t->deps == 2)
        return 5;
    if (t->fn == h_bsr)
        return 6;
    if (t->fn == h_rts)
        return 7;
    return 0;
}

static void e_fin_path(Jc *j, Jit *jt, uint32_t w0, uint32_t len, uint32_t cyc, uint32_t newpc,
                       int body_cycles, void *const *ctab, uint8_t **to_acct);

/* A fragment's stop exits (the state after body i, the held registers
 * stored): each site loads its code, one shared tail stores the registers. */
static void e_stop_exits(Jc *j, Jit *jt, uint32_t code_base, uint32_t start)
{
    Xe *x = &j->x;
    uint32_t i, nf = 0;
    uint8_t *to_common[BLK_MAX * 4 + 8];
    int flush[BLK_MAX * 4 + 8];
    if (!j->nstops)
        return;
    for (i = 0; i < j->nstops; i++) {
        x_patch(j->stops[i], x->p);
        x_movi(x, X_R10, code_base | (j->stop_index[i] & 0xFFF));
        to_common[i] = x_jmp(x);
        flush[i] = (j->stop_index[i] & STOP_FLUSH) != 0;
        nf += (uint32_t)flush[i];
    }
    if (nf) {
        /* the pending flags (T0, T1, T4, T6: r10 stays) */
        for (i = 0; i < j->nstops; i++)
            if (flush[i])
                x_patch(to_common[i], x->p);
        j->pend = 1;
        e_flush_ccr(j);
    }
    for (i = 0; i < j->nstops; i++)
        if (!flush[i])
            x_patch(to_common[i], x->p);
    e_store_held(j);                    /* (T6 only: r10 stays) */
    x_load(x, 1, X_RDX, X_RSP, SLOT(4));
    x_store(x, 0, X_RDX, RUN_OFF(code), X_R10);
    x_store_i(x, X_RDX, RUN_OFF(block_pc), start);
    x_patch(x_jmp(x), jt->epilogue);
}

#define OFF_SSTACK(k) ((int32_t)(offsetof(dsp_core_t, stack) + 64 * (k)))

/* The stack's depth check for a native push or pop: SP's error bits clear
 * and room for it (SP & 15 at most `hi`, at least `lo`); else to *slow. eax:
 * SP & 15 after it. */
static void e_sp_check(Jc *j, int lo, int hi, uint8_t **slow, int *nslow)
{
    Xe *x = &j->x;
    x_load(x, 0, X_RAX, X_RBX, OFF_REG(DSP_REG_SP));
    x_testi(x, 0, X_RAX, 0x30);
    slow[(*nslow)++] = x_jcc(x, CC_NE);
    x_alui(x, A_AND, 0, X_RAX, 15);
    x_alui(x, A_CMP, 0, X_RAX, hi);
    slow[(*nslow)++] = x_jcc(x, CC_A);
    if (lo > 0) {
        x_alui(x, A_CMP, 0, X_RAX, lo);
        slow[(*nslow)++] = x_jcc(x, CC_B);
    }
}

/* dsp_stack_push(pc, sr) with no overflow: eax is SP & 15 (incremented);
 * pc in ecx, sr in edx (both & 0xFFFF here). SSH, SSL follow; SP stored. */
static void e_push(Jc *j)
{
    Xe *x = &j->x;
    x_alui(x, A_ADD, 0, X_RAX, 1);
    x_alui(x, A_AND, 0, X_RCX, 0xFFFF);
    x_alui(x, A_AND, 0, X_RDX, 0xFFFF);
    x_store_idx(x, X_RBX, X_RAX, 4, OFF_SSTACK(0), X_RCX);
    x_store_idx(x, X_RBX, X_RAX, 4, OFF_SSTACK(1), X_RDX);
    x_store(x, 0, X_RBX, OFF_REG(DSP_REG_SSH), X_RCX);
    x_store(x, 0, X_RBX, OFF_REG(DSP_REG_SSL), X_RDX);
    x_store(x, 0, X_RBX, OFF_REG(DSP_REG_SP), X_RAX);
}

/* A native tail with a run-time pc (in ecx): as e_fin_path, then the table. */
static void e_fin_dyn(Jc *j, Jit *jt, uint32_t w0, uint32_t len, uint32_t cyc, int body_cycles,
                      void *const *ctab, uint8_t **to_acct)
{
    Xe *x = &j->x;
    uint8_t *no_end1, *no_end2, *more, *in;
    int total = body_cycles + (int)cyc;
    x_store(x, 0, X_RBX, (int32_t)offsetof(dsp_core_t, pc), X_RCX);
    x_store_i(x, X_RBX, (int32_t)offsetof(dsp_core_t, cur_inst), w0);
    x_store_i(x, X_RBX, (int32_t)offsetof(dsp_core_t, cur_inst_len), len);
    x_store16_i(x, X_RBX, (int32_t)offsetof(dsp_core_t, instr_cycle), cyc);
    x_alui_m(x, A_ADD, X_RBX, OFF_NUMINST, total);
    x_load(x, 0, X_RAX, X_RBX, OFF_REG(DSP_REG_SR));
    x_testi(x, 0, X_RAX, BWF_LF);
    no_end1 = x_jcc(x, CC_E);
    x_load(x, 0, X_RAX, X_RBX, OFF_REG(DSP_REG_LA));
    x_alui(x, A_ADD, 0, X_RAX, 1);
    x_alu(x, A_CMP, 0, X_RAX, X_RCX);
    no_end2 = x_jcc(x, CC_NE);
    x_mov(x, 1, X_RCX, X_RBX);
    x_call(x, (const void *)jit_fin_end);
    *to_acct = x_jmp(x);
    x_patch(no_end1, x->p);
    x_patch(no_end2, x->p);
    x_load(x, 1, X_RDX, X_RSP, SLOT(4));
    x_load(x, 0, X_RAX, X_RDX, RUN_OFF(save));
    x_alui(x, A_SUB, 0, X_RAX, total);
    x_store(x, 0, X_RDX, RUN_OFF(save), X_RAX);
    x_alui_m(x, A_ADD, X_RDX, RUN_OFF(cc), total);
    x_test(x, 0, X_RAX, X_RAX);
    more = x_jcc(x, CC_G);
    x_store_i(x, X_RDX, RUN_OFF(code), JC_CHUNK);           /* a chunk ended */
    x_patch(x_jmp(x), jt->epilogue);
    x_patch(more, x->p);
    x_alui(x, A_CMP, 0, X_RCX, BWF_PSIZE);
    in = x_jcc(x, CC_B);
    e_to_dispatch(j, jt);
    x_patch(in, x->p);
    x_movi64(x, X_RAX, (uint64_t)(uintptr_t)ctab);
    x_jmp_tab(x, X_RAX, X_RCX);
}

/* The stack instructions natively (term_kind 4-7), from the registers in
 * memory; jumps to the handler's path (slow) when the stack could overflow
 * or underflow. */
static int e_stack_term(Jc *j, Jit *jt, const FOp *t, int nt, uint32_t last, int body_cycles,
                        void *const *ctab, uint8_t **slow, uint8_t **to_acct, int *nacct)
{
    Xe *x = &j->x;
    int nslow = 0;
    if (nt == 4 || nt == 5) {
        /* emu_do_imm / emu_dor_imm: push LA, LC; LA; push pc + 2, SR; LF; LC */
        uint32_t la = nt == 4 ? (t->w1 & 0xFFFF) : ((last + t->w1) & 0xFFFF);
        uint32_t lc = ((t->w0 >> 8) & 0xFF) + ((t->w0 & 0xF) << 8);
        e_sp_check(j, 0, 13, slow, &nslow);
        x_load(x, 0, X_RCX, X_RBX, OFF_REG(DSP_REG_LA));
        x_load(x, 0, X_RDX, X_RBX, OFF_REG(DSP_REG_LC));
        e_push(j);
        x_store_i(x, X_RBX, OFF_REG(DSP_REG_LA), la);
        x_movi(x, X_RCX, last + 2);
        x_load(x, 0, X_RDX, X_RBX, OFF_REG(DSP_REG_SR));
        e_push(j);
        x_alui_m(x, A_OR, X_RBX, OFF_REG(DSP_REG_SR), (int32_t)BWF_LF);
        x_store_i(x, X_RBX, OFF_REG(DSP_REG_LC), lc);
        e_fin_path(j, jt, t->w0, 2, 6, last + 2, body_cycles, ctab, &to_acct[(*nacct)++]);
    } else if (nt == 6) {
        /* h_bsr: push pc + len, SR; branch */
        e_sp_check(j, 0, 14, slow, &nslow);
        x_movi(x, X_RCX, last + t->len);
        x_load(x, 0, X_RDX, X_RBX, OFF_REG(DSP_REG_SR));
        e_push(j);
        e_fin_path(j, jt, t->w0, 0, t->cyc, (last + t->imm) & 0xFFFFFF, body_cycles, ctab,
                   &to_acct[(*nacct)++]);
    } else {
        /* h_rts: pop the PC */
        e_sp_check(j, 1, 15, slow, &nslow);
        x_alui(x, A_SUB, 0, X_RAX, 1);
        x_load(x, 0, X_RCX, X_RBX, OFF_REG(DSP_REG_SSH));       /* the new pc */
        x_load_idx(x, X_RDX, X_RBX, X_RAX, 4, OFF_SSTACK(0));
        x_store(x, 0, X_RBX, OFF_REG(DSP_REG_SSH), X_RDX);
        x_load_idx(x, X_RDX, X_RBX, X_RAX, 4, OFF_SSTACK(1));
        x_store(x, 0, X_RBX, OFF_REG(DSP_REG_SSL), X_RDX);
        x_store(x, 0, X_RBX, OFF_REG(DSP_REG_SP), X_RAX);
        e_fin_dyn(j, jt, t->w0, 0, 4, body_cycles, ctab, &to_acct[(*nacct)++]);
    }
    return nslow;
}

/* The DO loop's end natively (jit_loop_end_c while the stack can't
 * underflow): two pops (LA, LC), SR's mode bits cleared but LF from the
 * pop, pc the loop's end. Jumps to *slow otherwise. */
static int e_loop_end_native(Jc *j, const FOp *t, uint32_t end_pc, uint8_t **slow)
{
    Xe *x = &j->x;
    int nslow = 0;
    e_sp_check(j, 2, 15, slow, &nslow);
    x_alui(x, A_SUB, 0, X_RAX, 2);                          /* s2 */
    x_load(x, 0, X_RCX, X_RBX, OFF_REG(DSP_REG_SSL));       /* the first pop's SR */
    x_alui(x, A_AND, 0, X_RCX, (int32_t)BWF_LF);
    x_load(x, 0, X_RDX, X_RBX, OFF_REG(DSP_REG_SR));
    x_alui(x, A_AND, 0, X_RDX, 0x7F);
    x_alu(x, A_OR, 0, X_RDX, X_RCX);
    x_store(x, 0, X_RBX, OFF_REG(DSP_REG_SR), X_RDX);
    x_load_idx(x, X_RCX, X_RBX, X_RAX, 4, OFF_SSTACK(0) + 4);   /* stack[0][s1] */
    x_store(x, 0, X_RBX, OFF_REG(DSP_REG_LA), X_RCX);
    x_load_idx(x, X_RCX, X_RBX, X_RAX, 4, OFF_SSTACK(1) + 4);
    x_store(x, 0, X_RBX, OFF_REG(DSP_REG_LC), X_RCX);
    x_load_idx(x, X_RCX, X_RBX, X_RAX, 4, OFF_SSTACK(0));
    x_store(x, 0, X_RBX, OFF_REG(DSP_REG_SSH), X_RCX);
    x_load_idx(x, X_RCX, X_RBX, X_RAX, 4, OFF_SSTACK(1));
    x_store(x, 0, X_RBX, OFF_REG(DSP_REG_SSL), X_RCX);
    x_store(x, 0, X_RBX, OFF_REG(DSP_REG_SP), X_RAX);
    x_store_i(x, X_RBX, (int32_t)offsetof(dsp_core_t, pc), end_pc);
    x_store_i(x, X_RBX, (int32_t)offsetof(dsp_core_t, cur_inst), t->w0);
    x_store_i(x, X_RBX, (int32_t)offsetof(dsp_core_t, cur_inst_len), t->len);
    x_store16_i(x, X_RBX, (int32_t)offsetof(dsp_core_t, instr_cycle), t->cyc);
    return nslow;
}

/* emu_calc_cc's condition cc (0-15) from SR in memory: the jump taken when
 * it is false. Uses rax, rcx, rdx. */
static uint8_t *e_cc_false(Jc *j, uint32_t cc)
{
    Xe *x = &j->x;
    x_load(x, 0, X_RAX, X_RBX, OFF_REG(DSP_REG_SR));
    switch (cc & 7) {
    case 0:                                             /* C */
        break;
    case 1:                                             /* N ^ V */
        x_mov(x, 0, X_RCX, X_RAX);
        x_shift(x, S_SHR, 0, X_RCX, 2);
        x_alu(x, A_XOR, 0, X_RAX, X_RCX);
        x_shift(x, S_SHR, 0, X_RAX, 1);
        break;
    case 2:                                             /* Z */
        x_shift(x, S_SHR, 0, X_RAX, 2);
        break;
    case 3:                                             /* N */
        x_shift(x, S_SHR, 0, X_RAX, 3);
        break;
    case 4:                                             /* Z | (~U & ~E) */
        x_mov(x, 0, X_RCX, X_RAX);
        x_not(x, 0, X_RCX);
        x_mov(x, 0, X_RDX, X_RCX);
        x_shift(x, S_SHR, 0, X_RCX, 4);
        x_shift(x, S_SHR, 0, X_RDX, 5);
        x_alu(x, A_AND, 0, X_RCX, X_RDX);
        x_shift(x, S_SHR, 0, X_RAX, 2);
        x_alu(x, A_OR, 0, X_RAX, X_RCX);
        break;
    case 5:                                             /* E */
        x_shift(x, S_SHR, 0, X_RAX, 5);
        break;
    case 6:                                             /* L */
        x_shift(x, S_SHR, 0, X_RAX, 6);
        break;
    default:                                            /* Z | (N ^ V) */
        x_mov(x, 0, X_RCX, X_RAX);
        x_shift(x, S_SHR, 0, X_RCX, 2);
        x_alu(x, A_XOR, 0, X_RCX, X_RAX);
        x_shift(x, S_SHR, 0, X_RCX, 1);
        x_shift(x, S_SHR, 0, X_RAX, 2);
        x_alu(x, A_OR, 0, X_RAX, X_RCX);
        break;
    }
    x_alui(x, A_AND, 0, X_RAX, 1);
    /* 0-7 true when the term is 0; 8-15 when it is 1 */
    return x_jcc(x, cc < 8 ? CC_NE : CC_E);
}

/* A native terminator's tail (fin): the transient fields, pc, num_inst; a DO
 * loop's end (LF and pc == LA + 1) goes through C and on to the generic
 * accounting (*to_acct); else the accounting with the cycles known and a jump
 * straight to the next fragment. */
static void e_fin_path(Jc *j, Jit *jt, uint32_t w0, uint32_t len, uint32_t cyc, uint32_t newpc,
                       int body_cycles, void *const *ctab, uint8_t **to_acct)
{
    Xe *x = &j->x;
    uint8_t *no_end1, *no_end2, *more;
    int total = body_cycles + (int)cyc;
    x_store_i(x, X_RBX, (int32_t)offsetof(dsp_core_t, cur_inst), w0);
    x_store_i(x, X_RBX, (int32_t)offsetof(dsp_core_t, cur_inst_len), len);
    x_store16_i(x, X_RBX, (int32_t)offsetof(dsp_core_t, instr_cycle), cyc);
    x_store_i(x, X_RBX, (int32_t)offsetof(dsp_core_t, pc), newpc);
    x_alui_m(x, A_ADD, X_RBX, OFF_NUMINST, total);
    x_load(x, 0, X_RAX, X_RBX, OFF_REG(DSP_REG_SR));
    x_testi(x, 0, X_RAX, BWF_LF);
    no_end1 = x_jcc(x, CC_E);
    x_load(x, 0, X_RCX, X_RBX, OFF_REG(DSP_REG_LA));
    x_alui(x, A_ADD, 0, X_RCX, 1);
    x_alui(x, A_CMP, 0, X_RCX, (int32_t)newpc);
    no_end2 = x_jcc(x, CC_NE);
    x_mov(x, 1, X_RCX, X_RBX);
    x_call(x, (const void *)jit_fin_end);
    *to_acct = x_jmp(x);
    x_patch(no_end1, x->p);
    x_patch(no_end2, x->p);
    x_load(x, 1, X_RDX, X_RSP, SLOT(4));
    x_load(x, 0, X_RCX, X_RDX, RUN_OFF(save));
    x_alui(x, A_SUB, 0, X_RCX, total);
    x_store(x, 0, X_RDX, RUN_OFF(save), X_RCX);
    x_alui_m(x, A_ADD, X_RDX, RUN_OFF(cc), total);
    x_test(x, 0, X_RCX, X_RCX);
    more = x_jcc(x, CC_G);
    x_store_i(x, X_RDX, RUN_OFF(code), JC_CHUNK);           /* a chunk ended */
    x_patch(x_jmp(x), jt->epilogue);
    x_patch(more, x->p);
    if (newpc < BWF_PSIZE) {
        x_movi64(x, X_RAX, (uint64_t)(uintptr_t)&ctab[newpc]);
        x_jmp_m(x, X_RAX, 0);
    } else {
        e_to_dispatch(j, jt);
    }
}

static void *jit_compile_chain(Jit *jt, const FOp *ops, uint32_t n, uint32_t start, uint32_t last,
                               int body_cycles, const uint32_t *gen_addr, const uint32_t *fgen_addr,
                               void *const *ctab, int loop_ok, int total, uint32_t end_pc,
                               const dsp_core_t *c, int paths)
{
    Jc j;
    uint8_t *code, *fail_jumps[48], *to_loop = NULL, *plain_checks;
    uint8_t *stops[BLK_MAX * 4 + 8];
    uint32_t stop_index[BLK_MAX * 4 + 8];
    uint8_t *bails[BLK_MAX + 8];
    uint32_t bail_index[BLK_MAX + 8];
    ColdRec cold[COLD_MAX];
    uint8_t *ccode;
    uint32_t i, nb = n - 1, ns, rmask = 0, need, need_cold, nfail = 0, r;
    const FOp *t = &ops[n - 1];
    int k, loop, nt, straight;
    DWORD old;
    uint8_t *to_acct[4];
    int nacct = 0;

    for (i = 0; i < nb; i++)
        if (!jit_body_ok(&ops[i]))
            return NULL;
    /* paths: 1 the straight path, 2 the loop path (a DO loop body's), as the
     * block has been seen to run: a single-block loop body only as a loop,
     * the code half the size (and an unseen way in goes to C) */
    loop = loop_ok && jit_body_ok(t) && (paths & 2);
    straight = !loop || (paths & 1);
    nt = getenv("BW_DSP_JIT_NOTERM") ? 0 : term_kind(t);
    ns = (loop || (straight && nt == 3)) ? n : nb;  /* the bodies either path runs */
    memset(&j, 0, sizeof j);
    j.stops = stops;
    j.stop_index = stop_index;
    j.bails = bails;
    j.bail_index = bail_index;
    j.cold = cold;
    j.jt = jt;
    for (k = 0; k < V_COUNT; k++)
        j.loc[k] = -1;
    for (i = 0; i < ns; i++)
        jit_scan(&j, &ops[i], &rmask);
    {
        uint32_t mode;
        if (!jit_mode_of(c, rmask, &mode))
            return NULL;
        j.scale = (int)(mode & 3);
        j.bitrev = mode >> 8;
    }
    if (loop)
        j.used |= 1u << V_LC;
    if (j.no_acc[0])
        j.used &= ~(1u << V_A);
    if (j.no_acc[1])
        j.used &= ~(1u << V_B);
    {
        static const int pref[V_COUNT] = { V_SR, V_A, V_B, V_R0, V_R0 + 4, V_R0 + 1, V_R0 + 5,
                                           V_X0, V_Y0, V_X1, V_Y1, V_R0 + 2, V_R0 + 3, V_R0 + 6,
                                           V_R0 + 7, V_LC };
        int p = 0;
        for (k = 0; k < V_COUNT && p < POOL_N; k++)
            if (j.used & (1u << pref[k]))
                j.loc[pref[k]] = (int8_t)s_pool[p++];
        for (k = 0; k < 4; k++)
            j.spare[k] = p < POOL_N ? (int8_t)s_pool[p++] : -1;
    }
    need = 4096 + n * (loop ? 2800 : 1400);
    need_cold = 1024 + n * 800;
    if (jt->nfn >= JIT_MAX_FUNCS || jt->used + need > JIT_COLD_START ||
        jt->cold_used + need_cold > JIT_BYTES)
        return NULL;
    code = jt->base + jt->used;
    ccode = jt->base + jt->cold_used;
    if (!VirtualProtect(code, need, PAGE_READWRITE, &old))
        return NULL;
    if (!VirtualProtect(ccode, need_cold, PAGE_READWRITE, &old)) {
        VirtualProtect(code, need, PAGE_EXECUTE_READ, &old);
        return NULL;
    }
    j.x.p = code;
    j.x.end = code + need;
    j.xc.p = ccode;
    j.xc.end = ccode + need_cold;
    if (jt->fault_cold_after && --jt->fault_cold_after == 0) {
        /* a test aid: a fault in cold code (the hot code jumps there first) */
        uint8_t *go = x_jmp(&j.x), *back = j.x.p;
        x_patch(go, j.xc.p);
        x_call(&j.xc, (const void *)jit_fault_probe);
        x_patch(x_jmp(&j.xc), back);
    }
    memset(jt->sec[jt->nfn], 0, sizeof jt->sec[0]);
    if (jt->fault_after && --jt->fault_after == 0)
        x_call(&j.x, (const void *)jit_fault_probe);       /* a test aid: a fault in a fragment */

    /* -- the frame loop's conditions for this block. P: the table holds a
     * fragment only while its block's words were checked at the current
     * generation (a new one empties it), and a fragment that called C, which
     * may have written P, checks that before using the table again -- */
    x_load(&j.x, 0, X_RAX, X_RBX, OFF_REG(DSP_REG_SR));
    if (ns) {
        if (j.scale) {
            x_mov(&j.x, 0, X_RCX, X_RAX);
            x_alui(&j.x, A_AND, 0, X_RCX, (1u << DSP_SR_S0) | (1u << DSP_SR_S1));
            x_alui(&j.x, A_CMP, 0, X_RCX, j.scale << DSP_SR_S0);
        } else {
            x_testi(&j.x, 0, X_RAX, (1u << DSP_SR_S0) | (1u << DSP_SR_S1));
        }
        fail_jumps[nfail++] = x_jcc(&j.x, CC_NE);             /* the scaling mode */
        x_load(&j.x, 0, X_RCX, X_RBX, OFF_REG(DSP_REG_A2));
        x_alu_m(&j.x, A_OR, 0, X_RCX, X_RBX, OFF_REG(DSP_REG_B2));
        x_alui(&j.x, A_CMP, 0, X_RCX, 0xFF);
        fail_jumps[nfail++] = x_jcc(&j.x, CC_A);              /* A2/B2 narrow */
        for (r = 0; r < 8; r++)
            if (rmask & (1u << r)) {
                x_load16(&j.x, X_RCX, X_RBX, OFF_REG(DSP_REG_M0 + r));
                if (j.bitrev & (1u << r))
                    x_test(&j.x, 0, X_RCX, X_RCX);
                else
                    x_alui(&j.x, A_CMP, 0, X_RCX, 0xFFFF);
                fail_jumps[nfail++] = x_jcc(&j.x, CC_NE);     /* the addressing modes */
            }
    }
    x_load(&j.x, 1, X_RDX, X_RSP, SLOT(4));
    x_load(&j.x, 0, X_RCX, X_RDX, RUN_OFF(save));
    x_alui(&j.x, A_SUB, 0, X_RCX, body_cycles);
    fail_jumps[nfail++] = x_jcc(&j.x, CC_LE);                 /* no chunk end inside */
    /* the DO loop state (eax: SR): run_loop's conditions take the loop path;
     * else a loop that ends inside the block sends it to C */
    if (loop && !straight) {
        /* the loop path only: run_loop's conditions, else C */
        x_testi(&j.x, 0, X_RAX, BWF_LF);
        fail_jumps[nfail++] = x_jcc(&j.x, CC_E);
        x_load(&j.x, 0, X_RCX, X_RBX, OFF_REG(DSP_REG_LA));
        x_alui(&j.x, A_ADD, 0, X_RCX, 1);
        x_alui(&j.x, A_CMP, 0, X_RCX, (int32_t)end_pc);
        fail_jumps[nfail++] = x_jcc(&j.x, CC_NE);
        x_load(&j.x, 0, X_RAX, X_RBX, OFF_REG(DSP_REG_SSH));
        x_alui(&j.x, A_CMP, 0, X_RAX, (int32_t)start);
        fail_jumps[nfail++] = x_jcc(&j.x, CC_NE);
        to_loop = x_jmp(&j.x);
    } else {
        uint8_t *lf_clear, *la_other, *ssh_other, *not_inside;
        x_testi(&j.x, 0, X_RAX, BWF_LF);
        lf_clear = x_jcc(&j.x, CC_E);
        x_load(&j.x, 0, X_RCX, X_RBX, OFF_REG(DSP_REG_LA));
        x_alui(&j.x, A_ADD, 0, X_RCX, 1);
        if (loop) {
            x_alui(&j.x, A_CMP, 0, X_RCX, (int32_t)end_pc);
            la_other = x_jcc(&j.x, CC_NE);
            x_load(&j.x, 0, X_RAX, X_RBX, OFF_REG(DSP_REG_SSH));
            x_alui(&j.x, A_CMP, 0, X_RAX, (int32_t)start);
            to_loop = x_jcc(&j.x, CC_E);
            ssh_other = x_jmp(&j.x);                          /* LA + 1 the end: not inside */
            x_patch(la_other, j.x.p);
        } else {
            ssh_other = NULL;
        }
        x_alui(&j.x, A_CMP, 0, X_RCX, (int32_t)start);
        not_inside = x_jcc(&j.x, CC_BE);
        x_alui(&j.x, A_CMP, 0, X_RCX, (int32_t)last);
        fail_jumps[nfail++] = x_jcc(&j.x, CC_BE);             /* the DO loop ends inside */
        x_patch(not_inside, j.x.p);
        x_patch(lf_clear, j.x.p);
        if (ssh_other)
            x_patch(ssh_other, j.x.p);
    }

    /* -- the straight path: the bodies, then the last instruction's handler -- */
    if (straight) {
    if (s_frag_counter) {
        x_movi64(&j.x, X_RAX, (uint64_t)(uintptr_t)s_frag_counter);
        x_alui_m(&j.x, A_ADD, X_RAX, 0, 1);
    }
    jt->sec[jt->nfn][4] = (uint32_t)(j.x.p - code);
    e_load_held(&j);
    jt->sec[jt->nfn][0] = (uint32_t)(j.x.p - code);
    for (i = 0; i < nb; i++)
        e_body(&j, &ops[i], i);
    if (nt == 3)
        e_body(&j, t, n - 1);           /* the last one's body too (it can't stop) */
    e_flush_ccr(&j);
    jt->sec[jt->nfn][1] = (uint32_t)(j.x.p - code);
    e_store_held(&j);
    jt->sec[jt->nfn][2] = (uint32_t)(j.x.p - code);
    if (nt == 3) {
        e_fin_path(&j, jt, t->w0, t->len, t->cyc, last + t->len, body_cycles, ctab,
                   &to_acct[nacct++]);
    } else if (nt == 1) {
        uint8_t *nottaken = t->cc == 16 ? NULL : e_cc_false(&j, t->cc);
        e_fin_path(&j, jt, t->w0, 0, 2, (last + t->imm) & 0xFFFFFF, body_cycles, ctab,
                   &to_acct[nacct++]);
        if (nottaken) {
            x_patch(nottaken, j.x.p);
            e_fin_path(&j, jt, t->w0, t->len, 2, last + t->len, body_cycles, ctab,
                       &to_acct[nacct++]);
        }
    } else if (nt == 2) {
        uint8_t *nottaken;
        x_load(&j.x, 0, X_RAX, X_RBX, OFF_REG(t->r1));
        if (t->cc)
            x_shift(&j.x, S_SHR, 0, X_RAX, (int)t->cc);
        x_alui(&j.x, A_AND, 0, X_RAX, 1);
        x_alui(&j.x, A_CMP, 0, X_RAX, (int32_t)t->space);
        nottaken = x_jcc(&j.x, CC_NE);
        e_fin_path(&j, jt, t->w0, 0, 6, (last + t->imm) & 0xFFFFFF, body_cycles, ctab,
                   &to_acct[nacct++]);
        x_patch(nottaken, j.x.p);
        e_fin_path(&j, jt, t->w0, 2, 6, last + 2, body_cycles, ctab, &to_acct[nacct++]);
    } else {
        if (nt >= 4) {
            uint8_t *slow[4];
            int ns2 = e_stack_term(&j, jt, t, nt, last, body_cycles, ctab, slow, to_acct, &nacct), q;
            for (q = 0; q < ns2; q++)
                x_patch(slow[q], j.x.p);        /* the stack's edges: the handler */
        }
        if (body_cycles)
            x_alui_m(&j.x, A_ADD, X_RBX, OFF_NUMINST, body_cycles);
        x_store_i(&j.x, X_RBX, (int32_t)offsetof(dsp_core_t, pc), last);
        x_mov(&j.x, 1, X_RCX, X_RBX);
        /* a P write in it moves the generation through the write hook */
        if (t->fn) {
            x_movi64(&j.x, X_RDX, (uint64_t)(uintptr_t)t);
            x_call(&j.x, (const void *)t->fn);
        } else {
            x_call(&j.x, (const void *)dsp56k_execute_instruction);
        }
    }
    for (i = 0; i < (uint32_t)nacct; i++)
        x_patch(to_acct[i], j.x.p);     /* after a DO loop's end in C: as after a call */
    /* -- the frame loop's accounting: cycles = bodies + instr_cycle -- */
    x_load16(&j.x, X_RAX, X_RBX, (int32_t)offsetof(dsp_core_t, instr_cycle));
    if (body_cycles)
        x_alui(&j.x, A_ADD, 0, X_RAX, body_cycles);
    x_load(&j.x, 1, X_RDX, X_RSP, SLOT(4));
    x_load(&j.x, 0, X_RCX, X_RDX, RUN_OFF(save));
    x_alu(&j.x, A_SUB, 0, X_RCX, X_RAX);
    x_store(&j.x, 0, X_RDX, RUN_OFF(save), X_RCX);
    x_alu_m(&j.x, A_ADD, 0, X_RAX, X_RDX, RUN_OFF(cc));
    x_store(&j.x, 0, X_RDX, RUN_OFF(cc), X_RAX);
    x_test(&j.x, 0, X_RCX, X_RCX);
    {
        uint8_t *more = x_jcc(&j.x, CC_G);
        x_store_i(&j.x, X_RDX, RUN_OFF(code), JC_CHUNK);       /* a chunk ended */
        x_patch(x_jmp(&j.x), jt->epilogue);
        x_patch(more, j.x.p);
    }
    }                                   /* (straight) */
    /* a halt, a REP, an interrupt: back to C */
    plain_checks = j.x.p;
    x_load8(&j.x, X_RAX, X_RBX, (int32_t)offsetof(dsp_core_t, is_idle));
    x_load(&j.x, 0, X_RCX, X_RBX, (int32_t)offsetof(dsp_core_t, loop_rep));
    x_alu(&j.x, A_OR, 0, X_RAX, X_RCX);
    x_load16(&j.x, X_RCX, X_RBX, (int32_t)offsetof(dsp_core_t, interrupt_state));
    x_alu(&j.x, A_OR, 0, X_RAX, X_RCX);
    x_load16(&j.x, X_RCX, X_RBX, (int32_t)offsetof(dsp_core_t, interrupt_counter));
    x_alu(&j.x, A_OR, 0, X_RAX, X_RCX);
    {
        uint8_t *plain = x_jcc(&j.x, CC_E);
        e_to_dispatch(&j, jt);
        x_patch(plain, j.x.p);
    }
    /* -- the next block, through the table (C ran: P may be new) -- */
    x_movi64(&j.x, X_RAX, (uint64_t)(uintptr_t)gen_addr);
    x_load(&j.x, 0, X_RAX, X_RAX, 0);
    x_movi64(&j.x, X_RCX, (uint64_t)(uintptr_t)fgen_addr);
    x_alu_m(&j.x, A_CMP, 0, X_RAX, X_RCX, 0);
    {
        uint8_t *same = x_jcc(&j.x, CC_E);
        e_to_dispatch(&j, jt);
        x_patch(same, j.x.p);
    }
    x_load(&j.x, 0, X_RAX, X_RBX, (int32_t)offsetof(dsp_core_t, pc));
    x_alui(&j.x, A_CMP, 0, X_RAX, BWF_PSIZE);
    {
        uint8_t *in = x_jcc(&j.x, CC_B);
        e_to_dispatch(&j, jt);
        x_patch(in, j.x.p);
    }
    x_movi64(&j.x, X_RCX, (uint64_t)(uintptr_t)ctab);
    x_jmp_tab(&j.x, X_RCX, X_RAX);
    /* -- a condition failed: C runs the block -- */
    for (i = 0; i < nfail; i++)
        x_patch(fail_jumps[i], jt->exit_dispatch);
    /* -- the cold code: slow paths, stops and bails (the state after body i) -- */
    cold_swap(&j);
    e_cold(&j);
    e_stop_exits(&j, jt, JR_STOP, start);
    for (i = 0; i < j.nbails; i++) {
        x_patch(j.bails[i], j.x.p);
        x_load(&j.x, 1, X_RDX, X_RSP, SLOT(4));
        x_store_i(&j.x, X_RDX, RUN_OFF(code), JR_STOP | j.bail_index[i]);
        x_store_i(&j.x, X_RDX, RUN_OFF(block_pc), start);
        x_patch(x_jmp(&j.x), jt->epilogue);
    }
    cold_swap(&j);

    /* -- the loop path: iterations as run_loop's; num_inst, save and cc at
     * each iteration's end; LC - 1; at 0 the loop's end (C), else the next
     * iteration if it fits, or back to C at the loop's start -- */
    if (loop) {
        uint8_t *top, *done, *again;
        int lc;
        j.nstops = j.nbails = 0;
        x_patch(to_loop, j.x.p);
        if (s_frag_counter) {
            x_movi64(&j.x, X_RAX, (uint64_t)(uintptr_t)s_frag_counter);
            x_alui_m(&j.x, A_ADD, X_RAX, 0, 1);
        }
        e_load_held(&j);
        int lpend = !getenv("BW_DSP_JIT_NOLPEND") && loop_pend_ok(ops, n), pend_end;
        top = j.x.p;
        jt->sec[jt->nfn][3] = (uint32_t)(j.x.p - code);
        j.pend = 0;
        for (i = 0; i < n; i++)
            e_body(&j, &ops[i], i);
        if (!lpend)
            e_flush_ccr(&j);            /* else at the loop's exits */
        pend_end = j.pend;
        x_alui_m(&j.x, A_ADD, X_RBX, OFF_NUMINST, total);
        x_load(&j.x, 1, X_RDX, X_RSP, SLOT(4));
        x_load(&j.x, 0, X_RCX, X_RDX, RUN_OFF(save));
        x_alui(&j.x, A_SUB, 0, X_RCX, total);
        x_store(&j.x, 0, X_RDX, RUN_OFF(save), X_RCX);
        x_load(&j.x, 0, X_RAX, X_RDX, RUN_OFF(cc));
        x_alui(&j.x, A_ADD, 0, X_RAX, total);
        x_store(&j.x, 0, X_RDX, RUN_OFF(cc), X_RAX);
        lc = e_get(&j, V_LC, T3);
        x_alui(&j.x, A_SUB, 0, lc, 1);
        x_movzx16(&j.x, lc, lc);
        e_set(&j, V_LC, lc);
        x_test(&j.x, 0, lc, lc);
        done = x_jcc(&j.x, CC_E);
        x_alui(&j.x, A_SUB, 0, X_RCX, total - t->cyc);
        again = x_jcc(&j.x, CC_G);
        x_patch(again, top);
        /* the next iteration doesn't fit: back to C at the loop's start */
        j.pend = pend_end;
        e_flush_ccr(&j);
        e_store_held(&j);
        x_mov(&j.x, 1, X_RCX, X_RBX);
        x_movi64(&j.x, X_RDX, (uint64_t)(uintptr_t)t);
        x_call(&j.x, (const void *)jit_loop_budget_c);
        x_load(&j.x, 1, X_RDX, X_RSP, SLOT(4));
        x_load(&j.x, 0, X_RCX, X_RDX, RUN_OFF(save));
        x_test(&j.x, 0, X_RCX, X_RCX);
        {
            uint8_t *more = x_jcc(&j.x, CC_G);
            x_store_i(&j.x, X_RDX, RUN_OFF(code), JC_CHUNK);   /* a chunk ended */
            x_patch(x_jmp(&j.x), jt->epilogue);
            x_patch(more, j.x.p);
        }
        e_to_dispatch(&j, jt);                                /* C carries on at SSH */
        /* the loop's end */
        x_patch(done, j.x.p);
        j.pend = pend_end;
        e_flush_ccr(&j);
        e_store_held(&j);
        if (!getenv("BW_DSP_JIT_NOTERM")) {
            /* natively, then straight to the code after the loop */
            uint8_t *lslow[4], *more;
            int nl = e_loop_end_native(&j, t, end_pc, lslow), q;
            x_load(&j.x, 1, X_RDX, X_RSP, SLOT(4));
            x_load(&j.x, 0, X_RCX, X_RDX, RUN_OFF(save));
            x_test(&j.x, 0, X_RCX, X_RCX);
            more = x_jcc(&j.x, CC_G);
            x_store_i(&j.x, X_RDX, RUN_OFF(code), JC_CHUNK);   /* a chunk ended */
            x_patch(x_jmp(&j.x), jt->epilogue);
            x_patch(more, j.x.p);
            if (end_pc < BWF_PSIZE) {
                x_movi64(&j.x, X_RAX, (uint64_t)(uintptr_t)&ctab[end_pc]);
                x_jmp_m(&j.x, X_RAX, 0);
            } else {
                e_to_dispatch(&j, jt);
            }
            for (q = 0; q < nl; q++)
                x_patch(lslow[q], j.x.p);
        }
        x_mov(&j.x, 1, X_RCX, X_RBX);
        x_movi64(&j.x, X_RDX, (uint64_t)(uintptr_t)t);
        x_movi(&j.x, X_R8, end_pc);
        x_call(&j.x, (const void *)jit_loop_end_c);
        x_load(&j.x, 1, X_RDX, X_RSP, SLOT(4));
        x_load(&j.x, 0, X_RCX, X_RDX, RUN_OFF(save));
        x_test(&j.x, 0, X_RCX, X_RCX);
        {
            uint8_t *more = x_jcc(&j.x, CC_G);
            x_store_i(&j.x, X_RDX, RUN_OFF(code), JC_CHUNK);   /* a chunk ended */
            x_patch(x_jmp(&j.x), jt->epilogue);
            x_patch(more, j.x.p);
        }
        x_patch(x_jmp(&j.x), plain_checks);
        cold_swap(&j);
        e_cold(&j);
        /* stops and bails in an iteration: save and cc as at its start */
        e_stop_exits(&j, jt, JR_LSTOP, start);
        for (i = 0; i < j.nbails; i++) {
            x_patch(j.bails[i], j.x.p);
            x_load(&j.x, 1, X_RDX, X_RSP, SLOT(4));
            x_store_i(&j.x, X_RDX, RUN_OFF(code), JR_LSTOP | j.bail_index[i]);
            x_store_i(&j.x, X_RDX, RUN_OFF(block_pc), start);
            x_patch(x_jmp(&j.x), jt->epilogue);
        }
        cold_swap(&j);
    }
    VirtualProtect(code, need, PAGE_EXECUTE_READ, &old);
    VirtualProtect(ccode, need_cold, PAGE_EXECUTE_READ, &old);
    if (j.x.bad || j.xc.bad || nfail > 40)
        return NULL;
    FlushInstructionCache(GetCurrentProcess(), code, (SIZE_T)(j.x.p - code));
    FlushInstructionCache(GetCurrentProcess(), ccode, (SIZE_T)(j.xc.p - ccode));
    /* a fragment runs in the trampoline's frame: unwind info with no prologue
     * (its cold code too: the cold area's one entry) */
    jt->rtf[jt->nfn].BeginAddress = (DWORD)(code - jt->base);
    jt->rtf[jt->nfn].EndAddress = (DWORD)(j.x.p - jt->base);
    jt->rtf[jt->nfn].UnwindData = 32;
    jt->nfn++;
    jt->used = (uint32_t)(j.x.p - jt->base + 15) & ~15u;
    jt->cold_used = (uint32_t)(j.xc.p - jt->base + 15) & ~15u;
    jt->rtf_cold.EndAddress = jt->cold_used;
    return code;
}
