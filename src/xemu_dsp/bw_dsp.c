/* SPDX-License-Identifier: GPL-2.0-or-later (with xemu's DSP it hosts; see NOTICE.md) */
/*
 * bw_dsp.c - Blood Wake's wrapper around xemu's MCPX DSP (D-012). See bw_dsp.h.
 *
 * Only the C interpreter is built (xemu's JIT is not vendored); dsp.c's JIT
 * entry points are satisfied below and never selected.
 *
 * Two engines run a frame (lane au-dsp):
 *   - interp: xemu's interpreter, as since D-012;
 *   - recomp: the translated engine (bw_dsp_fast.c), built at run time from
 *     the DSP's own P memory, meant to be bit-identical to the interpreter.
 * BW_DSP_ENGINE=interp|recomp picks one (unset: the bridge's default, set
 * through bw_dsp_engine_default: recomp in every edition since 0.3.0; the
 * audio rework's gate, call D5, and lane au-orig for Original).
 * BW_DSP_LOCKSTEP=1 (a test aid) runs both every frame:
 * the interpreter on the real DSP (its result is what the game gets), the
 * translated engine on a copy of the state from the same start, with the
 * interpreter's scratch and FIFO traffic replayed to it; then it compares
 * the whole state and every write. At the first difference it logs the
 * frame, the PCs and the first differing register or word with both values,
 * and that DSP stays on the interpreter for the rest of the boot.
 */
#include "qemu/osdep.h"
#include "dsp.h"
#include "dsp_internal.h"
#include "interp/dsp_cpu.h"
#include "ui/xemu-settings.h"
#include "bw_dsp.h"
#include "bw_dsp_engine.h"
#include "bw_dsp_fast.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

struct bw_xemu_config g_config = { { false } };

/* dsp.c names the JIT engine; it is never chosen (use_dsp_jit is false). */
const DSPOps jit_dsp_ops;
void dsp_jit_init(DSPState *dsp)
{
    dsp_c_init(dsp);
}

/* The state layout is a file format (docs/audio/trace_format.md). */
_Static_assert(offsetof(BwDspState, reserved) == 129 * 4, "BwDspState core layout");
_Static_assert(offsetof(BwDspState, periph) == BW_DSP_CORE_WORDS * 4, "BwDspState core size");
_Static_assert(offsetof(BwDspState, mix) == BW_DSP_OFF_M * 4, "BwDspState memory offsets");

enum { ENG_INTERP, ENG_RECOMP };

/* One recorded scratch or FIFO transfer (lockstep). */
typedef struct {
    uint8_t fifo, dir;
    uint32_t addr, off;
    size_t len;
} IoRec;

typedef struct {
    /* the real callbacks, while recording */
    void *opaque;
    dsp_scratch_rw_func scratch;
    dsp_fifo_rw_func fifo;
    IoRec *rec;
    size_t n, cap, pos;
    uint8_t *data;
    size_t dlen, dcap;
    int bad;                    /* replay: the first mismatch, 1-based */
    char why[200];
} IoLog;

struct BwDsp {
    DSPState *dsp;
    /* The caller's callbacks: the DSP gets the trampolines below, so the
     * observation hooks (bw_dsp_set_hooks) see every transfer. */
    void *opaque;
    bw_dsp_scratch_rw scratch;
    bw_dsp_fifo_rw fifo;
    int is_gp;
    int engine;                 /* ENG_*: this DSP's engine now */
    BwFast *fast;
    long frames;
    /* lockstep */
    int lockstep;
    DSPState *sh_dsp;
    dsp_core_t *sh_core;
    IoLog io;
    long lock_frames;
    int64_t t_interp, t_recomp;
    uint64_t c_interp;
};

DSPState *bw_dsp_xemu(BwDsp *b)
{
    return b->dsp;
}

static const BwDspHooks *volatile s_hooks;

void bw_dsp_set_hooks(const BwDspHooks *hooks)
{
    s_hooks = hooks;
}

