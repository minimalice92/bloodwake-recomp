/*
 * bwg_gate.c - the latency mode's shared parts (docs/gfx/arch.md 3.3, 3.8,
 * 4; lane gfx-lat11; see bwg_gate.h).
 *
 * Parity mode (the default, and Original always) keeps every wait inside the
 * flip on the NV2A thread: the pacing, the screen wait, the present and the
 * GPU limit (lat_flip). The latency mode moves the GPU limit to one gate
 * before the game pass (lat-q's jit_wait hook), so the walk never waits for
 * the GPU, and feeds that gate with the GPU's own timing:
 *   - which flips the GPU has finished (the back end's fence, seen by a
 *     watcher thread, which is the only thread-safe view of a single-threaded
 *     D3D11 device's progress);
 *   - the GPU's time per frame (timestamps where the back end has them,
 *     else the spacing of the done events while it was busy), so a pass can
 *     be aimed to hand its frame over as the GPU comes free (lat-q's
 *     JIT_PACE had the present slot only: q.md section 10's gap);
 *   - the measured display horizon: present -> screen (DXGI's frame
 *     statistics) or -> GPU done, published for the high frame rate's
 *     prediction (arch.md 3.8 item 4).
 * No graphics API here; no game state; nothing changes what is drawn.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bwg_core.h"
#include "bwg_gate.h"

static int s_mode, s_limit = 1, s_gpu_aim = 1, s_live;
static HANDLE s_prog;                       /* auto-reset: the GPU finished another flip */

#define GR 256u                             /* per-flip records, by flip number */
typedef struct {
    volatile uint32_t flip;                 /* the flip this record holds */
    volatile double present_ms;             /* its present call (or its flip's end), qpc ms */
    volatile double done_ms;                /* the watcher saw it finished, qpc ms */
    volatile double tb, te;                 /* GPU timestamps, GPU clock ms (te 0: none) */
} GateRec;
static GateRec s_rec[GR];
static volatile LONG s_sub, s_done;         /* the last flip submitted, finished */
static volatile double s_done_ms;           /* when the last finished one was seen */

/* Windows of samples (each written by one thread) and their published medians. */
typedef struct {
    double v[128];
    unsigned n, cap;
} Win;
static void win_add(Win *w, double x)
{
    w->v[w->n++ % w->cap] = x;
}
static double win_q(const Win *w, double q)
{
    double v[128];
    unsigned n = w->n < w->cap ? w->n : w->cap, i, j;
    if (!n)
        return 0.0;
    memcpy(v, w->v, n * sizeof v[0]);
    for (i = 1; i < n; i++)                 /* at most 128 values: insertion sort */
        for (j = i; j > 0 && v[j - 1] > v[j]; j--) {
            double x = v[j]; v[j] = v[j - 1]; v[j - 1] = x;
        }
    j = (unsigned)(q * (double)(n - 1) + 0.5);
    return (double)v[j < n ? j : n - 1];
}
static unsigned win_n(const Win *w)
{
    return w->n < w->cap ? w->n : w->cap;
}

/* watcher thread */
static Win s_w_busy = { {0}, 0, 32 };       /* done(k) - done(k-1), GPU busy at k's present */
static Win s_w_tail = { {0}, 0, 32 };       /* done(k) - present(k), GPU idle at k's present */
static Win s_w_pdone = { {0}, 0, 32 };      /* present -> done, every flip */
static volatile double s_busy_med, s_tail_med, s_pdone_med;
/* NV2A thread */
static Win s_w_ts = { {0}, 0, 32 };         /* timestamps: the GPU's own time per frame */
static Win s_w_tsgap = { {0}, 0, 32 };      /* timestamps: the GPU idle before a frame */
static Win s_w_off = { {0}, 0, 128 };       /* done_ms - te: the clocks' offset (min = the calibration) */
static Win s_w_lag = { {0}, 0, 128 };       /* the watcher's lag behind the calibrated end */
static Win s_w_scr = { {0}, 0, 32 };        /* present -> screen */
static volatile double s_ts_med, s_scr_med;
static volatile uint32_t s_scr_flip;        /* the last flip with a screen time */
static double s_te_prev;
static uint32_t s_te_prev_flip;
/* stats (per second) */
static unsigned long s_st_done, s_st_ts, s_st_scr, s_st_unfin_most;

