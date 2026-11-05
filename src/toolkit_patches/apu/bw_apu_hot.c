/* bw_apu_hot.c - lane au-hot's test aid (docs/audio/hot.md): BW_APU_HOTSTAT.
 * The APU frame thread's work per WASAPI pass, split by part; see
 * bw_apu_hot.h. Blood Wake file (part of xbox_apu).
 *
 *   BW_APU_HOTSTAT=1        on (any edition; off and inert when unset)
 *   BW_APU_HOTSTAT_MS=<ms>  a pass costing at least this from its event gets a
 *                           line of its own (default 5)
 *   BW_APU_HOTSTAT_DIR=<d>  every pass's record into <d>/hot_<pid>.bin too
 *                           (tools/bw_hotstat.py reads them)
 *
 * The lines ([APU-HOT]) come from the aid's own writer thread, every 250 ms,
 * never from the frame thread. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <psapi.h>
#include <intrin.h>
#include <winternl.h>
#include "bw_apu_hot.h"

int g_bw_hot;

/* au-vp: the watch (below) */
static int s_watch;
static DWORD s_dsp_tid;
static void hot_watch_print(void);
static void hot_watch_start(void);

extern unsigned xa2_underruns(void);
extern int xa2_queued_samples(void);
extern int bw_wa_period(void);

#define HOT_RING 16384
#define HOT_VERSION 2

/* One pass. Times in microseconds unless named otherwise. */
typedef struct {
    double   t_s;               /* the pass's event, s since the aid started */
    float    late;              /* the wake after the event */
    float    waited;            /* the wait that ended in the wake */
    float    relock;            /* the wait's return -> the lock taken */
    float    cost;              /* the event -> the critical write */
    float    slack;             /* xa2's own estimate of the critical write's slack */
    float    seg[BW_HOT_NSEG];  /* the parts, summed over the pass */
    float    frame_max;         /* the longest frame iteration */
    float    fm_seg[BW_HOT_NSEG];
    float    ran;               /* the frame thread's cycles over the pass, in us */
    float    dsp_wall, dsp_ran, dsp_max;   /* the DSP thread's frames finished in the pass */
    float    v_all, v_res;      /* the voices' times, and the resampler's part */
    int32_t  dsp_frames, dsp_lag_max, dsp_ecore;
    int32_t  frames, periods, voices, v3d, vhrtf, vsinc, vadpcm, vstream;
    int32_t  taps;
    int32_t  pf;                /* the process's page faults over the pass */
    int32_t  cpu_wake, cpu_cw, ecore_frames;
    int32_t  q_wake;            /* the queue (samples) at the wake, -1 unknown */
    int32_t  pad;               /* the queue just before the critical write */
    uint32_t underruns;         /* xa2's count so far (short passes) */
    int32_t  got;               /* the pass started with: 1 a wake on the event, 0 the timer, -1 no wait */
    int32_t  missed;            /* events between the last pass and this one, past the first */
    int64_t  cyc_raw;           /* the cycles (for the writer's calibration) */
    int64_t  part_cyc[BW_HOT_NPART];   /* the voices' parts past the resampler, TSC cycles */
    int32_t  den[BW_HOT_NDEN];  /* the denormal census, summed over the pass's voice frames */
    int32_t  ftz;               /* BW_APU_VP_FTZ was on */
} BwHotRec;

static BwHotRec s_ring[HOT_RING];
static volatile LONG64 s_w, s_r;

/* the frame thread's state */
static LARGE_INTEGER s_qpf;
static int64_t s_t0, s_P;                       /* start; one engine period, ticks */
static int64_t s_grid;                          /* an event's time, ticks (0: none yet) */
static int     s_open;                          /* a pass is being counted */
static int64_t s_pass_t, s_wake_t, s_first_t;   /* its event; the wake; the first frame */
static int64_t s_cyc0, s_seg[BW_HOT_NSEG], s_fseg[BW_HOT_NSEG], s_fmax, s_fmax_seg[BW_HOT_NSEG];
static int     s_frames, s_periods, s_cpu_wake, s_ecf, s_q_wake, s_got, s_missed;
static int64_t s_last_pass_t;
static float   s_late, s_waited, s_relock;
static DWORD   s_pf0;
static int     s_voices, s_v3d, s_vhrtf, s_vsinc, s_vadpcm, s_vstream, s_taps;
static int64_t s_v_all, s_v_res, s_part[BW_HOT_NPART];
static int     s_den[BW_HOT_NDEN];
int g_bw_vp_ftz;
static int64_t s_dsp_frames0, s_dsp_wall0, s_dsp_ran0, s_dsp_ecore0;
static int     s_lag_max;

/* the DSP thread's (one writer) */
static volatile int64_t s_dsp_frames, s_dsp_wall, s_dsp_ran, s_dsp_ecore;
static volatile int64_t s_dsp_max, s_dsp_max_epoch, s_dsp_epoch;

/* processors: efficiency class per logical processor (hybrid CPUs) */
static unsigned char s_eff[256];
static unsigned char s_eff_max;
static int s_n_ecore, s_n_cpu;

static BOOL (WINAPI *p_pmi)(HANDLE, PPROCESS_MEMORY_COUNTERS, DWORD);

int64_t bw_hot_t(void)
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart;
}

int64_t bw_hot_cycles(void)
{
    ULONG64 c = 0;
    QueryThreadCycleTime(GetCurrentThread(), &c);
    return (int64_t)c;
}

static DWORD page_faults(void)
{
    PROCESS_MEMORY_COUNTERS m;
    if (!p_pmi)
        return 0;
    m.cb = sizeof m;
    return p_pmi(GetCurrentProcess(), &m, sizeof m) ? m.PageFaultCount : 0;
}

static int is_ecore(int cpu)
{
    return cpu >= 0 && cpu < 256 && s_eff_max && s_eff[cpu] < s_eff_max;
}

static void cpu_classes(void)
{
    ULONG len = 0;
    BYTE *buf;
    GetSystemCpuSetInformation(NULL, 0, &len, GetCurrentProcess(), 0);
    if (!len || !(buf = (BYTE *)malloc(len)))
        return;
    if (GetSystemCpuSetInformation((PSYSTEM_CPU_SET_INFORMATION)buf, len, &len, GetCurrentProcess(), 0)) {
        ULONG off = 0;
        while (off < len) {
            PSYSTEM_CPU_SET_INFORMATION s = (PSYSTEM_CPU_SET_INFORMATION)(buf + off);
            if (s->Type == CpuSetInformation && s->CpuSet.Group == 0 && s->CpuSet.LogicalProcessorIndex < 256) {
                s_eff[s->CpuSet.LogicalProcessorIndex] = s->CpuSet.EfficiencyClass;
                if (s->CpuSet.EfficiencyClass > s_eff_max)
                    s_eff_max = s->CpuSet.EfficiencyClass;
            }
            if (!s->Size)
                break;
            off += s->Size;
        }
        s_n_cpu = (int)GetActiveProcessorCount(0);
        for (off = 0; off < 256 && (int)off < s_n_cpu; off++)
            if (s_eff_max && s_eff[off] < s_eff_max)
                s_n_ecore++;
    }
    free(buf);
}

/* ---- the writer thread ---------------------------------------------------- */

static double s_tsc_per_us;          /* thread cycles per microsecond (calibrated) */
static int64_t s_cal_qpc0, s_cal_tsc0;

static float pct(float *v, int n, double q)
{
    int i, j;
    float t;
    if (n <= 0)
        return 0.0f;
    for (i = 1; i < n; i++)          /* insertion sort: a 5 s window is about 500 */
        for (j = i; j > 0 && v[j - 1] > v[j]; j--)
            t = v[j], v[j] = v[j - 1], v[j - 1] = t;
    i = (int)(q * (n - 1) + 0.5);
    return v[i < 0 ? 0 : i >= n ? n - 1 : i];
}

static const char *s_seg_name[BW_HOT_NSEG] = {
    "ack", "vp", "dsp", "dsp_wait", "monitor", "write", "irq", "isr", "yield", "pace" };

