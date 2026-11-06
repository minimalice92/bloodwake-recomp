/*
 * MCPX APU DSP (GP/EP) - Stub implementation
 *
 * The DSP Global Processor (GP) and Encode Processor (EP) handle effects
 * processing (reverb, chorus, etc.) and final output encoding. The full
 * DSP is ~3000 lines of DSP56300 emulation code.
 *
 * For initial audio, we bypass the DSP entirely:
 * - VP mixbins are passed directly to the EP output
 * - GP effects processing is skipped
 * - The EP just copies mixbin 0/1 (front L/R) to the monitor buffer
 *
 * This gives us basic voice playback without effects. The DSP can be
 * connected later for reverb, EQ, and other processing.
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2019-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include "apu_state.h"
#include "fpconv.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Blood Wake carried patch (docs/TOOLKIT_ISSUES.md TK-16): mixbin
 * diagnostics. BW_APU_BINSTAT=1 prints, once per second of APU time, the RMS
 * of every mixbin that carried anything and its submix headroom, and every
 * five seconds the full routing (eight bins and volumes) of each voice that
 * played. The original file is xboxrecomp/src/apu/apu_dsp.c (sha256 guarded
 * in CMakeLists.txt). */
static int s_bw_binstat = -1;
static double s_bw_bin_sq[NUM_MIXBINS];
static unsigned s_bw_bin_frames, s_bw_bin_secs;
static uint8_t s_bw_seen[256];

/* Diagnostic (Blood Wake carried patch): BW_APU_PAGECHECK=<voice> prints,
 * every 10 s of APU time, each segment of that stream voice's two SSLs with
 * how the resolver classes its page and what each copy holds: the RAM copy
 * (heap/image numbering) and the contiguous-window copy, by RMS and
 * zero-crossing rate of the first 1024 stereo s16 frames. Music reads as a
 * low crossing rate; whichever copy reads as noise is the wrong one. */
extern const volatile uint8_t *xbox_PhysRamNameMap(void);
extern const volatile uint8_t *xbox_ContiguousPageMap(void);

static void bw_copy_stats(const uint8_t *p, unsigned frames, double *rms, double *zcr)
{
    const int16_t *s = (const int16_t *)p;
    double sq = 0.0;
    unsigned zc = 0;
    for (unsigned i = 0; i < frames; i++) {
        sq += (double)s[2 * i] * s[2 * i];
        if (i && ((s[2 * i] < 0) != (s[2 * i - 2] < 0)))
            zc++;
    }
    *rms = frames ? sqrt(sq / frames) : 0.0;
    *zcr = frames > 1 ? (double)zc / (frames - 1) : 0.0;
}

static void bw_pagecheck(MCPXAPUState *d)
{
    static int voice = -2;
    static unsigned frames;
    if (voice == -2) {
        const char *e = getenv("BW_APU_PAGECHECK");
        voice = e && *e ? atoi(e) : -1;
    }
    if (voice < 0 || voice >= MCPX_HW_MAX_VOICES || ++frames % 15000)
        return;
    for (int idx = 0; idx < MCPX_HW_SSLS_PER_VOICE; idx++) {
        const MCPXAPUVPSSLData *s = &d->vp.ssl[voice];
        for (int k = 0; k < s->count[idx]; k++) {
            uint32_t ent = d->regs[NV_PAPU_VPSSLADDR] + (s->base[idx] + k) * 8;
            uint32_t off = *(const uint32_t *)mcpx_apu_phys(ent);
            uint32_t len = *(const uint32_t *)mcpx_apu_phys(ent + 4);
            uint32_t a = off & 0x0FFFFFFFu;
            unsigned n = (len & 0xFFFF) < 1024 ? (len & 0xFFFF) : 1024;
            double rr = -1, zr = -1, rw = -1, zw = -1;
            if (a + n * 4 <= (64u << 20)) {
                bw_copy_stats(d->ram_ptr + a, n, &rr, &zr);
                bw_copy_stats(d->ram_ptr + 0x80000000u + a, n, &rw, &zw);
            }
            fprintf(stderr, "[APU-PAGE] f=%u v=%d ssl=%d seg=%d phys=%08X len=%u"
                    " class=%s named=%u win=%u ram:rms=%.0f,zcr=%.2f"
                    " window:rms=%.0f,zcr=%.2f\n",
                    frames / 1500, voice, idx, k, off, len & 0xFFFF,
                    mcpx_apu_phys_class(off),
                    a < (64u << 20) ? xbox_PhysRamNameMap()[a >> 12] : 0,
                    a < (64u << 20) ? xbox_ContiguousPageMap()[a >> 12] : 0,
                    rr, zr, rw, zw);
        }
    }
}

