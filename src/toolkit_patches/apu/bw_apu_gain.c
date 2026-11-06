/* bw_apu_gain.c - the output gain at the clean level (call GAIN-1, lane
 * au-gain, docs/audio/gain.md) and its test aids; see bw_apu_gain.h. Blood
 * Wake file (part of xbox_apu). Everything here runs on the APU frame thread
 * (mcpx_apu_dsp_frame and mcpx_apu_monitor_frame), so nothing is locked.
 *
 * Before 0.3.1 Remastered's output had +4 dB and a look-ahead limiter at
 * -1 dBFS (BW_FIX AUDIO_MAKEUP_GAIN). In battles the limiter ducked the whole
 * mix by up to 5 dB on big explosions (au-hot's logs: 88 half-minute windows
 * in 257 battle boots). The gain is now set from the loudest peak measured
 * across the game (docs/audio/gain.md: missions, battles, split screen, the
 * storm, the cave; stereo and 5.1), so that peak lands just under -1 dBFS and
 * nothing ever needs limiting: every sample is the mix's own times one
 * factor, rounded to 16 bits, with no delay.
 *
 * The guard: a sample that would still pass -1 dBFS (a peak louder than any
 * measured) brings the gain down at once, just enough, and lets it back up
 * over about 100 ms; it is logged every time, to be investigated. While it is
 * idle (always, as measured) the output is exactly round(x * gain). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bw_apu_gain.h"
#include "../../bw_edition.h"

/* The gains (dB) that put the measured maximum just under the ceiling, per
 * layout (docs/audio/gain.md section 3). Measured (seen): the game's own mix
 * already reaches -0.9 dBFS in Original's battles and full scale in
 * Remastered's, stereo and the fold-down alike, so the clean level is 0 dB:
 * the stage is skipped and the output is the mix bit for bit. 5.1's six
 * channels peaked lower (-4.9 dBFS in two battles), but their downmix (what a
 * virtual-surround headset or Windows' fold to stereo gets) peaks with the
 * stereo, so 5.1 stays at 0 dB too (call GAIN-4). */
#define BW_GAIN_CLEAN_DB_2 0.0       /* stereo: the EP's mix, or the fold-down */
#define BW_GAIN_CLEAN_DB_6 0.0       /* 5.1: the fold-down's six channels */
/* -1 dBFS, in 16-bit units (full scale 32768). */
#define BW_GAIN_CEIL (32768.0 * 0.89125093813374556)
#define BW_RATE 48000

int g_bw_peak;

static int s_init;
static int s_on2, s_on6;               /* the gain applies (a factor other than 1) */
static double s_g2 = 1.0, s_g6 = 1.0;
static double s_db2, s_db6;
static double s_rel;                    /* the guard's release per sample */
static const char *s_why = "";

/* ---- the guard ---------------------------------------------------------- */

typedef struct {
    const char *name;
    double env;                          /* 1 = idle */
    unsigned long long n;                /* samples (frames) seen */
    unsigned long held, held_win;        /* samples under the guard: all, this 30 s */
    unsigned events, events_win;
    double deepest_win, deepest;         /* the least factor */
    double over_win;                     /* the loudest would-be sample, dBFS */
} BwGuard;

static BwGuard s_guard2 = { "stereo", 1.0, 0, 0, 0, 0, 0, 1.0, 1.0, -200.0 };
static BwGuard s_guard6 = { "5.1", 1.0, 0, 0, 0, 0, 0, 1.0, 1.0, -200.0 };

static double dbfs(double v)
{
    return v > 0.0 ? 20.0 * log10(v / 32768.0) : -200.0;
}