static void line_parts(char *o, size_t cap, const float *seg)
{
    /* the nested parts printed on their own: dsp (less the wait), monitor
     * (less the write), irq (less the routine) */
    _snprintf(o, cap, "vp %.2f, dsp wait %.2f, dsp %.2f, write %.2f, monitor %.2f, isr %.2f, irq %.2f,"
                      " yield %.2f, ack %.2f, pace %.2f",
              seg[BW_HOT_VP] / 1000.0, seg[BW_HOT_DSPW] / 1000.0,
              (seg[BW_HOT_DSPF] - seg[BW_HOT_DSPW]) / 1000.0, seg[BW_HOT_SUB] / 1000.0,
              (seg[BW_HOT_MON] - seg[BW_HOT_SUB]) / 1000.0, seg[BW_HOT_ISR] / 1000.0,
              (seg[BW_HOT_IRQ] - seg[BW_HOT_ISR]) / 1000.0, seg[BW_HOT_YLD] / 1000.0,
              seg[BW_HOT_ACK] / 1000.0, seg[BW_HOT_PACE] / 1000.0);
    o[cap - 1] = 0;
}

static double cyc_ms(int64_t c)
{
    return s_tsc_per_us > 0 ? c / s_tsc_per_us / 1000.0 : -1.0;
}

static void print_pass(const char *what, const BwHotRec *r)
{
    char parts[400], fparts[400];
    float sum = 0.0f;
    int i;
    for (i = 0; i < BW_HOT_NSEG; i++)
        if (i != BW_HOT_DSPW && i != BW_HOT_SUB && i != BW_HOT_ISR)
            sum += r->seg[i];
    line_parts(parts, sizeof parts, r->seg);
    line_parts(fparts, sizeof fparts, r->fm_seg);
    fprintf(stderr, "[APU-HOT] %s at %.2f s: cost %.2f ms from the event (the wake %.2f late after a %.2f ms"
                    " wait, %s; relock %.2f); slack %.2f (xa2's %.2f); frames %d, periods %d, queue at the wake"
                    " %d, before the write %d; parts (ms) %s; the rest %.2f; ran %.2f of it (cpu %d -> %d,"
                    " %d frames on an E-core); page faults +%d; voices %d (3D %d, HRTF %d, sinc %d, ADPCM %d,"
                    " stream %d), their time %.2f ms (the resampler %.2f), sinc taps %d; the DSP thread: %d frames"
                    " done, %.2f ms (ran %.2f, longest %.3f; %d on an E-core), %d frames behind at most;"
                    " the longest frame %.2f ms: %s; the voices past the resampler (ms): lpf %.2f, i3dl2 %.2f,"
                    " hrtf %.2f, mix %.2f; denormals: hrtf coefficients %d, itd %d, history %d, room %d,"
                    " voice output %d, bins %d, sinc history %d%s; short passes so far %u\n",
            what, r->t_s, r->cost / 1000.0, r->late / 1000.0, r->waited / 1000.0,
            r->got > 0 ? "the event" : r->got == 0 ? "the timer" : "no wait", r->relock / 1000.0,
            (s_P * 1e6 / s_qpf.QuadPart - r->cost) / 1000.0, r->slack / 1000.0, r->frames, r->periods,
            r->q_wake, r->pad, parts, (r->cost - r->late - r->relock - sum) / 1000.0,
            s_tsc_per_us > 0 ? r->cyc_raw / s_tsc_per_us / 1000.0 : -1.0, r->cpu_wake, r->cpu_cw,
            r->ecore_frames, r->pf, r->voices, r->v3d, r->vhrtf, r->vsinc, r->vadpcm, r->vstream,
            r->v_all / 1000.0, r->v_res / 1000.0, r->taps, r->dsp_frames, r->dsp_wall / 1000.0,
            r->dsp_ran / 1000.0, r->dsp_max / 1000.0, r->dsp_ecore, r->dsp_lag_max, r->frame_max / 1000.0,
            fparts, cyc_ms(r->part_cyc[BW_HOT_P_LPF]), cyc_ms(r->part_cyc[BW_HOT_P_I3DL2]),
            cyc_ms(r->part_cyc[BW_HOT_P_HRTF]), cyc_ms(r->part_cyc[BW_HOT_P_MIX]),
            r->den[BW_HOT_D_COEFF], r->den[BW_HOT_D_ITD], r->den[BW_HOT_D_HIST], r->den[BW_HOT_D_ROOM],
            r->den[BW_HOT_D_OUT], r->den[BW_HOT_D_BINS], r->den[BW_HOT_D_SINC],
            r->ftz ? " (BW_APU_VP_FTZ on)" : "", r->underruns);
}

static DWORD WINAPI hot_writer(LPVOID arg)
{
    static float cost[4096], late[4096], vp[4096], dspw[4096];
    const char *dir = getenv("BW_APU_HOTSTAT_DIR");
    const char *ms = getenv("BW_APU_HOTSTAT_MS");
    double thr = ms && atof(ms) > 0 ? atof(ms) * 1000.0 : 5000.0;
    FILE *f = NULL;
    int n = 0, shown = 0, missed_tot = 0, nocw = 0;
    int64_t next = 0;
    unsigned u_last = 0;
    BwHotRec worst;
    float segmax[BW_HOT_NSEG];
    double segsum[BW_HOT_NSEG];
    int vmax = 0, tapsmax = 0, ecf = 0, pf = 0;
    int64_t partsum[BW_HOT_NPART];
    long long densum[BW_HOT_NDEN];
    double vsum = 0, tapsum = 0;
    (void)arg;
    memset(&worst, 0, sizeof worst);
    memset(segmax, 0, sizeof segmax);
    memset(segsum, 0, sizeof segsum);
    memset(partsum, 0, sizeof partsum);
    memset(densum, 0, sizeof densum);
    if (dir && *dir) {
        char path[MAX_PATH];
        _snprintf(path, sizeof path, "%s/hot_%lu.bin", dir, GetCurrentProcessId());
        path[sizeof path - 1] = 0;
        CreateDirectoryA(dir, NULL);
        f = fopen(path, "wb");
        if (f) {
            int32_t hdr[4] = { 0x544F4842 /* 'BHOT' */, HOT_VERSION, (int32_t)sizeof(BwHotRec), BW_HOT_NSEG };
            fwrite(hdr, sizeof hdr, 1, f);
        }
        fprintf(stderr, "[APU-HOT] records to %s%s\n", path, f ? "" : " (could not open it)");
    }
    for (;;) {
        int64_t now;
        Sleep(250);
        now = bw_hot_t();
        if (!s_tsc_per_us && now - s_cal_qpc0 > s_qpf.QuadPart) {
            int64_t tsc = (int64_t)__rdtsc();
            s_tsc_per_us = (double)(tsc - s_cal_tsc0) / ((double)(now - s_cal_qpc0) * 1e6 / s_qpf.QuadPart);
            fprintf(stderr, "[APU-HOT] calibrated: %.1f cycles per us; %d of the processors are efficiency"
                            " cores (class below %d) of %d\n", s_tsc_per_us, s_n_ecore, s_eff_max, s_n_cpu);
        }
        while (s_r < s_w) {
            BwHotRec r = s_ring[s_r % HOT_RING];
            int i;
            s_r++;
            if (f)
                fwrite(&r, sizeof r, 1, f);
            if (r.got == -2) {          /* a wake with no critical write after it */
                nocw++;
                continue;
            }
            if (n < 4096) {
                cost[n] = r.cost;
                late[n] = r.late;
                vp[n] = r.seg[BW_HOT_VP];
                dspw[n] = r.seg[BW_HOT_DSPW];
                n++;
            }
            for (i = 0; i < BW_HOT_NSEG; i++) {
                if (r.seg[i] > segmax[i])
                    segmax[i] = r.seg[i];
                segsum[i] += r.seg[i];
            }
            if (r.voices > vmax)
                vmax = r.voices;
            if (r.taps > tapsmax)
                tapsmax = r.taps;
            vsum += r.voices;
            tapsum += r.taps;
            ecf += r.ecore_frames;
            pf += r.pf;
            missed_tot += r.missed;
            for (i = 0; i < BW_HOT_NPART; i++)
                partsum[i] += r.part_cyc[i];
            for (i = 0; i < BW_HOT_NDEN; i++)
                densum[i] += r.den[i];
            if (r.cost > worst.cost)
                worst = r;
            if ((r.cost >= thr || r.underruns != u_last) && shown < 600) {
                shown++;
                print_pass(r.underruns != u_last ? "a short pass" : "slow pass", &r);
            }
            u_last = r.underruns;
        }
        hot_watch_print();               /* au-vp: the watch's stalls */
        if (f)
            fflush(f);
        if (!next)
            next = now + 5 * s_qpf.QuadPart;
        if (now >= next && n) {
            float c50 = pct(cost, n, 0.5), c99 = pct(cost, n, 0.99), cmax = pct(cost, n, 1.0);
            float l50 = pct(late, n, 0.5), lmax = pct(late, n, 1.0);
            float v50 = pct(vp, n, 0.5), v99 = pct(vp, n, 0.99);
            float w50 = pct(dspw, n, 0.5), w99 = pct(dspw, n, 0.99);
            fprintf(stderr, "[APU-HOT] 5 s: passes %d (no write after the wake %d, events missed %d); cost from"
                            " the event median %.2f p99 %.2f max %.2f ms; the wake late median %.2f max %.2f;"
                            " vp per pass median %.2f p99 %.2f max %.2f; dsp wait median %.2f p99 %.2f max %.2f;"
                            " isr max %.2f; write max %.2f; yield max %.2f; voices per pass avg %.0f max %d;"
                            " sinc taps per pass avg %.0f max %d; frames on an E-core %d; page faults %d;"
                            " the voices past the resampler in all (ms): lpf %.1f, i3dl2 %.1f, hrtf %.1f, mix %.1f;"
                            " denormals: hrtf coefficients %lld, itd %lld, history %lld, room %lld, voice output %lld,"
                            " bins %lld, sinc history %lld\n",
                    n, nocw, missed_tot, c50 / 1000.0, c99 / 1000.0, cmax / 1000.0, l50 / 1000.0, lmax / 1000.0,
                    v50 / 1000.0, v99 / 1000.0, segmax[BW_HOT_VP] / 1000.0, w50 / 1000.0, w99 / 1000.0,
                    segmax[BW_HOT_DSPW] / 1000.0, segmax[BW_HOT_ISR] / 1000.0, segmax[BW_HOT_SUB] / 1000.0,
                    segmax[BW_HOT_YLD] / 1000.0, vsum / n, vmax, tapsum / n, tapsmax, ecf, pf,
                    cyc_ms(partsum[0]), cyc_ms(partsum[1]), cyc_ms(partsum[2]), cyc_ms(partsum[3]),
                    densum[0], densum[1], densum[2], densum[3], densum[4], densum[5], densum[6]);
            if (worst.cost > 0)
                print_pass("the window's worst pass", &worst);
            n = nocw = missed_tot = 0;
            vmax = tapsmax = ecf = pf = 0;
            vsum = tapsum = 0;
            memset(&worst, 0, sizeof worst);
            memset(segmax, 0, sizeof segmax);
            memset(segsum, 0, sizeof segsum);
            memset(partsum, 0, sizeof partsum);
            memset(densum, 0, sizeof densum);
            next += 5 * s_qpf.QuadPart;
            if (next < now)
                next = now + 5 * s_qpf.QuadPart;
        }
    }
    return 0;
}