/* Diagnostic (Blood Wake carried patch): BW_APU_STEMS=<prefix> writes two
 * stems from the same mixbins as the output, for telling by ear (or by
 * measurement) which part of the mix an artifact lives in:
 * <prefix>.front.wav = bins 0/1 plus 0.71 x (2, 4/5); <prefix>.3d.wav = bins
 * 6/7 plus 0.71 x (8/9). 48 kHz stereo s16; the header is rewritten every
 * second so an abrupt end still leaves a valid file. */
typedef struct { FILE *f; uint32_t frames; } BwStem;
static BwStem s_bw_stem[2];
static int s_bw_stems = -1;
static unsigned s_bw_stem_tick;

static void bw_stem_header(BwStem *st)
{
    uint8_t h[44];
    uint32_t data = st->frames * 4, riff = 36 + data, sr = 48000, br = 48000 * 4;
    uint16_t pcm = 1, ch = 2, ba = 4, bps = 16;
    uint32_t fmtlen = 16;
    memcpy(h, "RIFF", 4); memcpy(h + 4, &riff, 4); memcpy(h + 8, "WAVEfmt ", 8);
    memcpy(h + 16, &fmtlen, 4); memcpy(h + 20, &pcm, 2); memcpy(h + 22, &ch, 2);
    memcpy(h + 24, &sr, 4); memcpy(h + 28, &br, 4); memcpy(h + 32, &ba, 2);
    memcpy(h + 34, &bps, 2); memcpy(h + 36, "data", 4); memcpy(h + 40, &data, 4);
    long pos = ftell(st->f);
    fseek(st->f, 0, SEEK_SET);
    fwrite(h, 1, 44, st->f);
    fseek(st->f, pos, SEEK_SET);
    fflush(st->f);
}

static int16_t bw_s16(float x)
{
    if (x > 1.0f) x = 1.0f;
    if (x < -1.0f) x = -1.0f;
    return (int16_t)(x * 32767.0f);
}

static void bw_stems_frame(float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    if (s_bw_stems < 0) {
        const char *e = getenv("BW_APU_STEMS");
        s_bw_stems = 0;
        if (e && *e) {
            char name[600];
            const char *suffix[2] = { "front", "3d" };
            for (int k = 0; k < 2; k++) {
                snprintf(name, sizeof(name), "%s.%s.wav", e, suffix[k]);
                s_bw_stem[k].f = fopen(name, "wb");
                if (s_bw_stem[k].f) {
                    uint8_t zero[44] = { 0 };
                    fwrite(zero, 1, 44, s_bw_stem[k].f);
                    s_bw_stems = 1;
                }
            }
            fprintf(stderr, "[APU] BW stems -> %s.front.wav / .3d.wav\n", e);
        }
    }
    if (!s_bw_stems)
        return;
    {
        const float c = 0.70710678f;
        int16_t a[NUM_SAMPLES_PER_FRAME][2], b[NUM_SAMPLES_PER_FRAME][2];
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            a[i][0] = bw_s16(mixbins[0][i] + c * (mixbins[2][i] + mixbins[4][i]));
            a[i][1] = bw_s16(mixbins[1][i] + c * (mixbins[2][i] + mixbins[5][i]));
            b[i][0] = bw_s16(mixbins[6][i] + c * mixbins[8][i]);
            b[i][1] = bw_s16(mixbins[7][i] + c * mixbins[9][i]);
        }
        if (s_bw_stem[0].f) { fwrite(a, 4, NUM_SAMPLES_PER_FRAME, s_bw_stem[0].f); s_bw_stem[0].frames += NUM_SAMPLES_PER_FRAME; }
        if (s_bw_stem[1].f) { fwrite(b, 4, NUM_SAMPLES_PER_FRAME, s_bw_stem[1].f); s_bw_stem[1].frames += NUM_SAMPLES_PER_FRAME; }
    }
    if (++s_bw_stem_tick % 1500 == 0)
        for (int k = 0; k < 2; k++)
            if (s_bw_stem[k].f)
                bw_stem_header(&s_bw_stem[k]);
}