static void tramp_scratch(void *o, uint8_t *ptr, uint32_t addr, size_t len, bool dir)
{
    BwDsp *b = (BwDsp *)o;
    const BwDspHooks *h = s_hooks;
    if (h && h->io && dir)
        h->io(b, 0, ptr, addr, len, dir);
    b->scratch(b->opaque, ptr, addr, len, dir);
    if (h && h->io && !dir)
        h->io(b, 0, ptr, addr, len, dir);
}

static void tramp_fifo(void *o, uint8_t *ptr, unsigned int index, size_t len, bool dir)
{
    BwDsp *b = (BwDsp *)o;
    const BwDspHooks *h = s_hooks;
    if (h && h->io && dir)
        h->io(b, 1, ptr, index, len, dir);
    b->fifo(b->opaque, ptr, index, len, dir);
    if (h && h->io && !dir)
        h->io(b, 1, ptr, index, len, dir);
}

static int s_engine_env = -1;   /* BW_DSP_ENGINE */
static int s_engine_default;    /* unset BW_DSP_ENGINE: bw_dsp_engine_default (the edition's) */
static int s_lockstep_env = -1; /* BW_DSP_LOCKSTEP */
static int s_stat_env = -1;     /* BW_DSP_FASTSTAT */
static int s_time_env;          /* BW_DSP_TIME: the engine's own time per frame, every 5 s */

/* The engine used when BW_DSP_ENGINE is unset (the audio rework's gate,
 * call D5): the bridge passes the edition's choice before the DSPs exist. */
void bw_dsp_engine_default(int recomp)
{
    s_engine_default = recomp != 0;
}

static void read_env(void)
{
    const char *e;
    if (s_engine_env >= 0)
        return;
    e = getenv("BW_DSP_ENGINE");
    if (e && *e)
        s_engine_env = (e[0] == 'r' || e[0] == 'f') ? ENG_RECOMP : ENG_INTERP;
    else
        s_engine_env = s_engine_default ? ENG_RECOMP : ENG_INTERP;
    {
        const char *l = getenv("BW_DSP_LOCKSTEP");
        const char *s = getenv("BW_DSP_FASTSTAT");
        const char *t = getenv("BW_DSP_TIME");
        s_lockstep_env = l && l[0] == '1';
        s_stat_env = s && *s ? atoi(s) : 0;
        s_time_env = t && t[0] == '1';
    }
    fprintf(stderr, "[DSP] engine: %s (BW_DSP_ENGINE=%s%s)\n",
            s_lockstep_env ? "lockstep: xemu's interpreter, checked frame by frame against the"
                             " translated engine"
                           : s_engine_env == ENG_RECOMP ? "the translated engine (recomp)"
                                                        : "xemu's interpreter",
            e ? e : "unset", s_lockstep_env ? ", BW_DSP_LOCKSTEP=1" : "");
}

