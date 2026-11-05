/*
 * MCPX APU Voice Processor - Standalone extraction from xemu
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2018-2019 Jannik Vogel
 * Copyright (c) 2019-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "apu_state.h"
#include "apu.h"
#include "fpconv.h"
#include <process.h>
#include "../../bw_latlog.h"   /* Blood Wake (lat-a): BW_LATLOG test aid (press to sound) */
#include "bw_apu_hot.h"        /* Blood Wake (au-hot): BW_APU_HOTSTAT, a test aid */
/* au-hot: what the voice being processed was, and the resampler's share */
static int s_hot_vflags, s_hot_taps;
static int64_t s_hot_vres;
#include <intrin.h>
static int bw_hot_isden(float x)        /* au-hot: a denormal (subnormal) */
{
    uint32_t u;
    memcpy(&u, &x, 4);
    return !(u & 0x7F800000u) && (u & 0x007FFFFFu);
}

/* #define DEBUG_MCPX */

#ifdef DEBUG_MCPX
#define DPRINTF(fmt, ...) fprintf(stderr, fmt, ## __VA_ARGS__)
#else
#define DPRINTF(fmt, ...) do { } while (0)
#endif

/* ============================================================
 * Voice list register table
 * ============================================================ */

static const struct {
    hwaddr top, current, next;
} voice_list_regs[] = {
    { NV_PAPU_TVL2D, NV_PAPU_CVL2D, NV_PAPU_NVL2D }, /* 2D */
    { NV_PAPU_TVL3D, NV_PAPU_CVL3D, NV_PAPU_NVL3D }, /* 3D */
    { NV_PAPU_TVLMP, NV_PAPU_CVLMP, NV_PAPU_NVLMP }, /* MP */
};

/* ============================================================
 * Notify status helper
 * ============================================================ */

static void bw_audit_notify(int notifier);   /* Blood Wake: BW_APU_AUDIT */
static void set_notify_status(MCPXAPUState *d, uint32_t v, int notifier,
                              int status)
{
    bw_audit_notify(notifier);
    hwaddr notify_offset = d->regs[NV_PAPU_FENADDR];
    notify_offset += 16 * (MCPX_HW_NOTIFIER_BASE_OFFSET +
                           v * MCPX_HW_NOTIFIER_COUNT + notifier);
    notify_offset += 15;

    stb_phys(address_space_memory, notify_offset, (uint8_t)status);
    stb_phys(address_space_memory, notify_offset - 1, 1);

    qatomic_or(&d->regs[NV_PAPU_ISTS],
               NV_PAPU_ISTS_FEVINTSTS | NV_PAPU_ISTS_FENINTSTS);
    d->set_irq = true;
}

/* ============================================================
 * Filter helpers
 * ============================================================ */

static void bw_sinc_reset(uint16_t v);   /* Blood Wake, below */

/* Blood Wake (APU audit A3): the I3DL2 filters' state of each 3D voice,
 * [direct, room]. */
static float s_bw_i3dl2[MCPX_HW_MAX_3D_VOICES][2];

static void voice_reset_filters(MCPXAPUState *d, uint16_t v)
{
    assert(v < MCPX_HW_MAX_VOICES);
    if (v < MCPX_HW_MAX_3D_VOICES)
        s_bw_i3dl2[v][0] = s_bw_i3dl2[v][1] = 0.0f;
    memset(&d->vp.filters[v].svf, 0, sizeof(d->vp.filters[v].svf));
    d->vp.filters[v].gain_valid = false;     /* a new voice starts at its level */
    hrtf_filter_clear_history(&d->vp.filters[v].hrtf);
    if (d->vp.filters[v].resampler) {
        src_reset(d->vp.filters[v].resampler);
    }
    d->vp.filters[v].rs_have = 0;
    d->vp.filters[v].rs_pos = 0.0f;
    bw_sinc_reset(v);                    /* Blood Wake: the sinc resampler's history */
}

static bool voice_should_mute(uint16_t v)
{
    bool m = (g_dbg_voice_monitor >= 0) && (v != g_dbg_voice_monitor);
    return m || mcpx_apu_debug_is_muted(v);
}

/* Blood Wake (diagnostic, APU audit A1): BW_APU_SOLO=env keeps only the
 * voices with an amplitude envelope running (and the multipass voices that
 * carry 3D sources), so an envelope artefact can be heard and measured on its
 * own; BW_APU_SOLO=voice:<handle> keeps one voice (and the multipass
 * voices). Off by default. */
static uint32_t voice_get_mask(MCPXAPUState *d, uint16_t voice_handle,
                               hwaddr offset, uint32_t mask);
static bool bw_solo_mute(MCPXAPUState *d, uint16_t v, bool multipass)
{
    static int solo = -1, solo_v = -1;
    if (solo < 0) {
        const char *e = getenv("BW_APU_SOLO");
        solo = 0;
        if (e && !strcmp(e, "env"))
            solo = 1;
        else if (e && !strncmp(e, "voice:", 6)) {
            solo = 2;
            solo_v = atoi(e + 6);
        }
        if (solo)
            fprintf(stderr, "[APU] BW_APU_SOLO=%s: only those voices are heard\n", e);
    }
    if (!solo || multipass)
        return false;
    if (solo == 2)
        return (int)v != solo_v;
    return voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                          NV_PAVS_VOICE_PAR_STATE_EACUR) == NV_PAVS_VOICE_PAR_STATE_EFCUR_OFF;
}

/* ============================================================
 * Utility functions
 * ============================================================ */

static float clampf(float v, float mn, float mx)
{
    if (v < mn) return mn;
    if (v > mx) return mx;
    return v;
}

static float attenuate(uint16_t vol)
{
    vol &= 0xFFF;
    return (vol == 0xFFF) ? 0.0f : powf(10.0f, vol / (64.0f * -20.0f));
}

/* ============================================================
 * Voice register accessors (read/write voice struct in RAM)
 * ============================================================ */

static uint32_t voice_get_mask(MCPXAPUState *d, uint16_t voice_handle,
                               hwaddr offset, uint32_t mask)
{
    hwaddr voice = d->regs[NV_PAPU_VPVADDR] + voice_handle * NV_PAVS_SIZE;
    return (ldl_le_phys(address_space_memory, voice + offset) & mask) >>
           ctz32(mask);
}

static void voice_set_mask(MCPXAPUState *d, uint16_t voice_handle,
                           hwaddr offset, uint32_t mask, uint32_t val)
{
    hwaddr voice = d->regs[NV_PAPU_VPVADDR] + voice_handle * NV_PAVS_SIZE;
    uint32_t v = ldl_le_phys(address_space_memory, voice + offset) & ~mask;
    stl_le_phys(address_space_memory, voice + offset,
                v | ((val << ctz32(mask)) & mask));
}

/* Blood Wake carried patch (lat-a, test aid; inert unless BW_LATLOG is set):
 * press to sound, stage by stage. VOICE_ON logs a V event (the voice, its
 * buffer address, the frame counter); the VP's first frame of that voice a W
 * event, and its first frame with sound in it (any sample of its bin
 * contribution above 1e-4 of full scale, -80 dBFS) a Y event, with the frame
 * (ep_frame_div, which the A events also carry, so a frame maps to its place
 * in the WAV dump) and the first sample over the mark. Read-only. */
static volatile uint8_t s_lat_vs[MCPX_HW_MAX_VOICES];   /* 1 on, 2 seen, 0 done */
static uint16_t s_lat_vn[MCPX_HW_MAX_VOICES];
#define BW_LAT_SOUND_MARK 1e-4f
#define BW_LAT_SOUND_FRAMES 375                       /* 250 ms: give up */

static void bw_lat_voice_frame(MCPXAPUState *d, uint16_t v,
                               float samples[][2], float g)
{
    int i, first = -1;
    float pk = 0.0f;
    for (i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
        float a = fabsf(samples[i][0]), b = fabsf(samples[i][1]);
        float m = (a > b ? a : b) * g;
        if (m > pk)
            pk = m;
        if (first < 0 && m > BW_LAT_SOUND_MARK)
            first = i;
    }
    if (s_lat_vs[v] == 1) {
        uint32_t ba = voice_get_mask(d, v, NV_PAVS_VOICE_CUR_PSL_START,
                                     NV_PAVS_VOICE_CUR_PSL_START_BA);
        bw_lat_ev(BW_LAT_VPMIX, bw_lat_now(), v, d->ep_frame_div, first, ba, 0, 0, pk, g);
        s_lat_vs[v] = 2;
        s_lat_vn[v] = 0;
    }
    if (first >= 0 || ++s_lat_vn[v] >= BW_LAT_SOUND_FRAMES) {
        bw_lat_ev(BW_LAT_VPSOUND, bw_lat_now(), v, d->ep_frame_div, first, s_lat_vn[v], 0, 0, pk, g);
        s_lat_vs[v] = 0;
    }
}

/* ============================================================
 * Voice off / lock
 * ============================================================ */

static __declspec(thread) int s_lat_off_by_game;   /* Blood Wake (au-orig): voice_off from the title's VOICE_OFF (its thread) */

static void voice_off(MCPXAPUState *d, uint16_t v)
{
    if (g_bw_lat_on && v < MCPX_HW_MAX_VOICES &&
        voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE, NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE)) {
        /* Blood Wake (lane au-orig, test aid): an active voice ends, where the
         * title's status reads (DirectSound's GetStatus) first see it stopped:
         * by the VP at its buffer's end (0) or by the title's VOICE_OFF (1),
         * with its buffer (BA, EBO) and the frame divider, as BW_LAT_VOICEON */
        bw_lat_ev(BW_LAT_VOICEOFF, bw_lat_now(), v, s_lat_off_by_game,
                  voice_get_mask(d, v, NV_PAVS_VOICE_CUR_PSL_START, NV_PAVS_VOICE_CUR_PSL_START_BA),
                  voice_get_mask(d, v, NV_PAVS_VOICE_PAR_NEXT, NV_PAVS_VOICE_PAR_NEXT_EBO),
                  d->ep_frame_div, 0, 0.0, 0.0);
    }
    voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                   NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE, 0);

    bool stream = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                 NV_PAVS_VOICE_CFG_FMT_DATA_TYPE) != 0;
    int notifier = MCPX_HW_NOTIFIER_SSLA_DONE;
    if (stream) {
        assert(v < MCPX_HW_MAX_VOICES);
        assert(d->vp.ssl[v].ssl_index <= 1);
        notifier += d->vp.ssl[v].ssl_index;
    }
    set_notify_status(d, v, notifier, NV1BA0_NOTIFICATION_STATUS_DONE_SUCCESS);
}

/* Per voice, as in xemu. A title's VOICE_LOCK arrives on its own thread while
 * the frame thread holds d->lock across every voice of a frame, so taking
 * d->lock here made each lock wait out the whole frame. The title only has to
 * wait while the voice processor is working on the voice it locks. */
/* au-vp (BW_APU_HOTSTAT, a test aid): the title's voice-lock waits, for the
 * hand-off's 5 s line (apu_core.c) */
volatile LONG64 g_bw_vlock_n, g_bw_vlock_spun, g_bw_vlock_us, g_bw_vlock_max;

static void voice_spin_lock(MCPXAPUState *d, uint16_t v)
{
    if (g_bw_hot) {
        LARGE_INTEGER f, a, b;
        LONG64 us, m;
        InterlockedIncrement64(&g_bw_vlock_n);
        if (!InterlockedCompareExchange(&d->vp.voice_spin[v], 1, 0))
            return;
        QueryPerformanceCounter(&a);
        while (InterlockedCompareExchange(&d->vp.voice_spin[v], 1, 0))
            SwitchToThread();
        QueryPerformanceCounter(&b);
        QueryPerformanceFrequency(&f);
        us = (b.QuadPart - a.QuadPart) * 1000000 / f.QuadPart;
        InterlockedIncrement64(&g_bw_vlock_spun);
        InterlockedAdd64(&g_bw_vlock_us, us);
        while ((m = g_bw_vlock_max) < us && InterlockedCompareExchange64(&g_bw_vlock_max, us, m) != m)
            ;
        return;
    }
    while (InterlockedCompareExchange(&d->vp.voice_spin[v], 1, 0))
        SwitchToThread();
}

static void voice_spin_unlock(MCPXAPUState *d, uint16_t v)
{
    InterlockedExchange(&d->vp.voice_spin[v], 0);
}

static void voice_lock(MCPXAPUState *d, uint16_t v, bool lock)
{
    assert(v < MCPX_HW_MAX_VOICES);
    voice_spin_lock(d, v);
    d->vp.voice_locked[v] = lock;
    voice_spin_unlock(d, v);
    qemu_cond_signal(&d->cond);
}

static bool is_voice_locked(MCPXAPUState *d, uint16_t v)
{
    assert(v < MCPX_HW_MAX_VOICES);
    return d->vp.voice_locked[v] != 0;
}

/* ============================================================
 * HRIR coefficient setter
 * ============================================================ */

/* TK-23 (Blood Wake carried patch): the HRIR taps DirectSound writes are
 * sign-magnitude bytes (bit 7 the sign, bits 0-6 the size), not two's
 * complement. Read as two's complement, every small negative tap became a
 * large one (0x81, -1, read as -127): the filters inverted polarity and the
 * ears came out near anti-phase (TK-18). Inferred from the title's own
 * filters: as sign-magnitude 21 of 24 are smoother, and both ears' DC gain
 * is positive (ANALYSIS, "TK-23"). BW_APU_HRIR_FORMAT=twos reverts. */
static float hrir_tap_to_float(int8_t value)
{
    static int twos = -1;
    uint8_t b = (uint8_t)value;
    if (twos < 0) {
        const char *e = getenv("BW_APU_HRIR_FORMAT");
        twos = e && e[0] == 't';
    }
    if (twos)
        return int8_to_float(value);
    return (b & 0x80 ? -(float)(b & 0x7F) : (float)b) / 128.0f;
}

static void set_hrir_coeff_tar(MCPXAPUState *d, int channel, int coeff_idx,
                               int8_t value)
{
    int entry = d->vp.hrtf.current_entry;
    d->vp.hrtf.entries[entry].hrir[channel][coeff_idx] = hrir_tap_to_float(value);
}

/* Blood Wake carried patch (APU audit A2, docs/day1/D2.md part 4): the
 * toolkit (xemu's hrtf.h) scales each ear's filter to unity gain on its own
 * (sum of |taps| = 1). A hardware FIR has no such stage, and doing it per ear
 * throws away the level difference between the ears that the title's filters
 * carry (seen, BW_APU_AUDIT: up to 6 dB, the louder ear always the leading
 * one by the ITD). Both ears now share one fixed scale, so the filters keep
 * their interaural and direction-dependent level. The absolute scale of the
 * taps on the console is unknown; the default keeps the toolkit's average
 * 3D level, measured on the game's own 3D sounds (BW_APU_HRIR_CAL, battle:
 * the per-ear scaled ears pass them at -3.7/-4.2 dB), so the 3D/2D balance
 * does not move on average. BW_APU_HRIR_NORM=ear restores the per-ear
 * scaling; BW_APU_HRIR_SCALE=<x> sets the fixed scale (1 = taps as s.7). */
#define BW_HRIR_SCALE_DEFAULT 0.188f   /* BW_APU_HRIR_CAL on the game's 3D sounds: same level */
/* APU audit A3: the I3DL2 send at the same average level the toolkit's
 * per-ear-scaled first ear gave it on the game's 3D sounds (-3.7 dB,
 * BW_APU_HRIR_CAL). */
#define BW_I3DL2_SEND_DEFAULT 0.652f

/* Calibration (diagnostic): BW_APU_HRIR_CAL=1 runs the toolkit's per-ear
 * scaled filter beside the real one on the same 3D voices and sums the
 * energies of both outputs and of the input, so the fixed scales can be set
 * from the game's own sounds (the audit file prints them). */
static int s_bw_cal = -1;
static HrtfFilter s_bw_cal_f[MCPX_HW_MAX_3D_VOICES];
static double s_bw_cal_tk[2], s_bw_cal_fx, s_bw_cal_in;
static float s_bw_cal_scale;
static int bw_cal_on(void)
{
    if (s_bw_cal < 0) {
        const char *e = getenv("BW_APU_HRIR_CAL");
        s_bw_cal = e && e[0] == '1';
    }
    return s_bw_cal;
}

static void bw_hrtf_set_target(int v, HrtfFilter *f, float hrir[2][HRTF_NUM_TAPS], float itd)
{
    static int mode = -1;
    static float scale = BW_HRIR_SCALE_DEFAULT;
    int ch, k;
    if (bw_cal_on() && v >= 0 && v < MCPX_HW_MAX_3D_VOICES)
        hrtf_filter_set_target_params(&s_bw_cal_f[v], hrir, itd);
    if (mode < 0) {
        const char *e = getenv("BW_APU_HRIR_NORM");
        const char *s = getenv("BW_APU_HRIR_SCALE");
        mode = !(e && e[0] == 'e');
        if (s && *s && atof(s) > 0.0)
            scale = (float)atof(s);
        s_bw_cal_scale = mode ? scale : 0.0f;
        fprintf(stderr, "[APU] BW HRTF filters: %s (BW_APU_HRIR_NORM)\n",
                mode ? "one fixed scale for both ears (APU audit A2)"
                     : "each ear scaled to unity on its own (the toolkit's)");
    }
    if (!mode) {
        hrtf_filter_set_target_params(f, hrir, itd);
        return;
    }
    f->itd_tar = fmaxf(-(float)HRTF_MAX_DELAY_SAMPLES,
                       fminf(itd, (float)HRTF_MAX_DELAY_SAMPLES));
    for (ch = 0; ch < 2; ch++)
        for (k = 0; k < HRTF_NUM_TAPS; k++)
            f->ch[ch].hrir_coeff_tar[k] = hrir[ch][k] * scale;
}

/* ============================================================
 * Front-End method dispatch
 * ============================================================ */

/* Blood Wake carried patch (TK-17, docs/TOOLKIT_ISSUES.md): the title posts a
 * buffer page (INBUF SGE) or a stream segment (SSL) right after translating
 * it with MmGetPhysicalAddress. Where the heap and the contiguous window share
 * a physical number, the kernel's record of that translation is the only
 * place that knows which copy was meant, so the posted number is tagged with
 * it: bit 29 = the window, bit 28 = RAM. mcpx_apu_phys honours the tag before
 * its own rules (apu_core.c). Physical addresses stay below 64 MB, and
 * mcpx_apu_phys already drops bits 28-31, so those bits are free (bit 31 is
 * avoided: a title may hand over a 0x80000000-based address). BW_APU_POST_INTENT=0 turns the tagging off. The original
 * file is xboxrecomp/src/apu/apu_vp.c (sha256 guarded in CMakeLists.txt). */
extern const volatile uint8_t *bw_PhysLastTranslationMap(void);

static uint32_t bw_tag_intent(uint32_t phys)
{
    static int on = -1;
    static unsigned window_tags, ram_tags;
    uint32_t a = phys & 0x0FFFFFFFu;
    uint8_t kind;
    if (on < 0) {
        const char *e = getenv("BW_APU_POST_INTENT");
        on = !(e && e[0] == '0');
        fprintf(stderr, "[APU] BW posted-address intent tags: %s (TK-17)\n",
                on ? "on" : "off");
    }
    if (!on || a >= (64u << 20))
        return phys;
    kind = bw_PhysLastTranslationMap()[a >> 12];
    if (kind == 2) {
        if (++window_tags <= 3 || window_tags % 5000 == 0)
            fprintf(stderr, "[APU] BW posted %08X as a window page (%u so far)\n",
                    a, window_tags);
        return a | 0x20000000u;
    }
    if (kind == 1) {
        if (++ram_tags <= 3 || ram_tags % 5000 == 0)
            fprintf(stderr, "[APU] BW posted %08X as a RAM page (%u so far)\n",
                    a, ram_tags);
        return a | 0x10000000u;
    }
    return phys;
}

/* Diagnostic (Blood Wake carried patch): BW_APU_MPSTAT=1 prints, once per
 * second of APU time, what the multipass (submix) readers saw -- reads,
 * clears, reads that found the bin empty, the bin's peak -- and how many
 * samples the per-voice filter clamp at +-1.0 cut, per voice. */
