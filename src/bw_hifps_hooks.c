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
 *                        (default 2; 0 = no limit; P2b, bw_d3d11.c)
 *   BW_HIFPS_PACE=0      presents no longer held at least 1/target apart (P2b)
 *   BW_HIFPS_SCREENWAIT=0 a present the screen can't take yet is dropped at
 *                        once (D7) instead of waited for briefly (P2b)
 *   BW_HIFPS_INTERP=0    fixed step only, no interpolation (step "b0")
 *   BW_HIFPS_EXTRAP=1    camera extrapolated one step ahead, not interpolated
 *   BW_HIFPS_LIGHTNING=0 the lightning rolls per drawn frame again
 *   BW_HIFPS_RAND=0      drawn frames share the simulation's rand() again
 *   BW_HIFPS_WATER=0     the water stays at 30 Hz (no blend; the ocean cache
 *                        is used instead); BW_HIFPS_WATER_VERIFY=1 checks the
 *                        Water object is byte-identical after each frame
 *   BW_HIFPS_HUD=0       world markers at 30 Hz again
 *   BW_HIFPS_RAIN=0      rain per drawn frame again
 *   BW_HIFPS_AUDIT=1     test aid: lists what drawing alone moves in .data
 *   BW_HIFPS_LOG=1       a line a second: frames, steps, nodes lerped/snapped
 *   BW_STEPLOG=<file>    every edition: one line per game step (test aid)
 *   BW_RAINLOG=1         every edition: rain spawn and fall rates (test aid)
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
#include "bw_framelog.h"   /* P2b: BW_FRAMELOG test aid */
#include "bw_monitor.h"    /* contract 6: the game's monitor (B1, item 34) */

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
static int s_in_update;          /* inside Game_Update */
static uint32_t s_steps_since_draw, s_snapshots;

static Mat      s_prev[NODE_N], s_cur[NODE_N], s_wrote[NODE_N];
static uint32_t s_id_prev[NODE_N], s_id_cur[NODE_N];   /* hash ^ drawable | 1, 0 = free */
static uint8_t  s_touched[NODE_N];
static CamKey   s_cam_prev[MAX_VIEWS], s_cam_cur[MAX_VIEWS];
static uint8_t  s_rcam_saved[MAX_VIEWS][RCAM_SIZE];
static float    s_cam_wrote[MAX_VIEWS][16];
static uint8_t  s_scam_saved[MAX_VIEWS][64];   /* simulation view 0x1CE4A8 + 0x40*n */
static unsigned s_scam_written;
static unsigned s_cam_written;
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
static uint32_t s_water_gen;             /* +1 per running step: the water's state */
static unsigned long s_st_ob, s_st_obhit, s_st_obbad;

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
    s_lightning = env_u("BW_HIFPS_LIGHTNING", 1) != 0;
    s_randsplit = env_u("BW_HIFPS_RAND", 1) != 0;
    s_log = env_u("BW_HIFPS_LOG", 0) == 1;
    s_oceanrate = env_u("BW_HIFPS_OCEANRATE", 1) != 0;
    s_oceancache = env_u("BW_HIFPS_OCEANCACHE", 1) != 0;
    s_oceanverify = env_u("BW_HIFPS_OCEAN_VERIFY", 0) == 1;
    s_water = (int)env_u("BW_HIFPS_WATER", 1);     /* 0 off, 1 blend results (fast), 2 blend the field */
    s_waterverify = env_u("BW_HIFPS_WATER_VERIFY", 0) == 1;
    s_hudon = env_u("BW_HIFPS_HUD", 1) != 0;
    s_rain = env_u("BW_HIFPS_RAIN", 1) != 0;
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
     * 0 = no limit as before. */
    bw_d3d11_latency_config(2);
    bw_d3d11_pace_config(hz);
    s_on = 1;
    fprintf(stderr, "[BW] high frame rate (%s): up to %u fps (display %u Hz%s), "
            "fixed 30 Hz game step, %s%s%s\n", bw_edition_title(bw_edition()),
            hz, refresh, vsync ? ", vsync" : tear ? ", variable refresh" : "",
            !s_interp ? "no interpolation" : s_extrap ? "camera extrapolated" : "interpolated",
            s_lightning ? "" : ", lightning per frame", s_randsplit ? "" : ", shared rand()");
    {
        extern int g_bw_kickoff_async;
        if (!g_bw_kickoff_async)
            fprintf(stderr, "[BW] high frame rate: KickOff waits for each flush (BW_HIFPS_ASYNC=0)\n");
    }
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
    if (base)
        s_snapshots++;
    s_steps_since_draw++;
    s_st_steps++;
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
    if (tick <= 0.0 || gnow < (double)clk)
        return 1.0f;
    a = (gnow - (double)clk) / tick;
    return a < 0.0 ? 0.0f : a > 1.0 ? 1.0f : (float)a;
}

