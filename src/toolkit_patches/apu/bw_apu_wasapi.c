/*
 * bw_apu_wasapi.c - the APU's output straight to Windows' shared audio engine
 * (WASAPI), the port's output in every edition since 0.3.0 (lane lat-a,
 * docs/latency/a.md; lane au-out of the audio rework, docs/audio/out.md; lane
 * au-orig for Original, docs/audio/orig.md). A port change, like the D3D11
 * back end: the game's sound and its logic are unchanged, only the host's
 * path from the chip to Windows. BW_AUDIO_OUT=xaudio2 keeps XAudio2.
 *
 * Blood Wake file, compiled into the toolkit's xbox_apu library beside the
 * carried apu_xaudio2.c. That file keeps the backend's contract (the xa2_*
 * functions apu_core.c calls), the lost/changed-device rebuild and the
 * no-device wait (0.2.5 item 68), the test switches and the counters, and
 * calls in here instead of XAudio2 on a boot that takes this output.
 *
 * Why (seen, docs/latency/a.md): XAudio2 keeps its own buffering between
 * reading a sample from our voice and the speakers, 1916 samples (39.9 ms)
 * by its own CurrentLatencyInSamples on the maintainer's monitor output, on
 * top of the APU's queue. A shared-mode WASAPI stream is read by Windows'
 * audio engine one period (480 frames, 10 ms there) per pass straight from
 * the buffer written here, so that buffering is gone.
 *
 * - Shared mode only, never exclusive: the device stays everyone's.
 * - The stream is what XAudio2's voice was: 48 kHz, 16-bit, 2 channels, or 6
 *   (5.1) when the device has 6 or more (bw_surround_output_wanted, the same
 *   rule). AUTOCONVERTPCM: Windows converts to its mix format where they
 *   differ (rate, channel count, sample type), as XAudio2 does.
 * - Event-driven (au-out; BW_WASAPI_EVENT=0 polls, as lat-a built it): the
 *   engine signals one event per pass, and the APU frame thread waits on it
 *   when the queue is full (apu_core.c), so it tops the stream up right after
 *   each pass instead of on a timer. The engine's period is the device's own;
 *   where the device offers a smaller shared period (IAudioClient3), the
 *   smallest one is asked for (BW_WASAPI_PERIOD=<frames> picks one, 0 never).
 * - The APU frame thread writes each 256-sample period as it is made (no
 *   staging), and reads the padding (frames written, not yet read) for its
 *   pacing; the marks are in samples (apu_xaudio2.c, xa2_marks_samples).
 * - The buffer is 100 ms, above the adaptive queue's cap plus a period.
 * - A failing call (AUDCLNT_E_DEVICE_INVALIDATED: the device went away) is
 *   reported to apu_xaudio2.c, which rebuilds on the default device. On a
 *   rebuild the new stream is opened on the output worker (bw_apu_out.c) into
 *   a second slot and swapped in at a period boundary; when the old stream
 *   still plays (Windows has a new default device), what it hadn't played yet
 *   is written to the new one first, so the sound goes on where it was.
 * - au-fx (docs/audio/fx.md): a stream that replaces a lost one has no tail
 *   to take over. Started empty, the engine's first pass came at a random
 *   moment of its own clock, often before the frame thread had written a
 *   whole period (two of the APU's 256-sample periods for one of the
 *   engine's 480), and played the rest as silence: one short pass, 4.7 ms,
 *   after about 1 in 4 forced losses (lane V's RC1 soak, seen). Now such a
 *   stream is held (opened, not started) until the writes reach the queue's
 *   floor (one engine period, event-driven), then started. Wherever this
 *   output runs; BW_WASAPI_PREFILL=0 starts it at once, as RC1 did.
 */
#define COBJMACROS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../bw_edition.h"
#include "bw_apu_wasapi.h"
#include "bw_apu_out.h"

extern int bw_surround_output_wanted(int device_channels);

#define WA_RATE        48000
#define WA_BUFFER_HNS  1000000          /* 100 ms, in 100-ns units */
#define WA_HIST        9600             /* frames kept for the tail carried to a new device */

