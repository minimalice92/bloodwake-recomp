/* bw_framelog.h - per-frame event log and long-frame stack sampler (agent
 * P2b, test aid, every edition, off unless BW_FRAMELOG is set). See
 * bw_framelog.c. No windows.h here: recomp_manual.c includes this. */
#ifndef BW_FRAMELOG_H
#define BW_FRAMELOG_H

enum {
    BW_FL_G_FRAME = 1,      /* game thread: Graphics_HandleMessage message 8 begins */
    BW_FL_G_FRAME_END,      /*   ... ends (a = steps run since the last frame, b = the last PUT
                                 handed over, lat-q: matches the frame to its walk and flip) */
    BW_FL_G_UPD,            /*   Game_Update begins */
    BW_FL_G_UPD_END,        /*   Game_Update ends */
    BW_FL_G_KICK,           /*   a KickOff (a = waited us, b = segments out at entry, c = ring seq) */
    BW_FL_G_BOT,            /*   D3D's wait for a GPU time (a = waited us, b = fence, c = GPU time at entry) */
    BW_FL_G_JIT,            /*   lat-q: the just-in-time wait before a pass (a = waited us, b = what it
                                 waited for: 1 the walk, 2 the flip hold, 4 a present-side wait,
                                 8 the present pacing's slot, 16 gave up on an idle walk,
                                 32 started at the beat across a present-side wait;
                                 c = 1 when the guard ended it) */
    BW_FL_N_WALK = 10,      /* NV2A thread: a segment walked (a = PUT, b = us walking, c = GET before) */
    BW_FL_N_FLIP,           /*   the back end's flip (a = us presenting, b = 1 presented / 0 dropped / 2 off-screen) */
    BW_FL_N_HOLD,           /*   a flip hold begins */
    BW_FL_N_HOLD_END,       /*   ... ends (a = us held) */
    BW_FL_N_GPUDONE,        /*   the GPU finished flip n (a = flip, b = us since its submit, c = flips in flight) */
    BW_FL_N_GPUWAIT         /*   the frame-latency limit waited (a = us, b = flips in flight before) */
};

extern int g_bw_fl_on;
void bw_fl_init(void);
void bw_fl_event_(unsigned type, unsigned a, unsigned b, unsigned c);
#define bw_fl_event(t, a, b, c) do { if (g_bw_fl_on) bw_fl_event_((t), (a), (b), (c)); } while (0)
/* Register the calling thread for the stack sampler: 0 the game thread, 1 the NV2A thread. */
void bw_fl_thread(int role);
unsigned long long bw_fl_ticks(void);           /* the host performance counter */
unsigned bw_fl_us_since(unsigned long long t0); /* microseconds since a bw_fl_ticks() */
void bw_fl_dump(const char *why);

#endif
