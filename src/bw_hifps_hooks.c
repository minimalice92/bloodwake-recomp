/*
 * bw_hifps_hooks.c - Remastered high frame rate (D-016; route (b) of
 * docs/remaster/120fps.md, agent R4's design; built by agent D7).
 *
 * The simulation stays at the game's own fixed 30 Hz step. The screen is
 * drawn as often as the display can show it, and the frames between steps
 * show the camera and the scene interpolated between the last two steps. No
 * game rule changes: every step is still 1/30 s, the game's own
 * accumulator decides when one runs, and the simulation never sees an
 * interpolated value (the wrappers put the stepped state back after each
 * drawn frame).
 *
 * Off in Original, always (D-007, D-013): every wrapper below calls the
 * game's own body unchanged unless bw_hifps_boot() switched this boot on,
 * which needs an edition other than Original, the registry entry HIFPS
 * (BW_FIX=-HIFPS turns it off) and a gameplay boot (missions, battles, the
 * attract demo; menus keep the game's own 60 fps).
 *
 * D-011 wrappers (with the D-016 amendment for the enhanced editions): each
 * calls sub_X_gen, the generated body, unchanged. tools/run_pipeline.py hands
 * this file to the generator with recomp_manual.c (--exclude-manual).
 *   0x00091380 Game_Update           writes Game+0x18 = 4 (the developers'
 *                                     game-speed mode, unscaled: the
 *                                     accumulator without its one-step floor,
 *                                     120fps.md §2)
 *   0x00083EF0 Overlays_Update       the last call of each step: snapshot of
 *                                     the camera and the scene nodes; and the
 *                                     step log (BW_STEPLOG, every edition,
 *                                     read-only)
 *   0x00028600 Graphics_HandleMessage message 8 (draw a frame): interpolated
 *                                     state in, the game's own frame, the
 *                                     stepped state back out; the drawn frame
 *                                     rolls its own rand() dice
 *   0x00030250 Sky_UpdateLightning   rolls for a bolt once per step, not
 *                                     once per drawn frame (120fps.md §5)
 *   0x00098130 WaterWorld_EvaluateVertexBatch  reuses a tile within a step
 *                                     (when the water isn't being blended)
 *   0x00084AE0 HudMarkers_Update,
 *   0x00085850 HudReticle_Update     record each step's input, so the drawn
 *                                     frame can project the markers again with
 *                                     the blended camera
 *   0x0002E530 Weather_RenderRain    rain falls by the frame time and spawns
 *                                     once per step (two frame-rate traps)
 * Message 8 also blends the water (both FFT buffers, the WaterWorld clock)
 * and the simulation camera's view matrix; everything is put back after the
 * frame.
 *
 * Switches (gameplay in every edition but Original, which ignores all of them):
 *   BW_HIFPS=<fps>       target frame rate; default: the display's refresh
 *                        rate (a little under it with variable refresh);
 *                        0 = off (the D-013 30 fps, as Original)
 *   BW_HIFPS_VSYNC=1     sync to the display (no tearing; for displays
 *                        without variable refresh)
 *   BW_HIFPS_TEAR=0      no ALLOW_TEARING (variable refresh needs it)
 *   BW_HIFPS_ASYNC=<n>   KickOff segments in flight (default 15; P2b)
 *   BW_GPU_LATENCY=<n>   flipped frames unfinished on the GPU at most
 *                        (default 2 (P2b); 1 with just-in-time frames; 0 = no limit;
 *                        s = each flip waits for its own frame; bw_d3d11.c)
 *   BW_HIFPS_PACE=0      presents no longer held at least 1/target apart (P2b)
 *   BW_HIFPS_SCREENWAIT=0 a present the screen can't take yet is dropped at
 *                        once (D7) instead of waited for briefly (P2b)
 *   BW_HIFPS_INTERP=0    fixed step only, no interpolation (step "b0")
 *   BW_HIFPS_PREDICT=<p> drawn frames predicted p of a step ahead of the
 *                        interpolation (0..1; 1 = they show "now" rather than
 *                        one step ago; default 0 = interpolate), with
 *                        BW_HIFPS_PREDICT_SCOPE=cam|boat|all (default all) and
 *                        BW_HIFPS_PREDICT_WATER=0, BW_HIFPS_PREDICT_FADE=<ms>
 *                        (corrections cross-faded), BW_HIFPS_PREDICT_ORDER=2
 *                        (a parabola through three steps; lat-p, the
 *                        prediction section below)
 *   BW_HIFPS_JIT=0|1|2|3|4 just-in-time frames (lat-q; off by default, D-027): each game
 *                        pass waits, before its steps, pad read and drawing,
 *                        until the frames already drawn are presented (1: the
 *                        walk has caught up and the flip hold's tick is due;
 *                        2: only while the flip hold or a present-side wait
 *                        would queue the frame). BW_HIFPS_JIT_LEAD=<us>
 *                        starts the pass that much before the hold's tick;
 *                        3: only while the frame cap is reached; 4: the head
 *                        start (a test switch; docs/latency/q.md 11-12)
 *   BW_HIFPS_LATMODE=0|1|2 the latency mode (gfx-lat11; 2 by default, the
 *                        maintainer's call; 0 = parity, as before): every
 *                        wait at one gate before the pass, the GPU limit
 *                        there instead of in the flip, the pass aimed at the
 *                        GPU coming free (1), or the head start through the
 *                        gate (2); docs/gfx/lanes/lat11.md
 *   BW_HIFPS_EXTRAP=1    camera extrapolated one step ahead, not interpolated
 *                        (= BW_HIFPS_PREDICT=1 BW_HIFPS_PREDICT_SCOPE=cam)
 *   BW_PREDLOG=<file>    test aid: per drawn frame and per step, what the
 *                        prediction shows against the true poses
 *                        (tools/bw_predict.py reads it)
 *   BW_HIFPS_LIGHTNING=0 the lightning rolls per drawn frame again
 *   BW_HIFPS_RAND=0      drawn frames share the simulation's rand() again
 *   BW_HIFPS_WATER=0     the water stays at 30 Hz (no blend; the ocean cache
 *                        is used instead); BW_HIFPS_WATER_VERIFY=1 checks the
 *                        Water object is byte-identical after each frame
 *   BW_HIFPS_HUD=0       world markers at 30 Hz again
 *   BW_HIFPS_HUD_LERP=0  the HUD replay projects the newest step's world
 *                        points (the reticle's sawtooth of 0.2.4/0.2.5)
 *   BW_AIMLOG=<file>     test aid: the reticle per drawn frame and per step
 *                        (tools/bw_aimstat.py reads it; tools/bw_aimshots.py
 *                        checks it against BW_VFX_SHOT pictures)
 *   BW_HIFPS_RAIN=0      rain per drawn frame again
 *   BW_HIFPS_PFXGROW=0   particles grow (or shrink) by their per-frame factor
 *                        on every drawn frame again (lane wake: the boat's
 *                        wake and spray, smoke, splashes; docs/remaster/wake.md)
 *   BW_HIFPS_PFXJITTER=0 the Stinger's and Wave Gun's bolts re-roll their
 *                        kinks on every drawn frame again (lane au-fx, the
 *                        wake audit's T2; docs/audio/fx.md)
 *   BW_HIFPS_PFXTRAIL=0  trail ribbons (shields, power-ups, the cannon's muzzle
 *                        spark) take a point on every drawn frame again, so
 *                        they shrink with the frame rate (lane au-fx, T1)
 *   BW_HIFPS_BOLTPHASE=0 the Stinger bolt's bend walks on every drawn frame
 *                        again (lane au-fx, T3)
 *   BW_HIFPS_PFXSCRIPT=0 effect group scripts (their waits, spawns and loops)
 *                        run on every drawn frame again (lane au-fx, B+)
 *   BW_HIFPS_PFXDEATH=0  particles end on any drawn frame again, as their
 *                        time-based life runs out, not at the game step's
 *                        frame (lane fx-death); BW_HIFPS_PFXSTOPHOLD=1 (a
 *                        test, off) also holds a stopped emitter to the step
 *   BW_PFXCHECK=<file>   every edition: the per-step particle checker (test
 *                        aid, lane au-fx, read-only): each drawn frame, every
 *                        particle's scale, trail points, bolt re-rolls, the
 *                        bolt's bend and the group scripts' progress and wait
 *                        clocks against an independent reference of
 *                        Original's 30 Hz sequence; a line a second
 *   BW_FXLOG=<file>      every edition: the particle emitters per game step
 *                        (test aid, lane wake); BW_FXLOG_ALL=1 adds every
 *                        emitter with live particles (gate lane V: scale,
 *                        age, life, alpha, trail length, jitter re-rolls per
 *                        step; tools/bw_fpsparity.py)
 *   BW_HIFPS_AUDIT=1     test aid: lists what drawing alone moves in .data
 *   BW_HIFPS_LOG=1       a line a second: frames, steps, nodes lerped/snapped
 *   BW_STEPLOG=<file>    every edition: one line per game step (test aid)
 *   BW_STEPLOG_OBJ=<file> every edition: one line per game step with the
 *                        state sound timing could reach (test aid, lane
 *                        au-orig): the rand() seed, the mission clocks, the
 *                        objective message queue, every objective's flags and
 *                        every boat. Read-only
 *   BW_RAINLOG=1         every edition: rain spawn and fall rates (test aid)
 *   BW_LATLOG=<file>     every edition: controller-latency timestamps (test
 *                        aid, lat-m; bw_latlog.c): each step's input state,
 *                        each drawn frame's newest step and its blend weights,
 *                        the pads as the game's read left them. Read-only
 *
 * Addresses are seen in code (120fps.md §1-§6, and the generated C read
 * again for this file) unless a comment says otherwise.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "bw_edition.h"
#include "gpu/bw_d3d11.h"
#include "gpu/core/bwg_gate.h"   /* gfx-lat11: the latency mode's gate (BW_HIFPS_LATMODE) */
#include "bw_framelog.h"   /* P2b: BW_FRAMELOG test aid */
#include "bw_latlog.h"     /* lat-m: BW_LATLOG test aid (every edition, read-only) */
#include "bw_monitor.h"    /* contract 6: the game's monitor (B1, item 34) */
#include "bw_guestcall.h"  /* au-fx: PFX_ExpandBounds from the trail hold (T1) */

extern ptrdiff_t g_xbox_mem_offset;
extern __declspec(thread) uint32_t g_eax, g_ecx, g_edx, g_esp;   /* RECOMP_TLS (TK-7) */
extern __declspec(thread) uint32_t g_ebp, g_seh_ebp;
extern void bw_set_host_flip_pace(unsigned hz);                  /* carried nv2a_pb_exec.c */

#define HP(va)    ((void *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))
#define HU8(va)   (*(volatile uint8_t  *)HP(va))
#define HU16(va)  (*(volatile uint16_t *)HP(va))
#define HU32(va)  (*(volatile uint32_t *)HP(va))
#define HU64(va)  (*(volatile uint64_t *)HP(va))
#define HF32(va)  (*(volatile float    *)HP(va))

/* Game object (Game_Update's this, 0x1D5C08) */
#define GAME_VA          0x001D5C08u
#define GAME_NOW_QPC     0x00u    /* u64: QPC since start at the last Game_Update */
#define GAME_STEP_QPC    0x08u    /* u64: the step clock, + freq/30 per step */
#define GAME_STATE       0x10u    /* 0 running, 1 paused, 2 ending */
#define GAME_SPEED_MODE  0x18u    /* 0 normal (one-step floor), 1 x2, 2 x1/2, 3 x1/4,
                                     other: x1 with no floor. Game_InitLevel zeroes it */
#define GAME_SPEED_FIXED 4u
/* System object: QPC frequency (System_GetQpcFrequency 0xAB580) */
#define SYS_QPC_FREQ_VA  (0x001E6980u + 0x108u)
/* Weapons clock: +33 ms per running step (Weapons_Update 0x63F00) */
#define WEAPONS_MS_VA    0x001CDDC0u
/* Players (bw_state.c): count at +0xC0, 0x30-byte entries from +8, boat first */
#define PLAYERS_VA       0x001CEE58u
/* Cameras: the sim block 0x1CE4A0 (Camera_Update's this), one 0x21C record
 * per view; Camera_ApplyView (0x78470) builds LookAtLH(eye +0x114, target
 * +0x120, up +0x180) and copies it into the render camera Camera_Get(n+1)
 * = 0x1C70F0 + (n+1)*0xB0, view matrix first. */
#define CAM_VA           0x001CE4A0u
#define CAM_STRIDE       0x21Cu
#define CAM_MODE         0x108u
#define CAM_EYE          0x114u
#define CAM_AT           0x120u
#define CAM_UP           0x180u
#define CAM_NVIEWS_VA    (CAM_VA + 0x98Cu)
#define RCAM_VA(n)       (0x001C70F0u + (uint32_t)(n) * 0xB0u)
#define RCAM_SIZE        0xB0u
#define MAX_VIEWS        4
/* Scene-node pool (NodePool_Init 0x1A400): [0x1A48A4] = base, 6000 x 0x60:
 * matrix +0x00, drawable +0x4C, name hash +0x50, flags +0x56 (0 = free) */
#define NODEPOOL_PTR_VA  0x001A48A4u
#define NODE_N           6000
#define NODE_SIZE        0x60u
#define NODE_DRAWABLE    0x4Cu
#define NODE_HASH        0x50u
#define NODE_FLAGS       0x56u
/* Sky (Sky_UpdateLightning, this = 0x1A9F88): the flash level */
#define SKY_FLASH        0x1813Cu
/* crt_rand (0xE00C3): seed at crt_getptd() + 0x14 */
#define PTD_SEED         0x14u

/* Snap rather than interpolate beyond these (per step; 120fps.md step 3). */
#define SNAP_MOVE_M      30.0f
#define SNAP_DOT         0.70f    /* about 45 degrees */
#define SNAP_CAM_M       50.0f

typedef struct { float m[16]; } Mat;
typedef struct { float eye[3], at[3], up[3]; uint32_t mode; } CamKey;

static int s_on;                 /* this boot: high frame rate on */
static unsigned s_target;
static int s_interp = 1, s_extrap, s_log, s_lightning = 1, s_randsplit = 1;
static int s_pfxgrow = 1;        /* lane wake: BW_HIFPS_PFXGROW (particle growth by frame time) */
static int s_pfxjit = 1;         /* lane au-fx: BW_HIFPS_PFXJITTER (bolt jitter per game step, T2); on again with the timer-field test and the T3 keep (au-fx's crash hunt, docs/audio/fx.md 7.5) */
static int s_pfxtrail = 1;       /* lane au-fx: BW_HIFPS_PFXTRAIL (trail points per game step, T1) */
static int s_hold_frame;         /* lane au-fx: this drawn frame has no new game step (T1, T3 hold) */
static int s_boltphase = 1;      /* lane au-fx: BW_HIFPS_BOLTPHASE (the bolt's bend per game step, T3) */
static int s_pfxscript = 1;      /* lane au-fx: BW_HIFPS_PFXSCRIPT (effect group scripts per game step) */
static int s_pfxdeath = 1;       /* lane fx-death: BW_HIFPS_PFXDEATH (particles end on the step grid) */
static int s_pfxstophold;        /* lane fx-death: BW_HIFPS_PFXSTOPHOLD=1 (stopped emitters held to the step); off:
                                    no scene tried ever reached it (docs/audio/fxdeath.md 5) */
static int s_pf_on;              /* lane au-fx (B+): this drawn frame is a running one, the per-step fixes apply */
static uint32_t s_pf_m;          /* lane au-fx (B+): the game steps this drawn frame follows */
static float s_pf_alpha;         /* lane au-fx (B+): this frame's step fraction (step_alpha) */
static float s_pf_since;         /* lane au-fx (B+): the frame times summed since the last step's frame */
static int s_pf_since_ok;
static LARGE_INTEGER s_bp_t0;    /* lane au-fx (B+): BW_HIFPS_LOG's cost line */
static unsigned long long s_st_bp_ticks;
static unsigned long s_st_bp_frames;
static int s_in_update;          /* inside Game_Update */
static uint32_t s_steps_since_draw, s_snapshots;
static unsigned s_jit;           /* lat-q: BW_HIFPS_JIT (0 off, 1, 2, 3 only at the cap, 4 the head start) */
static uint64_t s_jit_lead_ns;   /* lat-q: BW_HIFPS_JIT_LEAD, in ns */
static int s_jit_pace = 1;      /* lat-q: BW_HIFPS_JIT_PACE (the pacing's slot) */
static int s_jit_beat;          /* lat-q: BW_HIFPS_PACE_ADAPT=1, aim at the steady beat */
static int s_latmode;           /* gfx-lat11: BW_HIFPS_LATMODE (0 parity, 1 the gate, 2 with the head start) */
static int s_pred_ahead_auto;   /* gfx-lat11: BW_HIFPS_PREDICT_AHEAD=auto (the measured horizon) */
static float s_pred_base;       /* ... the prediction without its horizon */
static unsigned long s_st_gate_gpu, s_st_gate_aim;   /* the gate's GPU waits, passes aimed at the GPU */
static double s_st_gate_gpu_ms;
extern volatile uint32_t g_bw_ko_user, g_bw_ko_last_put;        /* recomp_manual.c (lat-q) */

static Mat      s_prev[NODE_N], s_cur[NODE_N], s_wrote[NODE_N];
static uint32_t s_id_prev[NODE_N], s_id_cur[NODE_N];   /* hash ^ drawable | 1, 0 = free */
static uint8_t  s_touched[NODE_N];
static CamKey   s_cam_prev[MAX_VIEWS], s_cam_cur[MAX_VIEWS];
static uint8_t  s_rcam_saved[MAX_VIEWS][RCAM_SIZE];
static float    s_cam_wrote[MAX_VIEWS][16];
static uint8_t  s_scam_saved[MAX_VIEWS][64];   /* simulation view 0x1CE4A8 + 0x40*n */
static unsigned s_scam_written;
static unsigned s_cam_written;
/* HIFPS_EYE (lane gfx-rtwater S0, docs/gfx/lanes/rtwater.md R4): the eye the
 * view was built from, written into each blended view's simulation record
 * (CAM_EYE) for the drawn frame and put back after it. The frame's own code
 * reads that eye: Render_SetFrameConstants 0x33EB0 uploads it as c[146] and
 * copies it to 0x1C26C0, which Mesh_SetupAndDrawLOD 0x36960 takes into each
 * mesh's space as c[156]; Ocean_Render 0x38EC0 copies it for the sea's own
 * c[146] (bw_hifps_eye_ocean below); Sky_Render 0x347A0 centres the dome and
 * the sun on its x and y (seen in the generated C). Before, it stayed the
 * step's: up to about a metre off the drawn camera (tools/hifps_camsweep.py,
 * BW_GPU_TEST_EYE=2). */
static int      s_eye = 1;                       /* HIFPS_EYE and BW_HIFPS_EYE */
static uint8_t  s_eye_step[MAX_VIEWS][12];       /* the step's eye, put back */
static float    s_eye_drawn[MAX_VIEWS][3];       /* the eye this frame's view was built from */
static unsigned s_eye_written;
static unsigned long s_st_eye, s_st_eyeover, s_st_eyeocean;
static unsigned long s_st_cam, s_st_camover;
static unsigned s_nviews, s_nviews_prev, s_rcam_n;
static unsigned long s_st_frames, s_st_steps, s_st_lerped, s_st_snapped, s_st_zero;
static double s_st_alpha;
static uint32_t s_render_seed;
static int s_have_render_seed;
static FILE *s_steplog;
/* The ocean (see sub_00098130 and the message-8 wrapper). */
#define OCEAN_SCROLL_INC0  0x0018F558u  /* per-call increments Ocean_FillVertices adds */
#define OCEAN_SCROLL_INC1  0x0018F55Cu  /* to its shimmer angles 0x1C70B4 / 0x1C70B0 */
static int s_oceanrate = 1, s_oceancache = 1, s_oceanverify;
static int s_water, s_waterverify, s_hudon, s_rain; /* defined further down */
static FILE *s_aimlog;                   /* BW_AIMLOG (test aid, the HUD section) */
static double aim_ms(void);              /* BW_AIMLOG: host ms */
/* The weight each view's camera is drawn at this frame (apply_interp): t, t
 * plus the prediction, or 1 where the camera shows the step (a cut, a camera
 * held at the step); the HUD replay blends its world points by it. */
static float s_hud_u[MAX_VIEWS] = { 1.0f, 1.0f, 1.0f, 1.0f };
static int s_hudlerp;                    /* BW_HIFPS_HUD_LERP; defined further down */
static float s_aim_a;                    /* BW_AIMLOG: this frame's blend factor */
static uint32_t s_water_gen;             /* +1 per running step: the water's state */
static unsigned long s_st_ob, s_st_obhit, s_st_obbad;
/* Prediction (lat-p; the prediction section below). */
#define PRED_CAM   0
#define PRED_BOAT  1
#define PRED_ALL   2
static float    s_pred;                   /* BW_HIFPS_PREDICT: 0 = interpolate */
static int      s_pred_scope = PRED_ALL;  /* BW_HIFPS_PREDICT_SCOPE */
static int      s_pred_water = 1;         /* BW_HIFPS_PREDICT_WATER */
static float    s_pred_fade;              /* BW_HIFPS_PREDICT_FADE: ms, 0 = none */
static int      s_pred_order = 1;         /* BW_HIFPS_PREDICT_ORDER: 1 straight line, 2 parabola */
static float    s_pred_ahead;             /* BW_HIFPS_PREDICT_AHEAD: ms past "now" (in s_pred) */
static int      s_pred_guard;             /* BW_HIFPS_PREDICT_GUARD: lower p while guesses can't hold */
static float    s_pred_guard_rise = 0.1f; /* BW_HIFPS_PREDICT_GUARD_RISE: p's rise per step when calm */
static float    s_pred_now;               /* this frame's p (s_pred, or less under the guard) */
static Mat      s_prev2[NODE_N];          /* the step before s_prev (the fade, order 2) */
static uint32_t s_id_prev2[NODE_N];
static CamKey   s_cam_prev2[MAX_VIEWS];
static unsigned s_nviews_prev2;
static int      s_have_prev2;
static Mat      s_prev3[NODE_N];          /* and the one before (the fade with order 2) */
static uint32_t s_id_prev3[NODE_N];
static CamKey   s_cam_prev3[MAX_VIEWS];
static unsigned s_nviews_prev3;
static int      s_have_prev3;
static uint32_t s_boat_node[MAX_VIEWS];   /* the local players' boats: node index + 1, 0 = none */
static unsigned s_nboats;
static FILE    *s_plog;                   /* BW_PREDLOG */
static void pl_step(uint32_t base, int before);
static void guard_step(void);
static int pl_pad(uint8_t blk[18]);
static void pl_pad_hex(const uint8_t blk[18]);

/* ---- boot ------------------------------------------------------------- */

static unsigned env_u(const char *name, unsigned dflt)
{
    const char *e = getenv(name);
    return e && *e ? (unsigned)strtoul(e, NULL, 10) : dflt;
}

/* The refresh rate of the monitor the game's window will be on: the play
 * host's window (BW_PARENT_HWND, bw_host.c) or the primary monitor, as
 * bw_game_monitor() says (contract 6, shared with widescreen). */
static unsigned display_refresh(void)
{
    DEVMODEA dm;
    const char *dev = NULL;
    MONITORINFOEXA mi;
    HMONITOR mon = bw_game_monitor();
    memset(&dm, 0, sizeof dm);
    dm.dmSize = sizeof dm;
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    if (mon && GetMonitorInfoA(mon, (MONITORINFO *)&mi))
        dev = mi.szDevice;
    if (!EnumDisplaySettingsA(dev, ENUM_CURRENT_SETTINGS, &dm) || dm.dmDisplayFrequency <= 1)
        return 0;
    return (unsigned)dm.dmDisplayFrequency;
}

unsigned bw_hifps_boot(int gameplay)
{
    const char *e = getenv("BW_HIFPS");
    unsigned refresh, hz, vsync, tear;

    if (!gameplay || !bw_edition_at_least(BW_ED_REMASTERED) || !bw_fix_enabled(BW_FIX_HIFPS))
        return 0;
    if (e && *e == '0') {
        fprintf(stderr, "[BW] high frame rate off (BW_HIFPS=0): 30 fps as Original\n");
        return 0;
    }
    refresh = display_refresh();
    vsync = env_u("BW_HIFPS_VSYNC", 0) == 1;
    tear = !vsync && env_u("BW_HIFPS_TEAR", 1) != 0;
    hz = e && *e ? (unsigned)atoi(e) : 0;
    if (!hz) {
        hz = refresh ? refresh : 120;
        /* Variable refresh: stay a little under the display's maximum, so a
         * frame that comes early never tears (the usual practice). */
        if (tear && hz >= 100)
            hz -= hz / 40 + 1;
    }
    if (hz < 30) hz = 30;
    if (hz > 500) hz = 500;
    s_target = hz;
    s_interp = env_u("BW_HIFPS_INTERP", 1) != 0;
    s_extrap = env_u("BW_HIFPS_EXTRAP", 0) == 1;
    {
        /* lat-p: prediction. BW_HIFPS_EXTRAP=1 is the camera alone, a whole
         * step ahead (what it always did); BW_HIFPS_PREDICT overrides. */
        const char *pe = getenv("BW_HIFPS_PREDICT"), *ps = getenv("BW_HIFPS_PREDICT_SCOPE");
        s_pred = s_extrap ? 1.0f : 0.0f;
        s_pred_scope = s_extrap ? PRED_CAM : PRED_ALL;
        if (pe && *pe) {
            double v = atof(pe);
            s_pred = v > 0.0 ? (v < 1.0 ? (float)v : 1.0f) : 0.0f;
        }
        if (ps && *ps)
            s_pred_scope = !_stricmp(ps, "cam") ? PRED_CAM : !_stricmp(ps, "boat") ? PRED_BOAT : PRED_ALL;
        s_pred_water = env_u("BW_HIFPS_PREDICT_WATER", 1) != 0;
        pe = getenv("BW_HIFPS_PREDICT_FADE");
        if (pe && *pe) {
            double v = atof(pe);
            s_pred_fade = v > 0.0 ? (v < 1000.0 / 30.0 ? (float)v : 1000.0f / 30.0f) : 0.0f;
        }
        s_pred_order = env_u("BW_HIFPS_PREDICT_ORDER", 1) == 2 ? 2 : 1;
        pe = getenv("BW_HIFPS_PREDICT_AHEAD");
        if (pe && !_stricmp(pe, "auto") && s_pred > 0.0f) {
            /* gfx-lat11 (arch.md 3.8 item 4): follow the measured horizon,
             * 0.8 of the frame's draw -> screen (or -> GPU done off screen;
             * lat-p's best fixed horizon was about 0.8 of that time), each
             * frame from the latency mode's medians (pred_ahead_follow).
             * Without BW_HIFPS_LATMODE nothing is measured: 0, as unset. */
            s_pred_ahead_auto = 1;
            s_pred_base = s_pred;
        } else if (pe && *pe && s_pred > 0.0f) {
            /* D-027: predict past "now" by this many ms, towards the moment the
             * frame reaches the screen (its drawing, present, GPU and display:
             * docs/latency/p.md, "Predicting to the screen"). It simply adds to
             * the prediction, so every path (nodes, cameras, water, the fade,
             * the guards) follows it. */
            double v = atof(pe);
            s_pred_ahead = v > 0.0 ? (float)(v < 1000.0 / 30.0 ? v : 1000.0 / 30.0) : 0.0f;
            s_pred += s_pred_ahead * (30.0f / 1000.0f);
        }
        /* D-027 (lat-p): the guard. While the next steps can't be guessed (the
         * right stick's look swing, the camera shaking, a fresh contact or ram),
         * p falls to 0 as fast as the display clock may stand still, so the
         * picture never shows a guess there: it waits on the real path, as the
         * interpolation does. When calm, p rises back at this much a step. */
        s_pred_guard = s_pred > 0.0f && env_u("BW_HIFPS_PREDICT_GUARD", 0) == 1;
        pe = getenv("BW_HIFPS_PREDICT_GUARD_RISE");
        if (pe && *pe) {
            double v = atof(pe);
            s_pred_guard_rise = v > 0.01 ? (v < 1.0 ? (float)v : 1.0f) : 0.01f;
        }
        s_pred_now = s_pred;
        pe = getenv("BW_PREDLOG");
        if (pe && *pe && (s_plog = fopen(pe, "a")) != NULL)
            fprintf(s_plog, "# boot %lu: predlog 1, predict %.3f scope %s water %d fade %.1f order %d ahead %.1f guard %d rise %.3f, target %u fps\n",
                    (unsigned long)GetCurrentProcessId(), s_pred,
                    s_pred_scope == PRED_CAM ? "cam" : s_pred_scope == PRED_BOAT ? "boat" : "all",
                    s_pred_water, s_pred_fade, s_pred_order, s_pred_ahead, s_pred_guard,
                    s_pred_guard_rise, hz);
    }
    s_lightning = env_u("BW_HIFPS_LIGHTNING", 1) != 0;
    s_eye = bw_fix_enabled(BW_FIX_HIFPS_EYE) && env_u("BW_HIFPS_EYE", 1) != 0;
    if (!s_eye)
        fprintf(stderr, "[BW] hifps: drawn frames keep the step's camera position (HIFPS_EYE off)\n");
    s_randsplit = env_u("BW_HIFPS_RAND", 1) != 0;
    s_log = env_u("BW_HIFPS_LOG", 0) == 1;
    s_oceanrate = env_u("BW_HIFPS_OCEANRATE", 1) != 0;
    s_oceancache = env_u("BW_HIFPS_OCEANCACHE", 1) != 0;
    s_oceanverify = env_u("BW_HIFPS_OCEAN_VERIFY", 0) == 1;
    s_water = (int)env_u("BW_HIFPS_WATER", 1);     /* 0 off, 1 blend results (fast), 2 blend the field */
    s_waterverify = env_u("BW_HIFPS_WATER_VERIFY", 0) == 1;
    s_hudon = env_u("BW_HIFPS_HUD", 1) != 0;
    s_hudlerp = env_u("BW_HIFPS_HUD_LERP", 1) != 0;   /* lane aim: world points between steps */
    {
        const char *al = getenv("BW_AIMLOG");      /* test aid: the reticle per frame */
        if (al && *al && (s_aimlog = fopen(al, "w")) != NULL)
            fprintf(s_aimlog, "# aimlog 1: F frame ms snapshots steps interp alpha [step replayed obj"
                    " aimx aimy aimcol tgtlink tgtx tgty] cam fx fy fz tx ty tz; S snapshots ms obj"
                    " aimx aimy aimcol tgtlink tgtx tgty\n");
    }
    s_rain = env_u("BW_HIFPS_RAIN", 1) != 0;
    s_pfxgrow = env_u("BW_HIFPS_PFXGROW", 1) != 0;
    s_pfxjit = env_u("BW_HIFPS_PFXJITTER", 1) != 0;   /* on again with au-fx's fix (off on main 04:01-) */
    s_pfxtrail = env_u("BW_HIFPS_PFXTRAIL", 1) != 0;
    s_boltphase = env_u("BW_HIFPS_BOLTPHASE", 1) != 0;
    s_pfxscript = env_u("BW_HIFPS_PFXSCRIPT", 1) != 0;
    s_pfxdeath = env_u("BW_HIFPS_PFXDEATH", 1) != 0;
    s_pfxstophold = env_u("BW_HIFPS_PFXSTOPHOLD", 0) == 1;
    /* lat-q (D-027): off by default for now; BW_HIFPS_JIT=1 turns it on.
     * On the maintainer's screen (with BW_GPU_LATENCY=1): every frame's picture
     * 18.8 -> 13.3 ms old at the screen and judder 5.9 -> 2.0 ms, but the
     * present intervals spread 2.2 -> 6.0 ms (p95-p5), which D-027's
     * smoothness rule doesn't allow; BW_HIFPS_PACE_ADAPT=1 brings it to 4.8
     * only (docs/latency/q.md). */
    s_jit = env_u("BW_HIFPS_JIT", 0);
    if (s_jit > 4)
        s_jit = 1;
    s_jit_pace = env_u("BW_HIFPS_JIT_PACE", 1) != 0;
    s_jit_beat = env_u("BW_HIFPS_PACE_ADAPT", 0) == 1;
    s_jit_lead_ns = (uint64_t)env_u("BW_HIFPS_JIT_LEAD", 0) * 1000u;
    if (s_jit_lead_ns > 20000000u)
        s_jit_lead_ns = 20000000u;
    /* gfx-lat11 (docs/gfx/arch.md 3.3, 3.8, 4): the latency mode, 2 by
     * default since the maintainer's call of 2026-11-04 ("I'll take a 10%
     * drop in frames for 10ms lower latency and half the frame-to-frame
     * variance"; docs/gfx/lanes/lat11.md). BW_HIFPS_LATMODE=0 is parity
     * (every wait inside the flip, as before; Original is parity always: it
     * never gets here). lat-q's own test switches (BW_HIFPS_JIT, and the
     * flip's serial or measuring BW_GPU_LATENCY=s|m) keep their old meaning:
     * set, they mean parity unless BW_HIFPS_LATMODE is set too.
     *   1  the gate: just-in-time frames (as BW_HIFPS_JIT=1) with the GPU
     *      limit held before the pass instead of in the flip (the walk never
     *      waits for the GPU), each frame submitted at its flip, and the pass
     *      aimed to hand its frame over as the GPU comes free (the GPU's own
     *      measured time; BW_HIFPS_LATMODE_GPUAIM=0 leaves the aim at the
     *      present slot only);
     *   2  the gate with the head start (as BW_HIFPS_JIT=4: a steady beat, a
     *      pass may start while one frame is still on the NV2A thread), the
     *      beat never faster than the GPU's measured time per frame
     *      (BW_HIFPS_LATMODE_GPUAIM=0 drops that floor).
     * The GPU limit is BW_GPU_LATENCY when set (a number), else 1. */
    {
        const char *jt = getenv("BW_HIFPS_JIT"), *gl = getenv("BW_GPU_LATENCY");
        unsigned dflt = 2;
        if ((jt && *jt) || (gl && *gl && !(*gl >= '0' && *gl <= '9')))
            dflt = 0;                   /* lat-q's switches, as they were */
        s_latmode = (int)env_u("BW_HIFPS_LATMODE", dflt);
    }
    if (s_latmode > 2)
        s_latmode = 1;
    if (s_latmode) {
        const char *gl = getenv("BW_GPU_LATENCY");
        int lim = gl && *gl >= '0' && *gl <= '9' ? atoi(gl) : 1;
        s_jit = s_latmode == 2 ? 4 : 1;
        bwg_gate_config(s_latmode, lim, env_u("BW_HIFPS_LATMODE_GPUAIM", 1) != 0);
    }
    {
        extern int g_bw_kickoff_async;                 /* recomp_manual.c */
        /* segments the game may run ahead of the walk; 0 = wait for each flush.
         * 15 (the most the KickOff tracks; P2b). P1 kept D7's 3 because at
         * 4 and up mission 3 showed ~35 ms frames; P2b found their cause in
         * the GPU backlog, not the depth (the GPU limit below removes them at
         * any depth), and with it 15 holds in every scene measured: mission 1
         * 201 -> 217 fps, battle 249 -> 282, split 198 -> 222 (quiet window,
         * docs/day1/P2b.md). BW_HIFPS_ASYNC=3 gives the old depth. */
        g_bw_kickoff_async = (int)env_u("BW_HIFPS_ASYNC", 15);
        if (g_bw_kickoff_async > 15)
            g_bw_kickoff_async = 15;
    }
    {
        /* P1: the NV2A thread walks and acknowledges one KickOff segment at
         * a time (bw_wait.c; BW_PB_SEGWALK=0 walks to the newest PUT) */
        extern void bw_segwalk_default(int on);
        bw_segwalk_default(1);
    }
    bw_set_host_flip_pace(hz);
    bw_d3d11_present_config(vsync, tear, 0);
    /* P2b: at most 2 flipped frames unfinished on the GPU. Nothing in the
     * present path waits for the GPU (D7's never-blocking present), so when
     * the GPU is the slower side the NV2A thread queued ~40 frames (half a
     * second) ahead and then stalled inside the driver for a big piece of
     * that backlog at once (docs/day1/P2b.md). BW_GPU_LATENCY=<n> overrides,
     * 0 = no limit as before. lat-q: 1 adds nothing measurable without
     * just-in-time frames (lat-m, on screen), so it stays 2 while those are
     * off. */
    bw_d3d11_latency_config(s_latmode ? bwg_gate_limit() : s_jit == 4 ? 1 : 2);   /* gfx-lat11: the
                                    latency mode's limit, kept in the flip only if its gate can't run */
    if (s_jit == 4)
        bw_d3d11_beat_auto(1);      /* lat-q: the head start's beat (a test switch) */
    if (s_jit == 3) {
        /* lat-q: BW_HIFPS_JIT=3, just-in-time frames only while the frame
         * cap is being reached (the back end watches the presents), with at
         * most 1 frame on the GPU then (the cap's slack pays for it) */
        bw_d3d11_cap_watch(1);
        bw_d3d11_latency_capped(1);
    }
    bw_d3d11_pace_config(hz);
    s_on = 1;
    fprintf(stderr, "[BW] high frame rate (%s): up to %u fps (display %u Hz%s), "
            "fixed 30 Hz game step, %s%s%s\n", bw_edition_title(bw_edition()),
            hz, refresh, vsync ? ", vsync" : tear ? ", variable refresh" : "",
            !s_interp ? "no interpolation" : s_pred > 0.0f ? "predicted" : "interpolated",
            s_lightning ? "" : ", lightning per frame", s_randsplit ? "" : ", shared rand()");
    if (!s_pfxgrow)
        fprintf(stderr, "[BW] high frame rate: particles grow per drawn frame again (BW_HIFPS_PFXGROW=0)\n");
    if (!s_pfxjit)
        fprintf(stderr, "[BW] high frame rate: bolt jitter re-rolls per drawn frame again (BW_HIFPS_PFXJITTER=0)\n");
    if (!s_pfxtrail)
        fprintf(stderr, "[BW] high frame rate: trail ribbons take a point per drawn frame again (BW_HIFPS_PFXTRAIL=0)\n");
    if (!s_boltphase)
        fprintf(stderr, "[BW] high frame rate: the Stinger bolt's bend walks per drawn frame again (BW_HIFPS_BOLTPHASE=0)\n");
    if (!s_pfxscript)
        fprintf(stderr, "[BW] high frame rate: effect group scripts run per drawn frame again (BW_HIFPS_PFXSCRIPT=0)\n");
    if (!s_pfxdeath)
        fprintf(stderr, "[BW] high frame rate: particles end on any drawn frame again (BW_HIFPS_PFXDEATH=0)\n");
    else if (s_pfxstophold)
        fprintf(stderr, "[BW] high frame rate: stopped emitters held to the next step (BW_HIFPS_PFXSTOPHOLD=1, a test)\n");
    if (s_interp && s_pred > 0.0f)
        fprintf(stderr, "[BW] high frame rate: drawn frames predicted %.2f of a step ahead "
                "(%s%s), corrections faded over %.1f ms, order %d, %.1f ms past now%s\n", s_pred,
                s_pred_scope == PRED_CAM ? "the cameras" : s_pred_scope == PRED_BOAT
                ? "the cameras and the players' boats" : "the cameras and every scene node",
                s_pred_scope == PRED_ALL && s_pred_water ? ", the water too" : "",
                s_pred_fade, s_pred_order, s_pred_ahead, s_plog ? "; BW_PREDLOG on" : "");
    if (s_interp && s_pred_guard)
        fprintf(stderr, "[BW] high frame rate: prediction guarded (look stick, camera shake, "
                "contact): it falls back to the real path there, rising %.2f a step when calm\n",
                s_pred_guard_rise);
    {
        extern int g_bw_kickoff_async;
        if (!g_bw_kickoff_async)
            fprintf(stderr, "[BW] high frame rate: KickOff waits for each flush (BW_HIFPS_ASYNC=0)\n");
    }
    if (s_jit)
        fprintf(stderr, "[BW] high frame rate: just-in-time frames (BW_HIFPS_JIT=%u: %s), lead %.2f ms\n",
                s_jit, s_jit == 1 ? "each pass waits until the frames drawn are presented"
                     : s_jit == 3 ? "only while the frame cap is being reached, then with at most 1 frame on the GPU"
                     : s_jit == 4 ? "the head start: a steady beat at the frames' own rate, each pass its own work"
                                    " before its beat, at most one frame on the NV2A thread, 1 on the GPU (a test)"
                                  : "each pass waits only while its frame would queue",
                (double)s_jit_lead_ns / 1e6);
    if (s_latmode)
        fprintf(stderr, "[BW] high frame rate: the latency mode (BW_HIFPS_LATMODE=%d): every wait at one gate before"
                        " the pass, at most %d GPU frame%s unfinished there%s%s\n", s_latmode, bwg_gate_limit(),
                bwg_gate_limit() == 1 ? "" : "s", s_latmode == 1 && bwg_gate_gpu_aim()
                ? ", each pass aimed to hand its frame over as the GPU comes free" : "",
                s_latmode == 2 ? (bwg_gate_gpu_aim() ? ", the head start's beat no faster than the GPU"
                                                     : ", the head start's beat its own (no GPU floor)") : "");
    if (s_pred_ahead_auto)
        fprintf(stderr, "[BW] high frame rate: prediction follows the measured horizon (BW_HIFPS_PREDICT_AHEAD=auto%s)\n",
                s_latmode ? "" : "; nothing is measured without BW_HIFPS_LATMODE, so 0");
    return hz;
}