/* One stream's period: n frames of ch channels, gain g, in place. */
static void gain_period(BwGuard *G, int16_t *s, int n, int ch, double g)
{
    int k, c;
    for (k = 0; k < n; k++, s += ch) {
        double v[6], pk = 0.0, need = 1.0, e = G->env;
        for (c = 0; c < ch; c++) {
            v[c] = s[c] * g;
            if (fabs(v[c]) > pk)
                pk = fabs(v[c]);
        }
        if (pk > BW_GAIN_CEIL)
            need = (BW_GAIN_CEIL - 0.5) / pk;  /* rounded, still under the ceiling */
        if (e < 1.0) {
            e += (1.0 - e) * s_rel;           /* release toward 1 */
            if (e > 0.99999)
                e = 1.0;
        }
        if (need < e) {
            if (e == 1.0) {
                G->events++;
                G->events_win++;
                if (G->events <= 20)
                    fprintf(stderr, "[APU] BW gain guard (%s): at %.3f s of audio the mix with"
                            " the gain would reach %+.2f dBFS; held under -1 dBFS (GAIN-1: each"
                            " time the guard acts is logged, to be investigated)%s\n",
                            G->name, (double)(G->n + k) / BW_RATE, dbfs(pk),
                            G->events == 20 ? "; further events in the 30 s lines only" : "");
            }
            e = need;
        }
        G->env = e;
        if (e < 1.0) {
            G->held++;
            G->held_win++;
            if (e < G->deepest_win)
                G->deepest_win = e;
            if (e < G->deepest)
                G->deepest = e;
            if (dbfs(pk) > G->over_win)
                G->over_win = dbfs(pk);
            for (c = 0; c < ch; c++)
                v[c] *= e;
        }
        for (c = 0; c < ch; c++) {
            double o = nearbyint(v[c]);       /* round half to even, as lrint */
            s[c] = (int16_t)(o > 32767.0 ? 32767 : o < -32768.0 ? -32768 : (int)o);
        }
    }
    G->n += (unsigned)n;
    if (G->n % (BW_RATE * 30) < (unsigned)n && G->held_win) {
        fprintf(stderr, "[APU] BW gain guard (%s): %lu samples held in 30 s (%u events; %u in all),"
                " deepest %.2f dB, the loudest would-be sample %+.2f dBFS\n", G->name, G->held_win,
                G->events_win, G->events, 20.0 * log10(G->deepest_win), G->over_win);
        G->held_win = 0;
        G->events_win = 0;
        G->deepest_win = 1.0;
        G->over_win = -200.0;
    }
}

/* ---- the switches --------------------------------------------------------- */

static double env_db(const char *name, double dflt, int *set)
{
    const char *e = getenv(name);
    if (e && *e) {
        *set = 1;
        return atof(e);
    }
    return dflt;
}