void bw_hot_init(void)
{
    const char *e = getenv("BW_APU_HOTSTAT");
    HANDLE h;
    if (!(e && *e && *e != '0'))
        return;
    QueryPerformanceFrequency(&s_qpf);
    s_t0 = s_cal_qpc0 = bw_hot_t();
    s_cal_tsc0 = (int64_t)__rdtsc();
    {
        HMODULE k = GetModuleHandleA("kernel32.dll");
        p_pmi = k ? (BOOL (WINAPI *)(HANDLE, PPROCESS_MEMORY_COUNTERS, DWORD))
                        GetProcAddress(k, "K32GetProcessMemoryInfo") : NULL;
    }
    cpu_classes();
    {
        const char *z = getenv("BW_APU_VP_FTZ");
        g_bw_vp_ftz = z && z[0] == '1';
    }
    h = CreateThread(NULL, 0, hot_writer, NULL, 0, NULL);
    if (!h) {
        fprintf(stderr, "[APU-HOT] no writer thread: off\n");
        return;
    }
    CloseHandle(h);
    hot_watch_start();                  /* au-vp: the watch */
    fprintf(stderr, "[APU-HOT] the frame thread's work per pass (BW_APU_HOTSTAT, a test aid); efficiency classes"
                    " up to %d%s\n", s_eff_max,
            g_bw_vp_ftz ? "; BW_APU_VP_FTZ: flush-to-zero around the VP (an experiment)" : "");
    g_bw_hot = 1;
}

/* ---- the frame thread's hooks ---------------------------------------------- */

static int64_t period_ticks(void)
{
    int p = bw_wa_period();
    return (int64_t)(p > 0 ? p : 480) * s_qpf.QuadPart / 48000;
}

/* the last event at or before t, by the grid */
static int64_t grid_at(int64_t t)
{
    int64_t k;
    if (!s_grid)
        return t;
    k = (t - s_grid) / s_P;
    if (t < s_grid)
        k = -((s_grid - t + s_P - 1) / s_P);
    return s_grid + k * s_P;
}

static void pass_open(int64_t t_start)
{
    int64_t ev = grid_at(t_start);
    memset(s_seg, 0, sizeof s_seg);
    memset(s_fmax_seg, 0, sizeof s_fmax_seg);
    s_fmax = 0;
    s_frames = s_periods = s_ecf = 0;
    s_voices = s_v3d = s_vhrtf = s_vsinc = s_vadpcm = s_vstream = s_taps = 0;
    s_v_all = s_v_res = 0;
    memset(s_part, 0, sizeof s_part);
    memset(s_den, 0, sizeof s_den);
    s_lag_max = 0;
    s_pass_t = ev;
    s_missed = s_last_pass_t ? (int)((ev - s_last_pass_t + s_P / 2) / s_P) - 1 : 0;
    if (s_missed < 0)
        s_missed = 0;
    s_cyc0 = bw_hot_cycles();
    s_pf0 = page_faults();
    s_cpu_wake = (int)GetCurrentProcessorNumber();
    s_dsp_frames0 = s_dsp_frames;
    s_dsp_wall0 = s_dsp_wall;
    s_dsp_ran0 = s_dsp_ran;
    s_dsp_ecore0 = s_dsp_ecore;
    s_dsp_epoch++;
    s_open = 1;
}

void bw_hot_wake(int64_t t_ret, int64_t t_lock, int64_t waited, int got)
{
    double us = 1e6 / (double)s_qpf.QuadPart;
    if (!s_P)
        s_P = period_ticks();
    if (got && waited * us >= 300.0) {
        /* a wait that ended on a fresh event: the grid follows the earliest
         * wakes (a wake can only be late), drifting with the engine's clock */
        int64_t pred, e;
        if (!s_grid)
            s_grid = t_ret;
        pred = grid_at(t_ret);
        if (t_ret - pred > s_P / 2)
            pred += s_P;                 /* nearer the next one: early against it */
        e = t_ret - pred;
        if (e < 0)
            s_grid = t_ret;
        else
            s_grid = pred + e / 32;
    }
    if (!got)
        return;                          /* the timer: not a pass of its own */
    if (s_open && s_wake_t) {
        /* the last wake never got its critical write: note it, start again */
        BwHotRec *r = &s_ring[s_w % HOT_RING];
        memset(r, 0, sizeof *r);
        r->t_s = (double)(s_pass_t - s_t0) * us / 1e6;
        r->got = -2;
        s_w++;
    }
    pass_open(t_ret + s_qpf.QuadPart / 1000);   /* a wake up to 1 ms early on the grid is its event's */
    s_wake_t = t_ret;
    s_late = (float)((t_ret - s_pass_t) * us);
    if (s_late < 0.0f)
        s_late = 0.0f;
    s_waited = (float)(waited * us);
    s_relock = (float)((t_lock - t_ret) * us);
    s_got = 1;
    s_q_wake = xa2_queued_samples();
}

void bw_hot_seg(int seg, int64_t ticks)
{
    s_seg[seg] += ticks;
    s_fseg[seg] += ticks;
}