static const GUID k_clsid_mmde = { 0xBCDE0395, 0xE52F, 0x467C,
                                   { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const GUID k_iid_mmde   = { 0xA95664D2, 0x9614, 0x4F35,
                                   { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const GUID k_iid_ac     = { 0x1CB9AD4C, 0xDBFA, 0x4C32,
                                   { 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2 } };
static const GUID k_iid_ac3    = { 0x7ED4EE07, 0x8E67, 0x4CD4,
                                   { 0x8C, 0x1A, 0x2B, 0x7A, 0x59, 0x87, 0xAD, 0x42 } };
static const GUID k_iid_rc     = { 0xF294ACFC, 0x3146, 0x4483,
                                   { 0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2 } };
static const GUID k_iid_clock  = { 0xCD63314F, 0x3FBA, 0x4A1B,
                                   { 0x81, 0x2C, 0xEF, 0x96, 0x35, 0x87, 0x28, 0xE7 } };
static const GUID k_pcm        = { 0x00000001, 0x0000, 0x0010,
                                   { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };

/* au-lat (docs/audio/lat.md, deliverable 4): WASAPI exclusive mode, a test
 * switch only (BW_WASAPI_EXCLUSIVE=1, wherever this output runs; off by
 * default, never in a preset, no launcher row): the stream takes the device for itself, so the
 * shared engine's own buffering (about 15 ms here, the "windows" stage) goes,
 * and with it every other program's sound on that device while the game runs.
 * - The device's own format (its Windows format when that is 48 kHz with our
 *   channel count, else the first of 16-bit, 24-in-32, 32-bit float, 24-bit,
 *   32-bit that it takes at 48 kHz; nothing is resampled here), event-driven at
 *   the device's minimum period (BW_WASAPI_XPERIOD=<ms> asks for another).
 * - The device wants a whole period at each of its events, and the APU makes
 *   256-frame periods: so the frame thread writes into a ring (WX_RING frames)
 *   and an output thread of the stream's own (MMCSS "Pro Audio") hands the
 *   device each period from it, silence where the ring runs short (counted),
 *   then sets the event the frame thread waits on. The pacing's queue is the
 *   ring; the engine's pass is the device's period.
 * - Anything that fails (the format, the period, the device busy or not
 *   allowing exclusive use) falls back to the shared stream, with the reason
 *   logged. A rebuild releases the old stream before opening the new one (the
 *   frame thread's rebuild, apu_xaudio2.c), since the old one holds the device.
 */
#define WX_RING 16384                   /* frames, 341 ms */
enum { WX_S16, WX_S24_32, WX_F32, WX_S24, WX_S32 };
typedef struct {
    IAudioClient       *ac;             /* borrowed from the stream */
    IAudioRenderClient *rc;
    HANDLE   ev, thr;                   /* the device's event; the output thread */
    volatile LONG stop, failed, hr;
    UINT32   cap;                       /* the device's period, frames */
    int      ch, kind, bps;             /* channels, sample kind, bytes per sample */
    volatile LONG64 silence;            /* frames of silence handed (the prefill, shortages) */
    int      data;                      /* the first data has been handed */
} WxCtx;
static int16_t s_xr[WX_RING * 6];       /* the ring, 16-bit frames at the boot's layout */
static volatile LONG64 s_xr_w, s_xr_r;  /* frames written to it / handed from it, ever */
static volatile LONG s_x_short;         /* periods handed short */
static volatile LONG64 s_x_short_frames;

/* One stream: the live one (the frame thread's), or one the worker opened
 * that waits to be swapped in. */
typedef struct {
    IAudioClient       *ac;
    IAudioRenderClient *rc;
    IAudioClock        *clk;
    UINT64   clk_freq;
    UINT32   cap;                       /* the buffer, frames */
    int      ch;
    int      period;                    /* the engine's period, in our 48 kHz frames */
    int      event;                     /* event-driven */
    int      lowlat;                    /* IAudioClient3's shared period */
    int      fl32;                      /* written in the engine's 32-bit float (lowlat only) */
    unsigned long mix_rate;
    uint64_t written;                   /* frames written to this stream */
    UINT32   pad;                       /* the last padding read */
    UINT32   start_at;                  /* au-fx: held, not started, until written reaches this (0: started) */
    LONGLONG held_q0;                   /* au-fx: when it was put in place held (performance counter) */
    int      fresh;                     /* au-fx: put in place with no tail (BW_TEST_AUDIO_SLOWSTART) */
    WxCtx   *x;                         /* au-lat: exclusive mode (NULL: shared) */
    char     line[600];                 /* the device line */
} WaStream;

static WaStream s_w;                    /* live */
static WaStream s_n;                    /* opened on the worker, not started */
static WaStream s_old;                  /* stopped, let go on the worker */
static HRESULT  s_err;
static LARGE_INTEGER s_qpf;
static HANDLE   s_ev;                   /* the engine's pass event, every stream's */
static int16_t  s_hist[WA_HIST * 6];    /* the last frames written, for the tail */
static uint64_t s_hist_n;
static int      s_hist_ch = 2;
/* queue before each write (BW_XA2_STATS): a histogram in 16-frame bins */
#define WA_QBINS 320
static unsigned s_qh[WA_QBINS];

/* The output, every edition (lane au-orig, docs/audio/orig.md): WASAPI unless
 * BW_AUDIO_OUT=xaudio2 (any edition); in Remastered and later BW_FIX=-AUDIO_WASAPI
 * keeps XAudio2 too (the registry line is retired as an edition change and
 * kept as that alias, src/bw_fixes.def). */
int bw_wa_wanted(void)
{
    static int w = -1;
    if (w < 0) {
        const char *e = getenv("BW_AUDIO_OUT");
        if (e && *e)
            w = e[0] == 'w' || e[0] == 'W'; /* wasapi | xaudio2, any edition */
        else if (bw_edition_at_least(BW_ED_REMASTERED))
            w = bw_fix_enabled(BW_FIX_AUDIO_WASAPI);   /* on unless -AUDIO_WASAPI */
        else
            w = 1;                          /* Original: the port's output too (0.3.0) */
    }
    return w;
}

/* au-fx: BW_WASAPI_PREFILL=0 starts a stream that replaces a lost one at
 * once (RC1's way); otherwise it is held until it holds the queue's floor. */
static int wa_prefill_wanted(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("BW_WASAPI_PREFILL");
        on = !(e && *e == '0');
        if (!on)
            fprintf(stderr, "[XA2] WASAPI: a new stream after a device loss starts at once"
                            " (BW_WASAPI_PREFILL=0)\n");
    }
    return on;
}

/* au-fx: the frames a stream that replaces a lost one is held for: the
 * queue's floor (the carried apu_xaudio2.c's marks, from the live stream,
 * i.e. the new device's period), at most half its buffer; 0: start at once. */
extern int xa2_marks_samples(int *floor, int *start, int *cap);
static UINT32 wa_hold_frames(void)
{
    int fl = 0, st, cap;
    if (!wa_prefill_wanted() || !xa2_marks_samples(&fl, &st, &cap) || fl <= 0)
        return 0;
    if (s_w.cap && (UINT32)fl > s_w.cap / 2)
        fl = (int)(s_w.cap / 2);
    return (UINT32)fl;
}

/* au-fx (the lead's addition, after lane V's soak): a stream with no tail to
 * carry also starts the adaptive queue again at its start mark (AO-5's 32 ms,
 * the carried apu_core.c), which steps down to the floor as at boot, so the
 * first passes after a lost device have a margin. BW_WASAPI_REBUILD_START=0
 * keeps the queue where it was. */
static int s_qrestart;

static int wa_rebuild_start_wanted(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("BW_WASAPI_REBUILD_START");
        on = !(e && *e == '0');
        if (!on)
            fprintf(stderr, "[XA2] WASAPI: the queue stays where it was after a device loss"
                            " (BW_WASAPI_REBUILD_START=0)\n");
    }
    return on;
}

int bw_wa_take_queue_restart(void)
{
    int r = s_qrestart;
    s_qrestart = 0;
    return r;
}

/* au-fx: the live stream was just put in place with tail frames in it: hold
 * it if that's under the queue's floor (1), or leave it to be started (0).
 * With no tail at all, the queue starts again at its start mark (above). */
static int wa_hold(UINT32 tail)
{
    LARGE_INTEGER q;
    UINT32 n;
    if (!tail && wa_rebuild_start_wanted())
        s_qrestart = 1;
    n = wa_hold_frames();
    if (!n || tail >= n)
        return 0;
    s_w.start_at = n;
    QueryPerformanceCounter(&q);
    s_w.held_q0 = q.QuadPart;
    return 1;
}

/* au-fx, a test aid (any edition that takes this output; off unless set):
 * BW_TEST_AUDIO_SLOWSTART=<ms> makes the frame thread wait that long once,
 * right after its first write to a stream that replaced a lost one, as a
 * busy PC can (lane V's RC1 soak, inferred). With the stream started at once
 * (BW_WASAPI_PREFILL=0) the engine's first pass then finds one APU period,
 * a short pass every time; held to the floor, the stream isn't started
 * until the second write. It proves the hold without waiting for the race. */
static int wa_test_slowstart_ms(void)
{
    static int ms = -1;
    if (ms < 0) {
        const char *e = getenv("BW_TEST_AUDIO_SLOWSTART");
        ms = e && *e ? atoi(e) : 0;
        if (ms < 0 || ms > 200)
            ms = 0;
        if (ms)
            fprintf(stderr, "[XA2] test: after a device loss the frame thread waits %d ms after its first"
                            " write to the new stream (BW_TEST_AUDIO_SLOWSTART)\n", ms);
    }
    return ms;
}

static int wa_event_wanted(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("BW_WASAPI_EVENT");
        on = !(e && *e == '0');
    }
    return on;
}

/* ---- au-lat: exclusive mode (the comment at WX_RING) --------------------- */

static int wa_excl_wanted(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("BW_WASAPI_EXCLUSIVE");
        on = e && *e && *e != '0';
        if (on)
            fprintf(stderr, "[XA2] WASAPI: exclusive mode asked for (BW_WASAPI_EXCLUSIVE, a test switch):"
                            " the device is the game's alone while it runs\n");
    }
    return on;
}

int bw_wa_exclusive_wanted(void)
{
    return bw_wa_wanted() && wa_excl_wanted();
}

/* n 16-bit frames from the ring at r (frames, ever) into the device's buffer. */
static void wx_copy(const WxCtx *x, BYTE *dst, LONG64 r, UINT32 n)
{
    UINT32 i;
    int c;
    for (i = 0; i < n; i++) {
        const int16_t *s = s_xr + (size_t)((r + i) % WX_RING) * x->ch;
        BYTE *d = dst + (size_t)i * x->ch * x->bps;
        for (c = 0; c < x->ch; c++, d += x->bps) {
            int v = s[c];
            switch (x->kind) {
            case WX_S16:    memcpy(d, &s[c], 2); break;
            case WX_S24_32: { int32_t t = (int32_t)((uint32_t)v << 16); memcpy(d, &t, 4); } break;
            case WX_S32:    { int32_t t = (int32_t)((uint32_t)v << 16); memcpy(d, &t, 4); } break;
            case WX_F32:    { float f = v * (1.0f / 32768.0f); memcpy(d, &f, 4); } break;
            case WX_S24:    { uint32_t t = (uint32_t)v << 8; d[0] = (BYTE)t; d[1] = (BYTE)(t >> 8);
                              d[2] = (BYTE)(t >> 16); } break;
            }
        }
    }
}

/* The output thread: at each of the device's events, a whole period from the
 * ring (silence for what it lacks), then the frame thread's event. */
static DWORD WINAPI wx_thread(LPVOID arg)
{
    WxCtx *x = (WxCtx *)arg;
    DWORD task = 0;
    HANDLE av = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
    while (!x->stop) {
        BYTE *p = NULL;
        LONG64 w, r;
        UINT32 n;
        HRESULT hr;
        DWORD k = WaitForSingleObject(x->ev, 200);
        if (x->stop)
            break;
        if (k != WAIT_OBJECT_0)
            continue;                       /* stopped (a test stall) or gone: the frame thread sees it */
        hr = IAudioRenderClient_GetBuffer(x->rc, x->cap, &p);
        if (FAILED(hr)) {
            InterlockedExchange(&x->hr, (LONG)hr);
            InterlockedExchange(&x->failed, 1);
            SetEvent(s_ev);
            Sleep(2);
            continue;
        }
        w = InterlockedCompareExchange64(&s_xr_w, 0, 0);
        r = s_xr_r;
        n = w - r < (LONG64)x->cap ? (UINT32)(w - r) : x->cap;
        if (n)
            wx_copy(x, p, r, n);
        if (n < x->cap) {
            memset(p + (size_t)n * x->ch * x->bps, 0, (size_t)(x->cap - n) * x->ch * x->bps);
            InterlockedAdd64(&x->silence, (LONG64)(x->cap - n));
            if (x->data) {
                InterlockedIncrement(&s_x_short);
                InterlockedAdd64(&s_x_short_frames, (LONG64)(x->cap - n));
            }
        }
        if (n)
            x->data = 1;
        InterlockedExchange64(&s_xr_r, r + n);
        hr = IAudioRenderClient_ReleaseBuffer(x->rc, x->cap, 0);
        if (FAILED(hr)) {
            InterlockedExchange(&x->hr, (LONG)hr);
            InterlockedExchange(&x->failed, 1);
        }
        SetEvent(s_ev);
    }
    if (av)
        AvRevertMmThreadCharacteristics(av);
    return 0;
}

/* Stop and let go of a stream's output thread (before its client goes). */
static void wx_end(WaStream *w)
{
    WxCtx *x = w->x;
    if (!x)
        return;
    InterlockedExchange(&x->stop, 1);
    if (x->ev)
        SetEvent(x->ev);
    if (x->thr) {
        WaitForSingleObject(x->thr, 2000);
        CloseHandle(x->thr);
        InterlockedExchange64(&s_xr_r, InterlockedCompareExchange64(&s_xr_w, 0, 0));   /* the ring's rest goes */
    }
    if (x->ev)
        CloseHandle(x->ev);
    free(x);
    w->x = NULL;
}

/* Start a stream: exclusive, the device's first period is silence and the
 * output thread starts with it. */
static HRESULT wa_start(WaStream *w)
{
    HRESULT hr;
    if (w->x) {
        WxCtx *x = w->x;
        BYTE *p = NULL;
        if (SUCCEEDED(IAudioRenderClient_GetBuffer(w->rc, x->cap, &p))
                && SUCCEEDED(IAudioRenderClient_ReleaseBuffer(w->rc, x->cap, AUDCLNT_BUFFERFLAGS_SILENT)))
            InterlockedAdd64(&x->silence, (LONG64)x->cap);
        hr = IAudioClient_Start(w->ac);
        if (FAILED(hr))
            return hr;
        x->thr = CreateThread(NULL, 0, wx_thread, x, 0, NULL);
        if (!x->thr) {
            IAudioClient_Stop(w->ac);
            return E_FAIL;
        }
        return S_OK;
    }
    return IAudioClient_Start(w->ac);
}

static void wa_free(WaStream *w)
{
    wx_end(w);                              /* au-lat: the output thread first */
    if (w->ac)
        IAudioClient_Stop(w->ac);
    if (w->clk)
        IAudioClock_Release(w->clk);
    if (w->rc)
        IAudioRenderClient_Release(w->rc);
    if (w->ac)
        IAudioClient_Release(w->ac);
    memset(w, 0, sizeof *w);
}

void bw_wa_release(void)
{
    wa_free(&s_w);
}

/* au-out: the live stream stops and is set aside; the worker lets it go
 * (bw_wa_free_retired), so the frame thread doesn't wait on its release. */
void bw_wa_retire(void)
{
    if (!s_w.ac)
        return;
    wx_end(&s_w);                           /* au-lat */
    IAudioClient_Stop(s_w.ac);
    if (s_old.ac)
        wa_free(&s_old);
    s_old = s_w;
    memset(&s_w, 0, sizeof s_w);
}

void bw_wa_free_retired(void)
{
    wa_free(&s_old);
}

/* au-lat: the device's own format for exclusive mode, at 48 kHz with ch
 * channels: its Windows format (PKEY_AudioEngine_DeviceFormat) when that fits,
 * else the first of the plain ones it takes. 0 if none. */
static const GUID k_float_sub = { 0x00000003, 0x0000, 0x0010,
                                  { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
static const PROPERTYKEY k_devfmt = { { 0xF19F064D, 0x082C, 0x4E27,
                                        { 0xBC, 0x73, 0x68, 0x82, 0xA1, 0xBB, 0x8E, 0x4C } }, 0 };

static void wx_fmt(WAVEFORMATEXTENSIBLE *f, int ch, int bits, int valid, int fl)
{
    memset(f, 0, sizeof *f);
    f->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    f->Format.nChannels = (WORD)ch;
    f->Format.nSamplesPerSec = WA_RATE;
    f->Format.wBitsPerSample = (WORD)bits;
    f->Format.nBlockAlign = (WORD)(ch * bits / 8);
    f->Format.nAvgBytesPerSec = WA_RATE * f->Format.nBlockAlign;
    f->Format.cbSize = (WORD)(sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX));
    f->Samples.wValidBitsPerSample = (WORD)valid;
    f->dwChannelMask = ch == 6 ? 0x3F : 0x3;
    f->SubFormat = fl ? k_float_sub : k_pcm;
}

static int wx_kind(const WAVEFORMATEXTENSIBLE *f)
{
    int fl = IsEqualGUID(&f->SubFormat, &k_float_sub);
    int b = f->Format.wBitsPerSample, v = f->Samples.wValidBitsPerSample ? f->Samples.wValidBitsPerSample : b;
    if (fl)
        return b == 32 ? WX_F32 : -1;
    if (!IsEqualGUID(&f->SubFormat, &k_pcm))
        return -1;
    if (b == 16)
        return WX_S16;
    if (b == 24)
        return WX_S24;
    if (b == 32)
        return v == 24 ? WX_S24_32 : WX_S32;
    return -1;
}

static int wx_pick(IAudioClient *ac, IMMDevice *dev, int ch, WAVEFORMATEXTENSIBLE *out, char *desc, size_t dn)
{
    static const int cand[][3] = { { 16, 16, 0 }, { 32, 24, 0 }, { 32, 32, 1 }, { 24, 24, 0 }, { 32, 32, 0 } };
    IPropertyStore *ps = NULL;
    WAVEFORMATEXTENSIBLE f;
    size_t i;
    int k;
    if (SUCCEEDED(IMMDevice_OpenPropertyStore(dev, STGM_READ, &ps))) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(IPropertyStore_GetValue(ps, &k_devfmt, &pv)) && pv.vt == VT_BLOB
                && pv.blob.cbSize >= sizeof(WAVEFORMATEX)) {
            const WAVEFORMATEX *d = (const WAVEFORMATEX *)pv.blob.pBlobData;
            if (d->nSamplesPerSec == WA_RATE && d->nChannels == ch) {
                if (d->wFormatTag == WAVE_FORMAT_EXTENSIBLE && pv.blob.cbSize >= sizeof(WAVEFORMATEXTENSIBLE))
                    memcpy(&f, d, sizeof f);
                else
                    wx_fmt(&f, ch, d->wBitsPerSample, d->wBitsPerSample, d->wFormatTag == 3);
                f.dwChannelMask = ch == 6 ? 0x3F : 0x3;
                if ((k = wx_kind(&f)) >= 0
                        && IAudioClient_IsFormatSupported(ac, AUDCLNT_SHAREMODE_EXCLUSIVE, (WAVEFORMATEX *)&f, NULL) == S_OK) {
                    *out = f;
                    snprintf(desc, dn, "the device's own format, %u-bit%s", f.Format.wBitsPerSample,
                             k == WX_F32 ? " float" : k == WX_S24_32 ? " (24 valid)" : "");
                    PropVariantClear(&pv);
                    IPropertyStore_Release(ps);
                    return 1;
                }
            }
        }
        PropVariantClear(&pv);
        IPropertyStore_Release(ps);
    }
    for (i = 0; i < sizeof cand / sizeof cand[0]; i++) {
        wx_fmt(&f, ch, cand[i][0], cand[i][1], cand[i][2]);
        if (IAudioClient_IsFormatSupported(ac, AUDCLNT_SHAREMODE_EXCLUSIVE, (WAVEFORMATEX *)&f, NULL) == S_OK) {
            *out = f;
            snprintf(desc, dn, "%d-bit%s (the device's own format didn't fit)", cand[i][0],
                     cand[i][2] ? " float" : cand[i][1] == 24 && cand[i][0] == 32 ? " (24 valid)" : "");
            return 1;
        }
    }
    return 0;
}