static int s_bw_mpstat = -1;
static unsigned s_bw_mp_reads, s_bw_mp_clears, s_bw_mp_empty, s_bw_mp_frames;
static float s_bw_mp_peak;
static unsigned s_bw_clamped[MCPX_HW_MAX_VOICES];
static uint16_t s_bw_prev_vol[MCPX_HW_MAX_VOICES][8];
static unsigned s_bw_vol_changes[MCPX_HW_MAX_VOICES];
static float s_bw_vol_maxstep[MCPX_HW_MAX_VOICES];   /* dB */
static float s_bw_prev_rate[MCPX_HW_MAX_VOICES];
static unsigned s_bw_pitch_changes[MCPX_HW_MAX_VOICES];
static float s_bw_pitch_maxstep[MCPX_HW_MAX_VOICES]; /* semitones */
static uint8_t s_bw_mp_lpf[MCPX_HW_MAX_VOICES];

static int bw_mpstat_on(void)
{
    if (s_bw_mpstat < 0) {
        const char *e = getenv("BW_APU_MPSTAT");
        s_bw_mpstat = e && *e && *e != '0';
    }
    return s_bw_mpstat;
}

static void bw_mpstat_frame(void)
{
    if (!bw_mpstat_on() || ++s_bw_mp_frames < 1500)
        return;
    {
        char line[1024];
        int n = snprintf(line, sizeof(line),
                         "[APU-MP] reads=%u clears=%u empty=%u peak=%.2f clamped:",
                         s_bw_mp_reads, s_bw_mp_clears, s_bw_mp_empty, s_bw_mp_peak);
        for (int v = 0; v < MCPX_HW_MAX_VOICES && n < (int)sizeof(line) - 24; v++)
            if (s_bw_clamped[v])
                n += snprintf(line + n, sizeof(line) - n, " v%d=%u%s", v,
                              s_bw_clamped[v], s_bw_mp_lpf[v] ? "(mp)" : "");
        fprintf(stderr, "%s\n", line);
        n = snprintf(line, sizeof(line), "[APU-VOL] changes/s, max step dB:");
        for (int v = 0; v < MCPX_HW_MAX_VOICES && n < (int)sizeof(line) - 24; v++)
            if (s_bw_vol_changes[v])
                n += snprintf(line + n, sizeof(line) - n, " v%d=%u/%.1f", v,
                              s_bw_vol_changes[v], s_bw_vol_maxstep[v]);
        fprintf(stderr, "%s\n", line);
    }
    if (bw_mpstat_on()) {
        char line[1024];
        int n = snprintf(line, sizeof(line), "[APU-PITCH] changes/s, max step semitones:");
        for (int v = 0; v < MCPX_HW_MAX_VOICES && n < (int)sizeof(line) - 24; v++)
            if (s_bw_pitch_changes[v])
                n += snprintf(line + n, sizeof(line) - n, " v%d=%u/%.2f", v,
                              s_bw_pitch_changes[v], s_bw_pitch_maxstep[v]);
        fprintf(stderr, "%s\n", line);
    }
    memset(s_bw_pitch_changes, 0, sizeof(s_bw_pitch_changes));
    memset(s_bw_pitch_maxstep, 0, sizeof(s_bw_pitch_maxstep));
    memset(s_bw_vol_changes, 0, sizeof(s_bw_vol_changes));
    memset(s_bw_vol_maxstep, 0, sizeof(s_bw_vol_maxstep));
    s_bw_mp_reads = s_bw_mp_clears = s_bw_mp_empty = s_bw_mp_frames = 0;
    s_bw_mp_peak = 0.0f;
    memset(s_bw_clamped, 0, sizeof(s_bw_clamped));
    memset(s_bw_mp_lpf, 0, sizeof(s_bw_mp_lpf));
}

/* D3 diagnostics (2026-10-27, the voice-stop hang): list-changing front-end
 * methods that run while the front end is trapped (TK-20: the console would
 * hold them until the trap is cleared), and a VOICE_ON whose voice is
 * already reachable in the list it is put on (a double insert). [BWD3] lines,
 * at most 60. BW_D3_DIAG=0 silences. */
static int bwd3_apu_diag(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("BW_D3_DIAG");
        on = !(e && e[0] == '0');
    }
    return on;
}

static int bwd3_in_list(MCPXAPUState *d, unsigned top_reg, uint32_t h)
{
    uint32_t v = d->regs[top_reg] & 0xFFFF;
    int k;
    for (k = 0; v != 0xFFFF && k < MCPX_HW_MAX_VOICES; k++) {
        if (v == h)
            return 1;
        v = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_TAR_PITCH_LINK,
                           NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE);
    }
    return 0;
}

/* A ring of the last list-relevant events (FE methods, list-register and
 * FECTL/ISTS writes, idle traps), with the host thread, printed when the VP
 * finds a voice that is active but on no list it walks (an orphan: its stop
 * will wait forever for an idle report). */
#define BWD3_RING 256
static struct { uint32_t seq, what, arg, tid, fectl, par; } s_bwd3_ring[BWD3_RING];
static volatile LONG s_bwd3_seq;

void bwd3_apu_event(MCPXAPUState *d, uint32_t what, uint32_t arg)
{
    LONG n;
    if (!bwd3_apu_diag())
        return;
    n = InterlockedIncrement(&s_bwd3_seq);
    s_bwd3_ring[n % BWD3_RING].seq = (uint32_t)n;
    s_bwd3_ring[n % BWD3_RING].what = what;
    s_bwd3_ring[n % BWD3_RING].arg = arg;
    s_bwd3_ring[n % BWD3_RING].tid = GetCurrentThreadId();
    s_bwd3_ring[n % BWD3_RING].fectl = d->regs[NV_PAPU_FECTL];
    s_bwd3_ring[n % BWD3_RING].par = d->regs[NV_PAPU_FEDECPARAM];
}

static void bwd3_orphan_check(MCPXAPUState *d, const uint8_t *reached)
{
    static uint8_t seen_once[MCPX_HW_MAX_VOICES];
    static int reports;
    int v;
    if (!bwd3_apu_diag() || reports >= 3)
        return;
    for (v = 0; v < MCPX_HW_MAX_VOICES; v++) {
        int active = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_STATE,
                                    NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE) != 0;
        if (!active || reached[v]) {
            seen_once[v] = 0;
            continue;
        }
        if (++seen_once[v] < 3)          /* three frames in a row: not mid-edit */
            continue;
        {
            LONG end = s_bwd3_seq, k;
            reports++;
            fprintf(stderr, "[BWD3] APU: voice 0x%X is active but on no list the VP walks "
                    "(orphaned; lists 2D %04X 3D %04X MP %04X). Last events (seq what arg tid "
                    "fectl trap-param):\n", v, d->regs[voice_list_regs[0].top],
                    d->regs[voice_list_regs[1].top], d->regs[voice_list_regs[2].top]);
            for (k = end - BWD3_RING + 1; k <= end; k++) {
                int i = (int)(k % BWD3_RING);
                if (k <= 0 || s_bwd3_ring[i].seq != (uint32_t)k)
                    continue;
                fprintf(stderr, "[BWD3]   %u %05X %08X %u %08X %X\n", s_bwd3_ring[i].seq,
                        s_bwd3_ring[i].what, s_bwd3_ring[i].arg, s_bwd3_ring[i].tid,
                        s_bwd3_ring[i].fectl, s_bwd3_ring[i].par);
            }
            fflush(stderr);
            memset(seen_once, 0, sizeof seen_once);
            return;
        }
    }
}

static void bwd3_fe_note(MCPXAPUState *d, uint32_t method, uint32_t argument)
{
    static volatile LONG lines, dup_lines;
    if (method == NV1BA0_PIO_VOICE_ON || method == NV1BA0_PIO_VOICE_OFF
            || method == NV1BA0_PIO_SET_ANTECEDENT_VOICE || method == NV1BA0_PIO_VOICE_RELEASE
            || method == SE2FE_IDLE_VOICE || method == NV1BA0_PIO_SET_CURRENT_VOICE)
        bwd3_apu_event(d, method, argument);
    static unsigned long trapped_runs;
    int trapped = (d->regs[NV_PAPU_FECTL] & NV_PAPU_FECTL_FEMETHMODE)
                  == NV_PAPU_FECTL_FEMETHMODE_TRAPPED;
    if (!bwd3_apu_diag())
        return;
    if (trapped && (method == NV1BA0_PIO_VOICE_ON || method == NV1BA0_PIO_VOICE_OFF
                    || method == NV1BA0_PIO_SET_ANTECEDENT_VOICE
                    || method == NV1BA0_PIO_VOICE_RELEASE)) {
        trapped_runs++;
        if (InterlockedIncrement(&lines) <= 12)
            fprintf(stderr, "[BWD3] APU: method 0x%03X arg 0x%08X runs while the FE is "
                    "trapped (trap 0x%X param 0x%X; %lu so far)\n", method, argument,
                    d->regs[NV_PAPU_FEDECMETH], d->regs[NV_PAPU_FEDECPARAM], trapped_runs);
    }
    if (method == NV1BA0_PIO_VOICE_ON) {
        uint32_t h = argument & NV1BA0_PIO_VOICE_ON_HANDLE;
        unsigned list = GET_MASK(d->regs[NV_PAPU_FEAV], NV_PAPU_FEAV_LST);
        int l;
        for (l = 0; l < 3; l++)
            if (bwd3_in_list(d, voice_list_regs[l].top, h)
                    && InterlockedIncrement(&dup_lines) <= 40)
                fprintf(stderr, "[BWD3] APU: VOICE_ON 0x%X (list field %u, FEAV 0x%08X) but "
                        "it is already in list %d%s\n", h, list, d->regs[NV_PAPU_FEAV], l,
                        trapped ? " (FE trapped)" : "");
    }
}

/* VOICE_ON's list insert (unchanged from the toolkit's code): at the top of
 * list `list` (1-3), or after the antecedent voice when the list is INHERIT.
 * The new voice's own link is written before the store that makes it
 * reachable, so a walk that sees it also sees where it leads. */
static void voice_on_link(MCPXAPUState *d, unsigned int selected_handle,
                          unsigned int list)
{
    if (list != NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_INHERIT) {
        unsigned int top_reg = voice_list_regs[list - 1].top;
        voice_set_mask(d, (uint16_t)selected_handle,
                       NV_PAVS_VOICE_TAR_PITCH_LINK,
                       NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE,
                       d->regs[top_reg]);
        MemoryBarrier();
        d->regs[top_reg] = selected_handle;
    } else {
        unsigned int antecedent_voice =
            GET_MASK(d->regs[NV_PAPU_FEAV], NV_PAPU_FEAV_VALUE);
        assert(antecedent_voice != 0xFFFF);

        uint32_t next_handle = voice_get_mask(
            d, (uint16_t)antecedent_voice, NV_PAVS_VOICE_TAR_PITCH_LINK,
            NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE);
        voice_set_mask(d, (uint16_t)selected_handle,
                       NV_PAVS_VOICE_TAR_PITCH_LINK,
                       NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE,
                       next_handle);
        MemoryBarrier();
        voice_set_mask(d, (uint16_t)antecedent_voice,
                       NV_PAVS_VOICE_TAR_PITCH_LINK,
                       NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE,
                       selected_handle);
    }
}

/* Blood Wake carried patch (TK-45, second half): the frame thread updates a
 * voice's PAR_STATE (envelope stage, ACTIVE when a buffer ends) with
 * read-modify-writes inside voice_process, holding the voice's spin lock. The
 * FE methods that write PAR_STATE ran on the title's thread without it, so a
 * VOICE_OFF could be lost under the frame thread's write-back: the voice went
 * on playing, no idle report ever came, and DirectSound's stop waited (seen in
 * soak run -sk3-m06: VOICE_OFF 0x46, then no idle trap for 0x46 for 3 s while
 * the voices around it reported). VOICE_ON, VOICE_RELEASE, VOICE_OFF and
 * VOICE_PAUSE now write PAR_STATE under the same spin lock. BW_APU_FE_SPIN=0
 * restores the unlocked writes. */
static int fe_spin_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("BW_APU_FE_SPIN");
        on = !(e && e[0] == '0');
    }
    return on;
}

/* ============================================================
 * Blood Wake carried patch (diagnostic): the APU coverage audit.
 *
 * BW_APU_AUDIT=<file> counts what the title asks of the voice processor, so
 * what it uses can be compared with what this emulation implements:
 *   - every front-end method by offset, and for the voice-configuration
 *     methods the distinct values written (up to 48 each, with counts);
 *   - per voice-frame (one voice for one 32-sample frame): 2D/3D, stereo,
 *     multipass, the sample container and size, data type, loop, linked,
 *     persist, the filter mode by voice kind, the envelopes' stages, the
 *     pitch-envelope and cutoff-envelope scales, the pitch in semitones, the
 *     HRTF, the bins each slot sends to with a non-zero volume, and every
 *     voice-structure word (0x00-0x7C) that is non-zero.
 * One file per boot (<file>.<process id>), rewritten every 10 s of audio,
 * so it is whole even if the run
 * is cut short.
 * ============================================================ */
#define BW_AUD_VALS 48
typedef struct { uint32_t v; uint64_t n; } BwAudVal;
typedef struct {
    int on;
    char path[512];
    uint64_t frames;                     /* audio frames seen */
    uint64_t meth[0x800];                /* by method offset / 4 */
    BwAudVal vals[0x40][BW_AUD_VALS];    /* methods 0x300-0x3FC: distinct values */
    int nvals[0x40];
    uint64_t vf, vf3d, vfstereo, vfmulti, vfloop, vflinked, vfpersist, vfclearmix, vfadpcm;
    uint64_t vfdatatype, vfhrtf;
    uint64_t container[4], ssize[4];
    uint64_t fmode[3][4];                /* [2D mono, 2D stereo, 3D][fmode] */
    uint64_t eacur[16], efcur[16];
    uint64_t pitchscale, fcscale;        /* voice-frames with them non-zero */
    uint64_t pitchbucket[9];             /* semitones: <-24,-24..-12,-12..-3,-3..-0.5,
                                            -0.5..0.5,0.5..3,3..12,12..24,>24 */
    uint64_t binsend[32];                /* voice-frames sending to bin b */
    uint64_t word_nz[32];                /* voice words non-zero, by offset/4 */
    uint64_t headroom[8];
    /* v2 (after main's TK-45 merge) */
    BwAudVal vals2[0x40][BW_AUD_VALS];   /* methods 0x200-0x2FC: distinct values */
    int nvals2[0x40];
    uint64_t von2d, von3d, vonenv;       /* VOICE_ONs: 2D handles, 3D handles, with an EA */
    BwAudVal rates[BW_AUD_VALS];         /* source rate at VOICE_ON, Hz (10 Hz steps) */
    int nrates;
    BwAudVal envfmt[BW_AUD_VALS];        /* CFG_FMT of voices started with an attack */
    int nenvfmt;
    uint64_t hrir_n;                     /* filter entries completed (SET_HRIR_X) */
    uint64_t hrir_quad[2][3];            /* [louder ear 0/1][ITD <0, 0, >0] */
    uint64_t hrir_ild[8];                /* |ILD| dB: <1 1-3 3-6 6-10 10-15 15-20 20-30 >30 */
    uint64_t hrir_norm[2][8];            /* per ear, 20log10(1/L1): what the toolkit's
                                            unity-gain normalisation adds, dB:
                                            <-6 -6..-3 -3..-1 -1..1 1..3 3..6 6..10 >10 */
    uint64_t hrir_dcneg[2];
    uint64_t hrir_l1max[32], hrir_dcmax[32];   /* louder ear: L1 (0.5 steps), DC (0.25) */
    double hrir_eraw, hrir_enorm;        /* both ears: sum of taps^2, raw and per-ear unity L1 */
    uint64_t tarhrtf_fresh, tarhrtf_old; /* TAR_HRTF: the entry just completed, or another */
    uint32_t hrir_last;
    uint64_t notify[4];                  /* notifier writes by index */
    uint64_t decay0;                     /* voice-frames in EA decay with rate 0 */
    struct { uint64_t frame; uint32_t v, fmt, vol; } dip[64];   /* the first 64 */
    double binE[32];                     /* per-bin sum of squares, end of the VP frame */
    float binpk[32];
} BwAudit;
static BwAudit *s_aud;
static int s_aud_on = -1;

static int bw_audit_on(void)
{
    if (s_aud_on < 0) {
        const char *e = getenv("BW_APU_AUDIT");
        s_aud_on = 0;
        if (e && *e) {
            s_aud = (BwAudit *)calloc(1, sizeof(BwAudit));
            if (s_aud) {
                /* one file per boot: <path>.<process id> */
                _snprintf(s_aud->path, sizeof s_aud->path - 1, "%s.%d", e, _getpid());
                s_aud_on = 1;
                fprintf(stderr, "[APU-AUDIT] counting the title's APU use into %s\n", e);
            }
        }
    }
    return s_aud_on;
}

static void bw_aud_count(BwAudVal *t, int *n, uint32_t v)
{
    int k;
    for (k = 0; k < *n; k++)
        if (t[k].v == v) {
            t[k].n++;
            return;
        }
    if (*n < BW_AUD_VALS) {
        t[*n].v = v;
        t[*n].n = 1;
        (*n)++;
    }
}

static float hrir_tap_to_float(int8_t value);

/* SET_HRIR_X completes an entry: taps 0-29 are in the entry, tap 30 and the
 * ITD in the argument. Raw (un-normalised) per-ear energy, which ear is the
 * louder against the ITD's sign, and how much the toolkit's unity-gain
 * normalisation changes each ear. */
static void bw_audit_hrir(MCPXAPUState *d, uint32_t argument)
{
    BwAudit *a = s_aud;
    int e = d->vp.hrtf.current_entry, ch, k, louder, q;
    float l1[2] = {0, 0}, l2[2] = {0, 0}, dc[2] = {0, 0}, ild, nd;
    int16_t itd = (int16_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_X_ITD);
    if (e < 0 || e >= HRTF_ENTRY_COUNT)
        return;
    for (ch = 0; ch < 2; ch++) {
        for (k = 0; k < HRTF_NUM_TAPS; k++) {
            float c = k < 30 ? d->vp.hrtf.entries[e].hrir[ch][k]
                    : hrir_tap_to_float((int8_t)GET_MASK(argument, ch ? NV1BA0_PIO_SET_HRIR_X_RIGHT30
                                                                      : NV1BA0_PIO_SET_HRIR_X_LEFT30));
            l1[ch] += fabsf(c);
            l2[ch] += c * c;
            dc[ch] += c;
        }
        if (dc[ch] < 0) a->hrir_dcneg[ch]++;
        nd = l1[ch] > 0 ? -20.0f * log10f(l1[ch]) : 0.0f;
        q = nd < -6 ? 0 : nd < -3 ? 1 : nd < -1 ? 2 : nd <= 1 ? 3 : nd <= 3 ? 4 : nd <= 6 ? 5
          : nd <= 10 ? 6 : 7;
        a->hrir_norm[ch][q]++;
    }
    a->hrir_n++;
    for (ch = 0; ch < 2; ch++) {
        a->hrir_eraw += l2[ch];
        if (l1[ch] > 0) a->hrir_enorm += l2[ch] / (l1[ch] * l1[ch]);
    }
    louder = l2[1] > l2[0];
    a->hrir_quad[louder][itd < 0 ? 0 : itd == 0 ? 1 : 2]++;
    ild = (l2[0] > 0 && l2[1] > 0) ? fabsf(10.0f * log10f(l2[0] / l2[1])) : 99.0f;
    q = ild < 1 ? 0 : ild < 3 ? 1 : ild < 6 ? 2 : ild < 10 ? 3 : ild < 15 ? 4 : ild < 20 ? 5
      : ild < 30 ? 6 : 7;
    a->hrir_ild[q]++;
    a->hrir_last = (uint32_t)e;
    k = l2[1] > l2[0];
    q = (int)(l1[k] * 2.0f); a->hrir_l1max[q < 0 ? 0 : q > 31 ? 31 : q]++;
    q = (int)(dc[k] * 4.0f); a->hrir_dcmax[q < 0 ? 0 : q > 31 ? 31 : q]++;
}

