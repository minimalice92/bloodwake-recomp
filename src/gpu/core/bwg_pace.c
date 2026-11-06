/*
 * bwg_pace.c - the renderer's API-neutral core (docs/gfx/arch.md 3.2): the
 * present's pacing schedule (B1's steady schedule, the head start's beat, the
 * frame-cap watch), the screen-wait policy, the letterbox rectangle and their
 * BW_GPU_STATS lines, and the present side's wait flags the high frame rate's
 * just-in-time frames read (lat-q).
 *
 * Lifted verbatim from bw_d3d11.c (lane gfx-core, gate GC: the same decisions
 * in the same order, the same floating-point expressions), so every back end
 * paces and letterboxes the same way. No graphics API here: the back end
 * calls bwg_pace_present() at the flip before its present, bwg_screen_wait()
 * with its swap chain's frame-latency waitable, bwg_letterbox() for the
 * picture's rectangle.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bwg_core.h"
#include "bwg_pace.h"
#include "bwg_gate.h"

static DWORD s_screen_wait_ms;          /* P2b: bwg_pace_config */
/* lat-q: 1 while the NV2A thread waits on the present side (the present
 * pacing, the screen, the GPU limit): BW_HIFPS_JIT=2 waits only then. */
volatile LONG g_bw_present_waiting;
/* ... and whether the last finished flip waited there at all (BW_HIFPS_JIT=2
 * reads it: a flip that waited means the next frame would queue). */
volatile LONG g_bw_last_flip_waited;
/* ... and when the last flip reached the back end, before any of those
 * waits (qpc ms): BW_HIFPS_JIT learns from it how long a pass takes to
 * get its frame to the present. */
volatile double g_bw_flip_ready_ms;
/* ... and 1 while the present pacing itself waits (that frame's present
 * hasn't happened yet: the next beat is one period after its slot). */
volatile LONG g_bw_pace_waiting;
/* ... and 1 while the frame cap is being reached (cap_watch, below:
 * BW_HIFPS_JIT=3 turns just-in-time frames on only then). */
volatile LONG g_bw_cap_reached;
static int s_cap_watch;                 /* bwg_cap_watch */
/* ... and the flips the back end has finished (after the GPU limit's wait):
 * BW_HIFPS_JIT=4 counts the frames not yet through the NV2A thread. */
volatile LONG g_bw_flips_done;
/* ... and the presents the pacing has let through (its schedule advanced):
 * BW_HIFPS_JIT=4 aims a pass at the slot after the ones still to come. */
volatile LONG g_bw_paced_n;
/* lat-q: set by the present side's waits during a flip (the pacing, the
 * screen, the GPU limit); the back end clears it at the flip's start and
 * copies it to g_bw_last_flip_waited at its end. */
int bwg_flip_waited;
static unsigned s_screen_timeouts, s_screen_st_n;
static double s_screen_st_ms;

/* The screen wait's fallback, made visible (0.2.5, for C2's item 64): one
 * line when four timeouts in a row turn the wait to zero (frames are then
 * dropped at once until the screen takes one), one when it takes one again.
 * The first ten of each. */
static void screen_wait_note(int engaged)
{
    static unsigned n[2];
    if (++n[engaged] <= 10)
        fprintf(stderr, engaged
                ? "[GPU-PRESENT] the screen didn't take a frame in 4 waits in a row (frame %u): frames are"
                  " dropped without waiting until it takes one%s\n"
                : "[GPU-PRESENT] the screen takes frames again (frame %u)%s\n",
                (unsigned)bwg_gen, n[engaged] == 10 ? "; later ones aren't logged" : "");
}

/* BW_TEST_SCREENWAIT=<from_frame>:<count> (test aid, every edition, off by
 * default, never in a preset): from flip <from_frame> on, the next <count>
 * screen waits are treated as timed out (the timeout's time is slept, the
 * waitable isn't touched), so the fallback above can be tested on a screen
 * that is awake. */
static int screen_test_timeout(void)
{
    static int parsed;
    static unsigned from, left;
    if (!parsed) {
        const char *e = getenv("BW_TEST_SCREENWAIT");
        parsed = 1;
        if (e && *e && sscanf(e, "%u:%u", &from, &left) == 2 && left)
            fprintf(stderr, "[GPU-PRESENT] BW_TEST_SCREENWAIT: %u screen waits from flip %u treated as"
                            " timed out (test aid)\n", left, from);
        else
            left = 0;
    }
    if (!left || bwg_gen < from)
        return 0;
    left--;
    return 1;
}

