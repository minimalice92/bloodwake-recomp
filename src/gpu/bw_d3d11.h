/* bw_d3d11.h - Blood Wake D3D11 render back end (BW_GPU=d3d11). See bw_d3d11.c. */
#ifndef BW_D3D11_H
#define BW_D3D11_H

/* Register the back end with the pushbuffer executor when BW_GPU=d3d11.
 * Call before the title starts submitting GPU work. */
void bw_d3d11_register(void);

/* Remastered high frame rate (main.c, gameplay in every edition but Original):
 * Present's sync interval (0 or 1), ALLOW_TEARING for variable refresh, and
 * whether the flipped frame is read back into guest memory. Call before the
 * first frame. Original never calls it. */
void bw_d3d11_present_config(unsigned sync_interval, int allow_tearing, int readback);
/* P2b: at most max_frames flipped frames unfinished on the GPU (0 = no limit);
 * BW_GPU_LATENCY overrides. Remastered's high frame rate sets it. */
void bw_d3d11_latency_config(int max_frames);
/* lat-q: the limit while the frame cap is being reached (0 = the same). */
void bw_d3d11_latency_capped(int max_frames);
/* lat-q: watch whether the frame cap is being reached (g_bw_cap_reached,
 * transitions logged); BW_HIFPS_JIT=3 sets it. Original never calls it. */
void bw_d3d11_cap_watch(int on);
/* lat-q: the present schedule's period follows what the frames sustain
 * (BW_HIFPS_JIT=4, the head start; a test switch). Original never calls it. */
void bw_d3d11_beat_auto(int on);
/* P2b: presents no sooner than 1/hz s apart (0 = off); BW_HIFPS_PACE=0 overrides. */
void bw_d3d11_pace_config(unsigned hz);
/* Remastered widescreen (bw_widescreen.h): take this boot's factor and, when
 * it is above 1, listen for the in-stream 2D markers. main.c calls it after
 * bw_ws_init(), before the title runs. Does nothing at k == 1 (Original). */
void bw_d3d11_ws_start(void);

#endif
