/* bw_apu_wasapi.h - the APU's output straight to Windows' shared audio engine
 * (every edition since 0.3.0; lanes lat-a, au-out and au-orig). See
 * bw_apu_wasapi.c. Only the carried apu_xaudio2.c calls these. No windows.h
 * here. */
#ifndef BW_APU_WASAPI_H
#define BW_APU_WASAPI_H

#include <stdint.h>

/* This boot takes the WASAPI output: every edition by default (lane au-orig,
 * docs/audio/orig.md). BW_AUDIO_OUT=xaudio2 keeps XAudio2 in any edition, and
 * in Remastered and later so does BW_FIX=-AUDIO_WASAPI. Read once. */
int bw_wa_wanted(void);

/* A shared-mode stream on the default output device, started. At boot it
 * picks the layout (2, or 6 for 5.1) into *channels; later opens keep it.
 * quiet: no failure lines. Returns an HRESULT (as a long): 0x80070490 when
 * Windows has no output device. Logs the device line ("[XA2] output device:"). */
long bw_wa_open(int boot, int quiet, int *channels);
void bw_wa_release(void);
int bw_wa_active(void);

/* au-out: a rebuild. open_next (on the output worker) opens a stream on the
 * default device into a second slot, not started, with the boot's layout.
 * adopt_next (the frame thread, at a period boundary) swaps it in: the live
 * stream (if any) is stopped; with carry, what it held and hadn't played is
 * written to the new one first (returned: frames carried; -1 if the swap
 * failed); *old_consumed gets what the old stream had played. drop_next lets
 * an opened stream go unused. au-fx: a new stream that holds less than the
 * queue's floor (no tail: the old device was lost) is held, not started,
 * until the writes reach the floor (bw_wa_write starts it), so the engine's
 * first pass finds a whole period (BW_WASAPI_PREFILL=0: started at once). */
long bw_wa_open_next(int quiet, int channels);
int bw_wa_adopt_next(int carry, uint64_t *old_consumed);
void bw_wa_drop_next(void);
/* The live stream stops and is set aside (adopt_next does this to the old
 * one); free_retired lets it go, on the output worker. */
void bw_wa_retire(void);
void bw_wa_free_retired(void);
/* au-fx: 1, once, after a stream was put in place with no tail to carry (the
 * carried apu_core.c then starts its queue again at the start mark);
 * BW_WASAPI_REBUILD_START=0: never. */
int bw_wa_take_queue_restart(void);

/* Frames written and not yet read by the engine, or -1 (the device is gone:
 * bw_wa_last_error says why). */
int bw_wa_padding(void);
/* The stream's buffer, frames. */
int bw_wa_capacity(void);
/* au-out: the engine's period in 48 kHz frames (0 with no stream), the mix
 * rate, and the event the engine sets once a pass (NULL unless the live
 * stream is event-driven; a HANDLE). */
int bw_wa_period(void);
int bw_wa_mix_rate(void);
void *bw_wa_event(void);
/* Write n interleaved 16-bit frames (n <= capacity - padding); samples NULL
 * writes n frames of silence (BW_TEST_AUDIO_MUTE). 0 or an HRESULT failure
 * (as a long). */
long bw_wa_write(const int16_t *samples, int n);
/* Frames the engine has read from this stream since it opened; frames
 * written to it (consumed = written - padding). */
uint64_t bw_wa_consumed(void);
uint64_t bw_wa_written(void);
/* The stream's own clock: the frame now playing, as Windows reports it, and
 * the performance-counter time of that reading. 0 if there is no clock. */
int bw_wa_clock(uint64_t *frame, int64_t *qpc);
/* Stop consuming (BW_TEST_AUDIO_LOST). */
void bw_wa_stop(void);
long bw_wa_last_error(void);
/* au-out (BW_XA2_STATS): the queue just before each write since the last
 * call, in frames: min, 1st percentile, median. Returns the writes counted. */
int bw_wa_queue_stats(int *mn, int *p1, int *med);

/* au-lat: exclusive mode is asked for (BW_WASAPI_EXCLUSIVE, a test switch,
 * never in a preset; wherever this output runs). 1 also means the rebuild releases the old
 * stream before opening the new one (the frame thread's rebuild). */
int bw_wa_exclusive_wanted(void);
/* au-lat: the live stream is exclusive (0 shared or none); and the periods
 * its output thread handed the device short, with the frames of silence. */
int bw_wa_exclusive(void);
long bw_wa_excl_shorts(double *ms);

#endif
