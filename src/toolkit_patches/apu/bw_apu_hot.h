/* bw_apu_hot.h - lane au-hot's test aid (docs/audio/hot.md): BW_APU_HOTSTAT,
 * the APU frame thread's work per WASAPI pass, split by part, to find what
 * makes a battle's periods slow. Inert when off: every hook sits behind
 * g_bw_hot (one load and a branch), and nothing else runs. Blood Wake file.
 * No windows.h here.
 *
 * A pass runs from the engine's event (the wake on it) to the write that
 * completes the pass (the critical write). The frame thread files one record
 * per pass in a ring; a writer thread of the aid's own prints and saves them,
 * so the frame thread never writes to a file or stderr for it. */
#ifndef BW_APU_HOT_H
#define BW_APU_HOT_H

#include <stdint.h>

extern int g_bw_hot;            /* BW_APU_HOTSTAT set (bw_hot_init reads it) */

void bw_hot_init(void);         /* once, from the frame thread's start */
int64_t bw_hot_t(void);         /* QPC ticks */

/* The frame thread's parts. DSPF includes DSPW (the wait for the DSP
 * thread), MON includes SUB (the device write), IRQ includes ISR (the
 * guest's service routine), VP includes the voices' own times below. */
enum {
    BW_HOT_ACK,       /* the doorbell ack poll */
    BW_HOT_VP,        /* mcpx_apu_vp_frame */
    BW_HOT_DSPF,      /* mcpx_apu_dsp_frame, the wait included */
    BW_HOT_DSPW,      /* wait_done: the frame thread waiting for the DSP thread */
    BW_HOT_MON,       /* mcpx_apu_monitor_frame, the write included */
    BW_HOT_SUB,       /* xa2_submit_samples: the WASAPI write */
    BW_HOT_IRQ,       /* apu_deliver_irq, the routine included */
    BW_HOT_ISR,       /* the guest's interrupt service routine */
    BW_HOT_YLD,       /* the per-frame lock hand-off (unlock, yield, relock) */
    BW_HOT_PACE,      /* throttle's own work, outside its wait */
    BW_HOT_NSEG
};

void bw_hot_seg(int seg, int64_t ticks);
/* A part that may hold the critical write: its time up to the write counts
 * in that pass. */
void bw_hot_begin(int seg, int64_t t);
void bw_hot_end(int seg, int64_t t);
/* One frame iteration's work, from throttle's return through the yield. */
void bw_hot_frame_begin(int64_t t0);
void bw_hot_frame(int64_t t0, int64_t t1);
/* throttle's wait returned: on the event (got) after waiting `waited`
 * ticks; t_lock is when the lock was taken again. */
void bw_hot_wake(int64_t t_ret, int64_t t_lock, int64_t waited, int got);
/* The critical write (apu_xaudio2.c): pad was below a pass and this write of
 * n makes it whole; slack_us is xa2's estimate of the time to the next pass. */
void bw_hot_cw(int pad, int n, int64_t slack_us);
void bw_hot_submit(void);       /* any period submitted */
/* A voice processed (apu_vp.c): flags BW_HOT_V_*, its whole time, the
 * resampler's part, and the sinc taps it took. */
enum { BW_HOT_V_3D = 1, BW_HOT_V_HRTF = 2, BW_HOT_V_SINC = 4, BW_HOT_V_ADPCM = 8,
       BW_HOT_V_STREAM = 16, BW_HOT_V_STEREO = 32, BW_HOT_V_MP = 64 };
void bw_hot_voice(int flags, int64_t t_all, int64_t t_res, int taps);
/* The rest of a voice's time by part (TSC cycles), and a denormal census of
 * its float state after the frame: the HRTF's current coefficients, its ITD,
 * its history, the I3DL2 filter's state, and the samples the voice mixed. */
enum { BW_HOT_P_LPF, BW_HOT_P_I3DL2, BW_HOT_P_HRTF, BW_HOT_P_MIX, BW_HOT_NPART };
enum { BW_HOT_D_COEFF, BW_HOT_D_ITD, BW_HOT_D_HIST, BW_HOT_D_ROOM, BW_HOT_D_OUT, BW_HOT_D_BINS,
       BW_HOT_D_SINC, BW_HOT_NDEN };
void bw_hot_voice_parts(const int64_t part[BW_HOT_NPART], const int den[BW_HOT_NDEN]);
void bw_hot_bins_den(int n);    /* denormals in the mixbins after the VP's frame */
/* BW_APU_VP_FTZ=1 (an experiment only, never a default): flush-to-zero and
 * denormals-are-zero around the VP's frame. Returns the MXCSR to restore. */
unsigned bw_hot_ftz_enter(void);
void bw_hot_ftz_leave(unsigned saved);
extern int g_bw_vp_ftz;
/* The DSP thread: one frame's wall time (ticks) and the thread's cycles in it. */
void bw_hot_dsp_frame(int64_t wall, int64_t ran_cyc);
/* The frame thread's wait for the DSP thread: how many frames it was behind. */
void bw_hot_dsp_lag(int lag);
/* The thread's own cycle counter (QueryThreadCycleTime). */
int64_t bw_hot_cycles(void);

/* Lane au-vp (docs/audio/vp.md): the watch. The frame thread marks the stage
 * it is in (a seqlock of a few words, the frame thread only); a watcher
 * thread of the aid's own checks it every millisecond, and when a stage runs
 * 3 ms past what it should take (a wait: what it asked for; work: nothing),
 * it names what holds the thread: its state and wait reason, its priority,
 * whether it ran, the lock's owner and both their stacks, each processor's
 * DPC and interrupt time, the page faults. BW_APU_HOTSTAT_WATCH=0 turns the
 * watcher off (the aid's per-pass records stay). */
enum { BW_ST_LOOP, BW_ST_WAIT, BW_ST_RELOCK, BW_ST_PACE, BW_ST_TIMEDWAIT, BW_ST_ACK, BW_ST_VP,
       BW_ST_DSP, BW_ST_DSPWAIT, BW_ST_MON, BW_ST_WRITE, BW_ST_IRQ, BW_ST_ISR, BW_ST_YIELD,
       BW_ST_YRELOCK, BW_ST_TRAPWAIT, BW_ST_IDLE, BW_ST_N };
/* the frame thread enters stage st, which should take expect_us at most */
void bw_hot_stage(int st, int64_t expect_us);
/* the APU's lock (a CRITICAL_SECTION), so the watcher can name its owner */
void bw_hot_set_lock(void *cs);

#endif