/* TK-16: BW_APU_MIXDOWN=stereo (default) or toolkit (the original mixdown,
 * itself switched by RECOMP_APU_MIXDOWN_ALL). */
static int bw_stereo_mixdown(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("BW_APU_MIXDOWN");
        on = !(e && !strcmp(e, "toolkit"));
        fprintf(stderr, "[APU] BW mixdown: %s\n",
                on ? "stereo fold-down of the speaker bins (TK-16)"
                   : "toolkit (every bin, even left / odd right)");
    }
    return on;
}

static void bw_binstat_frame(MCPXAPUState *d,
                             float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    if (s_bw_binstat < 0) {
        const char *e = getenv("BW_APU_BINSTAT");
        s_bw_binstat = e && *e && *e != '0';
    }
    if (!s_bw_binstat)
        return;
    for (int b = 0; b < NUM_MIXBINS; b++)
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++)
            s_bw_bin_sq[b] += (double)mixbins[b][i] * mixbins[b][i];
    for (int v = 0; v < 256; v++) {
        if (g_dbg.vp.v[v].active) {
            s_bw_seen[v] = 1;
            g_dbg.vp.v[v].active = false;   /* voice_process sets it again */
        }
    }
    if (++s_bw_bin_frames < 1500)
        return;
    s_bw_bin_secs++;
    {
        char line[2048];
        int n = snprintf(line, sizeof(line), "[APU-BINS] t=%us", s_bw_bin_secs);
        for (int b = 0; b < NUM_MIXBINS && n < (int)sizeof(line) - 40; b++) {
            double rms = sqrt(s_bw_bin_sq[b] / (1500.0 * NUM_SAMPLES_PER_FRAME));
            if (rms > 0.0)
                n += snprintf(line + n, sizeof(line) - n, " b%d=%.1fdB/hr%u",
                              b, 20.0 * log10(rms), d->vp.submix_headroom[b]);
        }
        fprintf(stderr, "%s\n", line);
    }
    if (s_bw_bin_secs % 5 == 1) {
        for (int v = 0; v < 256; v++) {
            const struct McpxApuDebugVoice *x = &g_dbg.vp.v[v];
            if (!s_bw_seen[v])
                continue;
            fprintf(stderr, "[APU-ROUTE] t=%us v=%d %s", s_bw_bin_secs, v,
                    x->stereo ? "stereo" : "mono");
            if (x->multipass)
                fprintf(stderr, " multipass<-b%u", x->multipass_bin);
            for (int s = 0; s < 8; s++)
                fprintf(stderr, " %u:%03X", x->bin[s], x->vol[s]);
            fprintf(stderr, "\n");
        }
    }
    memset(s_bw_seen, 0, sizeof(s_bw_seen));
    memset(s_bw_bin_sq, 0, sizeof(s_bw_bin_sq));
    s_bw_bin_frames = 0;
}

/* ── DSP command doorbell acknowledgement ────────────────────────────────
 *
 * DirectSound does not stop at creating the device. It hands the audio DSP a
 * command block in guest RAM, writes a command word, and spins until the DSP
 * writes zero back. On real hardware the GP runs a DSP56300 program that does
 * that. Here the DSP is a passthrough stub, so the word never changes and the
 * title hangs inside DirectSound initialisation -- which on Wreckless gates the
 * entire engine, not just audio.
 *
 * RECOMP_APU_DSP_ACK=<addr>[,<addr>...] clears those guest dwords once per APU
 * frame, which is what "the command completed" looks like to the title.
 *
 * RECOMP_APU_DSP_ACK=auto, the default when the variable is unset, finds the
 * doorbell from what the title programs; "0" or "off" turns the ack off.
 * The XDK DSOUND GP program takes its commands through the first page of
 * the GP's scratch space: a 2 KB command FIFO, then a header whose dword at
 * +0x810 is the doorbell the host writes and the DSP program clears (the
 * DSOUND submit of every XDK title seen spins on <block>+0x810). The
 * scratch pages are what NV_PAPU_GPSADDR's scatter-gather table maps -- 8
 * bytes an entry, the physical page in the first dword, as xemu's apu.c
 * scratch_rw walks it -- so the doorbell is SGE[0] + 0x810 wherever the
 * contiguous allocator put the block. A fixed address moves with allocation
 * order (reserving physical page 0 moved one title's by a page); this does
 * not. Wreckless fits too: its doorbell 0x014F8810 is the first page of the
 * scratch its GPSADDR table (0x01504000) maps, not the table itself, which
 * is where the earlier note here looked.
 *
 * ponytail: this is a handshake acknowledgement, not a DSP. It says every
 * command succeeded instantly and computes nothing, so anything whose *result*
 * the title reads back will still be wrong. The real fix is DSP56300 emulation
 * in the GP/EP; this exists so audio init stops blocking everything behind it.
 */