/* P2b: even presents at the frame cap (Remastered high frame rate).
 *
 * The flip hold paces the walk on a tick of the host clock at the target
 * rate, but the present itself is made at the FLIP_STALL, before the hold,
 * and a frame that came late is let through at once: so near the cap two
 * presents could come well under one refresh apart (seen: 40% of present
 * intervals under 2.78 ms in a battle at 282 fps, 64% in mission 10 at 335,
 * off screen). On a 360 Hz variable-refresh screen a frame that comes
 * sooner than the screen can refresh tears or waits. Here a present is made
 * no sooner than 1/target s after the previous one (a short wait on the
 * NV2A thread, which holds the walk and so the game, as the hold does).
 * bw_d3d11_pace_config sets the rate; BW_HIFPS_PACE=0 turns it off. */
static double s_pace_period_ms, s_pace_last_ms, s_pace_st_ms;
static unsigned s_pace_st_n;
static HANDLE s_pace_timer;
/* 0.2.5 (item 64's check): the presents keep to a schedule. Each one used to
 * be timed from the previous present as it actually happened, so every
 * timer's late wake-up (about 0.1 ms) added up and the fixed caps landed
 * 1-2% under (V on RC6: 240 -> 235.5, 144 -> 141.7, 120 -> 118.9, every
 * present paced). Now the next present is due one period after the last
 * one was due; a present a timer woke late for may come a little sooner
 * after it, but never sooner than PACE_FLOOR of a period (at the 360 Hz
 * display's 350 cap that is 2.79 ms, still longer than a refresh), and a
 * frame more than a period late starts the schedule again from now (no
 * catch-up burst). The timer also wakes PACE_SPIN_MS before, not 0.25 ms,
 * so it is rarely late at all. BW_HIFPS_PACE_ABS=0 restores the old timing. */
#define PACE_FLOOR   0.975
#define PACE_SPIN_MS 0.4
static double s_pace_next_ms;
static int s_pace_abs = 1;
/* lat-q: BW_HIFPS_PACE_ADAPT=1 (Remastered high frame rate, test switch):
 * when frames come slower than the cap (the GPU or the game the slower
 * side), the schedule's period follows what the frames sustain: the median
 * of the last 32 present intervals times PACE_ADAPT_K, never below the cap's
 * period. Seen on the maintainer's screen with just-in-time frames: presents
 * followed each pass's own work (a pass with a game step takes ~2x one
 * without), so their intervals spread 1.7 -> 3.2 ms; with a steady beat to
 * aim at, JIT starts each pass its own work before the beat instead. */
#define PACE_ADAPT_K 0.96
static int s_pace_adapt;
static float s_pace_iv[32];
static unsigned s_pace_ivn;

/* lat-q: BW_HIFPS_JIT=4 (the head start, a test switch): the schedule's
 * period follows what the frames sustain. Replayed offline on the quiet
 * slot's JIT logs (tools/bw_headstart.py, inferred): a steady beat at the
 * frames' own rate, each pass started its own work before its beat even
 * while the frame before is still on the NV2A thread, spreads the presents
 * ~1.4 ms instead of JIT's 5.6 (as shipped 2.1). The beat: 2% longer when
 * 4 of the last 32 presents came over a tenth of a period late, 1% shorter
 * after 32 with none late; never under the cap's period or over 100 ms. */
static int s_beat_auto;
static double s_beat_T;
static unsigned char s_bl[32];
static unsigned s_bl_head, s_bl_n, s_bl_sum;
static unsigned long s_beat_up, s_beat_down;

void bwg_beat_auto(int on)
{
    s_beat_auto = on;
}

/* gfx-lat11: in the latency mode's head start (BW_HIFPS_LATMODE=2) the beat
 * never runs faster than the GPU's own measured time per frame
 * (BW_HIFPS_LATMODE_GPUAIM=0: no such floor); otherwise (BW_HIFPS_JIT=4) the
 * cap's period, as before. */