/* ---- the step log (BW_STEPLOG, every edition, read-only) --------------- */

static void steplog_step(void)
{
    static int init;
    uint32_t n, boat;
    if (!init) {
        const char *p = getenv("BW_STEPLOG");
        init = 1;
        if (p && *p) {
            s_steplog = fopen(p, "a");
            if (s_steplog)
                fprintf(s_steplog, "# boot %lu: wclk qpc_ms x y height throttle rudder revs turbo\n",
                        (unsigned long)GetCurrentProcessId());
        }
    }
    if (!s_steplog)
        return;
    n = HU32(PLAYERS_VA + 0xC0);
    boat = n >= 1 && n <= 4 ? HU32(PLAYERS_VA + 8) : 0;
    {
        uint64_t freq = HU64(SYS_QPC_FREQ_VA), now = HU64(GAME_VA + GAME_NOW_QPC);
        fprintf(s_steplog, "%u %.1f", HU32(WEAPONS_MS_VA),
                freq ? (double)now * 1000.0 / (double)freq : 0.0);
    }
    if (boat >= 0x00010000u && boat < 0x08000000u)
        fprintf(s_steplog, " %.3f %.3f %.3f %.2f %.2f %.0f %.4f",
                HF32(boat + 0x84), HF32(boat + 0x88), HF32(boat + 0x8C),
                HF32(boat + 0x398), HF32(boat + 0x39C), HF32(boat + 0x3B8),
                HF32(boat + 0x474));
    {
        /* the host counter in ms, tagged, last: matches BW_KICKLOG's t= */
        LARGE_INTEGER c, f;
        QueryPerformanceCounter(&c);
        QueryPerformanceFrequency(&f);
        fprintf(s_steplog, " h=%.1f", (double)c.QuadPart * 1000.0 / (double)f.QuadPart);
    }
    fputc('\n', s_steplog);
}

/* ---- the objective log (BW_STEPLOG_OBJ, lane au-orig, every edition) ----- *
 * One line per running step, beside BW_STEPLOG and keyed the same way (the
 * weapons clock), with what the sound's timing could reach in the game
 * (docs/audio/orig.md section 3; NATIVE_AUDIO.md section 5): when a voice
 * line or a sound ends is read through Sound_IsPlaying (0x182C0), by the
 * objective message queue (ObjMsgQueue_Update 0x7D740, through
 * SpotterVO_IsPlayingLine 0x8FA70) and by 15 sound routines that draw rand()
 * only when their sound has ended. So each line has:
 *  - the game thread's rand() seed: crt_rand (0xE00C3) keeps it at +0x14 of
 *    the CRT's per-thread data, found as _getptd (0xE3804) finds it from the
 *    thread's TIB: the thread object fs:[0x28], and when its +0x28 is set the
 *    slot [fs:[4] + [0x2784B8] * 4] + 8, else 0x287984; the pointer stored in
 *    the slot. Read from the step's own thread;
 *  - the mission clock 0x1D3FB8 (Objectives_DrawMissionClock's), the
 *    objectives' clock 0x1D3A58 (Objectives_Update adds each step's time) and
 *    the intro camera's pair 0x1D5C18 / 0x1D5C1C (ObjCond_IntroCamDone);
 *  - the message queue: its slot [0x1D3FEC], and that slot's entry in the
 *    queue [0x1D4000] (24 bytes: flag bytes +0..+3, the message +4, its
 *    time +0xC), as ObjMsgQueue_Update reads them; the spotter's line +0x17C
 *    (the spotter 0x1D57B8);
 *  - every objective's enabled, met and failed flags (count [0x1D3FE0],
 *    array [0x1D3FFC], 0xEC bytes each: +0x24, +0x48, +0x49; bw_state.c), one
 *    hex digit each;
 *  - every boat (Characters 0x1CDEB8 + 0x118: count +0, array [+8], 0x6A4
 *    bytes each, Boats_Add 0x6D8E0): position +0x84/+0x88/+0x8C (the cached
 *    one GameObj_UpdateCachedPosition fills each step) and hitpoints +0x20.
 * Nothing here writes guest memory or calls game code. */
extern __declspec(thread) uint32_t g_fs_base;   /* the toolkit's per-thread TIB (xbox_memory_layout.h) */
static FILE *s_objlog;

static int objlog_ok(uint32_t va)
{
    return va >= 0x1000u && va < 0x08000000u;
}

static uint32_t objlog_ptd(void)
{
    uint32_t fs = g_fs_base, slot, tls, th, at;
    if (!objlog_ok(fs))
        return 0;
    th = HU32(fs + 0x28);
    if (!objlog_ok(th))
        return 0;
    if (HU32(th + 0x28)) {
        tls = HU32(fs + 4);
        at = tls + HU32(0x2784B8u) * 4;          /* the index is negative (seen: -5), as 32-bit */
        if (!objlog_ok(tls) || !objlog_ok(at))
            return 0;
        slot = HU32(at) + 8;
    } else {
        slot = 0x287984u;
    }
    if (!objlog_ok(slot))
        return 0;
    return HU32(slot);
}

/* BW_STEPLOG_OBJ_RAND=1 (with BW_STEPLOG_OBJ; a diagnosis aid, never for a
 * timing or proof run: it costs an exception per rand() call): which code
 * drew each of the game thread's rand() calls since the last step. A
 * hardware write watch (DR0) on the game thread's seed (the per-thread
 * data's +0x14, the one store in crt_rand) traps after each store; the
 * handler reads the guest stack for the return address of the direct call
 * to crt_rand (a call rel32 to 0xE00C3 just before it, checked in the
 * loaded image), and through Rand_Float01 0xAB5A0, Rand_Int 0xAB5C0 and
 * PFX_RandFloat01 0x1EB80 to their caller (marked with the top bit). The
 * line gets " r:" and the callers in order, run-length (caller*count).
 * Read-only: nothing in the guest is written. */
extern __declspec(thread) uint32_t g_esp;
#define RW_MAX 512
static uint32_t s_rw_seq[RW_MAX];
static uint32_t s_rw_cnt[RW_MAX];
static int s_rw_n, s_rw_on;
static unsigned s_rw_lost;
static uintptr_t s_rw_addr;
static HANDLE s_rw_thread;
static const uint32_t k_rw_helpers[3] = { 0x000AB5A0u, 0x000AB5C0u, 0x0001EB80u };

static uint32_t rw_find_ret(uint32_t lo, uint32_t hi, uint32_t target, uint32_t *at)
{
    uint32_t a, r;
    for (a = lo; a <= hi; a += 4) {
        if (!objlog_ok(a))
            continue;
        r = HU32(a);
        if (r > 0x00011005u && r < 0x00180000u && HU8(r - 5) == 0xE8u && (uint32_t)(r + HU32(r - 4)) == target) {
            *at = a;
            return r;
        }
    }
    return 0;
}

static uint32_t rw_caller(void)
{
    uint32_t sp = g_esp, at = 0, r = rw_find_ret(sp - 8, sp + 16, 0x000E00C3u, &at), i, r2;
    if (!r)
        return 0;
    for (i = 0; i < 3; i++)
        if (r >= k_rw_helpers[i] && r < k_rw_helpers[i] + 0x40u) {
            r2 = rw_find_ret(at + 4, at + 64, k_rw_helpers[i], &at);
            return r2 ? (r2 | 0x80000000u) : r;
        }
    return r;
}

static LONG CALLBACK rw_veh(PEXCEPTION_POINTERS x)
{
    uint32_t c;
    if (x->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(x->ContextRecord->Dr6 & 1u) ||
        GetCurrentThreadId() != GetThreadId(s_rw_thread))
        return EXCEPTION_CONTINUE_SEARCH;
    x->ContextRecord->Dr6 &= ~(DWORD64)0xF;
    c = rw_caller();
    if (s_rw_n > 0 && s_rw_seq[s_rw_n - 1] == c)
        s_rw_cnt[s_rw_n - 1]++;
    else if (s_rw_n < RW_MAX) {
        s_rw_seq[s_rw_n] = c;
        s_rw_cnt[s_rw_n] = 1;
        s_rw_n++;
    } else
        s_rw_lost++;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static DWORD WINAPI rw_arm(LPVOID unused)
{
    CONTEXT c;
    (void)unused;
    if (SuspendThread(s_rw_thread) == (DWORD)-1)
        return 1;
    memset(&c, 0, sizeof c);
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (GetThreadContext(s_rw_thread, &c)) {
        c.Dr0 = (DWORD64)s_rw_addr;
        c.Dr7 = (c.Dr7 & ~(DWORD64)0xF0003) | 1u | (1u << 16) | (3u << 18);   /* L0; write; 4 bytes */
        if (SetThreadContext(s_rw_thread, &c))
            s_rw_on = 1;
    }
    ResumeThread(s_rw_thread);
    return 0;
}

static void rw_start(uint32_t ptd)
{
    HANDLE h;
    if (!objlog_ok(ptd) || ((ptd + 0x14) & 3u))
        return;
    s_rw_addr = (uintptr_t)HP(ptd + 0x14);
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &s_rw_thread,
                    THREAD_ALL_ACCESS, FALSE, 0);
    AddVectoredExceptionHandler(1, rw_veh);
    h = CreateThread(NULL, 0, rw_arm, NULL, 0, NULL);
    if (h) {
        WaitForSingleObject(h, 5000);
        CloseHandle(h);
    }
    fprintf(s_objlog, "# rand callers (BW_STEPLOG_OBJ_RAND): %s\n", s_rw_on ? "on" : "could not arm the watch");
}

static void objlog_step(void)
{
    static int init, rw_want;
    uint32_t ptd, n, arr, i, q, slot, sp;
    if (!init) {
        const char *p = getenv("BW_STEPLOG_OBJ");
        init = 1;
        if (p && *p) {
            s_objlog = fopen(p, "a");
            if (s_objlog) {
                uint32_t fs = g_fs_base, th = objlog_ok(fs) ? HU32(fs + 0x28) : 0;
                fprintf(s_objlog, "# boot %lu: wclk mclk otime rand intro msg(slot flags msg time) spotter"
                        " obj(en|met<<1|fail<<2 each) boats(n: x,y,z,hp ...)\n",
                        (unsigned long)GetCurrentProcessId());
                fprintf(s_objlog, "# rand: fs %08X thread %08X +0x28 %08X tls %08X index %u; slot 0x287984 holds"
                        " %08X; the per-thread data %08X\n", fs, th, objlog_ok(th) ? HU32(th + 0x28) : 0u,
                        objlog_ok(fs) ? HU32(fs + 4) : 0u, HU32(0x2784B8u), HU32(0x287984u), objlog_ptd());
                {
                    const char *r = getenv("BW_STEPLOG_OBJ_RAND");
                    rw_want = r && r[0] == '1';
                }
            }
        }
    }
    if (!s_objlog)
        return;
    ptd = objlog_ptd();
    if (rw_want && !s_rw_thread && objlog_ok(ptd))
        rw_start(ptd);
    fprintf(s_objlog, "%u %u %u %08X %u,%u", HU32(WEAPONS_MS_VA), HU32(0x1D3FB8u), HU32(0x1D3A58u),
            objlog_ok(ptd) ? HU32(ptd + 0x14) : 0u, HU32(0x1D5C18u), HU32(0x1D5C1Cu));
    q = HU32(0x1D4000u);
    slot = HU32(0x1D3FECu);
    if (objlog_ok(q) && slot < 4096u) {
        uint32_t e = q + slot * 24u;
        fprintf(s_objlog, " m%u:%02X%02X%02X%02X:%08X:%u", slot, HU8(e), HU8(e + 1), HU8(e + 2), HU8(e + 3),
                HU32(e + 4), HU32(e + 0xC));
    } else {
        fprintf(s_objlog, " m-");
    }
    sp = 0x1D57B8u;
    fprintf(s_objlog, " s%d", (int)HU32(sp + 0x17C));
    n = HU32(0x1D3FE0u);
    arr = HU32(0x1D3FFCu);
    fputs(" o", s_objlog);
    if (n && n <= 512u && objlog_ok(arr)) {
        for (i = 0; i < n; i++) {
            uint32_t o = arr + i * 0xECu;
            fputc("0123456789ABCDEF"[(HU8(o + 0x24) ? 1 : 0) | (HU8(o + 0x48) ? 2 : 0) | (HU8(o + 0x49) ? 4 : 0)],
                  s_objlog);
        }
    } else {
        fputc('-', s_objlog);
    }
    n = HU32(0x1CDFD0u);
    arr = HU32(0x1CDFD8u);
    if (n <= 64u && objlog_ok(arr)) {
        fprintf(s_objlog, " b%u", n);
        for (i = 0; i < n; i++) {
            uint32_t b = arr + i * 0x6A4u;
            fprintf(s_objlog, " %.2f,%.2f,%.2f,%u", HF32(b + 0x84), HF32(b + 0x88), HF32(b + 0x8C), HU32(b + 0x20));
        }
    } else {
        fputs(" b-", s_objlog);
    }
    if (s_rw_on) {
        fputs(" r:", s_objlog);
        for (i = 0; i < (uint32_t)s_rw_n; i++)
            fprintf(s_objlog, "%s%X*%u", i ? "," : "", s_rw_seq[i], s_rw_cnt[i]);
        if (s_rw_lost)
            fprintf(s_objlog, ",lost*%u", s_rw_lost);
        s_rw_n = 0;
        s_rw_lost = 0;
    }
    fputc('\n', s_objlog);
}

/* ---- BW_FXLOG (lane wake, every edition, read-only test aid) ------------- *
 * One line per running step: the boat wake and spray particle emitters, to
 * check whether the wake follows the boat's speed as in Original. Seen in
 * code (docs/remaster/wake.md): the effects manager 0x1A7848 keeps its
 * emitters in a list from +0x1C, next at +0x1D0; an emitter's spawn handler
 * is at +0x214 (0x22A30 Wake_SpawnParticle, 0x22B40 Spray_SpawnParticle),
 * its emission rate (particles/s) at +0x20, its spawn accumulator at +0x29C,
 * its live particle count at +0x1EC (PFX_Update compares it with the int16
 * maximum at +8), its owner boat at +0x2BC (Wake_SetupBoatWake) and the
 * boat's forward speed at +0x114 (Boat_UpdateWakeEmitter, per step). The
 * effects clock 0x1A5758 advances by each drawn frame's time. Nothing
 * guest-side is written; with BW_FXLOG unset each step costs one test. */
#define FX_MGR_HEAD_VA   (0x001A7848u + 0x1Cu)
#define FX_CLOCK_VA      0x001A5758u
#define FX_NEXT          0x1D0u
#define FX_SPAWN_FN      0x214u
/* guest RAM, or the toolkit's contiguous window at 0x80000000 (bw_state.c) */
#define FX_VA_OK(va)     (((va) >= 0x00010000u && (va) < 0x08000000u) \
                          || ((va) >= 0x80000000u && (va) < 0x83FFFF00u))
static FILE *s_fxlog;
static int s_fxlog_all;   /* BW_FXLOG_ALL=1 (gate lane V, call A7): every emitter, an "A" line per step */

/* BW_FXLOG_ALL=1 (with BW_FXLOG): one "A" line per running step after the
 * usual one, with every emitter that has live particles, for the frame-rate
 * parity check (tools/bw_fpsparity.py). Read-only, like the rest of BW_FXLOG.
 * Seen in code:
 *   - PFX_InitEmitter 0x1B6A0 keeps the emitter's descriptor at +0x2AC; a
 *     descriptor's 32-byte name sits just before it (Effects.pak's records
 *     are name[32] + 0x134 bytes, and the built-ins' names are 0x20 before
 *     their records, 0x1A7A10/0x1A7A30: inferred for the pak's, checked by
 *     the names it prints);
 *   - a particle (list from +0x1E0, next at +0) has its position at +4..+0xC,
 *     its age at +0x10 and its life at +0x14 (PFX_SpawnParticle 0x20360);
 *   - its optional fields sit at the offsets in the emitter's table at
 *     +0x230 (PFX_ComputeParticleLayout 0x1AFE0; field i is there when flag
 *     bit i is set): +0x238 (bit 2) the colour, 4 bytes, byte 0 the alpha
 *     (PFX_Update samples the colour track into it); +0x258 (bit 10) the
 *     trail history, +0xA points of 12 bytes, newest first, an unused
 *     point's x = FLT_MAX (PFX_ShiftTrailHistory 0x23F80); +0x280 (bit 20)
 *     the scale.
 * Per emitter: " | name spawn renderer flags live rate k scale_mean scale_max
 * age_mean age_max life_mean alpha_mean trail_n trail_pts trail_len_mean
 * trail_len_max jitter" (the trail fields 0 without a trail, the scales 1
 * without a scale field). alpha_mean is the particles' own colour's alpha,
 * or without that field the emitter's colour's (+0x10, byte 0: what every
 * particle of it is drawn with, inferred), -1 if neither is readable.
 * jitter: the re-rolls of the first particle's jitter points since the last
 * step, counted before every drawn frame (fxlog_all_frame), -1 for an
 * emitter without them. PFX_BehaviourSlot6 0x248F0 keeps the points in field
 * 15 (+0x26C, +0xA points of 12 bytes) and its regen timer in field 19
 * (+0x27C) and redraws the points with rand() when the timer is out (seen);
 * with a period under a frame that is every drawn frame, so the count per
 * step grows with the frame rate (the wake audit's T2). An emitter is
 * counted when its behaviour (+0x220, PFX_BindHandlers 0x1B4A0 from the
 * table at 0x187B10) is Slot6. Last on the entry: that behaviour's address. */
#define FXJ_N 128
static uint32_t s_fxj_e[FXJ_N], s_fxj_p[FXJ_N], s_fxj_h[FXJ_N], s_fxj_chg[FXJ_N];

static int fxj_slot(uint32_t e, int make)
{
    int i, free_i = -1;
    for (i = 0; i < FXJ_N; i++) {
        if (s_fxj_e[i] == e)
            return i;
        if (!s_fxj_e[i] && free_i < 0)
            free_i = i;
    }
    if (!make)
        return -1;
    if (free_i < 0)
        free_i = (int)(e >> 4) % FXJ_N;            /* full: reuse one */
    s_fxj_e[free_i] = e;
    s_fxj_p[free_i] = s_fxj_h[free_i] = s_fxj_chg[free_i] = 0;
    return free_i;
}

/* au-fx: Lightning_UpdateBolt 0x253B0 keeps its bolt points and regen timer
 * in the same fields (+0x26C, +0x27C; seen at 0x2561E-0x25731): the Stinger's
 * hit bolt (BUILTIN_Lightning) uses it, so it's counted too. */
static int fxj_has(uint32_t e, uint32_t fl, int *tn)
{
    uint32_t b = HU32(e + 0x220u);
    *tn = (int)(int16_t)HU16(e + 0xAu);
    (void)fl;
    return (b == 0x000248F0u || b == 0x000253B0u) && HU32(e + 0x26Cu) < 0x1000u && *tn >= 1 && *tn <= 256;
}

/* Before each drawn frame (every edition, BW_FXLOG_ALL only): has the first
 * particle's jitter changed since the frame before? Read-only. */
static void fxlog_all_frame(void)
{
    uint32_t e, n = 0;
    for (e = HU32(FX_MGR_HEAD_VA); FX_VA_OK(e) && n < 4096u; e = HU32(e + FX_NEXT), n++) {
        uint32_t fl = HU32(e), pp, h = 2166136261u, off;
        int tn, i, s;
        if (!HU32(e + 0x1ECu) || !fxj_has(e, fl, &tn))
            continue;
        pp = HU32(e + 0x1E0u);
        if (!FX_VA_OK(pp))
            continue;
        off = HU32(e + 0x26Cu);
        for (i = 0; i < tn * 3 && i < 192; i++)
            h = (h ^ HU32(pp + off + 4u * (uint32_t)i)) * 16777619u;
        s = fxj_slot(e, 1);
        if (s_fxj_p[s] == pp && s_fxj_h[s] != h)
            s_fxj_chg[s]++;
        s_fxj_p[s] = pp;
        s_fxj_h[s] = h;
    }
}

static void fxlog_all_step(void)
{
    uint32_t e, n = 0;
    fprintf(s_fxlog, "A %u %.4f", HU32(WEAPONS_MS_VA), HF32(FX_CLOCK_VA));
    for (e = HU32(FX_MGR_HEAD_VA); FX_VA_OK(e) && n < 4096u; e = HU32(e + FX_NEXT), n++) {
        uint32_t live = HU32(e + 0x1ECu), fl, desc, pp, np = 0;
        uint32_t o_col, o_trail, o_scale;
        int tn, grow, col, trail, i;
        char name[33];
        double s_sum = 0.0, s_max = 0.0, a_sum = 0.0, a_max = 0.0, l_sum = 0.0, c_sum = 0.0;
        double tp_sum = 0.0, tl_sum = 0.0, tl_max = 0.0;
        if (!live)
            continue;
        fl = HU32(e);
        desc = HU32(e + 0x2ACu);
        name[0] = 0;
        if (FX_VA_OK(desc) && FX_VA_OK(desc - 0x20u)) {
            for (i = 0; i < 32; i++) {
                char c = (char)HU8(desc - 0x20u + (uint32_t)i);
                if (!c)
                    break;
                name[i] = (c > ' ' && c < 127) ? c : '_';
            }
            name[i] = 0;
        }
        if (!name[0])
            _snprintf(name, sizeof name, "d%08X", desc);
        name[32] = 0;
        o_col = HU32(e + 0x238u);
        o_trail = HU32(e + 0x258u);
        o_scale = HU32(e + 0x280u);
        grow = (fl & 0x00100000u) && o_scale < 0x1000u;
        col = (fl & 0x00000004u) && o_col < 0x1000u;
        tn = (int)(int16_t)HU16(e + 0xAu);
        trail = (fl & 0x00000400u) && o_trail < 0x1000u && tn > 1 && tn <= 256;
        for (pp = HU32(e + 0x1E0u); FX_VA_OK(pp) && np < 4096u; pp = HU32(pp), np++) {
            double ag = HF32(pp + 0x10u), lf = HF32(pp + 0x14u);
            a_sum += ag;
            if (ag > a_max)
                a_max = ag;
            l_sum += lf;
            if (grow) {
                double sc = HF32(pp + o_scale);
                s_sum += sc;
                if (sc > s_max)
                    s_max = sc;
            }
            if (col)
                c_sum += HU8(pp + o_col);
            if (trail) {
                uint32_t h = pp + o_trail;
                int pts = 0;
                double len = 0.0, px = 0.0, py = 0.0, pz = 0.0;
                for (i = 0; i < tn; i++, h += 12u) {
                    double x, y, z;
                    if (HU32(h) == 0x7F7FFFFFu)
                        break;
                    x = HF32(h);
                    y = HF32(h + 4u);
                    z = HF32(h + 8u);
                    if (pts)
                        len += sqrt((x - px) * (x - px) + (y - py) * (y - py) + (z - pz) * (z - pz));
                    px = x;
                    py = y;
                    pz = z;
                    pts++;
                }
                tp_sum += pts;
                tl_sum += len;
                if (len > tl_max)
                    tl_max = len;
            }
        }
        if (!np)
            continue;
        {
            int jt, js = -1, jit = -1;
            if (fxj_has(e, fl, &jt) && (js = fxj_slot(e, 0)) >= 0) {
                jit = (int)s_fxj_chg[js];
                s_fxj_chg[js] = 0;
            }
            fprintf(s_fxlog, " | %s %05X %05X %08X %u %.3f %.5f %.4f %.4f %.4f %.4f %.4f %.1f %d %.2f %.3f %.3f %d %05X",
                    name, HU32(e + FX_SPAWN_FN), HU32(e + 0x228u), fl, live, HF32(e + 0x20u), HF32(e + 0xBCu),
                    grow ? s_sum / np : 1.0, grow ? s_max : 1.0, a_sum / np, a_max, l_sum / np,
                    col ? c_sum / np : (double)HU8(e + 0x10u), trail ? tn : 0, trail ? tp_sum / np : 0.0,
                    trail ? tl_sum / np : 0.0, trail ? tl_max : 0.0, jit, HU32(e + 0x220u));
            /* au-fx (T3): a Lightning emitter adds its bolt's phase (+0x2A8)
             * and its address, so the walk per step can be measured */
            if (HU32(e + 0x220u) == 0x000253B0u)
                fprintf(s_fxlog, " %.4f %08X", HF32(e + 0x2A8u), e);
        }
    }
    fputc('\n', s_fxlog);
    /* forget emitters that are gone (freed or reused): the next frame starts them again */
    {
        int i;
        for (i = 0; i < FXJ_N; i++) {
            uint32_t m = 0;
            if (!s_fxj_e[i])
                continue;
            for (e = HU32(FX_MGR_HEAD_VA), n = 0; FX_VA_OK(e) && n < 4096u; e = HU32(e + FX_NEXT), n++)
                if (e == s_fxj_e[i]) {
                    m = 1;
                    break;
                }
            if (!m)
                s_fxj_e[i] = 0;
        }
    }
}

static void fxlog_step(void)
{
    static int init;
    uint32_t e, n = 0, boat = 0, np, gk_e = 0;
    if (!init) {
        const char *p = getenv("BW_FXLOG");
        init = 1;
        if (p && *p) {
            const char *all = getenv("BW_FXLOG_ALL");
            s_fxlog = fopen(p, "a");
            s_fxlog_all = all && all[0] == '1';
            if (s_fxlog)
                fprintf(s_fxlog, "# boot %lu: wclk fxclock boat throttle revs | per emitter: W|S owner"
                                 " rate live max acc speed hullC%s\n", (unsigned long)GetCurrentProcessId(),
                        s_fxlog_all ? "; A wclk fxclock | name spawn renderer flags live rate k scale_mean"
                                      " scale_max age_mean age_max life_mean alpha_mean trail_n trail_pts"
                                      " trail_len_mean trail_len_max jitter behaviour" : "");
        }
    }
    if (!s_fxlog)
        return;
    np = HU32(PLAYERS_VA + 0xC0);
    if (np >= 1 && np <= 4)
        boat = HU32(PLAYERS_VA + 8);
    fprintf(s_fxlog, "%u %.4f %08X", HU32(WEAPONS_MS_VA), HF32(FX_CLOCK_VA), boat);
    if (boat >= 0x00010000u && boat < 0x08000000u)
        fprintf(s_fxlog, " %.3f %.0f", HF32(boat + 0x398), HF32(boat + 0x3B8));
    else
        fprintf(s_fxlog, " - -");
    fprintf(s_fxlog, " |");
    for (e = HU32(FX_MGR_HEAD_VA); FX_VA_OK(e) && n < 4096u; e = HU32(e + FX_NEXT), n++) {
        uint32_t fn = HU32(e + FX_SPAWN_FN), hull;
        if (fn != 0x00022A30u && fn != 0x00022B40u)
            continue;
        hull = HU32(e + 0x10Cu);
        fprintf(s_fxlog, " %c %08X %.3f %u %d %.4f %.3f %.4f", fn == 0x00022A30u ? 'W' : 'S',
                HU32(e + 0x2BCu), HF32(e + 0x20u), HU32(e + 0x1ECu), (int)(int16_t)HU16(e + 8u),
                HF32(e + 0x29Cu), HF32(e + 0x114u),
                FX_VA_OK(hull) ? HF32(hull + 0xCu) : -1.0f);
        if (HU32(e + 0x2BCu) == boat && !gk_e)
            gk_e = e;
    }
    /* The boat's emitter's per-frame size factor (+0xBC, used when flag
     * 0x100000 is set: PFX_Update multiplies each live particle's scale, at
     * the layout offset +0x280, by it once per drawn frame) and its live
     * particles' scales (list from +0x1E0, next at +0, age +0x10). */
    if (gk_e) {
        uint32_t pp = HU32(gk_e + 0x1E0u), np2 = 0, off = HU32(gk_e + 0x280u);
        double sum = 0.0, mx = 0.0, agesum = 0.0;
        int grow = (HU32(gk_e) & 0x00100000u) != 0;
        for (; grow && FX_VA_OK(pp) && np2 < 4096u; pp = HU32(pp), np2++) {
            double sc = HF32(pp + off);
            sum += sc;
            agesum += HF32(pp + 0x10u);
            if (sc > mx)
                mx = sc;
        }
        fprintf(s_fxlog, " k=%.5f grow=%d scale_mean=%.3f scale_max=%.3f age_mean=%.3f", HF32(gk_e + 0xBCu), grow,
                np2 ? sum / np2 : 0.0, mx, np2 ? agesum / np2 : 0.0);
    }
    /* Every other emitter with the per-frame scale factor and live
     * particles: " g <spawn handler> <renderer +0x228> <k> <live> <max scale>
     * <max age>", so the effects can be told apart by their handlers. */
    n = 0;
    for (e = HU32(FX_MGR_HEAD_VA); FX_VA_OK(e) && n < 4096u; e = HU32(e + FX_NEXT), n++) {
        uint32_t pp, np2 = 0, off;
        double mx = 0.0, amx = 0.0;
        if (e == gk_e || !(HU32(e) & 0x00100000u) || !HU32(e + 0x1ECu))
            continue;
        off = HU32(e + 0x280u);
        for (pp = HU32(e + 0x1E0u); FX_VA_OK(pp) && np2 < 4096u; pp = HU32(pp), np2++) {
            double sc = HF32(pp + off), ag = HF32(pp + 0x10u);
            if (sc > mx)
                mx = sc;
            if (ag > amx)
                amx = ag;
        }
        fprintf(s_fxlog, " g %05X %05X %.4f %u %.3f %.3f", HU32(e + FX_SPAWN_FN), HU32(e + 0x228u),
                HF32(e + 0xBCu), HU32(e + 0x1ECu), mx, amx);
    }
    fprintf(s_fxlog, " n=%u\n", n);
    if (s_fxlog_all)
        fxlog_all_step();
}

/* ---- BW_LATLOG (lat-m, every edition, read-only) ------------------------- *
 * Each running step's end (its weapons clock, throttle, rudder and camera
 * mode), each drawn frame (the newest step it shows and with what weight),
 * and the pads as the game's own read left them. Nothing guest-side is
 * written; with BW_LATLOG unset each call site is one test of a flag. */
#define DEVICES_VA       0x001C7468u   /* Devices_GetPad's this: pads at +0x3F8 + 4*port */
#define DEVICES_PADS     0x3F8u
#define PAD_HANDLE       0x04u         /* the XInput handle (0: not open, not polled) */
#define PAD_PACKET       0x21u         /* XInputGetState's packet number */
#define PAD_BLOCK        0x37u         /* Pad_PollState's copy of the gamepad block (18 bytes) */
static int64_t s_lat_upd0, s_lat_upd1;   /* this pass's Game_Update begin and end */
static uint32_t s_lat_wclk;              /* the newest running step's weapons clock */
static uint32_t s_lat_steps;             /* running steps since the last drawn frame */
static long s_lat_frame;                 /* drawn frames so far */
static float s_lat_camw = 1.0f;          /* view 0's camera weight in this frame */
static float s_lat_nodew = 1.0f;         /* player 1's boat's weight (lat-p: u, times the fade) */
static uint8_t s_lat_pad[4][18];
static uint8_t s_lat_pad_seen[4];

static int lat_guest_ok(uint32_t va)
{
    return va >= 0x00010000u && va < 0x08000000u;
}

static void lat_step(void)
{
    uint32_t n = HU32(PLAYERS_VA + 0xC0);
    uint32_t boat = n >= 1 && n <= 4 ? HU32(PLAYERS_VA + 8) : 0;
    double thr = 0.0, rud = 0.0;
    if (lat_guest_ok(boat)) {
        thr = HF32(boat + 0x398);
        rud = HF32(boat + 0x39C);
    } else
        boat = 0;
    s_lat_wclk = HU32(WEAPONS_MS_VA);
    s_lat_steps++;
    bw_lat_ev4(BW_LAT_STEP, bw_lat_now(), s_lat_wclk, HU32(CAM_VA + CAM_MODE),
               HU32(GAME_VA + GAME_STATE), HU32(CAM_NVIEWS_VA), boat, thr, rud,
               HF32(CAM_VA + CAM_EYE), HF32(CAM_VA + CAM_EYE + 8));
}

/* At a drawn frame's start: Devices_Update (message 0xA) has just polled the
 * pads, so each open pad's block is what the game's read returned. Logged
 * when it changes. */
static void lat_pads(int64_t t)
{
    unsigned p;
    for (p = 0; p < 4; p++) {
        uint32_t pad = HU32(DEVICES_VA + DEVICES_PADS + 4u * p);
        uint8_t blk[18];
        uint64_t ana, sticks;
        if (!lat_guest_ok(pad) || !HU32(pad + PAD_HANDLE))
            continue;
        memcpy(blk, HP(pad + PAD_BLOCK), sizeof blk);
        if (s_lat_pad_seen[p] && !memcmp(blk, s_lat_pad[p], sizeof blk))
            continue;
        memcpy(s_lat_pad[p], blk, sizeof blk);
        s_lat_pad_seen[p] = 1;
        memcpy(&ana, blk + 2, 8);
        memcpy(&sticks, blk + 10, 8);
        bw_lat_ev(BW_LAT_PAD, t, p, HU32(pad + PAD_PACKET), blk[0] | (blk[1] << 8),
                  (int64_t)ana, (int64_t)sticks, 0, 0.0, 0.0);
    }
}

static void lat_frame(int64_t t, int on, int interp, float node_w)
{
    uint32_t steps = s_lat_steps;
    s_lat_steps = 0;
    bw_lat_ev(BW_LAT_FRAME, t, ++s_lat_frame, s_lat_wclk, g_bw_lat_flips, s_lat_upd1, s_lat_upd0,
              (int64_t)(steps & 0xFFFFu) | ((int64_t)(interp != 0) << 16)
              | ((int64_t)(HU32(GAME_VA + GAME_STATE) & 0xFu) << 20) | ((int64_t)(on != 0) << 24),
              node_w, s_lat_camw);
}

static void lat_frame_end(void)
{
    bw_lat_ev(BW_LAT_FRAMEEND, bw_lat_now(), s_lat_frame, 0, g_bw_lat_flips, 0, 0, 0, 0.0, 0.0);
}

/* ---- snapshots, once per step ------------------------------------------ */

static void cam_read(unsigned n, CamKey *k)
{
    uint32_t v = CAM_VA + n * CAM_STRIDE;
    int i;
    for (i = 0; i < 3; i++) {
        k->eye[i] = HF32(v + CAM_EYE + 4 * i);
        k->at[i]  = HF32(v + CAM_AT + 4 * i);
        k->up[i]  = HF32(v + CAM_UP + 4 * i);
    }
    k->mode = HU32(v + CAM_MODE);
}

