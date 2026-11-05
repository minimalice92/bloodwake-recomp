/*
 * bw_apu_xdsp.c - the GP and EP DSPs, run by xemu's interpreter (D-012).
 *
 * Blood Wake file, compiled into the toolkit's xbox_apu library beside the
 * carried apu_dsp.c, which calls in here unless BW_APU_DSP=fold, which runs
 * the stub's stereo fold-down (TK-16) instead (D-012: the DSP is the default).
 *
 * Ported from xemu v0.8.139 hw/xbox/mcpx/apu/dsp/gp_ep.c (GPL-2.0-or-later),
 * with the toolkit's differences:
 *   - The GP/EP memory windows (guest 0xFE830000 / 0xFE850000) are plain RAM
 *     here, not MMIO, so DirectSound's writes land in guest memory. Each frame
 *     they are merged into the DSPs' memories against a shadow copy (words the
 *     CPU changed go in), the DSPs run, and their memories are copied back
 *     except where the CPU wrote meanwhile.
 *   - GPRST / EPRST are polled from those windows once a frame (a write there
 *     does not trap), and a reset -> run edge bootstraps the DSP.
 *   - Scratch and FIFO pages resolve through the toolkit's mcpx_apu_phys
 *     (TK-15 / TK-17's resolver), not xemu's flat RAM.
 *   - The EP's output FIFO 0 is the final mix: it lands in monitor.frame_buf,
 *     which mcpx_apu_monitor_frame hands to the device every 8 frames.
 */
#include "apu_state.h"
#include "fpconv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "bw_dsp.h"
#include "bw_dsp_trace.h"
#include "bw_apu_hot.h"     /* au-hot: BW_APU_HOTSTAT, a test aid */

#define GP_WIN 0xFE830000u
#define EP_WIN 0xFE850000u
#define SG_PAGE 4096u
#define FIFO_MASK 0x00FFFFFFu
/* A frame normally ends when the program waits for the next one. The caps
 * (about 4x the console GP's 106k cycles per 32-sample frame at 160 MHz, and
 * 8 frames for the EP) only stop a program that never does from stalling the
 * audio thread. */
#define BW_GP_MAX_CYCLES 400000u
#define BW_EP_MAX_CYCLES 800000u
/* A program that hits its cap this many periods in a row can't keep real
 * time at all: told "surround" (or AC3), DirectSound puts its Dolby encoder
 * on the EP, which here never finishes a frame (seen, run 20261027-0400xx-
 * r6-gpu-D2-spk6-surround: every EP run to the cap), so the audio stalled
 * and the game hung in its voice-stop wait. The boot then switches to the
 * fold-down (which runs that mode fine, seen). */
#define BW_CAP_FALLBACK 3

extern ptrdiff_t xbox_GetMemoryOffset(void);
extern const volatile uint8_t *xbox_ContiguousPageMap(void);

/* Unset BW_APU_DSP picks this: 1 the DSPs, 0 the stub's fold-down.
 * D-012's rule met 2026-10-27 (docs/day1/D2.md): 30 fps in the heaviest
 * scenes, no dropouts, clicks or level change against the fold-down. */
#define BW_XDSP_DEFAULT 1

static int s_on = -1;
static int s_survey;                        /* BW_APU_XDSP_SURVEY=1 */
static BwDsp *s_gp, *s_ep;
static uint32_t s_gprst, s_eprst;           /* last reset words seen */
static long s_frames, s_gp_runs, s_ep_runs, s_sink_frames;
static uint64_t s_gp_cycles, s_ep_cycles;
static uint32_t s_gp_max, s_ep_max;
/* Host time spent here (QueryPerformanceCounter ticks), per 5 s line. */
static int64_t s_qpf, s_t_all, s_t_gp, s_t_ep, s_t_sync, s_t_frame_max;