static double beat_floor(void)
{
    double g = bwg_gate_mode() == 2 && bwg_gate_gpu_aim() ? bwg_gate_gpu_period() : 0.0;
    if (g > 100.0)
        g = 100.0;                      /* as the beat itself: never over 100 ms */
    return g > s_pace_period_ms ? g : s_pace_period_ms;
}

/* gfx-lat11: what one pass that runs a game step needs before its present
 * (its work and the walk's tail, the hook's measure; 0 = unknown). Set each
 * pass in the latency mode's head start (BW_HIFPS_LATMODE=2). */
volatile double g_bwg_beat_need_ms;
static unsigned long s_beat_back;

static void beat_tune(int late)
{
    double fl = beat_floor();
    if (s_beat_T < fl)
        s_beat_T = fl;
    if (bwg_gate_mode() == 2 && g_bwg_beat_need_ms > 0.0) {
        /* gfx-lat11: the beat back to the GPU's time when it sits far above
         * both that and what a stepping pass needs. Seen at a mission's start
         * on a busy PC: the GPU's frames took ~200 ms for a few seconds, the
         * floor raised the beat there, and then it came down 1% per 32
         * presents: 5-7 fps for minutes with the GPU at 3-5 ms a frame. In
         * steady play the beat sits near the GPU's time, far under 1.5 x a
         * stepping pass's need, so this never fires there (inferred from the
         * quiet windows: beat ~4.7 ms, a stepping pass ~8-10 ms). */
        double need = fl > g_bwg_beat_need_ms ? fl : g_bwg_beat_need_ms;
        if (s_beat_T > need * 1.5) {
            s_beat_T = fl;
            s_bl_n = s_bl_sum = 0;
            s_beat_back++;
        }
    }
    if (s_bl_n == 32)
        s_bl_sum -= s_bl[s_bl_head];
    else
        s_bl_n++;
    s_bl[s_bl_head] = (unsigned char)(late != 0);
    s_bl_sum += late != 0;
    s_bl_head = (s_bl_head + 1) % 32u;
    if (s_bl_sum >= 4) {
        s_beat_T = s_beat_T * 1.02 < 100.0 ? s_beat_T * 1.02 : 100.0;
        s_bl_n = s_bl_sum = 0;
        s_beat_up++;
    } else if (s_bl_n >= 32 && !s_bl_sum) {
        s_beat_T = s_beat_T * 0.99 > fl ? s_beat_T * 0.99 : fl;
        s_bl_n = 0;
        s_beat_down++;
    }
}

static double pace_period(void)
{
    float v[32];
    unsigned n = s_pace_ivn < 32u ? s_pace_ivn : 32u, i, j;
    double m;
    if (s_beat_auto && s_beat_T > s_pace_period_ms)
        return s_beat_T;
    if (!s_pace_adapt || n < 16)
        return s_pace_period_ms;
    memcpy(v, s_pace_iv, n * sizeof v[0]);
    for (i = 1; i < n; i++)
        for (j = i; j > 0 && v[j - 1] > v[j]; j--) {
            float x = v[j]; v[j] = v[j - 1]; v[j - 1] = x;
        }
    m = (double)v[n / 2] * PACE_ADAPT_K;
    return m > s_pace_period_ms ? m : s_pace_period_ms;
}
static double s_pace_st_gap = -1.0;     /* BW_GPU_STATS: the shortest gap between presents */

void bwg_pace_config(unsigned hz)
{
    const char *e = getenv("BW_HIFPS_PACE");
    s_pace_period_ms = (hz && !(e && e[0] == '0')) ? 1000.0 / (double)hz : 0.0;
    e = getenv("BW_HIFPS_SCREENWAIT");
    s_screen_wait_ms = hz && !(e && e[0] == '0') ? 8u : 0u;
    e = getenv("BW_HIFPS_PACE_ABS");
    s_pace_abs = !(e && e[0] == '0');
    s_pace_next_ms = 0.0;
    e = getenv("BW_HIFPS_PACE_ADAPT");                  /* lat-q */
    s_pace_adapt = s_pace_abs && e && e[0] == '1';
    s_pace_ivn = 0;
    if (s_pace_adapt && s_pace_period_ms > 0.0)
        fprintf(stderr, "[GPU-PRESENT] the schedule follows the frames' own rate when they come slower"
                        " than the cap (BW_HIFPS_PACE_ADAPT)\n");
    if (s_pace_period_ms > 0.0)
        fprintf(stderr, "[GPU-PRESENT] presents at least %.3f ms apart (%u per second%s; BW_HIFPS_PACE=0 turns it off)\n",
                s_pace_period_ms, hz, s_pace_abs ? ", on a steady schedule" : ", timed from the last present (BW_HIFPS_PACE_ABS=0)");
}