static void snapshot_step(void)
{
    uint32_t base = HU32(NODEPOOL_PTR_VA), i;
    unsigned n;
    if (s_plog)
        pl_step(base, 1);                   /* the frames since the last step, judged */
    if (s_pred > 0.0f && s_pred_fade > 0.0f && s_pred_order == 2) {
        /* lat-p: the fade's previous prediction of order 2 needs the step before that too */
        memcpy(s_prev3, s_prev2, sizeof s_prev2);
        memcpy(s_id_prev3, s_id_prev2, sizeof s_id_prev2);
        memcpy(s_cam_prev3, s_cam_prev2, sizeof s_cam_prev2);
        s_nviews_prev3 = s_nviews_prev2;
        s_have_prev3 = s_snapshots >= 3;
    }
    if (s_pred > 0.0f && (s_pred_fade > 0.0f || s_pred_order == 2)) {
        /* lat-p: the step before too, for the cross-fade's previous prediction and
         * the parabola of order 2 */
        memcpy(s_prev2, s_prev, sizeof s_prev);
        memcpy(s_id_prev2, s_id_prev, sizeof s_id_prev);
        memcpy(s_cam_prev2, s_cam_prev, sizeof s_cam_prev);
        s_nviews_prev2 = s_nviews_prev;
        s_have_prev2 = s_snapshots >= 2;
    }
    memcpy(s_prev, s_cur, sizeof s_cur);
    memcpy(s_id_prev, s_id_cur, sizeof s_id_cur);
    memcpy(s_cam_prev, s_cam_cur, sizeof s_cam_cur);
    s_nviews_prev = s_nviews;
    if (base) {
        for (i = 0; i < NODE_N; i++) {
            uint32_t nd = base + i * NODE_SIZE;
            if (!HU16(nd + NODE_FLAGS)) {
                s_id_cur[i] = 0;
                continue;
            }
            memcpy(&s_cur[i], HP(nd), sizeof(Mat));
            s_id_cur[i] = (HU32(nd + NODE_HASH) ^ HU32(nd + NODE_DRAWABLE)) | 1u;
        }
    }
    s_nviews = HU32(CAM_NVIEWS_VA);
    if (s_nviews > MAX_VIEWS)
        s_nviews = 0;                       /* not set up */
    for (n = 0; n < s_nviews; n++)
        cam_read(n, &s_cam_cur[n]);
    /* The local players' boats (lat-p, BW_HIFPS_PREDICT_SCOPE=boat): player
     * n's boat object at Players+8+0x30*n (bw_state.c), its render item at
     * +4, whose matrix is the scene node at item+0x30 (an override) or
     * item+8 (bw_state.c obj_world_pos, RenderItem_ComputeWorldMatrix). */
    s_nboats = 0;
    if (s_pred > 0.0f || s_plog) {
        uint32_t np = HU32(PLAYERS_VA + 0xC0);
        for (n = 0; base && np <= MAX_VIEWS && n < np; n++) {
            uint32_t obj = HU32(PLAYERS_VA + 8u + 0x30u * n), item, m;
            s_boat_node[n] = 0;
            if (obj < 0x00010000u || obj >= 0x08000000u)
                continue;
            item = HU32(obj + 4);
            if (!((item >= 0x00010000u && item < 0x08000000u) || (item >= 0x80000000u && item < 0x83FFFF00u)))
                continue;
            m = HU32(item + 0x30);
            if (!m)
                m = HU32(item + 8);
            if (m >= base && m < base + NODE_N * NODE_SIZE && (m - base) % NODE_SIZE == 0)
                s_boat_node[n] = (m - base) / NODE_SIZE + 1u;
        }
        s_nboats = np <= MAX_VIEWS ? np : 0;
    }
    if (base)
        s_snapshots++;
    s_steps_since_draw++;
    s_st_steps++;
    if (s_pred_guard)
        guard_step();
    if (s_plog)
        pl_step(base, 0);                   /* this step's poses, logged */
}

static int is_boat(uint32_t i)
{
    unsigned n;
    for (n = 0; n < s_nboats; n++)
        if (s_boat_node[n] == i + 1u)
            return 1;
    return 0;
}

/* ---- interpolation ----------------------------------------------------- */

static float dot3(const float *a, const float *b) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }
static float len3(const float *a) { return sqrtf(dot3(a, a)); }
static void norm3(float *a) { float l = len3(a); if (l > 1e-12f) { a[0] /= l; a[1] /= l; a[2] /= l; } }

/* Rows 0-2: basis (lerped, Gram-Schmidt, lengths lerped); row 3: translation.
 * 0 when the step moved or turned too far to blend (a snap). */
static int lerp_mat(const Mat *a, const Mat *b, float t, Mat *o)
{
    float ra[3][3], rb[3][3], r[3][3], la[3], lb[3];
    int i, j;
    float dx = b->m[12] - a->m[12], dy = b->m[13] - a->m[13], dz = b->m[14] - a->m[14];
    if (!(dx*dx + dy*dy + dz*dz <= SNAP_MOVE_M * SNAP_MOVE_M))   /* also NaN */
        return 0;
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            ra[i][j] = a->m[i*4 + j];
            rb[i][j] = b->m[i*4 + j];
        }
        la[i] = len3(ra[i]);
        lb[i] = len3(rb[i]);
        if (!(la[i] > 1e-6f) || !(lb[i] > 1e-6f))
            return 0;
    }
    if (dot3(ra[0], rb[0]) < SNAP_DOT * la[0] * lb[0] ||
        dot3(ra[2], rb[2]) < SNAP_DOT * la[2] * lb[2])
        return 0;
    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++)
            r[i][j] = ra[i][j] / la[i] + (rb[i][j] / lb[i] - ra[i][j] / la[i]) * t;
    norm3(r[0]);
    {
        float d = dot3(r[1], r[0]);
        for (j = 0; j < 3; j++) r[1][j] -= d * r[0][j];
    }
    norm3(r[1]);
    {
        float d0 = dot3(r[2], r[0]), d1 = dot3(r[2], r[1]);
        for (j = 0; j < 3; j++) r[2][j] -= d0 * r[0][j] + d1 * r[1][j];
    }
    norm3(r[2]);
    *o = *b;
    for (i = 0; i < 3; i++) {
        float l = la[i] + (lb[i] - la[i]) * t;
        for (j = 0; j < 3; j++)
            o->m[i*4 + j] = r[i][j] * l;
    }
    o->m[12] = a->m[12] + dx * t;
    o->m[13] = a->m[13] + dy * t;
    o->m[14] = a->m[14] + dz * t;
    return 1;
}

/* D3DXMatrixLookAtLH(eye, at, up), the call Camera_ApplyView makes. */
static void look_at_lh(const float *eye, const float *at, const float *up, float *m)
{
    float z[3] = { at[0] - eye[0], at[1] - eye[1], at[2] - eye[2] }, x[3], y[3];
    norm3(z);
    x[0] = up[1]*z[2] - up[2]*z[1]; x[1] = up[2]*z[0] - up[0]*z[2]; x[2] = up[0]*z[1] - up[1]*z[0];
    norm3(x);
    y[0] = z[1]*x[2] - z[2]*x[1]; y[1] = z[2]*x[0] - z[0]*x[2]; y[2] = z[0]*x[1] - z[1]*x[0];
    m[0] = x[0]; m[1] = y[0]; m[2]  = z[0]; m[3]  = 0;
    m[4] = x[1]; m[5] = y[1]; m[6]  = z[1]; m[7]  = 0;
    m[8] = x[2]; m[9] = y[2]; m[10] = z[2]; m[11] = 0;
    m[12] = -dot3(x, eye); m[13] = -dot3(y, eye); m[14] = -dot3(z, eye); m[15] = 1;
}

/* Fraction of a step since the step clock: (now - clock) / (freq / 30). The
 * last step simulated up to `clock`; drawing it at `now` shows the state
 * one step behind, blended (120fps.md "Display delay"). */
static LARGE_INTEGER s_host_at_update;  /* host QPC when Game_Update read its clock */
static double s_alpha_gnow_ms, s_alpha_raw;   /* the last draw's game clock and t, unclamped */

static float step_alpha(void)
{
    uint64_t freq = HU64(SYS_QPC_FREQ_VA), now = HU64(GAME_VA + GAME_NOW_QPC);
    uint64_t clk = HU64(GAME_VA + GAME_STEP_QPC);
    double tick = freq ? (double)freq / 30.0 : 0.0, a, gnow = (double)now;
    /* Sampled now, at the draw, rather than when Game_Update began: a frame
     * that ran a step draws a few ms later than one that didn't, and showing
     * both at their own Game_Update time made the steps' frames lag (a 30 Hz
     * judder). The game's clock is advanced by the host time since then. */
    if (s_host_at_update.QuadPart && freq) {
        static double host_freq;
        LARGE_INTEGER t;
        if (!host_freq) {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            host_freq = (double)f.QuadPart;
        }
        QueryPerformanceCounter(&t);
        gnow += (double)(t.QuadPart - s_host_at_update.QuadPart) * (double)freq / host_freq;
    }
    s_alpha_gnow_ms = freq ? gnow * 1000.0 / (double)freq : 0.0;   /* BW_PREDLOG */
    s_alpha_raw = tick > 0.0 ? (gnow - (double)clk) / tick : 0.0;
    if (tick <= 0.0 || gnow < (double)clk)
        return 1.0f;
    a = (gnow - (double)clk) / tick;
    return a < 0.0 ? 0.0f : a > 1.0 ? 1.0f : (float)a;
}

/* ---- prediction: drawn frames that show "now" (lat-p) --------------------
 *
 * The interpolation shows each drawn frame one step behind: the last two
 * steps blended at t = (game clock - step clock) / step, i.e. the state at
 * the game's clock minus one step (0-33 ms behind; docs/remaster/120fps.md
 * "Display delay"). Prediction draws the same blend at u = t + p: p = 1
 * carries the last step's motion on for a whole step, so the frame shows the
 * state at the game's clock ("now"); a fraction goes part of the way. It is
 * render only, exactly like the interpolation: what it writes (node
 * matrices, render and simulation view matrices, the water's blend factor,
 * the HUD replay that follows the blended camera) goes back after the frame
 * through the same restore, so no game step ever sees a predicted value.
 * The snap rules are the interpolation's (a node that moved over 30 m or
 * turned over ~45 degrees in the last step, a freed or reused node, a camera
 * cut: the step is shown). Remastered and later only, like everything in
 * this file; off unless BW_HIFPS_PREDICT says otherwise.
 *   BW_HIFPS_PREDICT=<p>          0..1 of a step; default 0 (interpolate)
 *   BW_HIFPS_PREDICT_SCOPE=cam    the cameras only
 *                         =boat   the cameras and the local players' boats
 *                         =all    the cameras and every blended node (default)
 *   BW_HIFPS_PREDICT_WATER=0      with scope all, the water stays interpolated
 *                                 (BW_HIFPS_WATER=2's exact blend always does:
 *                                 it moves the water's clock, which must not
 *                                 run past the newest step)
 *   BW_HIFPS_PREDICT_FADE=<ms>    each correction cross-faded over this long
 *                                 (0..33; default 0)
 *   BW_HIFPS_PREDICT_ORDER=2      past the newest step, the parabola through
 *                                 three steps instead of the straight line
 *                                 through two (default 1)
 *   BW_HIFPS_PREDICT_AHEAD=<ms>   predict that much past "now" too, towards the
 *                                 moment the frame reaches the screen (0..33;
 *                                 default 0; only with PREDICT above 0)
 *   BW_HIFPS_EXTRAP=1             the old switch: PREDICT=1, SCOPE=cam
 *
 * BW_PREDLOG=<file> (test aid; the high frame rate only; reads what this
 * file keeps already and writes nothing the game reads). Lines:
 *   S <snap> <wclk> <nviews> {<mode> <eye xyz> <at xyz> <up xyz> <fov>}x nviews
 *     <nboats> {<node+1> <m0..m15>}x nboats
 *       after each step's snapshot: the poses frames blend towards;
 *   F <frame> <snap> <steps> <t> <ucam> <uboat> <unode> <camflags> <game ms>
 *     <host ms> <raw t> <eye xyz drawn> <boat xyz drawn> <lerped> <snapped>
 *       each drawn frame that blends: its factors (u = t + p), the views it
 *       blended (bit n; a clear bit is a cut: the step was shown), what it drew
 *       for view 0's eye and player 1's boat ("-" when nothing), so the tool's
 *       replay can be checked against the frame;
 *   H/h/He every 300 steps, cumulative: the error of every blended or resting
 *     scene node that moves, judged when the next step arrives, for each p of
 *     BW_PREDLOG_P (default 0,0.25,0.5,0.75,1, as if scope all), against the
 *     true pose at the time the frame shows ("pred": 0 for an interpolation)
 *     and at the game clock when it was drawn ("now"); log-binned histograms
 *     of the position error (m) and the angle error (degrees);
 *   C <snap> <judged> <microseconds> what the judging cost the game thread.
 * The true pose between two steps is their blend (the game has nothing
 * between steps). Frames whose next step is a snap are not judged, and are
 * counted (He). tools/bw_predict.py replays the cameras and boats from S and
 * F, and reads H. */
#define PL_MAXF   64                      /* frames per step judged (bit per frame) */
#define PL_NP_MAX 8
#define PL_NB     82                      /* 0: < 1e-5 (and 0); 1..80: 10 a decade to 1e3; 81: over */
typedef struct { float t, un; } PlFrame;
typedef struct { uint32_t n, bin[PL_NB]; double sum, max; } PlHist;
static PlFrame  s_pl_f[PL_MAXF];
static unsigned s_pl_nf;
static uint64_t s_pl_blend[NODE_N], s_pl_rest[NODE_N];
static unsigned long s_pl_frames, s_pl_flerp, s_pl_fsnap, s_pl_over;
static unsigned long s_pl_judged, s_pl_snapnext, s_pl_gone, s_pl_cost_n;
static unsigned long s_pl_camheld;        /* camera predictions held at the step (cam_pred) */
static unsigned long s_pl_beyond;         /* node-frames past the next step (not judged) */
static unsigned s_pl_camflags;
static float    s_pl_eye[3];
static float    s_pl_p[PL_NP_MAX];
static unsigned s_pl_np;
static PlHist   s_pl_h[PL_NP_MAX][4];     /* pred pos, pred angle, now pos, now angle */
static double   s_pl_cost_us;

/* The cross-fade (BW_HIFPS_PREDICT_FADE=<ms>): a prediction is corrected when
 * the next step arrives, and without a fade the picture jumps by the
 * correction at that instant (docs/latency/p.md, "step jumps"). With one, for
 * the first <ms> after a step the frame blends from the previous step's
 * prediction carried on (the step before, the step before that, at u + 1: the
 * same moment) to the new one, so the picture is continuous and the
 * correction is spread over the fade. The weight of the new prediction: */
static float pred_fade_w(float t)
{
    float w;
    if (!(s_pred_fade > 0.0f) || !s_have_prev2)
        return 1.0f;
    w = t * (1000.0f / 30.0f) / s_pred_fade;
    return w < 1.0f ? (w > 0.0f ? w : 0.0f) : 1.0f;
}

/* What a drawn frame shows for scene node i at u with fade weight w (1 = no
 * fade): 1 = *o is the pose, 0 = the step as it stands (at rest), -1 = a snap
 * (the step is shown). The interpolation is u = t, w = 1. */
/* BW_HIFPS_PREDICT_ORDER=2: past the newest step, the parabola through three
 * steps (z, a, b at -2, -1, 0) instead of the straight line through two, so a
 * motion that is speeding up or easing off is carried on as it goes (offline
 * on the s1 logs, a full step ahead: the boat's error p50 1.20 -> 0.28 cm, p95
 * 6.82 -> 3.66 cm; docs/latency/p.md). */
static void quad3f(const float *z, const float *a, const float *b, float tau, float *o)
{
    int i;
    for (i = 0; i < 3; i++)
        o[i] = b[i] + (3.0f * b[i] - 4.0f * a[i] + z[i]) * 0.5f * tau
             + (b[i] - 2.0f * a[i] + z[i]) * 0.5f * tau * tau;
}

/* The order-2 guard: the parabola only while the motion changes by at most
 * half of itself from one step to the next (plus 1 cm). Abrupt changes keep
 * the straight line: there a parabola overshoots further (seen offline: the
 * camera's eye up to 12.7 m against the line's 8.0, p99 2.6 m against 1.2). */
static int smooth3(const float *z, const float *a, const float *b)
{
    float v[3], acc[3];
    int i;
    for (i = 0; i < 3; i++) {
        v[i] = b[i] - a[i];
        acc[i] = b[i] - 2.0f * a[i] + z[i];
    }
    return len3(acc) <= 0.5f * len3(v) + 0.01f;
}

/* A node's pose at u: lerp_mat(a, b, u); with order 2, past the newest step
 * and with the step before (z, no snap from it), the rows (normalised) and
 * the translation on the parabola, then Gram-Schmidt as lerp_mat, the row
 * lengths carried on in a straight line. 0 = a snap between a and b. Mirrored
 * by pred_mat() in tools/bw_predict.py. */
static int pred_mat(const Mat *z, const Mat *a, const Mat *b, float u, Mat *o)
{
    Mat tmp;
    float tau, r[3][3], rz[3], ra[3], rb[3];
    int i, j;
    if (!(s_pred_order == 2 && z && u > 1.0f))
        return lerp_mat(a, b, u, o);
    if (!lerp_mat(a, b, 0.0f, &tmp))
        return 0;
    if (!lerp_mat(z, a, 0.0f, &tmp) || !smooth3(&z->m[12], &a->m[12], &b->m[12]))
        return lerp_mat(a, b, u, o);
    /* the rows too (normalised): a node that spins in place keeps the
     * straight line (seen: with only the translation guarded, the other
     * nodes' angle errors doubled with order 2 and the fade) */
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            rz[j] = z->m[i*4 + j];
            ra[j] = a->m[i*4 + j];
            rb[j] = b->m[i*4 + j];
        }
        norm3(rz);
        norm3(ra);
        norm3(rb);
        if (!smooth3(rz, ra, rb))
            return lerp_mat(a, b, u, o);
    }
    tau = u - 1.0f;
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            rz[j] = z->m[i*4 + j];
            ra[j] = a->m[i*4 + j];
            rb[j] = b->m[i*4 + j];
        }
        norm3(rz);
        norm3(ra);
        norm3(rb);
        quad3f(rz, ra, rb, tau, r[i]);
    }
    norm3(r[0]);
    {
        float d = dot3(r[1], r[0]);
        for (j = 0; j < 3; j++) r[1][j] -= d * r[0][j];
    }
    norm3(r[1]);
    {
        float d0 = dot3(r[2], r[0]), d1 = dot3(r[2], r[1]);
        for (j = 0; j < 3; j++) r[2][j] -= d0 * r[0][j] + d1 * r[1][j];
    }
    norm3(r[2]);
    *o = *b;
    for (i = 0; i < 3; i++) {
        float la = len3(&a->m[i*4]), lb = len3(&b->m[i*4]), l = lb + (lb - la) * tau;
        for (j = 0; j < 3; j++)
            o->m[i*4 + j] = r[i][j] * l;
    }
    quad3f(&z->m[12], &a->m[12], &b->m[12], tau, &o->m[12]);
    return 1;
}

static int pred_node(uint32_t i, float u, float w, Mat *o)
{
    int moving = memcmp(&s_prev[i], &s_cur[i], sizeof(Mat)) != 0;
    Mat on, oo;
    const Mat *z2 = s_pred_order == 2 && s_have_prev2 && s_id_prev2[i] == s_id_cur[i] ? &s_prev2[i] : NULL;
    const Mat *z3 = s_pred_order == 2 && s_have_prev3 && s_id_prev3[i] == s_id_cur[i] ? &s_prev3[i] : NULL;
    if (moving) {
        if (!pred_mat(z2, &s_prev[i], &s_cur[i], u, &on))
            return -1;
    } else {
        on = s_cur[i];
    }
    if (w < 1.0f && s_id_prev2[i] == s_id_cur[i]
        && memcmp(&s_prev2[i], &s_prev[i], sizeof(Mat))
        && pred_mat(z3, &s_prev2[i], &s_prev[i], u + 1.0f, &oo) && lerp_mat(&oo, &on, w, o))
        return 1;
    if (!moving)
        return 0;
    *o = on;
    return 1;
}

/* The interpolation's camera cut rules between two steps' keys (a mode
 * change, an eye jump over 50 m, the view turning over ~30 degrees). */
static int cam_is_cut(const CamKey *a, const CamKey *b)
{
    float d[3], fa[3], fb[3];
    int j;
    for (j = 0; j < 3; j++) {
        d[j] = b->eye[j] - a->eye[j];
        fa[j] = a->at[j] - a->eye[j];
        fb[j] = b->at[j] - b->eye[j];
    }
    if (a->mode != b->mode || !(len3(d) <= SNAP_CAM_M))
        return 1;
    norm3(fa);
    norm3(fb);
    return !(dot3(fa, fb) >= 0.866f);
}

/* A camera's eye, target and up blended between two steps' keys at u. Up to
 * u = 1 that is the interpolation, unchanged. Beyond it (a prediction), the
 * eye and the target are carried on separately, which goes wrong when the
 * game pulls the camera in fast (seen in BW_PREDLOG, mission 1: the eye 6.7 m
 * from the boat, 3.2 m a step later, probably pushed in by scenery): carried
 * on, the eye passes the target and the view flips by 90-150 degrees. So a
 * prediction is held at the newest step (returns 0, the keys of b) when it
 * would bring the eye under half the step's distance from the target, or
 * turn the view more than the step's own turn, carried on, plus 10 degrees. */
static int cam_pred(const CamKey *z, const CamKey *a, const CamKey *b, float u,
                    float *eye, float *at, float *up)
{
    float fa[3], fb[3], fp[3], db, dp, ta, tp;
    int j;
    if (s_pred_order == 2 && z && u > 1.0f && !cam_is_cut(z, a)
        && smooth3(z->eye, a->eye, b->eye) && smooth3(z->at, a->at, b->at)) {
        /* order 2: the parabola through the three steps' keys (z = the step
         * before a, the same shot) */
        quad3f(z->eye, a->eye, b->eye, u - 1.0f, eye);
        quad3f(z->at, a->at, b->at, u - 1.0f, at);
        quad3f(z->up, a->up, b->up, u - 1.0f, up);
    } else {
        for (j = 0; j < 3; j++) {
            eye[j] = a->eye[j] + (b->eye[j] - a->eye[j]) * u;
            at[j]  = a->at[j]  + (b->at[j]  - a->at[j])  * u;
            up[j]  = a->up[j]  + (b->up[j]  - a->up[j])  * u;
        }
    }
    if (!(u > 1.0f))
        return 1;
    for (j = 0; j < 3; j++) {
        fa[j] = a->at[j] - a->eye[j];
        fb[j] = b->at[j] - b->eye[j];
        fp[j] = at[j] - eye[j];
    }
    db = len3(fb);
    dp = len3(fp);
    norm3(fa);
    norm3(fb);
    norm3(fp);
    ta = acosf(fminf(1.0f, fmaxf(-1.0f, dot3(fa, fb))));      /* the step's turn */
    tp = acosf(fminf(1.0f, fmaxf(-1.0f, dot3(fb, fp))));      /* the prediction's */
    if (dp >= 0.5f * db && tp <= ta * (u - 1.0f) + 0.1745f)
        return 1;
    for (j = 0; j < 3; j++) {
        eye[j] = b->eye[j];
        at[j] = b->at[j];
        up[j] = b->up[j];
    }
    s_pl_camheld++;
    return 0;
}

static uint64_t pl_frame_bit(void)
{
    return s_pl_nf < PL_MAXF ? 1ull << s_pl_nf : 0;
}

static double pl_host_ms(void)
{
    static double f;
    LARGE_INTEGER c;
    if (!f) {
        LARGE_INTEGER q;
        QueryPerformanceFrequency(&q);
        f = (double)q.QuadPart;
    }
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / f;
}

static void pl_frame(uint32_t base, float t, float uc, float ub, float un)
{
    uint32_t b = s_nboats ? s_boat_node[0] : 0;
    s_pl_frames++;
    if (s_pl_nf < PL_MAXF) {
        s_pl_f[s_pl_nf].t = t;
        s_pl_f[s_pl_nf].un = un;
        s_pl_nf++;
    } else {
        s_pl_over++;
    }
    fprintf(s_plog, "F %lu %u %u %.6f %.6f %.6f %.6f %u %.3f %.3f %.4f", s_pl_frames, s_snapshots,
            s_steps_since_draw, t, uc, ub, un, s_pl_camflags, s_alpha_gnow_ms, pl_host_ms(),
            s_alpha_raw);
    if (s_pl_camflags & 1u)
        fprintf(s_plog, " %.9g %.9g %.9g", s_pl_eye[0], s_pl_eye[1], s_pl_eye[2]);
    else
        fputs(" - - -", s_plog);
    if (b && base) {
        const float *m = s_touched[b - 1] ? s_wrote[b - 1].m
                       : (const float *)HP(base + (b - 1) * NODE_SIZE);
        fprintf(s_plog, " %.9g %.9g %.9g", m[12], m[13], m[14]);
    } else {
        fputs(" - - -", s_plog);
    }
    fprintf(s_plog, " %lu %lu %lu\n", s_pl_flerp, s_pl_fsnap, s_pl_camheld);
    s_pl_flerp = s_pl_fsnap = 0;
    s_pl_camheld = 0;
    {   /* "P": the pad block at this frame's draw (the newest the next step will
         * read), when it changed (lat-p, the input-aware guard). Read-only. */
        static uint8_t last[18];
        static int seen;
        uint8_t blk[18];
        if (pl_pad(blk) && (!seen || memcmp(blk, last, sizeof blk))) {
            memcpy(last, blk, sizeof blk);
            seen = 1;
            fprintf(s_plog, "P %lu", s_pl_frames);
            pl_pad_hex(blk);
            fputc('\n', s_plog);
        }
    }
}

static void pl_hist(PlHist *h, double v)
{
    int b = 0;
    h->n++;
    h->sum += v;
    if (v > h->max)
        h->max = v;
    if (v >= 1e-5) {
        b = 1 + (int)floor((log10(v) + 5.0) * 10.0);
        if (b > PL_NB - 1)
            b = PL_NB - 1;
    }
    h->bin[b]++;
}

static double pl_pos_err(const Mat *a, const Mat *b)
{
    double dx = (double)a->m[12] - b->m[12], dy = (double)a->m[13] - b->m[13];
    double dz = (double)a->m[14] - b->m[14];
    return sqrt(dx * dx + dy * dy + dz * dz);
}

/* The angle of the rotation between two bases (rows 0-2, each normalised):
 * |Ra - Rb|^2 (Frobenius) = 8 sin^2(angle / 2), accurate for small angles. */
static double pl_ang_err(const Mat *a, const Mat *b)
{
    double s = 0.0, x;
    int i, j;
    for (i = 0; i < 3; i++) {
        double la = 0.0, lb = 0.0;
        for (j = 0; j < 3; j++) {
            la += (double)a->m[i*4 + j] * a->m[i*4 + j];
            lb += (double)b->m[i*4 + j] * b->m[i*4 + j];
        }
        la = la > 1e-24 ? 1.0 / sqrt(la) : 0.0;
        lb = lb > 1e-24 ? 1.0 / sqrt(lb) : 0.0;
        for (j = 0; j < 3; j++) {
            double d = a->m[i*4 + j] * la - b->m[i*4 + j] * lb;
            s += d * d;
        }
    }
    x = sqrt(s / 8.0);
    return 2.0 * asin(x > 1.0 ? 1.0 : x) * (180.0 / 3.14159265358979323846);
}

static void pl_init_p(void)
{
    const char *e = getenv("BW_PREDLOG_P");
    const char *p = e && *e ? e : "0,0.25,0.5,0.75,1";
    while (*p && s_pl_np < PL_NP_MAX) {
        char *end;
        double v = strtod(p, &end);
        if (end == p)
            break;
        s_pl_p[s_pl_np++] = (float)(v < 0.0 ? 0.0 : v > 2.0 ? 2.0 : v);
        p = *end == ',' ? end + 1 : end;
    }
}

/* Every node a frame since the last step blended (or drew at rest), judged
 * now that the next step is in memory: for each p, what a frame at u = t + p
 * shows (the blend of the last two steps) against the true pose at the time
 * it shows (the blend of the newest step and the next) and at its game clock. */
static void pl_judge_nodes(uint32_t base)
{
    uint32_t i;
    unsigned f, k;
    if (!base || !s_pl_nf)
        return;
    for (i = 0; i < NODE_N; i++) {
        uint64_t mb = s_pl_blend[i], mr = s_pl_rest[i];
        uint32_t nd = base + i * NODE_SIZE;
        Mat nx, now, sh, ts;
        if (!(mb | mr))
            continue;
        if (!HU16(nd + NODE_FLAGS)
            || ((HU32(nd + NODE_HASH) ^ HU32(nd + NODE_DRAWABLE)) | 1u) != s_id_cur[i]) {
            s_pl_gone++;
            continue;
        }
        memcpy(&nx, HP(nd), sizeof nx);
        if (!mb && !memcmp(&nx, &s_cur[i], sizeof nx))
            continue;                                   /* at rest through the next step too */
        if (!lerp_mat(&s_cur[i], &nx, 0.0f, &now)) {
            s_pl_snapnext++;                            /* the next step jumps: not judged */
            continue;
        }
        for (f = 0; f < s_pl_nf; f++) {
            uint64_t bit = 1ull << f;
            float t = s_pl_f[f].t;
            if (!((mb | mr) & bit))
                continue;
            lerp_mat(&s_cur[i], &nx, t, &now);
            s_pl_judged++;
            for (k = 0; k < s_pl_np; k++) {
                float u = t + s_pl_p[k];
                if (u > 2.0f) {
                    s_pl_beyond++;                  /* its truth needs the step after next */
                    continue;
                }
                /* as the frame would draw it at this p (with the run's fade) */
                if (pred_node(i, u, s_pl_p[k] > 0.0f ? pred_fade_w(t) : 1.0f, &sh) <= 0)
                    sh = s_cur[i];
                /* the true pose at the time the frame shows: up to the newest
                 * step, the blend before it (an interpolation's own pose: 0);
                 * after it, the blend towards the next step */
                if (u <= 1.0f) {
                    if (!(mb & bit) || !lerp_mat(&s_prev[i], &s_cur[i], u, &ts))
                        ts = s_cur[i];
                } else {
                    lerp_mat(&s_cur[i], &nx, u - 1.0f, &ts);
                }
                pl_hist(&s_pl_h[k][0], pl_pos_err(&sh, &ts));
                pl_hist(&s_pl_h[k][1], pl_ang_err(&sh, &ts));
                pl_hist(&s_pl_h[k][2], pl_pos_err(&sh, &now));
                pl_hist(&s_pl_h[k][3], pl_ang_err(&sh, &now));
            }
        }
    }
}

/* Port 0's pad block as the game's own read (Pad_PollState, in Devices_Update at
 * the pass's start) left it: 18 bytes, digital word, 8 analog bytes, 4 sticks.
 * 0 when the pad is not open. Read-only. */
static int pl_pad(uint8_t blk[18])
{
    uint32_t pad = HU32(DEVICES_VA + DEVICES_PADS);
    if (!lat_guest_ok(pad) || !HU32(pad + PAD_HANDLE))
        return 0;
    memcpy(blk, HP(pad + PAD_BLOCK), 18);
    return 1;
}

static void pl_pad_hex(const uint8_t blk[18])
{
    unsigned i;
    fputc(' ', s_plog);
    for (i = 0; i < 18; i++)
        fprintf(s_plog, "%02x", blk[i]);
}

/* "X" after each step's S line: player 1's boat state the guards may read
 * (lat-p, for the event analysis): World_OnContact's touching flag (+0x67C,
 * cleared by Boat_Update) and the contact time Boat_Update accumulates from it
 * (+0x680 ms, 0 when not touching; Boat_UpdateScrapeSound starts a scrape over
 * 15 ms), Boat_RamBoat's cooldown (+0x6A0, 5 after a ram), throttle and rudder
 * (+0x398/+0x39C, as BW_STEPLOG), and the pad block the step read. Read-only. */
static void pl_step_state(void)
{
    uint32_t n = HU32(PLAYERS_VA + 0xC0);
    uint32_t boat = n >= 1 && n <= 4 ? HU32(PLAYERS_VA + 8) : 0;
    uint8_t blk[18];
    if (!lat_guest_ok(boat))
        boat = 0;
    fprintf(s_plog, "X %u", s_snapshots);
    if (boat)
        fprintf(s_plog, " %u %u %d %.6g %.6g", (unsigned)HU8(boat + 0x67C), HU32(boat + 0x680),
                (int)HU32(boat + 0x6A0), HF32(boat + 0x398), HF32(boat + 0x39C));
    else
        fputs(" - - - - -", s_plog);
    if (pl_pad(blk))
        pl_pad_hex(blk);
    else
        fputs(" -", s_plog);
    /* view 0's shake, as Camera_UpdateShake (0x766D0) keeps it at the view's
     * record (+0x21C a view) +0x299 (on), +0x29C (ms left), +0x2A0 (strength) */
    fprintf(s_plog, " %u %u %.6g\n", (unsigned)HU8(CAM_VA + 0x299u), HU32(CAM_VA + 0x29Cu),
            HF32(CAM_VA + 0x2A0u));
}

static void pl_step(uint32_t base, int before)
{
    unsigned n, k;
    if (!s_pl_np)
        pl_init_p();
    if (before) {
        double t0 = pl_host_ms();
        pl_judge_nodes(base);
        s_pl_cost_us += (pl_host_ms() - t0) * 1000.0;
        s_pl_cost_n++;
        s_pl_nf = 0;
        memset(s_pl_blend, 0, sizeof s_pl_blend);
        memset(s_pl_rest, 0, sizeof s_pl_rest);
        return;
    }
    fprintf(s_plog, "S %u %u %u", s_snapshots, HU32(WEAPONS_MS_VA), s_nviews);
    for (n = 0; n < s_nviews; n++) {
        const CamKey *c = &s_cam_cur[n];
        fprintf(s_plog, " %u %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.6g", c->mode,
                c->eye[0], c->eye[1], c->eye[2], c->at[0], c->at[1], c->at[2],
                c->up[0], c->up[1], c->up[2], HF32(CAM_VA + n * CAM_STRIDE + 0x148u));
    }
    fprintf(s_plog, " %u", s_nboats);
    for (n = 0; n < s_nboats; n++) {
        uint32_t b = s_boat_node[n];
        fprintf(s_plog, " %u", b);
        for (k = 0; k < 16; k++)
            fprintf(s_plog, " %.9g", b ? s_cur[b - 1].m[k] : 0.0f);
    }
    fputc('\n', s_plog);
    pl_step_state();
    fflush(s_plog);
    if (s_snapshots % 300u == 0) {
        unsigned j;
        fprintf(s_plog, "H %u %u %d -5 10\n", s_snapshots, s_pl_np, PL_NB);
        for (k = 0; k < s_pl_np; k++)
            for (n = 0; n < 4; n++) {
                const PlHist *h = &s_pl_h[k][n];
                fprintf(s_plog, "h %.3f %u %u %.6g %.6g", s_pl_p[k], n, h->n, h->sum, h->max);
                for (j = 0; j < PL_NB; j++)
                    fprintf(s_plog, " %u", h->bin[j]);
                fputc('\n', s_plog);
            }
        fprintf(s_plog, "He %u %lu %lu %lu %lu %lu %lu\n", s_snapshots, s_pl_judged, s_pl_snapnext,
                s_pl_gone, s_pl_over, s_pl_frames, s_pl_beyond);
        fprintf(s_plog, "C %u %lu %.0f\n", s_snapshots, s_pl_cost_n, s_pl_cost_us);
        fflush(s_plog);
    }
}

/* ---- the guard (D-027, lat-p; BW_HIFPS_PREDICT_GUARD=1) ------------------- *
 * What the newest step left (read at its snapshot, read-only): the right
 * stick in the pad block the step read, how many steps since it last moved,
 * view 0's camera shake (Camera_UpdateShake: the view record +0x299), and
 * player 1's boat's fresh contact (Boat_Update's contact time +0x680 under
 * GUARD_CONTACT_MS, from World_OnContact's flag) or ram (Boat_RamBoat's
 * cooldown +0x6A0). Measured offline first (tools/bw_predict_guard.py). */
#define GUARD_LOOK_STEPS  8u
#define GUARD_CONTACT_MS  70u
/* The look swing's dead zone (seen with the scripted pad's partial throws:
 * 15% moves nothing, 25% about 1.5 degrees a step, 100% 25): a real pad
 * rests a little off centre and jitters, so the guard reads the stick only
 * past 20%, and counts it as moved only when that changes by over 5%. */
#define GUARD_STICK_DZ    6553
#define GUARD_STICK_MOVE  1638

static int16_t guard_stick(int16_t v)
{
    return (v > -GUARD_STICK_DZ && v < GUARD_STICK_DZ) ? 0 : v;
}
static int      s_g_look_step;            /* the step's pad: the right stick off centre */
static int16_t  s_g_rx, s_g_ry;           /* ... and its position */
static unsigned s_g_look_age = 99;        /* steps since the right stick last moved */
static uint8_t  s_g_shake[2];             /* view 0 shaking: this step, the one before */
static int      s_g_contact;              /* a fresh contact or a ram in this step */
static double   s_g_tau = -1.0;           /* the last frame's step clock (steps + t) */

static void guard_step(void)
{
    uint8_t blk[18];
    uint32_t n = HU32(PLAYERS_VA + 0xC0);
    uint32_t boat = n >= 1 && n <= 4 ? HU32(PLAYERS_VA + 8) : 0;
    int16_t rx = 0, ry = 0;
    if (pl_pad(blk)) {
        memcpy(&rx, blk + 14, 2);
        memcpy(&ry, blk + 16, 2);
    }
    rx = guard_stick(rx);
    ry = guard_stick(ry);
    s_g_look_age = (abs(rx - s_g_rx) > GUARD_STICK_MOVE || abs(ry - s_g_ry) > GUARD_STICK_MOVE)
                   ? 0u : (s_g_look_age < 99u ? s_g_look_age + 1u : 99u);
    s_g_rx = rx;
    s_g_ry = ry;
    s_g_look_step = rx != 0 || ry != 0;
    s_g_shake[1] = s_g_shake[0];
    s_g_shake[0] = HU8(CAM_VA + 0x299u);
    s_g_contact = 0;
    if (lat_guest_ok(boat)) {
        uint32_t cms = HU32(boat + 0x680);
        s_g_contact = (cms > 0u && cms <= GUARD_CONTACT_MS) || (int32_t)HU32(boat + 0x6A0) > 0;
    }
}

/* This frame's p: toward 0 while a guard holds, never faster than the display
 * clock standing still (so it never runs backwards), back toward s_pred at
 * s_pred_guard_rise a step when calm. The newest pad (this pass's own poll,
 * the one the next step will read) starts the look guard before its step. */
static float guard_frame_p(float t)
{
    double tau = (double)s_snapshots + (double)t, dt;
    float target = s_pred, p = s_pred_now;
    int on = s_g_look_step || s_g_look_age < GUARD_LOOK_STEPS || s_g_shake[0] || s_g_shake[1] || s_g_contact;
    if (!on) {
        uint8_t blk[18];
        int16_t rx = 0, ry = 0;
        if (pl_pad(blk)) {
            memcpy(&rx, blk + 14, 2);
            memcpy(&ry, blk + 16, 2);
        }
        on = guard_stick(rx) != 0 || guard_stick(ry) != 0;
    }
    if (on)
        target = 0.0f;
    dt = s_g_tau >= 0.0 ? tau - s_g_tau : 0.0;
    if (dt < 0.0)
        dt = 0.0;                           /* a new boot or a reset clock */
    if (dt > 2.0)
        dt = 2.0;
    s_g_tau = tau;
    if (target < p) {
        p -= (float)dt;                     /* the display clock stands still */
        if (p < target)
            p = target;
    } else if (target > p) {
        p += s_pred_guard_rise * (float)dt;
        if (p > target)
            p = target;
    }
    return p;
}