static void bw_audit_method(MCPXAPUState *d, uint32_t method, uint32_t argument)
{
    if (!bw_audit_on())
        return;
    if (method < 0x2000)
        s_aud->meth[method >> 2]++;
    if (method >= 0x300 && method < 0x400) {
        int m = (method - 0x300) >> 2;
        bw_aud_count(s_aud->vals[m], &s_aud->nvals[m], argument);
    } else if (method >= 0x200 && method < 0x300) {
        int m = (method - 0x200) >> 2;
        bw_aud_count(s_aud->vals2[m], &s_aud->nvals2[m], argument);
    }
    if (method == NV1BA0_PIO_SET_HRIR_X)
        bw_audit_hrir(d, argument);
    if (method == NV1BA0_PIO_SET_VOICE_TAR_HRTF) {
        uint32_t h = argument & NV1BA0_PIO_SET_VOICE_TAR_HRTF_HANDLE;
        if (h != HRTF_NULL_HANDLE) {
            if (h == s_aud->hrir_last) s_aud->tarhrtf_fresh++;
            else s_aud->tarhrtf_old++;
        }
    }
    if (method == NV1BA0_PIO_VOICE_ON) {
        uint16_t v = (uint16_t)(argument & NV1BA0_PIO_VOICE_ON_HANDLE);
        uint32_t fmt;
        if (v >= MCPX_HW_MAX_VOICES)
            return;
        fmt = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT, 0xFFFFFFFF);
        if (v < MCPX_HW_MAX_3D_VOICES) s_aud->von3d++;
        else s_aud->von2d++;
        if (!(fmt & NV_PAVS_VOICE_CFG_FMT_MULTIPASS)) {
            int16_t p = (int16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_PITCH_LINK,
                                                 NV_PAVS_VOICE_TAR_PITCH_LINK_PITCH);
            float hz = 48000.0f * powf(2.0f, p / 4096.0f);
            bw_aud_count(s_aud->rates, &s_aud->nrates, (uint32_t)(hz / 10.0f + 0.5f) * 10);
        }
        if (voice_get_mask(d, v, NV_PAVS_VOICE_CFG_ENV0, NV_PAVS_VOICE_CFG_ENV0_EA_ATTACKRATE)) {
            s_aud->vonenv++;
            bw_aud_count(s_aud->envfmt, &s_aud->nenvfmt, fmt);
        }
    }
}

static void bw_audit_notify(int notifier)
{
    if (bw_audit_on() && notifier >= 0 && notifier < 4)
        s_aud->notify[notifier]++;
}

static void bw_audit_bins(float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    int b, i;
    if (!bw_audit_on())
        return;
    for (b = 0; b < 32 && b < NUM_MIXBINS; b++) {
        double e = 0;
        float pk = s_aud->binpk[b];
        for (i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            float x = mixbins[b][i];
            e += (double)x * x;
            if (fabsf(x) > pk) pk = fabsf(x);
        }
        s_aud->binE[b] += e;
        s_aud->binpk[b] = pk;
    }
}

static void bw_audit_voice(MCPXAPUState *d, uint16_t v, int stereo, int multipass,
                           int fmode, float rate, const int bin[8], const uint16_t vol[8])
{
    uint32_t fmt, w;
    int k, kind;
    float st;
    if (!bw_audit_on())
        return;
    s_aud->vf++;
    fmt = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT, 0xFFFFFFFF);
    if (v < MCPX_HW_MAX_3D_VOICES) s_aud->vf3d++;
    if (stereo) s_aud->vfstereo++;
    if (multipass) s_aud->vfmulti++;
    if (fmt & NV_PAVS_VOICE_CFG_FMT_LOOP) s_aud->vfloop++;
    if (fmt & NV_PAVS_VOICE_CFG_FMT_LINKED) s_aud->vflinked++;
    if (fmt & NV_PAVS_VOICE_CFG_FMT_PERSIST) s_aud->vfpersist++;
    if (fmt & NV_PAVS_VOICE_CFG_FMT_CLEAR_MIX) s_aud->vfclearmix++;
    if (fmt & NV_PAVS_VOICE_CFG_FMT_DATA_TYPE) s_aud->vfdatatype++;
    s_aud->container[(fmt >> 30) & 3]++;
    s_aud->ssize[(fmt >> 28) & 3]++;
    s_aud->headroom[(fmt >> 13) & 7]++;
    kind = v < MCPX_HW_MAX_3D_VOICES ? 2 : stereo ? 1 : 0;
    s_aud->fmode[kind][fmode & 3]++;
    w = voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE, 0xFFFFFFFF);
    s_aud->efcur[(w >> 24) & 0xF]++;
    s_aud->eacur[(w >> 28) & 0xF]++;
    if (((w >> 28) & 0xF) == NV_PAVS_VOICE_PAR_STATE_EFCUR_DECAY &&
        !voice_get_mask(d, v, NV_PAVS_VOICE_CFG_ENVA, NV_PAVS_VOICE_CFG_ENVA_EA_DECAYRATE))
    {
        if (s_aud->decay0 < 64) {
            s_aud->dip[s_aud->decay0].frame = s_aud->frames;
            s_aud->dip[s_aud->decay0].v = v;
            s_aud->dip[s_aud->decay0].fmt = fmt;
            s_aud->dip[s_aud->decay0].vol = (uint32_t)vol[0] | ((uint32_t)vol[2] << 16);
        }
        s_aud->decay0++;
    }
    if (voice_get_mask(d, v, NV_PAVS_VOICE_CFG_ENV0, NV_PAVS_VOICE_CFG_ENV0_EF_PITCHSCALE))
        s_aud->pitchscale++;
    if (voice_get_mask(d, v, NV_PAVS_VOICE_CFG_ENV1, NV_PAVS_VOICE_CFG_ENV1_EF_FCSCALE))
        s_aud->fcscale++;
    if (v < MCPX_HW_MAX_3D_VOICES &&
        voice_get_mask(d, v, NV_PAVS_VOICE_CFG_HRTF_TARGET,
                       NV_PAVS_VOICE_CFG_HRTF_TARGET_HANDLE) != HRTF_NULL_HANDLE)
        s_aud->vfhrtf++;
    st = rate > 0.0f ? -12.0f * log2f(rate) : 0.0f;     /* + = pitched up */
    k = st < -24 ? 0 : st < -12 ? 1 : st < -3 ? 2 : st < -0.5f ? 3 : st <= 0.5f ? 4
      : st <= 3 ? 5 : st <= 12 ? 6 : st <= 24 ? 7 : 8;
    s_aud->pitchbucket[k]++;
    for (k = 0; k < 8; k++)
        if (vol[k] != 0xFFF && bin[k] >= 0 && bin[k] < 32)
            s_aud->binsend[bin[k]]++;
    for (k = 0; k < 32; k++)
        if (voice_get_mask(d, v, (uint32_t)k * 4, 0xFFFFFFFF))
            s_aud->word_nz[k]++;
}

static void bw_audit_frame(void)
{
    FILE *f;
    int k, m;
    BwAudit *a;
    if (!bw_audit_on())
        return;
    a = s_aud;
    if (++a->frames % (1500 * 10))
        return;
    f = fopen(a->path, "w");
    if (!f)
        return;
    fprintf(f, "# APU coverage audit (BW_APU_AUDIT), %.0f s of audio\n", a->frames / 1500.0);
    fprintf(f, "voice-frames %llu: 3D %llu, stereo %llu, multipass %llu, loop %llu, linked %llu,"
               " persist %llu, clear-mix %llu, data-type bit %llu, HRTF %llu, pitch-env scale %llu,"
               " cutoff-env scale %llu\n",
            (unsigned long long)a->vf, (unsigned long long)a->vf3d, (unsigned long long)a->vfstereo,
            (unsigned long long)a->vfmulti, (unsigned long long)a->vfloop,
            (unsigned long long)a->vflinked, (unsigned long long)a->vfpersist,
            (unsigned long long)a->vfclearmix, (unsigned long long)a->vfdatatype,
            (unsigned long long)a->vfhrtf, (unsigned long long)a->pitchscale,
            (unsigned long long)a->fcscale);
    fprintf(f, "container (B8, B16, ADPCM, B32): %llu %llu %llu %llu\n",
            (unsigned long long)a->container[0], (unsigned long long)a->container[1],
            (unsigned long long)a->container[2], (unsigned long long)a->container[3]);
    fprintf(f, "sample size (U8, S16, S24, S32): %llu %llu %llu %llu\n",
            (unsigned long long)a->ssize[0], (unsigned long long)a->ssize[1],
            (unsigned long long)a->ssize[2], (unsigned long long)a->ssize[3]);
    for (k = 0; k < 3; k++)
        fprintf(f, "filter mode, %s: 0:%llu 1:%llu 2:%llu 3:%llu\n",
                k == 0 ? "2D mono" : k == 1 ? "2D stereo" : "3D",
                (unsigned long long)a->fmode[k][0], (unsigned long long)a->fmode[k][1],
                (unsigned long long)a->fmode[k][2], (unsigned long long)a->fmode[k][3]);
    fprintf(f, "EA stage (off delay attack hold decay sustain release force):");
    for (k = 0; k < 8; k++)
        fprintf(f, " %llu", (unsigned long long)a->eacur[k]);
    fprintf(f, "\nEF stage:");
    for (k = 0; k < 8; k++)
        fprintf(f, " %llu", (unsigned long long)a->efcur[k]);
    fprintf(f, "\npitch, semitones (<-24 -24..-12 -12..-3 -3..-.5 +-.5 .5..3 3..12 12..24 >24):");
    for (k = 0; k < 9; k++)
        fprintf(f, " %llu", (unsigned long long)a->pitchbucket[k]);
    fprintf(f, "\nheadroom 0-7:");
    for (k = 0; k < 8; k++)
        fprintf(f, " %llu", (unsigned long long)a->headroom[k]);
    fprintf(f, "\nbins sent to (voice-frames):");
    for (k = 0; k < 32; k++)
        if (a->binsend[k])
            fprintf(f, " %d:%llu", k, (unsigned long long)a->binsend[k]);
    fprintf(f, "\nvoice words non-zero (offset:voice-frames):");
    for (k = 0; k < 32; k++)
        if (a->word_nz[k])
            fprintf(f, " %02X:%llu", k * 4, (unsigned long long)a->word_nz[k]);
    fprintf(f, "\nmethods (offset:count):");
    for (k = 0; k < 0x800; k++)
        if (a->meth[k])
            fprintf(f, " %03X:%llu", k * 4, (unsigned long long)a->meth[k]);
    fprintf(f, "\n");
    for (m = 0; m < 0x40; m++) {
        if (!a->nvals[m])
            continue;
        fprintf(f, "method %03X values (%d%s):", 0x300 + m * 4, a->nvals[m],
                a->nvals[m] == BW_AUD_VALS ? "+" : "");
        for (k = 0; k < a->nvals[m]; k++)
            fprintf(f, " %08X:%llu", a->vals[m][k].v, (unsigned long long)a->vals[m][k].n);
        fprintf(f, "\n");
    }
    for (m = 0; m < 0x40; m++) {
        if (!a->nvals2[m])
            continue;
        fprintf(f, "method %03X values (%d%s):", 0x200 + m * 4, a->nvals2[m],
                a->nvals2[m] == BW_AUD_VALS ? "+" : "");
        for (k = 0; k < a->nvals2[m]; k++)
            fprintf(f, " %08X:%llu", a->vals2[m][k].v, (unsigned long long)a->vals2[m][k].n);
        fprintf(f, "\n");
    }
    fprintf(f, "voice-ons: 2D handles %llu, 3D handles %llu, with an attack %llu\n",
            (unsigned long long)a->von2d, (unsigned long long)a->von3d,
            (unsigned long long)a->vonenv);
    fprintf(f, "source rate at voice-on (Hz:count):");
    for (k = 0; k < a->nrates; k++)
        fprintf(f, " %u:%llu", a->rates[k].v, (unsigned long long)a->rates[k].n);
    fprintf(f, "\nCFG_FMT of voices with an attack:");
    for (k = 0; k < a->nenvfmt; k++)
        fprintf(f, " %08X:%llu", a->envfmt[k].v, (unsigned long long)a->envfmt[k].n);
    fprintf(f, "\nEA decay frames with rate 0: %llu\n", (unsigned long long)a->decay0);
    fprintf(f, "EA decay-0 events (VP frame before the dip:voice:CFG_FMT:vol0/vol2):");
    for (k = 0; k < 64 && (uint64_t)k < a->decay0; k++)
        fprintf(f, " %llu:%u:%08X:%03X/%03X", (unsigned long long)a->dip[k].frame, a->dip[k].v,
                a->dip[k].fmt, a->dip[k].vol & 0xFFF, (a->dip[k].vol >> 16) & 0xFFF);
    fprintf(f, "\n");
    fprintf(f, "HRIR entries %llu: louder ear 0 (ITD<0 =0 >0) %llu %llu %llu, louder ear 1 %llu %llu %llu;"
               " DC<0 ear0 %llu ear1 %llu; TAR_HRTF fresh %llu other %llu\n",
            (unsigned long long)a->hrir_n,
            (unsigned long long)a->hrir_quad[0][0], (unsigned long long)a->hrir_quad[0][1],
            (unsigned long long)a->hrir_quad[0][2], (unsigned long long)a->hrir_quad[1][0],
            (unsigned long long)a->hrir_quad[1][1], (unsigned long long)a->hrir_quad[1][2],
            (unsigned long long)a->hrir_dcneg[0], (unsigned long long)a->hrir_dcneg[1],
            (unsigned long long)a->tarhrtf_fresh, (unsigned long long)a->tarhrtf_old);
    fprintf(f, "HRIR |ILD| dB (<1 1-3 3-6 6-10 10-15 15-20 20-30 >30):");
    for (k = 0; k < 8; k++)
        fprintf(f, " %llu", (unsigned long long)a->hrir_ild[k]);
    if (a->hrir_eraw > 0)
        fprintf(f, "\nHRIR energy raw %.6g, per-ear unity %.6g: the fixed scale with the same mean is %.4f",
                a->hrir_eraw, a->hrir_enorm, sqrt(a->hrir_enorm / a->hrir_eraw));
    if (s_bw_cal > 0 && s_bw_cal_fx > 0 && s_bw_cal_in > 0 && s_bw_cal_scale > 0)
        fprintf(f, "\nHRIR calibration on the game's 3D sounds (BW_APU_HRIR_CAL): per-ear-scaled ears"
                   " %.1f/%.1f dB, fixed scale %.4f gives %.1f dB (input 0 dB): the fixed scale for the"
                   " same level is %.4f, the send scale for the old first-ear level %.4f",
                10 * log10(s_bw_cal_tk[0] / s_bw_cal_in), 10 * log10(s_bw_cal_tk[1] / s_bw_cal_in),
                s_bw_cal_scale, 10 * log10(s_bw_cal_fx / s_bw_cal_in),
                s_bw_cal_scale * sqrt(0.5 * (s_bw_cal_tk[0] + s_bw_cal_tk[1]) / s_bw_cal_fx),
                sqrt(s_bw_cal_tk[0] / s_bw_cal_in));
    fprintf(f, "\nHRIR louder ear L1 (0.5 steps from 0):");
    for (k = 0; k < 32; k++)
        fprintf(f, " %llu", (unsigned long long)a->hrir_l1max[k]);
    fprintf(f, "\nHRIR louder ear DC (0.25 steps from 0):");
    for (k = 0; k < 32; k++)
        fprintf(f, " %llu", (unsigned long long)a->hrir_dcmax[k]);
    for (m = 0; m < 2; m++) {
        fprintf(f, "\nHRIR ear %d normalisation gain dB (<-6 -6..-3 -3..-1 +-1 1..3 3..6 6..10 >10):", m);
        for (k = 0; k < 8; k++)
            fprintf(f, " %llu", (unsigned long long)a->hrir_norm[m][k]);
    }
    fprintf(f, "\nnotifier writes (SSLA SSLB 2 3): %llu %llu %llu %llu\n",
            (unsigned long long)a->notify[0], (unsigned long long)a->notify[1],
            (unsigned long long)a->notify[2], (unsigned long long)a->notify[3]);
    fprintf(f, "bin level at the VP's output (bin:rms dBFS/peak dBFS):");
    for (k = 0; k < 32; k++)
        if (a->binE[k] > 0)
            fprintf(f, " %d:%.1f/%.1f", k,
                    10.0 * log10(a->binE[k] / ((double)a->frames * NUM_SAMPLES_PER_FRAME)),
                    20.0 * log10(a->binpk[k] > 0 ? a->binpk[k] : 1e-9));
    fprintf(f, "\n");
    fclose(f);
}

static void fe_method(MCPXAPUState *d, uint32_t method, uint32_t argument)
{
    unsigned int slot;

    bwd3_fe_note(d, method, argument);
    bw_audit_method(d, method, argument);   /* Blood Wake: BW_APU_AUDIT */

    /* Blood Wake carried patch (TK-20): while the front end is trapped, the
     * decode registers belong to the trap until the title's handler has read
     * them and freed the front end; on the console a trapped front end does
     * not consume further methods. Overwritten by the next PIO method, the
     * handler read a method/parameter that was not the trap's -- a voice
     * handle of 0x26A721 -- and faulted (Battle Mode on the GPU path with the
     * dispatch gate, where the handler runs at the raising thread's next
     * safe point). The methods themselves still run at once, as before. */
    if ((d->regs[NV_PAPU_FECTL] & NV_PAPU_FECTL_FEMETHMODE)
            != NV_PAPU_FECTL_FEMETHMODE_TRAPPED) {
        d->regs[NV_PAPU_FEDECMETH] = method;
        d->regs[NV_PAPU_FEDECPARAM] = argument;
    }
    unsigned int selected_handle, list;

    switch (method) {
    case NV1BA0_PIO_VOICE_LOCK:
        voice_lock(d, (uint16_t)d->regs[NV_PAPU_FECV], argument & 1);
        break;

    case NV1BA0_PIO_SET_ANTECEDENT_VOICE:
        d->regs[NV_PAPU_FEAV] = argument;
        break;

    case NV1BA0_PIO_VOICE_ON: {
        selected_handle = argument & NV1BA0_PIO_VOICE_ON_HANDLE;

        bool locked = is_voice_locked(d, (uint16_t)selected_handle);
        if (!locked) voice_lock(d, (uint16_t)selected_handle, true);

        /* Blood Wake carried patch (TK-45, docs/TOOLKIT_ISSUES.md): this runs
         * on the title's thread, beside the frame thread's walk of the voice
         * lists, with nothing in between (in xemu both run under the BQL). It
         * used to link the voice in first and set it ACTIVE last, so a walk in
         * between found a listed, inactive voice and raised SE2FE_IDLE_VOICE
         * for a voice that had just been started. DirectSound's handler then
         * unlinked it: a playing voice on no list, never walked again, and
         * whose stop waited forever for its idle report (the mission-5
         * combat freeze, run -D3hH's event ring: VOICE_ON 0x44, then an idle
         * trap for 0x44 from the frame thread). The voice is now set up and
         * made ACTIVE first and linked last. BW_APU_VOICEON_ORDER=toolkit
         * restores the old order. */
        static int link_first = -1;
        if (link_first < 0) {
            const char *e = getenv("BW_APU_VOICEON_ORDER");
            link_first = e && !strcmp(e, "toolkit");
        }

        list = GET_MASK(d->regs[NV_PAPU_FEAV], NV_PAPU_FEAV_LST);
        if (link_first)
            voice_on_link(d, selected_handle, list);

        if (fe_spin_on())
            voice_spin_lock(d, (uint16_t)selected_handle);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_OFFSET,
                       NV_PAVS_VOICE_PAR_OFFSET_CBO, 0);
        d->vp.ssl[selected_handle].ssl_seg = 0;
        d->vp.ssl[selected_handle].ssl_index = 0;

        unsigned int ea_start = GET_MASK(argument, NV1BA0_PIO_VOICE_ON_ENVA);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_EACUR, ea_start);
        if (ea_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_DELAY) {
            uint16_t delay_time =
                (uint16_t)voice_get_mask(d, (uint16_t)selected_handle,
                    NV_PAVS_VOICE_CFG_ENV0, NV_PAVS_VOICE_CFG_ENV0_EA_DELAYTIME);
            voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EACOUNT, delay_time * 16);
        } else if (ea_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_ATTACK) {
            voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EACOUNT, 0);
        } else if (ea_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_HOLD) {
            uint16_t hold_time =
                (uint16_t)voice_get_mask(d, (uint16_t)selected_handle,
                    NV_PAVS_VOICE_CFG_ENVA, NV_PAVS_VOICE_CFG_ENVA_EA_HOLDTIME);
            voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EACOUNT, hold_time * 16);
        }

        unsigned int ef_start = GET_MASK(argument, NV1BA0_PIO_VOICE_ON_ENVF);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_EFCUR, ef_start);
        if (ef_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_DELAY) {
            uint16_t delay_time =
                (uint16_t)voice_get_mask(d, (uint16_t)selected_handle,
                    NV_PAVS_VOICE_CFG_ENV1, NV_PAVS_VOICE_CFG_ENV0_EA_DELAYTIME);
            voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EFCOUNT, delay_time * 16);
        } else if (ef_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_ATTACK) {
            voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EFCOUNT, 0);
        } else if (ef_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_HOLD) {
            uint16_t hold_time =
                (uint16_t)voice_get_mask(d, (uint16_t)selected_handle,
                    NV_PAVS_VOICE_CFG_ENVF, NV_PAVS_VOICE_CFG_ENVA_EA_HOLDTIME);
            voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EFCOUNT, hold_time * 16);
        }

        voice_reset_filters(d, (uint16_t)selected_handle);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE, 1);
        if (fe_spin_on())
            voice_spin_unlock(d, (uint16_t)selected_handle);
        if (!link_first) {
            MemoryBarrier();         /* ACTIVE and the setup before the link */
            voice_on_link(d, selected_handle, list);
        }

        if (!locked) voice_lock(d, (uint16_t)selected_handle, false);
        if (g_bw_lat_on && selected_handle < MCPX_HW_MAX_VOICES) {
            /* Blood Wake (lat-a, test aid): the voice starts on the chip */
            uint16_t h = (uint16_t)selected_handle;
            bw_lat_ev(BW_LAT_VOICEON, bw_lat_now(), h,
                      voice_get_mask(d, h, NV_PAVS_VOICE_CUR_PSL_START,
                                     NV_PAVS_VOICE_CUR_PSL_START_BA),
                      voice_get_mask(d, h, NV_PAVS_VOICE_PAR_NEXT, NV_PAVS_VOICE_PAR_NEXT_EBO),
                      d->ep_frame_div, list,
                      voice_get_mask(d, h, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_MULTIPASS),
                      0.0, 0.0);
            s_lat_vs[h] = 1;
        }