void bwg_gate_config(int mode, int limit, int gpu_aim)
{
    s_mode = mode;
    s_limit = limit < 0 ? 0 : limit > 8 ? 8 : limit;
    s_gpu_aim = gpu_aim;
    if (mode && !s_prog)
        s_prog = CreateEventW(NULL, FALSE, FALSE, NULL);
}

int bwg_gate_mode(void)
{
    return s_mode;
}

int bwg_gate_limit(void)
{
    return s_limit;
}

int bwg_gate_gpu_aim(void)
{
    return s_gpu_aim;
}

void bwg_gate_live(int live)
{
    s_live = live;
    if (!live && s_prog)
        SetEvent(s_prog);                   /* a gate waiting on the GPU looks again */
}

void bwg_gate_submitted(uint32_t flip, double present_ms)
{
    GateRec *r = &s_rec[flip % GR];
    r->done_ms = 0.0;
    r->te = 0.0;
    r->tb = 0.0;
    r->present_ms = present_ms;
    r->flip = flip;
    InterlockedExchange(&s_sub, (LONG)flip);   /* a full barrier: the record first */
    {
        uint32_t u = flip - (uint32_t)s_done;
        if (u > s_st_unfin_most && u < 1000u)
            s_st_unfin_most = u;
    }
}

void bwg_gate_gpu_done(uint32_t flip, double done_ms)
{
    GateRec *r = &s_rec[flip % GR];
    uint32_t prev = (uint32_t)s_done;
    double prev_ms = s_done_ms;
    if (r->flip == flip) {
        double p = r->present_ms;
        r->done_ms = done_ms;
        if (p > 0.0 && done_ms >= p) {
            win_add(&s_w_pdone, done_ms - p);
            s_pdone_med = win_q(&s_w_pdone, 0.5);
            if (flip == prev + 1 && prev_ms > 0.0) {
                if (prev_ms > p) {                  /* busy with the one before at its present */
                    win_add(&s_w_busy, done_ms - prev_ms);
                    s_busy_med = win_q(&s_w_busy, 0.5);
                } else {
                    win_add(&s_w_tail, done_ms - p);
                    s_tail_med = win_q(&s_w_tail, 0.5);
                }
            }
        }
    }
    s_done_ms = done_ms;
    InterlockedExchange(&s_done, (LONG)flip);
    s_st_done++;
    if (s_prog)
        SetEvent(s_prog);
}

/* BW_TEST_GPUSTALL=<from_s>:<secs>:<ms> (test aid, the latency mode only,
 * never in a preset): from <from_s> s after the gate's first flip, for
 * <secs> s, the GPU is made to look <ms> per frame: the back end's done
 * watcher sees each flip no sooner than that after the one before (as
 * BW_TEST_LATSLOW does for a whole run), and the timestamps report that
 * long; so a slow stretch mid-mission, and the recovery after it, can be
 * tested on a fast GPU. */
double bwg_gate_test_stall_ms(void)
{
    static int parsed;
    static double t0, from, len, ms;
    double now = bwg_qpc_ms();
    if (!parsed) {
        const char *e = getenv("BW_TEST_GPUSTALL");
        parsed = 1;
        t0 = now;
        if (e && *e && sscanf(e, "%lf:%lf:%lf", &from, &len, &ms) == 3 && len > 0.0 && ms > 0.0)
            fprintf(stderr, "[GPU-GATE] BW_TEST_GPUSTALL: from %.1f s for %.1f s, the GPU looks %.1f ms a frame"
                            " (test aid)\n", from, len, ms);
        else
            ms = 0.0;
    }
    if (ms <= 0.0)
        return 0.0;
    return now >= t0 + from * 1000.0 && now < t0 + (from + len) * 1000.0 ? ms : 0.0;
}