void bw_gain_init(void)
{
    int forced = 0, on;
    const char *p;
    if (s_init)
        return;
    s_init = 1;
    p = getenv("BW_AUDIO_PEAKLOG");
    g_bw_peak = p && *p && *p != '0';
    /* Every edition (GAIN-1: a pure volume change). In Remastered and later
     * BW_FIX=-AUDIO_MAKEUP_GAIN (the launcher's "Louder output" unticked)
     * still turns it off; Original ignores BW_FIX, so there it is
     * BW_AUDIO_GAIN_DB=0. */
    on = bw_is_original() || bw_fix_enabled(BW_FIX_AUDIO_MAKEUP_GAIN);
    s_why = bw_is_original() ? "every edition" : on ? "every edition; BW_FIX AUDIO_MAKEUP_GAIN"
                                                    : "BW_FIX=-AUDIO_MAKEUP_GAIN";
    s_db2 = on ? BW_GAIN_CLEAN_DB_2 : 0.0;
    s_db6 = on ? BW_GAIN_CLEAN_DB_6 : 0.0;
    {
        double f = env_db("BW_AUDIO_GAIN_DB", 0.0, &forced);
        if (forced) {
            s_db2 = s_db6 = f;
            s_why = "BW_AUDIO_GAIN_DB";
        }
    }
    s_g2 = pow(10.0, s_db2 / 20.0);
    s_g6 = pow(10.0, s_db6 / 20.0);
    s_on2 = s_db2 != 0.0;
    s_on6 = s_db6 != 0.0;
    s_rel = 1.0 - exp(-1.0 / (0.100 * BW_RATE));
    if ((s_on2 || s_on6) && forced)
        fprintf(stderr, "[APU] BW output gain %+.2f dB stereo, %+.2f dB 5.1 (%s): every sample"
                        " times one factor, no delay; a guard holds anything past -1 dBFS and logs"
                        " each time it acts\n", s_db2, s_db6, s_why);
    else if (s_on2 || s_on6)
        fprintf(stderr, "[APU] BW output gain %+.2f dB stereo, %+.2f dB 5.1, at the clean level"
                        " (the loudest measured peak just under -1 dBFS: the waveform kept, no"
                        " limiting; a guard at -1 dBFS logs if it ever acts) (%s)\n",
                s_db2, s_db6, s_why);
    else
        fprintf(stderr, "[APU] BW output gain off: the mix as the game made it, bit for bit (the"
                        " clean level, GAIN-1: the game's own peaks reach within 1 dB of full"
                        " scale) (%s)\n", s_why);
    if (g_bw_peak)
        fprintf(stderr, "[APU] BW peak log on (BW_AUDIO_PEAKLOG, a test aid): the mix before the"
                        " gain every 10 s of audio\n");
}

/* ---- BW_AUDIO_PEAKLOG ---------------------------------------------------- */

typedef struct {
    double v;                 /* the peak (16-bit units, or 1.0-scaled for floats) */
    unsigned long long at;    /* the stereo stream's sample (frame) index then */
    int which;                /* channel or bin */
} BwPk;

static struct {
    BwPk mix, bins, fold2, fold6, dmx;
    unsigned long full, binover, fold2over, fold6over;
    unsigned long over[5];    /* mix samples past -5, -4, -3, -2, -1 dBFS: what a
                                 gain of +4..0 dB would push past the ceiling */
} s_w, s_run;
/* -5, -4, -3, -2, -1 dBFS in 16-bit units */
static const double s_over_at[5] = { 18426.6, 20673.6, 23197.0, 26028.2, 29204.7 };
static unsigned long long s_n2;         /* stereo frames seen by the stage */
static int s_seen_fold2, s_seen_fold6;

static void pk_note(BwPk *p, double v, int which)
{
    if (v > p->v) {
        p->v = v;
        p->at = s_n2;
        p->which = which;
    }
}

void bw_peak_bins(const float *bins, int nbins, int nsamples)
{
    int b, i;
    for (b = 0; b < nbins; b++)
        for (i = 0; i < nsamples; i++) {
            double a = fabs(bins[b * nsamples + i]);
            if (a >= 1.0)
                s_w.binover++;
            if (a > s_w.bins.v)
                pk_note(&s_w.bins, a, b);
        }
}

void bw_peak_fold2(float l, float r)
{
    double a = fabs(l), b = fabs(r);
    s_seen_fold2 = 1;
    if (a >= 1.0) s_w.fold2over++;
    if (b >= 1.0) s_w.fold2over++;
    pk_note(&s_w.fold2, a > b ? a : b, a > b ? 0 : 1);
}

void bw_peak_fold6(const float *ch6)
{
    int k;
    /* an ITU-style downmix, not normalised (what a device downmixing 5.1
     * without scaling would get; Windows' own matrix is not known here) */
    double l = ch6[0] + 0.70710678 * (ch6[2] + ch6[4]);
    double r = ch6[1] + 0.70710678 * (ch6[2] + ch6[5]);
    s_seen_fold6 = 1;
    for (k = 0; k < 6; k++) {
        double a = fabs(ch6[k]);
        if (a >= 1.0)
            s_w.fold6over++;
        pk_note(&s_w.fold6, a, k);
    }
    pk_note(&s_w.dmx, fabs(l) > fabs(r) ? fabs(l) : fabs(r), fabs(l) > fabs(r) ? 0 : 1);
}