/* au-lat: w->ac (activated on dev) initialized in exclusive mode, event-driven
 * at the device's minimum period; the output thread's context made (started
 * by wa_start). On a failure the caller activates the client again and opens
 * the shared stream. */
static HRESULT wx_initialize(WaStream *w, IMMDevice *dev, const char **step, char *how, size_t hown)
{
    WAVEFORMATEXTENSIBLE f;
    REFERENCE_TIME dp = 0, mp = 0, per;
    char fd[120];
    const char *e = getenv("BW_WASAPI_XPERIOD");
    WxCtx *x;
    UINT32 n = 0;
    HRESULT hr;
    *step = "exclusive mode: the device's format";
    if (!wx_pick(w->ac, dev, w->ch, &f, fd, sizeof fd))
        return AUDCLNT_E_UNSUPPORTED_FORMAT;
    *step = "exclusive mode: GetDevicePeriod";
    hr = IAudioClient_GetDevicePeriod(w->ac, &dp, &mp);
    if (FAILED(hr))
        return hr;
    per = mp > 0 ? mp : dp;
    if (e && *e && atof(e) > 0) {
        REFERENCE_TIME want = (REFERENCE_TIME)(atof(e) * 10000.0);
        per = want < mp ? mp : want;
    }
    *step = "exclusive mode: Initialize";
    hr = IAudioClient_Initialize(w->ac, AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, per, per,
                                 (const WAVEFORMATEX *)&f, NULL);
    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        /* the driver's own alignment: that many frames, as a period */
        if (FAILED(IAudioClient_GetBufferSize(w->ac, &n)) || !n)
            return hr;
        per = (REFERENCE_TIME)(10000000.0 * n / WA_RATE + 0.5);
        IAudioClient_Release(w->ac);
        w->ac = NULL;
        *step = "exclusive mode: Activate (aligned)";
        hr = IMMDevice_Activate(dev, &k_iid_ac, CLSCTX_ALL, NULL, (void **)&w->ac);
        if (FAILED(hr))
            return hr;
        *step = "exclusive mode: Initialize (aligned)";
        hr = IAudioClient_Initialize(w->ac, AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, per, per,
                                     (const WAVEFORMATEX *)&f, NULL);
    }
    if (FAILED(hr))
        return hr;
    x = (WxCtx *)calloc(1, sizeof *x);
    if (!x)
        return E_OUTOFMEMORY;
    x->ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    *step = "exclusive mode: SetEventHandle";
    if (!x->ev || FAILED(hr = IAudioClient_SetEventHandle(w->ac, x->ev))
            || FAILED(hr = IAudioClient_GetBufferSize(w->ac, &x->cap)) || !x->cap) {
        if (x->ev)
            CloseHandle(x->ev);
        free(x);
        return FAILED(hr) ? hr : E_FAIL;
    }
    x->ch = w->ch;
    x->kind = wx_kind(&f);
    x->bps = f.Format.wBitsPerSample / 8;
    w->x = x;
    w->event = 1;
    w->period = (int)x->cap;
    w->mix_rate = WA_RATE;
    snprintf(how, hown, "EXCLUSIVE (BW_WASAPI_EXCLUSIVE), event-driven, the device's period %u frames (%.2f ms;"
             " its minimum %.2f, default %.2f ms), %s", x->cap, x->cap / 48.0, mp / 1e4, dp / 1e4, fd);
    return S_OK;
}

