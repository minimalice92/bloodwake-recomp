/* bw_latlog.h - controller-latency timestamps (lane lat-m, 2026-11-03; a test
 * aid in every edition, inert unless BW_LATLOG is set). See bw_latlog.c. No
 * windows.h here: carried toolkit files include this. */
#ifndef BW_LATLOG_H
#define BW_LATLOG_H

#include <stdint.h>

/* Event letters (one line each in the file; fields in bw_latlog.c). */
#define BW_LAT_PRESS    'P'   /* USB: the first report that carries a script step */
#define BW_LAT_RELEASE  'R'   /* USB: the first report after that step's hold ended */
#define BW_LAT_PAD      'I'   /* game thread: a pad's polled block changed */
#define BW_LAT_STEP     'S'   /* game thread: a running game step's final state */
#define BW_LAT_FRAME    'F'   /* game thread: a drawn frame begins (message 8) */
#define BW_LAT_FRAMEEND 'f'   /* game thread: ... ends */
#define BW_LAT_FLIP     'X'   /* NV2A thread: the back end's flip, its present */
#define BW_LAT_GPU      'G'   /* watcher thread: the GPU finished a flipped frame */
#define BW_LAT_PCOUNT   'D'   /* NV2A thread: a presented flip's DXGI present count */
#define BW_LAT_STATS    'T'   /* NV2A thread: DXGI frame statistics changed */
#define BW_LAT_DUMPED   'M'   /* NV2A thread: this flip is being frame-dumped */
#define BW_LAT_UPDATE   'U'   /* game thread: a Game_Update that ran steps, with the step clock */
#define BW_LAT_REPORT   'O'   /* USB: a pad report produced for port 0 */
#define BW_LAT_ISR      'Q'   /* USB: the title's USB interrupt handler runs (b: how long, au-usb) */
#define BW_LAT_CPU      'C'   /* USB thread, about 1 s apart: its CPU, the process's, passes, ISRs (au-usb) */
#define BW_LAT_AUDIO    'A'   /* APU: an output period handed over (WAV index, device queue) */
#define BW_LAT_MENU     'N'   /* game thread: the menus' navigation moved this pass (1 up, 2 down) */
/* lat-a (press to sound, stage by stage): */
#define BW_LAT_VOICEON  'V'   /* APU front end: VOICE_ON (a voice starts on the chip) */
#define BW_LAT_VPMIX    'W'   /* VP: the first frame that mixes a voice after its VOICE_ON */
#define BW_LAT_VPSOUND  'Y'   /* VP: that voice's first frame with sound in it */
#define BW_LAT_XBUF     'B'   /* XAudio2: a device buffer submitted (or refused) */
#define BW_LAT_XPERF    'K'   /* XAudio2: its performance data (latency, glitches), about 1 s apart */
#define BW_LAT_XPASS    'J'   /* XAudio2: a processing pass of the source voice starts */
#define BW_LAT_WCLOCK   'H'   /* WASAPI output: the stream's clock (the frame now playing) */
/* au-orig (Original's game-logic proof): */
#define BW_LAT_VOICEOFF 'Z'   /* VP: an active voice ends (at its buffer's end, or the title's VOICE_OFF) */

extern int g_bw_lat_on;
void bw_lat_init(void);
void bw_lat_dump(const char *why);
int64_t bw_lat_now(void);                       /* the host performance counter */
void bw_lat_ev(int type, int64_t t, int64_t a, int64_t b, int64_t c, int64_t d,
               int64_t e, int64_t f, double x, double y);
void bw_lat_ev4(int type, int64_t t, int64_t a, int64_t b, int64_t c, int64_t d,
                int64_t e, double x, double y, double z, double w);
/* The back end's flip count so far (the game thread reads it at each frame). */
extern volatile long g_bw_lat_flips;

#endif