static int64_t qpc(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

BwDsp *bw_dsp_new(void *opaque, bw_dsp_scratch_rw scratch, bw_dsp_fifo_rw fifo, bool is_gp)
{
    BwDsp *b = (BwDsp *)calloc(1, sizeof(*b));
    if (!b)
        return NULL;
    b->opaque = opaque;
    b->scratch = scratch;
    b->fifo = fifo;
    read_env();
    b->dsp = dsp_init(b, tramp_scratch, tramp_fifo, is_gp);
    dsp_set_halt_requested(b->dsp, false);
    dsp_set_cycle_count(b->dsp, 0);
    b->is_gp = is_gp;
    b->engine = ENG_INTERP;
    if (s_engine_env == ENG_RECOMP || s_lockstep_env) {
        b->fast = bwf_new(is_gp);
        if (b->fast) {
            b->engine = ENG_RECOMP;
            bwf_set_profile(b->fast, s_stat_env >= 3 ? 2 : s_stat_env ? 1 : 0);
        }
    }
    if (s_lockstep_env && b->fast) {
        b->sh_dsp = (DSPState *)calloc(1, sizeof(DSPState));
        b->sh_core = (dsp_core_t *)calloc(1, sizeof(dsp_core_t));
        b->lockstep = b->sh_dsp && b->sh_core;
    }
    return b;
}

void bw_dsp_free(BwDsp *b)
{
    if (!b)
        return;
    dsp_destroy(b->dsp);
    bwf_free(b->fast);
    free(b->sh_dsp);
    free(b->sh_core);
    free(b->io.rec);
    free(b->io.data);
    free(b);
}

void bw_dsp_reset(BwDsp *b)
{
    const BwDspHooks *h;
    dsp_reset(b->dsp);
    if (b->fast)
        bwf_p_changed(b->fast);         /* whatever the reset left in P */
    h = s_hooks;
    if (h && h->event)
        h->event(b, BW_DSP_EV_RESET);
}

void bw_dsp_bootstrap(BwDsp *b)
{
    const BwDspHooks *h;
    dsp_bootstrap(b->dsp);
    if (b->fast)
        bwf_p_changed(b->fast);
    h = s_hooks;
    if (h && h->event)
        h->event(b, BW_DSP_EV_BOOT);
}

uint32_t bw_dsp_run_frame_raw(BwDsp *b, uint32_t max_cycles)
{
    uint32_t used;
    dsp_start_frame(b->dsp);
    dsp_set_halt_requested(b->dsp, false);
    dsp_set_cycle_count(b->dsp, 0);
    do {
        dsp_run(b->dsp, 1000);
        used = dsp_get_cycle_count(b->dsp);
    } while (!dsp_get_halt_requested(b->dsp) && used < max_cycles);
    return used;
}

/* ---- lockstep: recording and replaying the outside traffic -------------- */

static void io_put(IoLog *l, int fifo, uint32_t addr, size_t len, bool dir, const uint8_t *p)
{
    IoRec *r;
    if (l->n == l->cap) {
        size_t nc = l->cap ? l->cap * 2 : 256;
        IoRec *nr = (IoRec *)realloc(l->rec, nc * sizeof(IoRec));
        if (!nr)
            return;
        l->rec = nr;
        l->cap = nc;
    }
    if (l->dlen + len > l->dcap) {
        size_t nc = l->dcap ? l->dcap : 65536;
        uint8_t *nd;
        while (nc < l->dlen + len)
            nc *= 2;
        nd = (uint8_t *)realloc(l->data, nc);
        if (!nd)
            return;
        l->data = nd;
        l->dcap = nc;
    }
    r = &l->rec[l->n++];
    r->fifo = (uint8_t)fifo;
    r->dir = (uint8_t)dir;
    r->addr = addr;
    r->len = len;
    r->off = (uint32_t)l->dlen;
    memcpy(l->data + l->dlen, p, len);
    l->dlen += len;
}

static void rec_scratch(void *opaque, uint8_t *ptr, uint32_t addr, size_t len, bool dir)
{
    IoLog *l = (IoLog *)opaque;
    l->scratch(l->opaque, ptr, addr, len, dir);
    io_put(l, 0, addr, len, dir, ptr);
}

static void rec_fifo(void *opaque, uint8_t *ptr, unsigned int index, size_t len, bool dir)
{
    IoLog *l = (IoLog *)opaque;
    if (dir)
        io_put(l, 1, index, len, dir, ptr);
    l->fifo(l->opaque, ptr, index, len, dir);
    if (!dir)
        io_put(l, 1, index, len, dir, ptr);
}

static void replay(IoLog *l, int fifo, uint8_t *ptr, uint32_t addr, size_t len, bool dir)
{
    IoRec *r = l->pos < l->n ? &l->rec[l->pos] : NULL;
    l->pos++;
    if (!r || r->fifo != fifo || r->dir != (uint8_t)dir || r->addr != addr || r->len != len) {
        if (!l->bad) {
            l->bad = (int)l->pos;
            _snprintf(l->why, sizeof l->why - 1,
                      "transfer %u: recomp asked %s %s 0x%X len %u, interpreter did %s",
                      (unsigned)l->pos, fifo ? "fifo" : "scratch", dir ? "write" : "read", addr,
                      (unsigned)len, r ? (r->fifo ? "a fifo transfer" : "a scratch transfer")
                                       : "nothing more");
        }
        if (!dir)
            memset(ptr, 0, len);
        return;
    }
    if (dir) {
        if (memcmp(l->data + r->off, ptr, len) && !l->bad) {
            size_t i;
            for (i = 0; i < len && l->data[r->off + i] == ptr[i]; i++)
                ;
            l->bad = (int)l->pos;
            _snprintf(l->why, sizeof l->why - 1,
                      "transfer %u (%s write 0x%X, %u bytes): byte %u is 0x%02X, interpreter 0x%02X",
                      (unsigned)l->pos, fifo ? "fifo" : "scratch", addr, (unsigned)len,
                      (unsigned)i, ptr[i], l->data[r->off + i]);
        }
    } else {
        memcpy(ptr, l->data + r->off, len);
    }
}

static void rep_scratch(void *opaque, uint8_t *ptr, uint32_t addr, size_t len, bool dir)
{
    replay((IoLog *)opaque, 0, ptr, addr, len, dir);
}

static void rep_fifo(void *opaque, uint8_t *ptr, unsigned int index, size_t len, bool dir)
{
    replay((IoLog *)opaque, 1, ptr, index, len, dir);
}

/* The shadow: a copy of the whole state, wired to itself. */
static void shadow_copy(BwDsp *b)
{
    DSPState *d = b->dsp;
    dsp_core_t *c = (dsp_core_t *)d->backend;
    /* P as the engine last saw it: then its blocks and native code carry on
     * from frame to frame, as when it drives the game */
    int same_p = !memcmp(b->sh_core->pram, c->pram, sizeof c->pram);
    *b->sh_dsp = *d;
    *b->sh_core = *c;
    b->sh_dsp->backend = b->sh_core;
    b->sh_core->opaque = b->sh_dsp;
    b->sh_dsp->dma.mem_opaque = b->sh_core;
    b->sh_dsp->dma.rw_opaque = &b->io;
    b->sh_dsp->dma.scratch_rw = rep_scratch;
    b->sh_dsp->dma.fifo_rw = rep_fifo;
    if (!same_p)
        bwf_p_changed(b->fast);     /* a new state: P differs */
}

#define CMP1(field)                                                                       \
    if (a->field != t->field) {                                                           \
        _snprintf(out, cap - 1, "%s: interpreter 0x%X, recomp 0x%X", #field,              \
                  (unsigned)a->field, (unsigned)t->field);                                \
        return 1;                                                                         \
    }
#define CMPN(arr, n, name)                                                                \
    for (i = 0; i < (n); i++)                                                             \
        if (a->arr[i] != t->arr[i]) {                                                     \
            _snprintf(out, cap - 1, "%s[0x%X]: interpreter 0x%06X, recomp 0x%06X", name, i,  \
                      (unsigned)a->arr[i], (unsigned)t->arr[i]);                          \
            return 1;                                                                     \
        }

/* The first difference between the interpreter's state (a) and the
 * translated engine's (t), or 0. */
static int compare_core(const dsp_core_t *a, const dsp_core_t *t, char *out, size_t cap)
{
    unsigned i, k;
    CMP1(pc);
    CMPN(registers, DSP_REG_MAX, "register");
    for (k = 0; k < 2; k++)
        for (i = 0; i < 16; i++)
            if (a->stack[k][i] != t->stack[k][i]) {
                _snprintf(out, cap - 1, "stack[%u][%u]: interpreter 0x%X, recomp 0x%X", k, i,
                          (unsigned)a->stack[k][i], (unsigned)t->stack[k][i]);
                return 1;
            }
    CMPN(xram, DSP_XRAM_SIZE, "X");
    CMPN(yram, DSP_YRAM_SIZE, "Y");
    CMPN(pram, DSP_PRAM_SIZE, "P");
    CMPN(mixbuffer, DSP_MIXBUFFER_SIZE, "mixbuffer");
    CMPN(periph, DSP_PERIPH_SIZE, "periph");
    CMP1(is_gp);
    CMP1(is_idle);
    CMP1(cycle_count);
    CMP1(instr_cycle);
    CMP1(loop_rep);
    CMP1(pc_on_rep);
    CMP1(interrupt_state);
    CMP1(interrupt_instr_fetch);
    CMP1(interrupt_save_pc);
    CMP1(interrupt_counter);
    CMP1(interrupt_ipl_to_raise);
    CMP1(interrupt_pipeline_count);
    CMPN(interrupt_ipl, 4, "interrupt_ipl");
    CMPN(interrupt_is_pending, 4, "interrupt_is_pending");
    CMP1(num_inst);
    CMP1(cur_inst_len);
    CMP1(cur_inst);
    CMP1(disasm_memory_ptr);
    return 0;
}

static int compare_dsp(const DSPState *a, const DSPState *t, char *out, size_t cap)
{
    CMP1(save_cycles);
    CMP1(interrupts);
    CMP1(is_gp);
    CMP1(dma.configuration);
    CMP1(dma.control);
    CMP1(dma.start_block);
    CMP1(dma.next_block);
    CMP1(dma.error);
    CMP1(dma.eol);
    CMP1(dma.dma_read_count);
    return 0;
}

static uint32_t lockstep_frame(BwDsp *b, uint32_t max_cycles)
{
    DSPState *d = b->dsp;
    dsp_core_t *c = (dsp_core_t *)d->backend;
    uint32_t start_pc = c->pc, used, used2;
    char why[300] = "";
    int64_t t0, t1, t2;
    int diff = 0;

    shadow_copy(b);

    /* the interpreter on the real DSP, its traffic recorded */
    b->io.opaque = d->dma.rw_opaque;
    b->io.scratch = d->dma.scratch_rw;
    b->io.fifo = d->dma.fifo_rw;
    b->io.n = b->io.pos = b->io.dlen = 0;
    b->io.bad = 0;
    d->dma.rw_opaque = &b->io;
    d->dma.scratch_rw = rec_scratch;
    d->dma.fifo_rw = rec_fifo;
    t0 = qpc();
    used = bw_dsp_run_frame_raw(b, max_cycles);
    t1 = qpc();
    d->dma.rw_opaque = b->io.opaque;
    d->dma.scratch_rw = b->io.scratch;
    d->dma.fifo_rw = b->io.fifo;

    /* the translated engine on the copy, the traffic replayed */
    used2 = bwf_run_frame(b->fast, b->sh_dsp, max_cycles);
    t2 = qpc();
    b->t_interp += t1 - t0;
    b->t_recomp += t2 - t1;
    b->c_interp += used;

    if (b->io.bad) {
        _snprintf(why, sizeof why - 1, "%s", b->io.why);
        diff = 1;
    } else if (b->io.pos != b->io.n) {
        _snprintf(why, sizeof why - 1, "recomp made %u transfers, the interpreter %u",
                  (unsigned)b->io.pos, (unsigned)b->io.n);
        diff = 1;
    } else if (used != used2) {
        _snprintf(why, sizeof why - 1, "cycles: interpreter %u, recomp %u", used, used2);
        diff = 1;
    } else {
        diff = compare_core(c, b->sh_core, why, sizeof why) ||
               compare_dsp(d, b->sh_dsp, why, sizeof why);
    }
    why[sizeof why - 1] = 0;
    if (diff) {
        fprintf(stderr, "[DSP-LOCK] %s DIFFERENT at frame %ld (%ld identical before): start PC"
                        " 0x%04X, end PC interpreter 0x%04X recomp 0x%04X, %u cycles; %s."
                        " This DSP stays on the interpreter.\n",
                b->is_gp ? "GP" : "EP", b->frames, b->lock_frames, start_pc, c->pc,
                b->sh_core->pc, used, why);
        b->lockstep = 0;
        b->engine = ENG_INTERP;
    } else {
        b->lock_frames++;
    }
    return used;
}

static void report(BwDsp *b)
{
    static int64_t qpf;
    if (!qpf) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        qpf = f.QuadPart;
    }
    if (b->lockstep && b->lock_frames) {
        double n = b->is_gp ? 7500.0 : 938.0;
        double ti = (double)b->t_interp / qpf * 1e6 / n, tr = (double)b->t_recomp / qpf * 1e6 / n;
        fprintf(stderr, "[DSP-LOCK] %s: %ld frames identical so far; the last %.0f: interpreter"
                        " %.2f us/frame, recomp %.2f us/frame (%.2fx), %.0f cycles/frame\n",
                b->is_gp ? "GP" : "EP", b->lock_frames, n, ti, tr, tr > 0 ? ti / tr : 0.0,
                (double)b->c_interp / n);
        b->t_interp = b->t_recomp = 0;
        b->c_interp = 0;
    }
    if (s_time_env && !b->lockstep) {
        double n = b->is_gp ? 7500.0 : 938.0;
        int64_t io = (b->fast && b->engine == ENG_RECOMP) ? bwf_take_io_time(b->fast) : 0;
        fprintf(stderr, "[DSP-TIME] %s: %.2f us/frame inside the engine (%s), of which %.2f in the"
                        " DMA's system-memory callbacks; the last %.0f\n",
                b->is_gp ? "GP" : "EP", (double)b->t_recomp / qpf * 1e6 / n,
                b->engine == ENG_RECOMP ? "recomp" : "interpreter", (double)io / qpf * 1e6 / n, n);
        b->t_recomp = 0;
    }
    if (s_stat_env && b->fast && b->engine == ENG_RECOMP) {
        uint64_t nf, ns, nd;
        char kinds[1600];
        bwf_take_stats(b->fast, &nf, &ns, &nd);
        fprintf(stderr, "[DSP-FAST] %s: %llu instructions by the translated handlers, %llu by"
                        " the interpreter's step, %llu decodes\n", b->is_gp ? "GP" : "EP",
                (unsigned long long)nf, (unsigned long long)ns, (unsigned long long)nd);
        if (s_stat_env >= 2) {
            bwf_kind_report(b->fast, kinds, sizeof kinds);
            fprintf(stderr, "[DSP-FAST] %s kinds: %s\n", b->is_gp ? "GP" : "EP", kinds);
        }
        if (s_stat_env >= 3) {
            char prof[4000];
            bwf_profile_report(b->fast, prof, sizeof prof, 45);
            fprintf(stderr, "[DSP-FAST] %s profile: %s\n", b->is_gp ? "GP" : "EP", prof);
        }
    }
}