/* When the next present is due (host ms, bwg_qpc_ms), the one answer for the
 * limiter below and for anything that aims a frame at it (the latency
 * batch's JIT pacing): the schedule, but never sooner than PACE_FLOOR of a
 * period after the last present. */
static double pace_due_ms(void)
{
    if (s_pace_abs && s_pace_next_ms > 0.0) {
        double floor_ms = s_pace_last_ms + pace_period() * PACE_FLOOR;
        return s_pace_next_ms > floor_ms ? s_pace_next_ms : floor_ms;
    }
    return s_pace_last_ms + s_pace_period_ms;
}

/* lat-q: is the frame cap being reached? (BW_HIFPS_JIT=3: just-in-time
 * frames only while it is; bw_d3d11_cap_watch turns the watch on). Seen at
 * a 120 cap: just-in-time frames keep the presents even there (spread 0.63
 * ms) with the picture 16.7 -> 7.1 ms old; uncapped they spread the
 * presents 2.2 -> 6.0 ms. Over the last 32 presents: reached when the
 * median gap is within 2% of the cap's period and at most 4 came late (more
 * than a tenth of a period after their slot); no longer reached when the
 * median is 4% over or 12 came late. Each state holds at least 1 s; a
 * release within 10 s of an engage (the cap didn't hold with the frames
 * just in time; seen on a busy PC at a 90 cap: releases ~4 s after each
 * engage, every ~5.5 s) makes the next engage wait 2 s, then 4, 8 ... 30 s
 * (none again once an engage holds 10 s). The first twenty transitions are logged, all
 * are counted (BW_GPU_STATS). */
static float s_cap_iv[32];
static unsigned char s_cap_late[32];
static unsigned s_cap_ivn;
static double s_cap_since_ms, s_cap_backoff_ms, s_cap_engaged_ms;
static unsigned long s_cap_on_n, s_cap_off_n, s_cap_st_on, s_cap_st_n, s_cap_st_late;

void bwg_cap_watch(int on)
{
    s_cap_watch = on;
}

static void cap_watch(double gap, double now, int late)
{
    float v[32];
    unsigned n, i, j, nl = 0;
    double med, p = s_pace_period_ms;
    if (!s_cap_watch || p <= 0.0)
        return;
    s_cap_st_n++;
    s_cap_st_on += g_bw_cap_reached != 0;
    s_cap_st_late += late != 0;
    s_cap_late[s_cap_ivn % 32u] = (unsigned char)(late != 0);
    s_cap_iv[s_cap_ivn++ % 32u] = (float)(gap < 1000.0 ? gap : 1000.0);
    n = s_cap_ivn < 32u ? s_cap_ivn : 32u;
    if (n < 32 || now - s_cap_since_ms < 1000.0)
        return;
    memcpy(v, s_cap_iv, n * sizeof v[0]);
    for (i = 1; i < n; i++)
        for (j = i; j > 0 && v[j - 1] > v[j]; j--) {
            float x = v[j]; v[j] = v[j - 1]; v[j - 1] = x;
        }
    for (i = 0; i < n; i++)
        nl += s_cap_late[i];
    med = (double)v[n / 2];
    if (!g_bw_cap_reached) {
        if (med <= p * 1.02 && nl <= 4 && now - s_cap_since_ms >= s_cap_backoff_ms) {
            g_bw_cap_reached = 1;
            s_cap_since_ms = s_cap_engaged_ms = now;
            if (++s_cap_on_n <= 20)
                fprintf(stderr, "[GPU-PRESENT] the frame cap is reached (median gap %.2f ms, cap %.2f, %u of 32"
                                " late): just-in-time frames on (frame %u)\n", med, p, nl, (unsigned)bwg_gen);
        }
    } else {
        if (now - s_cap_engaged_ms > 10000.0)
            s_cap_backoff_ms = 0.0;             /* held long enough: no backoff */
        if (med >= p * 1.04 || nl >= 12) {
            g_bw_cap_reached = 0;
            if (now - s_cap_engaged_ms < 10000.0)
                s_cap_backoff_ms = s_cap_backoff_ms < 2000.0 ? 2000.0
                                 : s_cap_backoff_ms * 2.0 > 30000.0 ? 30000.0 : s_cap_backoff_ms * 2.0;
            s_cap_since_ms = now;
            if (++s_cap_off_n <= 20)
                fprintf(stderr, "[GPU-PRESENT] the frame cap is no longer reached (median gap %.2f ms, cap %.2f,"
                                " %u of 32 late, %.1f s on): just-in-time frames off (frame %u; the next engage"
                                " no sooner than %.0f s)\n", med, p, nl, (now - s_cap_engaged_ms) / 1000.0,
                        (unsigned)bwg_gen, (s_cap_backoff_ms > 1000.0 ? s_cap_backoff_ms : 1000.0) / 1000.0);
        }
    }
}