static int64_t s_mark[BW_HOT_NSEG], s_mark_cw[BW_HOT_NSEG];
static int64_t s_cur_f0;                /* the frame in progress began */

void bw_hot_begin(int seg, int64_t t)
{
    s_mark[seg] = t;
}

void bw_hot_end(int seg, int64_t t)
{
    if (s_mark[seg]) {
        int64_t from = s_mark[seg];
        if (s_mark_cw[seg] > from)
            from = s_mark_cw[seg];       /* the part before the write went with its pass */
        bw_hot_seg(seg, t - from);
        s_fseg[seg] += from - s_mark[seg];
    }
    s_mark[seg] = 0;
    s_mark_cw[seg] = 0;
}

void bw_hot_frame_begin(int64_t t0)
{
    s_cur_f0 = t0;
    if (!s_open) {
        /* the thread didn't wait for the event since the last critical write
         * (a timer wake that found the queue already drawn down): the event
         * came before this frame, but the grid follows the earliest wakes and
         * so sits a little after the true events; 0.3 ms of grace keeps the
         * pass on its own event, not the one before */
        if (!s_P)
            s_P = period_ticks();
        pass_open(t0 + s_qpf.QuadPart * 3 / 10000);
        s_wake_t = 0;
        s_late = s_waited = s_relock = 0.0f;
        s_got = -1;
        s_q_wake = -1;
        s_first_t = t0;
    }
}

void bw_hot_frame(int64_t t0, int64_t t1)
{
    int cpu = (int)GetCurrentProcessorNumber();
    s_frames++;
    if (is_ecore(cpu))
        s_ecf++;
    if (t1 - t0 > s_fmax) {
        s_fmax = t1 - t0;
        memcpy(s_fmax_seg, s_fseg, sizeof s_fseg);
    }
    memset(s_fseg, 0, sizeof s_fseg);
}

void bw_hot_submit(void)
{
    s_periods++;
}

void bw_hot_cw(int pad, int n, int64_t slack_us)
{
    double us = 1e6 / (double)s_qpf.QuadPart;
    int64_t t = bw_hot_t();
    BwHotRec *r;
    int i;
    (void)n;
    if (!s_open) {
        if (!s_P)
            s_P = period_ticks();
        pass_open(t);
        s_got = -1;
    }
    r = &s_ring[s_w % HOT_RING];
    memset(r, 0, sizeof *r);
    r->t_s = (double)(s_pass_t - s_t0) * us / 1e6;
    r->late = s_got > 0 ? s_late : 0.0f;
    r->waited = s_waited;
    r->relock = s_relock;
    r->cost = (float)((t - s_pass_t) * us);
    r->slack = (float)slack_us;
    /* the frame in progress (the write's own) counts up to the write: its
     * open parts (the monitor, the write) too */
    {
        int64_t part[BW_HOT_NSEG];
        for (i = 0; i < BW_HOT_NSEG; i++) {
            part[i] = 0;
            if (s_mark[i]) {
                int64_t from = s_mark_cw[i] > s_mark[i] ? s_mark_cw[i] : s_mark[i];
                part[i] = t - from;
                s_mark_cw[i] = t;
            }
        }
        if (s_cur_f0 && t - s_cur_f0 > s_fmax) {
            s_fmax = t - s_cur_f0;
            for (i = 0; i < BW_HOT_NSEG; i++)
                s_fmax_seg[i] = s_fseg[i] + part[i];
        }
        for (i = 0; i < BW_HOT_NSEG; i++) {
            r->seg[i] = (float)((s_seg[i] + part[i]) * us);
            r->fm_seg[i] = (float)(s_fmax_seg[i] * us);
        }
    }
    r->frame_max = (float)(s_fmax * us);
    r->cyc_raw = bw_hot_cycles() - s_cyc0;
    r->ran = 0.0f;
    r->dsp_frames = (int32_t)(s_dsp_frames - s_dsp_frames0);
    r->dsp_wall = (float)((s_dsp_wall - s_dsp_wall0) * us);
    r->dsp_ran = s_tsc_per_us > 0 ? (float)((s_dsp_ran - s_dsp_ran0) / s_tsc_per_us) : -1.0f;
    r->dsp_max = s_dsp_max_epoch == s_dsp_epoch ? (float)(s_dsp_max * us) : 0.0f;
    r->dsp_ecore = (int32_t)(s_dsp_ecore - s_dsp_ecore0);
    r->dsp_lag_max = s_lag_max;
    r->frames = s_frames + 1;           /* those ended, and the write's own */
    r->periods = s_periods + 1;         /* those written, and this one */
    r->voices = s_voices;
    r->v3d = s_v3d;
    r->vhrtf = s_vhrtf;
    r->vsinc = s_vsinc;
    r->vadpcm = s_vadpcm;
    r->vstream = s_vstream;
    r->v_all = (float)(s_v_all * us);
    r->v_res = (float)(s_v_res * us);
    r->taps = s_taps;
    memcpy(r->part_cyc, s_part, sizeof s_part);
    memcpy(r->den, s_den, sizeof s_den);
    r->ftz = g_bw_vp_ftz;
    r->pf = (int32_t)(page_faults() - s_pf0);
    r->cpu_wake = s_cpu_wake;
    r->cpu_cw = (int)GetCurrentProcessorNumber();
    r->ecore_frames = s_ecf;
    r->q_wake = s_q_wake;
    r->pad = pad;
    r->underruns = xa2_underruns();
    r->got = s_got;
    r->missed = s_missed;
    s_w++;
    s_last_pass_t = s_pass_t;
    s_open = 0;
    s_wake_t = 0;
}

void bw_hot_voice(int flags, int64_t t_all, int64_t t_res, int taps)
{
    s_voices++;
    s_v3d += !!(flags & BW_HOT_V_3D);
    s_vhrtf += !!(flags & BW_HOT_V_HRTF);
    s_vsinc += !!(flags & BW_HOT_V_SINC);
    s_vadpcm += !!(flags & BW_HOT_V_ADPCM);
    s_vstream += !!(flags & BW_HOT_V_STREAM);
    s_v_all += t_all;
    s_v_res += t_res;
    s_taps += taps;
}

void bw_hot_voice_parts(const int64_t part[BW_HOT_NPART], const int den[BW_HOT_NDEN])
{
    int i;
    for (i = 0; i < BW_HOT_NPART; i++)
        s_part[i] += part[i];
    for (i = 0; i < BW_HOT_NDEN; i++)
        s_den[i] += den[i];
}

void bw_hot_bins_den(int n)
{
    s_den[BW_HOT_D_BINS] += n;
}

unsigned bw_hot_ftz_enter(void)
{
    unsigned m = _mm_getcsr();
    _mm_setcsr(m | 0x8040);             /* FTZ (bit 15) and DAZ (bit 6) */
    return m;
}

void bw_hot_ftz_leave(unsigned saved)
{
    _mm_setcsr(saved);
}

void bw_hot_dsp_lag(int lag)
{
    if (lag > s_lag_max)
        s_lag_max = lag;
}

/* ---- the DSP thread's ---------------------------------------------------- */

void bw_hot_dsp_frame(int64_t wall, int64_t ran_cyc)
{
    int64_t ep = s_dsp_epoch;
    s_dsp_frames++;
    s_dsp_wall += wall;
    s_dsp_ran += ran_cyc;
    if (is_ecore((int)GetCurrentProcessorNumber()))
        s_dsp_ecore++;
    if (!s_dsp_tid)
        s_dsp_tid = GetCurrentThreadId();   /* au-vp: the watch names it */
    if (s_dsp_max_epoch != ep) {
        s_dsp_max_epoch = ep;
        s_dsp_max = wall;
    } else if (wall > s_dsp_max) {
        s_dsp_max = wall;
    }
}

/* ==== au-vp (docs/audio/vp.md): the watch ==================================
 * What holds the frame thread when one of its stages overruns. The frame
 * thread marks its stage (bw_hot_stage, a seqlock); the watcher wakes every
 * millisecond on a high-resolution timer at time-critical priority, keeps a
 * short ring of every processor's times, the frame thread's cycles and the
 * process's page faults, and when a stage has run 3 ms past what it should
 * take, takes a deep sample: the thread states (NtQuerySystemInformation),
 * the APU lock's owner, and the stacks of the frame thread and the owner
 * (suspended for the few microseconds of GetThreadContext and an unwind, with
 * nothing allocated meanwhile). When the stage ends, the stall's record goes
 * to the writer thread, which prints it ([APU-HOT-STALL]). */