uint32_t bw_dsp_run_frame(BwDsp *b, uint32_t max_cycles)
{
    uint32_t used;
    const BwDspHooks *h = s_hooks;
    {
        /* BW_DSP_PIN=n (a test aid): the thread running the DSPs on logical CPU n */
        static __declspec(thread) int pinned;
        if (!pinned) {
            const char *e = getenv("BW_DSP_PIN");
            pinned = 1;
            if (e && *e) {
                SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1 << atoi(e));
                fprintf(stderr, "[DSP] the DSP thread pinned to logical CPU %d (BW_DSP_PIN)\n", atoi(e));
            }
        }
    }
    if (h && h->run_begin)
        h->run_begin(b, max_cycles);
    if (b->lockstep) {
        used = lockstep_frame(b, max_cycles);
    } else {
        int64_t t0 = s_time_env ? qpc() : 0;
        if (b->engine == ENG_RECOMP)
            used = bwf_run_frame(b->fast, b->dsp, max_cycles);
        else
            used = bw_dsp_run_frame_raw(b, max_cycles);
        if (s_time_env)
            b->t_recomp += qpc() - t0;
    }
    if (h && h->run_end)
        h->run_end(b, used);
    b->frames++;
    /* every 5 s of audio: 7,500 GP frames, 938 EP periods */
    if (b->frames % (b->is_gp ? 7500 : 938) == 0)
        report(b);
    return used;
}