/* Initialize w->ac (activated on dev) for fmt. Event-driven when wanted, at
 * IAudioClient3's smallest shared period when the device offers one smaller
 * than its default and the buffer it gives holds the pacing's queue. On a
 * failure of the smaller period, or of the event, the client is activated
 * again and the plainer stream tried. */
static HRESULT wa_initialize(WaStream *w, IMMDevice *dev, const WAVEFORMATEXTENSIBLE *fmt,
                             const WAVEFORMATEX *mix, const char **step, char *how, size_t hown)
{
    DWORD base = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    int ev = wa_event_wanted();
    HRESULT hr;
    REFERENCE_TIME dp = 0, mp = 0;
    snprintf(how, hown, "polled");
    if (wa_excl_wanted()) {
        /* au-lat: exclusive mode (a test switch), else the shared stream */
        const char *xs = "exclusive mode";
        hr = wx_initialize(w, dev, &xs, how, hown);
        if (SUCCEEDED(hr))
            return S_OK;
        fprintf(stderr, "[XA2] WASAPI: %s failed (0x%08lX%s): the shared stream instead\n", xs, (unsigned long)hr,
                hr == AUDCLNT_E_DEVICE_IN_USE ? ", the device is in use" :
                hr == AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED ? ", Windows doesn't allow exclusive use of it" :
                hr == AUDCLNT_E_UNSUPPORTED_FORMAT ? ", no 48 kHz format of ours" : "");
        if (w->ac)
            IAudioClient_Release(w->ac);
        w->ac = NULL;
        *step = "Activate";
        hr = IMMDevice_Activate(dev, &k_iid_ac, CLSCTX_ALL, NULL, (void **)&w->ac);
        if (FAILED(hr))
            return hr;
        snprintf(how, hown, "polled");
    }
    if (ev) {
        IAudioClient3 *ac3 = NULL;
        UINT32 p_def = 0, p_fund = 0, p_min = 0, p_max = 0, want = 0;
        const char *e = getenv("BW_WASAPI_PERIOD");
        if (SUCCEEDED(IAudioClient_QueryInterface(w->ac, &k_iid_ac3, (void **)&ac3))) {
            if (SUCCEEDED(IAudioClient3_GetSharedModeEnginePeriod(ac3, mix, &p_def, &p_fund, &p_min, &p_max))
                    && p_fund) {
                if (e && *e)
                    want = (UINT32)strtoul(e, NULL, 10);
                else if (p_min < p_def)
                    want = p_min;
                if (want) {
                    want = want / p_fund * p_fund;
                    if (want < p_min)
                        want = p_min;
                    if (want > p_max)
                        want = p_max;
                }
            }
            /* The smaller period's stream takes no conversion flags (seen:
             * AUDCLNT_E_INVALID_STREAM_FLAG with AUTOCONVERTPCM), so it is
             * tried in our 16-bit format as it is, then in the engine's own
             * mix format when that is 48 kHz 32-bit float with our channel
             * count (bw_wa_write then converts each sample). Its buffer is
             * the engine's choice, about two periods (seen: 1056 frames at a
             * 480-frame period): it must hold a period and two of the APU's,
             * and the pacing's marks are then held within it
             * (xa2_marks_samples), so its queue can't grow as far on a busy
             * PC as the 100 ms buffer lets it. */
            UINT32 need = (UINT32)((uint64_t)want * WA_RATE / (mix->nSamplesPerSec ? mix->nSamplesPerSec : WA_RATE))
                          + 512;
            int att, mix_ok = mix->nSamplesPerSec == WA_RATE && mix->nChannels == w->ch
                && mix->wBitsPerSample == 32
                && (mix->wFormatTag == 3 /* WAVE_FORMAT_IEEE_FLOAT */
                    || (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix->cbSize >= 22
                        && ((const WAVEFORMATEXTENSIBLE *)mix)->SubFormat.Data1 == 3));
            for (att = 0; want && ac3 && att < 2; att++) {
                const WAVEFORMATEX *f = att == 0 ? (const WAVEFORMATEX *)fmt : mix;
                if (att == 1 && !mix_ok)
                    break;
                *step = "InitializeSharedAudioStream";
                hr = IAudioClient3_InitializeSharedAudioStream(ac3, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, want, f, NULL);
                if (SUCCEEDED(hr) && SUCCEEDED(IAudioClient_GetBufferSize(w->ac, &w->cap))
                        && w->cap >= need && SUCCEEDED(IAudioClient_SetEventHandle(w->ac, s_ev))) {
                    IAudioClient3_Release(ac3);
                    w->event = 1;
                    w->lowlat = 1;
                    w->fl32 = att == 1;
                    w->period = (int)((uint64_t)want * WA_RATE / (mix->nSamplesPerSec ? mix->nSamplesPerSec : WA_RATE));
                    snprintf(how, hown, "event-driven, IAudioClient3 shared period %u frames (%.2f ms; device"
                             " default %u, %u..%u)%s", want, want * 1000.0 / mix->nSamplesPerSec, p_def, p_min,
                             p_max, w->fl32 ? ", in the engine's float format" : "");
                    return S_OK;
                }
                fprintf(stderr, "[XA2] WASAPI: the shared period of %u frames in %s %s (0x%08lX, buffer %u frames)\n",
                        want, att == 0 ? "our 16-bit format" : "the engine's float format",
                        FAILED(hr) ? "was refused" : "gives too small a buffer", (unsigned long)hr, w->cap);
                w->cap = 0;
                IAudioClient3_Release(ac3);
                ac3 = NULL;
                IAudioClient_Release(w->ac);
                w->ac = NULL;
                *step = "Activate";
                hr = IMMDevice_Activate(dev, &k_iid_ac, CLSCTX_ALL, NULL, (void **)&w->ac);
                if (FAILED(hr))
                    return hr;
                if (FAILED(IAudioClient_QueryInterface(w->ac, &k_iid_ac3, (void **)&ac3)))
                    ac3 = NULL;
            }
            if (want)
                fprintf(stderr, "[XA2] WASAPI: the device's default period instead\n");
            if (ac3)
                IAudioClient3_Release(ac3);
        }
        *step = "Initialize (shared mode, event-driven)";
        hr = IAudioClient_Initialize(w->ac, AUDCLNT_SHAREMODE_SHARED, base | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                     WA_BUFFER_HNS, 0, (const WAVEFORMATEX *)fmt, NULL);
        if (SUCCEEDED(hr))
            hr = IAudioClient_SetEventHandle(w->ac, s_ev);
        if (SUCCEEDED(hr)) {
            w->event = 1;
            snprintf(how, hown, "event-driven");
        } else {
            fprintf(stderr, "[XA2] WASAPI: the event-driven stream was refused (0x%08lX): polled instead\n",
                    (unsigned long)hr);
            IAudioClient_Release(w->ac);
            w->ac = NULL;
            *step = "Activate";
            hr = IMMDevice_Activate(dev, &k_iid_ac, CLSCTX_ALL, NULL, (void **)&w->ac);
            if (FAILED(hr))
                return hr;
            ev = 0;
        }
    }
    if (!ev) {
        *step = "Initialize (shared mode)";
        hr = IAudioClient_Initialize(w->ac, AUDCLNT_SHAREMODE_SHARED, base, WA_BUFFER_HNS, 0,
                                     (const WAVEFORMATEX *)fmt, NULL);
        if (FAILED(hr))
            return hr;
    }
    /* the engine's period: what the device runs the shared engine at */
    if (SUCCEEDED(IAudioClient_GetDevicePeriod(w->ac, &dp, &mp)) && dp > 0)
        w->period = (int)(dp * WA_RATE / 10000000);
    if (w->period <= 0)
        w->period = 480;
    return S_OK;
}