void bwg_pace_present(void)
{
    double now, due;
    int late;
    if (s_pace_period_ms <= 0.0)
        return;
    now = bwg_qpc_ms();
    due = pace_due_ms();
    late = s_pace_last_ms > 0.0 && now > due + s_pace_period_ms * 0.1;   /* lat-q: cap_watch */
    if (s_beat_auto && s_pace_last_ms > 0.0 && now < due + pace_period() * 4.0)
        /* lat-q, BW_HIFPS_JIT=4: a frame over 4 periods late is a stall (a
         * load, a busy GPU), not a beat a little too fast: not counted, so
         * the beat doesn't climb through a stall and stay slow after it */
        beat_tune(now > due + pace_period() * 0.1);
    if (s_pace_last_ms > 0.0 && now < due) {
        double t0 = now, spin = s_pace_abs ? PACE_SPIN_MS : 0.25;
        g_bw_present_waiting = 1;       /* lat-q */
        g_bw_pace_waiting = 1;
        bwg_flip_waited = 1;
        /* sleep on a high-resolution timer to a little before, then spin
         * the rest: a timer can only end late, so the gap is never short */
        if (due - now > spin + 0.1) {
            LARGE_INTEGER d;
            if (!s_pace_timer)
                s_pace_timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* HIGH_RESOLUTION */,
                                                      TIMER_ALL_ACCESS);
            d.QuadPart = -(LONGLONG)((due - now - spin) * 10000.0);
            if (s_pace_timer && SetWaitableTimer(s_pace_timer, &d, 0, NULL, NULL, FALSE))
                WaitForSingleObject(s_pace_timer, 20);
        }
        while ((now = bwg_qpc_ms()) < due)
            YieldProcessor();
        g_bw_present_waiting = 0;
        g_bw_pace_waiting = 0;
        s_pace_st_ms += now - t0;
        s_pace_st_n++;
    }
    if (s_pace_adapt && s_pace_last_ms > 0.0 && now - s_pace_last_ms < 100.0)
        s_pace_iv[s_pace_ivn++ % 32u] = (float)(now - s_pace_last_ms);   /* lat-q */
    if (s_pace_last_ms > 0.0)
        cap_watch(now - s_pace_last_ms, now, late);                      /* lat-q */
    if (s_pace_abs) {
        double p = pace_period();
        s_pace_next_ms = (s_pace_last_ms > 0.0 && now - due < p) ? due + p : now + p;
    }
    if (s_pace_last_ms > 0.0 && (s_pace_st_gap < 0.0 || now - s_pace_last_ms < s_pace_st_gap))
        s_pace_st_gap = now - s_pace_last_ms;       /* BW_GPU_STATS: the shortest gap */
    s_pace_last_ms = now;
    InterlockedIncrement(&g_bw_paced_n);                                 /* lat-q */
}

/* lat-q: the present pacing's next slot (qpc ms): 1 and *due_ms while the
 * pacing is on and has paced a present; BW_HIFPS_JIT aims each pass at it. */
int bwg_present_due(double *due_ms)
{
    if (s_pace_period_ms <= 0.0 || s_pace_last_ms <= 0.0)
        return 0;
    *due_ms = pace_due_ms();            /* B1's schedule: the one answer */
    return 1;
}

/* lat-q: the schedule's period now (BW_HIFPS_PACE_ADAPT's, or the cap's) */
double bwg_pace_period(void)
{
    return pace_period();
}