/* The interpreter as an engine (bw_dsp_engine.h). */
static void *interp_attach(BwDsp *b)
{
    return b;
}

static void interp_detach(void *h)
{
    (void)h;
}

static uint32_t interp_run_frame(void *h, uint32_t max_cycles)
{
    return bw_dsp_run_frame_raw((BwDsp *)h, max_cycles);
}

const BwDspEngine bw_dsp_engine_interp = {
    "interp", interp_attach, interp_detach, NULL, NULL, interp_run_frame
};

uint32_t bw_dsp_read(BwDsp *b, char space, uint32_t addr)
{
    return dsp_read_memory(b->dsp, space, addr);
}

void bw_dsp_write(BwDsp *b, char space, uint32_t addr, uint32_t value)
{
    dsp_write_memory(b->dsp, space, addr, value);
    if (space == 'P' && b->fast)
        bwf_p_changed(b->fast);
}

uint32_t *bw_dsp_mem(BwDsp *b, char space, uint32_t *words)
{
    dsp_core_t *core = (dsp_core_t *)b->dsp->backend;
    switch (space) {
    case 'X': *words = DSP_XRAM_SIZE; return core->xram;
    case 'Y': *words = DSP_YRAM_SIZE; return core->yram;
    case 'P': *words = DSP_PRAM_SIZE; return core->pram;
    case 'M': *words = DSP_MIXBUFFER_SIZE; return core->mixbuffer;
    default: *words = 0; return NULL;
    }
}