#ifdef _WIN32
        {
            /* Blood Wake (au-lat, BW_APU_VPWAKE, off by default): the frame
             * thread makes the voice's period now (carried apu_xaudio2.c) */
            extern void xa2_voice_wake(void);
            xa2_voice_wake();
        }
#endif
        break;
    }

    case NV1BA0_PIO_VOICE_RELEASE: {
        selected_handle = argument & NV1BA0_PIO_VOICE_ON_HANDLE;

        bool locked = is_voice_locked(d, (uint16_t)selected_handle);
        if (!locked) voice_lock(d, (uint16_t)selected_handle, true);

        uint16_t rr;
        if (fe_spin_on())
            voice_spin_lock(d, (uint16_t)selected_handle);
        rr = (uint16_t)voice_get_mask(d, (uint16_t)selected_handle,
            NV_PAVS_VOICE_TAR_LFO_ENV, NV_PAVS_VOICE_TAR_LFO_ENV_EA_RELEASERATE);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                       NV_PAVS_VOICE_CUR_ECNT_EACOUNT, rr * 16);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_EACUR,
                       NV_PAVS_VOICE_PAR_STATE_EFCUR_RELEASE);

        rr = (uint16_t)voice_get_mask(d, (uint16_t)selected_handle,
            NV_PAVS_VOICE_CFG_MISC, NV_PAVS_VOICE_CFG_MISC_EF_RELEASERATE);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                       NV_PAVS_VOICE_CUR_ECNT_EFCOUNT, rr * 16);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_EFCUR,
                       NV_PAVS_VOICE_PAR_STATE_EFCUR_RELEASE);
        if (fe_spin_on())
            voice_spin_unlock(d, (uint16_t)selected_handle);

        if (!locked) voice_lock(d, (uint16_t)selected_handle, false);
        break;
    }

    case NV1BA0_PIO_VOICE_OFF: {
        uint16_t h = (uint16_t)(argument & NV1BA0_PIO_VOICE_OFF_HANDLE);
        if (fe_spin_on() && h < MCPX_HW_MAX_VOICES)
            voice_spin_lock(d, h);
        s_lat_off_by_game = 1;
        voice_off(d, h);
        s_lat_off_by_game = 0;
        if (fe_spin_on() && h < MCPX_HW_MAX_VOICES)
            voice_spin_unlock(d, h);
        break;
    }

    case NV1BA0_PIO_VOICE_PAUSE: {
        uint16_t h = (uint16_t)(argument & NV1BA0_PIO_VOICE_PAUSE_HANDLE);
        if (fe_spin_on() && h < MCPX_HW_MAX_VOICES)
            voice_spin_lock(d, h);
        voice_set_mask(d, h, NV_PAVS_VOICE_PAR_STATE, NV_PAVS_VOICE_PAR_STATE_PAUSED,
                       (argument & NV1BA0_PIO_VOICE_PAUSE_ACTION) != 0);
        if (fe_spin_on() && h < MCPX_HW_MAX_VOICES)
            voice_spin_unlock(d, h);
        break;
    }

    case NV1BA0_PIO_SET_CURRENT_HRTF_ENTRY:
        d->vp.hrtf.current_entry =
            GET_MASK(argument, NV1BA0_PIO_SET_CURRENT_HRTF_ENTRY_HANDLE);
        break;

    case NV1BA0_PIO_SET_CURRENT_VOICE:
        d->regs[NV_PAPU_FECV] = argument;
        break;

    case NV1BA0_PIO_SET_VOICE_CFG_VBIN:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_VBIN, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_FMT:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_FMT, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_ENV0:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_ENV0, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_ENVA:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_ENVA, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_ENV1:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_ENV1, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_ENVF:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_ENVF, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_MISC:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_MISC, 0xFFFFFFFF, argument);
        break;

    case NV1BA0_PIO_SET_VOICE_TAR_HRTF: {
        int handle = GET_MASK(argument, NV1BA0_PIO_SET_VOICE_TAR_HRTF_HANDLE);
        int current_voice = d->regs[NV_PAPU_FECV];
        voice_set_mask(d, (uint16_t)current_voice,
                       NV_PAVS_VOICE_CFG_HRTF_TARGET,
                       NV_PAVS_VOICE_CFG_HRTF_TARGET_HANDLE, handle);
        if (current_voice < MCPX_HW_MAX_3D_VOICES &&
            handle != HRTF_NULL_HANDLE) {
            assert(handle < HRTF_ENTRY_COUNT);
            bw_hrtf_set_target(current_voice, &d->vp.filters[current_voice].hrtf,
                               d->vp.hrtf.entries[handle].hrir,
                               d->vp.hrtf.entries[handle].itd);
        }
        break;
    }

    case NV1BA0_PIO_SET_VOICE_TAR_VOLA:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_VOLA, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_VOLB:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_VOLB, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_VOLC:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_VOLC, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_LFO_ENV:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_LFO_ENV, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_FCA:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_FCA, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_FCB:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_FCB, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_PITCH:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_PITCH_LINK,
                       NV_PAVS_VOICE_TAR_PITCH_LINK_PITCH,
                       (argument & NV1BA0_PIO_SET_VOICE_TAR_PITCH_STEP) >> 16);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_BUF_BASE:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CUR_PSL_START,
                       NV_PAVS_VOICE_CUR_PSL_START_BA, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_BUF_LBO:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CUR_PSH_SAMPLE,
                       NV_PAVS_VOICE_CUR_PSH_SAMPLE_LBO, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_BUF_CBO:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_PAR_OFFSET,
                       NV_PAVS_VOICE_PAR_OFFSET_CBO, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_BUF_EBO:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_PAR_NEXT,
                       NV_PAVS_VOICE_PAR_NEXT_EBO, argument);
        break;

    case NV1BA0_PIO_SET_CURRENT_INBUF_SGE:
        d->vp.inbuf_sge_handle = argument & NV1BA0_PIO_SET_CURRENT_INBUF_SGE_HANDLE;
        break;

    case NV1BA0_PIO_SET_CURRENT_INBUF_SGE_OFFSET: {
        hwaddr sge_address =
            d->regs[NV_PAPU_VPSGEADDR] + d->vp.inbuf_sge_handle * 8;
        stl_le_phys(address_space_memory, sge_address,
                    bw_tag_intent(argument &
                                  NV1BA0_PIO_SET_CURRENT_INBUF_SGE_OFFSET_PARAMETER));
        break;
    }

    case NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE:
        d->vp.outbuf_sge_handle =
            argument & NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE_HANDLE;
        break;

    case NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE_OFFSET: {
        hwaddr sge_address =
            d->regs[NV_PAPU_VPSGEADDR] + d->vp.outbuf_sge_handle * 8;
        stl_le_phys(address_space_memory, sge_address,
                    argument & NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE_OFFSET_PARAMETER);
        break;
    }

    case NV1BA0_PIO_SET_VOICE_SSL_A: {
        int ssl = 0;
        int current_voice = d->regs[NV_PAPU_FECV];
        assert(current_voice < MCPX_HW_MAX_VOICES);
        d->vp.ssl[current_voice].base[ssl] =
            GET_MASK(argument, NV1BA0_PIO_SET_VOICE_SSL_A_BASE);
        d->vp.ssl[current_voice].count[ssl] =
            (uint8_t)GET_MASK(argument, NV1BA0_PIO_SET_VOICE_SSL_A_COUNT);
        break;
    }
    case NV1BA0_PIO_SET_VOICE_SSL_B: {
        int ssl = 1;
        int current_voice = d->regs[NV_PAPU_FECV];
        assert(current_voice < MCPX_HW_MAX_VOICES);
        d->vp.ssl[current_voice].base[ssl] =
            GET_MASK(argument, NV1BA0_PIO_SET_VOICE_SSL_A_BASE);
        d->vp.ssl[current_voice].count[ssl] =
            (uint8_t)GET_MASK(argument, NV1BA0_PIO_SET_VOICE_SSL_A_COUNT);
        break;
    }

    case NV1BA0_PIO_SET_CURRENT_SSL: {
        assert((argument & 0x3f) == 0);
        assert(argument < (MCPX_HW_MAX_SSL_PRDS * NV_PSGE_SIZE));
        d->vp.ssl_base_page = argument;
        break;
    }

    case NV1BA0_PIO_SET_HRTF_SUBMIXES:
        d->vp.hrtf_submix[0] = (uint8_t)((argument >> 0) & 0x1f);
        d->vp.hrtf_submix[1] = (uint8_t)((argument >> 8) & 0x1f);
        d->vp.hrtf_submix[2] = (uint8_t)((argument >> 16) & 0x1f);
        d->vp.hrtf_submix[3] = (uint8_t)((argument >> 24) & 0x1f);
        break;

    case NV1BA0_PIO_SET_HRTF_HEADROOM:
        d->vp.hrtf_headroom = (uint8_t)(argument & NV1BA0_PIO_SET_HRTF_HEADROOM_AMOUNT);
        break;

    case SE2FE_IDLE_VOICE:
        if (d->regs[NV_PAPU_FETFORCE1] & NV_PAPU_FETFORCE1_SE2FE_IDLE_VOICE) {
            d->regs[NV_PAPU_FECTL] &= ~NV_PAPU_FECTL_FEMETHMODE;
            d->regs[NV_PAPU_FECTL] |= NV_PAPU_FECTL_FEMETHMODE_TRAPPED;
            d->regs[NV_PAPU_FECTL] &= ~NV_PAPU_FECTL_FETRAPREASON;
            d->regs[NV_PAPU_FECTL] |= NV_PAPU_FECTL_FETRAPREASON_REQUESTED;
            /* Latched here, once per trap; update_irq no longer re-derives
             * it from FECTL (see there). */
            qatomic_or(&d->regs[NV_PAPU_ISTS], NV_PAPU_ISTS_FETINTSTS);
            d->set_irq = true;
            apu_irq_stats.traps++;
        }
        break;

    default:
        /* Handle range-based cases that can't use case ranges in MSVC */
        if (method >= NV1BA0_PIO_SET_HRIR && method < NV1BA0_PIO_SET_HRIR_X) {
            assert(d->vp.hrtf.current_entry < HRTF_ENTRY_COUNT);
            slot = (method - NV1BA0_PIO_SET_HRIR) / 4;
            int8_t left0 = (int8_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_LEFT0);
            int8_t right0 = (int8_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_RIGHT0);
            int8_t left1 = (int8_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_LEFT1);
            int8_t right1 = (int8_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_RIGHT1);
            int coeff_idx = slot * 2;
            set_hrir_coeff_tar(d, 0, coeff_idx, left0);
            set_hrir_coeff_tar(d, 1, coeff_idx, right0);
            set_hrir_coeff_tar(d, 0, coeff_idx + 1, left1);
            set_hrir_coeff_tar(d, 1, coeff_idx + 1, right1);
        } else if (method == NV1BA0_PIO_SET_HRIR_X) {
            assert(d->vp.hrtf.current_entry < HRTF_ENTRY_COUNT);
            int8_t left30 = (int8_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_X_LEFT30);
            int8_t right30 = (int8_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_X_RIGHT30);
            int16_t itd = (int16_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_X_ITD);
            set_hrir_coeff_tar(d, 0, 30, left30);
            set_hrir_coeff_tar(d, 1, 30, right30);
            d->vp.hrtf.entries[d->vp.hrtf.current_entry].itd = s6p9_to_float(itd);
            {
                /* Blood Wake (TK-18 diagnostic): BW_APU_HRIRDUMP=<n> prints
                 * the first n filter entries the title completes (both
                 * ears' 31 taps as int8 x128, and the ITD), to see whether
                 * the ears' 3D mix is anti-phase in the data itself. */
                static int left_to_dump = -1, nseen;
                static unsigned writes;
                static uint32_t seen[64];
                uint32_t hsh = 2166136261u;
                int dup = 0, si;
                if (left_to_dump < 0) {
                    const char *e = getenv("BW_APU_HRIRDUMP");
                    left_to_dump = e ? atoi(e) : 0;
                }
                if (left_to_dump > 0) {
                    /* distinct filters only (taps and ITD), with a count of
                     * all completed entries */
                    int ch, k, entry = d->vp.hrtf.current_entry;
                    writes++;
                    for (ch = 0; ch < 2; ch++)
                        for (k = 0; k < HRTF_NUM_TAPS; k++)
                            hsh = (hsh ^ (uint32_t)(int)(d->vp.hrtf.entries[entry].hrir[ch][k] * 128.0f))
                                  * 16777619u;
                    hsh = (hsh ^ (uint16_t)itd) * 16777619u;
                    for (si = 0; si < nseen; si++)
                        dup |= seen[si] == hsh;
                    if (!dup && nseen < 64)
                        seen[nseen++] = hsh;
                    if ((writes & (writes - 1)) == 0)
                        fprintf(stderr, "[APU-HRIR] %u entries completed, %d distinct\n",
                                writes, nseen);
                }
                if (left_to_dump > 0 && !dup) {
                    int ch, k, entry = d->vp.hrtf.current_entry;
                    left_to_dump--;
                    fprintf(stderr, "[APU-HRIR] entry %d itd %d (%.3f), raw word 0x%08X\n", entry, itd,
                            d->vp.hrtf.entries[entry].itd, argument);
                    for (ch = 0; ch < 2; ch++) {
                        fprintf(stderr, "[APU-HRIR]   %s", ch ? "R" : "L");
                        for (k = 0; k < HRTF_NUM_TAPS; k++)
                            fprintf(stderr, " %d",
                                    (int)(d->vp.hrtf.entries[entry].hrir[ch][k] * 128.0f));
                        fprintf(stderr, "\n");
                    }
                }
            }
        } else if (method >= NV1BA0_PIO_SET_SSL_SEGMENT_OFFSET &&
                   method < NV1BA0_PIO_SET_SSL_SEGMENT_LENGTH + 8 * 64) {
            assert((method & 0x3) == 0);
            hwaddr addr = d->regs[NV_PAPU_VPSSLADDR]
                          + (d->vp.ssl_base_page * 8)
                          + (method - NV1BA0_PIO_SET_SSL_SEGMENT_OFFSET);
            /* TK-17: tag the offset word (even slot) with the title's intent;
             * the length word (odd slot) is not an address. */
            stl_le_phys(address_space_memory, addr,
                        ((method - NV1BA0_PIO_SET_SSL_SEGMENT_OFFSET) & 4)
                            ? argument : bw_tag_intent(argument));
        } else if (method >= NV1BA0_PIO_SET_SUBMIX_HEADROOM &&
                   method <= NV1BA0_PIO_SET_SUBMIX_HEADROOM + 4 * (NUM_MIXBINS - 1)) {
            assert((method & 3) == 0);
            slot = (method - NV1BA0_PIO_SET_SUBMIX_HEADROOM) / 4;
            d->vp.submix_headroom[slot] =
                (uint8_t)(argument & NV1BA0_PIO_SET_SUBMIX_HEADROOM_AMOUNT);
        } else if ((method >= NV1BA0_PIO_SET_OUTBUF_BA &&
                    method < NV1BA0_PIO_SET_OUTBUF_BA + 32) ||
                   (method >= NV1BA0_PIO_SET_OUTBUF_LEN &&
                    method < NV1BA0_PIO_SET_OUTBUF_LEN + 32)) {
            /* Outbuf base/length - ignore for now */
        } else {
            /* Unknown method - silently ignore */
            DPRINTF("Unknown FE method: 0x%08X arg=0x%08X\n", method, argument);
        }
        break;
    }
}

/* ============================================================
 * VP MMIO read/write (exposed to apu_core.c)
 * ============================================================ */

uint64_t mcpx_apu_vp_read(void *opaque, hwaddr addr, unsigned int size)
{
    (void)opaque; (void)size;

    switch (addr) {
    case NV1BA0_PIO_FREE:
        return 0x80; /* Always pretend queue is empty */
    default:
        break;
    }
    return 0;
}

void mcpx_apu_vp_write(void *opaque, hwaddr addr, uint64_t val,
                        unsigned int size)
{
    MCPXAPUState *d = (MCPXAPUState *)opaque;
    (void)size;

    /* Dispatch known methods through fe_method */
    fe_method(d, (uint32_t)addr, (uint32_t)val);
}

/* ============================================================
 * SGE data pointer resolution
 * ============================================================ */

static hwaddr get_data_ptr(hwaddr sge_base, unsigned int max_sge, uint32_t addr)
{
    unsigned int entry = addr / TARGET_PAGE_SIZE;
    assert(entry <= max_sge);
    uint32_t prd_address =
        ldl_le_phys(address_space_memory, sge_base + entry * 4 * 2);
    return prd_address + addr % TARGET_PAGE_SIZE;
}

/* ============================================================
 * Envelope processing
 * ============================================================ */

