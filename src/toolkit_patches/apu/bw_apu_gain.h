/* bw_apu_gain.h - the output gain (call GAIN-1, lane au-gain,
 * docs/audio/gain.md) and its test aids. Blood Wake file (part of xbox_apu).
 * No windows.h here.
 *
 * The finished mix (the EP's final mix, or the fold-down's) gets one fixed
 * gain, set from the loudest peak measured across the game so that the peak
 * lands just under -1 dBFS: a pure volume change, every sample scaled by the
 * same factor, no delay, no limiter in the way. A guard stays behind it for
 * a peak louder than any measured: it holds the output under -1 dBFS and
 * logs every time it acts ([APU] BW gain guard), which is to be investigated.
 *
 *   BW_AUDIO_GAIN_DB=<dB>   the gain in any edition and layout (0 = none: the
 *                           output is the mix as it was, bit for bit)
 *   BW_AUDIO_PEAKLOG=1      a test aid: the mix's peaks before the gain, every
 *                           10 s of audio ([APU] BW peak lines)
 *   BW_AUDIO_WAV_PRE=<path> a test aid: the stereo period before the gain as
 *                           a WAV (aligned with RECOMP_AUDIO_WAV's dump), and
 *                           with a 5.1 output the six channels to <path>.6.wav
 *                           (aligned with BW_AUDIO_WAV6's) */
#ifndef BW_APU_GAIN_H
#define BW_APU_GAIN_H

#include <stdint.h>

extern int g_bw_peak;           /* BW_AUDIO_PEAKLOG set (read by bw_gain_init) */

void bw_gain_init(void);        /* reads the switches; any thread, any time */

/* The stereo period (n interleaved pairs), in place: the aids, then the gain.
 * in_wav: the period goes to the WAV dump too (not dropped by a stall). */
void bw_gain_stage2(int16_t *s, int n, int in_wav);
/* The 5.1 period (n interleaved sixes) the device gets, in place. */
void bw_gain_stage6(int16_t *s, int n);

/* BW_AUDIO_PEAKLOG's views upstream of the period, behind g_bw_peak: the
 * voice processor's bins (as floats, 1.0 = full scale, before the 24-bit
 * clamp into the DSPs' mix buffer), and the fold-down's channels before its
 * clamp (stereo, and the six of a 5.1 output). */
void bw_peak_bins(const float *bins, int nbins, int nsamples);
void bw_peak_fold2(float l, float r);
void bw_peak_fold6(const float *ch6);

#endif