static void apply_interp(float t)
{
    uint32_t base = HU32(NODEPOOL_PTR_VA), i;
    unsigned n;
    /* lat-p: the blend factor per thing drawn. t alone is the interpolation
     * (one step behind); t + p predicts that much of a step further (u up to
     * 2: the last step's motion carried on). p is s_pred, or less under the
     * guard. */
    float p = s_pred_now = s_pred_guard ? guard_frame_p(t) : s_pred;
    float un = t + (s_pred_scope >= PRED_ALL ? p : 0.0f);
    float ub = t + (s_pred_scope >= PRED_BOAT ? p : 0.0f);
    float uc = t + p;
    float w = p > 0.0f ? pred_fade_w(t) : 1.0f;          /* the cross-fade, 1 = none */
    uint64_t bit = s_plog ? pl_frame_bit() : 0;
    /* BW_LATLOG (lat-m): how far into the newest step the player's boat is
     * drawn; with the fade, the newest step arrives at w of it (inferred for
     * a boat moving through both steps, the usual case) */
    s_lat_nodew = ub > t ? ub * w : ub;
    s_pl_camflags = 0;
    memset(s_touched, 0, sizeof s_touched);
    if (base) {
        for (i = 0; i < NODE_N; i++) {
            uint32_t nd = base + i * NODE_SIZE;
            Mat o;
            float u, wi;
            int r;
            if (!s_id_cur[i] || s_id_prev[i] != s_id_cur[i])
                continue;                                   /* new, freed or reused */
            u = s_pred_scope == PRED_BOAT && is_boat(i) ? ub : un;
            wi = u > t ? w : 1.0f;                          /* only a prediction fades */
            if (!memcmp(&s_prev[i], &s_cur[i], sizeof(Mat))
                && !(wi < 1.0f && s_id_prev2[i] == s_id_cur[i]
                     && memcmp(&s_prev2[i], &s_prev[i], sizeof(Mat)))) {
                if (bit && !memcmp(HP(nd), &s_cur[i], sizeof(Mat)))
                    s_pl_rest[i] |= bit;                    /* drawn at rest (BW_PREDLOG) */
                continue;                                   /* did not move (and isn't
                                                               fading out of a move) */
            }
            if (memcmp(HP(nd), &s_cur[i], sizeof(Mat)))
                continue;                                   /* written since the step
                                                               (animation, attachments) */
            r = pred_node(i, u, wi, &o);
            if (r < 0) {
                s_st_snapped++;
                s_pl_fsnap++;
                continue;
            }
            if (r == 0) {
                if (bit)
                    s_pl_rest[i] |= bit;
                continue;
            }
            memcpy(HP(nd), &o, sizeof(Mat));
            s_wrote[i] = o;
            s_touched[i] = 1;
            s_st_lerped++;
            s_pl_flerp++;
            if (bit)
                s_pl_blend[i] |= bit;
        }
    }
    s_rcam_n = 0;
    for (n = 0; n < MAX_VIEWS; n++)
        s_hud_u[n] = 1.0f;                                  /* the HUD's world points: at the step */
    if (s_nviews && s_nviews == s_nviews_prev) {
        s_rcam_n = s_nviews;
        for (n = 0; n < s_nviews; n++) {
            const CamKey *a = &s_cam_prev[n], *b = &s_cam_cur[n];
            float eye[3], at[3], up[3], m[16], u;
            int j;
            memcpy(s_rcam_saved[n], HP(RCAM_VA(n + 1)), RCAM_SIZE);
            /* A cut (show the step): a mode change, an eye jump over 50 m, or,
             * within 50 m (a cutscene's next shot), the view turning more than
             * ~30 degrees in one step, which no camera move in play does (that
             * would be 900 degrees a second). */
            if (cam_is_cut(a, b))
                continue;
            u = uc;                                         /* t, or t + the prediction */
            if (!cam_pred(s_pred_order == 2 && s_have_prev2 && s_nviews_prev2 == s_nviews
                          ? &s_cam_prev2[n] : NULL, a, b, u, eye, at, up))
                u = 1.0f;                                   /* held at the step */
            if (n < MAX_VIEWS)
                s_hud_u[n] = u;                             /* the HUD's world points follow */
            if (n == 0)
                s_lat_camw = u;                             /* BW_LATLOG: view 0 blended */
            if (w < 1.0f && s_nviews_prev2 == s_nviews && !cam_is_cut(&s_cam_prev2[n], a)) {
                /* the cross-fade: from the previous step's prediction, carried on */
                float eo[3], ao[3], uo[3];
                if (cam_pred(s_pred_order == 2 && s_have_prev3 && s_nviews_prev3 == s_nviews
                             ? &s_cam_prev3[n] : NULL, &s_cam_prev2[n], a, u + 1.0f, eo, ao, uo)) {
                    for (j = 0; j < 3; j++) {
                        eye[j] = eo[j] + (eye[j] - eo[j]) * w;
                        at[j]  = ao[j] + (at[j]  - ao[j]) * w;
                        up[j]  = uo[j] + (up[j]  - uo[j]) * w;
                    }
                    /* BW_LATLOG: the newest step reaches the picture at w of
                     * its weight (the faded-out prediction never saw it) */
                    if (n == 0)
                        s_lat_camw = u * w;
                }
            }
            s_pl_camflags |= 1u << n;
            if (n == 0)
                memcpy(s_pl_eye, eye, sizeof s_pl_eye);
            look_at_lh(eye, at, up, m);
            memcpy(HP(RCAM_VA(n + 1)), m, sizeof m);
            /* The simulation camera's copy of the view (Camera_ApplyView
             * writes both), which Camera_WorldToScreen and others read. */
            memcpy(s_scam_saved[n], HP(CAM_VA + 8u + 0x40u * n), 64);
            memcpy(HP(CAM_VA + 8u + 0x40u * n), m, sizeof m);
            s_scam_written |= 1u << n;
            memcpy(s_cam_wrote[n], m, sizeof m);
            s_cam_written |= 1u << n;
            s_st_cam++;
            if (s_eye && n < MAX_VIEWS) {
                /* HIFPS_EYE: the eye this view was built from, for the frame */
                uint32_t ev = CAM_VA + n * CAM_STRIDE + CAM_EYE;
                memcpy(s_eye_step[n], HP(ev), 12);
                memcpy(s_eye_drawn[n], eye, sizeof s_eye_drawn[n]);
                memcpy(HP(ev), eye, 12);
                s_eye_written |= 1u << n;
                s_st_eye++;
            }
        }
    }
    if (s_plog)
        pl_frame(base, t, uc, ub, un);
}

static void restore_interp(void)
{
    uint32_t base = HU32(NODEPOOL_PTR_VA), i;
    unsigned n;
    if (base)
        for (i = 0; i < NODE_N; i++)
            if (s_touched[i] && !memcmp(HP(base + i * NODE_SIZE), &s_wrote[i], sizeof(Mat)))
                memcpy(HP(base + i * NODE_SIZE), &s_cur[i], sizeof(Mat));
    /* The render cameras go back to their bytes from before the frame. A
     * camera the frame itself rewrote (it would have drawn with the stepped
     * view, not ours) is counted: BW_HIFPS_LOG "camera overwritten". */
    for (n = 0; n < s_rcam_n; n++) {
        if ((s_cam_written & (1u << n)) && memcmp(HP(RCAM_VA(n + 1)), s_cam_wrote[n], 64))
            s_st_camover++;
        memcpy(HP(RCAM_VA(n + 1)), s_rcam_saved[n], RCAM_SIZE);
        if ((s_scam_written & (1u << n)) && !memcmp(HP(CAM_VA + 8u + 0x40u * n), s_cam_wrote[n], 64))
            memcpy(HP(CAM_VA + 8u + 0x40u * n), s_scam_saved[n], 64);
    }
    /* HIFPS_EYE: the step's eye back, unless the frame wrote one itself
     * (counted with the cameras: BW_HIFPS_LOG "eyes ... overwritten") */
    for (n = 0; n < MAX_VIEWS; n++)
        if (s_eye_written & (1u << n)) {
            uint32_t ev = CAM_VA + n * CAM_STRIDE + CAM_EYE;
            if (!memcmp(HP(ev), s_eye_drawn[n], 12))
                memcpy(HP(ev), s_eye_step[n], 12);
            else
                s_st_eyeover++;
        }
    s_eye_written = 0;
    s_cam_written = 0;
    s_scam_written = 0;
    s_rcam_n = 0;
}

/* HIFPS_EYE: the parts of a drawn frame that keep state of their own read the
 * step's eye, as before: the rain (Weather_RenderRain spawns and wraps its
 * drops around the eye) and the lightning bolt (Sky_BuildLightningBolt, from
 * Sky_UpdateLightning, aims it by target minus eye), both kept from frame to
 * frame. to_step 1 puts the step's eye in, 0 the drawn one back. */
static void eye_swap(int to_step)
{
    unsigned n;
    for (n = 0; n < MAX_VIEWS; n++)
        if (s_eye_written & (1u << n))
            memcpy(HP(CAM_VA + n * CAM_STRIDE + CAM_EYE),
                   to_step ? (const void *)s_eye_step[n] : (const void *)s_eye_drawn[n], 12);
}

/* HIFPS_EYE, for the ocean: Ocean_Render 0x38EC0 copies the eye to the
 * ocean's +0x30..+0x38 just before its quadtree walk (Ocean_CullQuadtree
 * 0x38720), which picks the tiles by it; Ocean_SetPass1Constants 0x38A60 and
 * Ocean_SetPass2Constants 0x38C60 then upload it (z negated) as the sea's
 * c[146] (seen in the generated C). The walk's wrapper (bw_visextra_hooks.c)
 * calls this around its outermost call: to_step 1 before it, when the copy is
 * a drawn eye (1 is returned), so the tiles stay the step's (the high frame
 * rate's water reuse and blending work per step's tiles); 0 after it, the
 * drawn eye back for the constants. */
static int s_eye_ocean_view = -1;
int bw_hifps_eye_ocean(uint32_t ocean, int to_step)
{
    unsigned n;
    if (!to_step) {
        n = (unsigned)s_eye_ocean_view;
        s_eye_ocean_view = -1;
        if (n < MAX_VIEWS && (s_eye_written & (1u << n)) && !memcmp(HP(ocean + 0x30u), s_eye_step[n], 12))
            memcpy(HP(ocean + 0x30u), s_eye_drawn[n], 12);
        return 0;
    }
    if (!s_eye_written || ocean < 0x00010000u)
        return 0;
    for (n = 0; n < MAX_VIEWS; n++)
        if ((s_eye_written & (1u << n)) && !memcmp(HP(ocean + 0x30u), s_eye_drawn[n], 12)) {
            memcpy(HP(ocean + 0x30u), s_eye_step[n], 12);
            s_eye_ocean_view = (int)n;
            s_st_eyeocean++;
            return 1;
        }
    return 0;
}

/* ---- the drawn frame's own dice ---------------------------------------- */

/* crt_getptd 0x000E3804 (the CRT's per-thread data; crt_rand's seed is at
 * +0x14), called as the game calls it: a return address pushed, `ret` pops
 * it. The caller's registers are put back: this runs at a function entry. */
extern void sub_000E3804(void);

static uint32_t ptd_va(void)
{
    static uint32_t ptd;
    if (!ptd) {
        uint32_t ea = g_eax, ec = g_ecx, ed = g_edx, bp = g_ebp, sbp = g_seh_ebp;
        g_esp -= 4;
        HU32(g_esp) = 0x00028600u;
        sub_000E3804();
        ptd = g_eax;
        g_eax = ea; g_ecx = ec; g_edx = ed; g_ebp = bp; g_seh_ebp = sbp;
        if (ptd < 0x00010000u || ptd >= 0x08000000u)
            ptd = 1;                                        /* unusable: never again */
    }
    return ptd > 1 ? ptd : 0;
}

/* ---- the water between steps (R4 step 5; D7, second task) ---------------
 *
 * The Water object (WaterWorld+0 -> [0x1D77F8]; Water_Update 0x95230, read
 * for this, seen): a 32 x 32 FFT height field per buffer, two buffers. +0x0C
 * is the newest buffer (written by the step's FFT), +0x10 the one before
 * (both toggle every Water_Update); +0x08 counts updates. Heights at +0x2AB8
 * + buffer*0x1000 (floats); the Bezier patches built from them at +0x4AB8 +
 * buffer*0x10000 (64 bytes per cell), with a "built" byte per cell at +0x24AB8
 * + buffer*0x400 that Water_Update clears for the new buffer
 * (Water_GetBezierPatch 0x94280). The ocean's vertices (Water_HeightAndSlopeAt
 * 0x94930) read the newest buffer only. WaterWorld keeps its clock at +0xACA4
 * (int ms), +0xACA8 (seconds) and +0xACAC (the step before, seconds)
 * (WaterWorld_Tick 0x987B0); ripples and wakes are evaluated against +0xACA8
 * and skip themselves before their start time (WaterWorld_DisturbanceHeightAt
 * 0x98EB0 compares it with each one's start and end), so an earlier draw time
 * is safe.
 *
 * So for a drawn frame between steps: the newest buffer's heights become the
 * blend of the two (prev + (newest - prev) * t, the same t as the camera and
 * nodes), its patch cache is emptied so the patches are rebuilt from the
 * blend, and the clock reads the blended time. After the frame the heights,
 * the patch bytes, the cache flags and the clock are put back byte for byte,
 * so the next step sees exactly what it would have. BW_HIFPS_WATER=0 turns it
 * off; BW_HIFPS_WATER_VERIFY=1 compares the whole Water object and the
 * WaterWorld clock before and after each frame and counts differing bytes. */
#include "bw_waterworld.h"   /* WW_VA, its clock, the Water object (item 71) */

static int s_water = 1, s_waterverify;
static int s_water_on_frame;          /* BW_HIFPS_WATER=2: the buffer is blended */
static int s_water_lerp_frame;         /* BW_HIFPS_WATER=1: the batches blend their results */
static float s_water_alpha;
static unsigned long s_st_obprev, s_st_oblerp;
static uint32_t s_w_obj, s_w_new;
static uint8_t s_w_h[W_HEIGHTS_SZ], s_w_pf[W_PFLAG_SZ];
static uint8_t *s_w_pd, *s_w_verify;
static uint32_t s_w_ms, s_w_s;
static uint8_t s_w_clock_verify[12];
static unsigned long s_st_water, s_st_waterbad;

static void apply_water(float t)
{
    uint32_t w = HU32(WW_VA), nw, pv, hn, hp, i;
    float tn, tp;
    s_water_on_frame = 0;
    if (!s_water || !w || w < 0x00010000u)
        return;
    nw = HU32(w + W_IDX_NEW);
    pv = HU32(w + W_IDX_PREV);
    if (nw > 1u || pv > 1u || nw == pv || HU32(w + W_UPDATES) < 2u)
        return;
    if (s_water == 1) {                     /* the fast way: see sub_00098130 */
        s_water_lerp_frame = 1;
        s_water_alpha = t;
        s_st_water++;
        return;
    }
    if (t > 1.0f)
        t = 1.0f;          /* lat-p: the exact blend moves the water's clock; never past the step */
    if (!s_w_pd && !(s_w_pd = (uint8_t *)malloc(W_PATCH_SZ)))
        return;
    if (s_waterverify) {
        if (!s_w_verify && !(s_w_verify = (uint8_t *)malloc(W_OBJ_SIZE)))
            return;
        memcpy(s_w_verify, HP(w), W_OBJ_SIZE);
        memcpy(s_w_clock_verify, HP(WW_VA + WW_CLOCK_MS), 12);
    }
    hn = w + W_HEIGHTS + nw * W_HEIGHTS_SZ;
    hp = w + W_HEIGHTS + pv * W_HEIGHTS_SZ;
    memcpy(s_w_h, HP(hn), W_HEIGHTS_SZ);
    memcpy(s_w_pf, HP(w + W_PFLAG + nw * W_PFLAG_SZ), W_PFLAG_SZ);
    memcpy(s_w_pd, HP(w + W_PATCH + nw * W_PATCH_SZ), W_PATCH_SZ);
    for (i = 0; i < W_HEIGHTS_SZ / 4; i++) {
        float a = HF32(hp + 4 * i), b = ((const float *)s_w_h)[i];
        HF32(hn + 4 * i) = a + (b - a) * t;
    }
    memset(HP(w + W_PFLAG + nw * W_PFLAG_SZ), 0, W_PFLAG_SZ);
    s_w_ms = HU32(WW_VA + WW_CLOCK_MS);
    s_w_s = HU32(WW_VA + WW_CLOCK_S);
    tn = HF32(WW_VA + WW_CLOCK_S);
    tp = HF32(WW_VA + WW_CLOCK_PREV_S);
    if (tn > tp && tn - tp < 1.0f) {
        HF32(WW_VA + WW_CLOCK_S) = tp + (tn - tp) * t;
        HU32(WW_VA + WW_CLOCK_MS) = s_w_ms - (uint32_t)((tn - tp) * (1.0f - t) * 1000.0f + 0.5f);
    }
    s_w_obj = w;
    s_w_new = nw;
    s_water_on_frame = 1;
    s_st_water++;
}

static void restore_water(void)
{
    uint32_t w = s_w_obj, nw = s_w_new;
    s_water_lerp_frame = 0;
    if (!s_water_on_frame)
        return;
    memcpy(HP(w + W_HEIGHTS + nw * W_HEIGHTS_SZ), s_w_h, W_HEIGHTS_SZ);
    memcpy(HP(w + W_PFLAG + nw * W_PFLAG_SZ), s_w_pf, W_PFLAG_SZ);
    memcpy(HP(w + W_PATCH + nw * W_PATCH_SZ), s_w_pd, W_PATCH_SZ);
    HU32(WW_VA + WW_CLOCK_MS) = s_w_ms;
    HU32(WW_VA + WW_CLOCK_S) = s_w_s;
    if (s_waterverify && s_w_verify) {
        const uint8_t *now = (const uint8_t *)HP(w);
        uint32_t i, diff = 0;
        for (i = 0; i < W_OBJ_SIZE; i++)
            diff += now[i] != s_w_verify[i];
        diff += memcmp(HP(WW_VA + WW_CLOCK_MS), s_w_clock_verify, 12) != 0;
        if (diff) {
            static int shown;
            s_st_waterbad++;
            if (shown++ < 8) {
                uint32_t first = 0;
                while (first < W_OBJ_SIZE && now[first] == s_w_verify[first])
                    first++;
                fprintf(stderr, "[BW] hifps water verify: %u bytes differ after the frame "
                        "(first at Water+0x%X)\n", diff, first);
            }
        }
    }
    s_water_on_frame = 0;
}

/* ---- the HUD's world markers between steps ------------------------------
 *
 * HudMarkers_Update 0x84AE0 and HudReticle_Update 0x85850 (seen; virtual,
 * thiscall, one stack argument, ret 4) project world points to the screen with
 * Camera_WorldToScreen once per step: the markers' world positions at
 * this+0x9E4 + 12*i (i < 8, count at +0x20) and the reticle's two points at
 * +0x1C0/+0x1CC are projected in place, and the quads drawn from them are
 * written into the same object. Camera_WorldToScreen uses the simulation
 * camera's view matrix (0x1CE4A0 + 8 + 0x40*n, written by Camera_ApplyView)
 * and the render camera's projection (seen in its code).
 *
 * Each call during a step is recorded with the object's bytes as they were
 * before it. On a drawn frame between steps, with the camera blended (render
 * and simulation view matrices), the game's own function runs again on those
 * bytes, so the markers are projected with the camera that is drawn; after
 * the frame every byte it wrote goes back to the step's value. Object sizes
 * cover every offset the two functions write (markers 0xA6C, reticle 0x1E0).
 * Coordinated with D5's widescreen: its Camera_WorldToScreen wrapper
 * (bw_remaster_hooks.c) runs inside the replay as in the step.
 * BW_HIFPS_HUD=0 turns it off.
 *
 * One slot per (function, object). 4-player split screen alone has 8 such
 * objects (a marker set and a reticle per view), and a rematch or a new
 * battle in the same boot makes new ones, so the table once filled up for
 * good and later markers juddered at 30 Hz (item 63). A full table now
 * reuses the slot unused longest (one not recorded in the running step:
 * an object gone with its level is never replayed again, since replay takes
 * only slots of the latest step). A miss (every slot in use this step) is
 * counted in the BW_HIFPS_LOG line.
 *
 * The world points go between the steps too (lane aim, 2026-11-03). The
 * replay used to project the newest step's world points with the blended
 * camera, which matches neither step: the reticle's aim point is set at the
 * start of a step from where the boat was (Players_FrameUpdate 0x7AE80 ->
 * Players_UpdateReticle 0x79D20 -> HudReticle_SetAimPoint 0x85A10), the
 * camera at its end, so between steps the aim point sat one whole step ahead
 * of the drawn camera and snapped back at the next step: a 30 Hz sawtooth on
 * the reticle while turning (seen in BW_AIMLOG: 15 HUD units off the path
 * the steps trace at the 95th percentile, the same in 0.2.4 and 0.2.5;
 * docs/remaster/aim.md). Each record now keeps the
 * step before's input bytes too, and the replay blends the world points
 * (the reticle's aim and target points, the markers' positions) between the
 * two steps by the weight its view's camera was drawn at, so each frame
 * projects the in-between point with the in-between camera; at the step
 * itself that is exactly the step's own picture. A point whose direction
 * from the view's camera turned more than about 15 degrees in one step (a
 * new target, a marker slot taken by another boat) is shown at the step; a
 * distance rule would not do, since the aim point sits hundreds of metres
 * ahead and sweeps tens of metres a step in a turn (seen: a 30 m rule
 * snapped up to half of them). BW_HIFPS_HUD_LERP=0 projects the newest
 * step's points as before. */
#define HUD_RECS      16
#define HUD_MAXSZ     0xA6Cu
#define HUD_SNAP_COS  0.966f         /* cos 15 degrees: a point that turned further from the eye is not blended */
#define HUD_SNAP_FAR  5000.0f        /* metres: a point further than this from the eye is not blended */
typedef struct {
    uint32_t fn, obj, arg, size, step;
    uint32_t prev_step;              /* the step `prev` was recorded in; 0 = none */
    uint8_t  pre[HUD_MAXSZ], post[HUD_MAXSZ], replay[HUD_MAXSZ];
    uint8_t  prev[HUD_MAXSZ];        /* the step before's input bytes */
    int      replayed;
} HudRec;
static HudRec s_hud[HUD_RECS];
static int s_nhud, s_hudon = 1, s_hudlerp = 1;
static unsigned long s_st_hud, s_st_hudmoved, s_st_hudtouched, s_st_hudlerp, s_st_hudsnap;
static unsigned long s_hud_reused, s_hud_missed;   /* this boot */
extern void sub_00084AE0_gen(void);
extern void sub_00085850_gen(void);

static HudRec *hud_rec(uint32_t fn, uint32_t obj, uint32_t size)
{
    int i, old = -1;
    uint32_t now = s_snapshots + 1;          /* the step that is running now */
    for (i = 0; i < s_nhud; i++)
        if (s_hud[i].fn == fn && s_hud[i].obj == obj)
            return &s_hud[i];
    if (s_nhud < HUD_RECS)
        i = s_nhud++;
    else {
        for (i = 0; i < HUD_RECS; i++)
            if (s_hud[i].step != now && (old < 0 || s_hud[i].step < s_hud[old].step))
                old = i;
        if (old < 0) {
            s_hud_missed++;
            return NULL;
        }
        i = old;
        s_hud_reused++;
    }
    s_hud[i].fn = fn;
    s_hud[i].obj = obj;
    s_hud[i].size = size;
    s_hud[i].replayed = 0;
    s_hud[i].step = s_hud[i].prev_step = 0;  /* a reused slot keeps nothing of its last object */
    return &s_hud[i];
}

static void hud_record(uint32_t fn, uint32_t size)
{
    HudRec *r;
    uint32_t step = s_snapshots + 1;         /* the step that is running now */
    if (!s_on || !s_hudon || !s_in_update || g_ecx < 0x00010000u)
        return;
    r = hud_rec(fn, g_ecx, size);
    if (!r)
        return;
    if (r->step != step) {                   /* its first call in this step */
        if (r->step && r->step + 1u == step) {
            memcpy(r->prev, r->pre, size);   /* the step before's inputs */
            r->prev_step = r->step;
        } else
            r->prev_step = 0;                /* a gap: nothing to blend from */
    }
    r->arg = HU32(g_esp + 4);
    r->step = step;
    memcpy(r->pre, HP(g_ecx), size);
}

static int hud_finite(float f)
{
    return f == f && f < 1e30f && f > -1e30f;
}

/* Blend one world point (3 floats) of the record's inputs into the object:
 * a from the step before (seen from eye ea), b from the step (eye eb). */
static void hud_lerp3(uint32_t va, const uint8_t *a, const uint8_t *b, const float *ea,
                      const float *eb, float u)
{
    float p[3], q[3], da[3], db[3], la = 0.0f, lb = 0.0f, dot = 0.0f;
    int j;
    memcpy(p, a, sizeof p);
    memcpy(q, b, sizeof q);
    for (j = 0; j < 3; j++) {
        if (!hud_finite(p[j]) || !hud_finite(q[j]))
            return;
        da[j] = p[j] - ea[j];
        db[j] = q[j] - eb[j];
        la += da[j] * da[j];
        lb += db[j] * db[j];
        dot += da[j] * db[j];
    }
    if (p[0] == q[0] && p[1] == q[1] && p[2] == q[2])
        return;                              /* didn't move: the step's value is it */
    if (la < 1e-4f || lb < 1e-4f || la > HUD_SNAP_FAR * HUD_SNAP_FAR || lb > HUD_SNAP_FAR * HUD_SNAP_FAR
        || dot < HUD_SNAP_COS * sqrtf(la) * sqrtf(lb)) {
        s_st_hudsnap++;
        return;                              /* shown at the step */
    }
    for (j = 0; j < 3; j++)
        HF32(va + 4u * (uint32_t)j) = p[j] + (q[j] - p[j]) * u;
    s_st_hudlerp++;
}

/* With the step's input bytes in the object: blend its world points from
 * the step before's. The reticle's aim point +0x1C0 and target point +0x1CC;
 * the markers' positions +0x9E4 + 12*i, i below both steps' counts (+0x20,
 * at most 8). The view is the object's +0x1C, the one both functions pass to
 * Camera_WorldToScreen. */
static void hud_world_blend(const HudRec *r)
{
    uint32_t view, i;
    int32_t n, np;
    float u;
    const float *ea, *eb;
    if (!s_hudlerp || !r->prev_step || r->prev_step + 1u != r->step)
        return;
    memcpy(&view, r->pre + 0x1C, 4);
    if (view >= MAX_VIEWS)
        return;
    u = s_hud_u[view];
    if (u == 1.0f || !hud_finite(u))
        return;                              /* the camera shows the step */
    ea = s_cam_prev[view].eye;               /* the two steps' eyes (apply_interp blended them) */
    eb = s_cam_cur[view].eye;
    if (r->fn == 0x00085850u) {
        uint32_t a, b;
        /* Only points the game set in both steps: the aim point when
         * HudReticle_SetAimPoint 0x85A10 showed the sprite (+0x58 alpha
         * 0xFE; HudReticle_Hide leaves 0), the target point when
         * HudReticle_SetTarget 0x85A70 linked the box (+0x20). Otherwise the
         * bytes are last step's projection, not a world point. */
        memcpy(&a, r->prev + 0x58, 4);
        memcpy(&b, r->pre + 0x58, 4);
        if ((a >> 24) && (b >> 24))
            hud_lerp3(r->obj + 0x1C0u, r->prev + 0x1C0, r->pre + 0x1C0, ea, eb, u);
        memcpy(&a, r->prev + 0x20, 4);
        memcpy(&b, r->pre + 0x20, 4);
        if (a && b)
            hud_lerp3(r->obj + 0x1CCu, r->prev + 0x1CC, r->pre + 0x1CC, ea, eb, u);
        return;
    }
    memcpy(&n, r->pre + 0x20, 4);
    memcpy(&np, r->prev + 0x20, 4);
    if (np < n)
        n = np;
    if (n > 8)
        n = 8;
    for (i = 0; n > 0 && i < (uint32_t)n; i++)
        hud_lerp3(r->obj + 0x9E4u + 12u * i, r->prev + 0x9E4 + 12u * i, r->pre + 0x9E4 + 12u * i,
                  ea, eb, u);
}

static void hud_call(const HudRec *r)
{
    uint32_t ea = g_eax, ec = g_ecx, ed = g_edx, bp = g_ebp, sbp = g_seh_ebp;
    g_esp -= 4;
    HU32(g_esp) = r->arg;
    g_esp -= 4;
    HU32(g_esp) = 0x00028600u;               /* a return address for stack scans */
    g_ecx = r->obj;
    if (r->fn == 0x00084AE0u)
        sub_00084AE0_gen();
    else
        sub_00085850_gen();                  /* both end in `ret 4` */
    g_eax = ea; g_ecx = ec; g_edx = ed; g_ebp = bp; g_seh_ebp = sbp;
}

static void hud_replay(void)
{
    int i;
    for (i = 0; i < s_nhud; i++) {
        HudRec *r = &s_hud[i];
        r->replayed = 0;
        if (!s_hudon || r->step != s_snapshots)  /* not updated in the latest step */
            continue;
        memcpy(r->post, HP(r->obj), r->size);
        memcpy(HP(r->obj), r->pre, r->size);
        hud_world_blend(r);
        hud_call(r);
        memcpy(r->replay, HP(r->obj), r->size);
        r->replayed = 1;
        s_st_hud++;
    }
}

static void hud_restore(void)
{
    int i;
    uint32_t k;
    for (i = 0; i < s_nhud; i++) {
        HudRec *r = &s_hud[i];
        uint8_t *now;
        if (!r->replayed)
            continue;
        now = (uint8_t *)HP(r->obj);
        for (k = 0; k < r->size; k++) {     /* what the frame itself changed stays */
            s_st_hudmoved += r->replay[k] != r->post[k];
            if (now[k] == r->replay[k])
                now[k] = r->post[k];
            else
                s_st_hudtouched++;
        }
        r->replayed = 0;
    }
}

/* HudMarkers_Update 0x00084AE0 and HudReticle_Update 0x00085850: record, then
 * the game's own body. */
void sub_00084AE0(void)
{
    uint32_t obj = g_ecx;
    hud_record(0x00084AE0u, 0xA6Cu);
    sub_00084AE0_gen();
    if (s_aimlog && s_in_update && obj >= 0x00010000u)     /* BW_AIMLOG: marker 0 as the step drew it */
        fprintf(s_aimlog, "M %u %.3f %08X %d %.3f %.3f\n", s_snapshots + 1, aim_ms(), obj,
                (int)HU32(obj + 0x20), HF32(obj + 0x9E4), HF32(obj + 0x9E8));
}

/* ---- BW_AIMLOG=<file>: the reticle per drawn frame (test aid, lane aim) --
 *
 * A line per drawn frame of the high frame rate (message 8, after the HUD
 * replay, right before the game draws: what the frame will show) and a line
 * per game step's HudReticle_Update (after the game's body: the step's own
 * values). Each gives the host time and, per reticle object, the aim sprite's
 * screen position (+0x24/+0x28, as HudReticle_Update writes it) and colour
 * word (+0x58: alpha 0 = hidden), the target link (+0x20: 0 = no target box)
 * and the target box's position (+0x8C/+0x90). Frame lines add the step
 * count, the steps since the last frame, the blend factor and render camera
 * 1's forward axis and translation. Read-only; off unless set. */
static double aim_ms(void)
{
    static double f;
    LARGE_INTEGER t;
    if (!f) {
        LARGE_INTEGER q;
        QueryPerformanceFrequency(&q);
        f = (double)q.QuadPart / 1000.0;
    }
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / f;
}

static void aim_obj(uint32_t obj)
{
    fprintf(s_aimlog, " %08X %.3f %.3f %08X %08X %.3f %.3f", obj, HF32(obj + 0x24), HF32(obj + 0x28),
            HU32(obj + 0x58), HU32(obj + 0x20), HF32(obj + 0x8C), HF32(obj + 0x90));
}

static void aim_frame(int interp, float a)
{
    static unsigned long n;
    const float *v = (const float *)HP(RCAM_VA(1));
    int i;
    fprintf(s_aimlog, "F %lu %.3f %u %u %d %.4f", ++n, aim_ms(), s_snapshots, s_steps_since_draw,
            interp, a);
    for (i = 0; i < s_nhud; i++) {
        const HudRec *r = &s_hud[i];
        if (r->fn != 0x00085850u || r->step + 2u < s_snapshots)
            continue;
        fprintf(s_aimlog, " [%u %d", r->step, r->replayed);
        aim_obj(r->obj);
        fputc(']', s_aimlog);
    }
    for (i = 0; i < s_nhud; i++) {                 /* the markers: count, marker 0 */
        const HudRec *r = &s_hud[i];
        if (r->fn != 0x00084AE0u || r->step + 2u < s_snapshots)
            continue;
        fprintf(s_aimlog, " {%u %d %08X %d %.3f %.3f}", r->step, r->replayed, r->obj,
                (int)HU32(r->obj + 0x20), HF32(r->obj + 0x9E4), HF32(r->obj + 0x9E8));
    }
    fprintf(s_aimlog, " cam %.5f %.5f %.5f %.3f %.3f %.3f\n", v[2], v[6], v[10], v[12], v[13], v[14]);
    if (!(n & 63u))
        fflush(s_aimlog);
}

void sub_00085850(void)
{
    uint32_t obj = g_ecx;
    hud_record(0x00085850u, 0x1E0u);
    sub_00085850_gen();
    if (s_aimlog && s_in_update && obj >= 0x00010000u) {
        fprintf(s_aimlog, "S %u %.3f", s_snapshots + 1, aim_ms());
        aim_obj(obj);
        fputc('\n', s_aimlog);
    }
}

/* ---- BW_HIFPS_AUDIT=1: what moves on frames without a game step ---------
 *
 * A test aid for the frame-rate-trap audit (R4 step 4). On a drawn frame with
 * no new game step, the game's .data/.bss (0x170000-0x1F0000) is compared
 * before the frame and after it (after every value this file blends is put
 * back), so what remains changed was advanced by drawing alone. A value that
 * is time-scaled moves in proportion to the frame time; a per-frame
 * accumulator moves by the same amount whatever the frame time, so at 120 fps
 * it would run four times as fast as at 30. For each changed float the size
 * of the change is fitted against the frame time (least squares, d = a + b*dt):
 * "per-frame share" a / mean(d) is about 0 when time-scaled and about 1 when
 * the value moves a fixed amount per drawn frame. Every 15 s the busiest
 * addresses are printed, and counters that go up by one per frame. Read-only;
 * off unless asked for. */
#define AUD_LO 0x00170000u
#define AUD_HI 0x001F0000u
#define AUD_N  ((AUD_HI - AUD_LO) / 4u)
typedef struct { uint32_t n, ninc; double sd, sdt, sdt2, sddt; } AudStat;
static int s_audit = -1;
static uint32_t *s_aud_pre;
static AudStat *s_aud;
static uint32_t s_aud_frames;
static double s_aud_dt_sum;
static ULONGLONG s_aud_last;

static int aud_float_ok(uint32_t u)
{
    uint32_t e = (u >> 23) & 0xFFu;
    float f;
    if (e == 0 || e == 0xFFu)
        return 0;
    memcpy(&f, &u, 4);
    f = f < 0 ? -f : f;
    return f > 1e-6f && f < 1e7f;
}

static void audit_pre(void)
{
    if (s_audit < 0) {
        s_audit = env_u("BW_HIFPS_AUDIT", 0) == 1;
        if (s_audit) {
            s_aud_pre = (uint32_t *)malloc(AUD_N * 4u);
            s_aud = (AudStat *)calloc(AUD_N, sizeof(AudStat));
            if (!s_aud_pre || !s_aud)
                s_audit = 0;
        }
    }
    if (s_audit)
        memcpy(s_aud_pre, HP(AUD_LO), AUD_N * 4u);
}

static void audit_post(uint32_t dt)
{
    const uint32_t *now = (const uint32_t *)HP(AUD_LO);
    uint32_t i;
    double x = (double)dt;
    s_aud_frames++;
    s_aud_dt_sum += x;
    for (i = 0; i < AUD_N; i++) {
        uint32_t a = s_aud_pre[i], b = now[i];
        if (a == b)
            continue;
        if (b == a + 1u) {
            s_aud[i].ninc++;
            continue;
        }
        if (aud_float_ok(a) && aud_float_ok(b)) {
            float fa, fb;
            double d;
            memcpy(&fa, &a, 4);
            memcpy(&fb, &b, 4);
            d = (double)fb - (double)fa;
            d = d < 0 ? -d : d;
            s_aud[i].n++;
            s_aud[i].sd += d;
            s_aud[i].sdt += x;
            s_aud[i].sdt2 += x * x;
            s_aud[i].sddt += d * x;
        }
    }
    {
        ULONGLONG t = GetTickCount64();
        if (!s_aud_last)
            s_aud_last = t;
        if (t - s_aud_last >= 15000 && s_aud_frames >= 20) {
            /* the whole table to hifps_audit.txt in the working folder (the
             * run folder with bw_run --own-cwd), rewritten each time */
            FILE *f = fopen("hifps_audit.txt", "w");
            if (f) {
                fprintf(f, "# %u frames without a step, mean dt %.2f ms\n# addr kind n mean share\n",
                        s_aud_frames, s_aud_dt_sum / s_aud_frames);
                for (i = 0; i < AUD_N; i++) {
                    const AudStat *st = &s_aud[i];
                    if (st->n >= s_aud_frames / 4u && st->n >= 10u) {
                        double n = st->n, md = st->sd / n, mx = st->sdt / n;
                        double var = st->sdt2 / n - mx * mx, cov = st->sddt / n - md * mx;
                        double slope = var > 1e-9 ? cov / var : 0.0, icpt = md - slope * mx;
                        fprintf(f, "%08X F %u %.6g %.3f\n", AUD_LO + 4u * i, st->n, md,
                                md > 0 ? icpt / md : 0.0);
                    }
                    if (st->ninc >= s_aud_frames / 4u && st->ninc >= 10u)
                        fprintf(f, "%08X I %u 1 1\n", AUD_LO + 4u * i, st->ninc);
                }
                fclose(f);
            }
            s_aud_last = t;
        }
    }
}