static void apply_interp(float t)
{
    uint32_t base = HU32(NODEPOOL_PTR_VA), i;
    unsigned n;
    memset(s_touched, 0, sizeof s_touched);
    if (base) {
        for (i = 0; i < NODE_N; i++) {
            uint32_t nd = base + i * NODE_SIZE;
            Mat o;
            if (!s_id_cur[i] || s_id_prev[i] != s_id_cur[i])
                continue;                                   /* new, freed or reused */
            if (!memcmp(&s_prev[i], &s_cur[i], sizeof(Mat)))
                continue;                                   /* did not move */
            if (memcmp(HP(nd), &s_cur[i], sizeof(Mat)))
                continue;                                   /* written since the step
                                                               (animation, attachments) */
            if (!lerp_mat(&s_prev[i], &s_cur[i], t, &o)) {
                s_st_snapped++;
                continue;
            }
            memcpy(HP(nd), &o, sizeof(Mat));
            s_wrote[i] = o;
            s_touched[i] = 1;
            s_st_lerped++;
        }
    }
    s_rcam_n = 0;
    if (s_nviews && s_nviews == s_nviews_prev) {
        s_rcam_n = s_nviews;
        for (n = 0; n < s_nviews; n++) {
            const CamKey *a = &s_cam_prev[n], *b = &s_cam_cur[n];
            float eye[3], at[3], up[3], m[16], d[3], u;
            int j;
            memcpy(s_rcam_saved[n], HP(RCAM_VA(n + 1)), RCAM_SIZE);
            for (j = 0; j < 3; j++) d[j] = b->eye[j] - a->eye[j];
            if (a->mode != b->mode || !(len3(d) <= SNAP_CAM_M))
                continue;                                   /* a cut: show the step */
            {
                /* A cut within 50 m (a cutscene's next shot): the view turns
                 * more than ~30 degrees in one step, which no camera move in
                 * play does (that would be 900 degrees a second). */
                float fa[3], fb[3];
                for (j = 0; j < 3; j++) {
                    fa[j] = a->at[j] - a->eye[j];
                    fb[j] = b->at[j] - b->eye[j];
                }
                norm3(fa);
                norm3(fb);
                if (!(dot3(fa, fb) >= 0.866f))
                    continue;
            }
            u = s_extrap ? 1.0f + t : t;                    /* extrapolate: one step ahead */
            for (j = 0; j < 3; j++) {
                eye[j] = a->eye[j] + (b->eye[j] - a->eye[j]) * u;
                at[j]  = a->at[j]  + (b->at[j]  - a->at[j])  * u;
                up[j]  = a->up[j]  + (b->up[j]  - a->up[j])  * u;
            }
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
        }
    }
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
    s_cam_written = 0;
    s_scam_written = 0;
    s_rcam_n = 0;
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
 * counted in the BW_HIFPS_LOG line. */
#define HUD_RECS      16
#define HUD_MAXSZ     0xA6Cu
typedef struct {
    uint32_t fn, obj, arg, size, step;
    uint8_t  pre[HUD_MAXSZ], post[HUD_MAXSZ], replay[HUD_MAXSZ];
    int      replayed;
} HudRec;
static HudRec s_hud[HUD_RECS];
static int s_nhud, s_hudon = 1;
static unsigned long s_st_hud, s_st_hudmoved, s_st_hudtouched;
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
    return &s_hud[i];
}

static void hud_record(uint32_t fn, uint32_t size)
{
    HudRec *r;
    if (!s_on || !s_hudon || !s_in_update || g_ecx < 0x00010000u)
        return;
    r = hud_rec(fn, g_ecx, size);
    if (!r)
        return;
    r->arg = HU32(g_esp + 4);
    r->step = s_snapshots + 1;               /* the step that is running now */
    memcpy(r->pre, HP(g_ecx), size);
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
    hud_record(0x00084AE0u, 0xA6Cu);
    sub_00084AE0_gen();
}

