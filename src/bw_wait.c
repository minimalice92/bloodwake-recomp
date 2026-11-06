/* bw_wait.c - a short, precise, non-spinning wait for guest busy-loops.
 *
 * recomp_manual.c cannot include windows.h (the generated code's register
 * macros), so its hooks call this. 200 us on a high-resolution waitable timer
 * (Windows 10 1803+); without one, a yield. Used by the KickOff replacement
 * (sub_0012C9F0), where Sleep(1) overshot the vblank and cost frames. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "bw_wait.h"
#include "gpu/core/bwg_latch.h"   /* I1 (lane gfx-regen): the idle point's front-buffer rule */

extern void (*bwg_latch_wake_fn)(void);

extern volatile LONG g_bw_kickoffs;   /* KickOffs, for BW_KICKOFF_STATS */

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

void bw_short_wait(void)
{
    static __declspec(thread) HANDLE timer;
    static __declspec(thread) int tried;
    LARGE_INTEGER due;

    if (!timer && !tried) {
        tried = 1;
        timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                       TIMER_ALL_ACCESS);
    }
    if (!timer) {
        SwitchToThread();
        return;
    }
    due.QuadPart = -2000;                    /* 200 us, relative, in 100 ns units */
    if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE))
        WaitForSingleObject(timer, 5);
    else
        SwitchToThread();
}

/* The NV2A thread's idle wait. The toolkit's loop ends each pass with
 * Sleep(0), so it held a whole core even between frames. Here: spin as before
 * while work is recent (BW_ACK_IDLE_US, default 300 us since the last
 * segment), then nap on a high-resolution timer (BW_ACK_NAP_US, default
 * 200 us) or until the title's next KickOff sets the wake event, whichever
 * comes first. BW_ACK_NAP=0 keeps the spin. */
static HANDLE s_ack_wake;

void bw_ack_kick(void)
{
    InterlockedIncrement(&g_bw_kickoffs);
    if (s_ack_wake)
        SetEvent(s_ack_wake);
}

/* I1 (lane gfx-regen): a lock on a game thread wakes a napping graphics
 * thread (the loading screen's progress bar; the forced read), without
 * counting as a KickOff. */
static void bw_ack_wake(void)
{
    if (s_ack_wake)
        SetEvent(s_ack_wake);
}

void bw_ack_idle(int worked)
{
    static int mode = -1;
    static HANDLE timer;
    static LONGLONG idle_ticks, nap_100ns, last_work;
    LARGE_INTEGER now, due;
    HANDLE h[2];

    /* I1: the front-buffer rule and the forced reads, on this thread (it runs
     * the executor and the back end); a test of two counters when idle */
    bwg_latch_idle();

    if (mode < 0) {
        const char *e = getenv("BW_ACK_NAP");
        LARGE_INTEGER f;
        long idle_us = 300, nap_us = 200;
        mode = !(e && e[0] == '0');
        if ((e = getenv("BW_ACK_IDLE_US")) != NULL) idle_us = atol(e);
        if ((e = getenv("BW_ACK_NAP_US")) != NULL) nap_us = atol(e);
        QueryPerformanceFrequency(&f);
        idle_ticks = f.QuadPart * idle_us / 1000000;
        nap_100ns = nap_us * 10;
        if (mode) {
            s_ack_wake = CreateEventA(NULL, FALSE, FALSE, NULL);
            timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                           TIMER_ALL_ACCESS);
            if (!s_ack_wake || !timer)
                mode = 0;
            else
                bwg_latch_wake_fn = bw_ack_wake;
        }
    }
    if (!mode) {
        Sleep(0);
        return;
    }
    QueryPerformanceCounter(&now);
    if (worked || !last_work || now.QuadPart - last_work < idle_ticks) {
        if (worked || !last_work)
            last_work = now.QuadPart;
        Sleep(0);
        return;
    }
    due.QuadPart = -nap_100ns;
    if (!SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) {
        Sleep(0);
        return;
    }
    h[0] = s_ack_wake;
    h[1] = timer;
    WaitForMultipleObjects(2, h, FALSE, 5);
}

/* The KickOff hand-off (D7, 2026-10-27). The KickOff replacement in
 * recomp_manual.c waits for the NV2A thread to acknowledge its PFB flush.
 * It used to poll with bw_short_wait, so every KickOff lost up to a whole
 * timer period after the flush was already acknowledged: in Remastered's
 * high frame rate the game thread sat 49% in that wait while the NV2A thread
 * idled 56% (profile with callers, run 20261027-023300-r6-gpu-d7hfprof).
 * Now the ack thread signals an event as it clears the flush bit
 * (bw_flush_acked, carried xbox_memory_layout.c), and the wait wakes on that
 * event or on the same ~200 us timer as before, whichever comes first, so a
 * posted interrupt still gets a safe point at least that often. Same
 * register protocol, same waits; only the wake-up is prompt.
 * BW_KICKOFF_EVENT=0 restores the timer-only poll. */
static HANDLE s_flush_ack;
static int s_flush_mode = -1;