/* ---- the whole state ------------------------------------------------------ */

int bw_dsp_is_gp(BwDsp *b)
{
    return b->dsp->is_gp;
}

static void core_out(const DSPState *d, const dsp_core_t *c, BwDspState *s)
{
    int i;
    memset(s, 0, BW_DSP_CORE_WORDS * 4);
    s->version = BW_DSP_STATE_VERSION;
    s->is_gp = c->is_gp;
    s->pc = c->pc;
    memcpy(s->regs, c->registers, sizeof s->regs);
    memcpy(s->stack, c->stack, sizeof s->stack);
    s->loop_rep = c->loop_rep;
    s->pc_on_rep = c->pc_on_rep;
    s->is_idle = c->is_idle;
    s->cycle_count = c->cycle_count;
    s->instr_cycle = c->instr_cycle;
    s->cur_inst = c->cur_inst;
    s->cur_inst_len = c->cur_inst_len;
    s->int_state = c->interrupt_state;
    s->int_instr_fetch = c->interrupt_instr_fetch;
    s->int_save_pc = c->interrupt_save_pc;
    s->int_counter = c->interrupt_counter;
    s->int_ipl_to_raise = c->interrupt_ipl_to_raise;
    s->int_pipeline_count = c->interrupt_pipeline_count;
    for (i = 0; i < 4; i++) {
        s->int_ipl[i] = c->interrupt_ipl[i];
        s->int_pending[i] = c->interrupt_is_pending[i];
    }
    s->save_cycles = d->save_cycles;
    s->interrupts = d->interrupts;
    s->dma_configuration = d->dma.configuration;
    s->dma_control = d->dma.control;
    s->dma_start_block = d->dma.start_block;
    s->dma_next_block = d->dma.next_block;
    s->dma_error = d->dma.error;
    s->dma_eol = d->dma.eol;
    s->dma_read_count = d->dma.dma_read_count;
    memcpy(s->periph, c->periph, sizeof s->periph);
}