static volatile LONG s_st_seq;           /* odd while the frame thread writes */
static volatile int s_st, s_st_cpu;
static volatile int64_t s_st_t, s_st_exp;
static volatile int64_t s_long_dur;      /* the last overrun stage's exact length, ticks */
static volatile LONG s_long_seq;         /* and its sequence number */
static DWORD s_ft_tid, s_watch_tid;
static HANDLE s_ft_h;
static CRITICAL_SECTION *s_lock_cs;
extern int xbox_IrqlRaisedCount(void);
extern unsigned long xbox_DispatchGateOwner(void);   /* the toolkit's one-CPU gate (kernel_hal.c) */

/* The frame thread's production: the last voice-processor frame's start, and
 * a trapped episode (the front end still trapped after the interrupt's
 * delivery, the frame thread waiting a millisecond at a time and making no
 * frame) from its first wait. When one ends, its length and its waits. */
static volatile int64_t s_vp_t, s_trap_t0;
static int s_trap_n;                     /* the episode's waits so far (frame thread only) */
static int64_t s_trap_max;               /* and the longest */
static volatile int64_t s_ep_len, s_ep_max, s_gap_len;
static volatile int s_ep_n;
static volatile LONG s_ep_id, s_gap_id;
/* every trapped wait (asked for 1 ms), for the 5 s line: how long they really
 * took (the timer's resolution shows here) */
static volatile LONG s_tw_n, s_tw_h[5], s_tw_max_us, s_ep_cnt, s_ep_max_us;
static const char *s_tw_bin[5] = { "<1.5", "1.5-3", "3-8", "8-16", ">=16" };

static const char *s_st_name[BW_ST_N] = {
    "the loop's own work", "the wait for the engine's event", "taking the lock after that wait",
    "throttle's own work", "a timed wait on the condition", "the doorbell ack", "the voice processor",
    "the DSP stage", "the wait for the DSP thread", "the monitor", "the device write",
    "the interrupt's delivery", "the guest's interrupt routine", "the hand-off's yield",
    "taking the lock after the yield", "the wait while the front end stays trapped", "idle (paused)" };

void bw_hot_set_lock(void *cs)
{
    s_lock_cs = (CRITICAL_SECTION *)cs;
}

void bw_hot_stage(int st, int64_t expect_us)
{
    int64_t t;
    if (!s_watch || GetCurrentThreadId() != s_ft_tid)
        return;
    t = bw_hot_t();
    if (s_st_t && s_st != BW_ST_IDLE && t - s_st_t > (s_st_exp + 3000) * s_qpf.QuadPart / 1000000) {
        s_long_dur = t - s_st_t;
        s_long_seq = s_st_seq;
    }
    if (s_st == BW_ST_TRAPWAIT && s_st_t) {
        /* a trapped wait ended: how long the 1 ms really took */
        int64_t d = t - s_st_t;
        LONG us = (LONG)(d * 1000000 / s_qpf.QuadPart);
        s_trap_n++;
        if (d > s_trap_max)
            s_trap_max = d;
        InterlockedIncrement(&s_tw_n);
        InterlockedIncrement(&s_tw_h[us < 1500 ? 0 : us < 3000 ? 1 : us < 8000 ? 2 : us < 16000 ? 3 : 4]);
        if (us > s_tw_max_us)
            s_tw_max_us = us;
    }
    if (st == BW_ST_TRAPWAIT && !s_trap_t0) {
        s_trap_n = 0;
        s_trap_max = 0;
        s_trap_t0 = t;                   /* an episode begins */
    }
    if (st == BW_ST_VP) {
        if (s_trap_t0) {                 /* the episode ended: frames again */
            LONG us = (LONG)((t - s_trap_t0) * 1000000 / s_qpf.QuadPart);
            s_ep_len = t - s_trap_t0;
            s_ep_n = s_trap_n;
            s_ep_max = s_trap_max;
            InterlockedIncrement(&s_ep_id);
            InterlockedIncrement(&s_ep_cnt);
            if (us > s_ep_max_us)
                s_ep_max_us = us;
            s_trap_t0 = 0;
        }
        if (s_vp_t && t - s_vp_t > 15 * s_qpf.QuadPart / 1000) {
            s_gap_len = t - s_vp_t;
            InterlockedIncrement(&s_gap_id);
        }
        s_vp_t = t;
    }
    InterlockedIncrement(&s_st_seq);     /* odd: being written */
    s_st = st;
    s_st_exp = expect_us > 0 ? expect_us : 0;
    s_st_cpu = (int)GetCurrentProcessorNumber();
    s_st_t = t;
    InterlockedIncrement(&s_st_seq);     /* even: whole */
}

/* ---- the watcher's samples ---- */
typedef LONG (NTAPI *nqsi_fn)(ULONG, PVOID, ULONG, PULONG);
typedef LONG (NTAPI *nqit_fn)(HANDLE, ULONG, PVOID, ULONG, PULONG);
typedef HRESULT (WINAPI *gtd_fn)(HANDLE, PWSTR *);
static nqsi_fn p_nqsi;
static nqit_fn p_nqit;

#define W_CPUS 64
#define W_RING 64
typedef struct {
    int64_t t;                           /* QPC */
    int64_t ft_cyc;                      /* the frame thread's cycles */
    DWORD pf;                            /* the process's page faults */
    int64_t idle[W_CPUS], dpc[W_CPUS], isr[W_CPUS], busy[W_CPUS];   /* 100 ns units */
} WSample;
static WSample s_wr[W_RING];
static int s_wr_n, s_ncpu_w;

typedef struct {                         /* SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION, spelled out */
    LARGE_INTEGER IdleTime, KernelTime, UserTime, DpcTime, InterruptTime;
    ULONG InterruptCount;
} WProcPerf;

static void w_sample(WSample *s)
{
    static WProcPerf pp[W_CPUS];
    ULONG len = 0;
    int i, n = 0;
    ULONG64 c = 0;
    s->t = bw_hot_t();
    if (s_ft_h && QueryThreadCycleTime(s_ft_h, &c))
        s->ft_cyc = (int64_t)c;
    s->pf = page_faults();
    if (p_nqsi && p_nqsi(8 /* SystemProcessorPerformanceInformation */, pp, sizeof pp, &len) >= 0)
        n = (int)(len / sizeof pp[0]);
    if (n > W_CPUS)
        n = W_CPUS;
    s_ncpu_w = n;
    for (i = 0; i < n; i++) {
        s->idle[i] = pp[i].IdleTime.QuadPart;
        s->dpc[i] = pp[i].DpcTime.QuadPart;
        s->isr[i] = pp[i].InterruptTime.QuadPart;
        /* KernelTime includes the idle time */
        s->busy[i] = pp[i].KernelTime.QuadPart + pp[i].UserTime.QuadPart - pp[i].IdleTime.QuadPart;
    }
}

/* a thread's state from the system's process list */
typedef struct {
    DWORD tid;
    int found, state, reason, prio, base;
    unsigned cswitch;
    int64_t kern, user;                  /* 100 ns */
} WThr;

static BYTE *s_spi;                      /* the process list's buffer */
static ULONG s_spi_cap;
static ULONG s_hardfaults;               /* the process's hard faults, at the last deep sample */