/* Open a stream on Windows' default output into w (not started). At boot it
 * picks the layout into *channels; later opens keep it, whatever the device
 * has: Windows converts (AUTOCONVERTPCM). */
static HRESULT wa_open_into(WaStream *w, int boot, int quiet, int *channels)
{
    IMMDeviceEnumerator *en = NULL;
    IMMDevice *dev = NULL;
    WAVEFORMATEX *mix = NULL;
    WAVEFORMATEXTENSIBLE fmt;
    const char *step = "the device enumerator";
    char how[200], desc[440];
    HRESULT hr;
    int dev_ch;

    memset(w, 0, sizeof *w);
    QueryPerformanceFrequency(&s_qpf);
    if (!s_ev)
        s_ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    hr = CoCreateInstance(&k_clsid_mmde, NULL, CLSCTX_ALL, &k_iid_mmde, (void **)&en);
    if (FAILED(hr))
        goto fail;
    step = "the default output device";
    dev = (IMMDevice *)bw_out_test_device();    /* BW_TEST_AUDIO_DEVICE (a test), or none */
    hr = dev ? S_OK : IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev);
    IMMDeviceEnumerator_Release(en);
    if (FAILED(hr))
        goto fail;                          /* 0x80070490: Windows has none */
    step = "Activate";
    hr = IMMDevice_Activate(dev, &k_iid_ac, CLSCTX_ALL, NULL, (void **)&w->ac);
    if (FAILED(hr))
        goto fail;
    step = "GetMixFormat";
    hr = IAudioClient_GetMixFormat(w->ac, &mix);
    if (FAILED(hr) || !mix)
        goto fail;
    dev_ch = mix->nChannels;
    w->mix_rate = mix->nSamplesPerSec;

    if (boot)
        *channels = bw_surround_output_wanted(dev_ch) ? 6 : 2;
    w->ch = *channels;
    if (boot)   /* the same line as the XAudio2 path */
        fprintf(stderr, "[XA2] the audio device has %d channels; output: %s\n", dev_ch,
                w->ch == 6 ? "5.1 (6 channels)" : "stereo");

    memset(&fmt, 0, sizeof fmt);
    fmt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    fmt.Format.nChannels = (WORD)w->ch;
    fmt.Format.nSamplesPerSec = WA_RATE;
    fmt.Format.wBitsPerSample = 16;
    fmt.Format.nBlockAlign = (WORD)(w->ch * 2);
    fmt.Format.nAvgBytesPerSec = WA_RATE * fmt.Format.nBlockAlign;
    fmt.Format.cbSize = (WORD)(sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX));
    fmt.Samples.wValidBitsPerSample = 16;
    fmt.dwChannelMask = w->ch == 6 ? 0x3F : 0x3;   /* FL FR FC LFE BL BR, or FL FR */
    fmt.SubFormat = k_pcm;

    hr = wa_initialize(w, dev, &fmt, mix, &step, how, sizeof how);
    if (FAILED(hr))
        goto fail;
    step = "GetBufferSize";
    hr = IAudioClient_GetBufferSize(w->ac, &w->cap);
    if (FAILED(hr))
        goto fail;
    if (w->x)
        w->cap = WX_RING;                   /* au-lat: exclusive, the pacing's buffer is the ring */
    step = "the render client";
    hr = IAudioClient_GetService(w->ac, &k_iid_rc, (void **)&w->rc);
    if (FAILED(hr))
        goto fail;
    if (FAILED(IAudioClient_GetService(w->ac, &k_iid_clock, (void **)&w->clk))
            || FAILED(IAudioClock_GetFrequency(w->clk, &w->clk_freq)) || !w->clk_freq) {
        if (w->clk)
            IAudioClock_Release(w->clk);
        w->clk = NULL;                      /* only the latency log reads it */
    }
    /* au-out: the device in full, in one line (the bug report reads it) */
    if (!bw_out_describe(dev, desc, sizeof desc))
        snprintf(desc, sizeof desc, "\"?\"");
    snprintf(w->line, sizeof w->line, "%s; WASAPI %s, %s, engine period %d frames (%.2f ms); our stream"
             " %d Hz %d ch 16-bit, buffer %u frames (%.1f ms)", desc, w->x ? "exclusive" : "shared", how,
             w->period, w->period / 48.0, WA_RATE, w->ch, w->cap, w->cap * 1000.0 / WA_RATE);
    if (w->x) {
        w->x->ac = w->ac;
        w->x->rc = w->rc;
    }
    CoTaskMemFree(mix);
    IMMDevice_Release(dev);
    return S_OK;