static void core_in(DSPState *d, dsp_core_t *c, const BwDspState *s)
{
    int i;
    c->is_gp = s->is_gp != 0;
    c->pc = s->pc;
    memcpy(c->registers, s->regs, sizeof s->regs);
    memcpy(c->stack, s->stack, sizeof s->stack);
    c->loop_rep = s->loop_rep;
    c->pc_on_rep = s->pc_on_rep;
    c->is_idle = s->is_idle != 0;
    c->cycle_count = s->cycle_count;
    c->instr_cycle = (uint16_t)s->instr_cycle;
    c->cur_inst = s->cur_inst;
    c->cur_inst_len = s->cur_inst_len;
    c->interrupt_state = (uint16_t)s->int_state;
    c->interrupt_instr_fetch = (uint16_t)s->int_instr_fetch;
    c->interrupt_save_pc = (uint16_t)s->int_save_pc;
    c->interrupt_counter = (uint16_t)s->int_counter;
    c->interrupt_ipl_to_raise = (uint16_t)s->int_ipl_to_raise;
    c->interrupt_pipeline_count = (uint16_t)s->int_pipeline_count;
    for (i = 0; i < 4; i++) {
        c->interrupt_ipl[i] = (int16_t)s->int_ipl[i];
        c->interrupt_is_pending[i] = (uint16_t)s->int_pending[i];
    }
    d->save_cycles = s->save_cycles;
    d->interrupts = s->interrupts;
    d->dma.configuration = s->dma_configuration;
    d->dma.control = s->dma_control;
    d->dma.start_block = s->dma_start_block;
    d->dma.next_block = s->dma_next_block;
    d->dma.error = s->dma_error != 0;
    d->dma.eol = s->dma_eol != 0;
    d->dma.dma_read_count = s->dma_read_count;
    memcpy(c->periph, s->periph, sizeof s->periph);
}