void sub_00085850(void)
{
    hud_record(0x00085850u, 0x1E0u);
    sub_00085850_gen();
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
        sub_0002E530_gen();
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
    sub_0002E530_gen();
    HU32(RAIN_FALL_K) = k_bits;
    HU32(RAIN_INTENSITY) = i_bits;
    rain_count(sky);
}

/* ---- the wrappers -------------------------------------------------------- */

/* Game_Update 0x00091380 (thiscall, this = 0x1D5C08): the game's own fixed
 * 30 Hz accumulator without its one-step floor (speed mode outside 1..3:
 * time unscaled, the floor skipped; 120fps.md §2, seen in code). Game_InitLevel
 * zeroes the mode, so it is written on every call. */
extern void sub_00091380_gen(void);
void sub_00091380(void)
{
    if (s_on) {
        HU32(GAME_VA + GAME_SPEED_MODE) = GAME_SPEED_FIXED;
        QueryPerformanceCounter(&s_host_at_update);   /* about when it reads its clock */
    }
    s_in_update = 1;
    bw_fl_event(BW_FL_G_UPD, 0, 0, 0);
    sub_00091380_gen();
    bw_fl_event(BW_FL_G_UPD_END, 0, 0, 0);
    s_in_update = 0;
}

/* Overlays_Update 0x00083EF0: the last call of a running step (after the
 * camera and the physics), so a snapshot sees each step's final state, also
 * when a frame runs two steps. */
extern void sub_00083EF0_gen(void);
void sub_00083EF0(void)
{
    sub_00083EF0_gen();
    if (s_in_update && HU32(GAME_VA + GAME_STATE) == 0) {
        if (s_on) {
            snapshot_step();
            s_water_gen++;
        }
        steplog_step();
    }
}

/* Graphics_HandleMessage 0x00028600 (thiscall; [esp+4] = the message,
 * type at +0, dt at +4, paused at +8; type 8 = draw a frame: AnimLib_Update
 * then Graphics_RenderFrame). */
extern void sub_00028600_gen(void);
void sub_00028600(void)
{
    uint32_t msg, ptd = 0, sim_seed = 0;
    int interp, scaled = 0, audit;
    float inc0 = 0.0f, inc1 = 0.0f;
    if (g_bw_fl_on) {
        /* P2b, BW_FRAMELOG (test aid, every edition): each drawn frame */
        msg = HU32(g_esp + 4);
        if (msg && HU32(msg) == 8) {
            unsigned st = s_steps_since_draw;
            bw_fl_thread(0);
            bw_fl_event(BW_FL_G_FRAME, 0, 0, 0);
            if (!s_on) {
                sub_00028600_gen();
                bw_fl_event(BW_FL_G_FRAME_END, st, 0, 0);
                return;
            }
        }
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
    if (interp) {
        float a = step_alpha();
        s_st_alpha += a;
        apply_interp(a);
        apply_water(a);
        hud_replay();
    }
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
    sub_00028600_gen();
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
    bw_fl_event(BW_FL_G_FRAME_END, s_steps_since_draw, 0, 0);
    if (!s_steps_since_draw)
        s_st_zero++;
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
                    " (verify %lu, tiles blended %lu, older evaluated %lu), hud replays %lu (bytes moved %lu, frame-touched %lu)"
                    "; hud objects this boot: %d slots of %d, %lu reused, %lu missed\n",
                    s_st_frames, s_st_steps, s_st_zero,
                    s_st_frames ? s_st_alpha / s_st_frames : 0.0, s_st_lerped, s_st_snapped,
                    s_st_ob, s_st_obhit, s_st_obbad, s_st_cam, s_st_camover,
                    s_st_water, s_st_waterbad, s_st_oblerp, s_st_obprev, s_st_hud, s_st_hudmoved,
                    s_st_hudtouched, s_nhud, HUD_RECS, s_hud_reused, s_hud_missed);
            s_st_frames = s_st_steps = s_st_zero = s_st_lerped = s_st_snapped = 0;
            s_st_ob = s_st_obhit = s_st_obbad = s_st_cam = s_st_camover = 0;
            s_st_water = s_st_waterbad = s_st_hud = s_st_hudmoved = s_st_hudtouched = 0;
            s_st_oblerp = s_st_obprev = 0;
            s_st_alpha = 0.0;
            last = now;
        }
    }
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
    sub_00030250_gen();
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