/* Weather_RenderRain 0x0002E530 (thiscall, this = the sky 0x1A9F88, [esp+4] =
 * paused; ret 4). Two frame-rate traps in its code (seen, D7 audit; the
 * storm array 0x1A57A8.. moved by a fixed amount per drawn frame in
 * BW_HIFPS_AUDIT):
 *   - each drop falls speed * [0x175374] per call, with no frame time
 *     (0x2EB00), so at 150 fps the rain would fall five times as fast;
 *   - each call spawns rand()/32767 * [0x1A9A88] (the rain intensity,
 *     Weather_Update) * [0x175780] new drops (0x2E8AD), so the rain would be
 *     five times as dense.
 * For the duration of the call only, [0x175374] is scaled by the frame time
 * over 1/30 s (Graphics+0x18, the frame's dt in seconds, 0x1A9A80), and the
 * intensity is set to 0 on a drawn frame with no new game step, so drops are
 * spawned once per step as at 30 fps. Both are put back after the call
 * ([0x175374] is shared with PFX_SetupCamera and others, which never run
 * inside this one). Splashes already age by the frame time. BW_HIFPS_RAIN=0
 * turns it off. */
#define RAIN_FALL_K      0x00175374u
#define RAIN_INTENSITY   0x001A9A88u
#define GFX_FRAME_DT_S   0x001A9A80u
static int s_rain = 1;
/* BW_RAINLOG=1 (every edition, read-only test aid): once a second, the rain
 * as a viewer would measure it: drops spawned per second (list members not
 * seen on the frame before; the live list is at this+0x11948, linked through
 * +0, height at +0xC), the mean fall speed (height change of drops seen on
 * both frames, over the real time between the frames) and the mean number of
 * live drops, so Original at 30 fps and the high frame rate can be compared. */
#define RAIN_MAXD 4096
static uint32_t s_rd_addr[2][RAIN_MAXD];
static float s_rd_y[2][RAIN_MAXD];
static uint32_t s_rd_n[2];
static int s_rd_cur;

static int rd_cmp(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static void rain_count(uint32_t sky)
{
    static int on = -1;
    static double live_sum, fall_sum, fall_n, t_prev;
    static unsigned long frames, spawned;
    static ULONGLONG last;
    static LARGE_INTEGER f0;
    LARGE_INTEGER now;
    double t, dt;
    uint32_t n = 0, p, i;
    int c, pv;
    if (on < 0) {
        on = env_u("BW_RAINLOG", 0) == 1;
        QueryPerformanceFrequency(&f0);
    }
    if (!on)
        return;
    QueryPerformanceCounter(&now);
    t = (double)now.QuadPart / (double)f0.QuadPart;
    dt = t_prev > 0 ? t - t_prev : 0;
    t_prev = t;
    c = s_rd_cur ^= 1;
    pv = c ^ 1;
    for (p = HU32(sky + 0x11948u); p >= 0x00010000u && p < 0x08000000u && n < RAIN_MAXD; p = HU32(p))
        s_rd_addr[c][n++] = p;
    qsort(s_rd_addr[c], n, 4, rd_cmp);
    for (i = 0; i < n; i++) {
        uint32_t *hit;
        s_rd_y[c][i] = HF32(s_rd_addr[c][i] + 0xC);
        hit = (uint32_t *)bsearch(&s_rd_addr[c][i], s_rd_addr[pv], s_rd_n[pv], 4, rd_cmp);
        if (!hit) {
            spawned++;
        } else if (dt > 0 && dt < 0.2) {
            float d = s_rd_y[c][i] - s_rd_y[pv][hit - s_rd_addr[pv]];
            fall_sum += (d < 0 ? -d : d) / dt;
            fall_n++;
        }
    }
    s_rd_n[c] = n;
    live_sum += n;
    frames++;
    {
        ULONGLONG tk = GetTickCount64();
        if (!last)
            last = tk;
        if (tk - last >= 1000) {
            fprintf(stderr, "[BW] rain: %lu drops spawned/s, fall %.2f units/s, %.1f live, %lu frames\n",
                    spawned, fall_n ? fall_sum / fall_n : 0.0, frames ? live_sum / frames : 0.0, frames);
            live_sum = fall_sum = fall_n = 0;
            frames = spawned = 0;
            last = tk;
        }
    }
}

extern void sub_0002E530_gen(void);
void sub_0002E530(void)
{
    uint32_t k_bits, i_bits, sky = g_ecx;
    float dt;
    if (!s_on || !s_rain || HU8(g_esp + 4) != 0) {
        eye_swap(1);                        /* HIFPS_EYE: the rain keeps the step's eye */
        sub_0002E530_gen();
        eye_swap(0);
        rain_count(sky);
        return;
    }
    k_bits = HU32(RAIN_FALL_K);
    i_bits = HU32(RAIN_INTENSITY);
    dt = HF32(GFX_FRAME_DT_S);
    if (dt > 0.0f && dt < 0.2f)
        HF32(RAIN_FALL_K) = HF32(RAIN_FALL_K) * dt * 30.0f;
    if (s_steps_since_draw == 0)
        HF32(RAIN_INTENSITY) = 0.0f;
    eye_swap(1);                            /* HIFPS_EYE: the rain keeps the step's eye */
    sub_0002E530_gen();
    eye_swap(0);
    HU32(RAIN_FALL_K) = k_bits;
    HU32(RAIN_INTENSITY) = i_bits;
    rain_count(sky);
}

/* ---- just-in-time frames (lat-q, BW_HIFPS_JIT) -------------------------
 *
 * The game draws a frame per pass, and with KickOff segments in flight
 * (BW_HIFPS_ASYNC) it can encode a frame or more ahead of the NV2A walk,
 * which waits for the flip hold's tick (the frame cap), the present pacing,
 * the screen and the GPU limit (BW_GPU_LATENCY). A frame that waits in that
 * queue shows steps, a pad read and an interpolation clock from when it was
 * encoded. Here the pass waits first, at the start of Game_Update, before
 * the game's own code runs, so its steps, its pad read (message 0xA, right
 * after Game_Update) and its drawing all happen as late as they can:
 *   1  until the walk has caught up with every segment handed over (the
 *      previous frame's present, screen wait and GPU wait are all inside
 *      that walk), then until the pending flip hold's tick, less the lead;
 *   2  "only when the queue is full": until the hold's tick less the lead,
 *      and for the walk only while the NV2A thread is in a present-side wait
 *      (pacing, screen, GPU limit) or the last finished flip waited in one,
 *      so the game keeps overlapping the walk when nothing downstream is
 *      holding frames.
 * Nothing in the game changes: its accumulator reads the clock after the
 * wait, as it would after any slower frame; this is the KickOff's existing
 * back-pressure moved from the middle of a frame to its start. Posted
 * interrupts get a safe point each turn, as in the KickOff's wait. Guards
 * (counted, logged with BW_HIFPS_LOG): 250 ms with the NV2A thread idle, or
 * 3 s in all; a wait through a busy walk or GPU (a slow GPU's own frame
 * time, which the as-shipped build spends in its KickOffs) is not cut short,
 * only counted ("long" when over 100 ms). */
extern int bw_flip_hold_due(uint64_t *due_ns);                  /* carried nv2a_pb_exec.c */
extern uint64_t xbox_HostNowNs(void);
extern void xbox_IrqSafePoint(void);
extern void bw_flush_wait(void);                                /* bw_wait.c */
extern volatile LONG g_bw_present_waiting, g_bw_last_flip_waited;   /* gpu/core/bwg_pace.c (lat-q) */
extern volatile LONG g_bw_cap_reached;
extern volatile double g_bw_flip_ready_ms;
extern volatile long g_bw_nv2a_walking;                          /* carried xbox_memory_layout.c */
extern volatile uint32_t g_bw_ack_put_reg, g_bw_ack_last_put, g_bw_ack_get_written;
extern volatile long g_bw_ack_flip_held;
extern int bw_d3d11_present_due(double *due_ms);
extern double bw_d3d11_pace_period(void);
extern volatile LONG g_bw_pace_waiting;
extern volatile double g_bwg_beat_need_ms;                       /* gpu/core/bwg_pace.c (gfx-lat11) */
static unsigned long s_st_jit_n, s_st_jit_guard, s_st_jit_long, s_st_jit_idle;
static double s_st_jit_ms;
/* The present pacing (BW_HIFPS_PACE) holds each present at least 1/target
 * after the last one. Measured in the quiet window (docs/latency/q.md): a
 * pass started at the hold's tick then waited most of a frame for its
 * present slot (frame age one whole period at a 120 cap), because the
 * slot follows the last present, not the tick. So the wait also aims at
 * that slot: the pass starts the work it takes before it (the 90th
 * percentile of the last 32 passes' start-to-flip times, plus 0.25 ms;
 * BW_HIFPS_JIT_LEAD fixes it instead). BW_HIFPS_JIT_PACE=0: the tick only. */
static double s_jit_prev_ms;            /* the last pass's start, qpc ms */
/* start-to-flip times of the last 32 passes, kept apart for passes that ran
 * a game step (seen: ~8-9 ms at a 120 cap) and passes that didn't (~4 ms),
 * since one pass in four steps at 120 fps and a single estimate would be the
 * stepping one */
static float s_jit_w[2][32];
static unsigned s_jit_wn[2];
static int s_jit_frame_stepped;          /* the last drawn frame ran a step */
/* BW_HIFPS_JIT=4 (the head start, a test switch): each pass's own work,
 * kept apart from its waits for the NV2A thread (those grow with an earlier
 * start, a feedback): pass start -> frame end less its KickOff waits, by
 * kind, and frame end -> its flip at the back end (measured when the walk
 * had caught up by the next pass) */
static float s_jit_cpu[2][32], s_jit_tail[32];
static unsigned s_jit_cpun[2], s_jit_tailn;
static double s_jit_fe_ms;               /* the last frame's end, qpc ms */
/* gfx-lat11: the measured horizon's game side, from the frame's draw (its
 * blend's clock) to its flip at the back end, the last 32 (latency mode) */
static double s_draw_ms;
static float s_draw_flip[32];
static unsigned s_draw_flipn;
static unsigned long long s_jit_kw0;     /* g_bw_ko_wait_us at the pass start */
static unsigned long s_jit_drawn;        /* frames drawn (message 8 ends) */
static long s_jit_flip_base;             /* g_bw_flips_done less s_jit_drawn, set when caught up */
static long s_jit_paced_base;            /* g_bw_paced_n less s_jit_drawn, the same */
extern volatile LONG g_bw_paced_n;                              /* bw_d3d11.c */
extern unsigned long long g_bw_ko_wait_us;                      /* recomp_manual.c */
extern volatile LONG g_bw_flips_done;                           /* gpu/core/bwg_pace.c */

static void jit_sort(float *v, unsigned n)
{
    unsigned i, j;
    for (i = 1; i < n; i++)                 /* at most 32 values: insertion sort */
        for (j = i; j > 0 && v[j - 1] > v[j]; j--) {
            float x = v[j]; v[j] = v[j - 1]; v[j - 1] = x;
        }
}

static double jit_qpc_ms(void)
{
    static double inv;
    LARGE_INTEGER t;
    if (!inv) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        inv = 1000.0 / (double)f.QuadPart;
    }
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * inv;
}

/* gfx-lat11: BW_HIFPS_PREDICT_AHEAD=auto (off by default; prediction itself
 * is off by default): the prediction reaches 0.8 of the measured horizon
 * past "now", the frame's draw -> its flip (here, the median of the last 32)
 * plus the core's present -> screen (on screen) or -> GPU done (median of
 * the last 32). 0 until both are known; at most a step. */
static void pred_ahead_follow(void)
{
    float v[32];
    unsigned n = s_draw_flipn < 32u ? s_draw_flipn : 32u;
    double h, a = 0.0;
    if (!s_pred_ahead_auto)
        return;
    h = bwg_gate_horizon_ms();
    if (n >= 8 && h > 0.0) {
        memcpy(v, s_draw_flip, n * sizeof v[0]);
        jit_sort(v, n);
        a = 0.8 * ((double)v[n / 2] + h);
    }
    if (a > 1000.0 / 30.0)
        a = 1000.0 / 30.0;
    s_pred_ahead = (float)a;
    s_pred = s_pred_base + s_pred_ahead * (30.0f / 1000.0f);
}

/* How long a pass takes to get its frame to the back end's flip (stepped:
 * one that runs a game step); the other kind's when one hasn't been seen. */
static double jit_work_ms(int stepped)
{
    float v[32];
    unsigned n, i, j;
    if (s_jit_lead_ns)
        return (double)s_jit_lead_ns / 1e6;
    if (s_jit == 4) {
        /* the head start: the kind's own work (90th percentile) and the
         * walk's tail after it (the median; 1 ms until measured) */
        int k = s_jit_cpun[stepped] ? stepped : !stepped;
        double c = 0.0, tl = 1.0;
        n = s_jit_cpun[k] < 32u ? s_jit_cpun[k] : 32u;
        if (n) {
            memcpy(v, s_jit_cpu[k], n * sizeof v[0]);
            jit_sort(v, n);
            c = (double)v[(n * 9u) / 10u];
        }
        n = s_jit_tailn < 32u ? s_jit_tailn : 32u;
        if (n) {
            memcpy(v, s_jit_tail, n * sizeof v[0]);
            jit_sort(v, n);
            tl = (double)v[n / 2];
        }
        if (c > 0.0)
            return c + tl + 0.25;
    }
    if (!s_jit_wn[stepped])
        stepped = !stepped;
    n = s_jit_wn[stepped] < 32u ? s_jit_wn[stepped] : 32u;
    if (!n)
        return 0.0;
    memcpy(v, s_jit_w[stepped], n * sizeof v[0]);
    for (i = 1; i < n; i++)                 /* 32 values: insertion sort */
        for (j = i; j > 0 && v[j - 1] > v[j]; j--) {
            float x = v[j]; v[j] = v[j - 1]; v[j - 1] = x;
        }
    return (double)v[(n * 9u) / 10u] + 0.25;
}

/* Will a pass whose Game_Update starts at host time t_ms run a game step?
 * The game's own test (Game_Update: while clock + freq/30 < now), with its
 * clock advanced by the host time since the last Game_Update (as
 * step_alpha does). Read-only. */
static int jit_step_due(double t_ms)
{
    uint64_t freq = HU64(SYS_QPC_FREQ_VA), now = HU64(GAME_VA + GAME_NOW_QPC);
    uint64_t clk = HU64(GAME_VA + GAME_STEP_QPC);
    LARGE_INTEGER hf;
    double gnow;
    if (!freq || !s_host_at_update.QuadPart)
        return 0;
    QueryPerformanceFrequency(&hf);
    gnow = (double)now + (t_ms - (double)s_host_at_update.QuadPart * 1000.0 / (double)hf.QuadPart)
                         * (double)freq / 1000.0;
    return (double)clk + (double)freq / 30.0 < gnow;
}

static void jit_sleep_ns(uint64_t ns)
{
    static HANDLE timer;
    if (ns > 300000u) {
        /* a high-resolution timer to ~0.25 ms before, at most 1 ms a turn */
        LARGE_INTEGER d;
        uint64_t chunk = ns - 250000u;
        if (chunk > 1000000u)
            chunk = 1000000u;
        if (!timer)
            timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* HIGH_RESOLUTION */,
                                           TIMER_ALL_ACCESS);
        d.QuadPart = -(LONGLONG)(chunk / 100u);
        if (timer && SetWaitableTimer(timer, &d, 0, NULL, NULL, FALSE)) {
            WaitForSingleObject(timer, 5);
            return;
        }
        Sleep(0);
        return;
    }
    YieldProcessor();
}

