/*
 * bw_latlog.c - controller-latency timestamps (lane lat-m, 2026-11-03; a test
 * aid in every edition, inert unless BW_LATLOG is set).
 *
 * Why: "the controller feels snappy" has to be chosen by numbers. This records,
 * on one clock (the host performance counter), each stage a press goes through
 * from the pad to the screen, so tools/bw_latency.py can say how long each
 * stage takes in each configuration (docs/latency/m.md):
 *
 *   P/R  the USB layer (carried usb_gamepad.c): when a scripted press (or its
 *        release) is due, and the first pad report that carries it. With
 *        BW_LATLOG set the pad script runs on this clock, to the microsecond
 *        (not GetTickCount's 15.6 ms), so the due time is exact.
 *   I    the game thread, at each drawn frame: a pad's block, as the game's
 *        own pad read (Pad_PollState, in Devices_Update just before) left it,
 *        whenever it changed. Read-only.
 *   S    the game thread, at the end of each running game step (Overlays_Update):
 *        the step's weapons clock, throttle and rudder (what BW_STEPLOG
 *        records) and the camera mode. Read-only.
 *   F/f  the game thread: each drawn frame (Graphics_HandleMessage message 8),
 *        its begin and end, the newest step it shows, and with what weight
 *        (Remastered's interpolation: nodes and camera), the back end's flip
 *        count at that moment.
 *   X    the NV2A thread: each back-end flip (one per FLIP_STALL, i.e. per
 *        game Present): its present call, the readback, the end.
 *   G    a watcher thread: when the GPU finished that flip's work (a D3D11
 *        fence signalled after the present).
 *   D/T  on screen only: the flip's DXGI present count and DXGI's frame
 *        statistics (which present reached the screen at which vblank).
 *   M    a flip that is being frame-dumped (RECOMP_FB_DUMP), for the
 *        cross-check against the pictures (tools/bw_latency.py check).
 *   U    the game thread: each Game_Update that ran steps, with the game's
 *        own step clock before and after, so each step's due time is known on
 *        the host clock (D-027: the game's wait for its next step, apart
 *        from the port's part).
 *   O    the USB layer: each report produced for port 0 (the report
 *        interval: how long a pad's change can wait for the next report).
 *   Q    the USB layer: each run of the title's USB interrupt handler (a
 *        finished report reaches the title's driver there).
 *   N    the game thread (bw_menu_hooks.c's FrontEnd_UpdateMenuNav wrapper):
 *        the menus' navigation moved this pass (menu press to screen without
 *        frame dumps).
 *   A    the APU (carried apu_core.c): each output period handed to the
 *        device, with its index in the WAV dump and the device queue, so a
 *        sound found in the WAV gets its hand-over time (press to sound).
 *
 * The events go into memory (BW_LATLOG_MAX of them, default 600000) and are
 * written as text when the run ends (the run limit, a crash or exit), with
 * the clock pairs needed to put file times on the same clock. Nothing here
 * writes guest state; with BW_LATLOG unset every hook is one test of a flag.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bw_latlog.h"

int g_bw_lat_on;
volatile long g_bw_lat_flips;

typedef struct {
    int64_t t, a, b, c, d, e, f;
    double x, y, z, w;
    int32_t type;
    uint32_t tid;
} LatEv;

static LatEv *s_ev;
static volatile LONG s_nev;
static LONG s_max;
static char s_path[MAX_PATH];
static LARGE_INTEGER s_freq, s_q0;
static FILETIME s_ft0;
static volatile LONG s_dumped;
static DWORD s_main_tid;

int64_t bw_lat_now(void)
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart;
}

void bw_lat_ev(int type, int64_t t, int64_t a, int64_t b, int64_t c, int64_t d,
               int64_t e, int64_t f, double x, double y)
{
    LONG i;
    LatEv *v;
    if (!s_ev)
        return;
    i = InterlockedIncrement(&s_nev) - 1;
    if (i >= s_max)
        return;
    v = &s_ev[i];
    v->t = t;
    v->a = a; v->b = b; v->c = c; v->d = d; v->e = e; v->f = f;
    v->x = x; v->y = y; v->z = 0.0; v->w = 0.0;
    v->type = type;
    v->tid = GetCurrentThreadId();
}

static void lat_atexit(void)
{
    bw_lat_dump("exit");
}

void bw_lat_init(void)
{
    const char *e = getenv("BW_LATLOG");
    if (!e || !*e)
        return;
    snprintf(s_path, sizeof s_path, "%s", e);
    s_max = (e = getenv("BW_LATLOG_MAX")) && *e ? atol(e) : 600000;
    if (s_max < 1000)
        s_max = 1000;
    s_ev = (LatEv *)calloc((size_t)s_max, sizeof(LatEv));
    if (!s_ev) {
        fprintf(stderr, "[BW] latency log: no memory for %ld events; off\n", (long)s_max);
        return;
    }
    QueryPerformanceFrequency(&s_freq);
    QueryPerformanceCounter(&s_q0);
    GetSystemTimePreciseAsFileTime(&s_ft0);
    s_main_tid = GetCurrentThreadId();
    g_bw_lat_on = 1;
    atexit(lat_atexit);
    fprintf(stderr, "[BW] latency log on -> %s (up to %ld events; BW_LATLOG)\n", s_path, (long)s_max);
}

void bw_lat_dump(const char *why)
{
    FILE *f;
    LONG n, k;
    LARGE_INTEGER q1;
    FILETIME ft1;
    if (!g_bw_lat_on || InterlockedExchange(&s_dumped, 1))
        return;
    QueryPerformanceCounter(&q1);
    GetSystemTimePreciseAsFileTime(&ft1);
    n = s_nev < s_max ? s_nev : s_max;
    f = fopen(s_path, "w");
    if (!f)
        return;
    setvbuf(f, NULL, _IOFBF, 1 << 20);
    fprintf(f, "# BW_LATLOG v1 (%s): %ld events (%ld dropped); qpc_freq %lld; pid %lu\n",
            why, (long)n, (long)(s_nev - n), (long long)s_freq.QuadPart,
            (unsigned long)GetCurrentProcessId());
    fprintf(f, "# clock %lld %llu %lld %llu (qpc, FILETIME at init; qpc, FILETIME at the dump)\n",
            (long long)s_q0.QuadPart,
            ((unsigned long long)s_ft0.dwHighDateTime << 32) | s_ft0.dwLowDateTime,
            (long long)q1.QuadPart, ((unsigned long long)ft1.dwHighDateTime << 32) | ft1.dwLowDateTime);
    fprintf(f, "# line: type t a b c d e f x y z w thread (t and *_qpc in counter ticks)\n");
    fprintf(f, "# P/R first report with a script step / after its hold: a step, b due_qpc, c due_ms,"
               " d bits (digital | analog<<8 | stick<<16 | pad<<24), e hold_ms\n");
    fprintf(f, "# I pad block changed (at a frame's start): a port, b packet, c buttons, d analog bytes,"
               " e sticks (LX LY RX RY, int16 each, LX lowest)\n");
    fprintf(f, "# S step end: a wclk, b camera mode (view 0), c game state, d views, e boat;"
               " x throttle, y rudder, z/w camera eye x/z\n");
    fprintf(f, "# F frame begin: a frame, b newest step's wclk, c flips done, d Game_Update end qpc,"
               " e Game_Update begin qpc, f steps since the last frame | interp<<16 | state<<20 | on<<24;"
               " x node weight, y camera weight (view 0) of the newest step\n");
    fprintf(f, "# f frame end: a frame, c flips done\n");
    fprintf(f, "# X flip: a flip, b present result (1 presented, 0 dropped, 2 off screen, -1 nothing new,"
               " -2 no device), c present call qpc, d present return qpc, e readback end qpc, f end qpc\n");
    fprintf(f, "# G GPU finished flip a; D flip a has present count b; T DXGI stats: a PresentCount,"
               " b PresentRefreshCount, c SyncRefreshCount, d SyncQPCTime; M flip a dumped (b = nth dump)\n");
    fprintf(f, "# U Game_Update that ran steps (t = its start): a guest now, b guest frequency, c step clock"
               " before, d after, e steps, f its end qpc (guest units; host = t + (C - a) * qpc_freq / b)\n");
    fprintf(f, "# O a USB report for port 0 (the emulated pad's report interval); Q the title's USB ISR runs"
               " (a controller, b how long it took, qpc ticks)\n");
    fprintf(f, "# C the USB controller thread, about 1 s apart: a its CPU, b the process's (100 ns, kernel + user),"
               " c passes, d ISR runs so far, e their time so far (qpc ticks), f the pass in ms;"
               " x its kernel part, y the process's\n");
    fprintf(f, "# A an audio output period handed over: a its first sample's index in the WAV (RECOMP_AUDIO_WAV),"
               " b device queue in samples then (-1 no XAudio2), c samples played, d in the WAV, e samples,"
               " f the APU frame counter (ep_frame_div) at its last frame\n");
    fprintf(f, "# N the menus' navigation moved this pass (FrontEnd_UpdateMenuNav's 0.2 s gate): a 1 up, 2 down\n");
    fprintf(f, "# V VOICE_ON: a voice, b buffer address, c end offset, d ep_frame_div then, e list, f multipass;"
               " W the VP's first frame of it: a voice, b ep_frame_div, c first sample over -80 dBFS (-1 none),"
               " d buffer address, x peak, y gain; Y its first frame with sound: a voice, b ep_frame_div,"
               " c first sample over the mark (-1: none in 250 ms), d frames after W, x peak, y gain\n");
    fprintf(f, "# B a device buffer: a its first sample's index (= the WAV's while XAudio2 is active), b samples,"
               " c buffers queued before, d samples played then, e 1 submitted 0 refused -1 failed,"
               " f samples submitted in all; K XAudio2: a CurrentLatencyInSamples (-1: the device line),"
               " b GlitchesSinceEngineStarted, c active source voices, x engine load;"
               " K -1: b device period default, c minimum (100 ns), d engine period default,"
               " e fundamental, f minimum (frames), x mix rate\n");
    fprintf(f, "# J an output pass: a bytes it still needed (XAudio2's callback; 0 on WASAPI), b samples played"
               " at its start, c buffers queued (-1 on WASAPI), d samples submitted, e draining, f 1 = seen from"
               " the played count between two polls (WASAPI), time the polls' midpoint; B on WASAPI: c frames"
               " queued before; H the WASAPI stream's clock (t its counter time): a the frame playing then,"
               " b samples played (read by the engine)\n");
    for (k = 0; k < n; k++) {
        const LatEv *v = &s_ev[k];
        fprintf(f, "%c %lld %lld %lld %lld %lld %lld %lld %.6g %.6g %.6g %.6g %c\n", (char)v->type,
                (long long)v->t, (long long)v->a, (long long)v->b, (long long)v->c, (long long)v->d,
                (long long)v->e, (long long)v->f, v->x, v->y, v->z, v->w,
                v->tid == s_main_tid ? 'm' : 'o');
    }
    fclose(f);
    fprintf(stderr, "[BW] latency log written: %s (%ld events)\n", s_path, (long)n);
}

/* The step event carries four floats; this one takes them all. */
void bw_lat_ev4(int type, int64_t t, int64_t a, int64_t b, int64_t c, int64_t d,
                int64_t e, double x, double y, double z, double w)
{
    LONG i;
    LatEv *v;
    if (!s_ev)
        return;
    i = InterlockedIncrement(&s_nev) - 1;
    if (i >= s_max)
        return;
    v = &s_ev[i];
    v->t = t;
    v->a = a; v->b = b; v->c = c; v->d = d; v->e = e; v->f = 0;
    v->x = x; v->y = y; v->z = z; v->w = w;
    v->type = type;
    v->tid = GetCurrentThreadId();
}