static float voice_step_envelope(MCPXAPUState *d, uint16_t v, uint32_t reg_0,
                           uint32_t reg_a, uint32_t rr_reg, uint32_t rr_mask,
                           uint32_t lvl_reg, uint32_t lvl_mask,
                           uint32_t count_mask, uint32_t cur_mask)
{
    uint8_t cur = (uint8_t)voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE, cur_mask);
    switch (cur) {
    case NV_PAVS_VOICE_PAR_STATE_EFCUR_OFF:
        voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, 0);
        voice_set_mask(d, v, lvl_reg, lvl_mask, 0xFF);
        return 1.0f;

    case NV_PAVS_VOICE_PAR_STATE_EFCUR_DELAY: {
        uint16_t count =
            (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask);
        voice_set_mask(d, v, lvl_reg, lvl_mask, 0x00);
        if (count == 0) {
            cur++;
            voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE, cur_mask, cur);
        } else {
            count--;
        }
        voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, count);
        return 0.0f;
    }

    case NV_PAVS_VOICE_PAR_STATE_EFCUR_ATTACK: {
        uint16_t count =
            (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask);
        uint16_t attack_rate =
            (uint16_t)voice_get_mask(d, v, reg_0, NV_PAVS_VOICE_CFG_ENV0_EA_ATTACKRATE);
        float value;
        if (attack_rate == 0) {
            value = 255.0f;
        } else {
            if (count <= (uint32_t)(attack_rate * 16)) {
                value = (count * 0xFF) / (float)(attack_rate * 16);
            } else {
                value = 255.0f;
            }
        }
        voice_set_mask(d, v, lvl_reg, lvl_mask, (uint32_t)value);
        if (count == (uint32_t)(attack_rate * 16)) {
            cur++;
            voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE, cur_mask, cur);
            uint16_t hold_time =
                (uint16_t)voice_get_mask(d, v, reg_a, NV_PAVS_VOICE_CFG_ENVA_EA_HOLDTIME);
            count = hold_time * 16;
        } else {
            count++;
        }
        voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, count);
        return value / 255.0f;
    }

    case NV_PAVS_VOICE_PAR_STATE_EFCUR_HOLD: {
        uint16_t count =
            (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask);
        voice_set_mask(d, v, lvl_reg, lvl_mask, 0xFF);
        if (count == 0) {
            cur++;
            voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE, cur_mask, cur);
            uint16_t decay_rate =
                (uint16_t)voice_get_mask(d, v, reg_a, NV_PAVS_VOICE_CFG_ENVA_EA_DECAYRATE);
            count = decay_rate * 16;
        } else {
            count--;
        }
        voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, count);
        return 1.0f;
    }

    case NV_PAVS_VOICE_PAR_STATE_EFCUR_DECAY: {
        uint16_t count =
            (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask);
        uint16_t decay_rate =
            (uint16_t)voice_get_mask(d, v, reg_a, NV_PAVS_VOICE_CFG_ENVA_EA_DECAYRATE);
        uint8_t sustain_level =
            (uint8_t)voice_get_mask(d, v, reg_a, NV_PAVS_VOICE_CFG_ENVA_EA_SUSTAINLEVEL);
        float value;
        if (decay_rate == 0) {
            value = 0.0f;
        } else {
            value = 255.0f * powf(0.99988799f, (decay_rate * 16 - count) *
                                                   4096.0f / decay_rate);
        }
        if (value <= (sustain_level + 0.2f) || (value > 255.0f)) {
            /* Blood Wake carried patch (APU audit A1, docs/day1/D2.md part
             * 4): a decay ends at the sustain level, it never goes below it
             * (xemu's own FIXME). With a decay rate of 0 (every enveloped
             * voice Blood Wake starts: ENVA 0xFF000000, sustain at full)
             * this frame returned 0, so each such sound dropped to silence
             * for one 32-sample frame after its attack: a click. The
             * volume ramp spreads it over 64 samples. BW_APU_ENV_DECAY=xemu
             * restores the dip. */
            static int decay_fix = -1;
            if (decay_fix < 0) {
                const char *e = getenv("BW_APU_ENV_DECAY");
                decay_fix = !(e && e[0] == 'x');
            }
            if (decay_fix && value < sustain_level)
                value = sustain_level;
            cur++;
            voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE, cur_mask, cur);
        } else {
            count--;
            voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, count);
            voice_set_mask(d, v, lvl_reg, lvl_mask, (uint32_t)value);
        }
        return value / 255.0f;
    }

    case NV_PAVS_VOICE_PAR_STATE_EFCUR_SUSTAIN: {
        uint8_t sustain_level =
            (uint8_t)voice_get_mask(d, v, reg_a, NV_PAVS_VOICE_CFG_ENVA_EA_SUSTAINLEVEL);
        voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, 0x00);
        voice_set_mask(d, v, lvl_reg, lvl_mask, sustain_level);
        return sustain_level / 255.0f;
    }

    case NV_PAVS_VOICE_PAR_STATE_EFCUR_RELEASE: {
        uint16_t count =
            (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask);
        uint16_t release_rate = (uint16_t)voice_get_mask(d, v, rr_reg, rr_mask);
        if (release_rate == 0) count = 0;
        float value = 0;
        if (count == 0) {
            voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE, cur_mask, ++cur);
        } else {
            float pos = clampf(1 - count / (release_rate * 16.0f), 0, 1);
            uint8_t lvl = (uint8_t)voice_get_mask(d, v, lvl_reg, lvl_mask);
            value = powf((float)M_E, -6.91f * pos) * lvl;
            count--;
            voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, count);
        }
        return value / 255.0f;
    }

    case NV_PAVS_VOICE_PAR_STATE_EFCUR_FORCE_RELEASE:
        if (count_mask == NV_PAVS_VOICE_CUR_ECNT_EACOUNT) {
            voice_off(d, v);
        }
        return 0.0f;

    default:
        fprintf(stderr, "[APU] Unknown envelope state 0x%x\n", cur);
        return 0.0f;
    }
}

/* ============================================================
 * Per-voice source statistics (RECOMP_APU_TRACE)
 *
 * What each voice fed the mixer over the last second: its format word, where
 * its data was read from and how that address resolved, the amplitude
 * envelope and the first two volumes/bins, plus the RMS and zero-crossing
 * rate of the fetched samples. The zero-crossing rate separates music
 * (~0.1-0.2) from white noise (~0.5) without a spectrum, so a voice that
 * reads garbage shows up by itself.
 *
 * RECOMP_APU_VOICE_DUMP=<voice> also writes that voice's fetched samples,
 * before gain, as raw s16 stereo 48 kHz to RECOMP_APU_VOICE_DUMP_FILE
 * (default voice_dump.raw in the working directory).
 * ============================================================ */

struct VpVoiceStat {
    uint32_t frames, zc;
    double   sq;
    float    last, ea, rate;
    uint32_t addr, fmt;
    uint16_t vol0, vol1;
    uint8_t  bin0, bin1;
    const char *cls;
};
static struct VpVoiceStat s_vstat[MCPX_HW_MAX_VOICES];
static int s_vstat_on = -1;
static int s_vdump_voice = -1;      /* -2: every voice, one file each */
static FILE *s_vdump;
static FILE *s_vdump_all[MCPX_HW_MAX_VOICES];
static uint8_t s_vdump_failed[MCPX_HW_MAX_VOICES]; /* open failed: no retry */
static char s_vdump_prefix[512];
static unsigned s_vstat_frames, s_vstat_secs;

static void vp_stat_init(void)
{
    const char *e;
    s_vstat_on = apu_env_on("RECOMP_APU_TRACE");
    e = getenv("RECOMP_APU_VOICE_DUMP");
    if (e && *e) {
        const char *path = getenv("RECOMP_APU_VOICE_DUMP_FILE");
        if (!strcmp(e, "all")) {
            /* RECOMP_APU_VOICE_DUMP=all: <file>.<voice>.raw for each voice
             * that renders anything (voice slots are not stable between
             * runs, so a sound cannot be picked by number in advance). */
            s_vdump_voice = -2;
            snprintf(s_vdump_prefix, sizeof(s_vdump_prefix), "%s",
                     path && *path ? path : "voice_dump");
            fprintf(stderr, "[APU] voice dump: every voice -> %s.<voice>.raw\n",
                    s_vdump_prefix);
            return;
        }
        s_vdump_voice = atoi(e);
        s_vdump = fopen(path && *path ? path : "voice_dump.raw", "wb");
        fprintf(stderr, "[APU] voice dump: voice %d -> %s\n", s_vdump_voice,
                s_vdump ? (path && *path ? path : "voice_dump.raw") : "(open failed)");
    }
}

static void vp_stat_samples(uint32_t v, const float samples[][2], int n,
                            hwaddr first_addr, uint32_t fmt)
{
    struct VpVoiceStat *s = &s_vstat[v];
    if (s_vstat_on < 0) vp_stat_init();
    if (!s_vstat_on && (int)v != s_vdump_voice && s_vdump_voice != -2) return;
    if (n <= 0) return;
    if (s->frames == 0) {
        s->addr = (uint32_t)first_addr;
        s->cls = mcpx_apu_phys_class(first_addr);
        s->fmt = fmt;
    }
    for (int i = 0; i < n; i++) {
        float x = samples[i][0];
        s->sq += (double)x * x;
        if ((x < 0) != (s->last < 0)) s->zc++;
        s->last = x;
    }
    s->frames += (uint32_t)n;
    if (s_vdump_voice == -2 && !s_vdump_all[v] && !s_vdump_failed[v]) {
        char name[600];
        snprintf(name, sizeof(name), "%s.%u.raw", s_vdump_prefix, (unsigned)v);
        s_vdump_all[v] = fopen(name, "wb");
        s_vdump_failed[v] = s_vdump_all[v] == NULL;
    }
    {
        FILE *df = s_vdump_voice == -2 ? s_vdump_all[v]
                 : (int)v == s_vdump_voice ? s_vdump : NULL;
        int16_t out[NUM_SAMPLES_PER_FRAME * 2];
        int m = n < NUM_SAMPLES_PER_FRAME ? n : NUM_SAMPLES_PER_FRAME;
        if (!df)
            return;
        for (int i = 0; i < m; i++) {
            for (int c = 0; c < 2; c++) {
                float x = samples[i][c] * 32767.0f;
                if (x > 32767.0f) x = 32767.0f;
                if (x < -32768.0f) x = -32768.0f;
                out[2 * i + c] = (int16_t)x;
            }
        }
        fwrite(out, sizeof(int16_t) * 2, (size_t)m, df);
    }
}

static void vp_stat_report(void)
{
    if (s_vstat_on <= 0 && s_vdump_voice == -1) return;
    if (++s_vstat_frames % 1500) return;
    /* Flush the dumps once a second, trace or not: a watchdog _exit or a
     * crash skips the stdio buffers. */
    if (s_vdump) fflush(s_vdump);
    for (unsigned v = 0; v < MCPX_HW_MAX_VOICES; v++)
        if (s_vdump_all[v]) fflush(s_vdump_all[v]);
    if (s_vstat_on <= 0) return;
    s_vstat_secs++;
    for (unsigned v = 0; v < MCPX_HW_MAX_VOICES; v++) {
        struct VpVoiceStat *s = &s_vstat[v];
        if (!s->frames) continue;
        fprintf(stderr, "[APU-VSTAT] t=%us v=%u fmt=%08X stream=%u st=%u ss=%u cs=%u spb=%u"
                " rate=%.3f ea=%.2f vol=%03X/%03X bin=%u/%u addr=%08X %s"
                " frames=%u rms=%.0f zcr=%.2f\n",
                s_vstat_secs, v, s->fmt,
                (unsigned)((s->fmt & NV_PAVS_VOICE_CFG_FMT_DATA_TYPE) != 0),
                (unsigned)((s->fmt & NV_PAVS_VOICE_CFG_FMT_STEREO) != 0),
                (unsigned)((s->fmt & NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE) >> ctz32(NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE)),
                (unsigned)((s->fmt & NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE) >> ctz32(NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE)),
                (unsigned)((s->fmt & NV_PAVS_VOICE_CFG_FMT_SAMPLES_PER_BLOCK) >> ctz32(NV_PAVS_VOICE_CFG_FMT_SAMPLES_PER_BLOCK)),
                s->rate, s->ea, s->vol0, s->vol1, s->bin0, s->bin1, s->addr,
                s->cls ? s->cls : "?", s->frames,
                sqrt(s->sq / s->frames) * 32768.0, (double)s->zc / s->frames);
        s->frames = 0; s->zc = 0; s->sq = 0;
    }
}

/* ============================================================
 * Ring monitor (RECOMP_APU_RING)
 *
 * A title that streams compressed audio decodes it on the CPU into a looping
 * PCM buffer voice and keeps writing ahead of the play cursor. If the writer
 * falls behind, the VP plays the previous lap's data again: no digital zero,
 * nothing a check on the mix can see, and only a stutter to the ear.
 *
 * Each looping PCM buffer is split into 1 KB blocks, and the samples the VP
 * fetches are hashed per block as it plays them. When the cursor leaves a
 * block it played whole, that hash is compared with the one from the
 * block's previous play. Once some block of a buffer has played differently
 * on two laps, the buffer is a ring (one loaded once and looped -- a sound
 * effect -- never changes and is never judged). In a ring, a non-silent
 * block that plays exactly as it did one lap before is a stale-lap replay.
 *
 * "lead" is how far ahead of the cursor the writer is: the blocks after the
 * one being entered whose memory has changed since they were last played,
 * in source frames.
 *
 *   [APU-RING] stale v=<voice> blk=<b>/<n> lap=<laps> ms=<wall ms>
 *       first 200 stale-lap replays
 *   [APU-RING] t=<s> v=<voice> bytes=<ring size> entries=<blocks played>
 *       stale=<this second> stale_total=<run> lead_min=<frames> lead_avg=<frames>
 *       once a VP second for each ring that played
 * ============================================================ */

#define RING_BLK      1024u
#define RING_MAX_BLKS 1024u

struct VpRing {
    uint32_t ba, bytes, nblk;
    uint32_t cur;                 /* block being played, ~0: none */
    uint32_t cur_frames;          /* frames of it played so far */
    uint64_t acc;                 /* hash of those frames */
    bool     acc_sound;
    uint32_t laps;
    bool     dynamic;
    uint32_t entries, stale, stale_total;
    uint32_t lead_min, lead_n;
    uint64_t lead_sum;
    uint64_t played[RING_MAX_BLKS];   /* hash of its last whole play */
    uint64_t mem[RING_MAX_BLKS];      /* memory hash when it was last entered */
    uint8_t  seen[RING_MAX_BLKS];     /* bit 0: played, bit 1: mem valid */
};
static struct VpRing *s_ring[MCPX_HW_MAX_VOICES];
static int s_ring_on = -1;
static unsigned s_ring_logged;
static unsigned s_ring_frames, s_ring_secs;

static uint64_t ring_hash_block(MCPXAPUState *d, uint32_t ba, uint32_t off,
                                uint32_t len)
{
    uint64_t h = 0x9E3779B97F4A7C15ull;
    while (len) {
        uint32_t lin = ba + off;
        uint32_t run = TARGET_PAGE_SIZE - lin % TARGET_PAGE_SIZE;
        if (run > len) run = len;
        const uint8_t *p = mcpx_apu_phys(
            get_data_ptr(d->regs[NV_PAPU_VPSGEADDR], 0xFFFFFFFF, lin));
        uint32_t i = 0;
        for (; i + 8 <= run; i += 8) {
            uint64_t w;
            memcpy(&w, p + i, 8);
            h = (h ^ w) * 0x100000001B3ull;
            h ^= h >> 29;
        }
        for (; i < run; i++)
            h = (h ^ p[i]) * 0x100000001B3ull;
        off += run;
        len -= run;
    }
    return h;
}

/* The cursor left block r->cur. */
static void ring_close_block(struct VpRing *r, uint32_t v, uint32_t frames_per_blk)
{
    uint32_t b = r->cur;
    if (b == ~0u || r->cur_frames < frames_per_blk)
        return;                              /* not played whole */
    if (r->seen[b] & 1) {
        if (r->acc != r->played[b]) {
            r->dynamic = true;
        } else if (r->dynamic && r->acc_sound) {
            r->stale++;
            r->stale_total++;
            if (s_ring_logged < 200) {
                s_ring_logged++;
                fprintf(stderr, "[APU-RING] stale v=%u blk=%u/%u lap=%u ms=%llu\n",
                        v, b, r->nblk, r->laps,
                        (unsigned long long)(qemu_clock_get_us(QEMU_CLOCK_REALTIME) / 1000));
            }
        }
    }
    r->played[b] = r->acc;
    r->seen[b] |= 1;
}

/* The VP fetched n frames of a looping PCM buffer voice, the first at
 * offset first; frames are frame_bytes apart in the buffer. */
static void vp_ring_check(MCPXAPUState *d, uint32_t v, uint32_t ba,
                          uint32_t ebo, uint32_t frame_bytes, uint32_t first,
                          const float samples[][2], int n)
{
    struct VpRing *r;
    uint32_t bytes = (ebo + 1) * frame_bytes;
    uint32_t fpb;

    if (s_ring_on < 0)
        s_ring_on = apu_env_on("RECOMP_APU_RING");
    if (!s_ring_on || v >= MCPX_HW_MAX_VOICES || bytes < 2 * RING_BLK ||
        frame_bytes == 0 || RING_BLK % frame_bytes)
        return;
    fpb = RING_BLK / frame_bytes;
    r = s_ring[v];
    if (!r) {
        r = s_ring[v] = calloc(1, sizeof(*r));
        if (!r) return;
        r->ba = ~0u;
    }
    if (r->ba != ba || r->bytes != bytes) {   /* a new buffer on this voice */
        uint32_t keep_total = r->stale_total;
        memset(r, 0, sizeof(*r));
        r->ba = ba;
        r->bytes = bytes;
        r->nblk = bytes / RING_BLK;           /* a partial last block is skipped */
        if (r->nblk > RING_MAX_BLKS) r->nblk = RING_MAX_BLKS;
        r->cur = ~0u;
        r->lead_min = ~0u;
        r->stale_total = keep_total;
    }

    for (int i = 0; i < n; i++) {
        uint32_t b = (first + (uint32_t)i) / fpb;
        if (b != r->cur) {
            ring_close_block(r, v, fpb);
            if (b < r->cur && r->cur != ~0u)
                r->laps++;
            r->cur = b;
            r->cur_frames = 0;
            r->acc = 0x9E3779B97F4A7C15ull;
            r->acc_sound = false;
            if (b < r->nblk) {
                /* Entering b: the writer's lead, then b's memory hash. */
                uint64_t h = ring_hash_block(d, ba, b * RING_BLK, RING_BLK);
                r->entries++;
                if (r->dynamic) {
                    uint32_t k, lead = 0;
                    for (k = 1; k < r->nblk; k++) {
                        uint32_t bb = (b + k) % r->nblk;
                        if (!(r->seen[bb] & 2))
                            break;
                        if (ring_hash_block(d, ba, bb * RING_BLK, RING_BLK) == r->mem[bb])
                            break;
                        lead++;
                    }
                    lead *= fpb;
                    if (lead < r->lead_min) r->lead_min = lead;
                    r->lead_sum += lead;
                    r->lead_n++;
                }
                r->mem[b] = h;
                r->seen[b] |= 2;
            }
        }
        if (b >= r->nblk)
            continue;
        {
            uint32_t w[2];
            memcpy(w, samples[i], sizeof(w));
            r->acc = (r->acc ^ (((uint64_t)w[0] << 32) | w[1])) * 0x100000001B3ull;
            r->acc ^= r->acc >> 29;
            if (w[0] | w[1]) r->acc_sound = true;
            r->cur_frames++;
        }
    }
}

static void vp_ring_report(unsigned secs)
{
    if (s_ring_on <= 0) return;
    for (unsigned v = 0; v < MCPX_HW_MAX_VOICES; v++) {
        struct VpRing *r = s_ring[v];
        if (!r || !r->entries || !r->dynamic) {
            if (r) r->entries = 0;
            continue;
        }
        fprintf(stderr, "[APU-RING] t=%us v=%u bytes=%u entries=%u stale=%u"
                " stale_total=%u lead_min=%u lead_avg=%llu\n",
                secs, v, r->bytes, r->entries, r->stale, r->stale_total,
                r->lead_n ? r->lead_min : 0,
                (unsigned long long)(r->lead_n ? r->lead_sum / r->lead_n : 0));
        r->entries = 0;
        r->stale = 0;
        r->lead_min = ~0u;
        r->lead_sum = 0;
        r->lead_n = 0;
    }
}

/* ============================================================
 * Sample fetching from voice buffers
 * ============================================================ */