static void w_threads(WThr *t, int n)
{
    ULONG len = 0, off = 0;
    DWORD pid = GetCurrentProcessId();
    int i;
    for (i = 0; i < n; i++)
        t[i].found = 0;
    if (!p_nqsi || !s_spi || p_nqsi(5 /* SystemProcessInformation */, s_spi, s_spi_cap, &len) < 0)
        return;
    for (;;) {
        SYSTEM_PROCESS_INFORMATION *p = (SYSTEM_PROCESS_INFORMATION *)(s_spi + off);
        if ((DWORD)(ULONG_PTR)p->UniqueProcessId == pid) {
            SYSTEM_THREAD_INFORMATION *th = (SYSTEM_THREAD_INFORMATION *)(p + 1);
            ULONG k;
            s_hardfaults = *(ULONG *)((BYTE *)p + 16);   /* HardFaultCount, in Reserved1 */
            for (k = 0; k < p->NumberOfThreads; k++)
                for (i = 0; i < n; i++)
                    if (t[i].tid && (DWORD)(ULONG_PTR)th[k].ClientId.UniqueThread == t[i].tid) {
                        t[i].found = 1;
                        t[i].state = (int)th[k].ThreadState;
                        t[i].reason = (int)th[k].WaitReason;
                        t[i].prio = (int)th[k].Priority;
                        t[i].base = (int)th[k].BasePriority;
                        t[i].cswitch = th[k].Reserved3;        /* ContextSwitches */
                        t[i].kern = th[k].Reserved1[0].QuadPart;
                        t[i].user = th[k].Reserved1[1].QuadPart;
                    }
            return;
        }
        if (!p->NextEntryOffset)
            return;
        off += p->NextEntryOffset;
    }
}

/* a thread's stack, suspended for the context and the unwind; nothing is
 * allocated and no lock is taken while it is suspended */
