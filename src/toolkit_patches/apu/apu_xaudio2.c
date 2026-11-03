/**
 * XAudio2 Audio Output Backend
 *
 * Provides low-latency audio output via XAudio2 (Win7+).
 * Called from the APU monitor frame to submit mixed samples.
 * Falls back gracefully if XAudio2 is unavailable.
 *
 * Blood Wake carried patch (D-016 Remastered surround, docs/day1/D2.md part
 * 3): the source voice carries 6 channels (5.1: FL FR FC LFE BL BR) when
 * bw_surround_output_wanted() says so for the device found; otherwise it is
 * as before (2 channels into a 2-channel mastering voice). And (0.2.5 item
 * 68) a lost or changed audio device is rebuilt on the default one: see
 * xa2_check_device. The original file is xboxrecomp/src/apu/apu_xaudio2.c
 * (sha256 guarded in CMakeLists.txt).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "apu_xaudio2.h"

/* The XAudio2 backend is Windows-only. On POSIX hosts apu_sdl2.c provides
 * the same xa2_* functions over SDL2. */
#if defined(_WIN32)

#define COBJMACROS
#include <windows.h>
#include <xaudio2.h>
#include <mmdeviceapi.h>     /* Blood Wake (item 68): the default-device notice */

#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "ole32.lib")

#define XA2_SAMPLE_RATE   48000
static int g_xa2_channels = 2;          /* Blood Wake: 2, or 6 for 5.1 */
#define XA2_CHANNELS      g_xa2_channels
extern int bw_surround_output_wanted(int device_channels);
/* Device buffer size and count. Defaults 512 x 4 (10.7 ms each, 43 ms
 * worst case queued); RECOMP_AUDIO_BUF_SAMPLES and RECOMP_AUDIO_BUF_COUNT
 * override them at xa2_init, clamped to the static storage below. */
#define XA2_BUF_SAMPLES_DEFAULT 512
#define XA2_NUM_BUFS_DEFAULT    4
#define XA2_BUF_SAMPLES_MIN     128
#define XA2_BUF_SAMPLES_MAX     4096
#define XA2_NUM_BUFS_MIN        2
#define XA2_NUM_BUFS_MAX        8

static int g_xa2_buf_samples = XA2_BUF_SAMPLES_DEFAULT;
static int g_xa2_num_bufs    = XA2_NUM_BUFS_DEFAULT;
#define XA2_BUF_SAMPLES g_xa2_buf_samples
#define XA2_NUM_BUFS    g_xa2_num_bufs

static int xa2_env_int(const char *name, int def, int lo, int hi)
{
    const char *e = getenv(name);
    int v;
    if (!e || !*e) return def;
    v = atoi(e);
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return v;
}

static IXAudio2               *g_xa2 = NULL;
static IXAudio2MasteringVoice *g_xa2_master = NULL;
static IXAudio2SourceVoice    *g_xa2_source = NULL;
/* One slot more than can be queued: with every buffer on the voice, the
 * next one is staged in a slot the voice does not hold. With only
 * XA2_NUM_BUFS slots the staging buffer was the oldest queued one, and
 * filling it overwrote audio the device had not played yet. */
#define XA2_RING (XA2_NUM_BUFS + 1)
static int16_t                 g_xa2_bufs[XA2_NUM_BUFS_MAX + 1][XA2_BUF_SAMPLES_MAX * 6];
static int                     g_xa2_next_buf = 0;
static int                     g_xa2_fill = 0;  /* samples staged in bufs[next_buf] */
static unsigned  g_xa2_underruns;
static int       g_xa2_dry;
static ULONGLONG g_xa2_underrun_logged;
static int                     g_xa2_initialized = 0;
static int                     g_xa2_frames_written = 0;