static int voice_get_samples(MCPXAPUState *d, uint32_t v, float samples[][2],
                       int num_samples_requested)
{
    assert(v < MCPX_HW_MAX_VOICES);
    bool stereo = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT,
                                 NV_PAVS_VOICE_CFG_FMT_STEREO) != 0;
    unsigned int channels = stereo ? 2 : 1;
    unsigned int sample_size = voice_get_mask(
        d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE);
    unsigned int container_sizes[4] = { 1, 2, 0, 4 };
    unsigned int container_size_index = voice_get_mask(
        d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE);
    unsigned int container_size = container_sizes[container_size_index];
    bool stream = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT,
                                 NV_PAVS_VOICE_CFG_FMT_DATA_TYPE) != 0;
    bool paused = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_STATE,
                                 NV_PAVS_VOICE_PAR_STATE_PAUSED) != 0;
    bool loop = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT,
                               NV_PAVS_VOICE_CFG_FMT_LOOP) != 0;
    uint32_t ebo = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_NEXT,
                                  NV_PAVS_VOICE_PAR_NEXT_EBO);
    uint32_t cbo = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_OFFSET,
                                  NV_PAVS_VOICE_PAR_OFFSET_CBO);
    uint32_t lbo = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CUR_PSH_SAMPLE,
                                  NV_PAVS_VOICE_CUR_PSH_SAMPLE_LBO);
    uint32_t ba = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CUR_PSL_START,
                                 NV_PAVS_VOICE_CUR_PSL_START_BA);
    unsigned int samples_per_block =
        1 + voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT,
                           NV_PAVS_VOICE_CFG_FMT_SAMPLES_PER_BLOCK);
    bool persist = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT,
                                  NV_PAVS_VOICE_CFG_FMT_PERSIST) != 0;

    int ssl_index = 0, ssl_seg = 0, page = 0, count = 0;
    int seg_len = 0, seg_cs = 0, seg_spb = 0, seg_s = 0;
    hwaddr segment_offset = 0;
    uint32_t segment_length = 0;
    size_t block_size;

    int adpcm_block_index = -1;
    /* Room for the largest block the MCPX accepts (see the check below). */
    uint32_t adpcm_block[36 * ADPCM_MAX_BLOCKS / 4];
    int16_t adpcm_decoded[65 * ADPCM_MAX_BLOCKS * 2];

    voice_set_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_STATE,
                   NV_PAVS_VOICE_PAR_STATE_NEW_VOICE, 0);

    if (paused) return -1;

    if (stream) {
        if (!persist) {
            int eacur = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_STATE,
                                       NV_PAVS_VOICE_PAR_STATE_EACUR);
            if (eacur < NV_PAVS_VOICE_PAR_STATE_EFCUR_RELEASE) {
                voice_off(d, (uint16_t)v);
                return -1;
            }
        }

        assert(!loop);
        ssl_index = d->vp.ssl[v].ssl_index;
        ssl_seg = d->vp.ssl[v].ssl_seg;
        page = d->vp.ssl[v].base[ssl_index] + ssl_seg;
        count = d->vp.ssl[v].count[ssl_index];

        if (count == 0) {
            voice_set_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_OFFSET,
                           NV_PAVS_VOICE_PAR_OFFSET_CBO, 0);
            d->vp.ssl[v].ssl_seg = 0;
            if (!persist) {
                d->vp.ssl[v].ssl_index = 0;
                voice_off(d, (uint16_t)v);
            } else {
                set_notify_status(d, v, MCPX_HW_NOTIFIER_SSLA_DONE +
                                  d->vp.ssl[v].ssl_index,
                                  NV1BA0_NOTIFICATION_STATUS_DONE_SUCCESS);
            }
            return -1;
        }

        hwaddr addr = d->regs[NV_PAPU_VPSSLADDR] + page * 8;
        segment_offset = ldl_le_phys(address_space_memory, addr);
        segment_length = ldl_le_phys(address_space_memory, addr + 4);
        assert(segment_offset != 0);
        assert(segment_length != 0);
        seg_len = (segment_length >> 0) & 0xffff;
        seg_cs = (segment_length >> 16) & 3;
        seg_spb = (segment_length >> 18) & 0x1f;
        seg_s = (segment_length >> 23) & 1;
        container_size_index = seg_cs;
        if (seg_cs == NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_ADPCM) {
            sample_size = NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S24;
        }
        assert(seg_len > 0);
        ebo = seg_len - 1;
    }

    bool adpcm =
        (container_size_index == NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_ADPCM);
    if (g_bw_hot)
        s_hot_vflags |= (adpcm ? BW_HOT_V_ADPCM : 0) | (stream ? BW_HOT_V_STREAM : 0);

    if (adpcm) {
        /* The block is 36 bytes per channel times the descriptor's 5-bit
         * samples-per-block field, read into the fixed buffers above. A
         * value past what they hold overran the stack. */
        if (samples_per_block > ADPCM_MAX_BLOCKS) {
            static int warned;
            if (!warned++)
                fprintf(stderr, "[APU] voice %u: ADPCM samples per block %u "
                        "is above %d; voice skipped\n",
                        v, samples_per_block, ADPCM_MAX_BLOCKS);
            return -1;
        }
        block_size = 36;
    } else {
        block_size = container_size;
    }
    block_size *= samples_per_block;

    int sample_count = 0;
    hwaddr stat_addr = 0;
    for (; (sample_count < num_samples_requested) && (cbo <= ebo);
         sample_count++, cbo++) {
        if (sample_count == 0 && s_vstat_on != 0) {
            uint32_t lin = adpcm ? (cbo / ADPCM_SAMPLES_PER_BLOCK) * (uint32_t)block_size
                                 : cbo * (uint32_t)block_size;
            stat_addr = stream ? segment_offset + lin
                               : get_data_ptr(d->regs[NV_PAPU_VPSGEADDR],
                                              0xFFFFFFFF, ba + lin);
        }
        if (adpcm) {
            unsigned int block_index = cbo / ADPCM_SAMPLES_PER_BLOCK;
            unsigned int block_position = cbo % ADPCM_SAMPLES_PER_BLOCK;
            if (adpcm_block_index != (int)block_index) {
                uint32_t linear_addr = block_index * (uint32_t)block_size;
                if (stream) {
                    hwaddr addr = segment_offset + linear_addr;
                    memcpy(adpcm_block, mcpx_apu_phys(addr),
                           block_size);
                } else {
                    linear_addr += ba;
                    for (unsigned int word_index = 0;
                         word_index < (9 * samples_per_block); word_index++) {
                        hwaddr addr = get_data_ptr(d->regs[NV_PAPU_VPSGEADDR],
                                                   0xFFFFFFFF, linear_addr);
                        adpcm_block[word_index] =
                            ldl_le_phys(address_space_memory, addr);
                        linear_addr += 4;
                    }
                }
                adpcm_decode_block(adpcm_decoded, (uint8_t *)adpcm_block,
                                   block_size, channels);
                adpcm_block_index = block_index;
            }

            samples[sample_count][0] =
                int16_to_float(adpcm_decoded[block_position * channels]);
            if (stereo) {
                samples[sample_count][1] = int16_to_float(
                    adpcm_decoded[block_position * channels + 1]);
            }
        } else {
            hwaddr addr;
            if (stream) {
                addr = segment_offset + cbo * block_size;
            } else {
                uint32_t linear_addr = ba + cbo * (uint32_t)block_size;
                addr = get_data_ptr(d->regs[NV_PAPU_VPSGEADDR], 0xFFFFFFFF,
                                    linear_addr);
            }

            for (unsigned int channel = 0; channel < channels; channel++) {
                uint32_t ival;
                float fval;
                switch (sample_size) {
                case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_U8:
                    ival = ldub_phys(address_space_memory, addr);
                    fval = uint8_to_float((uint8_t)(ival & 0xff));
                    break;
                case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S16:
                    ival = lduw_le_phys(address_space_memory, addr);
                    fval = int16_to_float((int16_t)(ival & 0xffff));
                    break;
                case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S24:
                    ival = ldl_le_phys(address_space_memory, addr);
                    fval = int24_to_float(ival);
                    break;
                case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S32:
                    ival = ldl_le_phys(address_space_memory, addr);
                    fval = int32_to_float(ival);
                    break;
                default:
                    fval = 0.0f;
                    break;
                }
                samples[sample_count][channel] = fval;
                addr += container_size;
            }
        }

        if (!stereo) {
            samples[sample_count][1] = samples[sample_count][0];
        }
    }

    vp_stat_samples(v, samples, sample_count, stat_addr,
                    voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT,
                                   0xFFFFFFFF));
    if (!stream && loop && !adpcm && sample_count > 0)
        vp_ring_check(d, v, ba, ebo, (uint32_t)block_size,
                      cbo - (uint32_t)sample_count, samples, sample_count);

    if (cbo >= ebo) {
        if (stream) {
            d->vp.ssl[v].ssl_seg += 1;
            cbo = 0;
            if (d->vp.ssl[v].ssl_seg < d->vp.ssl[v].count[ssl_index]) {
                /* Move to next segment */
            } else {
                int next_index = (ssl_index + 1) % 2;
                d->vp.ssl[v].ssl_index = next_index;
                d->vp.ssl[v].ssl_seg = 0;
                set_notify_status(d, v, MCPX_HW_NOTIFIER_SSLA_DONE + ssl_index,
                                  NV1BA0_NOTIFICATION_STATUS_DONE_SUCCESS);
            }
        } else {
            if (loop) {
                cbo = lbo;
            } else {
                cbo = ebo;
                voice_off(d, (uint16_t)v);
            }
        }
    }

    voice_set_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_OFFSET,
                   NV_PAVS_VOICE_PAR_OFFSET_CBO, cbo);
    return sample_count;
}

/* ============================================================
 * Voice resampling (linear interpolation in place of libsamplerate)
 *
 * libsamplerate is stubbed; voice_resample below interpolates linearly
 * between source frames at the pitch's ratio, keeping its position and
 * the last frame across calls in the voice's filter state.
 * ============================================================ */

static int voice_resample(MCPXAPUState *d, uint16_t v, float samples[][2],
                          int requested_num, float rate)
{
    /* Linear interpolation in place of libsamplerate. `rate` is the output
     * to source ratio the VP's pitch encodes (libsamplerate's src_ratio):
     * 1.088 for 44.1 kHz material at the 48 kHz frame rate, so
     * each output consumes 1/rate source samples. Fetching one source sample
     * per output, as the stub before did, played everything a semitone and a
     * half high, 8.8 % too fast -- and drained each stream buffer faster than
     * the title filled it, which is where the periodic dropouts came from.
     *
     * State per voice: the two source samples the output sits between
     * (rs_last at 0, rs_next at 1) and the output's position rs_pos in
     * [0,1). Source samples arrive from voice_get_samples in the voice's own
     * units; a shortfall (the voice ended or its stream ran dry) returns the
     * outputs that could be made, and a fetch failure with nothing made is
     * passed through as it was. */
    MCPXAPUVoiceFilter *f = &d->vp.filters[v];
    float src[NUM_SAMPLES_PER_FRAME * 4 + 2][2];
    const int max_src = (int)(sizeof(src) / sizeof(src[0]));
    float step;
    int need, got, in, out;

    if (!(rate > 0.0f)) rate = 1.0f;
    step = 1.0f / rate;

    if (!f->rs_have) {
        got = voice_get_samples(d, v, src, 2);
        /* A first fetch that gets one sample returns no output and drops
         * that sample: the next call fetches two afresh. A voice whose
         * stream starts one sample at a time loses its first sample. */
        if (got < 2) return got < 0 ? got : 0;
        f->rs_last[0] = src[0][0]; f->rs_last[1] = src[0][1];
        f->rs_next[0] = src[1][0]; f->rs_next[1] = src[1][1];
        f->rs_pos = 0.0f;
        f->rs_have = 1;
    }

    /* Source samples the next requested_num outputs will step past, exactly:
     * output k sits at rs_pos + k * step, and every whole unit that position
     * reaches before the last output is a source sample consumed. The last
     * output's own step is left pending in rs_pos for the next call, so
     * nothing fetched here goes unused -- fetching floor(rs_pos + n * step)
     * instead over-read by up to one sample a frame, which was dropped: the
     * music came out 3 % fast. A step above about 4.2 (rate below 0.24, a
     * voice pitched more than two octaves up) wants more than the 130-frame
     * buffer holds over 31 steps; make fewer outputs rather than lose
     * source, so such a voice comes out short (and late) each frame. */
    for (;;) {
        need = (int)floorf(f->rs_pos + (float)(requested_num - 1) * step);
        if (need <= max_src || requested_num <= 1) break;
        requested_num /= 2;
    }
    if (need > max_src) need = max_src;
    if (need < 0) need = 0;

    got = need > 0 ? voice_get_samples(d, v, src, need) : 0;
    if (got < 0) got = 0;

    for (in = 0, out = 0; out < requested_num; out++) {
        while (f->rs_pos >= 1.0f) {
            if (in >= got) goto done;
            f->rs_last[0] = f->rs_next[0]; f->rs_last[1] = f->rs_next[1];
            f->rs_next[0] = src[in][0];    f->rs_next[1] = src[in][1];
            in++;
            f->rs_pos -= 1.0f;
        }
        samples[out][0] = f->rs_last[0] + f->rs_pos * (f->rs_next[0] - f->rs_last[0]);
        samples[out][1] = f->rs_last[1] + f->rs_pos * (f->rs_next[1] - f->rs_last[1]);
        f->rs_pos += step;
    }
done:
    if (out == 0 && got == 0) {
        /* Nothing new and nothing left to interpolate: the voice has stopped
         * or its stream is empty. voice_process treats 0 as an underrun and
         * a negative as the end; the voice's active bit says which. */
        int active = voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                                    NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE);
        return active ? 0 : -1;
    }
    return out;
}

/* ============================================================
 * Blood Wake carried patch (Remastered, BW_FIX AUDIO_SINC_RESAMPLE, D-016):
 * a windowed-sinc resampler in place of the linear one.
 *
 * Each output sample is the source convolved with a Kaiser-windowed sinc
 * (beta 8, 8 zero crossings a side) whose cutoff follows the pitch: at or
 * below the source rate (step <= 1, e.g. 44.1 kHz material at 48 kHz) the
 * kernel passes the source's whole band; pitched up (step > 1) it narrows
 * to the output's band, so the voice doesn't alias, widening to at most 32
 * taps a side (step 4). The weights are normalised to sum to 1 (DC gain
 * exact). The kernel needs 8-32 source samples ahead of the output, which
 * are fetched ahead; samples before a voice's start are zeros, so nothing
 * is delayed. Original keeps the linear interpolation (what the port always
 * had; the hardware's method is unknown, xemu uses libsamplerate).
 * BW_APU_RESAMPLE=linear|sinc overrides the edition for A/B tests.
 * ============================================================ */

#include "../../bw_edition.h"
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define BW_SINC_ZC 8                   /* zero crossings a side at full band */
#define BW_SINC_RES 512                 /* table points per zero crossing */
#define BW_SINC_MAXHALF 32              /* taps a side at the narrowest */
#define BW_SINC_HIST 256

typedef struct {
    int have;
    int n;                              /* samples in hist */
    double pos;                         /* next output, in source samples from hist[0] */
    float hist[BW_SINC_HIST][2];
} BwSinc;
static BwSinc *s_bw_sinc;               /* one per voice, allocated on first use */
static float s_bw_sinc_tab[BW_SINC_ZC * BW_SINC_RES + 2];

static int bw_sinc_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("BW_APU_RESAMPLE");     /* linear | sinc: overrides */
        if (e && *e)
            on = e[0] == 's';
        else
            on = bw_fix_enabled(BW_FIX_AUDIO_SINC_RESAMPLE);
        if (on) {
            int i, k;
            double beta = 8.0, i0b = 1.0, t = 1.0;
            for (k = 1; k < 50; k++) {           /* I0(beta) by its series */
                t *= (beta / 2.0 / k) * (beta / 2.0 / k);
                i0b += t;
            }
            for (i = 0; i <= BW_SINC_ZC * BW_SINC_RES + 1; i++) {
                double x = (double)i / BW_SINC_RES, r = x / BW_SINC_ZC, a, w, s, term;
                if (r >= 1.0) {
                    s_bw_sinc_tab[i] = 0.0f;
                    continue;
                }
                a = beta * sqrt(1.0 - r * r);
                for (k = 1, w = 1.0, term = 1.0; k < 50; k++) {
                    term *= (a / 2.0 / k) * (a / 2.0 / k);
                    w += term;
                }
                s = x == 0.0 ? 1.0 : sin(M_PI * x) / (M_PI * x);
                s_bw_sinc_tab[i] = (float)(s * w / i0b);
            }
            s_bw_sinc = (BwSinc *)calloc(MCPX_HW_MAX_VOICES, sizeof(BwSinc));
            if (!s_bw_sinc)
                on = 0;
        }
        fprintf(stderr, "[APU] BW voice resampler: %s\n",
                on ? "windowed sinc (Kaiser, Remastered AUDIO_SINC_RESAMPLE)" : "linear");
    }
    return on;
}

static void bw_sinc_reset(uint16_t v)
{
    if (s_bw_sinc)
        s_bw_sinc[v].have = 0;
}

static inline float bw_sinc_k(double x)  /* x >= 0, in zero crossings */
{
    double fi = x * BW_SINC_RES;
    int i = (int)fi;
    float f = (float)(fi - i);
    if (i >= BW_SINC_ZC * BW_SINC_RES)
        return 0.0f;
    return s_bw_sinc_tab[i] + f * (s_bw_sinc_tab[i + 1] - s_bw_sinc_tab[i]);
}

static int voice_resample_sinc(MCPXAPUState *d, uint16_t v, float samples[][2],
                               int requested_num, float rate)
{
    BwSinc *s = &s_bw_sinc[v];
    double step, fc, half, last;
    int need, got = 0, out, lo, hi, j, keep;

    if (!(rate > 0.0f)) rate = 1.0f;
    step = 1.0 / rate;
    fc = step > 1.0 ? 1.0 / step : 1.0;
    if (fc < (double)BW_SINC_ZC / BW_SINC_MAXHALF)
        fc = (double)BW_SINC_ZC / BW_SINC_MAXHALF;
    half = BW_SINC_ZC / fc;

    if (!s->have) {
        memset(s->hist, 0, sizeof(float) * 2 * BW_SINC_MAXHALF);
        s->n = BW_SINC_MAXHALF;          /* zeros before the voice's start */
        s->pos = BW_SINC_MAXHALF;
        s->have = 1;
    }

    /* outputs that fit the history buffer, and the source they reach */
    for (;;) {
        last = s->pos + (double)(requested_num - 1) * step;
        need = (int)floor(last + half) + 1;
        if (need <= BW_SINC_HIST || requested_num <= 1)
            break;
        requested_num /= 2;
    }
    if (need > BW_SINC_HIST)
        need = BW_SINC_HIST;
    if (need > s->n) {
        got = voice_get_samples(d, v, &s->hist[s->n], need - s->n);
        if (got < 0) got = 0;
        s->n += got;
    }

    for (out = 0; out < requested_num; out++) {
        double p = s->pos;
        float acc0 = 0.0f, acc1 = 0.0f, wsum = 0.0f;
        lo = (int)ceil(p - half);
        hi = (int)floor(p + half);
        if (hi >= s->n)
            break;                       /* the source isn't there yet */
        if (lo < 0) lo = 0;
        if (g_bw_hot)
            s_hot_taps += hi - lo + 1;   /* au-hot */
        for (j = lo; j <= hi; j++) {
            float w = bw_sinc_k(fabs(p - j) * fc);
            acc0 += w * s->hist[j][0];
            acc1 += w * s->hist[j][1];
            wsum += w;
        }
        if (wsum != 0.0f) {
            samples[out][0] = acc0 / wsum;
            samples[out][1] = acc1 / wsum;
        } else {
            samples[out][0] = samples[out][1] = 0.0f;
        }
        s->pos += step;
    }

    /* drop what no later output can reach */
    keep = (int)floor(s->pos - BW_SINC_MAXHALF);
    if (keep > 0) {
        if (keep > s->n) keep = s->n;
        memmove(s->hist, s->hist[keep], sizeof(float) * 2 * (s->n - keep));
        s->n -= keep;
        s->pos -= keep;
    }

    if (out == 0 && got == 0) {
        int active = voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                                    NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE);
        return active ? 0 : -1;
    }
    return out;
}

/* ============================================================
 * Blood Wake (au-hot, docs/audio/hot.md): the HRTF filter without the
 * denormal stalls. A port bug in every edition.
 *
 * The filter (xemu's hrtf.h) moves its current coefficients and ITD a
 * hundredth of the way to their targets at every sample. Toward a target of
 * zero (an HRIR tap that is 0, a source straight ahead) a value decays into
 * the denormal range and stays there: below about 50 of the smallest
 * denormals, a hundredth of it rounds to nothing (seen: BW_APU_HOTSTAT's
 * census, thousands of stuck coefficients and ITDs per 5 s in a battle). On
 * this x64 CPU every multiply or add that reads or makes a denormal takes a
 * microcode assist of about 150 cycles, so a voice slot carrying one costs
 * up to about 0.1 ms a frame, and the battle's slow periods were that.
 *
 * The cure: the filter runs with flush-to-zero and denormals-are-zero, and
 * only the filter (the frame thread's MXCSR is restored right after; the
 * guest's code and the rest of the VP keep the default mode). What it
 * changes: a value under 2^-126 becomes 0. Every value it touches is a sum
 * of terms whose gains are at most about 1, so a difference that small can
 * only show in a result that is itself under about 2^-100, and every such
 * result is 0 once the bins are made 24-bit for the DSP (float_to_24b), so
 * the VP's output is the same, bit for bit (inferred; and proven in game by
 * BW_APU_HRTF_PROOF below).
 *
 *   BW_APU_HRTF_FTZ=0       the exact float path (the undo); unset: on
 *   BW_APU_HRTF_PROOF=1     test aid: beside every filter run, the exact path
 *                           on a shadow filter of its own, and beside the
 *                           bins a shadow set mixed from it; every frame the
 *                           bins are compared as floats and as the 24-bit
 *                           words the DSP gets ([APU-HRTF-PROOF] every 5 s)
 * ============================================================ */