fail:
    s_err = hr;
    if (!quiet)
        fprintf(stderr, "[XA2] WASAPI: %s failed: 0x%08lX\n", step, (unsigned long)hr);
    if (mix)
        CoTaskMemFree(mix);
    if (dev)
        IMMDevice_Release(dev);
    wa_free(w);
    return hr;
}

long bw_wa_open(int boot, int quiet, int *channels)
{
    HRESULT hr = wa_open_into(&s_w, boot, quiet, channels);
    if (FAILED(hr))
        return (long)hr;
    s_hist_ch = s_w.ch;
    s_w.fresh = !boot;
    if (!boot && wa_hold(0)) {
        /* au-fx: the frame thread's rebuild (BW_AUDIO_REBUILD_WORKER=0): held
         * until it holds the queue's floor, as on the worker's swap */
        fprintf(stderr, "[XA2] output device: %s\n", s_w.line);
        return S_OK;
    }
    hr = wa_start(&s_w);
    if (FAILED(hr)) {
        s_err = hr;
        if (!quiet)
            fprintf(stderr, "[XA2] WASAPI: Start failed: 0x%08lX\n", (unsigned long)hr);
        wa_free(&s_w);
        return (long)hr;
    }
    fprintf(stderr, "[XA2] output device: %s\n", s_w.line);
    if (boot)
        fprintf(stderr, "[XA2] output: WASAPI %s mode (the port's output, every edition; BW_AUDIO_OUT=xaudio2"
                " for XAudio2), %d Hz %d-channel 16-bit stream into a %lu Hz mix, buffer %u frames"
                " (%.1f ms)%s\n", s_w.x ? "exclusive" : "shared", WA_RATE, s_w.ch, s_w.mix_rate, s_w.cap,
                s_w.cap * 1000.0 / WA_RATE, s_w.event ? "; event-driven (BW_WASAPI_EVENT=0 polls)" : "");
    return S_OK;
}

