/*
 * bw_apu_out.c - the audio output's helpers (lane au-out of the audio rework,
 * docs/audio/out.md). Blood Wake file, compiled into the toolkit's xbox_apu
 * library beside the carried apu_xaudio2.c and bw_apu_wasapi.c.
 *
 * - MMCSS for the sound threads (every edition since 0.3.0): the APU frame thread,
 *   which makes every period and paces the device queue, and the DSP thread
 *   (bw_apu_xdsp.c) join Windows' multimedia class scheduler, so a busy PC
 *   delays them less and the queue can be smaller with no glitch.
 * - The output worker: a lost or changed device is opened here, off the
 *   frame thread, and swapped in at a period boundary (apu_xaudio2.c).
 * - One line that names an output device in full, with its mix format.
 */
#define COBJMACROS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <wchar.h>
#include <wctype.h>
#include "../../bw_edition.h"
#include "bw_apu_out.h"

#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "ole32.lib")

/* ---- MMCSS ----------------------------------------------------------------
 *
 * BW_AUDIO_MMCSS = pro ("Pro Audio": MMCSS's high category, the realtime
 * band while the thread uses less than the share the system keeps back for
 * everything else) | audio ("Audio": the medium category) | 0 (off: the
 * threads keep THREAD_PRIORITY_HIGHEST, as before). Every edition since
 * 0.3.0, either output (lane au-orig, docs/audio/orig.md: Original takes the
 * same output path as Remastered; host-side scheduling only, nothing in the
 * game changes). "Audio" by default (measured,
 * docs/audio/out.md 5): event-driven at a floor of one engine period, no
 * glitch with or without it on a quiet PC or beside two other games; with
 * all three pinned to 4 cores, one short pass in 5 runs without it and none
 * in 5 with "Audio" (none in 2 with "Pro Audio"), whose slack before each
 * pass was also the steadiest. ("Audio" is MMCSS's medium category: above
 * every normal thread, below pro-audio software's "Pro Audio".) */
#define BW_MMCSS_DEFAULT 'a'

/* au-hot (docs/audio/hot.md): BW_AUDIO_HIGHQOS, the sound thread opts out
 * of Windows 11's execution-speed throttling (its "high QoS"), so a game
 * whose window isn't in front keeps its sound threads off the efficiency
 * cores, where the voice processor runs 1.75 times slower (seen: the
 * resampler 5.9 against 3.4 ns a tap). Scheduling only: nothing it makes
 * changes. Any edition. On by default since 0.3.1 (call QOS-1, au-vp:
 * the launcher's High Priority Mode); BW_AUDIO_HIGHQOS=0: left to Windows,
 * as before (the undo, and the mode off). */
static void bw_audio_thread_highqos(const char *who)
{
    static int on = -1;
    THREAD_POWER_THROTTLING_STATE st;
    if (on < 0) {
        const char *e = getenv("BW_AUDIO_HIGHQOS");
        on = !(e && e[0] == '0');
    }
    if (!on)
        return;
    memset(&st, 0, sizeof st);
    st.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
    st.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    st.StateMask = 0;                    /* the control, with the throttling off: high QoS */
    if (SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &st, sizeof st))
        fprintf(stderr, "[APU] BW the %s thread: high QoS, no execution-speed throttling (BW_AUDIO_HIGHQOS;"
                        " =0 leaves it to Windows)\n", who);
    else
        fprintf(stderr, "[APU] BW the %s thread: high QoS refused (error %lu)\n", who, GetLastError());
}