void bwg_gate_gpu_times(uint32_t flip, double begin_ms, double end_ms)
{
    GateRec *r = &s_rec[flip % GR];
    double start, stall = bwg_gate_test_stall_ms();
    if (end_ms <= 0.0 || end_ms < begin_ms)
        return;
    if (stall > 0.0)
        end_ms = begin_ms + stall;      /* BW_TEST_GPUSTALL (test aid) */
    /* the GPU's own time for the frame: from when it reached the frame (or
     * finished the one before, if later) to its end, on the GPU's clock */
    start = begin_ms;
    if (s_te_prev_flip + 1 == flip && s_te_prev > start)
        start = s_te_prev;
    if (s_te_prev_flip + 1 == flip && begin_ms > s_te_prev)
        win_add(&s_w_tsgap, begin_ms - s_te_prev);
    else if (s_te_prev_flip + 1 == flip)
        win_add(&s_w_tsgap, 0.0);
    win_add(&s_w_ts, end_ms - start);
    s_ts_med = win_q(&s_w_ts, 0.5);
    s_te_prev = end_ms;
    s_te_prev_flip = flip;
    s_st_ts++;
    if (r->flip == flip) {
        r->tb = begin_ms;
        r->te = end_ms;
        if (r->done_ms > 0.0) {
            /* the watcher saw the end no sooner than it happened: the
             * smallest offset over the last 128 is the clocks' calibration
             * (as NTP's best sample); the rest is the watcher's lag */
            double off = r->done_ms - end_ms, best;
            win_add(&s_w_off, off);
            best = win_q(&s_w_off, 0.0);
            win_add(&s_w_lag, off - best);
        }
    }
}

void bwg_gate_screen(uint32_t flip, double screen_ms)
{
    GateRec *r = &s_rec[flip % GR];
    if (r->flip != flip || r->present_ms <= 0.0 || screen_ms < r->present_ms
            || screen_ms - r->present_ms > 1000.0)
        return;
    win_add(&s_w_scr, screen_ms - r->present_ms);
    s_scr_med = win_q(&s_w_scr, 0.5);
    s_scr_flip = flip;
    s_st_scr++;
}

uint32_t bwg_gate_unfinished(void)
{
    uint32_t u;
    if (!s_live)
        return 0;
    u = (uint32_t)s_sub - (uint32_t)s_done;
    return u < 0x80000000u ? u : 0;
}

uint32_t bwg_gate_done_count(void)
{
    return (uint32_t)s_done;
}

double bwg_gate_gpu_period(void)
{
    if (win_n(&s_w_ts) >= 16)
        return s_ts_med;
    if (win_n(&s_w_busy) >= 16)
        return s_busy_med;
    return 0.0;
}

double bwg_gate_gpu_free_ms(void)
{
    uint32_t d = (uint32_t)s_done, sub = (uint32_t)s_sub, j;
    double t = s_done_ms, b = bwg_gate_gpu_period(), tail = s_tail_med;
    if (!s_live || sub == d || (uint32_t)(sub - d) > 8u || t <= 0.0 || b <= 0.0)
        return 0.0;
    /* each unfinished flip ends one GPU frame after the one before it, or,
     * if the GPU was idle when it came, its usual tail after its present */
    for (j = d + 1; j != sub + 1; j++) {
        const GateRec *r = &s_rec[j % GR];
        double a = t + b, c = 0.0;
        if (r->flip == j && r->present_ms > 0.0 && win_n(&s_w_tail) >= 8)
            c = r->present_ms + tail;
        t = a > c ? a : c;
    }
    return t;
}

int bwg_gate_wait_progress(DWORD timeout_ms)
{
    if (!s_prog)
        return 0;
    return WaitForSingleObject(s_prog, timeout_ms) == WAIT_OBJECT_0;
}

double bwg_gate_horizon_ms(void)
{
    /* on screen: the screen's own times, while they keep coming */
    if (win_n(&s_w_scr) >= 8 && (uint32_t)s_sub - s_scr_flip < 64u)
        return s_scr_med;
    return win_n(&s_w_pdone) >= 8 ? s_pdone_med : 0.0;
}

void bwg_gate_stats_line(void)
{
    if (!s_mode)
        return;
    fprintf(stderr, "[GPU-GATE] GPU done %lu flips, on the GPU %.2f ms a frame (%s; idle before a frame p50"
                    " %.2f), present -> GPU done p50 %.2f, horizon %.2f ms, most unfinished %lu | the done"
                    " watcher's lag behind the timestamps p50 %.3f p99 %.3f max %.3f ms (%lu timestamped)\n",
            s_st_done, bwg_gate_gpu_period(), win_n(&s_w_ts) >= 16 ? "timestamps" : "done events",
            win_q(&s_w_tsgap, 0.5), s_pdone_med, bwg_gate_horizon_ms(), s_st_unfin_most,
            win_q(&s_w_lag, 0.5), win_q(&s_w_lag, 0.99), win_q(&s_w_lag, 1.0), s_st_ts);
    if (s_st_scr)
        fprintf(stderr, "[GPU-GATE] present -> screen p50 %.2f ms (%lu flips)\n", s_scr_med, s_st_scr);
    s_st_done = s_st_ts = s_st_scr = s_st_unfin_most = 0;
}