/* ---- Blood Wake (0.2.5 item 68): a lost or changed audio device ----------
 *
 * Unplugging headphones, a Bluetooth headset switching off, or a new default
 * device left the game silent until its next reboot: XAudio2 reports a
 * critical error and its voice stops consuming, and the APU then paced by
 * the host clock and dropped every period (apu_core.c, "A device that stops
 * consuming for good"). Now the engine's OnCriticalError and Windows'
 * default-device notice (IMMNotificationClient, render/console) only set a
 * flag; the APU frame thread, which makes every other xa2_* call, sees it at
 * its next submit and rebuilds XAudio2 and its voices on the default device,
 * with the channel layout the game was told at boot (the game can't be told
 * a new one mid-boot; XAudio2 maps 5.1 or stereo onto any device). The DSPs,
 * voices and mix state are the APU's and are untouched; what was queued on
 * the old device (at most XA2_NUM_BUFS buffers) is lost. If no device is
 * there, it tries again when Windows announces a new default device (or
 * every 5 s), silently in between.
 *
 * And (0.2.5 item 105) while the device queue is stalled: a device that
 * stops taking sound without an error (seen: the monitor's HDMI audio going
 * away and coming back) keeps the queue full, so the APU paces by the host
 * clock and submits nothing, and the check at the next submit never came:
 * the flag set by the critical error or Windows' notice was never acted on
 * and the boot stayed silent to its end. Now apu_core.c calls
 * xa2_poll_stalled each period while the queue is stalled: a flagged loss is
 * acted on there, and a device that has taken no sound for a second counts as
 * lost (at most every 5 s).
 *
 * Test switches (any edition; never in a preset):
 *   BW_TEST_AUDIO_LOST=<ms>[,<ms>...]  at those times after init, behave as if
 *       the device were lost: the voice stops consuming and the flag is set,
 *       as OnCriticalError would
 *   BW_TEST_AUDIO_STALL=<ms>[,<ms>...]  (item 105) at those times after init,
 *       the voice stops taking sound with no error and no notice
 *   BW_TEST_AUDIO_NODEVICE=<from>-<to>  (item 105) opens in that window (ms
 *       after the first open) fail as if Windows had no output device
 *   BW_AUDIO_REBUILD=0  no rebuild (the old behaviour, for comparing) */
static volatile LONG g_xa2_lost;       /* 0, or why: 1 critical error, 2 new default device, 3 test,
                                        * 4 no sound taken for a second while stalled (item 105) */
static volatile LONG g_xa2_lost_hr;
static uint64_t  g_xa2_played_base;   /* samples the voices before the last rebuild played */
static unsigned  g_xa2_rebuilds;
static ULONGLONG g_xa2_t0, g_xa2_retry_at, g_xa2_rebuilt_at;
static int       g_xa2_waiting;       /* no device after a loss: retrying */
static unsigned  g_xa2_quiet_events;  /* default-device notices while waiting */

static void STDMETHODCALLTYPE ecb_pass_start(IXAudio2EngineCallback *t) { (void)t; }
static void STDMETHODCALLTYPE ecb_pass_end(IXAudio2EngineCallback *t) { (void)t; }
static void STDMETHODCALLTYPE ecb_critical(IXAudio2EngineCallback *t, HRESULT err)
{
    (void)t;
    InterlockedExchange(&g_xa2_lost_hr, (LONG)err);
    InterlockedExchange(&g_xa2_lost, 1);
}
static IXAudio2EngineCallbackVtbl s_ecb_vtbl = { ecb_pass_start, ecb_pass_end, ecb_critical };
static IXAudio2EngineCallback s_ecb = { &s_ecb_vtbl };