void bw_audio_thread_mmcss(const char *who)
{
    static int mode = -1;
    const wchar_t *task;
    const char *name;
    DWORD idx = 0;
    HANDLE h;
    bw_proc_qos();                       /* au-vp: the process's power throttling, once (BW_PROC_QOS) */
    bw_audio_thread_highqos(who);        /* au-hot: before MMCSS's edition check (a test switch) */
    if (mode < 0) {
        const char *e = getenv("BW_AUDIO_MMCSS");
        mode = (e && *e) ? e[0] : BW_MMCSS_DEFAULT;
        if (mode == 'P' || mode == 'A')
            mode += 'a' - 'A';
    }
    if (mode == 'p') {
        task = L"Pro Audio";
        name = "Pro Audio";
    } else if (mode == 'a') {
        task = L"Audio";
        name = "Audio";
    } else {
        return;
    }
    h = AvSetMmThreadCharacteristicsW(task, &idx);
    if (h)
        fprintf(stderr, "[APU] BW the %s thread joins MMCSS \"%s\" (every edition; BW_AUDIO_MMCSS=pro|audio|0)\n",
                who, name);
    else
        fprintf(stderr, "[APU] BW the %s thread: MMCSS \"%s\" refused (error %lu); it keeps its priority\n",
                who, name, GetLastError());
}

/* ---- the output worker -------------------------------------------------- */

static HANDLE s_job_ev;
static long (*s_job_fn)(void *);
static void *s_job_arg;
static volatile LONG s_job_state;          /* 0 free, 1 posted or running, 2 finished */
static long s_job_result;
static double s_job_ms;

static DWORD WINAPI out_worker(LPVOID arg)
{
    LARGE_INTEGER f, a, b;
    (void)arg;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    QueryPerformanceFrequency(&f);
    for (;;) {
        WaitForSingleObject(s_job_ev, INFINITE);
        if (InterlockedCompareExchange(&s_job_state, 1, 1) != 1)
            continue;
        QueryPerformanceCounter(&a);
        s_job_result = s_job_fn(s_job_arg);
        QueryPerformanceCounter(&b);
        s_job_ms = (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart;
        InterlockedExchange(&s_job_state, 2);
    }
}

int bw_out_post(long (*fn)(void *), void *arg)
{
    static HANDLE th;
    if (InterlockedCompareExchange(&s_job_state, 0, 0) != 0)
        return 0;
    if (!th) {
        s_job_ev = CreateEventW(NULL, FALSE, FALSE, NULL);
        th = s_job_ev ? CreateThread(NULL, 0, out_worker, NULL, 0, NULL) : NULL;
        if (!th)
            return 0;
    }
    s_job_fn = fn;
    s_job_arg = arg;
    InterlockedExchange(&s_job_state, 1);
    SetEvent(s_job_ev);
    return 1;
}

int bw_out_poll(long *result, double *ms)
{
    if (InterlockedCompareExchange(&s_job_state, 2, 2) != 2)
        return 0;
    if (result)
        *result = s_job_result;
    if (ms)
        *ms = s_job_ms;
    InterlockedExchange(&s_job_state, 0);
    return 1;
}

int bw_out_busy(void)
{
    return InterlockedCompareExchange(&s_job_state, 0, 0) == 1;
}

/* ---- the game's running steps (lane au-lat, docs/audio/lat.md) -----------
 *
 * The WASAPI output's boot margin is held only while the game can stall
 * (the carried apu_core.c, bw_queue_adapt): it goes once the game has run its
 * steps smoothly for a few seconds, and comes back when they stop (a load, a
 * menu, a pause, a mission's end). The game thread notes each running step
 * here (src/bw_hifps_hooks.c, Overlays_Update's wrapper, every edition; a
 * host-side time and count, nothing in the game changes); the APU frame
 * thread reads the time since the last one. */
static volatile LONG64 s_step_qpc;
static volatile LONG s_steps;

void bw_audio_note_step(void)
{
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    InterlockedExchange64(&s_step_qpc, q.QuadPart);
    InterlockedIncrement(&s_steps);
}

long long bw_audio_since_step_us(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER q;
    LONG64 t = InterlockedCompareExchange64(&s_step_qpc, 0, 0);
    if (!t)
        return -1;
    if (!f.QuadPart)
        QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&q);
    return q.QuadPart > t ? (long long)((q.QuadPart - t) * 1000000.0 / (double)f.QuadPart) : 0;
}