static int64_t qpc(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

/* The safety net. The DSPs cost about 0.35 of a core on a quiet machine and
 * 0.6-0.7 with every core busy (parallel compiles and a dozen game instances
 * beside the game). Under that much load the DSPs can drop out where the
 * fold-down doesn't (attract demo side by side, runs 20261027-024603-*: DSPs
 * 19 starves in 22 s, fold-down 0), though sometimes both do (mission 10,
 * 20261027-023423-*: 382 and 400). Quiet, the stage takes 340-390 ms per
 * second of audio. So when two 5 s windows in a row each have the stage at
 * BW_XDSP_FALLBACK_MS ms or more per second of audio (the machine is short
 * of CPU) and at least 3 new dry spells, this boot switches to the fold-down
 * for good (at the next period boundary). The output stays the same except for the
 * reverb the game asks for in M28's zones (the fold-down equals the DSPs'
 * output to -55..-67 dB everywhere else), and DirectSound's doorbell goes
 * back to the stub's auto-ack. Switching back is not attempted: commands
 * acknowledged meanwhile never reached the GP. BW_APU_DSP_FALLBACK=0 keeps
 * the DSPs whatever happens. */
#define BW_XDSP_FALLBACK_MS 500.0
extern int xa2_is_active(void);
extern unsigned xa2_underruns(void);
static int s_fallback_pending;

/* Called with each 5 s line: ms = DSP stage host time per second of audio. */
static void fallback_check(double ms)
{
    static int armed = -1, bad;
    static unsigned last_u;
    static double limit = BW_XDSP_FALLBACK_MS;
    unsigned u, dry;

    if (armed < 0) {
        const char *e = getenv("BW_APU_DSP_FALLBACK");
        const char *m = getenv("BW_APU_DSP_FALLBACK_MS");   /* for tests */
        armed = !(e && e[0] == '0');
        if (m && *m)
            limit = atof(m);
    }
    if (!armed || !xa2_is_active())
        return;
    u = xa2_underruns();
    dry = u - last_u;
    last_u = u;
    if (ms >= limit && dry >= 3)
        bad++;
    else
        bad = 0;
    if (bad >= 2 && !s_fallback_pending) {
        s_fallback_pending = 1;
        fprintf(stderr, "[APU-XDSP] the DSP stage took %.0f ms per second of audio and the"
                        " device ran dry %u times in 5 s: the DSPs can't keep real time on"
                        " this machine now, so this boot switches to the fold-down"
                        " (BW_APU_DSP_FALLBACK=0 keeps the DSPs)\n", ms, dry);
    }
}

/* BW_APU_DSP=xemu (or 1, dsp): the GP and EP run DirectSound's programs
 * here. BW_APU_DSP=fold (or 0, stub): the stub's stereo fold-down (TK-16).
 * Unset: BW_XDSP_DEFAULT. Read once per boot. */
int bw_xdsp_enabled(void)
{
    if (s_on < 0) {
        const char *e = getenv("BW_APU_DSP");
        if (e && *e)
            s_on = e[0] == 'x' || e[0] == '1' || e[0] == 'd';
        else
            s_on = BW_XDSP_DEFAULT;
        fprintf(stderr, "[APU-XDSP] audio DSP stage: %s (BW_APU_DSP=%s)\n",
                s_on ? "the GP/EP DSPs run DirectSound's programs (xemu DSP56300)"
                     : "the stub's stereo fold-down (TK-16)",
                e && *e ? e : "unset");
    }
    return s_on;
}

static uint8_t *win(uint32_t guest)
{
    return (uint8_t *)(uintptr_t)(guest + xbox_GetMemoryOffset());
}

/* ---- scatter-gather (xemu scatter_gather_rw) -------------------------- */

/* Which copy a DSP page is (TK-17). Here the heap and the contiguous window
 * are separate storage that share physical numbers, and DirectSound fills
 * the DSPs' SGE tables with MmGetPhysicalAddress results. Seen in Blood
 * Wake (BW_APU_XDSP_SURVEY): the four tables, the FIFOs and the EP's scratch
 * are contiguous memory (the window); the GP's effect scratch that comes
 * with the effects image (entries 12-205, about 780 KB: the reverb's delay
 * lines) is heap RAM, whose numbers the contiguous allocator later also
 * hands out (textures, buffers).
 *
 * The first version of this file resolved by the general rules, which put a
 * table in the heap copy and wrote DSP output over the guest's main stack;
 * the second pinned everything to the window, which put the delay lines in
 * other allocations' memory: the reverb read their contents (a second of
 * loud noise at boot) and wrote over them. Now each entry's copy comes from
 * the kernel's record of the page's last translation (t1 RAM, t2 window),
 * taken once, when the entry's value first appears, and kept: a later
 * translation of the same number for something else (D3D translating a
 * texture there) must not move it. The tables are scanned when DirectSound
 * writes an SGE register (bw_xdsp_sge_written, right after its
 * translations) and once a frame. BW_APU_XDSP_PAGES=window restores the
 * pinning. */
extern const volatile uint8_t *xbox_PhysRamNameMap(void);
extern const volatile uint8_t *bw_PhysLastTranslationMap(void);

enum { COPY_NONE, COPY_WINDOW, COPY_RAM };
enum { WHY_T1, WHY_T2, WHY_NEXT, WHY_C, WHY_N, WHY_DEFAULT, WHY_PINNED, WHY_COUNT };
static const char *const s_why_name[WHY_COUNT] = {
    "last translation RAM", "last translation window", "next page of the entry before",
    "contiguous, never named", "named heap, not contiguous", "default window", "pinned"
};
static int s_pages_mode = -1;               /* 'w': pinned to the window */

static int page_copy(uint32_t pa, int prev_copy, uint32_t prev_pa, int *why)
{
    uint32_t pg = (pa & 0x03FFFFFFu) >> 12;
    uint8_t kind = bw_PhysLastTranslationMap()[pg];
    uint8_t c = xbox_ContiguousPageMap()[pg], n = xbox_PhysRamNameMap()[pg];

    if (s_pages_mode < 0) {
        const char *e = getenv("BW_APU_XDSP_PAGES");
        s_pages_mode = e && e[0] == 'w' ? 'w' : 'r';
    }
    if (s_pages_mode == 'w')
        return *why = WHY_PINNED, COPY_WINDOW;
    if (kind == 2)
        return *why = WHY_T2, COPY_WINDOW;
    if (kind == 1)
        return *why = WHY_T1, COPY_RAM;
    if (prev_copy && pa == prev_pa + SG_PAGE)
        return *why = WHY_NEXT, prev_copy;  /* a block translated by its first page */
    if (c && !n)
        return *why = WHY_C, COPY_WINDOW;
    if (n && !c)
        return *why = WHY_N, COPY_RAM;
    return *why = WHY_DEFAULT, COPY_WINDOW;
}

static uint8_t *copy_ptr(uint32_t pa, int copy)
{
    return mcpx_apu_phys((uint64_t)(pa & 0x03FFFFFFu)
                         | (copy == COPY_RAM ? 0x10000000u : 0x20000000u));
}

#define SGE_CACHE_MAX 4096
typedef struct {
    const char *name;
    uint32_t base_reg, max_reg;
    uint32_t base;                  /* the table the cache holds */
    int table_copy, table_why;
    uint32_t max_seen;
    uint32_t prd[SGE_CACHE_MAX];
    uint8_t copy[SGE_CACHE_MAX];
    unsigned by_why[WHY_COUNT], resolved, reported;
} SgeCache;

static SgeCache s_sgc[4] = {
    { "GP-scratch", NV_PAPU_GPSADDR, NV_PAPU_GPSMAXSGE },
    { "GP-fifo",    NV_PAPU_GPFADDR, NV_PAPU_GPFMAXSGE },
    { "EP-scratch", NV_PAPU_EPSADDR, NV_PAPU_EPSMAXSGE },
    { "EP-fifo",    NV_PAPU_EPFADDR, NV_PAPU_EPFMAXSGE },
};
static SRWLOCK s_sgc_lock = SRWLOCK_INIT;

/* The table itself: its copy decided when its address first appears. */
static void sgc_table(SgeCache *c, uint32_t base)
{
    if (c->base == base && c->table_copy)
        return;
    c->base = base;
    c->table_copy = page_copy(base, 0, 0, &c->table_why);
    memset(c->prd, 0, sizeof c->prd);
    memset(c->copy, 0, sizeof c->copy);
    memset(c->by_why, 0, sizeof c->by_why);
    c->resolved = c->max_seen = c->reported = 0;
    fprintf(stderr, "[APU-XDSP] SGE %s table 0x%08X: the %s copy (%s)\n", c->name, base,
            c->table_copy == COPY_RAM ? "RAM" : "window", s_why_name[c->table_why]);
}

/* Entry e's 4 KB, or NULL if the entry names none. An entry need not be
 * page-aligned (as in xemu, the DSP reads prd + offset): in the missions the
 * GP's delay lines are a heap block at 0x038648C0, one entry per 4 KB of it.
 * An earlier version took those for "not a page" and dropped every transfer,
 * so in gameplay the reverb had no memory. The 4 KB run on into the next
 * page of the same copy, which the host maps right after it. Caller holds
 * the lock. */
static uint8_t *sgc_entry(SgeCache *c, uint32_t e, uint32_t *prd_out)
{
    uint32_t prd = *(const uint32_t *)copy_ptr(c->base + e * 8, c->table_copy);
    int why, copy;

    *prd_out = prd;
    if (!prd || prd > (64u << 20) - SG_PAGE)
        return NULL;
    if (e >= SGE_CACHE_MAX)
        return copy_ptr(prd, page_copy(prd, 0, 0, &why));
    if (c->prd[e] != prd || !c->copy[e]) {
        copy = page_copy(prd, e ? c->copy[e - 1] : 0, e ? c->prd[e - 1] : 0, &why);
        c->copy[e] = (uint8_t)copy;
        c->prd[e] = prd;
        c->by_why[why]++;
        c->resolved++;
    }
    return copy_ptr(prd, c->copy[e]);
}

static void sgc_scan_locked(MCPXAPUState *d)
{
    int k;
    for (k = 0; k < 4; k++) {
        SgeCache *c = &s_sgc[k];
        uint32_t base = d->regs[c->base_reg], max = d->regs[c->max_reg], e, prd;
        if (!base)
            continue;
        sgc_table(c, base);
        if (max >= SGE_CACHE_MAX)
            max = SGE_CACHE_MAX - 1;
        for (e = 0; e <= max; e++)
            sgc_entry(c, e, &prd);
        if (max > c->max_seen || c->resolved != c->reported) {
            int w;
            char line[400];
            size_t n = 0;
            c->max_seen = max > c->max_seen ? max : c->max_seen;
            c->reported = c->resolved;
            for (w = 0; w < WHY_COUNT; w++)
                if (c->by_why[w])
                    n += _snprintf(line + n, sizeof line - n, "%s%u %s", n ? ", " : "",
                                   c->by_why[w], s_why_name[w]);
            line[sizeof line - 1] = 0;
            fprintf(stderr, "[APU-XDSP] SGE %s: entries 0..%u resolved (%s)\n", c->name, max,
                    n ? line : "none");
        }
    }
}

/* apu_core.c: DirectSound wrote an SGE address or count register. */
void bw_xdsp_sge_written(MCPXAPUState *d, uint32_t addr)
{
    (void)addr;
    if (!bw_xdsp_enabled())
        return;
    AcquireSRWLockExclusive(&s_sgc_lock);
    sgc_scan_locked(d);
    ReleaseSRWLockExclusive(&s_sgc_lock);
}

static void sg_rw(MCPXAPUState *d, int which, uint8_t *ptr, uint32_t addr, size_t len, bool dir)
{
    SgeCache *c = &s_sgc[which];
    uint32_t sge_base = d->regs[c->base_reg], max_sge = d->regs[c->max_reg];
    uint32_t entry = addr / SG_PAGE, off = addr % SG_PAGE, n = SG_PAGE - off;
    static int warned;

    AcquireSRWLockExclusive(&s_sgc_lock);
    if (sge_base)
        sgc_table(c, sge_base);
    while (len > 0) {
        uint32_t prd;
        uint8_t *host;
        if (entry > max_sge || !sge_base) {
            if (!dir)
                memset(ptr, 0, len);
            break;
        }
        if (n > len)
            n = (uint32_t)len;
        host = sgc_entry(c, entry, &prd);
        if (!host) {
            /* no memory there: drop the transfer rather than write anywhere */
            if (warned++ < 8)
                fprintf(stderr, "[APU-XDSP] SGE table 0x%08X entry %u = 0x%08X names no memory;"
                                " transfer of %u bytes dropped\n", sge_base, entry, prd, n);
            if (!dir)
                memset(ptr, 0, n);
        } else {
            host += off;
            if (dir)
                memcpy(host, ptr, n);
            else
                memcpy(ptr, host, n);
        }
        ptr += n;
        len -= n;
        entry++;
        off = 0;
        n = SG_PAGE;
    }
    ReleaseSRWLockExclusive(&s_sgc_lock);
}

/* Diagnostic: one SGE table's pages, as runs of entries with the same
 * evidence: c = the contiguous allocator has the page in use, n = a heap
 * address with that number was translated (xbox_PhysRamNameMap), t1/t2 =
 * the page's last translation was RAM / the window (TK-17's record), and
 * which copies are all zero (zR = RAM copy, zW = window copy). */
static int page_zero(const uint8_t *p)
{
    const uint64_t *q = (const uint64_t *)p;
    int i;
    for (i = 0; i < 512; i++)
        if (q[i])
            return 0;
    return 1;
}

static void page_desc(uint32_t pa, char *out, size_t cap)
{
    uint32_t pg = (pa & 0x03FFFFFFu) >> 12;
    const uint8_t *ram = mcpx_apu_phys((uint64_t)(pg << 12) | 0x10000000u);
    const uint8_t *wnd = mcpx_apu_phys((uint64_t)(pg << 12) | 0x20000000u);
    _snprintf(out, cap, "%s%s t%u %s%s", xbox_ContiguousPageMap()[pg] ? "c" : "-",
              xbox_PhysRamNameMap()[pg] ? "n" : "-", bw_PhysLastTranslationMap()[pg],
              page_zero(ram) ? "zR" : "..", page_zero(wnd) ? "zW" : "..");
    out[cap - 1] = 0;
}

static void sge_survey(const char *name, uint32_t base, uint32_t max)
{
    char d[32], run_d[32] = "";
    uint32_t e, run_start = 0, run_first = 0, run_last = 0;
    int k, tc = COPY_WINDOW;
    if (!base) {
        fprintf(stderr, "[APU-XDSP] SGE %s: no table\n", name);
        return;
    }
    page_desc(base, d, sizeof d);
    {
        const uint32_t *r = (const uint32_t *)mcpx_apu_phys((uint64_t)base | 0x10000000u);
        const uint32_t *w = (const uint32_t *)mcpx_apu_phys((uint64_t)base | 0x20000000u);
        fprintf(stderr, "[APU-XDSP] SGE %s: table 0x%08X [%s], entries 0..%u;"
                        " RAM copy %08X %08X %08X, window copy %08X %08X %08X\n",
                name, base, d, max, r[0], r[2], r[4], w[0], w[2], w[4]);
    }
    for (k = 0; k < 4; k++)
        if (s_sgc[k].base == base && s_sgc[k].table_copy)
            tc = s_sgc[k].table_copy;
    for (e = 0; e <= max && e < 4096; e++) {
        uint32_t prd = *(const uint32_t *)copy_ptr(base + e * 8, tc);
        page_desc(prd, d, sizeof d);
        if (e && (strcmp(d, run_d) || prd != run_last + 0x1000)) {
            fprintf(stderr, "[APU-XDSP]   %s %u-%u: 0x%08X-0x%08X [%s]\n", name, run_start, e - 1,
                    run_first, run_last, run_d);
            run_start = e;
            run_first = prd;
        } else if (!e) {
            run_first = prd;
        }
        strcpy(run_d, d);
        run_last = prd;
    }
    fprintf(stderr, "[APU-XDSP]   %s %u-%u: 0x%08X-0x%08X [%s]\n", name, run_start, e - 1,
            run_first, run_last, run_d);
}

static void sge_survey_all(MCPXAPUState *d, const char *when)
{
    fprintf(stderr, "[APU-XDSP] SGE survey (%s)\n", when);
    sge_survey("GP-scratch", d->regs[NV_PAPU_GPSADDR], d->regs[NV_PAPU_GPSMAXSGE]);
    sge_survey("GP-fifo", d->regs[NV_PAPU_GPFADDR], d->regs[NV_PAPU_GPFMAXSGE]);
    sge_survey("EP-scratch", d->regs[NV_PAPU_EPSADDR], d->regs[NV_PAPU_EPSMAXSGE]);
    sge_survey("EP-fifo", d->regs[NV_PAPU_EPFADDR], d->regs[NV_PAPU_EPFMAXSGE]);
}

static void gp_scratch_rw(void *opaque, uint8_t *ptr, uint32_t addr, size_t len, bool dir)
{
    MCPXAPUState *d = opaque;
    sg_rw(d, 0, ptr, addr, len, dir);
}

static void ep_scratch_rw(void *opaque, uint8_t *ptr, uint32_t addr, size_t len, bool dir)
{
    MCPXAPUState *d = opaque;
    sg_rw(d, 2, ptr, addr, len, dir);
}

/* A FIFO is a circular range [base, end) of a scatter-gather buffer, with its
 * position in a register (xemu circular_scatter_gather_rw). */
static void fifo_rw(MCPXAPUState *d, int which, uint32_t base_reg,
                    uint32_t end_reg, uint32_t cur_reg, uint8_t *ptr, size_t len, bool dir)
{
    uint32_t base = d->regs[base_reg] & FIFO_MASK, end = d->regs[end_reg] & FIFO_MASK;
    uint32_t cur = d->regs[cur_reg] & FIFO_MASK;

    if (end <= base) {                    /* not set up: nothing moves */
        if (!dir)
            memset(ptr, 0, len);
        return;
    }
    if (cur >= end)
        cur = base + (cur - base) % (end - base);
    if (cur < base)
        cur = base;
    while (len > 0) {
        uint32_t n = end - cur;
        if (n > len)
            n = (uint32_t)len;
        sg_rw(d, which, ptr, cur, n, dir);
        ptr += n;
        len -= n;
        cur += n;
        if (cur >= end)
            cur = base;
    }
    d->regs[cur_reg] = (d->regs[cur_reg] & ~FIFO_MASK) | cur;
}

/* Diagnostic, BW_APU_XDSP_FIFOSTAT=1: every 5 s of audio, per DSP and FIFO
 * (G = GP, E = EP; o = output, i = input), the transfers, bytes, and the RMS
 * of each 32-bit lane modulo 8, read as 24-bit samples in the low bits: an
 * interleave of N channels shows as N lanes with sound. */
typedef struct { long n; uint64_t bytes; double sq[8]; long cnt[8]; } FifoStat;
static FifoStat s_fs[2][2][4];
static int s_fifostat = -1;

static void fifo_note(int dsp, int dir, unsigned index, const uint8_t *p, size_t len)
{
    FifoStat *f;
    size_t i;
    if (s_fifostat < 0) {
        const char *e = getenv("BW_APU_XDSP_FIFOSTAT");
        s_fifostat = e && e[0] == '1';
    }
    if (!s_fifostat || index >= 4)
        return;
    f = &s_fs[dsp][dir][index];
    f->n++;
    f->bytes += len;
    for (i = 0; i + 4 <= len; i += 4) {
        int32_t v = (int32_t)(*(const uint32_t *)(p + i) << 8) >> 8;
        f->sq[(i / 4) % 8] += (double)v * v;
        f->cnt[(i / 4) % 8]++;
    }
}

static void fifo_report(void)
{
    int a, b, c, k;
    if (s_fifostat <= 0)
        return;
    for (a = 0; a < 2; a++)
        for (b = 0; b < 2; b++)
            for (c = 0; c < 4; c++) {
                FifoStat *f = &s_fs[a][b][c];
                char lanes[200];
                size_t n = 0;
                if (!f->n)
                    continue;
                for (k = 0; k < 8; k++)
                    n += _snprintf(lanes + n, sizeof lanes - n, " %.0f",
                                   f->cnt[k] ? 20.0 * log10(sqrt(f->sq[k] / f->cnt[k]) / 8388608.0 + 1e-12) : -999.0);
                lanes[sizeof lanes - 1] = 0;
                fprintf(stderr, "[APU-FIFO] %c%c%d: %ld transfers, %.0f bytes each; lane dBFS%s\n",
                        a ? 'E' : 'G', b ? 'o' : 'i', c, f->n, (double)f->bytes / f->n, lanes);
            }
    memset(s_fs, 0, sizeof s_fs);
}

static void gp_fifo_rw(void *opaque, uint8_t *ptr, unsigned int index, size_t len, bool dir)
{
    MCPXAPUState *d = opaque;
    if (dir) {
        if (index >= GP_OUTPUT_FIFO_COUNT)
            return;
        fifo_note(0, 1, index, ptr, len);
        fifo_rw(d, 1,
                NV_PAPU_GPOFBASE0 + 0x10 * index, NV_PAPU_GPOFEND0 + 0x10 * index,
                NV_PAPU_GPOFCUR0 + 0x10 * index, ptr, len, dir);
    } else {
        if (index >= GP_INPUT_FIFO_COUNT) {
            memset(ptr, 0, len);
            return;
        }
        fifo_rw(d, 1,
                NV_PAPU_GPIFBASE0 + 0x10 * index, NV_PAPU_GPIFEND0 + 0x10 * index,
                NV_PAPU_GPIFCUR0 + 0x10 * index, ptr, len, dir);
        fifo_note(0, 0, index, ptr, len);
    }
}

static const int16_t s_silence[256][2];
/* Where the EP's final mix goes: one 256-sample period block. */
static int16_t (*s_sink_target)[2];

static void ep_fifo_rw(void *opaque, uint8_t *ptr, unsigned int index, size_t len, bool dir)
{
    MCPXAPUState *d = opaque;
    if (dir) {
        if (index >= EP_OUTPUT_FIFO_COUNT)
            return;
        fifo_note(1, 1, index, ptr, len);
        if (index == 0) {
            /* The final mix (xemu ep_sink_samples): to the device (the
             * period block the frame's caller chose, s_sink_target), and
             * silence to the AC'97 FIFO the toolkit does not play. */
            size_t n = len < sizeof(d->monitor.frame_buf) ? len : sizeof(d->monitor.frame_buf);
            memcpy(s_sink_target ? (void *)s_sink_target : (void *)d->monitor.frame_buf, ptr, n);
            s_sink_frames++;
            ptr = (uint8_t *)s_silence;
            if (len > sizeof(s_silence))
                len = sizeof(s_silence);
        }
        fifo_rw(d, 3,
                NV_PAPU_EPOFBASE0 + 0x10 * index, NV_PAPU_EPOFEND0 + 0x10 * index,
                NV_PAPU_EPOFCUR0 + 0x10 * index, ptr, len, dir);
    } else {
        if (index >= EP_INPUT_FIFO_COUNT) {
            memset(ptr, 0, len);
            return;
        }
        fifo_rw(d, 3,
                NV_PAPU_EPIFBASE0 + 0x10 * index, NV_PAPU_EPIFEND0 + 0x10 * index,
                NV_PAPU_EPIFCUR0 + 0x10 * index, ptr, len, dir);
        fifo_note(1, 0, index, ptr, len);
    }
}

/* ---- the memory windows ------------------------------------------------ */

/* One window range: `words` 32-bit words at guest `va`, backed by DSP memory
 * `space` from `dsp_off`. The shadow is what both sides agreed on last. */
typedef struct {
    uint32_t va, words, dsp_off;
    char space;
    uint32_t *shadow;
} WinRange;

static WinRange s_gp_ranges[] = {
    { GP_WIN + NV_PAPU_GPXMEM,   0x1000, 0,                  'X', NULL },
    { GP_WIN + NV_PAPU_GPMIXBUF, 0x0400, 0,                  'M', NULL },
    { GP_WIN + NV_PAPU_GPYMEM,   0x0800, 0,                  'Y', NULL },
    { GP_WIN + NV_PAPU_GPPMEM,   0x1000, 0,                  'P', NULL },
};
static WinRange s_ep_ranges[] = {
    { EP_WIN + NV_PAPU_EPXMEM,   0x0C00, 0,                  'X', NULL },
    { EP_WIN + NV_PAPU_EPYMEM,   0x0100, 0,                  'Y', NULL },
    { EP_WIN + NV_PAPU_EPPMEM,   0x1000, 0,                  'P', NULL },
};

/* Both syncs go 16 words (64 bytes) at a time and skip a block with nothing
 * to do: the windows are about 19,000 words a frame each way, 1,500 frames a
 * second, and word by word the two syncs were about 8% of the DSP stage. */
#define SYNC_BLOCK 16

/* CPU -> DSP: words the CPU changed since the last sync. */
static void sync_in(BwDsp *dsp, WinRange *r, int n)
{
    int k;
    for (k = 0; k < n; k++) {
        uint32_t *w = (uint32_t *)win(r[k].va), i, j;
        for (i = 0; i < r[k].words; i += SYNC_BLOCK) {
            if (!memcmp(w + i, r[k].shadow + i, SYNC_BLOCK * 4))
                continue;
            for (j = i; j < i + SYNC_BLOCK; j++) {
                if (w[j] == r[k].shadow[j])
                    continue;
                if (r[k].space == 'M')
                    bw_dsp_write(dsp, 'X', 0x1400 + j, w[j] & 0xFFFFFF);
                else
                    bw_dsp_write(dsp, r[k].space, r[k].dsp_off + j, w[j] & 0xFFFFFF);
                r[k].shadow[j] = w[j];
            }
        }
    }
}

/* DSP -> CPU: the DSP's memory, except words the CPU wrote while it ran.
 * The DSP memories hold 24-bit words only (bw_xdsp_init masks xemu's 0xCA
 * fill), so a block where window, shadow and DSP agree has nothing to do. */
static void sync_out(BwDsp *dsp, WinRange *r, int n)
{
    int k;
    for (k = 0; k < n; k++) {
        uint32_t *w = (uint32_t *)win(r[k].va), words, i, j, e;
        uint32_t *m = bw_dsp_mem(dsp, r[k].space, &words);
        if (!m)
            continue;
        m += r[k].dsp_off;
        if (words > r[k].words)
            words = r[k].words;
        for (i = 0; i < words; i += SYNC_BLOCK) {
            e = i + SYNC_BLOCK < words ? i + SYNC_BLOCK : words;
            if (!memcmp(w + i, m + i, (e - i) * 4) && !memcmp(w + i, r[k].shadow + i, (e - i) * 4))
                continue;
            for (j = i; j < e; j++) {
                if (w[j] != r[k].shadow[j])
                    continue;             /* the CPU's, pushed next frame */
                w[j] = m[j] & 0x00FFFFFFu;
                r[k].shadow[j] = w[j];
            }
        }
    }
}

/* xemu's memories start as 0xCA bytes; the hardware's words are 24-bit. */
static void mask_mem(BwDsp *dsp)
{
    static const char spaces[] = "XYPM";
    int s;
    for (s = 0; s < 4; s++) {
        uint32_t words, i, *m = bw_dsp_mem(dsp, spaces[s], &words);
        for (i = 0; m && i < words; i++)
            m[i] &= 0x00FFFFFFu;
    }
}

static void ranges_init(WinRange *r, int n)
{
    int k;
    for (k = 0; k < n; k++) {
        r[k].shadow = (uint32_t *)calloc(r[k].words, 4);
        /* Start from what the window holds: anything already there goes in
         * on the first sync_in only if it differs from zero. */
    }
}

/* ---- reset edges -------------------------------------------------------- */

static int running(uint32_t rst)
{
    return (rst & NV_PAPU_GPRST_GPRST) && (rst & NV_PAPU_GPRST_GPDSPRST);
}

enum { ACT_NONE, ACT_RESET, ACT_BOOT };

/* On the frame thread: a reset word that changed is turned into an action
 * for the DSP (done in frame order by whoever runs the DSP stage), and an
 * EPRST write re-aligns the EP period with the frame counter, as xemu's
 * ep_write does (its FIXME: frame sync). */
static int poll_reset(MCPXAPUState *d, uint32_t rst_va, uint32_t *last, int is_ep)
{
    uint32_t v = *(volatile uint32_t *)win(rst_va);
    int act = ACT_NONE;
    if (v == *last)
        return ACT_NONE;
    if (!running(v))
        act = ACT_RESET;
    else if (!running(*last))
        act = ACT_BOOT;
    if (is_ep)
        d->ep_frame_div = 0;
    *last = v;
    return act;
}

static void do_reset_act(MCPXAPUState *d, BwDsp *dsp, int act, uint32_t rst, int is_ep)
{
    if (act == ACT_RESET) {
        bw_dsp_reset(dsp);
    } else if (act == ACT_BOOT) {
        fprintf(stderr, "[APU-XDSP] %s bootstrapped (RST 0x%X)\n", is_ep ? "EP" : "GP", rst);
        if (s_survey)
            sge_survey_all(d, is_ep ? "EP bootstrap" : "GP bootstrap");
        bw_dsp_bootstrap(dsp);
    }
}

/* ---- P memory watch (BW_DSP_PWATCH=1, a test aid, au-dsp) ---------------
 *
 * One log line whenever a DSP's program memory changes: what changed it
 * ("boot", a bootstrap; "cpu", DirectSound's writes to the window, merged in
 * by sync_in; "run", the DSP itself during its frame: DMA into P or a
 * movem), how many words, their range and a hash of the whole memory. It
 * writes no memory contents anywhere (call A6: the DSP programs are never
 * written to disk). Inert when unset. */
static int s_pwatch = -1;
static uint32_t *s_pwatch_last[2];

static void pwatch_check(BwDsp *dsp, int is_ep, const char *why)
{
    uint32_t words, *p, first = 0, last = 0, n = 0, i, h = 2166136261u;
    if (s_pwatch < 0) {
        const char *e = getenv("BW_DSP_PWATCH");
        s_pwatch = e && e[0] == '1';
    }
    if (!s_pwatch)
        return;
    p = bw_dsp_mem(dsp, 'P', &words);
    if (!s_pwatch_last[is_ep])
        s_pwatch_last[is_ep] = (uint32_t *)calloc(words, 4);
    for (i = 0; i < words; i++) {
        if ((p[i] & 0xFFFFFFu) != s_pwatch_last[is_ep][i]) {
            if (!n)
                first = i;
            last = i;
            n++;
        }
    }
    if (!n)
        return;
    for (i = 0; i < words; i++) {
        s_pwatch_last[is_ep][i] = p[i] & 0xFFFFFFu;
        h = (h ^ s_pwatch_last[is_ep][i]) * 16777619u;
    }
    fprintf(stderr, "[APU-XDSP] P watch: %s P changed (%s) at frame %ld: %u words in"
                    " 0x%04X-0x%04X, P hash %08X\n", is_ep ? "EP" : "GP", why, s_frames, n,
            first, last, h);
}

/* ---- one frame of the DSP stage ----------------------------------------- */

/* What the frame thread hands the DSP stage for one 32-sample frame. */
typedef struct {
    uint32_t mix[NUM_MIXBINS * NUM_SAMPLES_PER_FRAME];   /* the VP's bins, 24-bit */
    int div;                                             /* ep_frame_div */
    uint8_t gp_act, ep_act, gp_on, ep_on;
    uint32_t gprst, eprst;
    int16_t (*out)[2];                                   /* this period's block */
} XFrame;

static void dsp_do_frame(MCPXAPUState *d, const XFrame *f)
{
    uint32_t *mix, words;
    int i;
    int64_t t0, t1, t2, t3, t4;

    bw_dsp_trace_frame(s_frames, f->div);       /* BW_DSP_TRACE's frame clock */
    t0 = qpc();
    do_reset_act(d, s_gp, f->gp_act, f->gprst, 0);
    do_reset_act(d, s_ep, f->ep_act, f->eprst, 1);
    if (s_pwatch) {
        pwatch_check(s_gp, 0, f->gp_act == ACT_BOOT ? "boot" : "reset");
        pwatch_check(s_ep, 1, f->ep_act == ACT_BOOT ? "boot" : "reset");
    }

    /* New SGE entries get their copy decided while the translations that
     * made them are fresh (also on DirectSound's register writes). */
    if (f->div % 8 == 0) {
        AcquireSRWLockExclusive(&s_sgc_lock);
        sgc_scan_locked(d);
        ReleaseSRWLockExclusive(&s_sgc_lock);
    }

    sync_in(s_gp, s_gp_ranges, 4);
    sync_in(s_ep, s_ep_ranges, 3);
    if (s_pwatch) {
        pwatch_check(s_gp, 0, "cpu");
        pwatch_check(s_ep, 1, "cpu");
    }

    /* The VP's mixbins into the GP's mix buffer (X:0x1400). */
    mix = bw_dsp_mem(s_gp, 'M', &words);
    memcpy(mix, f->mix, (words < NUM_MIXBINS * NUM_SAMPLES_PER_FRAME
                         ? words : NUM_MIXBINS * NUM_SAMPLES_PER_FRAME) * 4);
    t1 = qpc();

    if (f->gp_on) {
        uint32_t c = bw_dsp_run_frame(s_gp, BW_GP_MAX_CYCLES);
        s_gp_runs++;
        s_gp_cycles += c;
        if (c > s_gp_max)
            s_gp_max = c;
        {
            /* The GP never reached its end of frame: the cap stopped it. */
            static int shown, run;
            if (c >= BW_GP_MAX_CYCLES) {
                if (shown++ < 5)
                    fprintf(stderr, "[APU-XDSP] GP ran %u cycles without finishing its frame"
                                    " (frame %ld)\n", c, s_frames);
                if (++run == BW_CAP_FALLBACK * 8 && !s_fallback_pending) {
                    s_fallback_pending = 1;
                    fprintf(stderr, "[APU-XDSP] the GP program never finishes a frame: this boot"
                                    " switches to the fold-down\n");
                }
            } else {
                run = 0;
            }
        }
        if (!f->ep_on) {
            /* No EP: the GP's mix-buffer bins 0/1, as xemu monitors it. */
            int off = (f->div % 8) * NUM_SAMPLES_PER_FRAME;
            for (i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
                f->out[off + i][0] = (int16_t)(bw_dsp_read(s_gp, 'X', 0x1400 + i) >> 8);
                f->out[off + i][1] = (int16_t)(bw_dsp_read(s_gp, 'X', 0x1420 + i) >> 8);
            }
        }
    }
    t2 = qpc();
    if (s_pwatch)
        pwatch_check(s_gp, 0, "run");
    if (f->ep_on && f->div % 8 == 0) {
        uint32_t c;
        s_sink_target = f->out;
        c = bw_dsp_run_frame(s_ep, BW_EP_MAX_CYCLES);
        s_ep_runs++;
        s_ep_cycles += c;
        if (c > s_ep_max)
            s_ep_max = c;
        {
            /* The EP never reached its end of frame: the cap stopped it. */
            static int shown, run;
            if (c >= BW_EP_MAX_CYCLES) {
                if (shown++ < 5)
                    fprintf(stderr, "[APU-XDSP] EP ran %u cycles without finishing its frame"
                                    " (frame %ld)\n", c, s_frames);
                if (++run == BW_CAP_FALLBACK && !s_fallback_pending) {
                    s_fallback_pending = 1;
                    fprintf(stderr, "[APU-XDSP] the EP program never finishes a frame (DirectSound's"
                                    " Dolby encoder, in surround or AC3 mode): this boot switches to"
                                    " the fold-down\n");
                }
            } else {
                run = 0;
            }
        }
    }
    t3 = qpc();
    if (s_pwatch)
        pwatch_check(s_ep, 1, "run");

    sync_out(s_gp, s_gp_ranges, 4);
    sync_out(s_ep, s_ep_ranges, 3);
    t4 = qpc();

    s_t_gp += t2 - t1;
    s_t_ep += t3 - t2;
    s_t_sync += (t1 - t0) + (t4 - t3);
    s_t_all += t4 - t0;
    if (t4 - t0 > s_t_frame_max)
        s_t_frame_max = t4 - t0;

    if (s_survey && (s_frames == 1500 * 2 || s_frames == 1500 * 20))
        sge_survey_all(d, s_frames == 1500 * 2 ? "2 s" : "20 s");
    if (++s_frames % (1500 * 5) == 0) {
        /* Host time per second of audio: milliseconds of one core. */
        double ms;
        if (!s_qpf) {
            LARGE_INTEGER fq;
            QueryPerformanceFrequency(&fq);
            s_qpf = fq.QuadPart;
        }
        ms = 1000.0 / (double)s_qpf / 5.0;
        fprintf(stderr, "[APU-XDSP] %ld frames: GP %s %ld runs, %.0f cycles avg (max %u);"
                        " EP %s %ld runs, %.0f avg (max %u); EP output frames %ld;"
                        " host %.1f ms/s (GP %.1f, EP %.1f, sync %.1f), longest frame %.3f ms\n",
                s_frames, f->gp_on ? "on" : "off", s_gp_runs,
                s_gp_runs ? (double)s_gp_cycles / s_gp_runs : 0.0, s_gp_max,
                f->ep_on ? "on" : "off", s_ep_runs,
                s_ep_runs ? (double)s_ep_cycles / s_ep_runs : 0.0, s_ep_max, s_sink_frames,
                s_t_all * ms, s_t_gp * ms, s_t_ep * ms, s_t_sync * ms,
                s_t_frame_max * 5.0 * ms);
        fallback_check(s_t_all * ms);
        fifo_report();
        s_gp_max = s_ep_max = 0;
        s_gp_runs = s_ep_runs = 0;
        s_gp_cycles = s_ep_cycles = 0;
        s_t_all = s_t_gp = s_t_ep = s_t_sync = s_t_frame_max = 0;
    }
}

/* ---- the DSP thread ------------------------------------------------------ */

/* With BW_APU_DSP_THREAD on (the default), the DSP stage runs on its own
 * thread, a pipeline beside the frame thread: the frame thread renders the
 * VP's frames and queues each one's mixbins, and the DSP thread runs the GP
 * (and the EP) on them in order. Nothing waits until a period's output is
 * due: at the period's last frame the frame thread needs the EP block of that
 * same period, which the EP made at the period's first frame, so the DSP
 * thread may run up to 7 frames behind without adding any latency (the
 * output is the same period, sample for sample, as without the thread). In
 * that time the frame thread is free to render the next frames' voices, so
 * under load the two halves of the work (VP about 0.1-0.2 of a core, the
 * DSPs 0.35-0.7) no longer have to fit in one thread's time. The GP-only
 * case (no EP) writes a slice per frame and so waits for the period's last
 * frame instead. BW_APU_DSP_THREAD=0 runs the stage on the frame thread as
 * before. */
#define XRING 32                    /* frames queued (4 periods) */
#define XBLOCKS 4                   /* period blocks in flight */
static int s_thread_mode = -1;
static XFrame s_ring[XRING];
static int16_t s_blocks[XBLOCKS][256][2];
static volatile LONG64 s_produced, s_done;     /* frames queued / finished */
static SRWLOCK s_ring_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE s_ring_cv = CONDITION_VARIABLE_INIT;
static int64_t s_period_seq = -1, s_period_first;  /* frame thread's view */
static int64_t s_wait_us, s_wait_max_us;           /* frame thread waiting */
static long s_waits, s_apu_periods;

static int thread_mode(void)
{
    if (s_thread_mode < 0) {
        const char *e = getenv("BW_APU_DSP_THREAD");
        s_thread_mode = !(e && e[0] == '0');
    }
    return s_thread_mode;
}

static int dsp_frame_guarded(MCPXAPUState *d, const XFrame *f)
{
    __try {
        dsp_do_frame(d, f);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static int shown;
        if (shown++ < 8)
            fprintf(stderr, "[APU-XDSP] DSP thread: exception 0x%08lX in frame %ld (div %d);"
                            " frame skipped\n", GetExceptionCode(), s_frames, f->div);
        return 0;
    }
    return 1;
}

static DWORD WINAPI dsp_thread(LPVOID arg)
{
    MCPXAPUState *d = arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    { extern void bw_audio_thread_mmcss(const char *who); bw_audio_thread_mmcss("DSP"); }   /* au-out: bw_apu_out.c */
    fprintf(stderr, "[APU-XDSP] DSP thread %lu running\n", GetCurrentThreadId());
    for (;;) {
        int64_t n;
        AcquireSRWLockExclusive(&s_ring_lock);
        while (s_done >= s_produced)
            SleepConditionVariableSRW(&s_ring_cv, &s_ring_lock, INFINITE, 0);
        n = s_done;
        ReleaseSRWLockExclusive(&s_ring_lock);

        if (g_bw_hot) {
            /* au-hot: this frame's wall time and the thread's own cycles in it */
            int64_t w0 = bw_hot_t(), c0 = bw_hot_cycles();
            dsp_frame_guarded(d, &s_ring[n % XRING]);
            bw_hot_dsp_frame(bw_hot_t() - w0, bw_hot_cycles() - c0);
        } else {
            dsp_frame_guarded(d, &s_ring[n % XRING]);
        }

        AcquireSRWLockExclusive(&s_ring_lock);
        s_done = n + 1;
        WakeAllConditionVariable(&s_ring_cv);
        ReleaseSRWLockExclusive(&s_ring_lock);
    }
    return 0;
}

/* The frame thread waits until the DSP thread has finished frame n. */
static void wait_done(int64_t n)
{
    int64_t t0 = 0, hot0 = 0;
    AcquireSRWLockExclusive(&s_ring_lock);
    if (s_done <= n) {
        t0 = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
        if (g_bw_hot) {
            hot0 = bw_hot_t();          /* au-hot: the wait, and how far behind the DSP thread was */
            bw_hot_dsp_lag((int)(n + 1 - s_done));
            bw_hot_stage(BW_ST_DSPWAIT, 0);   /* au-vp: the watch */
        }
        while (s_done <= n)
            SleepConditionVariableSRW(&s_ring_cv, &s_ring_lock, INFINITE, 0);
    }
    ReleaseSRWLockExclusive(&s_ring_lock);
    if (hot0) {
        bw_hot_seg(BW_HOT_DSPW, bw_hot_t() - hot0);
        bw_hot_stage(BW_ST_DSP, 0);
    }
    if (t0) {
        int64_t w = qemu_clock_get_us(QEMU_CLOCK_REALTIME) - t0;
        s_wait_us += w;
        s_waits++;
        if (w > s_wait_max_us)
            s_wait_max_us = w;
    }
}

/* ---- the public entry points ------------------------------------------ */

void bw_xdsp_init(MCPXAPUState *d)
{
    if (!bw_xdsp_enabled() || s_gp)
        return;
    bw_dsp_trace_init(d);                       /* BW_DSP_TRACE (a test aid) */
    /* The audio rework's gate (call D5) and the maintainer's answer of
     * 2026-11-04 (docs/audio/CALLS.md): every edition runs the translated
     * engine with its JIT, bit-identical to xemu's interpreter (the corpus,
     * lockstep in game and a 3 h Original lockstep soak: docs/audio/dsp.md,
     * V.md, orig.md). BW_DSP_ENGINE=interp keeps the interpreter (D-012's
     * reference); BW_DSP_ENGINE overrides either way. */
    {
        extern int bw_xdsp_edition_recomp(void);
        bw_dsp_engine_default(bw_xdsp_edition_recomp());
    }
    s_gp = bw_dsp_new(d, gp_scratch_rw, gp_fifo_rw, true);
    s_ep = bw_dsp_new(d, ep_scratch_rw, ep_fifo_rw, false);
    {
        const char *e = getenv("BW_APU_XDSP_SURVEY");
        s_survey = e && e[0] == '1';
    }
    mask_mem(s_gp);
    mask_mem(s_ep);
    ranges_init(s_gp_ranges, 4);
    ranges_init(s_ep_ranges, 3);
    if (thread_mode()) {
        HANDLE h = CreateThread(NULL, 0, dsp_thread, d, 0, NULL);
        if (h)
            CloseHandle(h);
        else
            s_thread_mode = 0;
    }
    fprintf(stderr, "[APU-XDSP] GP and EP on xemu's DSP56300 interpreter, %s\n",
            s_thread_mode ? "on their own thread (BW_APU_DSP_THREAD=0: on the frame thread)"
                          : "on the frame thread");
}

/* One 32-sample frame. Returns 1 when it handled the DSP stage. */
int bw_xdsp_frame(MCPXAPUState *d, float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    XFrame *f, local;
    int b, i, div;

    if (!bw_xdsp_enabled())
        return 0;
    if (!s_gp)
        bw_xdsp_init(d);
    if (!s_gp || !s_ep)
        return 0;
    if (d->ep_frame_div % 8 == 0) {
        if (s_fallback_pending)
            s_on = 0;                   /* switch at a period boundary */
        if (!s_on)
            return 0;
    }

    if (s_thread_mode) {
        /* a free slot: the DSP thread is at most XRING frames behind */
        if (s_produced - s_done >= XRING)
            wait_done(s_produced - XRING);
        f = &s_ring[s_produced % XRING];
    } else {
        f = &local;
    }

    f->gp_act = (uint8_t)poll_reset(d, GP_WIN + NV_PAPU_GPRST, &s_gprst, 0);
    f->ep_act = (uint8_t)poll_reset(d, EP_WIN + NV_PAPU_EPRST, &s_eprst, 1);
    f->gprst = s_gprst;
    f->eprst = s_eprst;
    f->gp_on = (uint8_t)running(s_gprst);
    f->ep_on = (uint8_t)running(s_eprst);
    div = f->div = d->ep_frame_div;
    for (b = 0; b < NUM_MIXBINS; b++)
        for (i = 0; i < NUM_SAMPLES_PER_FRAME; i++)
            f->mix[b * NUM_SAMPLES_PER_FRAME + i] = float_to_24b(mixbins[b][i]);

    if (!s_thread_mode) {
        f->out = d->monitor.frame_buf;
        s_sink_target = NULL;
        dsp_do_frame(d, f);
        return 1;
    }

    /* the period this frame belongs to, and its output block */
    if (div % 8 == 0 || s_period_seq < 0) {
        s_period_seq++;
        s_period_first = s_produced;
        memset(s_blocks[s_period_seq % XBLOCKS], 0, sizeof s_blocks[0]);
    }
    f->out = s_blocks[s_period_seq % XBLOCKS];

    AcquireSRWLockExclusive(&s_ring_lock);
    s_produced++;
    WakeAllConditionVariable(&s_ring_cv);
    ReleaseSRWLockExclusive(&s_ring_lock);

    /* The period's last frame: monitor_frame hands frame_buf to the device
     * right after this, so the period's output must be in it. With the EP
     * that is the block its run at the period's first frame filled. */
    if ((div + 1) % 8 == 0) {
        wait_done(f->ep_on ? s_period_first : s_produced - 1);
        memcpy(d->monitor.frame_buf, s_blocks[s_period_seq % XBLOCKS], sizeof s_blocks[0]);
        if (++s_apu_periods % 938 == 0) {
            /* every 5 s, from the frame thread */
            fprintf(stderr, "[APU-XDSP] frame thread waited for the DSP thread %ld"
                            " times in 5 s, %.1f ms in all (longest %.2f ms)\n",
                    s_waits, s_wait_us / 1000.0, s_wait_max_us / 1000.0);
            s_waits = 0;
            s_wait_us = s_wait_max_us = 0;
        }
    }
    return 1;
}

/* ---- surround output (Remastered, BW_FIX AUDIO_SURROUND) ----------------
 *
 * With a 6-channel (or larger) audio device, the game is told "surround"
 * (src/bw_audio_hooks.c, XGetAudioFlags) and the output is 5.1. In that mode
 * DirectSound sends 3D sources' front and rear gains to the 3D front bins
 * (6/7) and rear bins (8/9) (TK-18 notes, seen), so the bins hold a real
 * surround mix. The DSPs can't make it: told surround, DirectSound runs its
 * Dolby encoder on the EP, which never finishes a frame here (seen), so the
 * DSP stage stands down and the stub's bins become the six channels
 * (apu_dsp.c): FL = 0 + 6, FR = 1 + 7, C = 2, LFE = 3, BL = 4 + 8,
 * BR = 5 + 9. What surround loses is the GP's reverb (M28's zones only).
 * BW_AUDIO_SURROUND = auto (the edition decides; the default) | force (5.1
 * even on a stereo device, which XAudio2 then downmixes: for tests) | off.
 * Called by the carried apu_xaudio2.c at init, before any audio frame. */
#include "../../bw_edition.h"

/* The DSP engine's default (call D5): the translated engine in every edition
 * since 0.3.0 (lane au-orig, docs/audio/orig.md: Original's proof). A port
 * change, not an edition one: the engine is bit-identical to the
 * interpreter, so the game hears the same DSP output word for word. */
int bw_xdsp_edition_recomp(void)
{
    return 1;
}

static int s_surround_out, s_surround_decided;
static const char *s_surround_why = "";

int bw_surround_output_wanted(int device_channels)
{
    const char *e = getenv("BW_AUDIO_SURROUND");
    int want = bw_fix_enabled(BW_FIX_AUDIO_SURROUND) && device_channels >= 6;
    s_surround_why = want ? "Remastered AUDIO_SURROUND: a 6-channel audio device" : "";
    if (e && *e) {
        if (e[0] == 'f') {
            want = 1;
            s_surround_why = "BW_AUDIO_SURROUND=force";
        } else if (e[0] == 'o' || e[0] == '0') {
            want = 0;
            s_surround_why = "";
        }
    }
    s_surround_out = want;
    s_surround_decided = 1;
    if (want) {
        s_on = 0;                                 /* no frame has run yet */
        fprintf(stderr, "[APU-XDSP] 5.1 output: the game is told surround; the DSP stage stands"
                        " down (DirectSound's Dolby EP path doesn't run here) and the speaker"
                        " and 3D bins become the six channels\n");
    }
    return want;
}

/* For src/bw_audio_hooks.c: tell the game "surround"? -1 = not decided yet
 * (XAudio2 init hasn't run: the caller must not cache the answer), 0 or 1.
 * why, when given, gets what decided it ("" when not surround). Item 67. */
int bw_audio_surround_wanted(const char **why)
{
    if (why)
        *why = s_surround_why;
    return s_surround_decided ? s_surround_out : -1;
}

/* ---- BW_DSP_TRACE's view of the scatter-gather spaces (bw_dsp_trace.c) ---
 *
 * One entry's 4 KB as the DSP would read it (sg_rw's copy decision), for the
 * trace's snapshots: no FIFO position moves and nothing is logged. which: 0
 * GP scratch, 1 GP FIFO, 2 EP scratch, 3 EP FIFO. Returns the entry count. */
uint32_t bw_xdsp_peek_page(MCPXAPUState *d, int which, uint32_t entry, uint8_t *out)
{
    SgeCache *c;
    uint32_t base, max, prd;
    uint8_t *host;
    memset(out, 0, SG_PAGE);
    if (which < 0 || which > 3)
        return 0;
    c = &s_sgc[which];
    base = d->regs[c->base_reg];
    max = d->regs[c->max_reg];
    if (!base)
        return 0;
    if (entry > max)
        return max + 1;
    AcquireSRWLockExclusive(&s_sgc_lock);
    sgc_table(c, base);
    host = sgc_entry(c, entry, &prd);
    if (host)
        memcpy(out, host, SG_PAGE);
    ReleaseSRWLockExclusive(&s_sgc_lock);
    return max + 1;
}