#define W_FRAMES 12
static int w_stack(HANDLE h, DWORD64 *out)
{
    static CONTEXT c;
    int n = 0;
    if (!h || SuspendThread(h) == (DWORD)-1)
        return 0;
    memset(&c, 0, sizeof c);
    c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (GetThreadContext(h, &c)) {
        __try {
            while (n < W_FRAMES && c.Rip) {
                DWORD64 base = 0;
                PRUNTIME_FUNCTION rf;
                out[n++] = c.Rip;
                rf = RtlLookupFunctionEntry(c.Rip, &base, NULL);
                if (rf) {
                    PVOID hd = NULL;
                    DWORD64 ef = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, rf, &c, &hd, &ef, NULL);
                } else {
                    c.Rip = *(DWORD64 *)c.Rsp;   /* a leaf: the return address on top */
                    c.Rsp += 8;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    ResumeThread(h);
    return n;
}

/* What a stall is: a stage overran; the front end stayed trapped (no frame
 * made, the frame thread waiting a millisecond at a time) for 3 ms; or no
 * voice-processor frame for 15 ms. And, not a stall of the frame thread but
 * what makes one when a trap meets it: the one-CPU dispatch gate held by the
 * same other thread for 3 ms (seen at every 1 ms sample; a posted interrupt
 * waits for its holder's next safe point), the lowest of the four. */
enum { W_K_STAGE, W_K_TRAP, W_K_GAP, W_K_GATE };
static const char *s_k_name[4] = { "a stage overran", "the front end stayed trapped",
                                   "no voice-processor frame", "the dispatch gate held by one thread" };

/* the other threads named: the APU lock's owner, the dispatch gate's holder */
enum { W_O_LOCK, W_O_GATE, W_O_N };

typedef struct {
    LONG seq;
    int kind, st, cpu, deep, n_ft, irql;
    double t_s;                          /* the stall's start, s since the aid started */
    float exp_ms, dur_ms, seen_ms;       /* what it should take, what it took, its age when seen */
    float ran_ms;                        /* the frame thread's own run time over the stall */
    float cpu_dpc, cpu_isr, cpu_idle, cpu_busy;   /* its processor's, over the stall (ms) */
    float max_di;                        /* the most DPC + interrupt time of any processor */
    int max_di_cpu, pf, hf;
    int trap_n;                          /* a trapped episode's waits, and the longest (ms) */
    float trap_max_ms;
    LONG lock_count;
    WThr ft[2], dsp[2];                  /* at the first and second deep sample */
    DWORD64 ft_stack[W_FRAMES];
    struct {
        DWORD tid;                       /* 0: free */
        int role;                        /* 0 free, 1 the frame thread, 2 the DSP thread, 3 another */
        int n, n2;
        WThr thr[2];
        DWORD64 stack[W_FRAMES];
        DWORD64 stack2[W_FRAMES];        /* 4 ms on, if it still holds it (au-vp: what it waits on) */
        DWORD64 start;
        WCHAR desc[48];
    } o[W_O_N];
} WStall;

#define W_STALLS 256
static WStall s_ws[W_STALLS];
static volatile LONG64 s_ws_w, s_ws_r;
static HANDLE s_oh[W_O_N];
static DWORD s_oh_tid[W_O_N];

static const WSample *w_before(int64_t t)
{
    /* the latest ring sample at or before t (else the oldest) */
    const WSample *best = NULL, *oldest = NULL;
    int i;
    for (i = 0; i < W_RING && i < s_wr_n; i++) {
        const WSample *s = &s_wr[i];
        if (!oldest || s->t < oldest->t)
            oldest = s;
        if (s->t <= t && (!best || s->t > best->t))
            best = s;
    }
    return best ? best : oldest;
}

static void w_deep(WStall *w, int k)
{
    WThr t[2 + W_O_N];
    int i;
    memset(t, 0, sizeof t);
    t[0].tid = s_ft_tid;
    t[1].tid = s_dsp_tid;
    for (i = 0; i < W_O_N; i++)
        t[2 + i].tid = w->o[i].role >= 2 ? w->o[i].tid : 0;
    w_threads(t, 2 + W_O_N);
    w->ft[k] = t[0];
    w->dsp[k] = t[1];
    for (i = 0; i < W_O_N; i++)
        w->o[i].thr[k] = t[2 + i];
    w->deep = k + 1;
}

static int w_role(DWORD tid)
{
    return tid == 0 ? 0 : tid == s_ft_tid ? 1 : tid == s_dsp_tid ? 2 : 3;
}

static void w_other(WStall *w, int i)
{
    /* another thread named: its stack, where it started, its name */
    DWORD tid = w->o[i].tid;
    if (w->o[i].role < 2 || tid == s_watch_tid)
        return;
    if (s_oh_tid[i] != tid) {
        if (s_oh[i])
            CloseHandle(s_oh[i]);
        s_oh[i] = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
        s_oh_tid[i] = s_oh[i] ? tid : 0;
    }
    if (!s_oh[i])
        return;
    w->o[i].n = w_stack(s_oh[i], w->o[i].stack);
    if (p_nqit) {
        PVOID sa = NULL;
        if (p_nqit(s_oh[i], 9 /* ThreadQuerySetWin32StartAddress */, &sa, sizeof sa, NULL) >= 0)
            w->o[i].start = (DWORD64)(ULONG_PTR)sa;
    }
    {
        HMODULE k = GetModuleHandleA("kernel32.dll");
        gtd_fn gtd = k ? (gtd_fn)(void *)GetProcAddress(k, "GetThreadDescription") : NULL;
        PWSTR d = NULL;
        if (gtd && SUCCEEDED(gtd(s_oh[i], &d)) && d) {
            wcsncpy(w->o[i].desc, d, 47);
            w->o[i].desc[47] = 0;
            LocalFree(d);
        }
    }
}

static void w_finish(WStall *w, int64_t t0, int64_t dur, const WSample *b)
{
    /* the stall ended (dur ticks long): the samples over it */
    const WSample *a = w_before(t0);
    double us = 1e6 / (double)s_qpf.QuadPart;
    int i;
    w->dur_ms = (float)(dur * us / 1000.0);
    w->ran_ms = s_tsc_per_us > 0 ? (float)((b->ft_cyc - a->ft_cyc) / s_tsc_per_us / 1000.0) : -1.0f;
    w->pf = (int)(b->pf - a->pf);
    if (w->cpu >= 0 && w->cpu < s_ncpu_w) {
        w->cpu_dpc = (float)((b->dpc[w->cpu] - a->dpc[w->cpu]) / 1e4);
        w->cpu_isr = (float)((b->isr[w->cpu] - a->isr[w->cpu]) / 1e4);
        w->cpu_idle = (float)((b->idle[w->cpu] - a->idle[w->cpu]) / 1e4);
        w->cpu_busy = (float)((b->busy[w->cpu] - a->busy[w->cpu]) / 1e4);
    }
    w->max_di = 0.0f;
    w->max_di_cpu = -1;
    for (i = 0; i < s_ncpu_w; i++) {
        float di = (float)((b->dpc[i] - a->dpc[i] + b->isr[i] - a->isr[i]) / 1e4);
        if (di > w->max_di)
            w->max_di = di, w->max_di_cpu = i;
    }
    if (s_ws_w - s_ws_r < W_STALLS) {
        s_ws[s_ws_w % W_STALLS] = *w;
        s_ws_w++;
    }
}

static DWORD WINAPI hot_watch(LPVOID arg)
{
    HANDLE tm = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */,
                                       TIMER_ALL_ACCESS);
    int stalled = 0;
    LONG stall_seq = 0, stall_id = 0;
    int64_t stall_t0 = 0, det_t = 0, gate_t0 = 0;
    DWORD gate_tid = 0;
    int gate_done = 0;
    int64_t ms = s_qpf.QuadPart / 1000;
    double us = 1e6 / (double)s_qpf.QuadPart;
    static WStall cur;
    (void)arg;
    s_watch_tid = GetCurrentThreadId();
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    for (;;) {
        LONG s1, s2;
        int st, cpu;
        int64_t t, ex, now;
        WSample *smp;
        if (tm) {
            LARGE_INTEGER due;
            due.QuadPart = -10000;       /* 1 ms */
            SetWaitableTimer(tm, &due, 0, NULL, NULL, FALSE);
            WaitForSingleObject(tm, 100);
        } else {
            Sleep(1);
        }
        smp = &s_wr[s_wr_n % W_RING];
        w_sample(smp);
        s_wr_n++;
        now = smp->t;
        do {                             /* the stage, whole */
            s1 = s_st_seq;
            st = s_st;
            cpu = s_st_cpu;
            t = s_st_t;
            ex = s_st_exp;
            s2 = s_st_seq;
        } while ((s1 & 1) || s1 != s2);
        if (!t)
            continue;
        {
            /* the gate's holder: since when (as far as 1 ms samples see) */
            DWORD g = (DWORD)xbox_DispatchGateOwner();
            if (g != gate_tid) {
                gate_tid = g;
                gate_t0 = now;
                gate_done = 0;
            }
        }
        if (stalled) {
            int ended = 0;
            int64_t dur = now - stall_t0;
            if (cur.kind == W_K_STAGE && s1 != stall_seq) {
                ended = 1;
                if (s_long_seq == stall_seq)
                    dur = s_long_dur;
            } else if (cur.kind == W_K_TRAP && s_trap_t0 != stall_t0) {
                ended = 1;
                if (s_ep_id != stall_id) {
                    dur = s_ep_len;
                    cur.trap_n = s_ep_n;
                    cur.trap_max_ms = (float)(s_ep_max * us / 1000.0);
                }
            } else if (cur.kind == W_K_GAP && s_vp_t != stall_t0) {
                ended = 1;
                if (s_gap_id != stall_id)
                    dur = s_gap_len;
            } else if (cur.kind == W_K_GATE && (gate_tid != cur.o[W_O_GATE].tid ||
                                                (s_trap_t0 && now - s_trap_t0 > 3 * ms))) {
                ended = 1;                   /* let go (or a trap began: that one is named next) */
            }
            if (ended) {
                w_finish(&cur, stall_t0, dur, smp);
                stalled = 0;
            }
        }
        if (!stalled) {
            int kind = -1, i;
            int64_t tt = s_trap_t0, vt = s_vp_t, t0 = 0;
            if (tt && now - tt > 3 * ms) {
                kind = W_K_TRAP;
                t0 = tt;
                stall_id = s_ep_id;
            } else if (st != BW_ST_IDLE && now - t > (ex + 3000) * s_qpf.QuadPart / 1000000) {
                kind = W_K_STAGE;
                t0 = t;
                stall_seq = s1;
            } else if (st != BW_ST_IDLE && vt && now - vt > 15 * ms) {
                kind = W_K_GAP;
                t0 = vt;
                stall_id = s_gap_id;
            } else if (gate_tid && gate_tid != s_ft_tid && gate_tid != s_watch_tid && !gate_done &&
                       now - gate_t0 > 3 * ms) {
                kind = W_K_GATE;
                t0 = gate_t0;
                gate_done = 1;
            }
            if (kind >= 0) {
                /* a stall: the lock and the gate, the stacks, the states */
                DWORD gate = (DWORD)xbox_DispatchGateOwner();
                stalled = 1;
                stall_t0 = t0;
                det_t = now;
                memset(&cur, 0, sizeof cur);
                cur.kind = kind;
                cur.seq = s1;
                cur.st = st;
                cur.cpu = cpu;
                cur.t_s = (double)(t0 - s_t0) * us / 1e6;
                cur.exp_ms = kind == W_K_STAGE ? (float)(ex / 1000.0) : 0.0f;
                cur.seen_ms = (float)((now - t0) * us / 1000.0);
                cur.irql = xbox_IrqlRaisedCount();
                if (s_lock_cs) {
                    cur.o[W_O_LOCK].tid = (DWORD)(ULONG_PTR)s_lock_cs->OwningThread;
                    cur.lock_count = s_lock_cs->LockCount;
                }
                cur.o[W_O_GATE].tid = gate;
                for (i = 0; i < W_O_N; i++)
                    cur.o[i].role = w_role(cur.o[i].tid);
                if (s_ws_w < 64) {       /* the stacks for the first stalls only */
                    cur.n_ft = w_stack(s_ft_h, cur.ft_stack);
                    for (i = 0; i < W_O_N; i++)
                        w_other(&cur, i);
                }
                w_deep(&cur, 0);
            }
        } else if (cur.deep == 1 && now - det_t > 4 * ms) {
            ULONG hf0 = s_hardfaults;
            int i;
            w_deep(&cur, 1);             /* 4 ms on: still the same? */
            cur.hf = (int)(s_hardfaults - hf0);
            for (i = 0; i < W_O_N; i++)  /* and the other threads' stacks again, if they still hold it */
                if (cur.o[i].role >= 2 && s_oh[i] && s_oh_tid[i] == cur.o[i].tid && s_ws_w < 64 &&
                    (i == W_O_GATE ? (DWORD)xbox_DispatchGateOwner()
                                   : (s_lock_cs ? (DWORD)(ULONG_PTR)s_lock_cs->OwningThread : 0)) == cur.o[i].tid)
                    cur.o[i].n2 = w_stack(s_oh[i], cur.o[i].stack2);
        }
    }
    return 0;
}

static const char *w_state(int s)
{
    static const char *n[] = { "initialized", "ready (runnable, not running)", "running", "standby",
                               "terminated", "waiting", "transition", "deferred ready" };
    return s >= 0 && s < 8 ? n[s] : "other";
}

static const char *w_reason(int r)
{
    static const char *n[] = {
        "Executive", "FreePage", "PageIn", "PoolAllocation", "DelayExecution", "Suspended", "UserRequest",
        "WrExecutive", "WrFreePage", "WrPageIn", "WrPoolAllocation", "WrDelayExecution", "WrSuspended",
        "WrUserRequest", "WrEventPair", "WrQueue", "WrLpcReceive", "WrLpcReply", "WrVirtualMemory",
        "WrPageOut", "WrRendezvous", "WrKeyedEvent", "WrTerminated", "WrProcessInSwap", "WrCpuRateControl",
        "WrCalloutStack", "WrKernel", "WrResource", "WrPushLock", "WrMutex", "WrQuantumEnd", "WrDispatchInt",
        "WrPreempted", "WrYieldExecution", "WrFastMutex", "WrGuardedMutex", "WrRundown", "WrAlertByThreadId",
        "WrDeferredPreempt", "WrPhysicalFault", "WrIoRing", "WrMdlCache" };
    return r >= 0 && r < (int)(sizeof n / sizeof n[0]) ? n[r] : "?";
}

static void w_addr(char *o, size_t cap, DWORD64 a)
{
    HMODULE m = NULL;
    char path[MAX_PATH], *b;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(ULONG_PTR)a, &m) && m && GetModuleFileNameA(m, path, sizeof path)) {
        b = strrchr(path, '\\');
        _snprintf(o, cap, "%s+0x%llX", b ? b + 1 : path, (unsigned long long)(a - (DWORD64)(ULONG_PTR)m));
    } else {
        _snprintf(o, cap, "0x%llX", (unsigned long long)a);
    }
    o[cap - 1] = 0;
}