#define APU_DSP_ACK_MAX 8
#define GP_DOORBELL_OFFSET 0x810u
static uint32_t s_dsp_ack[APU_DSP_ACK_MAX];
static int s_dsp_ack_count = -1;
static int s_dsp_ack_auto;                /* =auto: derive from GPSADDR */
static uint32_t s_dsp_ack_auto_page;      /* the page last reported */
static volatile int s_gp_in_reset;        /* last GPRST write held the GP */

static void dsp_ack_init(void)
{
    const char *spec = getenv("RECOMP_APU_DSP_ACK");
    char buf[256], *p, *end;

    s_dsp_ack_count = 0;
    if (!spec || !*spec)
        spec = "auto";                    /* default: derive it */
    if (!strcmp(spec, "0") || !strcmp(spec, "off"))
        return;
    if (!strcmp(spec, "auto")) {
        s_dsp_ack_auto = 1;
        fprintf(stderr, "[APU] DSP doorbell ack: auto (GP scratch page 0"
                        " + 0x%X, from GPSADDR)\n", GP_DOORBELL_OFFSET);
        return;
    }
    strncpy(buf, spec, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    for (p = buf; *p && s_dsp_ack_count < APU_DSP_ACK_MAX; ) {
        unsigned long v = strtoul(p, &end, 0);
        if (end == p)
            break;
        if (v)
            s_dsp_ack[s_dsp_ack_count++] = (uint32_t)v;
        p = (*end == ',') ? end + 1 : end;
    }
    if (s_dsp_ack_count)
        fprintf(stderr, "[APU] DSP doorbell ack: %d address(es), first 0x%08X\n",
                s_dsp_ack_count, s_dsp_ack[0]);
}

/* SUM EVERY MIXBIN THE GUEST ROUTED TO, NOT JUST THE FIRST TWO.
 *
 * Default ON. RECOMP_APU_MIXDOWN_ALL=0 restores the previous two-bin read,
 * because this changes audible output for every title and an escape hatch
 * costs one branch. */
static int mcpx_apu_mixdown_all(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_APU_MIXDOWN_ALL");
        on = (e && *e) ? (atoi(e) != 0) : 1;
    }
    return on;
}

/* GPRST (GP region + 0xFFFC): the GP runs only with both reset bits set, as
 * in xemu's gp_write. Nothing else models the GP, so this is kept for the
 * =auto ack alone. Before the first write the GP counts as running: a title
 * that never touches GPRST keeps its ack. */
void mcpx_apu_dsp_gprst_write(uint32_t val)
{
    const uint32_t run = NV_PAPU_GPRST_GPRST | NV_PAPU_GPRST_GPDSPRST;
    int held = (val & run) != run;

    if (held != s_gp_in_reset && s_dsp_ack_auto)
        fprintf(stderr, "[APU] GPRST 0x%X: GP %s; DSP doorbell ack %s\n",
                val, held ? "held in reset" : "running",
                held ? "paused" : "resumed");
    s_gp_in_reset = held;
}

/* The doorbell for =auto: SGE[0] of the GP scratch table + 0x810, re-read on
 * every poll, so the slot always lies in the page the table names now. NULL
 * while GPSADDR is unset, the table's first entry names no page, or the GP
 * is held in reset: a title that tears DirectSound down frees the scratch,
 * and a dword cleared there after that would land in whatever reuses the
 * page. */