#include <xmmintrin.h>
static int s_hrtf_ftz = -1, s_hrtf_proof = -1;
static HrtfFilter s_hp_f[MCPX_HW_MAX_3D_VOICES];
static float s_hp_bins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
static float s_hp_out[NUM_SAMPLES_PER_FRAME][2];  /* the voice's exact HRTF output */
static int s_hp_cur;                    /* the voice being mixed has an exact output of its own */
static struct {
    unsigned long long frames, bins, bins24, binsf, calls, calls_diff, samp_diff, mp_reads, mp_incomplete;
    double max_df;
    int shown, shown_run;
} s_hp;

static int bw_hrtf_ftz(void)
{
    if (s_hrtf_ftz < 0) {
        const char *e = getenv("BW_APU_HRTF_FTZ");
        const char *pr = getenv("BW_APU_HRTF_PROOF");
        s_hrtf_ftz = !(e && e[0] == '0');
        s_hrtf_proof = pr && (pr[0] == '1' || pr[0] == '2') ? pr[0] - '0' : 0;
        fprintf(stderr, "[APU] BW HRTF filter: %s (BW_APU_HRTF_FTZ)%s\n",
                s_hrtf_ftz ? "flush-to-zero around it, so no denormal stalls (au-hot; the bins unchanged)"
                           : "the exact float path, denormal stalls and all",
                s_hrtf_proof == 1 ? "; BW_APU_HRTF_PROOF: the exact path beside it on the same targets, the bins"
                                    " compared every frame"
                : s_hrtf_proof == 2 ? "; BW_APU_HRTF_PROOF=2: the exact path beside it, the real filter reading its"
                                      " targets live" : "");
    }
    return s_hrtf_ftz;
}

static int bw_f32_bits_eq(const float *a, const float *b, size_t n)
{
    return memcmp(a, b, n * sizeof(float)) == 0;
}

/* Never inlined, and compiled with the floating-point environment honoured,
 * so no other float operation can move across the mode change (the compiled
 * code: stmxcsr, ldmxcsr, the filter's call, ldmxcsr; no x87 anywhere in
 * the VP, seen with dumpbin). */
#pragma fenv_access (on)
static __declspec(noinline) void bw_hrtf_run(HrtfFilter *f, int v, float samples[][2])
{
    float in[NUM_SAMPLES_PER_FRAME][2];
    HrtfFilter snap, *run = f;
    if (s_hrtf_proof > 0) {
        /* the shadow takes the real filter's input history and targets (plain
         * stores and copies, which the mode doesn't touch) and keeps its own
         * current coefficients and ITD, which only the exact path moves.
         * The game's thread writes a voice's targets (SET_VOICE_TAR_HRTF)
         * without a lock, so they can change while the filter runs; for the
         * proof both paths run on one snapshot of them (=1). =2 lets the real
         * filter read them live, as it does without the proof (the race then
         * shows as differences even with both paths exact). */
        HrtfFilter *s = &s_hp_f[v];
        if (s_hrtf_proof == 1) {
            snap = *f;
            run = &snap;
        }
        memcpy(in, samples, sizeof in);
        s->buf_pos = run->buf_pos;
        s->itd_tar = run->itd_tar;
        for (int c = 0; c < 2; c++) {
            memcpy(s->ch[c].buf, run->ch[c].buf, sizeof s->ch[c].buf);
            memcpy(s->ch[c].hrir_coeff_tar, run->ch[c].hrir_coeff_tar, sizeof s->ch[c].hrir_coeff_tar);
        }
    }
    if (s_hrtf_ftz) {
        unsigned m = _mm_getcsr();
        _mm_setcsr(m | 0x8040);          /* FTZ (bit 15) and DAZ (bit 6) */
        hrtf_filter_process(run, samples, samples);
        _mm_setcsr(m);
    } else {
        hrtf_filter_process(run, samples, samples);
    }
    if (run != f) {
        /* the snapshot's moving parts back into the voice's filter (its
         * targets stay whatever the game has written since) */
        f->buf_pos = run->buf_pos;
        f->itd_cur = run->itd_cur;
        for (int c = 0; c < 2; c++) {
            memcpy(f->ch[c].buf, run->ch[c].buf, sizeof f->ch[c].buf);
            memcpy(f->ch[c].hrir_coeff_cur, run->ch[c].hrir_coeff_cur, sizeof f->ch[c].hrir_coeff_cur);
        }
        f = run;                         /* the diagnostics below compare what ran */
    }
    if (s_hrtf_proof > 0) {
        int nd = 0;
        hrtf_filter_process(&s_hp_f[v], in, s_hp_out);   /* the exact path, the default mode */
        double mx = 0.0;
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++)
            for (int c = 0; c < 2; c++)
                if (memcmp(&s_hp_out[i][c], &samples[i][c], 4)) {
                    double df = fabs((double)s_hp_out[i][c] - (double)samples[i][c]);
                    nd++;
                    if (df > mx)
                        mx = df;
                }
        if (mx > 1e-20 && s_hp.shown_run < 12) {
            /* a difference that isn't denormal-sized: the two states, side by side */
            HrtfFilter *s = &s_hp_f[v];
            double dc = 0.0;
            int kc = -1, cc = -1;
            for (int c = 0; c < 2; c++)
                for (int k = 0; k < HRTF_NUM_TAPS; k++) {
                    double e = fabs((double)s->ch[c].hrir_coeff_cur[k] - (double)f->ch[c].hrir_coeff_cur[k]);
                    if (e > dc)
                        dc = e, kc = k, cc = c;
                }
            s_hp.shown_run++;
            fprintf(stderr, "[APU-HRTF-PROOF] run %llu, voice %d: output differs by up to %.3g; the ITD %.9g"
                            " (0x%08X) against the exact %.9g (0x%08X), target %.9g; coefficients differ by up to"
                            " %.3g (ear %d tap %d: %.9g against %.9g, target %.9g); buf_pos %d; input[0] %.9g\n",
                    s_hp.calls, v, mx, (double)f->itd_cur, *(uint32_t *)&f->itd_cur, (double)s->itd_cur,
                    *(uint32_t *)&s->itd_cur, (double)f->itd_tar, dc, cc, kc,
                    cc >= 0 ? (double)f->ch[cc].hrir_coeff_cur[kc] : 0.0,
                    cc >= 0 ? (double)s->ch[cc].hrir_coeff_cur[kc] : 0.0,
                    cc >= 0 ? (double)f->ch[cc].hrir_coeff_tar[kc] : 0.0, f->buf_pos, (double)in[0][0]);
        }
        s_hp.calls++;
        s_hp.calls_diff += nd != 0;
        s_hp.samp_diff += nd;
        s_hp_cur = 1;
    }
}
#pragma fenv_access (off)

/* the proof's frame: the shadow bins start as the real ones (silence) */
static void bw_hrtf_proof_begin(float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    memcpy(s_hp_bins, mixbins, sizeof s_hp_bins);
}

/* and end: every bin sample compared, as floats and as the DSP's 24-bit words */
static void bw_hrtf_proof_end(MCPXAPUState *d, float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    for (int b = 0; b < NUM_MIXBINS; b++)
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            float r = mixbins[b][i], x = s_hp_bins[b][i];
            s_hp.bins++;
            if (memcmp(&r, &x, 4)) {
                double df = fabs((double)r - (double)x);
                s_hp.binsf++;
                if (df > s_hp.max_df)
                    s_hp.max_df = df;
                if (float_to_24b(r) != float_to_24b(x)) {
                    s_hp.bins24++;
                    if (s_hp.shown++ < 20)
                        fprintf(stderr, "[APU-HRTF-PROOF] DIFFERS: frame %llu bin %d sample %d: %.9g (0x%06X)"
                                        " against the exact path's %.9g (0x%06X)\n", s_hp.frames, b, i,
                                (double)r, float_to_24b(r), (double)x, float_to_24b(x));
                }
            }
        }
    if (++s_hp.frames % 7500 == 0)
        fprintf(stderr, "[APU-HRTF-PROOF] %llu frames: %llu bin samples compared, %llu differ as the DSP's 24-bit"
                        " words, %llu as floats (the largest %.3g); %llu filter runs, %llu with an output not"
                        " bit-identical (%llu samples); multipass reads %llu (%llu of a shadow bin that differed)%s\n",
                s_hp.frames, s_hp.bins, s_hp.bins24, s_hp.binsf, s_hp.max_df, s_hp.calls, s_hp.calls_diff,
                s_hp.samp_diff, s_hp.mp_reads, s_hp.mp_incomplete,
                s_hrtf_ftz ? "" : " (BW_APU_HRTF_FTZ=0: both paths exact)");
    (void)d;
}

/* ============================================================
 * Voice processing (main per-voice function)
 * ============================================================ */