long bw_wa_open_next(int quiet, int channels)
{
    int ch = channels;
    wa_free(&s_n);
    return (long)wa_open_into(&s_n, 0, quiet, &ch);
}

void bw_wa_drop_next(void)
{
    wa_free(&s_n);
}

int bw_wa_adopt_next(int carry, uint64_t *old_consumed)
{
    int tail = 0;
    HRESULT hr;
    *old_consumed = 0;
    if (!s_n.ac)
        return -1;
    if (s_w.ac) {
        UINT32 pad = 0;
        /* the old stream stops first, so nothing it still holds plays twice */
        IAudioClient_Stop(s_w.ac);
        if (carry && SUCCEEDED(IAudioClient_GetCurrentPadding(s_w.ac, &pad)) && pad <= WA_HIST
                && pad <= s_n.cap && s_n.ch == s_hist_ch && (uint64_t)pad <= s_hist_n) {
            BYTE *p = NULL;
            if (pad && SUCCEEDED(IAudioRenderClient_GetBuffer(s_n.rc, pad, &p))) {
                int16_t *d = (int16_t *)p;
                uint64_t k, from = s_hist_n - pad;
                for (k = 0; k < pad; k++) {
                    const int16_t *h = s_hist + ((from + k) % WA_HIST) * s_hist_ch;
                    if (s_n.fl32) {
                        int c;
                        for (c = 0; c < s_n.ch; c++)
                            ((float *)p)[k * s_n.ch + c] = h[c] * (1.0f / 32768.0f);
                    } else {
                        memcpy(d + k * s_n.ch, h, (size_t)s_n.ch * sizeof(int16_t));
                    }
                }
                if (SUCCEEDED(IAudioRenderClient_ReleaseBuffer(s_n.rc, pad, 0))) {
                    tail = (int)pad;
                    s_n.written = pad;
                    s_n.pad = pad;
                }
            }
        } else {
            pad = (UINT32)s_w.pad;          /* gone: the last reading */
            if (FAILED(IAudioClient_GetCurrentPadding(s_w.ac, &pad)))
                pad = s_w.pad;
        }
        *old_consumed = s_w.written >= pad ? s_w.written - pad : 0;
        bw_wa_retire();                 /* let go on the worker */
    }
    s_w = s_n;
    memset(&s_n, 0, sizeof s_n);
    s_w.fresh = tail == 0;
    if (wa_hold((UINT32)tail)) {
        /* au-fx: no tail (or too little) to start on: held until the writes
         * reach the queue's floor (bw_wa_write starts it) */
        fprintf(stderr, "[XA2] output device: %s\n", s_w.line);
        return tail;
    }
    hr = wa_start(&s_w);
    if (FAILED(hr)) {
        s_err = hr;
        fprintf(stderr, "[XA2] WASAPI: Start failed: 0x%08lX\n", (unsigned long)hr);
        wa_free(&s_w);
        return -1;
    }
    fprintf(stderr, "[XA2] output device: %s\n", s_w.line);
    return tail;
}

int bw_wa_active(void)
{
    return s_w.ac != NULL;
}

int bw_wa_capacity(void)
{
    return (int)s_w.cap;
}

int bw_wa_period(void)
{
    return s_w.ac ? s_w.period : 0;
}

int bw_wa_mix_rate(void)
{
    return s_w.ac ? (int)s_w.mix_rate : 0;
}

void *bw_wa_event(void)
{
    return s_w.ac && s_w.event ? (void *)s_ev : NULL;
}

int bw_wa_padding(void)
{
    UINT32 pad = 0;
    HRESULT hr;
    if (!s_w.ac)
        return -1;
    if (s_w.x) {
        /* au-lat: exclusive, what the ring holds (the output thread hands it
         * to the device a period at a time); a failure there is a lost device */
        if (InterlockedCompareExchange(&s_w.x->failed, 0, 0)) {
            s_err = (HRESULT)s_w.x->hr;
            return -1;
        }
        pad = (UINT32)(InterlockedCompareExchange64(&s_xr_w, 0, 0) - InterlockedCompareExchange64(&s_xr_r, 0, 0));
        s_w.pad = pad;
        return (int)pad;
    }
    hr = IAudioClient_GetCurrentPadding(s_w.ac, &pad);
    if (FAILED(hr)) {
        s_err = hr;
        return -1;
    }
    s_w.pad = pad;
    return (int)pad;
}