static int flush_mode(void)
{
    if (s_flush_mode < 0) {
        const char *e = getenv("BW_KICKOFF_EVENT");
        int m = !(e && e[0] == '0');
        if (m && !s_flush_ack)
            s_flush_ack = CreateEventA(NULL, FALSE, FALSE, NULL);   /* auto-reset */
        s_flush_mode = m && s_flush_ack;
    }
    return s_flush_mode;
}

void bw_flush_acked(void)
{
    if (s_flush_mode > 0)
        SetEvent(s_flush_ack);
}

void bw_flush_wait(void)
{
    static __declspec(thread) HANDLE timer;
    static __declspec(thread) int tried;
    LARGE_INTEGER due;
    HANDLE h[2];

    if (!flush_mode()) {
        bw_short_wait();
        return;
    }
    if (!timer && !tried) {
        tried = 1;
        timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                       TIMER_ALL_ACCESS);
    }
    due.QuadPart = -2000;                    /* 200 us, as bw_short_wait */
    if (!timer || !SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) {
        WaitForSingleObject(s_flush_ack, 1);
        return;
    }
    h[0] = s_flush_ack;
    h[1] = timer;
    {
        /* BW_KICKOFF_STATS=1: once a second, the KickOffs, how many waited,
         * and the time spent waiting (game thread). */
        static int stats = -1;
        static LARGE_INTEGER f, t_last;
        static double waited_ms;
        static unsigned waits, kicks_last;
        LARGE_INTEGER t0, t1;
        if (stats < 0) {
            const char *e = getenv("BW_KICKOFF_STATS");
            stats = e && e[0] == '1';
            QueryPerformanceFrequency(&f);
        }
        if (stats)
            QueryPerformanceCounter(&t0);
        WaitForMultipleObjects(2, h, FALSE, 5);
        if (stats) {
            QueryPerformanceCounter(&t1);
            waited_ms += (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
            waits++;
            if (!t_last.QuadPart)
                t_last = t1;
            if (t1.QuadPart - t_last.QuadPart >= f.QuadPart) {
                unsigned k = (unsigned)g_bw_kickoffs;
                fprintf(stderr, "[BW] kickoff: %u kickoffs, %u wait turns, %.1f ms waiting\n",
                        k - kicks_last, waits, waited_ms);
                kicks_last = k;
                waits = 0;
                waited_ms = 0.0;
                t_last = t1;
            }
        }
    }
}

volatile LONG g_bw_kickoffs;

/* P1: the walk one segment at a time (bw_wait.h). The NV2A thread used to
 * walk everything between the last PUT it saw and the register's PUT in one
 * pass and only then move GET, so a KickOff waiting for GET (more than
 * g_bw_kickoff_async segments outstanding) waited for the whole batch, and
 * the NV2A thread then idled while the game produced the next segment: in
 * Remastered both threads waited about a third of the time (profiles
 * -130336-p1vfprof3, -130419-p1vfprof15). Walking each recorded segment on
 * its own, with GET moved after each, releases the game after one segment.
 * The same words are walked in the same order, split at the PUTs the title
 * itself published. A held flip now holds right after its own segment, as
 * the console's FLIP_STALL does, instead of after whatever followed it in
 * the batch; that changes Original's rhythm, so this is on only with
 * Remastered's high frame rate (bw_hifps_boot) unless BW_PB_SEGWALK=1/0
 * says otherwise. Single producer (the game thread's KickOff), single
 * consumer (the NV2A thread). */
#define BW_PQ 64
static unsigned s_pq[BW_PQ];
static volatile LONG s_pq_head, s_pq_tail;
int g_bw_segwalk = -1;

static int segwalk_on(void)
{
    if (g_bw_segwalk < 0) {
        const char *e = getenv("BW_PB_SEGWALK");
        g_bw_segwalk = e && e[0] == '1';
    }
    return g_bw_segwalk;
}

void bw_segwalk_default(int on)
{
    const char *e = getenv("BW_PB_SEGWALK");
    g_bw_segwalk = e && *e ? e[0] == '1' : on;
    fprintf(stderr, "[BW] NV2A walk: %s (P1, BW_PB_SEGWALK)\n",
            g_bw_segwalk ? "one KickOff segment at a time, GET after each" : "to the newest PUT");
}

void bw_put_queue_push(unsigned put)
{
    LONG h;
    if (!segwalk_on())
        return;
    h = s_pq_head;
    if (h - s_pq_tail >= BW_PQ)
        return;                              /* full: the walk takes the register */
    s_pq[h & (BW_PQ - 1)] = put;
    InterlockedExchange(&s_pq_head, h + 1);  /* publish after the slot */
}

unsigned bw_put_queue_take(unsigned last_put, unsigned put_reg)
{
    if (!segwalk_on())
        return put_reg;
    while (s_pq_tail != s_pq_head) {
        LONG t = s_pq_tail;
        unsigned q = s_pq[t & (BW_PQ - 1)];
        /* inside (last_put, put_reg] in ring order; anything else is a PUT
         * already walked (the register was read first) or from before a
         * ring restart, and is dropped */
        int inside = put_reg >= last_put ? (q > last_put && q <= put_reg)
                                         : (q > last_put || q <= put_reg);
        InterlockedExchange(&s_pq_tail, t + 1);
        if (inside)
            return q;
    }
    return put_reg;
}

double bw_now_ms(void)
{
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}