static void run_keep(BwPk *run, const BwPk *w)
{
    if (w->v > run->v)
        *run = *w;
}

static void fmt_pk(char *buf, size_t n, const char *what, const BwPk *p, int seen, double unit)
{
    if (!seen)
        _snprintf(buf, n, "%s -", what);
    else if (p->v <= 0.0)
        _snprintf(buf, n, "%s silent", what);
    else
        _snprintf(buf, n, "%s %+.2f dBFS (%d) at %.3f s", what, 20.0 * log10(p->v / unit), p->which,
                  (double)p->at / BW_RATE);
    buf[n - 1] = 0;
}

static void peak_line(void)
{
    char a[96], b[96], c[96], d[96], e[96], f[96], g[96];
    double t1 = (double)s_n2 / BW_RATE;
    int k;
    run_keep(&s_run.mix, &s_w.mix);
    run_keep(&s_run.bins, &s_w.bins);
    run_keep(&s_run.fold2, &s_w.fold2);
    run_keep(&s_run.fold6, &s_w.fold6);
    run_keep(&s_run.dmx, &s_w.dmx);
    s_run.full += s_w.full;
    s_run.binover += s_w.binover;
    s_run.fold2over += s_w.fold2over;
    s_run.fold6over += s_w.fold6over;
    for (k = 0; k < 5; k++)
        s_run.over[k] += s_w.over[k];
    fmt_pk(a, sizeof a, "mix", &s_w.mix, 1, 32768.0);
    fmt_pk(b, sizeof b, "bins", &s_w.bins, 1, 1.0);
    fmt_pk(c, sizeof c, "fold", &s_w.fold2, s_seen_fold2, 1.0);
    fmt_pk(d, sizeof d, "5.1", &s_w.fold6, s_seen_fold6, 1.0);
    fmt_pk(e, sizeof e, "dmx", &s_w.dmx, s_seen_fold6, 1.0);
    fmt_pk(f, sizeof f, "run mix", &s_run.mix, 1, 32768.0);
    fmt_pk(g, sizeof g, "run bins", &s_run.bins, 1, 1.0);
    fprintf(stderr, "[APU] BW peak %.0f-%.0f s: %s, %lu at full scale, past -5/-4/-3/-2/-1 dBFS"
            " %lu/%lu/%lu/%lu/%lu | %s, %lu over 1.0 | %s, %lu over | %s, %lu over; %s | %s, %lu at"
            " full scale; %s",
            t1 - 10.0, t1, a, s_w.full, s_w.over[0], s_w.over[1], s_w.over[2], s_w.over[3],
            s_w.over[4], b, s_w.binover, c, s_w.fold2over, d, s_w.fold6over, e, f, s_run.full, g);
    if (s_seen_fold6) {
        char h[96], i[96];
        fmt_pk(h, sizeof h, "run 5.1", &s_run.fold6, 1, 1.0);
        fmt_pk(i, sizeof i, "run dmx", &s_run.dmx, 1, 1.0);
        fprintf(stderr, "; %s, %lu over; %s", h, s_run.fold6over, i);
    }
    if (s_seen_fold2) {
        char h[96];
        fmt_pk(h, sizeof h, "run fold", &s_run.fold2, 1, 1.0);
        fprintf(stderr, "; %s, %lu over", h, s_run.fold2over);
    }
    fprintf(stderr, "\n");
    memset(&s_w, 0, sizeof s_w);
}