static uint32_t *dsp_ack_auto_slot(MCPXAPUState *d)
{
    uint32_t sge = qatomic_read(&d->regs[NV_PAPU_GPSADDR]), page;

    if (!sge || s_gp_in_reset)
        return NULL;
    page = *(const uint32_t *)mcpx_apu_phys(sge);
    if (!page || (page & 0xFFF))
        return NULL;                       /* table not filled in yet */
    if (page != s_dsp_ack_auto_page) {
        s_dsp_ack_auto_page = page;
        fprintf(stderr, "[APU] DSP doorbell: GP scratch SGE table 0x%08X maps"
                        " page 0x%08X (%s); doorbell at physical 0x%08X\n",
                sge, page, mcpx_apu_phys_class(page),
                page + GP_DOORBELL_OFFSET);
    }
    return (uint32_t *)(mcpx_apu_phys(page) + GP_DOORBELL_OFFSET);
}

/* D-012 (Blood Wake): BW_APU_DSP=xemu runs the GP and EP on xemu's DSP
 * interpreter (bw_apu_xdsp.c). Then the DSP answers DirectSound's doorbell
 * itself, so the stub's auto-ack stands down, and the DSP stage is its. */
int bw_xdsp_enabled(void);
void bw_xdsp_init(MCPXAPUState *d);
int bw_xdsp_frame(MCPXAPUState *d, float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME]);

void mcpx_apu_dsp_ack_poll(MCPXAPUState *d)
{
    if (bw_xdsp_enabled())
        return;
    int i;

    if (s_dsp_ack_count < 0)
        dsp_ack_init();
    if (!d->ram_ptr)
        return;
    if (s_dsp_ack_auto) {
        uint32_t *slot = dsp_ack_auto_slot(d);
        if (slot && *slot) {
            static int shown;
            if (shown++ < 3)
                fprintf(stderr, "[APU] DSP doorbell (auto): command 0x%08X"
                                " acknowledged\n", *slot);
            *slot = 0;
        }
    }
    for (i = 0; i < s_dsp_ack_count; i++) {
        uint32_t *slot = (uint32_t *)(d->ram_ptr + s_dsp_ack[i]);
        if (*slot) {
            static int shown[APU_DSP_ACK_MAX];
            if (shown[i]++ < 3)
                fprintf(stderr, "[APU] DSP doorbell 0x%08X: command 0x%08X"
                                " acknowledged\n", s_dsp_ack[i], *slot);
            *slot = 0;
        }
    }
}

void mcpx_apu_dsp_init(MCPXAPUState *d)
{
    /* The doorbell ack's state is file-static, so it outlives an instance:
     * a GP the last instance held in reset would leave this one's ack
     * paused, and the RECOMP_APU_DSP_ACK setting would never be read again.
     * Start from what a fresh process has. */
    s_dsp_ack_count = -1;                  /* re-read on the first poll */
    s_dsp_ack_auto = 0;
    s_dsp_ack_auto_page = 0;
    s_gp_in_reset = 0;

    /* Allocate minimal DSP state for GP and EP.
     * We need these to exist so reset doesn't crash,
     * but they won't actually run DSP programs. */
    d->gp.dsp = (DSPState *)calloc(1, sizeof(DSPState));
    d->ep.dsp = (DSPState *)calloc(1, sizeof(DSPState));

    if (d->gp.dsp) d->gp.dsp->is_gp = true;
    if (d->ep.dsp) d->ep.dsp->is_gp = false;

    d->gp.realtime = false;
    d->ep.realtime = false;

    fprintf(stderr, "[APU] DSP GP/EP initialized (STUBBED - passthrough mode)\n");
    bw_xdsp_init(d);
}

/* Called after the frame thread is joined. Only mcpx_apu_reset_locked
 * (currently uncalled) reads this state, and it would run on that thread.
 * Safe to call twice. */
void mcpx_apu_dsp_finalize(MCPXAPUState *d)
{
    free(d->gp.dsp);
    free(d->ep.dsp);
    d->gp.dsp = NULL;
    d->ep.dsp = NULL;
}

void mcpx_apu_update_dsp_preference(MCPXAPUState *d)
{
    /* In the real xemu, this reads settings to decide whether
     * GP/EP should run in realtime or cached mode. We ignore it. */
    (void)d;
}