static void voice_process(MCPXAPUState *d,
                          float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME],
                          float sample_buf[NUM_SAMPLES_PER_FRAME][2],
                          uint16_t v, int voice_list)
{
    assert(v < MCPX_HW_MAX_VOICES);
    bool stereo = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                 NV_PAVS_VOICE_CFG_FMT_STEREO) != 0;
    unsigned int channels = stereo ? 2 : 1;
    bool paused = voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                                 NV_PAVS_VOICE_PAR_STATE_PAUSED) != 0;

    struct McpxApuDebugVoice *dbg = &g_dbg.vp.v[v];
    s_hp_cur = 0;                        /* au-hot's proof: no exact output of its own yet */
    dbg->active = true;
    dbg->stereo = stereo;
    dbg->paused = paused;

    if (paused) return;

    /* Step filter envelope */
    float ef_value = voice_step_envelope(
        d, v, NV_PAVS_VOICE_CFG_ENV1, NV_PAVS_VOICE_CFG_ENVF,
        NV_PAVS_VOICE_CFG_MISC, NV_PAVS_VOICE_CFG_MISC_EF_RELEASERATE,
        NV_PAVS_VOICE_PAR_NEXT, NV_PAVS_VOICE_PAR_NEXT_EFLVL,
        NV_PAVS_VOICE_CUR_ECNT_EFCOUNT, NV_PAVS_VOICE_PAR_STATE_EFCUR);
    if (ef_value < 0.0f) ef_value = 0.0f;
    if (ef_value > 1.0f) ef_value = 1.0f;

    int16_t p = (int16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_PITCH_LINK,
                                         NV_PAVS_VOICE_TAR_PITCH_LINK_PITCH);
    int8_t ps = (int8_t)voice_get_mask(d, v, NV_PAVS_VOICE_CFG_ENV0,
                                        NV_PAVS_VOICE_CFG_ENV0_EF_PITCHSCALE);
    float rate = 1.0f / powf(2.0f, (p + ps * 32 * ef_value) / 4096.0f);
    dbg->rate = rate;
    if (bw_mpstat_on()) {
        float r0 = s_bw_prev_rate[v];
        if (r0 > 0.0f && r0 != rate) {
            float st = fabsf(12.0f * log2f(rate / r0));
            s_bw_pitch_changes[v]++;
            if (st > s_bw_pitch_maxstep[v]) s_bw_pitch_maxstep[v] = st;
        }
        s_bw_prev_rate[v] = rate;
    }

    /* Step amplitude envelope */
    float ea_value = voice_step_envelope(
        d, v, NV_PAVS_VOICE_CFG_ENV0, NV_PAVS_VOICE_CFG_ENVA,
        NV_PAVS_VOICE_TAR_LFO_ENV, NV_PAVS_VOICE_TAR_LFO_ENV_EA_RELEASERATE,
        NV_PAVS_VOICE_PAR_OFFSET, NV_PAVS_VOICE_PAR_OFFSET_EALVL,
        NV_PAVS_VOICE_CUR_ECNT_EACOUNT, NV_PAVS_VOICE_PAR_STATE_EACUR);
    if (ea_value < 0.0f) ea_value = 0.0f;
    if (ea_value > 1.0f) ea_value = 1.0f;

    float samples[NUM_SAMPLES_PER_FRAME][2];
    memset(samples, 0, sizeof(samples));

    bool multipass = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                    NV_PAVS_VOICE_CFG_FMT_MULTIPASS) != 0;
    dbg->multipass = multipass;
    if (g_bw_hot)
        s_hot_vflags |= (v < MCPX_HW_MAX_3D_VOICES ? BW_HOT_V_3D : 0) | (stereo ? BW_HOT_V_STEREO : 0) |
                        (multipass ? BW_HOT_V_MP : 0);

    if (multipass) {
        /* Read from multipass bin */
        int mp_bin = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                    NV_PAVS_VOICE_CFG_FMT_MULTIPASS_BIN);
        dbg->multipass_bin = (uint8_t)mp_bin;
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            samples[i][0] = mixbins[mp_bin][i];
            samples[i][1] = mixbins[mp_bin][i];
        }
        bool clear_mix = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                        NV_PAVS_VOICE_CFG_FMT_CLEAR_MIX) != 0;
        if (bw_mpstat_on()) {
            float pk = 0.0f;
            for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++)
                pk = fmaxf(pk, fabsf(mixbins[mp_bin][i]));
            s_bw_mp_reads++;
            s_bw_mp_clears += clear_mix;
            s_bw_mp_empty += pk == 0.0f;
            if (pk > s_bw_mp_peak) s_bw_mp_peak = pk;
            s_bw_mp_lpf[v] = 1;
        }
        if (s_hrtf_proof > 0) {
            /* au-hot's proof: a multipass voice reads a bin; the shadow's would
             * have to be the same for the proof to cover what follows */
            s_hp.mp_reads++;
            if (!bw_f32_bits_eq(mixbins[mp_bin], s_hp_bins[mp_bin], NUM_SAMPLES_PER_FRAME))
                s_hp.mp_incomplete++;
        }
        if (clear_mix) {
            memset(&mixbins[mp_bin][0], 0, sizeof(mixbins[0]));
            if (s_hrtf_proof > 0)
                memset(&s_hp_bins[mp_bin][0], 0, sizeof(s_hp_bins[0]));
        }
    } else {
        int empty = 0;
        int64_t hot_r0 = g_bw_hot ? bw_hot_t() : 0;   /* au-hot: the resampler's share */
        if (hot_r0 && bw_sinc_on())
            s_hot_vflags |= BW_HOT_V_SINC;
        for (int sample_count = 0; sample_count < NUM_SAMPLES_PER_FRAME;) {
            int active = voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                                        NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE);
            if (!active) return;
            int count = bw_sinc_on()
                ? voice_resample_sinc(d, v, &samples[sample_count],
                                      NUM_SAMPLES_PER_FRAME - sample_count, rate)
                : voice_resample(d, v, &samples[sample_count],
                                 NUM_SAMPLES_PER_FRAME - sample_count, rate);
            if (count < 0) break;
            /* An active voice that keeps producing nothing is an underrun,
             * not a reason to spin. Asking it again at once cannot change the
             * answer, and this loop holds d->lock: Burnout 3's vehicle select
             * sat here indefinitely while the title waited in voice_lock. */
            if (count == 0 && ++empty > 4) break;
            sample_count += count;
        }
        if (hot_r0)
            s_hot_vres += bw_hot_t() - hot_r0;
    }

    int active = voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                                NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE);
    if (!active) return;

    /* Get volume bins */
    int bin[8];
    bin[0] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V0BIN);
    bin[1] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V1BIN);
    bin[2] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V2BIN);
    bin[3] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V3BIN);
    bin[4] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V4BIN);
    bin[5] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V5BIN);
    bin[6] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_V6BIN);
    bin[7] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_V7BIN);

    if (v < MCPX_HW_MAX_3D_VOICES) {
        bin[0] = d->vp.hrtf_submix[0];
        bin[1] = d->vp.hrtf_submix[1];
        bin[2] = d->vp.hrtf_submix[2];
        bin[3] = d->vp.hrtf_submix[3];
    }

    uint16_t vol[8];
    vol[0] = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLA, NV_PAVS_VOICE_TAR_VOLA_VOLUME0);
    vol[1] = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLA, NV_PAVS_VOICE_TAR_VOLA_VOLUME1);
    vol[2] = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLB, NV_PAVS_VOICE_TAR_VOLB_VOLUME2);
    vol[3] = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLB, NV_PAVS_VOICE_TAR_VOLB_VOLUME3);
    vol[4] = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLC, NV_PAVS_VOICE_TAR_VOLC_VOLUME4);
    vol[5] = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLC, NV_PAVS_VOICE_TAR_VOLC_VOLUME5);
    vol[6] = (uint16_t)(voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLC, NV_PAVS_VOICE_TAR_VOLC_VOLUME6_B11_8) << 8);
    vol[6] |= (uint16_t)(voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLB, NV_PAVS_VOICE_TAR_VOLB_VOLUME6_B7_4) << 4);
    vol[6] |= (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLA, NV_PAVS_VOICE_TAR_VOLA_VOLUME6_B3_0);
    vol[7] = (uint16_t)(voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLC, NV_PAVS_VOICE_TAR_VOLC_VOLUME7_B11_8) << 8);
    vol[7] |= (uint16_t)(voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLB, NV_PAVS_VOICE_TAR_VOLB_VOLUME7_B7_4) << 4);
    vol[7] |= (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLA, NV_PAVS_VOICE_TAR_VOLA_VOLUME7_B3_0);

    for (int i = 0; i < 8; i++) {
        dbg->bin[i] = (uint8_t)bin[i];
        dbg->vol[i] = vol[i];
    }
    if (bw_mpstat_on()) {
        int changed = 0;
        for (int i = 0; i < 8; i++) {
            uint16_t a = s_bw_prev_vol[v][i] & 0xFFF, b = vol[i] & 0xFFF;
            if (a != b) {
                /* attenuation is in 1/64 dB; FFF is silence, counted as 60 dB */
                float da = a == 0xFFF ? 60.0f : a / 64.0f;
                float db = b == 0xFFF ? 60.0f : b / 64.0f;
                float step = fabsf(da - db) > 60.0f ? 60.0f : fabsf(da - db);
                if (step > s_bw_vol_maxstep[v]) s_bw_vol_maxstep[v] = step;
                changed = 1;
            }
            s_bw_prev_vol[v][i] = vol[i];
        }
        s_bw_vol_changes[v] += changed;
        {
            static int tv = -2;
            static unsigned long long vpf;
            static uint16_t last_v;
            if (tv == -2) {
                const char *e = getenv("BW_APU_VOLTRACE");
                tv = e && *e ? atoi(e) : -1;
            }
            if (v == last_v || last_v == 0) vpf++;   /* rough frame clock */
            last_v = v;
            if (changed && (int)v == tv) {
                fprintf(stderr, "[APU-VOLTRACE] f=%llu v=%u", vpf, (unsigned)v);
                for (int i = 0; i < 8; i++)
                    fprintf(stderr, " %u:%03X", (unsigned)bin[i], (unsigned)vol[i]);
                fprintf(stderr, " ea=%.2f\n", ea_value);
            }
        }
    }
    if (s_vstat_on > 0) {
        s_vstat[v].ea = ea_value;
        s_vstat[v].rate = rate;
        s_vstat[v].vol0 = vol[0]; s_vstat[v].vol1 = vol[1];
        s_vstat[v].bin0 = (uint8_t)bin[0]; s_vstat[v].bin1 = (uint8_t)bin[1];
    }

    if (voice_should_mute(v) || bw_solo_mute(d, v, multipass)) return;

    /* Low-pass filter */
    uint64_t hot_p0 = g_bw_hot ? __rdtsc() : 0, hot_p1 = 0, hot_p2 = 0, hot_p3 = 0;   /* au-hot */
    int fmode = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_MISC,
                               NV_PAVS_VOICE_CFG_MISC_FMODE);
    bw_audit_voice(d, v, stereo, multipass, fmode, rate, bin, vol);
    bool lpf = false;
    if (v < MCPX_HW_MAX_3D_VOICES) {
        lpf = (fmode == 1);
    } else {
        lpf = stereo ? (fmode == 1) : (fmode & 1) != 0;
    }
    if (lpf) {
        for (int ch = 0; ch < 2; ch++) {
            int16_t fc = (int16_t)voice_get_mask(
                d, v, NV_PAVS_VOICE_TAR_FCA + (ch % channels) * 4,
                NV_PAVS_VOICE_TAR_FCA_FC0);
            float fc_f = clampf(powf(2, fc / 4096.0f), 0.003906f, 1.0f);
            uint16_t q = (uint16_t)voice_get_mask(
                d, v, NV_PAVS_VOICE_TAR_FCA + (ch % channels) * 4,
                NV_PAVS_VOICE_TAR_FCA_FC1);
            float q_f = clampf(q / (1.0f * 0x8000), 0.079407f, 1.0f);
            sv_filter *filter = &d->vp.filters[v].svf[ch];
            setup_svf(filter, fc_f, q_f, F_LP);
            for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
                samples[i][ch] = run_svf(filter, samples[i][ch]);
                if (s_bw_mpstat > 0 && fabsf(samples[i][ch]) > 1.0f)
                    s_bw_clamped[v]++;
                samples[i][ch] = fminf(fmaxf(samples[i][ch], -1.0f), 1.0f);
            }
        }
    }

    /* Blood Wake carried patch (APU audit A3, docs/day1/D2.md part 4): a 3D
     * voice in filter mode 3 is I3DL2's. DirectSound packs two low-pass
     * coefficients into TAR_FCB, the direct path's in the low half and the
     * room (reverb send) path's in the high half (seen, DSound_HwVoiceSetFilter
     * 0xFEA34 and DSound_I3DL2ComputeSourceParams 0x10030D), each from the
     * one-pole I3DL2 formula a = (1 - G cos w - sqrt(2G(1 - cos w) -
     * G^2(1 - cos^2 w))) / (1 - G) scaled by 65536 (seen, 0x100196;
     * Blood Wake's 0xCEB4 is 0.807, a -1000 mB room HF at 5 kHz). The toolkit
     * ran no filter in mode 3, and its 3D voices fed their reverb send from
     * the HRTF's first ear. Here the direct path is y += (1 - a)(x - y)
     * before the HRTF, and slots 4-7 (the I3DL2 send and any other non-HRTF
     * slot) take the voice's own signal through the room filter, not an
     * ear (inferred: the HRTF slots are 0-3 and only they have ears).
     * Seen in use: the room coefficient in mission 28's caves only; the direct
     * one is 0 (flat) everywhere.
     * Level: the HRTF path's absolute scale is unknown (A2 keeps the
     * toolkit's average), and the send is kept at the average the toolkit's
     * first ear gave it (-3.7 dB on the game's 3D sounds), so the reverb's
     * average strength does not move; only its dependence on the source's
     * side goes, and the room filter comes in.
     * BW_APU_I3DL2_SEND=<x> sets the send's scale (1 = the voice's signal at
     * unity). BW_APU_I3DL2=toolkit restores the old path. */
    if (hot_p0)
        hot_p1 = __rdtsc();
    static int i3dl2_on = -1;
    static float i3dl2_send = BW_I3DL2_SEND_DEFAULT;
    float bw_room[NUM_SAMPLES_PER_FRAME];
    int bw_room_ok = 0;
    if (i3dl2_on < 0) {
        const char *e = getenv("BW_APU_I3DL2");
        const char *s = getenv("BW_APU_I3DL2_SEND");
        i3dl2_on = !(e && e[0] == 't');
        if (s && *s && atof(s) > 0.0)
            i3dl2_send = (float)atof(s);
        fprintf(stderr, "[APU] BW 3D voices' I3DL2 path: %s (BW_APU_I3DL2)\n",
                i3dl2_on ? "mode 3 filters, send from the voice (APU audit A3)"
                         : "no filter, send from the first ear (the toolkit's)");
    }
    if (i3dl2_on && v < MCPX_HW_MAX_3D_VOICES && channels == 1) {
        float *st = s_bw_i3dl2[v];
        if (fmode == 3) {
            uint32_t fcb = voice_get_mask(d, v, NV_PAVS_VOICE_TAR_FCB, 0xFFFFFFFF);
            float a_dir = (float)(fcb & 0xFFFF) / 65536.0f;
            float a_room = (float)(fcb >> 16) / 65536.0f;
            for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
                float x = samples[i][0];
                st[1] += (1.0f - a_room) * (x - st[1]);
                bw_room[i] = st[1] * i3dl2_send;
                if (a_dir > 0.0f) {
                    st[0] += (1.0f - a_dir) * (x - st[0]);
                    samples[i][0] = samples[i][1] = st[0];
                }
            }
        } else {
            for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++)
                bw_room[i] = samples[i][0] * i3dl2_send;
        }
        bw_room_ok = 1;
    }

    if (hot_p0)
        hot_p2 = __rdtsc();
    /* HRTF processing for 3D voices. RECOMP_APU_HRTF=0 turns it off: it is
     * on by default (xemu's), and so far checked on one title. */
    static int hrtf_on = -1;
    if (hrtf_on < 0) {
        const char *e = getenv("RECOMP_APU_HRTF");
        hrtf_on = !(e && e[0] == '0');
    }
    int bw_ears = 0;    /* Blood Wake (TK-18): the HRTF made a left and a right ear */
    if (v < MCPX_HW_MAX_3D_VOICES && g_config.audio.hrtf && hrtf_on) {
        uint16_t hrtf_handle =
            (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_CFG_HRTF_TARGET,
                                     NV_PAVS_VOICE_CFG_HRTF_TARGET_HANDLE);
        if (hrtf_handle != HRTF_NULL_HANDLE) {
            float cal[NUM_SAMPLES_PER_FRAME][2];
            double w = 0.0;
            if (bw_cal_on()) {
                /* weighted by the voice's front-slot gains: loud sounds count */
                w = (double)attenuate(vol[0]) * attenuate(vol[0])
                  + (double)attenuate(vol[2]) * attenuate(vol[2]);
                for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++)
                    s_bw_cal_in += w * samples[i][0] * samples[i][0];
                hrtf_filter_process(&s_bw_cal_f[v], samples, cal);
            }
            /* au-hot: flush-to-zero around the filter (BW_APU_HRTF_FTZ) */
            bw_hrtf_run(&d->vp.filters[v].hrtf, v, samples);
            if (g_bw_hot)
                s_hot_vflags |= BW_HOT_V_HRTF;   /* au-hot */
            if (bw_cal_on())
                for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
                    s_bw_cal_tk[0] += w * cal[i][0] * cal[i][0];
                    s_bw_cal_tk[1] += w * cal[i][1] * cal[i][1];
                    s_bw_cal_fx += w * 0.5 * (samples[i][0] * samples[i][0]
                                              + samples[i][1] * samples[i][1]);
                }
            bw_ears = 1;
        }
    }
    static int ears_mode = -1;          /* 0 one ear, 1 ears, 2 ears mirrored */
    {
        if (ears_mode < 0) {
            /* TK-18, a port fix in every edition (docs/day1/D2.md part 3):
             * the HRTF's two ears go to their own 3D slots. Which filter
             * channel is which ear was measured, not assumed: with filter
             * channel 0 in the left slots, sources on the player's left came
             * out louder on the right (lateral position vs L/R balance,
             * correlation -0.26 to -0.82, run 20261027-033644-r6-gpu-D2-
             * earsdir), so channel 1 is the left ear. BW_APU_HRTF_EARS=0 is
             * the toolkit's one ear to every slot (xemu's too); =mirror the
             * first, mirrored mapping. */
            const char *e = getenv("BW_APU_HRTF_EARS");
            ears_mode = 1;
            if (e && *e)
                ears_mode = e[0] == '0' ? 0 : e[0] == 'm' ? 2 : 1;
            fprintf(stderr, "[APU] BW 3D voices: %s (BW_APU_HRTF_EARS)\n",
                    ears_mode == 1 ? "left and right ears to their own slots (TK-18)"
                    : ears_mode == 2 ? "ears mirrored (test)"
                    : "one ear to every 3D slot (the toolkit's)");
        }
        if (!ears_mode)
            bw_ears = 0;
    }

    if (hot_p0)
        hot_p3 = __rdtsc();
    /* Mix into bins */
    float bw_lat_g = 0.0f;              /* Blood Wake (lat-a): the loudest slot's gain */
    for (int b = 0; b < 8; b++) {
        float g = ea_value;
        float hr;
        if ((v < MCPX_HW_MAX_3D_VOICES) && (b < 4)) {
            hr = (float)(1 << d->vp.hrtf_headroom);
        } else {
            hr = (float)(1 << d->vp.submix_headroom[bin[b]]);
        }
        g *= attenuate(vol[b]) / hr;
        if (g_bw_lat_on && g > bw_lat_g)
            bw_lat_g = g;
        /* The volumes are target registers: the hardware moves the level
         * toward them rather than jumping. Stepping to each frame's level
         * every 32 samples turned a voice whose volume the title changes
         * every frame (an engine that follows the revs) into a buzz at the
         * frame rate. Ramp from the gain the last frame ended on. */
        MCPXAPUVoiceFilter *f = &d->vp.filters[v];
        float g0 = f->gain_valid ? f->gain_prev[b] : g;
        float step = (g - g0) / NUM_SAMPLES_PER_FRAME;
        f->gain_prev[b] = g;
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            /* TK-18 (Blood Wake carried patch): a mono 3D voice's HRTF
             * output is two ears, but b % channels read the left one into
             * every bin, so 3D effects sat in the middle. Its first four
             * slots are the 3D submix bins (SET_HRTF_SUBMIXES; Blood Wake:
             * 6, 8, 7, 9 -- DirectSound's XTLK front-left, back-left,
             * front-right, back-right), so slots 0-1 take the left ear and
             * 2-3 the right (the bins: seen, the EP folds 6 into the left;
             * the filter's channel 1 is the left ear: measured, above). */
            int ch = (bw_ears && b < 4)
                ? ((b < 2) == (ears_mode == 1) ? 1 : 0)
                : (int)(b % channels);
            /* APU audit A3: a 3D voice's non-HRTF slots take its own
             * (room-filtered) signal, not an ear */
            float x = (bw_room_ok && b >= 4) ? bw_room[i] : samples[i][ch];
            mixbins[bin[b]][i] += (g0 + step * (i + 1)) * x;
            if (s_hrtf_proof > 0) {
                /* au-hot's proof: the same mix from the exact path's output */
                float xe = (bw_room_ok && b >= 4) ? bw_room[i] : s_hp_cur ? s_hp_out[i][ch] : samples[i][ch];
                s_hp_bins[bin[b]][i] += (g0 + step * (i + 1)) * xe;
            }
        }
    }
    d->vp.filters[v].gain_valid = true;
    if (hot_p0) {
        /* au-hot: the parts, and a census of the voice's float state */
        uint64_t hot_p4 = __rdtsc();
        int64_t part[BW_HOT_NPART];
        int den[BW_HOT_NDEN];
        memset(den, 0, sizeof den);
        part[BW_HOT_P_LPF] = (int64_t)(hot_p1 - hot_p0);
        part[BW_HOT_P_I3DL2] = (int64_t)(hot_p2 - hot_p1);
        part[BW_HOT_P_HRTF] = (int64_t)(hot_p3 - hot_p2);
        part[BW_HOT_P_MIX] = (int64_t)(hot_p4 - hot_p3);
        if (v < MCPX_HW_MAX_3D_VOICES) {
            HrtfFilter *hf = &d->vp.filters[v].hrtf;
            for (int c = 0; c < 2; c++) {
                for (int k = 0; k < HRTF_NUM_TAPS; k++)
                    den[BW_HOT_D_COEFF] += bw_hot_isden(hf->ch[c].hrir_coeff_cur[k]);
                for (int k = 0; k < HRTF_BUFLEN; k++)
                    den[BW_HOT_D_HIST] += bw_hot_isden(hf->ch[c].buf[k]);
            }
            den[BW_HOT_D_ITD] = bw_hot_isden(hf->itd_cur);
            den[BW_HOT_D_ROOM] = bw_hot_isden(s_bw_i3dl2[v][0]) + bw_hot_isden(s_bw_i3dl2[v][1]);
        }
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++)
            den[BW_HOT_D_OUT] += bw_hot_isden(samples[i][0]) + bw_hot_isden(samples[i][1]);
        if (s_bw_sinc && d->ep_frame_div % 8 == 0)
            for (int i = 0; i < s_bw_sinc[v].n; i++)
                den[BW_HOT_D_SINC] += bw_hot_isden(s_bw_sinc[v].hist[i][0]) +
                                      bw_hot_isden(s_bw_sinc[v].hist[i][1]);
        bw_hot_voice_parts(part, den);
    }
    if (g_bw_lat_on && s_lat_vs[v])
        bw_lat_voice_frame(d, v, samples, bw_lat_g);   /* Blood Wake (lat-a), test aid */

    /* VP monitor mix */
    if (d->monitor.point == MCPX_APU_DEBUG_MON_VP) {
        float g = 0.0f;
        for (int b = 0; b < 8; b++) {
            float hr = (float)(1 << d->vp.submix_headroom[bin[b]]);
            float bg = attenuate(vol[b]) / hr;
            if (bg > g) g = bg;
        }
        g *= ea_value;
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            sample_buf[i][0] += g * samples[i][0];
            sample_buf[i][1] += g * samples[i][1];
        }
    }

    (void)voice_list;
}

/* ============================================================
 * VP Frame - Process all voice lists
 *
 * Simplified single-threaded version (no worker threads initially)
 * ============================================================ */

/* RECOMP_APU_TRACE: every 60 VP frames, each active voice's play offset and
 * end offset, in samples (CBO and EBO as the VP holds them). A title that
 * times video or game events off a voice's play position follows this
 * clock; set against the flip count it shows whether audio runs fast or
 * slow relative to the frames. */
static unsigned s_pos_frames;

static void vp_trace_positions(MCPXAPUState *d)
{
    if (s_vstat_on < 0)
        vp_stat_init();
    if (s_vstat_on <= 0 || ++s_pos_frames % 60)
        return;

    for (int list = 0; list < 3; list++) {
        uint32_t h = d->regs[voice_list_regs[list].top];
        for (int i = 0; h != 0xFFFF && i < MCPX_HW_MAX_VOICES; i++) {
            uint16_t v = (uint16_t)h;
            if (voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                               NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE))
                fprintf(stderr, "[APU] voice %u pos=%u/%u\n", (unsigned)v,
                        voice_get_mask(d, v, NV_PAVS_VOICE_PAR_OFFSET,
                                       NV_PAVS_VOICE_PAR_OFFSET_CBO),
                        voice_get_mask(d, v, NV_PAVS_VOICE_PAR_NEXT,
                                       NV_PAVS_VOICE_PAR_NEXT_EBO));
            h = voice_get_mask(d, v, NV_PAVS_VOICE_TAR_PITCH_LINK,
                               NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE);
        }
    }
}

void mcpx_apu_vp_frame(MCPXAPUState *d,
                        float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    uint8_t bwd3_reached[MCPX_HW_MAX_VOICES];   /* D3 diagnostics: orphan check */
    memset(bwd3_reached, 0, sizeof bwd3_reached);
    bw_audit_frame();                    /* Blood Wake: BW_APU_AUDIT */
    memset(d->vp.sample_buf, 0, sizeof(d->vp.sample_buf));
    bw_hrtf_ftz();                       /* au-hot: the switches, read once */
    if (s_hrtf_proof > 0)
        bw_hrtf_proof_begin(mixbins);

    for (int list = 0; list < 3; list++) {
        hwaddr top, current, next;
        top = voice_list_regs[list].top;
        current = voice_list_regs[list].current;
        next = voice_list_regs[list].next;

        d->regs[current] = d->regs[top];

        for (int i = 0; d->regs[current] != 0xFFFF; i++) {
            if (i >= MCPX_HW_MAX_VOICES) {
                DPRINTF("Voice list contains invalid entry!\n");
                break;
            }

            uint16_t v = (uint16_t)d->regs[current];
            if (v < MCPX_HW_MAX_VOICES)
                bwd3_reached[v] = 1;
            d->regs[next] = voice_get_mask(d, v, NV_PAVS_VOICE_TAR_PITCH_LINK,
                               NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE);

            if (!voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                                NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE)) {
                /* One trap at a time: once the front end is trapped, a second
                 * idle voice in the same frame would overwrite FEDECPARAM
                 * before the ISR has read the first. The rest trap on later
                 * frames, after the ISR has resumed the front end. */
                if ((d->regs[NV_PAPU_FECTL] & NV_PAPU_FECTL_FEMETHMODE)
                        != NV_PAPU_FECTL_FEMETHMODE_TRAPPED)
                    fe_method(d, SE2FE_IDLE_VOICE, v);
            } else {
                /* Process voice directly (single-threaded) */
                voice_spin_lock(d, v);
                if (g_bw_hot) {
                    /* au-hot (BW_APU_HOTSTAT): the same call, timed */
                    int64_t h0 = bw_hot_t();
                    s_hot_vflags = s_hot_taps = 0;
                    s_hot_vres = 0;
                    voice_process(d, mixbins, d->vp.sample_buf, v, list);
                    bw_hot_voice(s_hot_vflags, bw_hot_t() - h0, s_hot_vres, s_hot_taps);
                } else {
                voice_process(d, mixbins, d->vp.sample_buf, v, list);
                }
                voice_spin_unlock(d, v);
            }
            d->regs[current] = d->regs[next];
        }
    }
    bwd3_orphan_check(d, bwd3_reached);
    if (s_hrtf_proof > 0)
        bw_hrtf_proof_end(d, mixbins);   /* au-hot: the bins against the exact path's */
    bw_audit_bins(mixbins);              /* Blood Wake: BW_APU_AUDIT */
    if (g_bw_hot) {
        int n = 0;                       /* au-hot: denormals in the bins */
        for (int b = 0; b < NUM_MIXBINS; b++)
            for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++)
                n += bw_hot_isden(mixbins[b][i]);
        bw_hot_bins_den(n);
    }

    vp_trace_positions(d);
    vp_stat_report();
    bw_mpstat_frame();
    if (s_ring_on > 0 && ++s_ring_frames % 1500 == 0)
        vp_ring_report(++s_ring_secs);

    /* VP monitor output */
    if (d->monitor.point == MCPX_APU_DEBUG_MON_VP) {
        int16_t isamp[NUM_SAMPLES_PER_FRAME * 2];
        src_float_to_short_array((float *)d->vp.sample_buf, isamp,
                                 NUM_SAMPLES_PER_FRAME * 2);
        int off = (d->ep_frame_div % 8) * NUM_SAMPLES_PER_FRAME;
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            d->monitor.frame_buf[off + i][0] += isamp[2 * i];
            d->monitor.frame_buf[off + i][1] += isamp[2 * i + 1];
        }
        memset(d->vp.sample_buf, 0, sizeof(d->vp.sample_buf));
        memset(mixbins, 0, sizeof(float) * NUM_MIXBINS * NUM_SAMPLES_PER_FRAME);
    }
}

/* ============================================================
 * VP Init / Finalize / Reset
 * ============================================================ */

void mcpx_apu_vp_init(MCPXAPUState *d)
{
    /* Single-threaded - no worker dispatch needed */
    (void)d;
}

/* Shutdown, after the frame thread has stopped: close the voice dumps, free
 * the ring monitors and clear the statistics, so a new instance reads the
 * switches again and starts from nothing. */
void mcpx_apu_vp_finalize(MCPXAPUState *d)
{
    (void)d;
    if (s_vdump)
        fclose(s_vdump);
    s_vdump = NULL;
    for (unsigned v = 0; v < MCPX_HW_MAX_VOICES; v++) {
        if (s_vdump_all[v])
            fclose(s_vdump_all[v]);
        s_vdump_all[v] = NULL;
        free(s_ring[v]);
        s_ring[v] = NULL;
    }
    memset(s_vdump_failed, 0, sizeof(s_vdump_failed));
    memset(s_vstat, 0, sizeof(s_vstat));
    s_vstat_on = -1;
    s_vdump_voice = -1;
    s_vstat_frames = s_vstat_secs = s_pos_frames = 0;
    s_ring_on = -1;
    s_ring_logged = 0;
    s_ring_frames = s_ring_secs = 0;
}

void mcpx_apu_vp_reset(MCPXAPUState *d)
{
    d->vp.ssl_base_page = 0;
    d->vp.hrtf_headroom = 0;
    memset(d->vp.ssl, 0, sizeof(d->vp.ssl));
    memset(d->vp.hrtf_submix, 0, sizeof(d->vp.hrtf_submix));
    memset(d->vp.submix_headroom, 0, sizeof(d->vp.submix_headroom));
    memset((void *)d->vp.voice_locked, 0, sizeof(d->vp.voice_locked));
    for (int v = 0; v < MCPX_HW_MAX_VOICES; v++) {
        hrtf_filter_init(&d->vp.filters[v].hrtf);
    }
    memset(s_hp_f, 0, sizeof s_hp_f);   /* au-hot's proof: the shadow filters as well */
}