void bwg_pace_stats_line(void)
{
    if (s_screen_st_n) {
        fprintf(stderr, "[GPU-PRESENT] waited for the screen %u times, %.1f ms\n", s_screen_st_n, s_screen_st_ms);
        s_screen_st_n = 0;
        s_screen_st_ms = 0.0;
    }
    if (s_cap_watch && s_cap_st_n) {
        fprintf(stderr, "[GPU-PRESENT] the frame cap reached for %lu of %lu presents, %lu late | just-in-time"
                        " frames on %lu times, off %lu this boot\n",
                s_cap_st_on, s_cap_st_n, s_cap_st_late, s_cap_on_n, s_cap_off_n);
        s_cap_st_on = s_cap_st_n = s_cap_st_late = 0;
    }
    if (s_beat_auto && s_beat_T > 0.0)
        fprintf(stderr, "[GPU-PRESENT] head start: the beat %.3f ms (%.1f per second), %lu longer and %lu shorter"
                        " this boot, %lu back to the GPU's time after a slow stretch\n", pace_period(),
                1000.0 / pace_period(), s_beat_up, s_beat_down, s_beat_back);
    if (!s_pace_st_n)
        return;
    fprintf(stderr, "[GPU-PRESENT] paced %u presents, %.1f ms waited, shortest gap %.3f ms\n",
            s_pace_st_n, s_pace_st_ms, s_pace_st_gap);
    s_pace_st_n = 0;
    s_pace_st_ms = 0.0;
    s_pace_st_gap = -1.0;
}


/* P2b (Remastered high frame rate): wait a little for the screen to take the
 * next frame instead of dropping it at once (seen on A 360 Hz screen: 13-54
 * frames a second dropped near the cap, each a skipped image). A screen that
 * keeps not taking frames (asleep: D7's case) gets the old zero wait after a
 * few timeouts, until it signals again. BW_HIFPS_SCREENWAIT=0 restores.
 * `waitable` is the swap chain's frame-latency waitable object. Returns 1
 * when it says a buffer is free; 0: drop this picture. */
int bwg_screen_wait(HANDLE waitable)
{
    DWORD to = (s_screen_wait_ms && s_screen_timeouts < 4) ? s_screen_wait_ms : 0, r;
    double tw = to ? bwg_qpc_ms() : 0.0;
    g_bw_present_waiting = to != 0;     /* lat-q */
    if (screen_test_timeout()) {    /* BW_TEST_SCREENWAIT (test aid): as if timed out */
        if (to)
            Sleep(to);
        r = WAIT_TIMEOUT;
    } else {
        r = WaitForSingleObject(waitable, to);
    }
    g_bw_present_waiting = 0;
    if (to) {
        double w = bwg_qpc_ms() - tw;
        if (w > 0.05) {
            s_screen_st_n++;
            s_screen_st_ms += w;
        }
        if (w > 0.2)
            bwg_flip_waited = 1;      /* lat-q */
    }
    if (r != WAIT_OBJECT_0) {
        if (to && ++s_screen_timeouts == 4)
            screen_wait_note(1);    /* the zero-wait fallback engages */
        return 0;
    }
    if (s_screen_timeouts >= 4)
        screen_wait_note(0);        /* the screen takes frames again */
    s_screen_timeouts = 0;
    return 1;
}

/* A 4:3 picture, as the console sends to the TV: the flipped surface is
 * often the 2x-wide anti-aliased 1280x480 one, which the GPU filters down.
 * The rectangle (x, y, w, h) of a picture wsk times wider than 4:3 in a
 * window cw_px x ch_px, centred, whole pixels at its top left. */
void bwg_letterbox(float wsk, unsigned cw_px, unsigned ch_px, float *x, float *y, float *pw, float *ph)
{
    float fw, fh, cw, ch, w, h;
    fw = 4.0f * wsk; fh = 3.0f;      /* widescreen (D5): the picture's own shape */
    cw = (float)cw_px; ch = (float)ch_px;
    if (cw * fh <= ch * fw) { w = cw; h = cw * fh / fw; }
    else { h = ch; w = ch * fw / fh; }
    *x = (float)(int)((cw - w) * 0.5f);
    *y = (float)(int)((ch - h) * 0.5f);
    *pw = w; *ph = h;
}