static const GUID k_iid_unknown = { 0x00000000, 0x0000, 0x0000,
                                    { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
static const GUID k_clsid_mmde  = { 0xBCDE0395, 0xE52F, 0x467C,
                                    { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const GUID k_iid_mmde    = { 0xA95664D2, 0x9614, 0x4F35,
                                    { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const GUID k_iid_mmnc    = { 0x7991EEC9, 0x7E89, 0x4D85,
                                    { 0x83, 0x90, 0x6C, 0x70, 0x3C, 0xEC, 0x60, 0xC0 } };

static HRESULT STDMETHODCALLTYPE nc_qi(IMMNotificationClient *t, REFIID riid, void **ppv)
{
    if (IsEqualGUID(riid, &k_iid_unknown) || IsEqualGUID(riid, &k_iid_mmnc)) {
        *ppv = t;
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE nc_addref(IMMNotificationClient *t) { (void)t; return 1; }
static ULONG STDMETHODCALLTYPE nc_release(IMMNotificationClient *t) { (void)t; return 1; }
static HRESULT STDMETHODCALLTYPE nc_state(IMMNotificationClient *t, LPCWSTR id, DWORD s)
{ (void)t; (void)id; (void)s; return S_OK; }
static HRESULT STDMETHODCALLTYPE nc_added(IMMNotificationClient *t, LPCWSTR id)
{ (void)t; (void)id; return S_OK; }
static HRESULT STDMETHODCALLTYPE nc_removed(IMMNotificationClient *t, LPCWSTR id)
{ (void)t; (void)id; return S_OK; }
static HRESULT STDMETHODCALLTYPE nc_default(IMMNotificationClient *t, EDataFlow flow, ERole role,
                                            LPCWSTR id)
{
    (void)t; (void)id;
    if (flow == eRender && role == eConsole)
        InterlockedCompareExchange(&g_xa2_lost, 2, 0);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE nc_prop(IMMNotificationClient *t, LPCWSTR id, const PROPERTYKEY k)
{ (void)t; (void)id; (void)k; return S_OK; }
static IMMNotificationClientVtbl s_nc_vtbl = { nc_qi, nc_addref, nc_release, nc_state, nc_added,
                                               nc_removed, nc_default, nc_prop };
static IMMNotificationClient s_nc = { &s_nc_vtbl };
static IMMDeviceEnumerator *s_mmde;

/* Destroy the voices and the engine (the COM state stays). */
static void xa2_release(void)
{
    if (g_xa2_source) {
        IXAudio2SourceVoice_Stop(g_xa2_source, 0, XAUDIO2_COMMIT_NOW);
        IXAudio2SourceVoice_FlushSourceBuffers(g_xa2_source);
        g_xa2_source->lpVtbl->DestroyVoice(g_xa2_source);
        g_xa2_source = NULL;
    }
    if (g_xa2_master) {
        g_xa2_master->lpVtbl->DestroyVoice(g_xa2_master);
        g_xa2_master = NULL;
    }
    if (g_xa2) {
        IXAudio2_Release(g_xa2);
        g_xa2 = NULL;
    }
}

/* BW_TEST_AUDIO_NODEVICE (item 105): an open in the window fails as if Windows
 * had no output device. */
static int xa2_test_nodevice(int quiet)
{
    static int parsed;
    static ULONGLONG t0;
    static unsigned long a, b;
    ULONGLONG now = GetTickCount64();
    if (!parsed) {
        const char *e = getenv("BW_TEST_AUDIO_NODEVICE");
        parsed = 1;
        t0 = now;
        if (e && *e) {
            char *end;
            a = strtoul(e, &end, 10);
            b = *end == '-' ? strtoul(end + 1, NULL, 10) : 0;
        }
    }
    if (b > a && now - t0 >= a && now - t0 < b) {
        if (!quiet)
            fprintf(stderr, "[XA2] test: no audio device (BW_TEST_AUDIO_NODEVICE, %llu ms)\n",
                    (unsigned long long)(now - t0));
        return 1;
    }
    return 0;
}

/* The engine, a mastering voice on the default device, and the source voice,
 * started. At boot it also decides the layout (5.1 or stereo); a rebuild
 * keeps it. quiet: no failure lines (retries while no device is there). */
static HRESULT xa2_open(int boot, int quiet)
{
    HRESULT hr;
    WAVEFORMATEX wfx = { 0 };

    hr = XAudio2Create(&g_xa2, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr) || !g_xa2) {
        if (!quiet)
            fprintf(stderr, "[XA2] XAudio2Create failed: 0x%08lX\n", hr);
        return FAILED(hr) ? hr : E_FAIL;
    }
    IXAudio2_RegisterForCallbacks(g_xa2, &s_ecb);
    if (xa2_test_nodevice(quiet))        /* item 105's test switch */
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);

    /* Blood Wake: the device's own channel count decides whether a 5.1
     * source voice is wanted; without one, a 2-channel mastering voice as
     * before. */
    {
        int dev_ch = 2;
        XAUDIO2_VOICE_DETAILS det;
        hr = IXAudio2_CreateMasteringVoice(g_xa2, &g_xa2_master,
            XAUDIO2_DEFAULT_CHANNELS, XA2_SAMPLE_RATE, 0, NULL, NULL, 0);
        if (SUCCEEDED(hr) && g_xa2_master) {
            g_xa2_master->lpVtbl->GetVoiceDetails(g_xa2_master, &det);
            dev_ch = (int)det.InputChannels;
        }
        if (boot)
            g_xa2_channels = bw_surround_output_wanted(dev_ch) ? 6 : 2;
        if (g_xa2_master && (FAILED(hr) || (g_xa2_channels == 2 && dev_ch != 2))) {
            g_xa2_master->lpVtbl->DestroyVoice(g_xa2_master);
            g_xa2_master = NULL;
        }
        if (!g_xa2_master)
            hr = IXAudio2_CreateMasteringVoice(g_xa2, &g_xa2_master,
                2, XA2_SAMPLE_RATE, 0, NULL, NULL, 0);
        if (boot || SUCCEEDED(hr))
            fprintf(stderr, "[XA2] the audio device has %d channels; output: %s\n", dev_ch,
                    g_xa2_channels == 6 ? "5.1 (6 channels)" : "stereo");
    }
    if (FAILED(hr)) {
        if (!quiet)
            fprintf(stderr, "[XA2] CreateMasteringVoice failed: 0x%08lX\n", hr);
        return hr;
    }

    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = XA2_CHANNELS;
    wfx.nSamplesPerSec  = XA2_SAMPLE_RATE;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = XA2_CHANNELS * 2;
    wfx.nAvgBytesPerSec = XA2_SAMPLE_RATE * wfx.nBlockAlign;

    if (g_xa2_channels == 6) {
        /* Blood Wake: 5.1 with its channel mask, so XAudio2 maps it onto any
         * device (and downmixes it on a stereo one) */
        WAVEFORMATEXTENSIBLE wfe;
        static const GUID pcm = { 0x00000001, 0x0000, 0x0010,
                                  { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
        memset(&wfe, 0, sizeof(wfe));
        wfe.Format = wfx;
        wfe.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        wfe.Format.cbSize = (WORD)(sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX));
        wfe.Samples.wValidBitsPerSample = 16;
        wfe.dwChannelMask = 0x3F;                    /* FL FR FC LFE BL BR */
        wfe.SubFormat = pcm;
        hr = IXAudio2_CreateSourceVoice(g_xa2, &g_xa2_source,
            (const WAVEFORMATEX *)&wfe, 0, XAUDIO2_DEFAULT_FREQ_RATIO, NULL, NULL, NULL);
    } else {
        hr = IXAudio2_CreateSourceVoice(g_xa2, &g_xa2_source,
            &wfx, 0, XAUDIO2_DEFAULT_FREQ_RATIO, NULL, NULL, NULL);
    }
    if (FAILED(hr)) {
        if (!quiet)
            fprintf(stderr, "[XA2] CreateSourceVoice failed: 0x%08lX\n", hr);
        return hr;
    }

    hr = IXAudio2SourceVoice_Start(g_xa2_source, 0, XAUDIO2_COMMIT_NOW);
    if (FAILED(hr)) {
        if (!quiet)
            fprintf(stderr, "[XA2] Start failed: 0x%08lX\n", hr);
        return hr;
    }
    return S_OK;
}

/* BW_TEST_AUDIO_LOST: the next test time (ms after init) due, or 0. */
static ULONGLONG test_lost_due(ULONGLONG since)
{
    static int parsed;
    static ULONGLONG at[8];
    static int n, next;
    if (!parsed) {
        const char *e = getenv("BW_TEST_AUDIO_LOST");
        parsed = 1;
        while (e && *e && n < 8) {
            char *end;
            unsigned long v = strtoul(e, &end, 10);
            if (end == e)
                break;
            at[n++] = v;
            e = end;
            while (*e == ',' || *e == ' ')
                e++;
        }
    }
    if (next < n && since >= at[next])
        return at[next++];
    return 0;
}

/* BW_TEST_AUDIO_STALL (item 105): the next stall time (ms after init) due, or 0. */
static ULONGLONG test_stall_due(ULONGLONG since)
{
    static int parsed;
    static ULONGLONG at[16];
    static int n, next;
    if (!parsed) {
        const char *e = getenv("BW_TEST_AUDIO_STALL");
        parsed = 1;
        while (e && *e && n < 16) {
            char *end;
            unsigned long v = strtoul(e, &end, 10);
            if (end == e)
                break;
            at[n++] = v;
            e = end;
            while (*e == ',' || *e == ' ')
                e++;
        }
    }
    if (next < n && since >= at[next])
        return at[next++];
    return 0;
}

/* The APU frame thread, before each submit (and while the queue is stalled,
 * item 105): a lost or changed device is rebuilt here (see above). */
static void xa2_check_device(void)
{
    ULONGLONG now = GetTickCount64(), t;
    LONG why;
    LARGE_INTEGER f, q0, q1;
    HRESULT hr;
    static const char *const k_why[] = { "", "XAudio2 reported a critical error",
                                         "Windows has a new default audio device",
                                         "test switch BW_TEST_AUDIO_LOST",
                                         "the device has taken no sound for a second" };

    if ((t = test_lost_due(now - g_xa2_t0)) != 0) {
        if (g_xa2_source)
            IXAudio2SourceVoice_Stop(g_xa2_source, 0, XAUDIO2_COMMIT_NOW);
        fprintf(stderr, "[XA2] test: the audio device is lost at %llu ms (the voice stops "
                "consuming)\n", (unsigned long long)t);
        InterlockedExchange(&g_xa2_lost, 3);
    }
    if ((t = test_stall_due(now - g_xa2_t0)) != 0) {
        if (g_xa2_source)
            IXAudio2SourceVoice_Stop(g_xa2_source, 0, XAUDIO2_COMMIT_NOW);
        fprintf(stderr, "[XA2] test: the device stops taking sound at %llu ms, with no error and no"
                " notice (BW_TEST_AUDIO_STALL)\n", (unsigned long long)t);
    }
    if (g_xa2_rebuilt_at && now - g_xa2_rebuilt_at >= 1000 && g_xa2_source) {
        XAUDIO2_VOICE_STATE st;
        IXAudio2SourceVoice_GetState(g_xa2_source, &st, 0);
        fprintf(stderr, "[XA2] one second after the rebuild the new device has played %llu "
                "samples (%.0f ms of sound)\n", (unsigned long long)st.SamplesPlayed,
                st.SamplesPlayed / 48.0);
        g_xa2_rebuilt_at = 0;
    }
    if (!g_xa2_lost && !(g_xa2_waiting && now >= g_xa2_retry_at))
        return;
    why = InterlockedExchange(&g_xa2_lost, 0);
    if (why) {
        const char *e = getenv("BW_AUDIO_REBUILD");
        if (g_xa2_waiting && why == 2)
            g_xa2_quiet_events++;        /* while no device: counted, said once when it's back */
        else
            fprintf(stderr, "[XA2] the audio device is gone or changed: %s\n", k_why[why < 5 ? why : 0]);
        if (why == 1)
            fprintf(stderr, "[XA2] (the error: 0x%08lX)\n", (unsigned long)g_xa2_lost_hr);
        if (e && *e == '0') {
            fprintf(stderr, "[XA2] no rebuild (BW_AUDIO_REBUILD=0): silent until the next boot\n");
            return;
        }
    }
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&q0);
    if (g_xa2_source) {
        XAUDIO2_VOICE_STATE st;
        IXAudio2SourceVoice_GetState(g_xa2_source, &st, 0);
        g_xa2_played_base += st.SamplesPlayed;
    }
    xa2_release();
    {
        static int com;
        if (!com++) {
            HRESULT c = CoInitializeEx(NULL, COINIT_MULTITHREADED);   /* this thread's, once */
            (void)c;
        }
    }
    hr = xa2_open(0, g_xa2_waiting);
    g_xa2_next_buf = 0;
    g_xa2_fill = 0;
    g_xa2_dry = 1;                       /* the empty new voice is not an underrun */
    if (FAILED(hr)) {
        xa2_release();
        QueryPerformanceCounter(&q1);
        if (!g_xa2_waiting)
            fprintf(stderr, "[XA2] no audio device (0x%08lX, %.1f ms to find out): the game runs "
                    "silent and tries again when Windows has a device (or every 5 s)\n", hr,
                    (double)(q1.QuadPart - q0.QuadPart) * 1000.0 / (double)f.QuadPart);
        g_xa2_waiting = 1;
        g_xa2_retry_at = now + 5000;     /* Windows' notice usually comes first */
        return;
    }
    QueryPerformanceCounter(&q1);
    if (g_xa2_quiet_events)
        fprintf(stderr, "[XA2] (Windows changed its default device %u more times while there was "
                "none)\n", g_xa2_quiet_events);
    g_xa2_quiet_events = 0;
    g_xa2_waiting = 0;
    g_xa2_rebuilds++;
    g_xa2_rebuilt_at = now;
    fprintf(stderr, "[XA2] rebuilt on the default audio device in %.1f ms (rebuild %u; %s, as "
            "the game was told at boot); sound goes on\n",
            (double)(q1.QuadPart - q0.QuadPart) * 1000.0 / (double)f.QuadPart, g_xa2_rebuilds,
            g_xa2_channels == 6 ? "5.1" : "stereo");
}

/* 0.2.5 item 105 (carried apu_core.c calls it): each period while the device
 * queue is stalled, when no submit runs xa2_check_device. A loss flagged
 * meanwhile is acted on, and a device that has taken no sound for a second
 * counts as lost (why 4), at most every 5 s. BW_AUDIO_REBUILD=0 keeps the old
 * behaviour (silent until the next boot). */
void xa2_poll_stalled(int stalled_ms)
{
    static int no_rebuild = -1;
    static ULONGLONG s_last;
    ULONGLONG now = GetTickCount64();
    if (no_rebuild < 0) {
        const char *e = getenv("BW_AUDIO_REBUILD");
        no_rebuild = e && *e == '0';
    }
    if (!g_xa2_initialized || no_rebuild)
        return;
    if (!g_xa2_lost && !g_xa2_waiting && stalled_ms >= 1000 && now - s_last >= 5000) {
        s_last = now;
        InterlockedCompareExchange(&g_xa2_lost, 4, 0);
    }
    xa2_check_device();
}

int xa2_init(void)
{
    HRESULT hr;
    int com_initialized;

    if (g_xa2_initialized) return 1;

    g_xa2_buf_samples = xa2_env_int("RECOMP_AUDIO_BUF_SAMPLES",
                                    XA2_BUF_SAMPLES_DEFAULT,
                                    XA2_BUF_SAMPLES_MIN, XA2_BUF_SAMPLES_MAX);
    g_xa2_num_bufs = xa2_env_int("RECOMP_AUDIO_BUF_COUNT", XA2_NUM_BUFS_DEFAULT,
                                 XA2_NUM_BUFS_MIN, XA2_NUM_BUFS_MAX);

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        fprintf(stderr, "[XA2] CoInitializeEx failed: 0x%08lX\n", hr);
        return 0;
    }
    com_initialized = SUCCEEDED(hr);

    hr = xa2_open(1, 0);
    if (FAILED(hr)) {
        /* Blood Wake (item 68): XAudio2 works but Windows has no audio
         * device (none plugged in, or the default one just went away): start
         * silent and wait for one, as after a loss, instead of the waveOut
         * fallback (which has no device either) and a silent boot. */
        if (g_xa2 && (hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)
                      || hr == XAUDIO2_E_DEVICE_INVALIDATED)) {
            xa2_release();
            g_xa2_waiting = 1;
            g_xa2_retry_at = GetTickCount64() + 5000;
            fprintf(stderr, "[XA2] no audio device at start (0x%08lX): the game starts silent and "
                    "its sound comes on when Windows has a device\n", hr);
        } else {
            goto fail;
        }
    }

    /* Blood Wake (item 68): hear about a new default device. Without the
     * enumerator, only the engine's own critical error is heard. */
    if (SUCCEEDED(CoCreateInstance(&k_clsid_mmde, NULL, CLSCTX_ALL, &k_iid_mmde, (void **)&s_mmde))
        && FAILED(IMMDeviceEnumerator_RegisterEndpointNotificationCallback(s_mmde, &s_nc))) {
        IMMDeviceEnumerator_Release(s_mmde);
        s_mmde = NULL;
    }

    g_xa2_next_buf = 0;
    g_xa2_fill = 0;
    g_xa2_dry = 0;
    g_xa2_initialized = 1;
    g_xa2_frames_written = 0;
    g_xa2_t0 = GetTickCount64();

    fprintf(stderr, "[XA2] XAudio2 initialized (%d Hz stereo 16-bit, %d x %d-sample buffers, %.1f ms queued max)\n",
            XA2_SAMPLE_RATE, XA2_NUM_BUFS, XA2_BUF_SAMPLES,
            1000.0 * XA2_NUM_BUFS * XA2_BUF_SAMPLES / XA2_SAMPLE_RATE);
    return 1;

fail:
    xa2_shutdown();
    /* Failed initialization still runs on the COM-initializing thread. */
    if (com_initialized) CoUninitialize();
    return 0;
}

void xa2_shutdown(void)
{
    if (s_mmde) {
        IMMDeviceEnumerator_UnregisterEndpointNotificationCallback(s_mmde, &s_nc);
        IMMDeviceEnumerator_Release(s_mmde);
        s_mmde = NULL;
    }
    xa2_release();

    if (g_xa2_initialized)
        fprintf(stderr, "[XA2] Shut down (%d frames written)\n", g_xa2_frames_written);
    g_xa2_initialized = 0;
}

int xa2_is_active(void)
{
    return g_xa2_initialized;
}

/* Hand mixed samples to XAudio2. Called from the APU frame thread with one
 * EP period (256 samples) at a time. Samples accumulate in the current device
 * buffer, which is submitted when full, so a call shorter than a buffer is
 * kept, not padded or dropped. Returns 1 if every sample was accepted; 0 if
 * the voice already had every buffer queued when one filled, in which case
 * that buffer is discarded (pacing by queue depth is what prevents it). */
int xa2_submit_samples(const int16_t *samples, int num_samples)
{
    XAUDIO2_VOICE_STATE state;
    XAUDIO2_BUFFER xbuf;
    int idx, n, ok = 1;
    HRESULT hr;

    if (!g_xa2_initialized) return 0;
    xa2_check_device();                  /* Blood Wake (item 68) */
    if (!g_xa2_source) return 0;

    while (num_samples > 0) {
        idx = g_xa2_next_buf;
        n = XA2_BUF_SAMPLES - g_xa2_fill;
        if (n > num_samples) n = num_samples;
        memcpy(&g_xa2_bufs[idx][g_xa2_fill * XA2_CHANNELS], samples,
               n * XA2_CHANNELS * sizeof(int16_t));
        g_xa2_fill += n;
        samples += n * XA2_CHANNELS;
        num_samples -= n;
        if (g_xa2_fill < XA2_BUF_SAMPLES)
            break;

        g_xa2_fill = 0;
        IXAudio2SourceVoice_GetState(g_xa2_source, &state,
                                     XAUDIO2_VOICE_NOSAMPLESPLAYED);
        if ((int)state.BuffersQueued >= XA2_NUM_BUFS) { ok = 0; continue; }

        memset(&xbuf, 0, sizeof(xbuf));
        xbuf.AudioBytes = XA2_BUF_SAMPLES * XA2_CHANNELS * sizeof(int16_t);
        xbuf.pAudioData = (const BYTE *)g_xa2_bufs[idx];
        hr = IXAudio2SourceVoice_SubmitSourceBuffer(g_xa2_source, &xbuf, NULL);
        if (FAILED(hr)) { ok = 0; continue; }

        g_xa2_next_buf = (idx + 1) % XA2_RING;
        g_xa2_frames_written++;
    }
    return ok;
}

/* Blood Wake: the source voice's channels (2, or 6 for 5.1). */
int xa2_output_channels(void)
{
    return g_xa2_initialized ? g_xa2_channels : 2;
}

int xa2_get_buffer_size(void)
{
    return XA2_BUF_SAMPLES;
}

int xa2_get_buffer_count(void)
{
    return XA2_NUM_BUFS;
}

/* Samples handed over and not yet played: the buffers the voice still
 * holds plus what is staged in the next one. Also the underrun detector:
 * the voice running dry after audio has started is one underrun, counted
 * once per dry spell and reported at most once a second. A drain announced
 * by xa2_expect_drain (the frame thread pausing) is not one. */
int xa2_queued_samples(void)
{
    XAUDIO2_VOICE_STATE state;

    if (!g_xa2_initialized || !g_xa2_source) return 0;
    IXAudio2SourceVoice_GetState(g_xa2_source, &state,
                                 XAUDIO2_VOICE_NOSAMPLESPLAYED);
    if (state.BuffersQueued == 0 && g_xa2_frames_written > 0) {
        if (!g_xa2_dry) {
            ULONGLONG now = GetTickCount64();
            g_xa2_dry = 1;
            g_xa2_underruns++;
            if (now - g_xa2_underrun_logged >= 1000) {
                g_xa2_underrun_logged = now;
                fprintf(stderr, "[XA2] underrun x%u\n", g_xa2_underruns);
            }
        }
    } else {
        g_xa2_dry = 0;
    }
    return (int)state.BuffersQueued * XA2_BUF_SAMPLES + g_xa2_fill;
}

/* Mark the coming dry spell as already seen; the first call that finds
 * buffers queued again clears it, so later dry spells count as before. */
void xa2_expect_drain(void)
{
    g_xa2_dry = 1;
}

unsigned xa2_underruns(void)
{
    return g_xa2_underruns;
}

/* Blood Wake (item 68): counted across rebuilds, so the APU's clock and its
 * starve accounting see one device. */
uint64_t xa2_samples_played(void)
{
    XAUDIO2_VOICE_STATE state;

    if (!g_xa2_initialized) return 0;
    if (!g_xa2_source) return g_xa2_played_base;
    IXAudio2SourceVoice_GetState(g_xa2_source, &state, 0);
    return g_xa2_played_base + state.SamplesPlayed;
}

#else /* !_WIN32: apu_sdl2.c implements the same contract over SDL2 */

/* ISO C wants at least one declaration in a translation unit. */
typedef int apu_xaudio2_posix_unused;

#endif /* _WIN32 */