long bw_wa_write(const int16_t *samples, int n)
{
    BYTE *p = NULL;
    HRESULT hr;
    int k;
    if (!s_w.rc)
        return (long)E_POINTER;
    if (s_w.pad / 16 < WA_QBINS)
        s_qh[s_w.pad / 16]++;
    else
        s_qh[WA_QBINS - 1]++;
    if (s_w.x) {
        /* au-lat: exclusive, into the ring (silence when muted, as below) */
        LONG64 w = s_xr_w, r = InterlockedCompareExchange64(&s_xr_r, 0, 0);
        int j;
        if (w - r + n > WX_RING)
            return 0;                       /* the pacing never lets it fill: dropped */
        for (j = 0; j < n; j++) {
            int16_t *d = s_xr + (size_t)((w + j) % WX_RING) * s_w.ch;
            if (samples)
                memcpy(d, samples + (size_t)j * s_w.ch, (size_t)s_w.ch * sizeof(int16_t));
            else
                memset(d, 0, (size_t)s_w.ch * sizeof(int16_t));
        }
        InterlockedExchange64(&s_xr_w, w + n);
        goto written;
    }
    hr = IAudioRenderClient_GetBuffer(s_w.rc, (UINT32)n, &p);
    if (FAILED(hr)) {
        s_err = hr;
        return (long)hr;
    }
    /* samples NULL (BW_TEST_AUDIO_MUTE, au-int): the same n frames go in as the
     * engine's own silence, so the stream's timing and padding are unchanged */
    if (samples && s_w.fl32) {
        float *f = (float *)p;
        size_t i, m = (size_t)n * s_w.ch;
        for (i = 0; i < m; i++)
            f[i] = samples[i] * (1.0f / 32768.0f);
    } else if (samples)
        memcpy(p, samples, (size_t)n * s_w.ch * sizeof(int16_t));
    hr = IAudioRenderClient_ReleaseBuffer(s_w.rc, (UINT32)n, samples ? 0 : AUDCLNT_BUFFERFLAGS_SILENT);
    if (FAILED(hr)) {
        s_err = hr;
        return (long)hr;
    }
written:
    /* what was written, for the tail a new device takes over (silence when muted) */
    for (k = 0; k < n; k++) {
        int16_t *h = s_hist + ((s_hist_n + (uint64_t)k) % WA_HIST) * s_hist_ch;
        if (samples)
            memcpy(h, samples + (size_t)k * s_w.ch, (size_t)s_hist_ch * sizeof(int16_t));
        else
            memset(h, 0, (size_t)s_hist_ch * sizeof(int16_t));
    }
    s_hist_n += (uint64_t)n;
    s_w.written += (uint64_t)n;
    s_w.pad += (UINT32)n;
    if (s_w.fresh) {
        int ms = wa_test_slowstart_ms();
        s_w.fresh = 0;
        if (ms)
            Sleep((DWORD)ms);                /* the test aid above */
    }
    if (s_w.start_at && s_w.written >= s_w.start_at) {
        /* au-fx: a held stream (it replaced a lost one) holds the queue's
         * floor now: the engine's first pass finds a whole period */
        LARGE_INTEGER q;
        UINT32 at = s_w.start_at;
        s_w.start_at = 0;
        hr = wa_start(&s_w);
        if (FAILED(hr)) {
            s_err = hr;
            fprintf(stderr, "[XA2] WASAPI: Start failed: 0x%08lX\n", (unsigned long)hr);
            return (long)hr;
        }
        QueryPerformanceCounter(&q);
        fprintf(stderr, "[XA2] the new stream started holding %llu frames (%.1f ms; the queue's floor %u),"
                " %.2f ms after it was put in place (BW_WASAPI_PREFILL=0 starts it at once)\n",
                (unsigned long long)s_w.written, s_w.written / 48.0, at,
                s_qpf.QuadPart ? (double)(q.QuadPart - s_w.held_q0) * 1000.0 / (double)s_qpf.QuadPart : 0.0);
    }
    return 0;
}

uint64_t bw_wa_written(void)
{
    return s_w.written;
}

uint64_t bw_wa_consumed(void)
{
    int pad = bw_wa_padding();
    if (pad < 0)
        pad = (int)s_w.pad;                 /* gone: the last reading */
    return s_w.written >= (uint64_t)pad ? s_w.written - (uint64_t)pad : 0;
}

int bw_wa_clock(uint64_t *frame, int64_t *qpc)
{
    UINT64 pos = 0, t = 0;
    if (!s_w.clk || FAILED(IAudioClock_GetPosition(s_w.clk, &pos, &t)))
        return 0;
    *frame = (uint64_t)((double)pos * WA_RATE / (double)s_w.clk_freq);
    if (s_w.x) {
        /* au-lat: exclusive, the device also played the silence the output
         * thread handed it (its first period, any shortage): our frames are
         * the rest */
        uint64_t sil = (uint64_t)InterlockedCompareExchange64(&s_w.x->silence, 0, 0);
        *frame = *frame > sil ? *frame - sil : 0;
    }
    *qpc = (int64_t)((double)t * (double)s_qpf.QuadPart / 1e7);   /* 100-ns units -> counter */
    return 1;
}

void bw_wa_stop(void)
{
    s_w.start_at = 0;                   /* au-fx: a held stream stays stopped too */
    if (s_w.ac)
        IAudioClient_Stop(s_w.ac);
}

long bw_wa_last_error(void)
{
    return (long)s_err;
}

int bw_wa_exclusive(void)
{
    return s_w.ac && s_w.x;
}

long bw_wa_excl_shorts(double *ms)
{
    if (ms)
        *ms = (double)InterlockedCompareExchange64(&s_x_short_frames, 0, 0) / 48.0;
    return InterlockedCompareExchange(&s_x_short, 0, 0);
}

/* The queue just before each write (BW_XA2_STATS): min, 1st percentile and
 * median, in frames, from 16-frame bins (the bin's middle); then cleared. */
int bw_wa_queue_stats(int *mn, int *p1, int *med)
{
    unsigned long n = 0, c = 0;
    int i, got_min = 0, got_p1 = 0;
    for (i = 0; i < WA_QBINS; i++)
        n += s_qh[i];
    if (!n)
        return 0;
    for (i = 0; i < WA_QBINS; i++) {
        if (!s_qh[i])
            continue;
        if (!got_min) {
            *mn = i * 16 + 8;
            got_min = 1;
        }
        c += s_qh[i];
        if (!got_p1 && c * 100 >= n) {
            *p1 = i * 16 + 8;
            got_p1 = 1;
        }
        if (c * 2 >= n) {
            *med = i * 16 + 8;
            break;
        }
    }
    memset(s_qh, 0, sizeof s_qh);
    return (int)n;
}