static void w_stack_str(char *o, size_t cap, const DWORD64 *s, int n)
{
    size_t k = 0;
    int i;
    o[0] = 0;
    for (i = 0; i < n && k + 100 < cap; i++) {
        char a[96];
        int w;
        w_addr(a, sizeof a, s[i]);
        w = _snprintf(o + k, cap - k, "%s%s", i ? " < " : "", a);
        if (w < 0)
            break;
        k += (size_t)w;
    }
    o[cap - 1] = 0;
}

static void w_thr_str(char *o, size_t cap, const WThr *t)
{
    int waiting = t->state == 5 || t->state == 1;
    if (!t->found)
        _snprintf(o, cap, "not found");
    else
        _snprintf(o, cap, "%s%s%s, priority %d (base %d), switches %u, kernel %.2f ms, user %.2f ms",
                  w_state(t->state), waiting ? " on " : "", waiting ? w_reason(t->reason) : "", t->prio,
                  t->base, t->cswitch, t->kern / 1e4, t->user / 1e4);
    o[cap - 1] = 0;
}

/* the writer thread prints the stalls (every 250 ms), and the trapped waits
 * every 5 s */
static void hot_watch_print(void)
{
    static int shown;
    static int64_t next_tw;
    static const char *o_name[W_O_N] = { "the APU lock", "the dispatch gate" };
    int64_t now = bw_hot_t();
    if (!next_tw)
        next_tw = now + 5 * s_qpf.QuadPart;
    if (now >= next_tw) {
        LONG n = InterlockedExchange(&s_tw_n, 0), h[5], mx = InterlockedExchange(&s_tw_max_us, 0);
        LONG en = InterlockedExchange(&s_ep_cnt, 0), emx = InterlockedExchange(&s_ep_max_us, 0);
        int i;
        for (i = 0; i < 5; i++)
            h[i] = InterlockedExchange(&s_tw_h[i], 0);
        if (n || en)
            fprintf(stderr, "[APU-HOT-TRAP] 5 s: trapped waits %ld (each asks 1 ms; ms: %s %ld, %s %ld, %s %ld,"
                            " %s %ld, %s %ld; the longest %.2f); trapped episodes %ld, the longest %.2f ms\n",
                    (long)n, s_tw_bin[0], (long)h[0], s_tw_bin[1], (long)h[1], s_tw_bin[2], (long)h[2],
                    s_tw_bin[3], (long)h[3], s_tw_bin[4], (long)h[4], mx / 1000.0, (long)en, emx / 1000.0);
        next_tw += 5 * s_qpf.QuadPart;
        if (next_tw < now)
            next_tw = now + 5 * s_qpf.QuadPart;
    }
    while (s_ws_r < s_ws_w) {
        static WStall w;
        char ft0[200], ft1[200], ds0[200], fts[1400], what[160];
        int i;
        w = s_ws[s_ws_r % W_STALLS];
        s_ws_r++;
        if (shown++ >= 400)
            continue;
        w_thr_str(ft0, sizeof ft0, &w.ft[0]);
        w_thr_str(ft1, sizeof ft1, &w.ft[1]);
        w_thr_str(ds0, sizeof ds0, &w.dsp[0]);
        w_stack_str(fts, sizeof fts, w.ft_stack, w.n_ft);
        if (w.kind == W_K_STAGE)
            _snprintf(what, sizeof what, "%s (it should take %.2f)",
                      w.st >= 0 && w.st < BW_ST_N ? s_st_name[w.st] : "?", w.exp_ms);
        else if (w.kind == W_K_TRAP)
            _snprintf(what, sizeof what, "%s: %d waits of 1 ms asked, the longest %.2f",
                      s_k_name[w.kind], w.trap_n, w.trap_max_ms);
        else
            _snprintf(what, sizeof what, "%s (the stage when seen: %s)", s_k_name[w.kind],
                      w.st >= 0 && w.st < BW_ST_N ? s_st_name[w.st] : "?");
        what[sizeof what - 1] = 0;
        fprintf(stderr, "[APU-HOT-STALL] at %.3f s, %.2f ms: %s; seen at %.2f; the frame thread ran %.2f ms of"
                        " it; its processor %d: DPC %.2f, interrupts %.2f, idle %.2f, busy %.2f ms; the most DPC +"
                        " interrupt time on any processor %.2f (cpu %d); page faults +%d, hard faults +%d; guest"
                        " IRQL raised %d\n",
                w.t_s, w.dur_ms, what, w.seen_ms, w.ran_ms, w.cpu, w.cpu_dpc, w.cpu_isr, w.cpu_idle,
                w.cpu_busy, w.max_di, w.max_di_cpu, w.pf, w.hf, w.irql);
        fprintf(stderr, "[APU-HOT-STALL]   the frame thread when seen: %s; 4 ms on: %s\n", ft0,
                w.deep > 1 ? ft1 : "(over by then)");
        for (i = 0; i < W_O_N; i++) {
            char who[200], st[96], t0[200], t1[200], stk[1400];
            if (w.o[i].start)
                w_addr(st, sizeof st, w.o[i].start);
            else
                strcpy(st, "?");
            if (w.o[i].role == 0)
                _snprintf(who, sizeof who, "free");
            else if (w.o[i].role == 1)
                _snprintf(who, sizeof who, "held by the frame thread");
            else
                _snprintf(who, sizeof who, "held by %s %lu (it started at %s%s%S)",
                          w.o[i].role == 2 ? "the DSP thread," : "thread", (unsigned long)w.o[i].tid, st,
                          w.o[i].desc[0] ? ", named " : "", w.o[i].desc);
            who[sizeof who - 1] = 0;
            if (w.o[i].role < 2) {
                fprintf(stderr, "[APU-HOT-STALL]   %s: %s\n", o_name[i], who);
                continue;
            }
            w_thr_str(t0, sizeof t0, &w.o[i].thr[0]);
            w_thr_str(t1, sizeof t1, &w.o[i].thr[1]);
            w_stack_str(stk, sizeof stk, w.o[i].stack, w.o[i].n);
            fprintf(stderr, "[APU-HOT-STALL]   %s: %s; when seen: %s; 4 ms on: %s; its stack: %s\n", o_name[i],
                    who, t0, w.deep > 1 ? t1 : "(over by then)", w.o[i].n ? stk : "(not taken)");
            if (w.o[i].n2) {
                w_stack_str(stk, sizeof stk, w.o[i].stack2, w.o[i].n2);
                fprintf(stderr, "[APU-HOT-STALL]   %s's holder 4 ms on, still holding it: its stack: %s\n", o_name[i], stk);
            }
        }
        fprintf(stderr, "[APU-HOT-STALL]   the DSP thread: %s; the frame thread's stack: %s; lock count %ld\n",
                ds0, w.n_ft ? fts : "(not taken)", (long)w.lock_count);
    }
}

static void hot_watch_start(void)
{
    const char *e = getenv("BW_APU_HOTSTAT_WATCH");
    HMODULE nt = GetModuleHandleA("ntdll.dll");
    HANDLE h;
    if (e && e[0] == '0')
        return;
    p_nqsi = nt ? (nqsi_fn)(void *)GetProcAddress(nt, "NtQuerySystemInformation") : NULL;
    p_nqit = nt ? (nqit_fn)(void *)GetProcAddress(nt, "NtQueryInformationThread") : NULL;
    s_spi_cap = 16u << 20;               /* the whole system's process list */
    s_spi = (BYTE *)VirtualAlloc(NULL, s_spi_cap, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    s_ft_tid = GetCurrentThreadId();     /* bw_hot_init runs on the frame thread */
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &s_ft_h,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0))
        s_ft_h = NULL;
    s_watch = 1;
    h = CreateThread(NULL, 256 * 1024, hot_watch, NULL, 0, NULL);
    if (!h) {
        s_watch = 0;
        fprintf(stderr, "[APU-HOT] no watcher thread: the watch is off\n");
        return;
    }
    CloseHandle(h);
    fprintf(stderr, "[APU-HOT] the watch (au-vp): a stage of the frame thread 3 ms past what it should take is"
                    " named ([APU-HOT-STALL]; BW_APU_HOTSTAT_WATCH=0 turns it off)%s\n",
            s_spi ? "" : "; no process list (no memory)");
}