long bw_audio_steps(void)
{
    return InterlockedCompareExchange(&s_steps, 0, 0);
}

/* ---- one line per device ------------------------------------------------ */

static const GUID k_clsid_mmde = { 0xBCDE0395, 0xE52F, 0x467C,
                                   { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const GUID k_iid_mmde   = { 0xA95664D2, 0x9614, 0x4F35,
                                   { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const GUID k_iid_ac     = { 0x1CB9AD4C, 0xDBFA, 0x4C32,
                                   { 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2 } };
static const GUID k_iid_ac3    = { 0x7ED4EE07, 0x8E67, 0x4CD4,
                                   { 0x8C, 0x1A, 0x2B, 0x7A, 0x59, 0x87, 0xAD, 0x42 } };
static const GUID k_float      = { 0x00000003, 0x0000, 0x0010,
                                   { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
/* PKEY_Device_FriendlyName: Windows' full name ("Speakers (Realtek(R) Audio)") */
static const PROPERTYKEY k_name = { { 0xA45C254E, 0xDF1C, 0x4EFD,
                                      { 0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0 } }, 14 };

/* BW_TEST_AUDIO_DEVICE=<part of a name> (a test switch, any edition; never in
 * a preset): every open takes the active output whose Windows name contains
 * it (any case) instead of Windows' default, so a second device can be
 * measured without changing Windows' default. NULL when unset or not found
 * (the caller then takes the default); an IMMDevice * the caller releases. */
static int name_has(const wchar_t *name, const char *part)
{
    size_t i, j, n = wcslen(name), m = strlen(part);
    for (i = 0; m && i + m <= n; i++) {
        for (j = 0; j < m; j++)
            if (towlower(name[i + j]) != (wchar_t)tolower((unsigned char)part[j]))
                break;
        if (j == m)
            return 1;
    }
    return 0;
}

void *bw_out_test_device(void)
{
    const char *want = getenv("BW_TEST_AUDIO_DEVICE");
    IMMDeviceEnumerator *en = NULL;
    IMMDeviceCollection *col = NULL;
    IMMDevice *hit = NULL;
    UINT i, cnt = 0;
    static int said;
    if (!want || !*want)
        return NULL;
    if (FAILED(CoCreateInstance(&k_clsid_mmde, NULL, CLSCTX_ALL, &k_iid_mmde, (void **)&en)))
        return NULL;
    if (SUCCEEDED(IMMDeviceEnumerator_EnumAudioEndpoints(en, eRender, DEVICE_STATE_ACTIVE, &col))) {
        IMMDeviceCollection_GetCount(col, &cnt);
        for (i = 0; i < cnt && !hit; i++) {
            IMMDevice *d = NULL;
            IPropertyStore *ps = NULL;
            if (FAILED(IMMDeviceCollection_Item(col, i, &d)))
                continue;
            if (SUCCEEDED(IMMDevice_OpenPropertyStore(d, STGM_READ, &ps))) {
                PROPVARIANT pv;
                PropVariantInit(&pv);
                if (SUCCEEDED(IPropertyStore_GetValue(ps, &k_name, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal
                        && name_has(pv.pwszVal, want))
                    hit = d;
                PropVariantClear(&pv);
                IPropertyStore_Release(ps);
            }
            if (d != hit)
                IMMDevice_Release(d);
        }
        IMMDeviceCollection_Release(col);
    }
    IMMDeviceEnumerator_Release(en);
    if (!said++)
        fprintf(stderr, "[XA2] test: BW_TEST_AUDIO_DEVICE=%s: %s\n", want,
                hit ? "that output is opened, not Windows' default" : "no active output by that name: Windows'"
                " default instead");
    return hit;
}

/* The same device as XAudio2 names it (its interface path), for
 * CreateMasteringVoice; 0 if none. */
int bw_out_test_device_xa2(wchar_t *buf, size_t n)
{
    IMMDevice *d = (IMMDevice *)bw_out_test_device();
    LPWSTR id = NULL;
    int ok = 0;
    if (!d)
        return 0;
    if (SUCCEEDED(IMMDevice_GetId(d, &id)) && id) {
        _snwprintf(buf, n, L"\\\\?\\SWD#MMDEVAPI#%s#{e6327cad-dcec-4949-ae8a-991e976a79d2}", id);
        buf[n - 1] = 0;
        CoTaskMemFree(id);
        ok = 1;
    }
    IMMDevice_Release(d);
    return ok;
}

int bw_out_describe(void *devp, char *buf, size_t n)
{
    IMMDevice *dev = (IMMDevice *)devp;
    IMMDeviceEnumerator *en = NULL;
    IPropertyStore *ps = NULL;
    IAudioClient *ac = NULL;
    WAVEFORMATEX *mix = NULL;
    char name[400] = "?", per[120] = "";
    int own = 0;
    if (!buf || !n)
        return 0;
    buf[0] = 0;
    if (!dev && (dev = (IMMDevice *)bw_out_test_device()) != NULL)
        own = 1;                         /* BW_TEST_AUDIO_DEVICE's, as XAudio2 opens it */
    if (!dev) {
        if (FAILED(CoCreateInstance(&k_clsid_mmde, NULL, CLSCTX_ALL, &k_iid_mmde, (void **)&en)))
            return 0;
        if (FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev)))
            dev = NULL;
        IMMDeviceEnumerator_Release(en);
        if (!dev)
            return 0;
        own = 1;
    }
    if (SUCCEEDED(IMMDevice_OpenPropertyStore(dev, STGM_READ, &ps))) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(IPropertyStore_GetValue(ps, &k_name, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal)
            WideCharToMultiByte(CP_UTF8, 0, pv.pwszVal, -1, name, (int)sizeof name, NULL, NULL);
        PropVariantClear(&pv);
        IPropertyStore_Release(ps);
    }
    if (SUCCEEDED(IMMDevice_Activate(dev, &k_iid_ac, CLSCTX_ALL, NULL, (void **)&ac))) {
        IAudioClient3 *ac3 = NULL;
        IAudioClient_GetMixFormat(ac, &mix);
        /* the shared engine's periods the device offers (read only) */
        if (mix && SUCCEEDED(IAudioClient_QueryInterface(ac, &k_iid_ac3, (void **)&ac3))) {
            UINT32 d = 0, f = 0, mn = 0, mx = 0;
            if (SUCCEEDED(IAudioClient3_GetSharedModeEnginePeriod(ac3, mix, &d, &f, &mn, &mx)))
                snprintf(per, sizeof per, "; engine periods default %u, fundamental %u, %u..%u frames", d, f,
                         mn, mx);
            IAudioClient3_Release(ac3);
        }
        IAudioClient_Release(ac);
    }
    if (mix) {
        int fl = mix->wFormatTag == 3 /* WAVE_FORMAT_IEEE_FLOAT */;
        unsigned long mask = 0;
        if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix->cbSize >= 22) {
            const WAVEFORMATEXTENSIBLE *x = (const WAVEFORMATEXTENSIBLE *)mix;
            fl = IsEqualGUID(&x->SubFormat, &k_float);
            mask = x->dwChannelMask;
        }
        snprintf(buf, n, "\"%s\": mix %lu Hz %u ch %u-bit %s (mask 0x%lX)%s", name,
                 (unsigned long)mix->nSamplesPerSec, mix->nChannels, mix->wBitsPerSample,
                 fl ? "float" : "int", mask, per);
        CoTaskMemFree(mix);
    } else {
        snprintf(buf, n, "\"%s\": mix format unknown", name);
    }
    if (own)
        IMMDevice_Release(dev);
    return 1;
}

/* ==== lane au-vp (docs/audio/vp.md): the stall's fixes ======================
 *
 * 1. The WAV capture off the frame thread (a test aid's fix).
 *    RECOMP_AUDIO_WAV (the harness's audio.wav) was written by the APU frame
 *    thread itself, under the APU lock: an fwrite the C library turns into a
 *    WriteFile every ~21 ms of sound, and the header rewritten every second.
 *    When the disk stalls, the frame thread stands still and the device runs
 *    dry (seen with BW_APU_HOTSTAT's watch: 1,080 and 969 ms inside WriteFile,
 *    two runs at the same instant, both short-passing). Now the frame thread
 *    copies each period into a ring (16 s of sound) and a writer thread of its
 *    own hands the same samples, in the same order, to the toolkit's
 *    apu_wav_write. The same bytes: an FNV-1a hash of what the frame thread
 *    handed over and of what the writer wrote (both up to the file's cap),
 *    printed when the file closes ([AUDIO-WAV] ... hashes). The frame thread
 *    waits only when the ring is full (a 16 s disk stall); after the close it
 *    drops, as apu_wav_write does once the file is closed.
 *    BW_AUDIO_WAV_ASYNC=0: written by the frame thread, as before (the undo).
 *
 * 2. The process's quality of service: BW_PROC_QOS (on by default since 0.3.1,
 *    call QOS-1: the launcher's High Priority Mode, which sets =0 when off;
 *    0.3.0's fix is the vblank ack's high-resolution steps in kernel_bridge.c
 *    and is independent of it; in 0.3.0 this was opt-in, =1).
 *    Windows 11 may run a process whose window is hidden, minimized or behind
 *    another at reduced speed (EcoQoS: execution-speed throttling), and may
 *    ignore its timer resolution request (timeBeginPeriod(1)) then. The sound
 *    threads are MMCSS's, but the game's own thread is not, and the APU's
 *    interrupt runs on it (the one-CPU dispatch gate posts it to the gate's
 *    holder). While that interrupt waits, a trapped front end makes the frame
 *    thread produce nothing, and at a 10 ms queue a wait of more than about
 *    8.5 ms is a short pass (seen on a quiet PC in rc4's trials: the posted
 *    interrupt's longest wait 10.61 ms in the run with the short pass, 5-9 ms
 *    in most others; vblank acks up to 8 ms late; on a busy PC 0.4 ms and
 *    never late). The fix opts the whole process out of both: ControlMask
 *    EXECUTION_SPEED | IGNORE_TIMER_RESOLUTION with StateMask 0 ("always high
 *    QoS", "always honour the timer resolution"). Scheduling only: nothing the
 *    game computes changes. =0 leaves both to Windows (the undo).
 *    BW_TEST_ECOQOS=1 (a test switch): the opposite, EcoQoS forced on the
 *    process, to reproduce a throttled game on demand.
 */
#include "apu_wav.h"

#define BW_WAV_RING (48000 * 16)            /* frames: 16 s */
static int16_t *s_wv_buf;
static volatile LONG64 s_wv_w, s_wv_r;      /* frames into / out of the ring, ever */
static HANDLE s_wv_th;
static volatile LONG s_wv_mode = -1;        /* 1 the writer thread, 0 the frame thread */
static volatile LONG s_wv_closed, s_wv_stop, s_wv_full;
static SRWLOCK s_wv_plock = SRWLOCK_INIT;   /* producers only (the writer never takes it) */
static unsigned long long s_wv_cap;         /* RECOMP_AUDIO_WAV_SECS in frames, 0 none */
static unsigned long long s_wv_hin = 14695981039346656037ull, s_wv_hout = 14695981039346656037ull;
static unsigned long long s_wv_nin, s_wv_nout;

/* FNV-1a over the samples as the file's little-endian bytes, up to the cap */
static unsigned long long bw_wv_hash(unsigned long long h, unsigned long long *done, const int16_t *s, int n)
{
    int i;
    if (s_wv_cap && *done + (unsigned long long)n > s_wv_cap)
        n = *done >= s_wv_cap ? 0 : (int)(s_wv_cap - *done);
    for (i = 0; i < n * 2; i++) {
        uint16_t v = (uint16_t)s[i];
        h = (h ^ (v & 0xFF)) * 1099511628211ull;
        h = (h ^ (v >> 8)) * 1099511628211ull;
    }
    *done += (unsigned long long)n;
    return h;
}

static DWORD WINAPI bw_wv_writer(LPVOID arg)
{
    (void)arg;
    for (;;) {
        int stop = s_wv_stop != 0;
        for (;;) {
            LONG64 r = s_wv_r, w = InterlockedCompareExchange64(&s_wv_w, 0, 0);
            LONG64 start = r % BW_WAV_RING, n = w - r;
            if (n <= 0)
                break;
            if (n > BW_WAV_RING - start)
                n = BW_WAV_RING - start;
            if (n > 4800)
                n = 4800;
            apu_wav_write(s_wv_buf + 2 * start, (int)n);
            s_wv_hout = bw_wv_hash(s_wv_hout, &s_wv_nout, s_wv_buf + 2 * start, (int)n);
            InterlockedExchange64(&s_wv_r, r + n);
        }
        if (stop)
            return 0;
        Sleep(10);
    }
}

static int bw_wv_mode(void)
{
    if (s_wv_mode < 0) {
        const char *e = getenv("BW_AUDIO_WAV_ASYNC");
        const char *c = getenv("RECOMP_AUDIO_WAV_SECS");
        int m = !(e && e[0] == '0');
        if (c && atof(c) > 0)
            s_wv_cap = (unsigned long long)(atof(c) * 48000 + 0.5);   /* apu_wav_open's own rounding */
        if (m) {
            s_wv_buf = (int16_t *)malloc(sizeof(int16_t) * 2 * BW_WAV_RING);
            s_wv_th = s_wv_buf ? CreateThread(NULL, 0, bw_wv_writer, NULL, 0, NULL) : NULL;
            if (!s_wv_th)
                m = 0;
        }
        fprintf(stderr, "[AUDIO-WAV] written by %s (BW_AUDIO_WAV_ASYNC)\n",
                m ? "a writer thread of its own, from a 16 s ring (au-vp; the same bytes)"
                  : "the APU frame thread (as before)");
        InterlockedExchange(&s_wv_mode, m);
    }
    return s_wv_mode;
}

void bw_wav_write(const int16_t *s, int n)
{
    if (!apu_wav_is_open() || s_wv_closed || !s || n <= 0)
        return;
    if (!bw_wv_mode()) {
        apu_wav_write(s, n);
        return;
    }
    AcquireSRWLockExclusive(&s_wv_plock);
    s_wv_hin = bw_wv_hash(s_wv_hin, &s_wv_nin, s, n);
    while (n > 0 && !s_wv_closed) {
        LONG64 w = s_wv_w, r = InterlockedCompareExchange64(&s_wv_r, 0, 0);
        LONG64 room = BW_WAV_RING - (w - r), at = w % BW_WAV_RING, k = n;
        if (room <= 0) {
            InterlockedIncrement(&s_wv_full);   /* 16 s of disk stall: wait, as the old path did */
            Sleep(1);
            continue;
        }
        if (k > room) k = room;
        if (k > BW_WAV_RING - at) k = BW_WAV_RING - at;
        memcpy(s_wv_buf + 2 * at, s, sizeof(int16_t) * 2 * (size_t)k);
        InterlockedExchange64(&s_wv_w, w + k);
        s += 2 * k;
        n -= (int)k;
    }
    ReleaseSRWLockExclusive(&s_wv_plock);
}

/* Producers stop; the ring's rest goes to the file (at most wait_ms); the
 * hashes are printed, once. 1 when the ring was drained. */
static int bw_wv_finish(DWORD wait_ms)
{
    static volatile LONG once;
    DWORD t0 = GetTickCount();
    int drained;
    if (s_wv_mode != 1 || InterlockedExchange(&once, 1))
        return 1;
    InterlockedExchange(&s_wv_closed, 1);
    AcquireSRWLockExclusive(&s_wv_plock);   /* no producer inside any more */
    ReleaseSRWLockExclusive(&s_wv_plock);
    while (s_wv_r != InterlockedCompareExchange64(&s_wv_w, 0, 0) && GetTickCount() - t0 < wait_ms)
        Sleep(1);
    drained = s_wv_r == InterlockedCompareExchange64(&s_wv_w, 0, 0);
    fprintf(stderr, "[AUDIO-WAV] the writer thread (au-vp): %llu frames handed over, %llu written (up to the"
                    " cap); hashes %016llX in, %016llX out: %s; waits on a full ring %ld%s\n",
            s_wv_nin, s_wv_nout, s_wv_hin, s_wv_hout,
            s_wv_nin == s_wv_nout && s_wv_hin == s_wv_hout ? "the same bytes" : "DIFFERENT",
            (long)s_wv_full, drained ? "" : " (the ring not drained in time)");
    return drained;
}

void bw_wav_close(void)
{
    if (bw_wv_finish(5000) && s_wv_mode == 1) {
        InterlockedExchange(&s_wv_stop, 1);
        WaitForSingleObject(s_wv_th, 2000);
    }
    apu_wav_close();
}

void bw_wav_close_nowait(int wait_ms)
{
    bw_wv_finish(wait_ms > 0 ? (DWORD)wait_ms : 0);   /* the caller then closes the file, its own way */
}

/* atexit runs the last registered first: this is registered at the APU's
 * start, after apu_wav_open registered its own close, so the ring is drained
 * before that close finalises the file. The console-control handlers run in
 * the same order. */
static void bw_wv_atexit(void)
{
    bw_wav_close();
}

static BOOL WINAPI bw_wv_ctrl(DWORD type)
{
    (void)type;
    bw_wav_close();
    return FALSE;
}

void bw_wav_started(void)
{
    if (apu_wav_is_open() && bw_wv_mode()) {
        atexit(bw_wv_atexit);
        SetConsoleCtrlHandler(bw_wv_ctrl, TRUE);
    }
}

void bw_proc_qos(void)
{
    static volatile LONG once;
    const char *e = getenv("BW_PROC_QOS"), *t = getenv("BW_TEST_ECOQOS");
    PROCESS_POWER_THROTTLING_STATE st, rb;
    const char *what;
    BOOL ok = TRUE, got;
    if (InterlockedExchange(&once, 1))
        return;
    memset(&st, 0, sizeof st);
    st.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    if (t && t[0] == '1') {
        st.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
        st.StateMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
        ok = SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &st, sizeof st);
        what = "EcoQoS forced on (BW_TEST_ECOQOS=1, a test switch)";
    } else if (!(e && e[0] == '0')) {
        st.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
        st.StateMask = 0;
        ok = SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &st, sizeof st);
        what = "always high QoS and always the timer resolution it asks for (au-vp; BW_PROC_QOS=0 leaves both"
               " to Windows)";
    } else {
        what = "left to Windows (BW_PROC_QOS=0)";
    }
    memset(&rb, 0, sizeof rb);
    rb.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    got = GetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &rb, sizeof rb);
    if (got)
        fprintf(stderr, "[APU] BW the process's power throttling: %s%s; read back: control 0x%lX, state 0x%lX"
                        " (BW_PROC_QOS)\n", what, ok ? "" : " (refused)", rb.ControlMask, rb.StateMask);
    else
        fprintf(stderr, "[APU] BW the process's power throttling: %s%s; read back refused (error %lu)"
                        " (BW_PROC_QOS)\n", what, ok ? "" : " (refused)", GetLastError());
}
