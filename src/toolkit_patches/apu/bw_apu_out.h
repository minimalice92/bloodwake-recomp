/* bw_apu_out.h - the audio output's helpers (lane au-out, docs/audio/out.md):
 * MMCSS for the sound threads, the worker that opens a lost or changed
 * device off the frame thread, and the one-line description of an output
 * device. Blood Wake file. No windows.h here. */
#ifndef BW_APU_OUT_H
#define BW_APU_OUT_H

#include <stddef.h>

/* The calling thread joins an MMCSS task, every edition since 0.3.0
 * (BW_AUDIO_MMCSS = pro | audio | 0; "audio" otherwise). who names the
 * thread in the log line. */
void bw_audio_thread_mmcss(const char *who);

/* Lane au-vp (docs/audio/vp.md, bw_apu_out.c): the WAV capture
 * (RECOMP_AUDIO_WAV) through a writer thread of its own, the same bytes in
 * the same order (BW_AUDIO_WAV_ASYNC=0: the frame thread writes, as
 * before); started: once after apu_wav_open; close: drains, then closes. And
 * the process's power throttling, once: with BW_PROC_QOS=1, always high QoS
 * and the timer resolution it asks for (unset: left to Windows, the default;
 * BW_TEST_ECOQOS=1 forces EcoQoS, a test switch). */
#include <stdint.h>
void bw_wav_write(const int16_t *s, int n);
void bw_wav_started(void);
void bw_wav_close(void);
void bw_wav_close_nowait(int wait_ms);   /* the run's end (main.c): the ring drained, at most wait_ms */
void bw_proc_qos(void);

/* One job at a time on the output worker (a normal-priority thread with COM
 * started for it, made at the first post). post: 0 when a job is still
 * running. poll: 1 once the job has finished, with what it returned and how
 * long it ran; the slot is then free again. */
int bw_out_post(long (*fn)(void *), void *arg);
int bw_out_poll(long *result, double *ms);
int bw_out_busy(void);

/* A device in one line: "<Windows' full name>": mix <rate> Hz <n> ch
 * <bits>-bit <int|float> (mask 0x..). dev is an IMMDevice *, or NULL for
 * Windows' default output (render, console). Returns 0 if there is none. */
int bw_out_describe(void *dev, char *buf, size_t n);

/* BW_TEST_AUDIO_DEVICE=<part of a name> (a test switch): the active output
 * whose Windows name contains it, opened instead of Windows' default, so a
 * second device can be measured without changing the default. NULL when
 * unset or not found (an IMMDevice * the caller releases). _xa2: the same
 * device as XAudio2's CreateMasteringVoice names it; 0 if none. */
void *bw_out_test_device(void);
int bw_out_test_device_xa2(wchar_t *buf, size_t n);

/* au-lat: the game's running steps. note: on the game thread at each running
 * step (src/bw_hifps_hooks.c; host-side only). since: microseconds since the
 * last one, -1 before the first. steps: how many so far. */
void bw_audio_note_step(void);
long long bw_audio_since_step_us(void);
long bw_audio_steps(void);

#endif