void bw_dsp_get_state(BwDsp *b, BwDspState *s)
{
    const dsp_core_t *c = (const dsp_core_t *)b->dsp->backend;
    core_out(b->dsp, c, s);
    memcpy(s->x, c->xram, sizeof s->x);
    memcpy(s->y, c->yram, sizeof s->y);
    memcpy(s->p, c->pram, sizeof s->p);
    memcpy(s->mix, c->mixbuffer, sizeof s->mix);
}

void bw_dsp_set_core(BwDsp *b, const BwDspState *s)
{
    core_in(b->dsp, (dsp_core_t *)b->dsp->backend, s);
}

void bw_dsp_set_state(BwDsp *b, const BwDspState *s)
{
    dsp_core_t *c = (dsp_core_t *)b->dsp->backend;
    core_in(b->dsp, c, s);
    memcpy(c->xram, s->x, sizeof s->x);
    memcpy(c->yram, s->y, sizeof s->y);
    memcpy(c->pram, s->p, sizeof s->p);
    memcpy(c->mixbuffer, s->mix, sizeof s->mix);
    memset((void *)c->pram_opcache, 0, sizeof c->pram_opcache);
    if (b->fast)
        bwf_p_changed(b->fast);
}

void bw_dsp_poke(BwDsp *b, char space, uint32_t addr, uint32_t value)
{
    dsp_core_t *c = (dsp_core_t *)b->dsp->backend;
    switch (space) {
    case 'X': if (addr < DSP_XRAM_SIZE) c->xram[addr] = value; break;
    case 'Y': if (addr < DSP_YRAM_SIZE) c->yram[addr] = value; break;
    case 'M': if (addr < DSP_MIXBUFFER_SIZE) c->mixbuffer[addr] = value; break;
    case 'P':
        if (addr < DSP_PRAM_SIZE) {
            c->pram[addr] = value;
            c->pram_opcache[addr] = NULL;
            if (b->fast)
                bwf_p_changed(b->fast);
        }
        break;
    }
}