static void jit_wait(void)
{
    LARGE_INTEGER t0, t, f, tprog, tlast;
    unsigned what = 0, guard = 0;
    uint32_t get_seen = 0xFFFFFFFFu;
    double ms, gap_max = 0.0, gw_ms = 0.0;
    long inflight = 0, pending = 0;
    int gw = 0;                         /* gfx-lat11: the last look waited at the gate for the GPU */
    if (s_jit == 3 && !g_bw_cap_reached) {
        /* BW_HIFPS_JIT=3 with the cap not reached: as shipped. The next
         * engaged pass learns nothing from this one (its frame queued). */
        s_jit_prev_ms = 0.0;
        return;
    }
    QueryPerformanceCounter(&t0);
    QueryPerformanceFrequency(&f);
    tprog = t0;
    tlast = t0;
    for (;;) {
        uint32_t user = g_bw_ko_user, put = g_bw_ko_last_put;
        uint64_t due, now;
        int behind, held;
        QueryPerformanceCounter(&t);
        {
            /* the longest gap between two looks (this thread not running) */
            double gp = (double)(t.QuadPart - tlast.QuadPart) * 1000.0 / (double)f.QuadPart;
            if (gp > gap_max)
                gap_max = gp;
            tlast = t;
        }
        /* The guards. A wait goes on while the NV2A thread is busy for it: in
         * a segment walk (its flip, present and GPU wait are inside one) or
         * moving GET. Seen on a shared GPU at 3.7 fps: a single 250 ms guard
         * ended waits that were the GPU's own 250+ ms frames, which the
         * as-shipped build shows just as often, as KickOff waits. So:
         *  - 250 ms with the NV2A thread idle (nothing walked, GET still):
         *    the wait's own fault, counted as a guard hit;
         *  - 3 s in all (the GPU limit's own no-progress rule), counted too. */
        if (user) {
            uint32_t g = HU32(user + 0x44);
            /* gfx-lat11: a wait at the gate for the GPU is the GPU's own
             * frame time, as the limit's wait in the flip is in parity mode */
            if (g != get_seen || g_bw_nv2a_walking || g_bw_present_waiting || gw) {
                get_seen = g;
                tprog = t;
            }
        }
        if ((double)(t.QuadPart - tprog.QuadPart) * 1000.0 / (double)f.QuadPart > 250.0
                || (double)(t.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart > 3000.0) {
            static unsigned noted;
            guard = 1;
            if (++noted <= 10)                  /* what it saw, for the next look (the first ten) */
                fprintf(stderr, "[BW] hifps: just-in-time guard after %.0f ms (%.0f since the NV2A thread"
                                " last moved): GET %08X PUT %08X, walking %ld, present wait %ld, hold %d,"
                                " longest gap between looks %.1f ms, waited for %u\n",
                        (double)(t.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart,
                        (double)(t.QuadPart - tprog.QuadPart) * 1000.0 / (double)f.QuadPart,
                        user ? HU32(user + 0x44) : 0u, user ? HU32(user + 0x40) : 0u,
                        (long)g_bw_nv2a_walking, (long)g_bw_present_waiting, bw_flip_hold_due(&due),
                        gap_max, what);
            break;
        }
        gw = 0;
        held = bw_flip_hold_due(&due);
        if (held) {
            now = xbox_HostNowNs();
            if (due > now + s_jit_lead_ns) {
                what |= 2;
                xbox_IrqSafePoint();
                jit_sleep_ns(due - now - s_jit_lead_ns);
                continue;
            }
            /* within the lead of the tick: the walk may go on, and the
             * frame before has flipped (the hold is armed at its flip), so
             * only the pacing's slot is left to look at */
        }
        /* user is the NV2A's USER DMA block (0xFD800000 in runs), as the
         * KickOff reads it; 0 until the first KickOff. The PUT register
         * (+0x40) counts, not the last PUT the async KickOff saw (put): that
         * can be older (seen: a 4 fps stretch of 250 ms guard waits) */
        (void)put;
        /* Behind = segments handed over that the walk hasn't finished: a walk
         * in progress, or the PUT register past the last PUT the walk took.
         * Not GET: XDK D3D writes GET itself when it restarts the ring
         * (nv2a_pb_scan.c), and a GET the title moved with nothing new to walk
         * left the wait looking at a walk with nothing to do (seen: "the walk
         * idle while behind" in missions 3 and 5). */
        behind = user && (g_bw_nv2a_walking
                          || (HU32(user + 0x40) & 0x03FFFFFFu) != (g_bw_ack_last_put & 0x03FFFFFFu));
        if (user && !behind && (HU32(user + 0x44) & 0x03FFFFFFu) != (HU32(user + 0x40) & 0x03FFFFFFu)) {
            /* the old test's "behind" (GET short of PUT) with the walk done:
             * what GET, PUT and the ack loop's own copies say (the first ten,
             * BW_HIFPS_LOG): which of them disagrees is the cause */
            static unsigned noted;
            if (s_log && ++noted <= 10)
                fprintf(stderr, "[BW] hifps: just-in-time: GET %08X short of PUT %08X with the walk done (last"
                                " walked %08X, GET written %08X, PUT seen %08X, flip held %ld)\n",
                        HU32(user + 0x44), HU32(user + 0x40), g_bw_ack_last_put, g_bw_ack_get_written,
                        g_bw_ack_put_reg, (long)g_bw_ack_flip_held);
        }
        if (behind && (double)(t.QuadPart - tprog.QuadPart) * 1000.0 / (double)f.QuadPart > 2.0) {
            /* Behind, but the NV2A thread hasn't walked, moved GET or waited on
             * the present side for 2 ms: it isn't working on what this pass
             * waits for (seen in mission 5: GET short of PUT with the walk idle
             * for 250 ms at a time, cause unknown), so don't wait for it. Counted;
             * the first ten logged with what was seen. */
            static unsigned noted;
            if (!(what & 16u)) {
                s_st_jit_idle++;
                what |= 16u;
            }
            if (++noted <= 10)
                fprintf(stderr, "[BW] hifps: just-in-time: the walk idle for 2 ms while behind (GET %08X PUT"
                                " %08X, walking %ld, present wait %ld, hold %d; the ack loop saw PUT %08X, last"
                                " walked %08X, GET written %08X, flip held %ld): not waited for\n",
                        HU32(user + 0x44), HU32(user + 0x40), (long)g_bw_nv2a_walking,
                        (long)g_bw_present_waiting, held, g_bw_ack_put_reg, g_bw_ack_last_put,
                        g_bw_ack_get_written, (long)g_bw_ack_flip_held);
            behind = 0;
        }
        if (s_jit == 4) {
            /* the head start: frames drawn and not yet through the NV2A
             * thread (its flip done); caught up = none (resynced then, so a
             * frame drawn without a flip doesn't count forever) */
            if (!behind) {
                s_jit_flip_base = (long)g_bw_flips_done - (long)s_jit_drawn;
                s_jit_paced_base = (long)g_bw_paced_n - (long)s_jit_drawn;
            }
            inflight = behind ? (long)s_jit_drawn - ((long)g_bw_flips_done - s_jit_flip_base) : 0;
            pending = behind ? (long)s_jit_drawn - ((long)g_bw_paced_n - s_jit_paced_base) : 0;
            if (pending < 0)
                pending = 0;
        }
        if (!held && behind && s_jit_beat && g_bw_present_waiting) {
            /* BW_HIFPS_PACE_ADAPT=1 (test): the walk has reached the last
             * frame's flip and only waits on the present side (its pacing
             * slot, the screen, the GPU limit); a pass whose own work would
             * miss the next beat starts now, overlapping that wait, so it
             * still lands on the beat (a pass with a game step takes ~2x) */
            double slot, nowm = jit_qpc_ms(), aim;
            if (bw_d3d11_present_due(&slot)) {
                if (g_bw_pace_waiting)
                    slot += bw_d3d11_pace_period();     /* that present is still to come */
                aim = slot - jit_work_ms(0);
                if (jit_step_due(aim))
                    aim = slot - jit_work_ms(1);
                if (aim <= nowm) {
                    what |= 32u;
                    break;
                }
            }
        }
        if (!held && behind && (s_jit == 4 ? inflight >= 2
                                : (s_jit != 2 || g_bw_present_waiting || g_bw_last_flip_waited))) {
            what |= (g_bw_present_waiting || g_bw_last_flip_waited) ? 4u : 1u;
            xbox_IrqSafePoint();
            bw_flush_wait();            /* GET's event, or ~200 us */
            continue;
        }
        if (s_latmode && !held) {
            /* gfx-lat11: the gate's GPU limit (in parity mode the flip waits
             * for it, holding the walk): at most the limit's flips unfinished
             * on the GPU; with the head start, a frame still on the NV2A
             * thread counts too, and the limit is one more */
            uint32_t unf = bwg_gate_unfinished() + (uint32_t)(inflight > 0 ? inflight : 0);
            if (unf > (uint32_t)bwg_gate_limit() + (s_jit == 4 ? 1u : 0u)) {
                double w0 = jit_qpc_ms();
                what |= 64u;
                gw = 1;
                xbox_IrqSafePoint();
                bwg_gate_wait_progress(1);
                gw_ms += jit_qpc_ms() - w0;
                continue;
            }
        }
        if (s_jit_pace && (held || !behind || s_jit == 4)) {
            /* the present pacing's slot, less the pass's own work (above) */
            double slot, nowm = jit_qpc_ms(), aim = 0.0;
            int have = bw_d3d11_present_due(&slot), by_gpu = 0;
            if (have && pending > 0)
                slot += (double)pending * bw_d3d11_pace_period();   /* the head start: after theirs */
            if (s_latmode == 1 && bwg_gate_gpu_aim()) {
                /* gfx-lat11: hand the frame over as the GPU comes free (the
                 * GPU's measured time), when that is later than the slot: a
                 * frame that would wait behind the one before on the GPU
                 * waits here instead, before its steps, pad read and draw */
                double gf = bwg_gate_gpu_free_ms();
                if (gf > 0.0 && (!have || gf > slot)) {
                    slot = gf;
                    have = 1;
                    by_gpu = 1;
                }
            }
            if (have) {
                aim = slot - jit_work_ms(0);
                if (jit_step_due(aim))          /* that pass would step: it needs longer */
                    aim = slot - jit_work_ms(1);
            }
            if (have && aim > nowm + 0.05) {
                what |= by_gpu ? 128u : 8u;     /* gfx-lat11: 128 = aimed at the GPU coming free */
                xbox_IrqSafePoint();
                jit_sleep_ns((uint64_t)((aim - nowm) * 1e6));
                continue;
            }
        }
        break;
    }
    {
        /* how long the last pass took to reach its flip: with the walk caught
         * up, the latest flip is that pass's frame */
        double r = g_bw_flip_ready_ms, nowm = jit_qpc_ms();
        uint32_t user = g_bw_ko_user;
        int caught = !user || (!g_bw_nv2a_walking
                               && (HU32(user + 0x40) & 0x03FFFFFFu) == (g_bw_ack_last_put & 0x03FFFFFFu));
        if (caught && s_jit_prev_ms > 0.0 && r > s_jit_prev_ms && r - s_jit_prev_ms < 100.0 && !guard)
            s_jit_w[s_jit_frame_stepped][s_jit_wn[s_jit_frame_stepped]++ % 32u] = (float)(r - s_jit_prev_ms);
        if (caught && s_draw_ms > 0.0 && r > s_draw_ms && r - s_draw_ms < 100.0 && !guard)
            s_draw_flip[s_draw_flipn++ % 32u] = (float)(r - s_draw_ms);   /* gfx-lat11: the horizon */
        if (s_jit == 4 && caught && s_jit_fe_ms > 0.0 && r > s_jit_fe_ms && r - s_jit_fe_ms < 50.0 && !guard)
            s_jit_tail[s_jit_tailn++ % 32u] = (float)(r - s_jit_fe_ms);
        s_jit_prev_ms = nowm;
        s_jit_kw0 = g_bw_ko_wait_us;
    }
    QueryPerformanceCounter(&t);
    ms = (double)(t.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
    s_st_jit_n++;
    s_st_jit_ms += ms;
    s_st_jit_guard += guard;
    if (what & 64u) {                   /* gfx-lat11 */
        s_st_gate_gpu++;
        s_st_gate_gpu_ms += gw_ms;
    }
    if (what & 128u)
        s_st_gate_aim++;
    if (s_latmode == 2)
        g_bwg_beat_need_ms = jit_work_ms(1);   /* gfx-lat11: the beat's way back (bwg_pace.c) */
    if (!guard && ms > 100.0)
        s_st_jit_long++;
    bw_fl_event(BW_FL_G_JIT, (unsigned)(ms * 1000.0), what, guard);
}

/* ---- the wrappers -------------------------------------------------------- */

/* Game_Update 0x00091380 (thiscall, this = 0x1D5C08): the game's own fixed
 * 30 Hz accumulator without its one-step floor (speed mode outside 1..3:
 * time unscaled, the floor skipped; 120fps.md §2, seen in code). Game_InitLevel
 * zeroes the mode, so it is written on every call. */
extern void sub_00091380_gen(void);
void sub_00091380(void)
{
    if (s_on && s_jit)
        jit_wait();                  /* lat-q: just-in-time frames, before the pass */
    if (s_on) {
        HU32(GAME_VA + GAME_SPEED_MODE) = GAME_SPEED_FIXED;
        QueryPerformanceCounter(&s_host_at_update);   /* about when it reads its clock */
    }
    s_in_update = 1;
    bw_fl_event(BW_FL_G_UPD, 0, 0, 0);
    if (g_bw_lat_on) {
        uint32_t steps0 = s_lat_steps;
        uint64_t clk0 = HU64(GAME_VA + GAME_STEP_QPC);
        s_lat_upd0 = bw_lat_now();
        sub_00091380_gen();
        s_lat_upd1 = bw_lat_now();
        /* BW_LATLOG (lat-m): a pass that ran steps, with the game's own step
         * clock, so each step's due time is known on the host clock (the
         * game's wait for its next step, apart from the port's part) */
        if (s_lat_steps != steps0)
            bw_lat_ev(BW_LAT_UPDATE, s_lat_upd0, (int64_t)HU64(GAME_VA + GAME_NOW_QPC),
                      (int64_t)HU64(SYS_QPC_FREQ_VA), (int64_t)clk0, (int64_t)HU64(GAME_VA + GAME_STEP_QPC),
                      (int64_t)(s_lat_steps - steps0), s_lat_upd1, 0.0, 0.0);
    } else
        sub_00091380_gen();
    bw_fl_event(BW_FL_G_UPD_END, 0, 0, 0);
    s_in_update = 0;
}

/* The frame time PFX_UpdateAll 0x1C350 hands every emitter's PFX_Update:
 * the float at [[0x1A7848+0x28]+0x1E8], which PFX_UpdateGroups 0x1F750
 * stores each drawn frame (0 while paused; seen in code). Its bits. */
#define PFX_VIEW_VA        (0x001A7848u + 0x28u)
static uint32_t pfx_dt(void)
{
    uint32_t v = HU32(PFX_VIEW_VA);
    return FX_VA_OK(v) ? HU32(v + 0x1E8u) : 0u;
}

/* ---- the per-step particle checker (BW_PFXCHECK; lane au-fx, call A10) --- *
 * A test aid, in every edition, read-only: it writes nothing guest-side and
 * calls no game code. After each drawn frame it compares what the particle
 * system holds with Original's 30 Hz sequence, worked out here on its own:
 * its own multiply chain, its own step count and its own table of every
 * particle, never the per-step fixes' values. Its model of Original (seen in
 * code: PFX_Update 0x1B980, PFX_SpawnParticle 0x20360, PFX_ShiftTrailHistory
 * 0x23F80, the bolt behaviours 0x248F0/0x253B0, PFXGroup_Update 0x1F170):
 *   - at 30 fps a drawn frame is one game step; a running drawn frame here
 *     follows m steps, counted at Overlays_Update (the last call of a
 *     running step);
 *   - a particle is born with age 0 and scale 1.0, and in every frame it
 *     lives its age steps by the frame time (fld dt; fadd age; fstp) and,
 *     when its emitter grows (flag 0x100000), its scale by k (+0xBC; fld k;
 *     fmul scale; fstp, at the x87's precision): after its first frame its
 *     age is the frame time, and with n = its steps so far (1 after a birth
 *     frame that ran a step, 0 after one that didn't) the scale must be
 *     ref[n], ref[0] = 1.0, ref[i] = k x ref[i-1];
 *   - a trail (flag 0x400, behaviours 1, 3, 4, 5, 7, 8, 9; +0xA = L points,
 *     all unused at birth) takes one point per step from the step after its
 *     birth (each behaviour shifts only a particle whose age isn't 0):
 *     min(n - 1, L) in use;
 *   - a bolt with a regen period of 0 (+0xCC) draws its kinks (the points at
 *     the field in +0x26C) again at every step and only then; the Lightning
 *     bolt's bend (+0x2A8) walks at every step and only then, besides the
 *     time-based turn PFX_Update gives every emitter (+0x2A8 += dt x +0xAC);
 *   - an effect group's script (list 0x1A7848+8, next +0x58, cursor +0x90)
 *     moves on only at a step, and its timed wait (op 5, target +0xA4)
 *     keeps the sum of the frame times since it began in +0xA8 (from 0;
 *     fld dt; fadd acc) and ends at the first step at which the sum reaches
 *     the target;
 *   - (lane fx-death) a particle ends (PFX_Update's test at the top of its
 *     loop, 0x1BD37: its age before the frame has reached its life) only on
 *     a drawn frame that follows a step, and then by Original's age: one
 *     born in the frames after step s's frame, up to and including step
 *     s+1's, is Original's frame-(s+1) newborn, whose age at a later step's
 *     frame is o + b (o the frame times before its birth frame since step
 *     s's frame, summed as an age is; b its own age at that frame's end); a
 *     frame that follows a step ends it iff o + b at the last step's frame
 *     before it has reached its life (in double: each term a float). One
 *     this can't place (first seen otherwise, or tracking started again
 *     while the game wasn't running) ends iff its age before the frame has
 *     reached its life, as the game's own test, also only at a step;
 *   - (fx-death) an emitter (list 0x1A7848+0x1C, next +0x1D0) with particles
 *     goes (PFX_Release 0x1B530) only on a frame that follows a step.
 * A particle is followed by its address from one drawn frame to the next
 * (its age must be the last one plus the frame time, or the same when its
 * update didn't run: its emitter's pause window, its steps then unchanged;
 * a newborn's is the frame time). One first seen otherwise ("unknown"), or
 * alive while the game isn't running or its emitter is frozen (+0x1F0 bit
 * 0), isn't judged. A frame of 2+ steps, and every particle that lived
 * through one, is counted apart ("multi"): Original runs one step per frame,
 * so its own hitches would show there. Output: a line a second, cumulative,
 * to the file and to stderr, and one at exit. */
#define PC_HN        131072u          /* the particle tables (a power of 2) */
#define PC_KN        256u             /* growth factors */
#define PC_NMAX      4096u            /* the longest chain */
#define PC_GN        1024u            /* groups */
#define PC_LN        64u              /* Lightning emitters */
#define PC_EN        1024u            /* the emitter tables (a power of 2) */
/* fl: 1 judged, 2 multi (4 followed, this frame only); fx-death: 8 placed (o),
 * 16 b, 32 followed from the last frame, 64 its emitter frozen, 128 newborn */
typedef struct { uint32_t p, e, age, n, fl, h, o, b, life; } pc_ent_t;
typedef struct { uint32_t g, cur, tg, acc, ref, fl; } pc_grp_t; /* fl: 1 in a timed wait */
typedef struct { uint32_t e, t, d, np; } pc_emt_t;             /* fx-death: time +0x294, desc +0x2AC */
enum { PCS_SCALE, PCS_TRAIL, PCS_BOLT, PCS_BEND, PCS_SCRIPT, PCS_WAIT, PCS_DEATH, PCS_RELEASE, PCS_N };
static const char *const s_pc_kind[PCS_N] = { "scale", "trail", "bolt", "bend", "script", "wait",
                                              "death", "release" };
static pc_emt_t s_pce[2][PC_EN];
static uint32_t s_pce_used[2][PC_EN / 2u], s_pce_n[2];
static uint32_t s_pc_pre;             /* fx-death: float bits, the frame times since the last step's frame */
static int s_pc_pre_ok;
static unsigned long long s_pc_unpl, s_pc_relempty, s_pc_held, s_pc_changed;
static unsigned s_pc_dnotes;
static FILE *s_pc;
static int s_pc_init;
static uint32_t s_pc_m;
static pc_ent_t *s_pc_t[2];
static uint32_t *s_pc_used[2], s_pc_nused[2];
static unsigned s_pc_cur;
static uint32_t s_pck_k[PC_KN], s_pck_len[PC_KN], *s_pck_ref[PC_KN];
static unsigned s_pck_n;
static pc_grp_t s_pcg[2][PC_GN];
static unsigned s_pcg_n[2];
static uint32_t s_pcl_e[2][PC_LN], s_pcl_ph[2][PC_LN], s_pcl_n[2];
static unsigned long long s_pc_frames, s_pc_stepf, s_pc_multif, s_pc_steps;
static unsigned long long s_pc_ok[PCS_N][2], s_pc_mis[PCS_N][2];
static double s_pc_maxrel[PCS_N];
static unsigned long long s_pc_unk, s_pc_untracked, s_pc_long, s_pc_late;
static unsigned s_pc_gnotes;
extern __declspec(thread) uint16_t g_fp_control_word;

/* the x87 at the game's precision (RECOMP_FP_PC, recomp_types.h): with
 * precision control 24 bits a result is rounded to a float's significand,
 * keeping the register's exponent range */
static double pc_round(double x)
{
    double ax = fabs(x);
    int ex;
    if (g_fp_control_word & 0x300u)
        return x;
    if ((ax <= 3.4028234663852886e38 && ax >= 1.1754943508222875e-38) || ax == 0.0 || x != x || ax == INFINITY)
        return (double)(float)x;
    x = frexp(x, &ex);
    return ldexp((double)(float)x, ex);
}

static uint32_t pc_mul(uint32_t a, uint32_t b)       /* fld a; fmul b; fstp */
{
    union { float f; uint32_t u; } x, y, r;
    x.u = a;
    y.u = b;
    r.f = (float)pc_round((double)x.f * (double)y.f);
    return r.u;
}

static uint32_t pc_add(uint32_t a, uint32_t b)       /* fld a; fadd b; fstp */
{
    union { float f; uint32_t u; } x, y, r;
    x.u = a;
    y.u = b;
    r.f = (float)pc_round((double)x.f + (double)y.f);
    return r.u;
}

static void pc_line(const char *tag)
{
    char buf[1024];
    int i, o;
    o = _snprintf(buf, sizeof buf, "%s %llu f %llu sf %llu mf %llu st %llu", tag,
                  (unsigned long long)GetTickCount64(), s_pc_frames, s_pc_stepf, s_pc_multif, s_pc_steps);
    for (i = 0; i < PCS_N && o > 0 && o < (int)sizeof buf; i++)
        o += _snprintf(buf + o, sizeof buf - (size_t)o, " | %s %llu %llu %llu %llu %.3g", s_pc_kind[i],
                       s_pc_ok[i][0], s_pc_mis[i][0], s_pc_ok[i][1], s_pc_mis[i][1], s_pc_maxrel[i]);
    if (o > 0 && o < (int)sizeof buf)
        o += _snprintf(buf + o, sizeof buf - (size_t)o, " | unknown %llu untracked %llu long %llu late %llu",
                       s_pc_unk, s_pc_untracked, s_pc_long, s_pc_late);
    if (o > 0 && o < (int)sizeof buf)          /* fx-death */
        _snprintf(buf + o, sizeof buf - (size_t)o, " | unplaced %llu held %llu changed %llu empty_between %llu",
                  s_pc_unpl, s_pc_held, s_pc_changed, s_pc_relempty);
    buf[sizeof buf - 1] = 0;
    fprintf(s_pc, "%s\n", buf);
    fflush(s_pc);
    fprintf(stderr, "[BW] pfxcheck: %s\n", buf);
}

static void pc_atexit(void)
{
    if (s_pc)
        pc_line("E");
}

static char s_pc_kindname[33];   /* BW_PFXCHECK_KIND: one kind's emitters, frame by frame ("K" lines) */
static unsigned long s_pc_klines;

static void pc_open(void)
{
    const char *p = getenv("BW_PFXCHECK"), *k = getenv("BW_PFXCHECK_KIND");
    s_pc_init = 1;
    if (!p || !*p)
        return;
    if (k && *k)
        _snprintf(s_pc_kindname, sizeof s_pc_kindname - 1, "%s", k);
    s_pc_t[0] = (pc_ent_t *)calloc(PC_HN, sizeof(pc_ent_t));
    s_pc_t[1] = (pc_ent_t *)calloc(PC_HN, sizeof(pc_ent_t));
    s_pc_used[0] = (uint32_t *)malloc(PC_HN / 2u * sizeof(uint32_t));
    s_pc_used[1] = (uint32_t *)malloc(PC_HN / 2u * sizeof(uint32_t));
    if (!s_pc_t[0] || !s_pc_t[1] || !s_pc_used[0] || !s_pc_used[1])
        return;
    s_pc = fopen(p, "a");
    if (!s_pc)
        return;
    fprintf(s_pc, "# pfxcheck 2, boot %lu, %s: C|E host_ms f frames sf frames_after_steps mf"
            " frames_after_2+_steps st steps | kind ok mismatch ok_multi mismatch_multi max_rel_diff"
            " (scale trail bolt bend script wait death release) | unknown untracked long late"
            " | unplaced held changed empty_between\n",
            (unsigned long)GetCurrentProcessId(), bw_edition_title(bw_edition()));
    fflush(s_pc);
    atexit(pc_atexit);
}

/* each running step (Overlays_Update) */
static void pfxcheck_step(void)
{
    if (!s_pc_init)
        pc_open();
    if (s_pc) {
        s_pc_m++;
        s_pc_steps++;
    }
}

static int pc_kidx(uint32_t k)
{
    unsigned i;
    uint32_t *r;
    for (i = 0; i < s_pck_n; i++)
        if (s_pck_k[i] == k)
            return (int)i;
    if (s_pck_n == PC_KN || (r = (uint32_t *)malloc(PC_NMAX * sizeof *r)) == NULL)
        return -1;
    r[0] = 0x3F800000u;                       /* 1.0 at birth */
    s_pck_k[s_pck_n] = k;
    s_pck_ref[s_pck_n] = r;
    s_pck_len[s_pck_n] = 1;
    return (int)s_pck_n++;
}

static int pc_refat(int ki, uint32_t n, uint32_t *out)
{
    uint32_t *r = s_pck_ref[ki];
    if (n >= PC_NMAX)
        return 0;
    while (s_pck_len[ki] <= n) {
        r[s_pck_len[ki]] = pc_mul(s_pck_k[ki], r[s_pck_len[ki] - 1u]);
        s_pck_len[ki]++;
    }
    *out = r[n];
    return 1;
}

static uint32_t pc_slot(uint32_t p)
{
    return ((p * 2654435761u) >> 15) & (PC_HN - 1u);
}

static pc_ent_t *pc_find(const pc_ent_t *t, uint32_t p)
{
    uint32_t i = pc_slot(p), j;
    for (j = 0; j < 64u; j++, i = (i + 1u) & (PC_HN - 1u)) {
        if (t[i].p == p)
            return (pc_ent_t *)&t[i];
        if (!t[i].p)
            return NULL;
    }
    return NULL;
}

static pc_ent_t *pc_put(unsigned w, uint32_t p)
{
    pc_ent_t *t = s_pc_t[w];
    uint32_t i = pc_slot(p), j;
    if (s_pc_nused[w] >= PC_HN / 2u)
        return NULL;
    for (j = 0; j < 64u; j++, i = (i + 1u) & (PC_HN - 1u)) {
        if (!t[i].p) {
            t[i].p = p;
            s_pc_used[w][s_pc_nused[w]++] = i;
            return &t[i];
        }
        if (t[i].p == p)
            return &t[i];
    }
    return NULL;
}

static void pc_count(int kind, int multi, int ok, double rel)
{
    if (ok) {
        s_pc_ok[kind][multi]++;
        return;
    }
    s_pc_mis[kind][multi]++;
    if (!multi && rel > s_pc_maxrel[kind])
        s_pc_maxrel[kind] = rel;
}

/* the first mismatches of each kind, in full ("M" lines) */
static void pc_note(const char *kind, uint32_t e, uint32_t p, uint32_t n, uint32_t got, uint32_t want)
{
    static unsigned notes;
    uint32_t desc = HU32(e + 0x2ACu);
    char name[33];
    int i;
    if (notes >= 60u)
        return;
    notes++;
    name[0] = 0;
    if (FX_VA_OK(desc) && FX_VA_OK(desc - 0x20u)) {
        for (i = 0; i < 32; i++) {
            char c = (char)HU8(desc - 0x20u + (uint32_t)i);
            if (!c)
                break;
            name[i] = (c > ' ' && c < 127) ? c : '_';
        }
        name[i] = 0;
    }
    fprintf(s_pc, "M %s %s e=%08X p=%08X fl=%08X beh=%08X L=%d n=%u got=%08X want=%08X age=%g life=%g"
            " frame=%llu\n", kind, name[0] ? name : "?", e, p, HU32(e), HU32(e + 0x220u),
            (int)(int16_t)HU16(e + 0xAu), n, got, want, p ? HF32(p + 0x10u) : 0.0f,
            p ? HF32(p + 0x14u) : 0.0f, s_pc_frames);
}

static int pc_shifter(uint32_t beh)          /* behaviours 1, 3, 4, 5, 7, 8, 9 (table 0x187B10) */
{
    return beh == 0x00024040u || beh == 0x00024180u || beh == 0x00024440u || beh == 0x00024700u
        || beh == 0x00024B50u || beh == 0x00024D30u || beh == 0x00025240u;
}

/* fx-death: the emitters of a frame, by address */
static uint32_t pce_slot(uint32_t e)
{
    return ((e * 2654435761u) >> 20) & (PC_EN - 1u);
}

static void pce_put(unsigned w, uint32_t e, uint32_t np)
{
    uint32_t i = pce_slot(e), j;
    if (s_pce_n[w] >= PC_EN / 2u)
        return;
    for (j = 0; j < PC_EN; j++, i = (i + 1u) & (PC_EN - 1u))
        if (!s_pce[w][i].e) {
            s_pce[w][i].e = e;
            s_pce[w][i].t = HU32(e + 0x294u);
            s_pce[w][i].d = HU32(e + 0x2ACu);
            s_pce[w][i].np = np;
            s_pce_used[w][s_pce_n[w]++] = i;
            return;
        }
}

/* still the same emitter in table w: on the list, its descriptor, its time not gone back */
static int pce_same(unsigned w, const pc_emt_t *o)
{
    uint32_t i = pce_slot(o->e), j;
    for (j = 0; j < PC_EN; j++, i = (i + 1u) & (PC_EN - 1u)) {
        const pc_emt_t *c = &s_pce[w][i];
        union { float f; uint32_t u; } a, b;
        if (!c->e)
            return 0;
        if (c->e != o->e)
            continue;
        a.u = c->t;
        b.u = o->t;
        return c->d == o->d && !(a.f < b.f);
    }
    return 0;
}

/* fx-death: the first death and release mismatches, in full ("M" lines) */
static void pc_dnote(const char *what, const pc_ent_t *pe, uint32_t m)
{
    union { float f; uint32_t u; } a, l, o, b;
    uint32_t desc;
    char name[33];
    int i;
    if (s_pc_dnotes >= 60u)
        return;
    s_pc_dnotes++;
    name[0] = 0;
    desc = FX_VA_OK(pe->e) ? HU32(pe->e + 0x2ACu) : 0;
    if (FX_VA_OK(desc) && FX_VA_OK(desc - 0x20u)) {
        for (i = 0; i < 32; i++) {
            char c = (char)HU8(desc - 0x20u + (uint32_t)i);
            if (!c)
                break;
            name[i] = (c > ' ' && c < 127) ? c : '_';
        }
        name[i] = 0;
    }
    a.u = pe->age;
    l.u = pe->life;
    o.u = pe->o;
    b.u = pe->b;
    fprintf(s_pc, "M %s %s e=%08X p=%08X m=%u age=%.6g life=%.6g o=%.6g b=%.6g o+b=%.6g fl=%X frame=%llu\n",
            what, name[0] ? name : "?", pe->e, pe->p, m, a.f, l.f, o.f, b.f, (double)o.f + (double)b.f,
            pe->fl, s_pc_frames);
}

/* after each drawn frame, every edition */
static void pfxcheck_frame(void)
{
    union { float f; uint32_t u; } dtu;
    uint32_t m, e, n = 0, g, w, pw, i, ln = 0;
    unsigned gn = 0;
    int run;
    const pc_ent_t *prev;
    if (!s_pc_init)
        pc_open();
    if (!s_pc)
        return;
    m = s_pc_m;
    s_pc_m = 0;
    dtu.u = pfx_dt();
    run = HU32(GAME_VA + GAME_STATE) == 0 && dtu.f > 0.0f;
    s_pc_frames++;
    if (m)
        s_pc_stepf++;
    if (m >= 2u)
        s_pc_multif++;
    pw = s_pc_cur;
    w = s_pc_cur ^= 1u;
    prev = s_pc_t[pw];
    for (i = 0; i < s_pc_nused[w]; i++)
        s_pc_t[w][s_pc_used[w][i]].p = 0;
    s_pc_nused[w] = 0;
    for (i = 0; i < s_pce_n[w]; i++)
        s_pce[w][s_pce_used[w][i]].e = 0;
    s_pce_n[w] = 0;
    for (e = HU32(FX_MGR_HEAD_VA); FX_VA_OK(e) && n < 4096u; e = HU32(e + FX_NEXT), n++) {
        uint32_t fl = HU32(e), beh = HU32(e + 0x220u), so = HU32(e + 0x280u), to = HU32(e + 0x258u);
        uint32_t bo = HU32(e + 0x26Cu), p, np = 0;
        int tn = (int)(int16_t)HU16(e + 0xAu), frozen = HU8(e + 0x1F0u) & 1, grow, trail, bolt, ki = -1;
        union { float f; uint32_t u; } k, per;
        k.u = HU32(e + 0xBCu);
        per.u = HU32(e + 0xCCu);
        grow = (fl & 0x00100000u) && so && so < 0x1000u && k.f > 0.0f && k.f != 1.0f && k.f < 100.0f;
        trail = (fl & 0x00000400u) && to < 0x1000u && tn > 1 && tn <= 256 && pc_shifter(beh);
        bolt = (beh == 0x000248F0u || beh == 0x000253B0u) && bo && bo < 0x1000u && tn >= 1 && tn <= 64;
        if (beh == 0x000253B0u && ln < PC_LN) {
            /* the bend: once per step while the bolt has a particle */
            uint32_t ph = HU32(e + 0x2A8u), live = HU32(e + 0x1ECu), j;
            for (j = 0; run && live && j < s_pcl_n[pw]; j++) {
                if (s_pcl_e[pw][j] != e)
                    continue;
                if (s_pcl_ph[pw][j] != 0xFFFFFFFFu) {
                    uint32_t timed = pc_add(pc_mul(dtu.u, HU32(e + 0xACu)), s_pcl_ph[pw][j]);
                    pc_count(PCS_BEND, m >= 2u, m ? ph != timed : ph == timed, 1.0);
                    if ((m ? ph == timed : ph != timed) && m < 2u)
                        pc_note("bend", e, 0, m, ph, timed);
                }
                break;
            }
            s_pcl_e[w][ln] = e;
            s_pcl_ph[w][ln] = live ? ph : 0xFFFFFFFFu;
            ln++;
        }
        if (grow)
            ki = pc_kidx(k.u);
        /* fx-death: every emitter's particles are followed now, for their
         * deaths; the other kinds are judged where they apply, as before */
        for (p = HU32(e + 0x1E0u); FX_VA_OK(p) && np < 4096u; p = HU32(p), np++) {
            uint32_t age = HU32(p + 0x10u), nn, f, h = 2166136261u, dfl = 0, o = 0, b = 0;
            const pc_ent_t *pe = pc_find(prev, p);
            pc_ent_t *ce;
            int multi, kinds = grow || trail || bolt;
            if (pe && pe->e == e && age == pc_add(dtu.u, pe->age)) {
                nn = pe->n + m;                        /* followed: its steps so far */
                f = (pe->fl & 3u) | 4u;
                if (!run || frozen)
                    f &= ~1u;
                dfl = (pe->fl & 24u) | 32u;
                o = pe->o;
                b = pe->b;
            } else if (pe && pe->e == e && age == pe->age) {
                nn = pe->n;                            /* its update didn't run (a pause window): */
                f = pe->fl & 3u;                       /* nothing of it moved, no step counted */
                if (!run || frozen)
                    f &= ~1u;
                dfl = (pe->fl & 24u) | 32u;
                o = pe->o;
                b = pe->b;
            } else if (age == dtu.u) {
                nn = m ? 1u : 0u;                      /* born in this frame */
                f = run && !frozen ? 1u : 0u;
                dfl = 128u;
                if (s_pc_pre_ok) {
                    dfl |= 8u;                         /* placed: the frame times before it since the step's frame */
                    o = s_pc_pre;
                }
            } else {
                nn = 0;
                f = 0;
                if (kinds)
                    s_pc_unk++;
            }
            if (!run)
                dfl &= ~24u;                           /* placed again from the next running frame on */
            else if (m && (dfl & 8u)) {
                b = age;                               /* its age at this step's frame */
                dfl |= 16u;
            }
            if (frozen)
                dfl |= 64u;
            if (m >= 2u)
                f |= 2u;
            multi = (f & 2u) != 0;
            if (!kinds)
                f &= ~1u;                              /* only the deaths judged */
            if (bolt) {
                int j;
                for (j = 0; j < tn * 3; j++)
                    h = (h ^ HU32(p + bo + 4u * (uint32_t)j)) * 16777619u;
            }
            if (f & 1u) {
                if (grow && ki >= 0) {
                    union { float f; uint32_t u; } got, want;
                    if (pc_refat(ki, nn, &want.u)) {
                        got.u = HU32(p + so);
                        pc_count(PCS_SCALE, multi, got.u == want.u,
                                 want.f != 0.0f ? fabs((double)got.f / (double)want.f - 1.0) : 1.0);
                        if (got.u != want.u && !multi)
                            pc_note("scale", e, p, nn, got.u, want.u);
                    } else
                        s_pc_long++;
                }
                if (trail) {
                    uint32_t hp = p + to;
                    int pts = 0, want = !nn ? 0 : nn - 1u < (uint32_t)tn ? (int)(nn - 1u) : tn;
                    while (pts < tn && HU32(hp + 12u * (uint32_t)pts) != 0x7F7FFFFFu)
                        pts++;
                    pc_count(PCS_TRAIL, multi, pts == want, fabs((double)(pts - want)));
                    if (pts != want && !multi)
                        pc_note("trail", e, p, nn, (uint32_t)pts, (uint32_t)want);
                }
                /* a bolt followed from the last frame: its kinks change at a
                 * step and only then (period 0), or never between steps; a
                 * bolt without the timer field (flag 19) re-rolls when its
                 * next pointer reads as 0.0 or less, so only the last one */
                if (bolt && (f & 4u) && ((fl & 0x00080000u) || !HU32(p))) {
                    int chg = h != pe->h;
                    if (per.f == 0.0f)
                        pc_count(PCS_BOLT, multi, chg == (m != 0), 1.0);
                    else if (!m)
                        pc_count(PCS_BOLT, multi, !chg, 1.0);
                    if ((per.f == 0.0f ? chg != (m != 0) : (!m && chg)) && !multi)
                        pc_note("bolt", e, p, nn, (uint32_t)chg, m);
                }
            }
            ce = pc_put(w, p);
            if (!ce) {
                s_pc_untracked++;
                continue;
            }
            ce->e = e;
            ce->age = age;
            ce->n = nn;
            ce->fl = (f & 3u) | dfl;
            ce->h = h;
            ce->o = o;
            ce->b = b;
            ce->life = HU32(p + 0x14u);
        }
        pce_put(w, e, np);
    }
    s_pcl_n[w] = ln;
    if (run) {
        /* fx-death: the particles of the last frame. One followed into this
         * frame survived; one gone while its emitter stayed ended (in this
         * frame's update); one gone with its emitter is the release's. It
         * must end iff this frame follows a step and its model has reached
         * its life (o + b placed, else its age), and only then. */
        for (i = 0; i < s_pc_nused[pw]; i++) {
            const pc_ent_t *pe = &s_pc_t[pw][s_pc_used[pw][i]], *ce;
            union { float f; uint32_t u; } a, l, o, b;
            int surv, due;
            pc_emt_t oe;
            if (!pe->p || (pe->fl & 64u))
                continue;                              /* frozen: not judged */
            ce = pc_find(s_pc_t[w], pe->p);
            surv = ce && ce->e == pe->e && (ce->fl & 32u);
            if (surv && (ce->fl & 64u))
                continue;                              /* frozen now: the game's own */
            if (!surv) {
                oe.e = pe->e;
                oe.t = 0;
                oe.d = 0;
                {
                    uint32_t q = pce_slot(pe->e), j;
                    for (j = 0; j < PC_EN; j++, q = (q + 1u) & (PC_EN - 1u)) {
                        if (!s_pce[pw][q].e)
                            break;
                        if (s_pce[pw][q].e == pe->e) {
                            oe = s_pce[pw][q];
                            break;
                        }
                    }
                }
                if (!pce_same(w, &oe))
                    continue;                          /* gone with its emitter */
                if (HU8(pe->e + 0x1F0u) & 1u)
                    continue;                          /* frozen now: the game's own */
                if (ce && ce->e == pe->e && !(ce->fl & 128u)) {
                    s_pc_changed++;                    /* its age set by the game's code: not judged */
                    continue;
                }
            }
            a.u = pe->age;
            l.u = pe->life;
            o.u = pe->o;
            b.u = pe->b;
            if (!(l.f > 0.0f) || !(l.f < 3.0e38f) || !(a.f >= 0.0f) || !(a.f < 3.0e38f))
                continue;                              /* no life to judge by */
            if (pe->fl & 8u)
                due = (pe->fl & 16u) && (double)o.f + (double)b.f >= (double)l.f;
            else {
                due = !(a.f < l.f);
                if (m && due)
                    s_pc_unpl++;
            }
            if (!m) {
                if (surv && !(a.f < l.f))
                    s_pc_held++;                       /* past its life, kept to the step */
                due = 0;
            }
            pc_count(PCS_DEATH, m >= 2u, surv != due, 1.0);
            if (surv == due && m < 2u)
                pc_dnote(surv ? "death-late" : m ? "death-early" : "death-between", pe, m);
        }
        /* the emitters of the last frame that had particles: gone only at a step */
        for (i = 0; i < s_pce_n[pw]; i++) {
            const pc_emt_t *oe = &s_pce[pw][s_pce_used[pw][i]];
            if (pce_same(w, oe))
                continue;
            if (!oe->np) {
                if (!m)
                    s_pc_relempty++;                   /* nothing of it drawn: counted, not judged */
                continue;
            }
            pc_count(PCS_RELEASE, m >= 2u, m != 0, 1.0);
            if (!m && s_pc_dnotes < 60u) {
                s_pc_dnotes++;
                fprintf(s_pc, "M release-between e=%08X d=%08X particles %u frame=%llu\n", oe->e, oe->d, oe->np,
                        s_pc_frames);
            }
        }
    }
    /* fx-death: the frame times since the last step's frame (fld dt; fadd) */
    if (!run)
        s_pc_pre_ok = 0;
    else if (m) {
        s_pc_pre = 0;
        s_pc_pre_ok = 1;
    } else if (s_pc_pre_ok)
        s_pc_pre = pc_add(dtu.u, s_pc_pre);
    if (s_pc_kindname[0] && s_pc_klines < 40000u) {
        /* the kind's emitters: what drives their spawning, once a drawn frame */
        for (e = HU32(FX_MGR_HEAD_VA), n = 0; FX_VA_OK(e) && n < 4096u; e = HU32(e + FX_NEXT), n++) {
            uint32_t desc = HU32(e + 0x2ACu), p, np = 0, born = 0;
            char name[33];
            int c;
            name[0] = 0;
            if (FX_VA_OK(desc) && FX_VA_OK(desc - 0x20u)) {
                for (c = 0; c < 32; c++) {
                    name[c] = (char)HU8(desc - 0x20u + (uint32_t)c);
                    if (!name[c])
                        break;
                }
                name[32] = 0;
            }
            if (strcmp(name, s_pc_kindname) != 0)
                continue;
            for (p = HU32(e + 0x1E0u); FX_VA_OK(p) && np < 4096u; p = HU32(p), np++)
                born += HU32(p + 0x10u) == dtu.u;
            fprintf(s_pc, "K %llu m %u dt %.4f e %08X time %.4f acc %.5f r %.3f %.3f %.3f ramp %.3f %.3f"
                    " left %d live %u max %d born %u hook %08X win %.3f %.3f fl %08X 1f0 %02X\n",
                    s_pc_frames, m, dtu.f, e, HF32(e + 0x294u), HF32(e + 0x29Cu), HF32(e + 0x20u),
                    HF32(e + 0x24u), HF32(e + 0x28u), HF32(e + 0x2Cu), HF32(e + 0x30u), (int)HU32(e + 0x2C0u),
                    HU32(e + 0x1ECu), (int)(int16_t)HU16(e + 8u), born, HU32(e + 0x22Cu),
                    HF32(e + 0x204u), HF32(e + 0x208u), HU32(e), HU8(e + 0x1F0u));
            s_pc_klines++;
        }
    }
    /* the group scripts */
    for (g = HU32(0x001A7848u + 8u), n = 0; FX_VA_OK(g) && n < 4096u && gn < PC_GN; g = HU32(g + 0x58u), n++) {
        pc_grp_t *o = NULL, *c = &s_pcg[w][gn++];
        union { float f; uint32_t u; } tg, acc, oacc, ref;
        uint32_t cur = HU32(g + 0x90u);
        tg.u = HU32(g + 0xA4u);
        acc.u = HU32(g + 0xA8u);
        for (i = 0; i < s_pcg_n[pw]; i++)
            if (s_pcg[pw][i].g == g) {
                o = &s_pcg[pw][i];
                break;
            }
        c->g = g;
        c->cur = cur;
        c->tg = tg.u;
        c->acc = acc.u;
        c->ref = acc.u;
        c->fl = 0;
        if (o && run) {
            oacc.u = o->acc;
            if (cur != o->cur || tg.u != o->tg || acc.f < oacc.f) {
                pc_count(PCS_SCRIPT, m >= 2u, m != 0, 1.0);     /* moved on: only at a step */
                if (!m && s_pc_gnotes < 30u) {
                    s_pc_gnotes++;
                    fprintf(s_pc, "M script g=%08X cur %08X->%08X op %u->%u tg %08X->%08X acc %g->%g frame=%llu\n",
                            g, o->cur, cur, FX_VA_OK(o->cur) ? HU8(o->cur) : 255u, FX_VA_OK(cur) ? HU8(cur) : 255u,
                            o->tg, tg.u, oacc.f, acc.f, s_pc_frames);
                }
            } else if (o->fl & 1u) {
                ref.u = pc_add(dtu.u, o->ref);
                pc_count(PCS_WAIT, m >= 2u, acc.u == ref.u,
                         ref.f != 0.0f ? fabs((double)acc.f / (double)ref.f - 1.0) : 1.0);
                if (acc.u != ref.u && m < 2u && s_pc_gnotes < 30u) {
                    s_pc_gnotes++;
                    fprintf(s_pc, "M wait g=%08X cur %08X tg %g acc %g ref %g dt %g m %u frame=%llu\n",
                            g, cur, tg.f, acc.f, ref.f, dtu.f, m, s_pc_frames);
                }
                /* due at this step but not moved on: the game compares the sum
                 * in the register (fadd; fst; fcomp), at the x87's precision,
                 * not the float it stored (inferred from two attract-demo
                 * notes: with 53-bit precision a sum just under the target is
                 * stored as the target and the wait ends one call later, as
                 * in Original) */
                if (m && !(acc.f < tg.f) && pc_round((double)dtu.f + (double)oacc.f) < (double)tg.f
                        && s_pc_gnotes < 30u) {
                    s_pc_gnotes++;                             /* the stored sum reached it, the register's not */
                    fprintf(s_pc, "M wait-register g=%08X tg %08X acc %08X was %08X dt %08X fpcw %04X frame=%llu\n",
                            g, tg.u, acc.u, oacc.u, dtu.u, (unsigned)g_fp_control_word, s_pc_frames);
                }
                if (m && !(pc_round((double)dtu.f + (double)oacc.f) < (double)tg.f)) {
                    s_pc_late++;
                    if (s_pc_gnotes < 30u) {
                        s_pc_gnotes++;
                        fprintf(s_pc, "M late g=%08X cur %08X op %u tg %08X acc %08X was %08X dt %08X m %u"
                                " fpcw %04X ac %08X frame=%llu\n", g, cur, FX_VA_OK(cur) ? HU8(cur) : 255u,
                                tg.u, acc.u, oacc.u, dtu.u, m, (unsigned)g_fp_control_word,
                                HU32(g + 0xACu), s_pc_frames);
                    }
                }
                c->ref = ref.u;
            }
        }
        if (FX_VA_OK(cur) && HU8(cur) == 5u && tg.f != 0.0f)
            c->fl = 1u;
        if (!FX_VA_OK(cur) || HU8(cur) == 0u)
            c->fl |= 2u;                                       /* no script left (or at its end) */
    }
    s_pcg_n[w] = gn;
    /* a group gone since the last frame with its script still running: the
     * script reached its end (op 0) in this frame, a move too (the game's own
     * releases come in its steps). One gone from its end (op 0, a group kept
     * for its children) went with its last child, which is time-based. */
    for (i = 0; run && i < s_pcg_n[pw]; i++) {
        uint32_t oc = s_pcg[pw][i].cur;
        unsigned j;
        for (j = 0; j < gn && s_pcg[w][j].g != s_pcg[pw][i].g; j++)
            ;
        if (j < gn || (s_pcg[pw][i].fl & 2u))
            continue;
        pc_count(PCS_SCRIPT, m >= 2u, m != 0, 1.0);
        if (!m && s_pc_gnotes < 30u) {
            s_pc_gnotes++;
            fprintf(s_pc, "M script gone g=%08X cur %08X frame=%llu\n", s_pcg[pw][i].g, oc, s_pc_frames);
        }
    }
    {
        static ULONGLONG last;
        ULONGLONG now = GetTickCount64();
        if (!last)
            last = now;
        if (now - last >= 1000u) {
            last = now;
            pc_line("C");
        }
    }
}

/* Overlays_Update 0x00083EF0: the last call of a running step (after the
 * camera and the physics), so a snapshot sees each step's final state, also
 * when a frame runs two steps. */
extern void sub_00083EF0_gen(void);
extern void bw_audio_note_step(void);   /* au-lat: the sound output's margin (bw_apu_out.c; host-side only) */
void sub_00083EF0(void)
{
    sub_00083EF0_gen();
    if (s_in_update && HU32(GAME_VA + GAME_STATE) == 0) {
        bw_audio_note_step();
        if (s_on) {
            snapshot_step();
            s_water_gen++;
        }
        steplog_step();
        objlog_step();
        fxlog_step();
        pfxcheck_step();
        if (g_bw_lat_on)
            lat_step();
    }
}

/* ---- particle growth by the frame time (lane wake, docs/remaster/wake.md) -- *
 * A frame-rate trap in the game's particle tick (seen in code): when an
 * emitter's per-frame scale factor k (+0xBC, copied from its descriptor by
 * PFX_CopyDescriptor) is neither 0 nor 1, PFX_ComputeParticleLayout gives its
 * particles a scale field (flag 0x100000 at +0, the field's offset at +0x280),
 * PFX_SpawnParticle starts it at 1.0, PFX_Update multiplies it by k once per
 * drawn frame (with no frame time; skipped only when the frame time is 0, i.e.
 * paused), and the sprite renderer multiplies the particle's size by it. So a
 * particle's size after t seconds is k^(frames drawn): k^(30 t) in Original's
 * 30 fps, k^(240 t) at 240 fps. The boat's wake and spray are such emitters
 * (BW_FXLOG's k=), and at the high frame rate their puffs swelled into big
 * white blobs at any speed. Remastered keeps the 30 fps growth per second: for
 * each drawn frame shorter than 1/30 s, every growing emitter's k is raised to
 * the power of the frame's 30 Hz steps (frame time x 30; the time
 * Graphics_RenderFrame hands the effects, ms x 0.001, seen at 0x2821B; a
 * longer frame keeps k, as Original does) and put back after the frame,
 * where it still holds the value written (an emitter freed and set up again
 * inside the frame keeps its new descriptor's k). An emitter set up inside
 * the frame (group scripts run before the particles) gets its full k for that
 * one frame, as at 30 fps. Only while the high frame rate is on (never in
 * Original); BW_HIFPS_PFXGROW=0 is the undo. */
#define PFX_HEAD_VA        (0x001A7848u + 0x1Cu)    /* = FX_MGR_HEAD_VA */
#define PFX_GROW_FLAG      0x00100000u
#define PFX_GROW_K         0xBCu
#define PFX_GROW_MAX       1024
static uint32_t s_pg_va[PFX_GROW_MAX], s_pg_k[PFX_GROW_MAX], s_pg_wrote[PFX_GROW_MAX];
static unsigned s_pg_n;
static unsigned long s_st_pg, s_st_pgover;

static void pfxgrow_apply(uint32_t dt_ms)
{
    uint32_t e, n = 0;
    float steps = (float)dt_ms * 0.001f * 30.0f;
    s_pg_n = 0;
    /* A frame of 1/30 s or longer keeps the game's own factor: Original
     * applies k once per frame however long the frame took (a hitch of 1.4 s
     * raised as k^42 swelled every live puff for that one frame: seen in an
     * open contest run, oc_fix, before this bound). */
    if (!s_pfxgrow || !dt_ms || steps >= 1.0f)
        return;
    for (e = HU32(PFX_HEAD_VA); FX_VA_OK(e) && n < 4096u; e = HU32(e + FX_NEXT), n++) {
        union { float f; uint32_t u; } k, w;
        if (!(HU32(e) & PFX_GROW_FLAG))
            continue;
        k.u = HU32(e + PFX_GROW_K);
        if (!(k.f > 0.0f) || k.f == 1.0f)
            continue;
        if (s_pg_n == PFX_GROW_MAX) {
            s_st_pgover++;
            break;
        }
        w.f = powf(k.f, steps);
        if (w.f == 1.0f)                  /* keep "neither 0 nor 1" (the layout's test) */
            w.f = nextafterf(1.0f, k.f);
        s_pg_va[s_pg_n] = e;
        s_pg_k[s_pg_n] = k.u;
        s_pg_wrote[s_pg_n] = w.u;
        HU32(e + PFX_GROW_K) = w.u;
        s_pg_n++;
    }
    s_st_pg += s_pg_n;
}

static void pfxgrow_restore(void)
{
    unsigned i;
    for (i = 0; i < s_pg_n; i++)
        if (HU32(s_pg_va[i] + PFX_GROW_K) == s_pg_wrote[i])
            HU32(s_pg_va[i] + PFX_GROW_K) = s_pg_k[i];
    s_pg_n = 0;
}

/* ---- the emitters alive after a drawn frame (lane au-fx, B+) ------------ *
 * What the per-step fixes put back after a frame is put back only into an
 * emitter still on the manager's list (one released inside the frame may
 * have had its memory freed). */
#define PFE_N 8192u
static uint32_t s_pfe_k[PFE_N], s_pfe_s[PFE_N], s_pfe_stamp;

static uint32_t pfe_slot(uint32_t e)
{
    return ((e * 2654435761u) >> 19) & (PFE_N - 1u);
}

static void pfe_build(void)
{
    uint32_t e, n = 0;
    if (!++s_pfe_stamp) {
        memset(s_pfe_s, 0, sizeof s_pfe_s);
        s_pfe_stamp = 1;
    }
    for (e = HU32(PFX_HEAD_VA); FX_VA_OK(e) && n < 4096u; e = HU32(e + FX_NEXT), n++) {
        uint32_t i = pfe_slot(e), j;
        for (j = 0; j < PFE_N; j++, i = (i + 1u) & (PFE_N - 1u)) {
            if (s_pfe_s[i] != s_pfe_stamp) {
                s_pfe_s[i] = s_pfe_stamp;
                s_pfe_k[i] = e;
                break;
            }
            if (s_pfe_k[i] == e)
                break;
        }
    }
}

static int pfe_live(uint32_t e)
{
    uint32_t i = pfe_slot(e), j;
    for (j = 0; j < PFE_N; j++, i = (i + 1u) & (PFE_N - 1u)) {
        if (s_pfe_s[i] != s_pfe_stamp)
            return 0;
        if (s_pfe_k[i] == e)
            return 1;
    }
    return 0;
}

/* ---- the growth per game step (lane au-fx, B+; call A10) ---------------- *
 * Supersedes the frame-time factor above while the game runs (that one stays
 * for frames while it doesn't: menus, pauses, the end). Original multiplies
 * every growing particle's scale by k once per drawn frame, i.e. once per
 * game step at 30 fps (seen: 0x1C02E-0x1C043, fld k; fmul scale; fstp,
 * skipped only when the frame time is 0). Remastered keeps that sequence
 * exactly, per step, and shows a blend between steps:
 *   - the stored scale: after a drawn frame that followed m steps, every
 *     particle alive before it holds its scale from before the frame
 *     multiplied by k m times, one fld/fmul/fstp each, as m frames of
 *     Original would have left it; after a frame with no step it holds the
 *     same bits as before. Per step the stored scale is k^n by Original's
 *     own chain;
 *   - birth: a particle born in a drawn frame that followed a step holds k
 *     after it (k x 1.0, as Original's newborn after its birth frame); one
 *     born in a frame with no step holds 1.0 and gets its first k at the
 *     next step's frame. The mapping: the frames after step s's frame, up to
 *     and including step s+1's, span Original's frame s+1, so a particle
 *     born in any of them is Original's frame-(s+1) newborn, whose first k
 *     comes with step s+1. The spawn itself is unchanged (time-based: its
 *     accumulator fills by the frame time);
 *   - the look, for the drawing only: scale x k^a, a = the fraction of a
 *     step since the last one (step_alpha; 0 at the steps), held at
 *     Original's last value k^J, J = ceil(30 x life). PFX_Update multiplies
 *     by +0xBC, so for the frame +0xBC reads F = k^(a newborn's steps + a)
 *     and every living particle's scale is set to its look / F beforehand;
 *     after the frame +0xBC is k again and every scale holds its stored
 *     bits, found by address and age (a particle's age after the frame is
 *     its age before plus the frame time; a newborn's is the frame time);
 *   - what is time-based stays live: age, position, the spawn accumulator,
 *     the colour and frame tracks.
 * An emitter released or set up again inside the frame (its +0xBC no longer
 * F) is left to the game; one set up inside a frame with no step has its
 * newborns put back to 1.0. Frozen emitters (+0x1F0 bit 0: frame time 0) are
 * left alone. BW_HIFPS_PFXGROW=0 is the undo (per drawn frame again);
 * BW_PFXCHECK checks the stored scales against its own chain. */
#define PGB_MAXP           65536u
#define PGB_MAXE           1024u
#define PFX_AGE            0x10u
#define PFX_LIFE           0x14u
#define PFX_FROZEN         0x1F0u
#define PFX_SCALE_OFF      0x280u
#define PFX_PARTS          0x1E0u
static uint32_t s_pgb_p[PGB_MAXP], s_pgb_age[PGB_MAXP], s_pgb_s0[PGB_MAXP], s_pgb_st[PGB_MAXP];
static uint32_t s_pgb_e[PGB_MAXE], s_pgb_k[PGB_MAXE], s_pgb_f[PGB_MAXE], s_pgb_off[PGB_MAXE];
static uint32_t s_pgb_i0[PGB_MAXE], s_pgb_i1[PGB_MAXE], s_pgb_m;
static unsigned s_pgb_ne, s_pgb_np;
static int s_pgb_mode;                 /* 0 nothing, 1 per step, 2 the frame-time factor */
static uint32_t s_pgd_k[PFE_N], s_pgd_s[PFE_N], s_pgd_stamp;   /* emitters put back this frame */
static unsigned long s_st_pgb_old, s_st_pgb_new, s_st_pgb_fresh, s_st_pgb_odd, s_st_pgb_gone, s_st_pgb_over;
static unsigned long s_st_pgb_still;   /* fx-death: put back with a frame time but no update run (counted) */

static double pgb_round(double x)      /* RECOMP_FP_PC (recomp_types.h) */
{
    double ax = fabs(x);
    int ex;
    if (g_fp_control_word & 0x300u)
        return x;
    if ((ax <= 3.4028234663852886e38 && ax >= 1.1754943508222875e-38) || ax == 0.0 || x != x || ax == INFINITY)
        return (double)(float)x;
    x = frexp(x, &ex);
    return ldexp((double)(float)x, ex);
}

static uint32_t pgb_mul(uint32_t k, uint32_t s)     /* fld k; fmul scale; fstp (0x1C02E) */
{
    union { float f; uint32_t u; } a, b, r;
    a.u = k;
    b.u = s;
    r.f = (float)pgb_round((double)a.f * (double)b.f);
    return r.u;
}

static uint32_t pgb_add(uint32_t dt, uint32_t age) /* fld dt; fadd age; fstp (0x1C003) */
{
    union { float f; uint32_t u; } a, b, r;
    a.u = dt;
    b.u = age;
    r.f = (float)pgb_round((double)a.f + (double)b.f);
    return r.u;
}

static void pgd_add(uint32_t e)
{
    uint32_t i = pfe_slot(e), j;
    for (j = 0; j < PFE_N; j++, i = (i + 1u) & (PFE_N - 1u))
        if (s_pgd_s[i] != s_pgd_stamp || s_pgd_k[i] == e) {
            s_pgd_s[i] = s_pgd_stamp;
            s_pgd_k[i] = e;
            return;
        }
}

static int pgd_has(uint32_t e)
{
    uint32_t i = pfe_slot(e), j;
    for (j = 0; j < PFE_N; j++, i = (i + 1u) & (PFE_N - 1u)) {
        if (s_pgd_s[i] != s_pgd_stamp)
            return 0;
        if (s_pgd_k[i] == e)
            return 1;
    }
    return 0;
}

static void pfxgrow_pre(uint32_t dt_ms)
{
    uint32_t e, n = 0, m = s_steps_since_draw > 16u ? 16u : s_steps_since_draw;
    float a;
    s_pgb_mode = 0;
    s_pgb_ne = s_pgb_np = 0;
    if (!s_pfxgrow || !dt_ms)
        return;
    if (!s_pf_on) {
        pfxgrow_apply(dt_ms);                  /* not running: the frame-time factor */
        s_pgb_mode = 2;
        return;
    }
    s_pgb_mode = 1;
    s_pgb_m = m;
    a = s_pf_alpha;
    for (e = HU32(PFX_HEAD_VA); FX_VA_OK(e) && n < 4096u; e = HU32(e + FX_NEXT), n++) {
        union { float f; uint32_t u; } k, f;
        uint32_t off, p, np, live = 0;
        double ra;
        if (!(HU32(e) & PFX_GROW_FLAG) || (HU8(e + PFX_FROZEN) & 1u))
            continue;
        k.u = HU32(e + PFX_GROW_K);
        off = HU32(e + PFX_SCALE_OFF);
        if (!(k.f > 0.0f) || k.f == 1.0f || !(k.f < 100.0f) || !off || off >= 0x1000u)
            continue;
        for (p = HU32(e + PFX_PARTS); FX_VA_OK(p) && live < 4096u; p = HU32(p))
            live++;
        if (s_pgb_ne == PGB_MAXE || s_pgb_np + live > PGB_MAXP) {
            s_st_pgb_over++;                   /* left to the game for this frame */
            continue;
        }
        f.f = powf(k.f, (m ? 1.0f : 0.0f) + a);
        if (!(f.f > 0.0f) || f.f == 1.0f)
            f.f = nextafterf(1.0f, k.f);       /* neither 0 nor 1 (the layout's test) */
        ra = pow((double)k.f, (double)a) / (double)f.f;
        s_pgb_e[s_pgb_ne] = e;
        s_pgb_k[s_pgb_ne] = k.u;
        s_pgb_f[s_pgb_ne] = f.u;
        s_pgb_off[s_pgb_ne] = off;
        s_pgb_i0[s_pgb_ne] = s_pgb_np;
        for (p = HU32(e + PFX_PARTS), np = 0; FX_VA_OK(p) && np < live; p = HU32(p), np++) {
            union { float f; uint32_t u; } s, d;
            double x = a, life = HF32(p + PFX_LIFE);
            uint32_t j;
            s.u = HU32(p + off);
            s_pgb_p[s_pgb_np] = p;
            s_pgb_age[s_pgb_np] = HU32(p + PFX_AGE);
            s_pgb_s0[s_pgb_np] = s.u;
            for (j = 0; j < m; j++)
                s.u = pgb_mul(k.u, s.u);
            s_pgb_st[s_pgb_np] = s.u;
            s_pgb_np++;
            if (!(life - (double)HF32(p + PFX_AGE) > 0.1)) {
                /* its last steps: the look is held at k^J */
                if (s.f > 0.0f && life > 0.0 && life < 1e5) {
                    double J = ceil(30.0 * life - 1e-4);
                    double nn = floor(log((double)s.f) / log((double)k.f) + 0.5);
                    if (nn >= J)
                        x = 0.0;               /* at Original's last value: held */
                    else if (nn + x > J)
                        x = J - nn;
                }
                d.f = (float)((double)s.f * pow((double)k.f, x) / (double)f.f);
            } else
                d.f = (float)((double)s.f * ra);    /* k^a / F, the same for the whole emitter */
            HU32(p + off) = d.u;
        }
        s_pgb_i1[s_pgb_ne] = s_pgb_np;
        HU32(e + PFX_GROW_K) = f.u;
        s_pgb_ne++;
    }
    s_st_pg += s_pgb_ne;
}

/* after the frame, with pfe_build's list */
static void pfxgrow_post(void)
{
    unsigned i;
    uint32_t dt, e, n = 0;
    if (s_pgb_mode == 2)
        pfxgrow_restore();
    if (s_pgb_mode != 1) {
        s_pgb_mode = 0;
        return;
    }
    s_pgb_mode = 0;
    dt = pfx_dt();
    if (!++s_pgd_stamp) {
        memset(s_pgd_s, 0, sizeof s_pgd_s);
        s_pgd_stamp = 1;
    }
    for (i = 0; i < s_pgb_ne; i++) {
        uint32_t off = s_pgb_off[i], j = s_pgb_i0[i], end = s_pgb_i1[i], p, np = 0;
        uint32_t nb = s_pgb_m ? s_pgb_k[i] : 0x3F800000u;   /* a newborn: k x 1.0 after a step, else 1.0 */
        e = s_pgb_e[i];
        if (!pfe_live(e) || HU32(e + PFX_GROW_K) != s_pgb_f[i] || HU32(e + PFX_SCALE_OFF) != off) {
            s_st_pgb_gone++;                   /* released, or set up again: the game's now */
            continue;
        }
        HU32(e + PFX_GROW_K) = s_pgb_k[i];
        pgd_add(e);
        for (p = HU32(e + PFX_PARTS); FX_VA_OK(p) && np < 4096u; p = HU32(p), np++) {
            uint32_t ag = HU32(p + PFX_AGE), q = j;
            if (!(q < end && s_pgb_p[q] == p)) {
                if (dt && ag == dt) {          /* born in this frame: age 0 + the frame time */
                    HU32(p + off) = nb;
                    s_st_pgb_new++;
                    continue;
                }
                for (q = j; q < end && s_pgb_p[q] != p; q++)
                    ;
                if (q == end) {
                    s_st_pgb_odd++;
                    continue;
                }
            }
            j = q + 1u;
            if (ag == s_pgb_age[q] && (!dt || s_pfxstophold)) {
                /* its update didn't run: no frame time (B+ as proven), or, only
                 * with fx-death's stopped-emitter hold on (BW_HIFPS_PFXSTOPHOLD=1,
                 * off: the T2 rule, never exercised), the emitter's pause window
                 * (a natural one, or a stopped emitter the hold keeps): no x k */
                HU32(p + off) = s_pgb_s0[q];
                s_st_pgb_still += dt != 0u;
            } else if (ag == pgb_add(dt, s_pgb_age[q])) {
                HU32(p + off) = s_pgb_st[q];
                s_st_pgb_old++;
            } else
                s_st_pgb_odd++;                /* expired by the game (age set to its life): gone next frame */
        }
    }
    /* emitters set up inside a frame with no step: their newborns had k */
    if (s_pgb_m || !dt)
        return;
    for (e = HU32(PFX_HEAD_VA); FX_VA_OK(e) && n < 4096u; e = HU32(e + FX_NEXT), n++) {
        union { float f; uint32_t u; } k;
        uint32_t off, p, np = 0;
        if (!(HU32(e) & PFX_GROW_FLAG) || pgd_has(e))
            continue;
        k.u = HU32(e + PFX_GROW_K);
        off = HU32(e + PFX_SCALE_OFF);
        if (!(k.f > 0.0f) || k.f == 1.0f || !off || off >= 0x1000u)
            continue;
        for (p = HU32(e + PFX_PARTS); FX_VA_OK(p) && np < 4096u; p = HU32(p), np++)
            if (HU32(p + PFX_AGE) == dt && HU32(p + off) == k.u) {
                HU32(p + off) = 0x3F800000u;
                s_st_pgb_fresh++;
            }
    }
}

/* ---- effect group scripts per game step (lane au-fx, B+; call A10) ------ *
 * A frame-rate trap (seen in code): PFX_UpdateGroups 0x1F750 runs
 * PFXGroup_Update 0x1F170 on every group once per drawn frame, and a group's
 * script (cursor +0x90) moves on in calls: its spawns (ops 1, 2, 17), loops
 * (3, 4) and waits run when a call reaches them, and its timed wait (op 5:
 * target +0xA4, set at the call that reaches it; +0xA8 the frame times added
 * since) ends at the first call at which the sum reaches the target. So at
 * 30 fps everything a script does falls on a step, and its waits are whole
 * steps (a wait of 0.1 s is 3 or 4 steps); at the high frame rate a script
 * moved on between steps, its waits ended up to a step early and a loop of
 * short waits ran faster (the wake audit's "judged not worth fixing"; now
 * fixed, B+). Remastered keeps the script per step: on a drawn frame with
 * no new step every group's cursor reads 0 for the frame, so
 * PFXGroup_Update runs no command (it still turns its children with it and
 * clears its frame flag, as every call does), and after the frame the
 * cursor is put back and a timed wait's sum takes the frame time as the
 * game would have added it (fld dt; fadd acc; fst), so the wait's clock
 * stays live and exact and its end falls on the first step's frame at which
 * the sum reaches the target. Frames that follow a step run the game's own
 * code. While the game isn't running, as before. BW_HIFPS_PFXSCRIPT=0 is the
 * undo; BW_PFXCHECK checks it. */
#define PFXG_HEAD_VA       (0x001A7848u + 8u)
#define PFXG_NEXT          0x58u
#define PFXG_CURSOR        0x90u
#define PFXG_WAIT_T        0xA4u
#define PFXG_WAIT_ACC      0xA8u
#define PFXG_MAX           512u
static uint32_t s_gs_g[PFXG_MAX], s_gs_cur[PFXG_MAX];
static unsigned s_gs_n;
static unsigned long s_st_gshold, s_st_gswait, s_st_gsover;

static void pfxscript_pre(void)
{
    uint32_t g, n = 0;
    s_gs_n = 0;
    if (!s_pfxscript || !s_pf_on || s_pf_m)
        return;
    for (g = HU32(PFXG_HEAD_VA); FX_VA_OK(g) && n < 4096u; g = HU32(g + PFXG_NEXT), n++) {
        uint32_t c = HU32(g + PFXG_CURSOR);
        if (!c)
            continue;
        if (s_gs_n == PFXG_MAX) {
            s_st_gsover++;
            break;
        }
        s_gs_g[s_gs_n] = g;
        s_gs_cur[s_gs_n] = c;
        s_gs_n++;
        HU32(g + PFXG_CURSOR) = 0;
    }
}

static void pfxscript_post(void)
{
    uint32_t g, n = 0, dt = pfx_dt();
    unsigned i = 0, q;
    if (!s_gs_n)
        return;
    for (g = HU32(PFXG_HEAD_VA); FX_VA_OK(g) && n < 4096u; g = HU32(g + PFXG_NEXT), n++) {
        uint32_t c;
        for (q = i; q < s_gs_n && s_gs_g[q] != g; q++)
            ;
        if (q == s_gs_n) {
            for (q = 0; q < i && s_gs_g[q] != g; q++)
                ;
            if (q == i)
                continue;
        } else
            i = q + 1u;
        if (HU32(g + PFXG_CURSOR) != 0)
            continue;                          /* set again inside the frame: the game's */
        c = s_gs_cur[q];
        HU32(g + PFXG_CURSOR) = c;
        s_st_gshold++;
        if (dt && FX_VA_OK(c) && HU8(c) == 5u && HF32(g + PFXG_WAIT_T) != 0.0f) {
            HU32(g + PFXG_WAIT_ACC) = pgb_add(dt, HU32(g + PFXG_WAIT_ACC));
            s_st_gswait++;
        }
    }
    s_gs_n = 0;
}

/* ---- particles end on the step grid (lane fx-death; call RC4-P3) -------- *
 * A frame-rate trap in the particles' time-based lives (seen in code):
 * PFX_Update 0x1B980 ends a particle at the top of its loop, before anything
 * else is done with it, when its age (+0x10, the frame times added since its
 * birth: fld dt; fadd age; fstp at 0x1C003) has reached its life (+0x14:
 * fld age; fcomp life at 0x1BD37, i.e. !(age < life)); every behaviour skips
 * it the same way (e.g. 0x24B61). At 30 fps a drawn frame is a game step,
 * so a particle born for step s+1 lives through whole steps: it gets its
 * last x k at step s+J (J = the steps whose frame finds it younger than its
 * life: ceil(30 x life)), is drawn and sampled until step s+J+1's frame ends
 * it. B+ (call A10) keeps the age live, and at the high frame rate it
 * reaches the life between two steps, a few ms after it, so the particle
 * was gone before step s+J+1 and, when born early in its step, before step
 * s+J: Torp_spray2's oldest puffs at 1.05^11, Original's at 1.05^12 (rc4's
 * parity, call RC4-P3). Remastered ends each particle where Original's
 * 30 Hz step would:
 *   - the model, by B+'s birth mapping: a particle born in the drawn frames
 *     after step s's frame, up to and including step s+1's, is Original's
 *     frame-(s+1) newborn. Its age as that newborn at a later step's frame
 *     is the frame times from the end of step s's frame to the end of that
 *     frame: o (the frame times before its birth frame since step s's frame,
 *     summed as an age is) plus its own age then (b). A drawn frame that
 *     follows m >= 1 steps ends it when o + b, at the last step's frame
 *     before this one, has reached its life (the sum in double: each term a
 *     float, so exact), as Original's frame ends one whose age before the
 *     frame has; a drawn frame with no step ends none. Its age is the
 *     game's, so a particle frozen by its emitter's pause window keeps b;
 *   - the game's own test is made to agree, for the frame only: a particle
 *     the model keeps but whose age has reached its life gets a life just
 *     above its age (the next float), and one the model ends but the game
 *     wouldn't gets its age for its life. After the frame the life is put
 *     back where it still holds the value written (dead particles are off
 *     the list). The age is never written, so it stays live and exact;
 *   - a particle the model can't place (alive when tracking began, or its
 *     age set by the game's code: PFX_ExpireParticles 0x1B3E0 writes 1.0)
 *     ends by the game's own test, but only at a step's frame: between
 *     steps it is kept;
 *   - a stopped emitter (PFX_Stop 0x1B440 zeroes its live count +0x1EC and
 *     its count to emit +0x2C0; a dying particle stops its sub-emitters at
 *     0x1C0A8) is released at the start of its next update (0x1BD7B), one
 *     drawn frame later: Original's next step, a few ms at the high frame
 *     rate. On a drawn frame with no step it is held instead: its update
 *     returns at the pause-window test (0x1B9B3-0x1B9D7: time inside
 *     (+0x204, +0x208)), which reads (-FLT_MAX, FLT_MAX) for the frame, so
 *     nothing of it moves, as at 30 fps, and it goes at the next step's
 *     frame. Its window is put back after the frame where it still holds
 *     the values written. This part is off (BW_HIFPS_PFXSTOPHOLD=1 turns it
 *     on): in every scene tried the stops came from game steps, whose next
 *     drawn frame is a step's anyway, and no emitter had sub-emitters (+0
 *     bit 21), so it was never reached, and a hook that writes game memory
 *     ships only after a scene has exercised it (au-fx's T2 lesson).
 * So per step the particles alive (their count, the x k's their scale got,
 * their trail) and the emitters released are Original's; BW_PFXCHECK checks
 * the deaths and the releases against its own model. Between steps a
 * particle past its life is drawn with its envelopes at their end (age over
 * life just under 1; with a fade-out, about transparent), as Original's last
 * frame shows its last look until the step. Frozen emitters (+0x1F0 bit 0,
 * frame time 0) are left alone; while the game isn't running nothing is
 * done and tracking starts again. Only while the high frame rate is on
 * (never in Original); BW_HIFPS_PFXDEATH=0 is the undo. */
#define PFX_FLAGS2         0x004u
#define PFX_KEEP_FLAG      0x00002000u      /* +4: kept when done (0x1BD95) */
#define PFX_LIVE           0x1ECu
#define PFX_TOEMIT         0x2C0u
#define PFX_WIN_START      0x204u
#define PFX_WIN_END        0x208u
#define PD_HN              16384u           /* tracking table slots (a power of 2), half used at most: the
                                               particle pool (300 blocks of 0x3D8 bytes, PFX_BlockPoolInit
                                               0x1AE40) holds ~6000 at the smallest; small to stay in cache */
#define PD_RN              16384u           /* lives written in one frame */
#define PD_EN              512u             /* stopped emitters held in one frame */
typedef struct { uint32_t p, e, age, o, b, fl; } pd_ent_t;    /* fl: 1 placed (o), 2 b */
static pd_ent_t *s_pd_t[2];
static uint32_t *s_pd_used[2], s_pd_nused[2];
static unsigned s_pd_cur;
static int s_pd_alloc, s_pd_have, s_pd_pre_ok;
static uint32_t s_pd_pre;                    /* float bits: frame times since the last step's frame */
static uint32_t s_pdr_p[PD_RN], s_pdr_e[PD_RN], s_pdr_l[PD_RN], s_pdr_w[PD_RN];
static unsigned s_pdr_n;
static uint32_t s_pdh_e[PD_EN], s_pdh_s[PD_EN], s_pdh_t[PD_EN];
static unsigned s_pdh_n;
static unsigned long s_st_pd_keep, s_st_pd_end, s_st_pd_unpl, s_st_pd_hold, s_st_pd_over, s_st_pd_untr;

static uint32_t pd_slot(uint32_t p)
{
    return ((p * 2654435761u) >> 18) & (PD_HN - 1u);
}

static const pd_ent_t *pd_find(const pd_ent_t *t, uint32_t p)
{
    uint32_t i = pd_slot(p), j;
    for (j = 0; j < 64u; j++, i = (i + 1u) & (PD_HN - 1u)) {
        if (t[i].p == p)
            return &t[i];
        if (!t[i].p)
            return NULL;
    }
    return NULL;
}

static pd_ent_t *pd_put(unsigned w, uint32_t p)
{
    pd_ent_t *t = s_pd_t[w];
    uint32_t i = pd_slot(p), j;
    if (s_pd_nused[w] >= PD_HN / 2u)
        return NULL;
    for (j = 0; j < 64u; j++, i = (i + 1u) & (PD_HN - 1u)) {
        if (!t[i].p) {
            t[i].p = p;
            s_pd_used[w][s_pd_nused[w]++] = i;
            return &t[i];
        }
        if (t[i].p == p)
            return &t[i];
    }
    return NULL;
}

/* before the frame, after pfxgrow_pre (which reads the lives as they are) */
static void pfxdeath_pre(void)
{
    uint32_t e, n = 0, m = s_pf_m;
    const pd_ent_t *tb = s_pd_have ? s_pd_t[s_pd_cur] : NULL;
    s_pdr_n = 0;
    s_pdh_n = 0;
    if (!s_pfxdeath || !s_pf_on)
        return;
    for (e = HU32(PFX_HEAD_VA); FX_VA_OK(e) && n < 4096u; e = HU32(e + FX_NEXT), n++) {
        uint32_t p, np = 0;
        if (HU8(e + PFX_FROZEN) & 1u)
            continue;
        if (s_pfxstophold && !m && !HU32(e + PFX_TOEMIT) && !HU32(e + PFX_LIVE)
                && !(HU32(e + PFX_FLAGS2) & PFX_KEEP_FLAG)) {
            /* stopped: its update would release it at once; held to the step */
            if (s_pdh_n == PD_EN) {
                s_st_pd_over++;
                continue;
            }
            s_pdh_e[s_pdh_n] = e;
            s_pdh_s[s_pdh_n] = HU32(e + PFX_WIN_START);
            s_pdh_t[s_pdh_n] = HU32(e + PFX_WIN_END);
            s_pdh_n++;
            HU32(e + PFX_WIN_START) = 0xFF7FFFFFu;
            HU32(e + PFX_WIN_END) = 0x7F7FFFFFu;
            s_st_pd_hold++;
            continue;
        }
        for (p = HU32(e + PFX_PARTS); FX_VA_OK(p) && np < 4096u; p = HU32(p), np++) {
            union { float f; uint32_t u; } a, l;
            int kill, want;
            a.u = HU32(p + PFX_AGE);
            l.u = HU32(p + PFX_LIFE);
            if (!(l.f > 0.0f) || !(l.f < 3.0e38f) || !(a.f >= 0.0f) || !(a.f < 3.0e38f))
                continue;                      /* the game's own (no life to align) */
            kill = !(a.f < l.f);
            if (!m)
                want = 0;                      /* no step: none ends */
            else {
                const pd_ent_t *pe = tb ? pd_find(tb, p) : NULL;
                if (pe && pe->e == e && pe->age == a.u && (pe->fl & 1u)) {
                    union { float f; uint32_t u; } o, b;
                    o.u = pe->o;
                    b.u = pe->b;
                    want = (pe->fl & 2u) && (double)o.f + (double)b.f >= (double)l.f;
                } else {
                    want = kill;               /* not placed: the game's own test, at the step */
                    if (kill)
                        s_st_pd_unpl++;
                }
            }
            if (want == kill)
                continue;
            if (s_pdr_n == PD_RN) {
                s_st_pd_over++;
                continue;
            }
            s_pdr_p[s_pdr_n] = p;
            s_pdr_e[s_pdr_n] = e;
            s_pdr_l[s_pdr_n] = l.u;
            s_pdr_w[s_pdr_n] = want ? a.u : a.u + 1u;   /* its age, or the next float above it */
            HU32(p + PFX_LIFE) = s_pdr_w[s_pdr_n];
            s_pdr_n++;
            if (want)
                s_st_pd_end++;
            else
                s_st_pd_keep++;
        }
    }
}

/* after the frame, with pfe_build's list, before B+'s own puts-back */
static void pfxdeath_post(void)
{
    union { float f; uint32_t u; } dt;
    uint32_t e, n = 0, m = s_pf_m, w, k;
    const pd_ent_t *prev;
    unsigned i = 0;
    /* the lives written, back into the particles still on their lists (in
     * list order per emitter, as written; newborns come first, unmatched) */
    while (i < s_pdr_n) {
        uint32_t p, np = 0;
        unsigned j = i, end = i;
        e = s_pdr_e[i];
        while (end < s_pdr_n && s_pdr_e[end] == e)
            end++;
        if (pfe_live(e))
            for (p = HU32(e + PFX_PARTS); FX_VA_OK(p) && np < 4096u && j < end; p = HU32(p), np++) {
                unsigned q;
                for (q = j; q < end && s_pdr_p[q] != p; q++)
                    ;
                if (q == end)
                    continue;
                if (HU32(p + PFX_LIFE) == s_pdr_w[q])
                    HU32(p + PFX_LIFE) = s_pdr_l[q];
                j = q + 1u;
            }
        i = end;
    }
    s_pdr_n = 0;
    for (i = 0; i < s_pdh_n; i++) {
        e = s_pdh_e[i];
        if (pfe_live(e) && HU32(e + PFX_WIN_START) == 0xFF7FFFFFu && HU32(e + PFX_WIN_END) == 0x7F7FFFFFu) {
            HU32(e + PFX_WIN_START) = s_pdh_s[i];
            HU32(e + PFX_WIN_END) = s_pdh_t[i];
        }
    }
    s_pdh_n = 0;
    /* follow every particle to the next frame (its age is the last one plus
     * the frame time, or the same when its update didn't run); a newborn's
     * age is the frame time, and it is placed by the frame times before it
     * since the last step's frame. At a step's frame each placed particle's
     * age is its b. */
    dt.u = pfx_dt();
    if (!s_pfxdeath || !s_pf_on || !(dt.f > 0.0f)) {
        s_pd_have = 0;                         /* tracking starts again at the next running frame */
        s_pd_pre_ok = 0;
        return;
    }
    if (!s_pd_alloc) {
        s_pd_t[0] = (pd_ent_t *)calloc(PD_HN, sizeof(pd_ent_t));
        s_pd_t[1] = (pd_ent_t *)calloc(PD_HN, sizeof(pd_ent_t));
        s_pd_used[0] = (uint32_t *)malloc(PD_HN / 2u * sizeof(uint32_t));
        s_pd_used[1] = (uint32_t *)malloc(PD_HN / 2u * sizeof(uint32_t));
        s_pd_alloc = s_pd_t[0] && s_pd_t[1] && s_pd_used[0] && s_pd_used[1] ? 1 : -1;
        if (s_pd_alloc < 0)
            fprintf(stderr, "[BW] hifps: particle death tracking: no memory, the game's own deaths\n");
    }
    if (s_pd_alloc < 0)
        return;
    prev = s_pd_have ? s_pd_t[s_pd_cur] : NULL;
    w = s_pd_cur ^ 1u;
    for (k = 0; k < s_pd_nused[w]; k++)
        s_pd_t[w][s_pd_used[w][k]].p = 0;
    s_pd_nused[w] = 0;
    for (e = HU32(PFX_HEAD_VA); FX_VA_OK(e) && n < 4096u; e = HU32(e + FX_NEXT), n++) {
        uint32_t p, np = 0;
        for (p = HU32(e + PFX_PARTS); FX_VA_OK(p) && np < 4096u; p = HU32(p), np++) {
            uint32_t age = HU32(p + PFX_AGE), fl = 0, o = 0, b = 0;
            const pd_ent_t *pe = prev ? pd_find(prev, p) : NULL;
            pd_ent_t *ce;
            if (pe && pe->e == e && (age == pe->age || age == pgb_add(dt.u, pe->age))) {
                fl = pe->fl;
                o = pe->o;
                b = pe->b;
            } else if (dt.u && age == dt.u && s_pd_pre_ok) {
                fl = 1u;                       /* born in this frame */
                o = s_pd_pre;
            }
            if (m && (fl & 1u)) {
                b = age;
                fl |= 2u;
            }
            ce = pd_put(w, p);
            if (!ce) {
                s_st_pd_untr++;
                continue;
            }
            ce->e = e;
            ce->age = age;
            ce->o = o;
            ce->b = b;
            ce->fl = fl;
        }
    }
    s_pd_cur = w;
    s_pd_have = 1;
    if (m) {
        s_pd_pre = 0;                          /* 0.0: the next step's frames start here */
        s_pd_pre_ok = 1;
    } else if (s_pd_pre_ok && dt.u)
        s_pd_pre = pgb_add(dt.u, s_pd_pre);
}

/* ---- bolt jitter per game step (lane au-fx, the wake audit's T2) --------- *
 * A frame-rate trap in two particle behaviours (seen in code): Slot6
 * 0x248F0 (the electric bolts: Sprk_Goldbolt in the Stinger's and the Wave
 * Gun's fire groups, DC_charge_ben3) and Lightning_UpdateBolt 0x253B0 (the
 * built-in BUILTIN_Lightning, the bolt of a Stinger hit: seen in this lane's
 * Stinger runs; no Effects.pak descriptor). Per particle and per call,
 * i.e. per drawn frame: if its regen timer (the field whose offset is at
 * emitter+0x27C) is above 0 (the float at 0x174820) it goes down by the frame
 * time; otherwise the bolt's kinks (the points at +0x26C) are drawn again
 * with rand() and the timer is set to the emitter's period (+0xCC), with no
 * frame time (0x24951-0x24A5E, 0x2561E-0x25731). Both kinds above have a
 * period of 0, so they re-roll on every drawn frame: 30 times a second on
 * the Xbox, 120-360 at the high frame rate, where the crackle turns to noise.
 * At 30 fps a bolt with period P re-rolls every 1 + ceil(30 P) steps.
 * Remastered keeps that, per game step: on a drawn frame with no new step,
 * every such particle whose timer is out gets a timer just above 0, so it
 * doesn't re-roll and the behaviour takes it below 0 again (frame time); on
 * a frame with a new step, the emitter's period reads (ceil(30 P) + 0.5)/30
 * minus the frame time, so a bolt re-rolled now is out again half a step
 * before its step comes round, as at 30 fps, and the period is put back after
 * the frame where it still holds the value written. A particle born on a
 * frame with no step rolls at birth as before (it isn't there yet when the
 * timers are looked at). The rand() calls are on the render seed (s_randsplit)
 * at the high frame rate, so fewer of them leave the game's own sequence as
 * it was. Frames while the game isn't running keep the game's own per-frame
 * behaviour (B+, call A10: a frame of 1/30 s or longer is held too when it
 * follows no step). Only while the high frame rate is on (never in
 * Original); BW_HIFPS_PFXJITTER=0 is the undo. */
#define PFX_BEH            0x220u                  /* the behaviour (PFX_BindHandlers 0x1B4A0) */
#define PFX_BEH_SLOT6      0x000248F0u
#define PFX_BEH_LIGHTNING  0x000253B0u
#define PFX_JIT_TIMER      0x27Cu                  /* offset of the particle's regen timer */
#define PFX_JIT_PERIOD     0xCCu
#define PFX_JIT_FIELD      0x00080000u             /* flag bit 19: the particles have the timer field */
#define PFX_PARTS          0x1E0u                  /* the first particle; next at +0 */
#define PFX_JIT_MAX        256
static uint32_t s_pj_va[PFX_JIT_MAX], s_pj_p[PFX_JIT_MAX], s_pj_wrote[PFX_JIT_MAX];
static unsigned s_pj_n;
static unsigned long s_st_pjtick, s_st_pjhold, s_st_pjover;

static void pfxjit_apply(uint32_t dt_ms)
{
    uint32_t e, n = 0;
    float dt = (float)dt_ms * 0.001f;
    int tick = s_pf_m != 0;
    s_pj_n = 0;
    /* B+ (call A10): per step on every running frame, also one of 1/30 s or
     * more (a long frame with no step re-rolled once more before) */
    if (!s_pfxjit || !s_pf_on)
        return;
    for (e = HU32(PFX_HEAD_VA); FX_VA_OK(e) && n < 4096u; e = HU32(e + FX_NEXT), n++) {
        uint32_t beh = HU32(e + PFX_BEH), off, p, np = 0;
        if (beh != PFX_BEH_SLOT6 && beh != PFX_BEH_LIGHTNING)
            continue;
        /* Only an emitter whose particles have the timer field (flag bit 19,
         * PFX_ComputeParticleLayout). Without it the offset at +0x27C is 0
         * and the behaviour's "timer" is the particle's next pointer (seen:
         * the Stinger hit's BUILTIN_Lightning, flags 0x8609, one particle):
         * a NULL next reads as 0.0 and the period 0.0 is written back, so the
         * game is unharmed at its own values, but any other value written
         * there breaks the particle list (seen: crashes in mesh drawing at
         * 60 fps before this test). The T3 wrapper keeps that bolt per
         * step instead (bw_hifps_bolt_hold). */
        off = HU32(e + PFX_JIT_TIMER);
        if (!(HU32(e) & PFX_JIT_FIELD) || !off || off >= 0x1000u)
            continue;
        if (tick) {
            union { float f; uint32_t u; } per, w;
            float c;
            if (s_pj_n == PFX_JIT_MAX) {
                s_st_pjover++;
                break;
            }
            per.u = HU32(e + PFX_JIT_PERIOD);
            if (!(per.f >= 0.0f && per.f < 100.0f))
                continue;                          /* not a period (or NaN): leave it */
            c = ceilf(per.f * 30.0f - 1e-3f);
            if (c < 0.0f)
                c = 0.0f;
            w.f = (c + 0.5f) / 30.0f - dt;
            s_pj_va[s_pj_n] = e;
            s_pj_p[s_pj_n] = per.u;
            s_pj_wrote[s_pj_n] = w.u;
            HU32(e + PFX_JIT_PERIOD) = w.u;
            s_pj_n++;
            s_st_pjtick++;
            continue;
        }
        for (p = HU32(e + PFX_PARTS); FX_VA_OK(p) && np < 4096u; p = HU32(p), np++) {
            if (HF32(p + off) <= 0.0f) {           /* out (not NaN): it would re-roll this frame */
                HF32(p + off) = 1e-6f;
                s_st_pjhold++;
            }
        }
    }
}

static void pfxjit_restore(void)
{
    unsigned i;
    for (i = 0; i < s_pj_n; i++)
        if (HU32(s_pj_va[i] + PFX_JIT_PERIOD) == s_pj_wrote[i])
            HU32(s_pj_va[i] + PFX_JIT_PERIOD) = s_pj_p[i];
    s_pj_n = 0;
}

/* ---- trail ribbons per game step (lane au-fx, the wake audit's T1) ------- *
 * A frame-rate trap (seen in code): behaviours 1, 3, 4, 5, 7, 8 and 9 (table
 * 0x187B10) call PFX_ShiftTrailHistory 0x23F80 for every particle on every
 * call, i.e. per drawn frame. With a trail of n + 1 points (+0xA; the
 * particle's history at +[emitter+0x258], 12 bytes a point, newest first,
 * an unused point's x = FLT_MAX) it widens the emitter's bounds by each point
 * (PFX_ExpandBounds 0x23EE0), moves points 0..n-1 to 1..n, puts the
 * particle's position in point 0 and widens by it; with flag 0x800 a colour
 * history (+[emitter+0x25C], 4 bytes a point) moves the same way and takes
 * the particle's colour (+[emitter+0x238]). So a ribbon spans n drawn
 * frames: n steps at 30 fps, a quarter to a twelfth of that at 120-360 fps
 * (SHLD_main1 1.07 s at 30 fps, 0.09 s at 360). The renderer reads +0xA as
 * its point count, so the length can't be rewritten instead.
 * Remastered keeps the points per game step (B+, call A10): the wrapper
 * sub_00023F80 (src/recomp_manual.c) asks here first.
 *   - Original takes no point at a particle's birth frame: every one of
 *     these behaviours calls the shift only for a particle whose age isn't 0
 *     (seen, e.g. 0x24C3D-0x24C5E), so after n steps a ribbon has n - 1.
 *   - A frame that follows m steps shifts m times, the first m - 1 here (the
 *     body's moves without the bounds: the points they drop aren't drawn)
 *     and the last by the game's own body. A particle born since the last
 *     step's frame (B+'s birth mapping: Original's newborn at this frame's
 *     first step) shifts m - 1 times, none when m is 1; one born in this
 *     frame isn't called (age 0). It is told by its age: below the frame
 *     times summed since the last step's frame (s_pf_since, the same float
 *     additions as the age's).
 *   - A drawn frame with no new game step stores nothing. For the drawing
 *     only, the points are shifted as the next step will (point 0 the
 *     particle where it is now, with its colour), and when the ribbon is
 *     full its last point is drawn the step fraction a (step_alpha) of the
 *     way from the oldest sample to the next, so the ribbon spans its n
 *     steps at every frame with its tail moving smoothly between the steps'
 *     samples; the bounds are widened by every point in use, as the shift
 *     would have (they're made again from nothing each frame). After the
 *     frame every point and colour is put back (trail_restore), so the
 *     history holds only the steps' samples. A particle born since the last
 *     step's frame has none, nor gets one drawn.
 * So per step the stored ribbon is Original's: one sample per step after the
 * birth step, newest first, at most n + 1 (BW_PFXCHECK checks the count);
 * at the steps it is drawn as stored. Tube_UpdateParticles 0x24E70 passes
 * through: its own timer (frame time) decides its shifts. Frames while the
 * game isn't running keep the game's own shift. Only while the high frame
 * rate is on (never in Original); BW_HIFPS_PFXTRAIL=0 is the undo. */
#define PFX_BEH_TUBE       0x00024E70u
#define PFX_EXPAND_BOUNDS  0x00023EE0u
#define TRH_N              8192u
#define TRH_POOL           (1u << 20)              /* dwords: points and colours put back */
static uint32_t s_trh_p[TRH_N], s_trh_e[TRH_N], s_trh_at[TRH_N], s_trh_len[TRH_N];
static uint32_t *s_trh_pool, s_trh_used;
static unsigned s_trh_n;
static unsigned long s_st_trhold, s_st_trextra, s_st_trrest, s_st_trover, s_st_trlate;

/* the body's moves (0x23FB0-0x2403A) without the bounds */
static void trail_shift(uint32_t p, uint32_t e, uint32_t h, int n)
{
    int i;
    for (i = n - 1; i >= 0; i--) {
        uint32_t s = h + 12u * (uint32_t)i;
        HU32(s + 12u) = HU32(s);
        HU32(s + 16u) = HU32(s + 4u);
        HU32(s + 20u) = HU32(s + 8u);
    }
    HU32(h) = HU32(p + 4u);
    HU32(h + 4u) = HU32(p + 8u);
    HU32(h + 8u) = HU32(p + 0xCu);
    if ((HU32(e) & 0x800u) && HU32(e + 0x25Cu) < 0x1000u && HU32(e + 0x238u) < 0x1000u) {
        uint32_t c = p + HU32(e + 0x25Cu);
        for (i = n - 1; i >= 0; i--)
            HU32(c + 4u * (uint32_t)i + 4u) = HU32(c + 4u * (uint32_t)i);
        HU32(c) = HU32(p + HU32(e + 0x238u));
    }
}

/* sub_00023F80's question (ecx = the particle, edx = its emitter): 1 if this
 * call was held here (the wrapper then returns), 0 for the game's own body. */
int bw_hifps_trail_hold(void)
{
    uint32_t p = g_ecx, e = g_edx, h, pt, need;
    int n, i, col, full, late;
    bw_gregs_t r;
    float tail[3] = { 0.0f, 0.0f, 0.0f };
    if (!s_pf_on || !s_pfxtrail || !FX_VA_OK(p) || !FX_VA_OK(e) || HU32(e + PFX_BEH) == PFX_BEH_TUBE)
        return 0;
    n = (int)(int16_t)HU16(e + 0xAu) - 1;
    if (n <= 0 || n > 255 || HU32(e + 0x258u) >= 0x1000u)
        return 0;                              /* no trail: the body does nothing either */
    h = p + HU32(e + 0x258u);
    col = (HU32(e) & 0x800u) && HU32(e + 0x25Cu) < 0x1000u && HU32(e + 0x238u) < 0x1000u;
    /* born after the last step's frame (its age, which steps by the same
     * frame times, is below their sum since that frame): Original's newborn
     * at the next step, which takes no point there (the behaviours call the
     * shift only for a particle whose age isn't 0) */
    late = s_pf_since_ok && HF32(p + 0x10u) < s_pf_since;
    if (s_pf_m) {
        int runs = (int)(s_pf_m > 8u ? 8u : s_pf_m) - late;
        if (runs <= 0) {
            s_st_trlate++;
            return 1;                          /* its birth step: no point */
        }
        for (i = 1; i < runs; i++)
            trail_shift(p, e, h, n);
        s_st_trextra += (unsigned long)(runs - 1);
        return 0;                              /* the body makes the last shift */
    }
    if (late) {
        s_st_trlate++;
        return 1;                              /* nothing yet, nor at the next step */
    }
    need = 3u * (uint32_t)(n + 1) + (uint32_t)(n + 1);
    if (!s_trh_pool)
        s_trh_pool = (uint32_t *)malloc(TRH_POOL * sizeof(uint32_t));
    if (!s_trh_pool || s_trh_n == TRH_N || s_trh_used + need > TRH_POOL) {
        s_st_trover++;                         /* can't be put back: the game's own shift */
        return 0;
    }
    s_trh_p[s_trh_n] = p;
    s_trh_e[s_trh_n] = e;
    s_trh_at[s_trh_n] = s_trh_used;
    s_trh_len[s_trh_n] = (uint32_t)(n + 1);
    for (i = 0; i < 3 * (n + 1); i++)
        s_trh_pool[s_trh_used + (uint32_t)i] = HU32(h + 4u * (uint32_t)i);
    for (i = 0; i <= n; i++)
        s_trh_pool[s_trh_used + 3u * (uint32_t)(n + 1) + (uint32_t)i] =
            col ? HU32(p + HU32(e + 0x25Cu) + 4u * (uint32_t)i) : 0u;
    s_trh_used += need;
    s_trh_n++;
    /* full (every point a sample): the drawn tail goes the step fraction
     * from the oldest sample (point n) to the next (point n - 1) */
    full = HU32(h + 12u * (uint32_t)n) != 0x7F7FFFFFu;
    if (full)
        for (i = 0; i < 3; i++) {
            float o = HF32(h + 12u * (uint32_t)n + 4u * (uint32_t)i);
            float y = HF32(h + 12u * (uint32_t)(n - 1) + 4u * (uint32_t)i);
            tail[i] = o + (y - o) * s_pf_alpha;
        }
    trail_shift(p, e, h, n);                   /* the next step's ribbon, its head the particle now */
    if (full)
        for (i = 0; i < 3; i++)
            HF32(h + 12u * (uint32_t)n + 4u * (uint32_t)i) = tail[i];
    bw_gregs_save(&r);
    for (i = 0, pt = h; i <= n; i++, pt += 12u)
        if (HU32(pt) != 0x7F7FFFFFu)
            bw_guest_call("[BW] hifps trail:", PFX_EXPAND_BOUNDS, e, 1, &pt);
    bw_gregs_restore(&r);
    s_st_trhold++;
    return 1;
}

/* After the frame: the stored points and colours back into every held
 * particle still alive, i.e. still on its emitter's list (the held ones are
 * in list order per emitter, as the behaviour walked them; the dead are
 * gone from the list, never reused inside the frame). */
static void trail_restore(void)
{
    unsigned i = 0;
    while (i < s_trh_n) {
        uint32_t e = s_trh_e[i], p, np = 0;
        unsigned j = i, end = i;
        while (end < s_trh_n && s_trh_e[end] == e)
            end++;
        if (pfe_live(e) && HU32(e + 0x258u) < 0x1000u) {
            uint32_t to = HU32(e + 0x258u), co = HU32(e + 0x25Cu);
            int col = (HU32(e) & 0x800u) && co < 0x1000u;
            for (p = HU32(e + PFX_PARTS); FX_VA_OK(p) && np < 4096u && j < end; p = HU32(p), np++) {
                unsigned q;
                uint32_t k, len, at;
                for (q = j; q < end && s_trh_p[q] != p; q++)
                    ;
                if (q == end)
                    continue;                  /* not held (born after the behaviour ran) */
                len = s_trh_len[q];
                at = s_trh_at[q];
                if ((uint32_t)(int16_t)HU16(e + 0xAu) == len) {
                    for (k = 0; k < 3u * len; k++)
                        HU32(p + to + 4u * k) = s_trh_pool[at + k];
                    if (col)
                        for (k = 0; k < len; k++)
                            HU32(p + co + 4u * k) = s_trh_pool[at + 3u * len + k];
                    s_st_trrest++;
                }
                j = q + 1u;
            }
        }
        i = end;
    }
    s_trh_n = 0;
    s_trh_used = 0;
}

/* ---- the bolt's bend per game step (lane au-fx, the wake audit's T3) ----- *
 * Lightning_UpdateBolt 0x253B0 (the Stinger's hit bolt, BUILTIN_Lightning;
 * ecx = the emitter) walks the bolt's phase (+0x2A8) on every call, i.e. per
 * drawn frame, for each live particle (seen, 0x25508-0x25562): +0.001 (the
 * float at 0x175518) and, when rand() / 32767 is 0.7 (0x175650) or more, a
 * kick of (rand() / 32767 - 0.5) x 3.0 (0x1753A0). The phase bends the
 * bolt's curve (sines of it move its control points, 0x255A9-0x2560E), and
 * the curve is sampled into the bolt only when its points are rolled again
 * (T2's timer). So at 120-360 fps the bend walked 4-12 times as far between
 * two shapes as at 30 fps once T2 holds the rolls to one a step; without
 * T2's fix it walked 4-12 times as fast. The constants are shared data, so
 * they can't be changed for one function. Remastered keeps the walk per game
 * step: the wrapper sub_000253B0 (src/recomp_manual.c) asks here before the
 * game's body and, on a drawn frame with no new step (as T1's hold), the
 * phase is put back after the body. The rest of the body runs as before:
 * the control points it makes from the walked phase sit in its own block
 * (+0x2F4), which only this function reads (and the emitter's create and
 * release), and are used in the same call only to roll the points, which T2
 * holds on such a frame. The rand() calls stay (render seed). Only while
 * the high frame rate is on (never in Original); BW_HIFPS_BOLTPHASE=0 is the
 * undo.
 * T2 for a bolt without a timer field (lane au-fx): the Stinger hit's
 * BUILTIN_Lightning (flags 0x8609: no field 19) re-rolls whenever its
 * "timer", which is then the particle's next pointer (NULL, 0.0), is out,
 * i.e. on every call, and T2's timer rewrite can't be used on it (see
 * pfxjit_apply). On a drawn frame with no new step this wrapper instead
 * keeps the bolt as the last step left it: each particle's points (the
 * field at the offset in +0x26C) and its strip (the trail field, +0x258), +0xA
 * points of 12 bytes each, are saved before the body and put back after it
 * (BW_HIFPS_PFXJITTER=0 is that part's undo). The body still widens the
 * bounds around the bolt it made, which lies where the kept one is, give or
 * take a frame's motion (inferred). The particles are the emitter's list
 * (+0x1E0) and the list the body walks ([[+0x2F4]+0x64]); seen to be the
 * same particle for this bolt (inferred from BW_FXLOG_ALL's re-roll count). */
#define PFX_BOLT_PHASE     0x2A8u
#define BOLT_KEEP_P        8
#define BOLT_KEEP_PTS      64
static unsigned long s_st_bolthold, s_st_boltkeep;
static uint32_t s_bk_p[BOLT_KEEP_P], s_bk_n, s_bk_tn, s_bk_opt, s_bk_ost;
static uint32_t s_bk_pts[BOLT_KEEP_P][BOLT_KEEP_PTS * 3], s_bk_str[BOLT_KEEP_P][BOLT_KEEP_PTS * 3];

static void bolt_keep_add(uint32_t p)
{
    uint32_t i;
    if (!FX_VA_OK(p) || s_bk_n >= BOLT_KEEP_P)
        return;
    for (i = 0; i < s_bk_n; i++)
        if (s_bk_p[i] == p)
            return;
    for (i = 0; i < s_bk_tn * 3u; i++) {
        s_bk_pts[s_bk_n][i] = HU32(p + s_bk_opt + 4u * i);
        s_bk_str[s_bk_n][i] = HU32(p + s_bk_ost + 4u * i);
    }
    s_bk_p[s_bk_n++] = p;
}

int bw_hifps_bolt_hold(uint32_t emitter, uint32_t *phase)
{
    uint32_t fl, p, n;
    s_bk_n = 0;
    if (!s_hold_frame || !FX_VA_OK(emitter) || (!s_boltphase && !s_pfxjit))
        return 0;
    *phase = HU32(emitter + PFX_BOLT_PHASE);
    fl = HU32(emitter);
    s_bk_tn = (uint32_t)(int32_t)(int16_t)HU16(emitter + 0xAu);
    s_bk_opt = HU32(emitter + 0x26Cu);
    s_bk_ost = HU32(emitter + 0x258u);
    if (s_pfxjit && !(fl & PFX_JIT_FIELD) && (fl & 0x00008000u) && (fl & 0x00000400u)
            && s_bk_tn >= 1 && s_bk_tn <= BOLT_KEEP_PTS && s_bk_opt && s_bk_opt < 0x1000u
            && s_bk_ost && s_bk_ost < 0x1000u) {
        for (p = HU32(emitter + PFX_PARTS), n = 0; FX_VA_OK(p) && n < 64u; p = HU32(p), n++)
            bolt_keep_add(p);
        p = HU32(emitter + 0x2F4u);
        if (FX_VA_OK(p))
            for (p = HU32(p + 0x64u), n = 0; FX_VA_OK(p) && n < 64u; p = HU32(p), n++)
                bolt_keep_add(p);
    }
    return 1;
}

void bw_hifps_bolt_release(uint32_t emitter, uint32_t phase)
{
    uint32_t k, i;
    if (s_boltphase) {
        HU32(emitter + PFX_BOLT_PHASE) = phase;
        s_st_bolthold++;
    }
    for (k = 0; k < s_bk_n; k++) {
        uint32_t p = s_bk_p[k];
        for (i = 0; i < s_bk_tn * 3u; i++) {
            HU32(p + s_bk_opt + 4u * i) = s_bk_pts[k][i];
            HU32(p + s_bk_ost + 4u * i) = s_bk_str[k][i];
        }
        s_st_boltkeep++;
    }
    s_bk_n = 0;
}

/* B+ (call A10): a drawn frame that follows m >= 2 steps runs the body m
 * times (the wrapper calls it m - 1 times more, with the same frame time),
 * so the bend walks and the bolt re-rolls once per step as m frames of
 * Original would. Only for a bolt without the timer field (flag 19: the
 * Stinger hit's BUILTIN_Lightning), whose body doesn't count time; one with
 * it would count the frame time m times, so it gets one walk (none in the
 * data). The number of extra calls, 0 when none. */
static unsigned long s_st_boltextra;
int bw_hifps_bolt_extra(uint32_t emitter)
{
    uint32_t m = s_pf_m > 4u ? 4u : s_pf_m;
    if (!s_pf_on || m < 2u || !s_boltphase || !FX_VA_OK(emitter) || (HU32(emitter) & PFX_JIT_FIELD))
        return 0;
    s_st_boltextra += m - 1u;
    return (int)(m - 1u);
}

/* Graphics_HandleMessage 0x00028600 (thiscall; [esp+4] = the message,
 * type at +0, dt at +4, paused at +8; type 8 = draw a frame: AnimLib_Update
 * then Graphics_RenderFrame). */
extern void sub_00028600_gen(void);
static void hifps_frame(void)
{
    uint32_t msg, ptd = 0, sim_seed = 0;
    int interp, scaled = 0, audit;
    float inc0 = 0.0f, inc1 = 0.0f, lat_w = 1.0f;
    int64_t lat_t = 0;
    if (s_fxlog_all) {
        /* BW_FXLOG_ALL (gate lane V, test aid, every edition, read-only):
         * the jitter re-rolls, counted before each drawn frame */
        msg = HU32(g_esp + 4);
        if (msg && HU32(msg) == 8)
            fxlog_all_frame();
    }
    if (g_bw_lat_on) {
        /* BW_LATLOG (lat-m, test aid, every edition): a drawn frame. In
         * Original (and with the high frame rate off) it shows the newest
         * step whole; otherwise it is logged below, once the blend is known. */
        msg = HU32(g_esp + 4);
        if (msg && HU32(msg) == 8) {
            lat_t = bw_lat_now();
            lat_pads(lat_t);
            s_lat_camw = 1.0f;
            if (!s_on)
                lat_frame(lat_t, 0, 0, 1.0f);
        }
    }
    if (g_bw_fl_on) {
        /* P2b, BW_FRAMELOG (test aid, every edition): each drawn frame */
        msg = HU32(g_esp + 4);
        if (msg && HU32(msg) == 8) {
            unsigned st = s_steps_since_draw;
            bw_fl_thread(0);
            bw_fl_event(BW_FL_G_FRAME, 0, 0, 0);
            if (!s_on) {
                sub_00028600_gen();
                bw_fl_event(BW_FL_G_FRAME_END, st, g_bw_ko_last_put, 0);
                if (lat_t)
                    lat_frame_end();
                return;
            }
        }
    }
    if (!s_on && lat_t) {
        sub_00028600_gen();
        lat_frame_end();
        return;
    }
    if (!s_on) {
        sub_00028600_gen();
        return;
    }
    msg = HU32(g_esp + 4);
    if (!msg || HU32(msg) != 8) {
        sub_00028600_gen();
        return;
    }
    interp = s_interp && s_snapshots >= 2 && HU32(GAME_VA + GAME_STATE) == 0;
    audit = interp && s_steps_since_draw == 0 && s_audit != 0;
    if (audit)
        audit_pre();
    s_draw_ms = s_latmode && interp ? jit_qpc_ms() : 0.0;   /* gfx-lat11: the horizon's draw */
    if (interp) {
        float a = step_alpha();
        s_st_alpha += a;
        s_aim_a = a;                               /* BW_AIMLOG */
        pred_ahead_follow();                       /* gfx-lat11: BW_HIFPS_PREDICT_AHEAD=auto */
        apply_interp(a);
        /* lat-p: with every node predicted, the water goes the same way, so
         * the hulls sit in the swell they float on */
        apply_water(s_pred_scope >= PRED_ALL && s_pred_water ? a + s_pred_now : a);
        hud_replay();
        lat_w = s_lat_nodew;                       /* t, or the predicted weight (lat-p) */
    }
    if (lat_t)
        lat_frame(lat_t, 1, interp, lat_w);
    if (s_randsplit && (ptd = ptd_va()) != 0) {
        sim_seed = HU32(ptd + PTD_SEED);
        if (!s_have_render_seed) {
            s_render_seed = sim_seed ^ 0x5EED1234u;
            s_have_render_seed = 1;
        }
        HU32(ptd + PTD_SEED) = s_render_seed;
    }
    /* The ocean's shimmer: Ocean_FillVertices adds a constant (level data,
     * 0x18F558/0x18F55C) to two angles per call, i.e. per drawn frame, with no
     * frame time (seen in its code, 0x37B81). At 120 fps it would shimmer four
     * times as fast. For this frame the increments are scaled by the number
     * of game steps since the last drawn frame, which is what Original at
     * 30 fps does (one frame, one step, one increment): the shimmer keeps its
     * speed and its 30 Hz cadence, and on a frame with no new step the
     * ocean's vertices come out exactly as on the frame before, which lets
     * the batch cache below reuse them. While paused (no steps) it goes by
     * the frame time instead, as Original's paused frames do. The increments
     * are put back after the frame. BW_HIFPS_OCEANRATE=0: per drawn frame. */
    if (s_oceanrate) {
        uint32_t dt = HU32(msg + 4);
        /* With the water blended (below) the shimmer follows the frame
         * time too, so it is smooth rather than stepped. */
        float k = HU32(GAME_VA + GAME_STATE) == 0 && !s_water_on_frame
                  ? (float)(s_steps_since_draw > 4u ? 4u : s_steps_since_draw)
                  : (float)(dt > 100u ? 100u : dt) * (30.0f / 1000.0f);
        inc0 = HF32(OCEAN_SCROLL_INC0);
        inc1 = HF32(OCEAN_SCROLL_INC1);
        HF32(OCEAN_SCROLL_INC0) = inc0 * k;
        HF32(OCEAN_SCROLL_INC1) = inc1 * k;
        scaled = 1;
    }
    if (s_aimlog)
        aim_frame(interp, interp ? s_aim_a : -1.0f);   /* test aid: what this frame draws */
    /* lane au-fx (B+, call A10): the particles' per-call quantities per game
     * step. A running frame with a frame time: s_pf_m = the steps it
     * follows; with none, sub_00023F80 holds the trails, sub_000253B0 the
     * bolt's bend and pfxjit_apply the re-rolls, and the group scripts wait */
    s_pf_m = s_steps_since_draw;
    s_pf_on = HU32(msg + 4) != 0 && HU32(GAME_VA + GAME_STATE) == 0;
    s_hold_frame = s_pf_on && s_pf_m == 0;
    s_pf_alpha = s_pf_on ? step_alpha() : 1.0f;
    if (s_log)
        QueryPerformanceCounter(&s_bp_t0);  /* BW_HIFPS_LOG: B+'s own cost per drawn frame */
    pfxgrow_pre(HU32(msg + 4));            /* lane wake, au-fx: the growth per step */
    pfxjit_apply(HU32(msg + 4));           /* lane au-fx: bolt jitter per game step (T2) */
    pfxscript_pre();                       /* lane au-fx: group scripts per game step */
    pfxdeath_pre();                        /* lane fx-death: deaths on the step grid (after the growth's reads) */
    if (s_log) {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        s_st_bp_ticks += (unsigned long long)(t.QuadPart - s_bp_t0.QuadPart);
    }
    sub_00028600_gen();
    if (s_log)
        QueryPerformanceCounter(&s_bp_t0);
    pfe_build();
    pfxdeath_post();                       /* lives and windows back first, then the follow */
    pfxscript_post();
    trail_restore();
    pfxjit_restore();
    pfxgrow_post();
    {
        /* the frame times since the last step's frame, summed as a particle's
         * age (fld dt; fadd age): T1 tells a particle born since from its age */
        union { float f; uint32_t u; } dt, sum;
        dt.u = pfx_dt();
        if (!s_pf_on)
            s_pf_since_ok = 0;
        else if (dt.f > 0.0f) {
            if (s_pf_m) {
                s_pf_since = dt.f;
                s_pf_since_ok = 1;
            } else if (s_pf_since_ok) {
                sum.f = s_pf_since;
                sum.u = pgb_add(dt.u, sum.u);
                s_pf_since = sum.f;
            }
        }
    }
    if (s_log) {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        s_st_bp_ticks += (unsigned long long)(t.QuadPart - s_bp_t0.QuadPart);
        s_st_bp_frames++;
    }
    s_hold_frame = 0;
    s_pf_on = 0;
    if (lat_t)
        lat_frame_end();
    if (scaled) {
        HF32(OCEAN_SCROLL_INC0) = inc0;
        HF32(OCEAN_SCROLL_INC1) = inc1;
    }
    if (ptd) {
        s_render_seed = HU32(ptd + PTD_SEED);
        HU32(ptd + PTD_SEED) = sim_seed;
    }
    if (interp) {
        hud_restore();
        restore_water();
        restore_interp();
    }
    if (audit && s_audit > 0)
        audit_post(HU32(msg + 4));
    bw_fl_event(BW_FL_G_FRAME_END, s_steps_since_draw, g_bw_ko_last_put, 0);
    if (!s_steps_since_draw)
        s_st_zero++;
    s_jit_frame_stepped = s_steps_since_draw != 0;     /* lat-q: for jit_work_ms */
    s_jit_drawn++;                                      /* lat-q: BW_HIFPS_JIT=4 */
    if (s_jit == 4 && s_jit_prev_ms > 0.0) {
        double fe = jit_qpc_ms(), c = fe - s_jit_prev_ms - (double)(g_bw_ko_wait_us - s_jit_kw0) / 1000.0;
        if (c > 0.0 && c < 100.0)
            s_jit_cpu[s_jit_frame_stepped][s_jit_cpun[s_jit_frame_stepped]++ % 32u] = (float)c;
        s_jit_fe_ms = fe;
    }
    s_steps_since_draw = 0;
    s_st_frames++;
    if (s_log) {
        static ULONGLONG last;
        ULONGLONG now = GetTickCount64();
        if (!last)
            last = now;
        if (now - last >= 1000) {
            fprintf(stderr, "[BW] hifps: %lu frames %lu steps (%lu frames with none), "
                    "alpha %.2f, nodes lerped %lu snapped %lu, ocean batches %lu reused %lu"
                    " (verify mismatches %lu), cameras %lu (overwritten %lu), water %lu"
                    " (verify %lu, tiles blended %lu, older evaluated %lu), hud replays %lu (bytes moved %lu, frame-touched %lu,"
                    " world points blended %lu snapped %lu)"
                    "; hud objects this boot: %d slots of %d, %lu reused, %lu missed\n",
                    s_st_frames, s_st_steps, s_st_zero,
                    s_st_frames ? s_st_alpha / s_st_frames : 0.0, s_st_lerped, s_st_snapped,
                    s_st_ob, s_st_obhit, s_st_obbad, s_st_cam, s_st_camover,
                    s_st_water, s_st_waterbad, s_st_oblerp, s_st_obprev, s_st_hud, s_st_hudmoved,
                    s_st_hudtouched, s_st_hudlerp, s_st_hudsnap, s_nhud, HUD_RECS, s_hud_reused, s_hud_missed);
            s_st_frames = s_st_steps = s_st_zero = s_st_lerped = s_st_snapped = 0;
            s_st_ob = s_st_obhit = s_st_obbad = s_st_cam = s_st_camover = 0;
            s_st_water = s_st_waterbad = s_st_hud = s_st_hudmoved = s_st_hudtouched = 0;
            s_st_hudlerp = s_st_hudsnap = 0;
            s_st_oblerp = s_st_obprev = 0;
            if (s_st_eye || s_st_eyeover)          /* HIFPS_EYE */
                fprintf(stderr, "[BW] hifps: eyes blended %lu (overwritten by the frame %lu),"
                        " ocean tiles picked by the step's eye %lu\n", s_st_eye, s_st_eyeover, s_st_eyeocean);
            s_st_eye = s_st_eyeover = s_st_eyeocean = 0;
            s_st_alpha = 0.0;
            if (s_st_pg || s_st_pgover)
                fprintf(stderr, "[BW] hifps: particle growth by the frame time: %lu emitter frames"
                        " (%lu frames over the %d cap)\n", s_st_pg, s_st_pgover, PFX_GROW_MAX);
            s_st_pg = s_st_pgover = 0;
            if (s_st_pjtick || s_st_pjhold || s_st_pjover)
                fprintf(stderr, "[BW] hifps: bolt jitter per game step: %lu emitter periods set on step"
                        " frames, %lu re-rolls held on frames between (%lu over the %d cap)\n",
                        s_st_pjtick, s_st_pjhold, s_st_pjover, PFX_JIT_MAX);
            s_st_pjtick = s_st_pjhold = s_st_pjover = 0;
            if (s_st_trhold || s_st_bolthold || s_st_boltkeep)
                fprintf(stderr, "[BW] hifps: per game step, on frames between steps: %lu trail shifts"
                        " held, %lu bolt bends put back, %lu timer-less bolts kept\n", s_st_trhold,
                        s_st_bolthold, s_st_boltkeep);
            s_st_trhold = s_st_bolthold = s_st_boltkeep = 0;
            if (s_st_pgb_old || s_st_pgb_new || s_st_pgb_odd || s_st_pgb_gone || s_st_pgb_over
                    || s_st_trrest || s_st_trextra || s_st_trover || s_st_boltextra || s_st_gshold || s_st_gsover)
                fprintf(stderr, "[BW] hifps: B+ per step: scales put back %lu, newborns %lu (%lu set up"
                        " in-frame), odd %lu, emitters gone %lu, over %lu; trails put back %lu, extra shifts"
                        " %lu, over %lu, born since a step %lu; bolt extra calls %lu; group scripts held %lu (waits timed %lu,"
                        " over %lu)\n", s_st_pgb_old, s_st_pgb_new, s_st_pgb_fresh, s_st_pgb_odd,
                        s_st_pgb_gone, s_st_pgb_over, s_st_trrest, s_st_trextra, s_st_trover, s_st_trlate,
                        s_st_boltextra, s_st_gshold, s_st_gswait, s_st_gsover);
            if (s_st_bp_frames) {
                LARGE_INTEGER f;
                QueryPerformanceFrequency(&f);
                fprintf(stderr, "[BW] hifps: B+ cost %.1f us a drawn frame (before and after the frame's"
                        " body, %lu frames)\n", (double)s_st_bp_ticks * 1e6 / (double)f.QuadPart
                        / (double)s_st_bp_frames, s_st_bp_frames);
            }
            if (s_st_pd_keep || s_st_pd_end || s_st_pd_hold || s_st_pd_over || s_st_pd_untr || s_st_pgb_still)
                fprintf(stderr, "[BW] hifps: particle deaths on the step grid: %lu kept to the step, %lu ended"
                        " at it, %lu not placed (the game's own test, at the step), %lu stopped emitters held%s;"
                        " %lu over a cap, %lu untracked; growth put back with no update run %lu\n", s_st_pd_keep,
                        s_st_pd_end, s_st_pd_unpl, s_st_pd_hold, s_pfxstophold ? "" : " (off)", s_st_pd_over,
                        s_st_pd_untr, s_st_pgb_still);
            s_st_pd_keep = s_st_pd_end = s_st_pd_unpl = s_st_pd_hold = s_st_pd_over = s_st_pd_untr = 0;
            s_st_pgb_still = 0;
            s_st_bp_ticks = 0;
            s_st_bp_frames = 0;
            s_st_pgb_old = s_st_pgb_new = s_st_pgb_fresh = s_st_pgb_odd = s_st_pgb_gone = s_st_pgb_over = 0;
            s_st_trrest = s_st_trextra = s_st_trover = s_st_trlate = s_st_boltextra = 0;
            s_st_gshold = s_st_gswait = s_st_gsover = 0;
            if (s_jit) {
                double slot = 0.0;
                int have_slot = bw_d3d11_present_due(&slot);
                fprintf(stderr, "[BW] hifps: just-in-time waits %lu, %.1f ms (mean %.2f), guard %lu;"
                                " a pass takes %.2f ms to its flip (%.2f with a game step; %u and %u"
                                " measured); pacing slot %s\n",
                        s_st_jit_n, s_st_jit_ms, s_st_jit_n ? s_st_jit_ms / s_st_jit_n : 0.0,
                        s_st_jit_guard, jit_work_ms(0), jit_work_ms(1), s_jit_wn[0], s_jit_wn[1],
                        have_slot ? "on" : "off");
                if (s_st_jit_idle)
                    fprintf(stderr, "[BW] hifps: just-in-time passes that didn't wait for an idle walk: %lu\n",
                            s_st_jit_idle);
                if (s_st_jit_long)
                    fprintf(stderr, "[BW] hifps: just-in-time waits over 100 ms through a busy walk or GPU: %lu\n",
                            s_st_jit_long);
                if (s_latmode)                  /* gfx-lat11 */
                    fprintf(stderr, "[BW] hifps: the latency gate waited for the GPU before %lu passes (%.1f ms),"
                                    " aimed %lu at the GPU coming free; GPU %.2f ms a frame, horizon %.2f ms\n",
                            s_st_gate_gpu, s_st_gate_gpu_ms, s_st_gate_aim, bwg_gate_gpu_period(),
                            bwg_gate_horizon_ms());
                if (s_pred_ahead_auto)
                    fprintf(stderr, "[BW] hifps: prediction %.2f ms past now (the measured horizon)\n",
                            (double)s_pred_ahead);
                s_st_gate_gpu = s_st_gate_aim = 0;
                s_st_gate_gpu_ms = 0.0;
                s_st_jit_n = s_st_jit_guard = s_st_jit_long = s_st_jit_idle = 0;
                s_st_jit_ms = 0.0;
            }
            last = now;
        }
    }
}

/* Graphics_HandleMessage 0x00028600: the frame above, then (every edition)
 * the per-step particle checker after each drawn frame (BW_PFXCHECK, test
 * aid, read-only). */
void sub_00028600(void)
{
    uint32_t msg = HU32(g_esp + 4);
    int draw = msg && HU32(msg) == 8;
    hifps_frame();
    if (draw && (s_pc || !s_pc_init))
        pfxcheck_frame();
}

/* Sky_UpdateLightning 0x00030250 (this = 0x1A9F88, no arguments, `ret`):
 * while no flash is lit it rolls rand()/32767 > 1 - 0.03 * level once per
 * call, and it is called once per drawn frame, so at 120 fps storms would
 * flash four times as often. Remastered rolls once per step, as Original at
 * 30 fps did: on a drawn frame with no new step and no flash lit, the call
 * is skipped, which is what a failed roll does (no write). */
extern void sub_00030250_gen(void);
void sub_00030250(void)
{
    static int zero_ok = -1;
    if (zero_ok < 0 && s_on) {
        /* The game compares the flash with the float at 0x174820; the gate
         * below assumes it is 0.0 (inferred from the code's use). */
        zero_ok = HF32(0x00174820u) == 0.0f;
        if (!zero_ok)
            fprintf(stderr, "[BW] hifps: lightning gate off (0x174820 = %g, not 0)\n",
                    HF32(0x00174820u));
    }
    if (s_on && s_lightning && zero_ok == 1 && s_steps_since_draw == 0
        && HF32(g_ecx + SKY_FLASH) == 0.0f) {
        g_esp += 4;                         /* the function's own `ret` */
        return;
    }
    eye_swap(1);                            /* HIFPS_EYE: the bolt keeps the step's eye */
    sub_00030250_gen();
    eye_swap(0);
}

/* WaterWorld_EvaluateVertexBatch 0x00098130 (thiscall, this = WaterWorld
 * 0x1D77F8; [esp+4] which outputs (low 5 bits), [esp+8] the vertex count, 25
 * from Ocean_FillVertices; `ret 8`). It evaluates the swell, ripples and
 * wakes at each vertex of an ocean tile (WaterWorld_VertexHeightNormal and
 * Water_HeightAndSlopeAt: about a third of the game thread at high frame
 * rates, profile run 20261027-023720-r6-gpu-d7hfprof2). Its inputs are the
 * tile's vertex positions (x, z at +0/+4 of each 0x28-byte vertex, through
 * the pointer table WaterWorld+0xCCB8..+0xCCE0, seen) and the water's state,
 * which only a game step changes (Water_Update, WaterWorld_Tick: per step,
 * 120fps.md §5). So on a drawn frame with no new step, a tile already
 * evaluated since the last step gives the same result: the bytes it left are
 * copied back instead of evaluating again. Only for Ocean_FillVertices's call
 * (return address 0x37CC2) and only in the high frame rate; Original always
 * evaluates. The one thing skipped besides the result is the function's own
 * time-spent counter (WaterWorld+0xCCF4, rdtsc profiling). Exactness is
 * checked with BW_HIFPS_OCEAN_VERIFY=1 (evaluate anyway, compare, count). */
/* Slots (BW_HIFPS_OCEAN_SLOTS, test aid): 2048 was D7's size, enough for one
 * view's ~105 tiles x 2 steps. Split screen at single-player detail (T3) has
 * 4 x ~104 tiles x 2 steps live, which collide in a direct-mapped table: at
 * 2048 about a quarter of the "older step" results were evicted and evaluated
 * again. 8192 (T3, 2026-10-27, the lead's go): 4P one-player sea 35-37 ->
 * 38-44 fps, 2P 75-76 -> 82-90 fps, older evaluations 22,896 -> 9,475 in 50 s,
 * BW_HIFPS_OCEAN_VERIFY mismatches 0 (T3.md section 1.4). */
#define OB_N_DEFAULT 8192u                /* T3: was 2048 (D7); ~21 MB */
static uint32_t s_ob_n = OB_N_DEFAULT;
#define OB_N        s_ob_n
#define OB_MAXV     64u                  /* vertices per batch: 25 seen */
#define OB_STRIDE   0x28u
#define OB_BYTES    (OB_MAXV * OB_STRIDE)
typedef struct {
    uint64_t key;
    uint32_t gen, base, len;
    uint8_t  bytes[OB_BYTES];
} ObEntry;
static ObEntry *s_ob;

extern void sub_00098130_gen(void);
/* The render-vertex batch, the game's body or the native ocean
 * (src/bw_ocean_native.c, OCEAN_NATIVE; Original always runs the body). Only
 * for Ocean_FillVertices's call (return address 0x37CC2, this = WaterWorld). */
extern void bw_ocean_eval(void);

/* One call of the game's batch body at the original call's stack frame
 * (return address, flags, count: restored each time, since the body's
 * `ret 8` pops them) with this = WaterWorld. */
static void ob_call(uint32_t esp0, const uint32_t frame[3])
{
    g_esp = esp0;
    HU32(esp0) = frame[0];
    HU32(esp0 + 4) = frame[1];
    HU32(esp0 + 8) = frame[2];
    g_ecx = WW_VA;
    bw_ocean_eval();                         /* frame[0] is 0x37CC2 (checked by the caller) */
}

static ObEntry *ob_slot(uint64_t h, uint32_t gen)
{
    return &s_ob[(uint32_t)((h ^ (h >> 29)) + (uint64_t)gen * 0x9E3779B1u) % OB_N];
}

/* The water between steps, the fast way (BW_HIFPS_WATER=1, the default): a
 * tile's 25 vertices evaluated at the newest step's water and at the step
 * before (both cached per step; the older one is usually still cached from
 * the step before, otherwise it is evaluated now with the Water object's
 * newest index pointed at the previous buffer and the WaterWorld clock at the
 * previous step, both put back at once), and the drawn frame gets the blend
 * of the two: height, the three normal components and the field at +0xC
 * (the five floats the batch writes, by its pointer table; checked on every
 * call). Heights are linear in the height field, so the blended height is
 * exactly the height of the blended field; the normals and ripples are
 * blended rather than re-evaluated, a sub-step approximation (inferred:
 * invisible). Original never gets here. */
#define OB_LERP_FIELDS 5
static const uint32_t s_ob_lerp_off[OB_LERP_FIELDS] = { 0x08u, 0x0Cu, 0x1Cu, 0x20u, 0x24u };

void sub_00098130(void)
{
    uint32_t flags, side, base, stride, nv, len, i, esp0, frame[3];
    uint64_t h = 1469598103934665603ull;
    ObEntry *e, *ep;
    int called = 0, lerp;
    if (HU32(g_esp) != 0x00037CC2u || g_ecx != WW_VA) {
        sub_00098130_gen();                      /* hulls, the secondary water: the game's own */
        return;
    }
    if (!s_on || !s_oceancache || s_water_on_frame) {
        bw_ocean_eval();
        return;
    }
    flags = HU32(g_esp + 4);
    side = HU32(g_esp + 8);
    base = HU32(WW_VA + 0xCCBCu);                /* vertex 0, x */
    stride = HU32(WW_VA + 0xCCD8u);
    nv = side;                                  /* vertices: 5 x 5 per tile (seen: 25 written) */
    if (stride != OB_STRIDE || side == 0 || nv > OB_MAXV || base < 0x00010000u
        || HU32(WW_VA + 0xCCB8u) != base + 8u) {   /* not the layout read above */
        bw_ocean_eval();
        return;
    }
    if (!s_ob) {
        uint32_t n = env_u("BW_HIFPS_OCEAN_SLOTS", OB_N_DEFAULT);
        s_ob_n = n >= 256u && n <= 65536u ? n : OB_N_DEFAULT;
        s_ob = (ObEntry *)calloc(OB_N, sizeof(ObEntry));
        if (!s_ob) {
            s_oceancache = 0;
            bw_ocean_eval();
            return;
        }
    }
    len = nv * OB_STRIDE;
    /* FNV-1a 64 over the inputs: flags, side, the pointer table, x and z */
    {
        uint32_t hdr[3] = { flags, side, base };
        const uint8_t *p = (const uint8_t *)hdr;
        for (i = 0; i < sizeof hdr; i++)
            h = (h ^ p[i]) * 1099511628211ull;
        p = (const uint8_t *)HP(WW_VA + 0xCCB8u);
        for (i = 0; i < 0x2Cu; i++)
            h = (h ^ p[i]) * 1099511628211ull;
        for (i = 0; i < nv; i++) {
            const uint32_t *v = (const uint32_t *)HP(base + i * OB_STRIDE);
            h = (h ^ v[0]) * 1099511628211ull;
            h = (h ^ v[1]) * 1099511628211ull;
        }
    }
    lerp = s_water_lerp_frame
        && HU32(WW_VA + 0xCCC0u) == base + 0x0Cu && HU32(WW_VA + 0xCCC8u) == base + 0x1Cu
        && HU32(WW_VA + 0xCCCCu) == base + 0x24u && HU32(WW_VA + 0xCCD0u) == base + 0x20u
        && s_water_gen > 0;
    esp0 = g_esp;
    frame[0] = HU32(esp0);
    frame[1] = flags;
    frame[2] = side;
    s_st_ob++;
    /* the newest step's result */
    e = ob_slot(h, s_water_gen);
    if (e->key == h && e->gen == s_water_gen && e->base == base && e->len == len) {
        s_st_obhit++;
        if (s_oceanverify && !lerp) {
            ob_call(esp0, frame);
            called = 1;
            if (memcmp(HP(base), e->bytes, len))
                s_st_obbad++;
        }
    } else {
        ob_call(esp0, frame);
        called = 1;
        e->key = h;
        e->gen = s_water_gen;
        e->base = base;
        e->len = len;
        memcpy(e->bytes, HP(base), len);
    }
    if (lerp) {
        /* the step before's result */
        ep = ob_slot(h, s_water_gen - 1u);
        if (!(ep->key == h && ep->gen == s_water_gen - 1u && ep->base == base && ep->len == len)) {
            uint32_t w = HU32(WW_VA), nw = HU32(w + W_IDX_NEW), pv = HU32(w + W_IDX_PREV);
            uint32_t ms = HU32(WW_VA + WW_CLOCK_MS), sec = HU32(WW_VA + WW_CLOCK_S);
            float tn = HF32(WW_VA + WW_CLOCK_S), tp = HF32(WW_VA + WW_CLOCK_PREV_S);
            HU32(w + W_IDX_NEW) = pv;
            HF32(WW_VA + WW_CLOCK_S) = tp;
            if (tn > tp && tn - tp < 1.0f)
                HU32(WW_VA + WW_CLOCK_MS) = ms - (uint32_t)((tn - tp) * 1000.0f + 0.5f);
            ob_call(esp0, frame);
            called = 1;
            HU32(w + W_IDX_NEW) = nw;
            HU32(WW_VA + WW_CLOCK_MS) = ms;
            HU32(WW_VA + WW_CLOCK_S) = sec;
            ep->key = h;
            ep->gen = s_water_gen - 1u;
            ep->base = base;
            ep->len = len;
            memcpy(ep->bytes, HP(base), len);
            s_st_obprev++;
        }
        /* the blend: the newest result's bytes, five floats per vertex lerped */
        memcpy(HP(base), e->bytes, len);
        for (i = 0; i < nv; i++) {
            int k;
            for (k = 0; k < OB_LERP_FIELDS; k++) {
                uint32_t off = i * OB_STRIDE + s_ob_lerp_off[k];
                float a, b;
                memcpy(&a, ep->bytes + off, 4);
                memcpy(&b, e->bytes + off, 4);
                HF32(base + off) = a + (b - a) * s_water_alpha;
            }
        }
        s_st_oblerp++;
    } else if (!called) {
        memcpy(HP(base), e->bytes, len);
    }
    g_esp = esp0 + 12;                       /* the function's own `ret 8`, once */
}