/* Blood Wake (Remastered AUDIO_SURROUND): the 5.1 period, filled beside
 * frame_buf by the fold-down when the device output has 6 channels, handed
 * to the device by apu_core.c's monitor_frame. */
int16_t g_bw_frame6[256][6];
extern int xa2_output_channels(void);
#include "bw_apu_gain.h"   /* Blood Wake (GAIN-1): BW_AUDIO_PEAKLOG's views, a test aid */

void mcpx_apu_dsp_frame(MCPXAPUState *d,
                         float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    /* Bypass DSP: take mixbin 0 (front-left) and mixbin 1 (front-right)
     * and write them directly to the monitor frame buffer as the final
     * EP output.
     *
     * The Xbox DirectSound typically routes:
     *   Mixbin 0 = Front Left
     *   Mixbin 1 = Front Right
     *   Mixbin 2 = Center (often unused in stereo)
     *   Mixbin 3 = LFE
     *   Mixbin 4-5 = Rear L/R
     *
     * For stereo output, bins 0 and 1 are what we want.
     */

    int off = (d->ep_frame_div % 8) * NUM_SAMPLES_PER_FRAME;

    {
        /* Blood Wake (D-012 scoping): BW_APU_GPDUMP=<file> writes the GP and
         * EP DSP memory windows (MCPX 0x30000-0x7FFFF, guest 0xFE830000..,
         * plain memory holding what DirectSound loaded) once, 25 s into the
         * audio, to see how much DSP code the title runs. */
        static long frames;
        static const char *path;
        static int checked;
        if (!checked) {
            checked = 1;
            path = getenv("BW_APU_GPDUMP");
        }
        if (path && ++frames == 48000 / NUM_SAMPLES_PER_FRAME * 25) {
            extern ptrdiff_t xbox_GetMemoryOffset(void);
            const uint8_t *p = (const uint8_t *)(uintptr_t)(0xFE830000u + xbox_GetMemoryOffset());
            FILE *f = fopen(path, "wb");
            if (f) {
                fwrite(p, 1, 0x50000, f);
                fclose(f);
                fprintf(stderr, "[APU] GP/EP memory (0xFE830000, 320 KB) written to %s\n", path);
            }
            {
                /* The GP's boot ROM copies its program from scratch memory
                 * (xemu dsp_bootstrap: the first 0x800 words). Scratch is
                 * the pages GPSADDR's SGE table names (8-byte entries, page
                 * address first): the first 16 pages to <file>.scratch. */
                char sp[600];
                uint32_t sge = qatomic_read(&d->regs[NV_PAPU_GPSADDR]), i, page;
                static const uint8_t zero[4096];
                _snprintf(sp, sizeof sp, "%s.scratch", path);
                sp[sizeof sp - 1] = 0;
                f = sge ? fopen(sp, "wb") : NULL;
                if (f) {
                    for (i = 0; i < 16; i++) {
                        page = *(const uint32_t *)mcpx_apu_phys(sge + i * 8);
                        fwrite(page && !(page & 0xFFF) ? (const void *)mcpx_apu_phys(page)
                                                       : (const void *)zero, 1, 4096, f);
                    }
                    fclose(f);
                    fprintf(stderr, "[APU] GP scratch (SGE table 0x%08X, 16 pages) written to %s\n",
                            sge, sp);
                }
            }
        }
    }

    bw_binstat_frame(d, mixbins);
    if (g_bw_peak)
        bw_peak_bins(&mixbins[0][0], NUM_MIXBINS, NUM_SAMPLES_PER_FRAME);
    if (bw_xdsp_frame(d, mixbins)) {
        bw_stems_frame(mixbins);
        return;                            /* D-012: the DSPs made the output */
    }
    bw_pagecheck(d);
    bw_stems_frame(mixbins);

    if (d->monitor.point != MCPX_APU_DEBUG_MON_VP) {
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            /* Bins 2..31 used to be computed and then dropped on the floor.
             * On hardware the GP and EP mix the submixes down; here they are
             * stubs, so thirty of thirty-two bins were discarded every frame
             * with no counter anywhere to say so.
             *
             * Measured on Jet Set Radio Future, one 200 s gameplay run, with
             * a positive control moving beside it:
             *
             *     [APU-BIN] 2D heard=557466 lost=0
             *               3D heard=0      lost=377768
             *               lost by bin: 6,7,8,9,10
             *
             * 557,466 music voice-frames heard and none lost; 377,768 effect
             * voice-frames produced correctly and thrown away. The title's 3D
             * positional voices -- its sound effects -- are routed to bins 6
             * to 10 by the guest's own V0BIN..V3BIN, and music on 2D voices
             * lands in bins 0 and 1, which is why the music was always
             * audible and no effect ever was. Every instrument upstream of
             * this line read healthy.
             *
             * Gating the HRTF submix override was tried first and did not fix
             * it, so the defect is the width of this mixdown and nothing else.
             *
             * Even bins left, odd bins right, which preserves the stereo
             * pairing the guest set up -- bins 6/7 and 8/9 arrive with matched
             * counts. This is not what a real EP does; it is the cheapest
             * mixdown that stops discarding audio. */
            float left, right;
            if (bw_stereo_mixdown()) {
                /* TK-16 (Blood Wake carried patch): a stereo fold-down of
                 * the speaker bins instead of "even bins left, odd bins
                 * right" at unity. That put the LFE bin (3) and the I3DL2
                 * reverb send (10) -- a full-level copy of every 3D effect
                 * -- in one ear each. Bins: 0/1 front, 2 centre, 3 LFE,
                 * 4/5 rear, 6-9 the 3D (crosstalk) front/rear pairs, 10 the
                 * I3DL2 send, 11-31 effect sends and submixes. The sends feed
                 * GP effects this stub doesn't run, and Blood Wake's voices
                 * carry their dry signal in the speaker bins, so they are
                 * left out; so is LFE, as a standard stereo fold-down does
                 * (inferred for the Xbox's EP). */
                const float c = 0.70710678f;
                left  = mixbins[0][i] + mixbins[6][i]
                      + c * (mixbins[2][i] + mixbins[4][i] + mixbins[8][i]);
                right = mixbins[1][i] + mixbins[7][i]
                      + c * (mixbins[2][i] + mixbins[5][i] + mixbins[9][i]);
            } else if (mcpx_apu_mixdown_all()) {
                left = 0.0f;
                right = 0.0f;
                for (int b = 0; b < NUM_MIXBINS; ++b) {
                    if (b & 1) right += mixbins[b][i];
                    else       left  += mixbins[b][i];
                }
            } else {
                left = mixbins[0][i];
                right = mixbins[1][i];
            }
            if (g_bw_peak)
                bw_peak_fold2(left, right);
            /* Clamp to [-1, 1] range */
            if (left > 1.0f) left = 1.0f;
            if (left < -1.0f) left = -1.0f;
            if (right > 1.0f) right = 1.0f;
            if (right < -1.0f) right = -1.0f;

            /* Convert to 16-bit and write (not accumulate) into frame buffer.
             * Each of the 8 sub-frames writes its own 32-sample slice. */
            d->monitor.frame_buf[off + i][0] = (int16_t)(left * 32767.0f);
            d->monitor.frame_buf[off + i][1] = (int16_t)(right * 32767.0f);

            if (xa2_output_channels() == 6) {
                /* Blood Wake (Remastered AUDIO_SURROUND, bw_apu_xdsp.c):
                 * the six channels from the bins, the I3DL2 send left out
                 * as in the fold-down. FL FR C LFE BL BR. */
                float ch6[6];
                int k;
                ch6[0] = mixbins[0][i] + mixbins[6][i];
                ch6[1] = mixbins[1][i] + mixbins[7][i];
                ch6[2] = mixbins[2][i];
                ch6[3] = mixbins[3][i];
                ch6[4] = mixbins[4][i] + mixbins[8][i];
                ch6[5] = mixbins[5][i] + mixbins[9][i];
                if (g_bw_peak)
                    bw_peak_fold6(ch6);
                for (k = 0; k < 6; k++) {
                    float v = ch6[k] > 1.0f ? 1.0f : ch6[k] < -1.0f ? -1.0f : ch6[k];
                    g_bw_frame6[off + i][k] = (int16_t)(v * 32767.0f);
                }
            }
        }
    }

    g_dbg.gp.cycles = 0;
    g_dbg.ep.cycles = 0;
}