static void peak_mix2(const int16_t *s, int n)
{
    int k, c;
    for (k = 0; k < n; k++) {
        for (c = 0; c < 2; c++) {
            int v = s[2 * k + c];
            int a = v < 0 ? -v : v, j;
            if (a >= 32767)
                s_w.full++;
            for (j = 0; j < 5 && a > s_over_at[j]; j++)
                s_w.over[j]++;
            if (a > s_w.mix.v) {
                s_w.mix.v = a;
                s_w.mix.at = s_n2 + k;
                s_w.mix.which = c;
            }
        }
    }
}

/* ---- BW_AUDIO_WAV_PRE ---------------------------------------------------- */

typedef struct {
    FILE *f;
    int ch;
    uint32_t frames;
} BwWav;

static BwWav s_pre2, s_pre6;
static int s_pre_tried;

static void wav_hdr(BwWav *w)
{
    uint8_t h[44];
    uint32_t data = w->frames * 2u * (uint32_t)w->ch, riff = 36 + data, sr = BW_RATE;
    uint32_t br = BW_RATE * 2u * (uint32_t)w->ch, fl = 16;
    uint16_t pcm = 1, ch = (uint16_t)w->ch, ba = (uint16_t)(2 * w->ch), bps = 16;
    long pos = ftell(w->f);
    memcpy(h, "RIFF", 4); memcpy(h + 4, &riff, 4); memcpy(h + 8, "WAVEfmt ", 8);
    memcpy(h + 16, &fl, 4); memcpy(h + 20, &pcm, 2); memcpy(h + 22, &ch, 2);
    memcpy(h + 24, &sr, 4); memcpy(h + 28, &br, 4); memcpy(h + 32, &ba, 2);
    memcpy(h + 34, &bps, 2); memcpy(h + 36, "data", 4); memcpy(h + 40, &data, 4);
    fseek(w->f, 0, SEEK_SET);
    fwrite(h, 1, 44, w->f);
    fseek(w->f, pos, SEEK_SET);
    fflush(w->f);
}

static void pre_open(void)
{
    const char *p = getenv("BW_AUDIO_WAV_PRE");
    s_pre_tried = 1;
    if (p && *p) {
        char p6[1024];
        static const uint8_t zero[44];
        _snprintf(p6, sizeof p6, "%s.6.wav", p);
        p6[sizeof p6 - 1] = 0;
        s_pre2.f = fopen(p, "wb");
        s_pre2.ch = 2;
        s_pre6.f = fopen(p6, "wb");
        s_pre6.ch = 6;
        if (s_pre2.f)
            fwrite(zero, 1, 44, s_pre2.f);
        if (s_pre6.f)
            fwrite(zero, 1, 44, s_pre6.f);
        fprintf(stderr, "[APU] BW the mix before the gain to %s (and %s with a 5.1 output):"
                        " BW_AUDIO_WAV_PRE, a test aid\n", p, p6);
    }
}

static void pre_write(BwWav *w, const int16_t *s, int n)
{
    if (!w->f)
        return;
    fwrite(s, 2u * (unsigned)w->ch, (size_t)n, w->f);
    w->frames += (uint32_t)n;
    if (w->frames % BW_RATE < (uint32_t)n)
        wav_hdr(w);
}

/* ---- the stages ------------------------------------------------------------ */

void bw_gain_stage2(int16_t *s, int n, int in_wav)
{
    if (!s_init)
        bw_gain_init();
    if (!s_pre_tried)
        pre_open();
    if (g_bw_peak)
        peak_mix2(s, n);
    if (in_wav)
        pre_write(&s_pre2, s, n);
    if (s_on2)
        gain_period(&s_guard2, s, n, 2, s_g2);
    s_n2 += (unsigned)n;
    if (g_bw_peak && s_n2 % (BW_RATE * 10) < (unsigned)n)
        peak_line();
}

void bw_gain_stage6(int16_t *s, int n)
{
    if (!s_init)
        bw_gain_init();
    if (!s_pre_tried)
        pre_open();
    pre_write(&s_pre6, s, n);
    if (s_on6)
        gain_period(&s_guard6, s, n, 6, s_g6);
}
