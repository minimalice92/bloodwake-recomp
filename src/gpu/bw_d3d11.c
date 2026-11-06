/*
 * bw_d3d11.c - Blood Wake GPU render back end (D-003 option B), prototype.
 *
 * Implements the toolkit's NV2A back-end interface (xboxrecomp
 * src/kernel/nv2a_backend.h) with Direct3D 11, so the pushbuffer executor
 * hands its decoded draws to the GPU instead of its CPU rasteriser. Enabled
 * with BW_GPU=d3d11 (and RECOMP_PB_EXEC, as for the CPU path); without it the
 * CPU rasteriser runs as before.
 *
 * What it does:
 *   - one D3D11 render target per guest colour surface (and one depth-stencil
 *     buffer per depth surface), seeded once from guest memory and
 *     authoritative on the GPU after that;
 *   - a texture whose address is a surface it rendered samples that render
 *     target directly (render-to-texture without a round trip);
 *   - other textures are decoded with nv2a_backend_decode_texture and cached,
 *     re-hashed once per frame so a texture the title rewrites (a movie frame)
 *     is re-uploaded;
 *   - at each flip the finished surface is read back into guest memory and
 *     presented, so the toolkit's window and frame dumps keep working.
 *
 * Not yet (prototype): register combiners and texture stages 1-3 (the
 * interface passes stage 0 and the diffuse colour), mipmaps, the clear
 * rectangle, back-face culling. BW_GPU_STATS=1 prints a line per second.
 */
#define COBJMACROS
/* WIN32_LEAN_AND_MEAN comes from the build */
#include "../bw_framelog.h"   /* P2b: BW_FRAMELOG test aid */
#include "../bw_latlog.h"     /* lat-m: BW_LATLOG test aid */
#include <math.h>   /* fabsf (flash_watch): an implicit declaration fails to link on newer toolsets */
#include <windows.h>
#include <intrin.h> /* __rdtsc (item 43's draw cpu measure) */
#include <d3d11.h>
#include <d3d11_4.h>   /* lat-m: ID3D11Fence for BW_LATLOG's GPU-done times */
#include <dxgi1_6.h>   /* IDXGISwapChain2 (frame-latency waitable), IDXGIFactory5 (tearing),
                          IDXGIFactory6 (the high-performance adapter, item 27) */
#include <d3dcompiler.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nv2a_backend.h"
#include "bw_d3d11.h"
#include "bw_rc_hlsl.h"
#include "bw_vsh_hlsl.h"
#include "bw_gamma.h"        /* the title's DAC gamma ramp, applied at present */
extern int bw_ab_off(const char *fix);      /* BW_GPU_AB (bw_ab.c) */
extern void bw_ab_frame(uint32_t n);
#include "d3d/d3d8_swizzle.h"   /* xbox_unswizzle_rect */
#include "toolkit_patches/kernel/bw_backend_ext.h"
#include "bw_vfx_d3d11.h"        /* VFX (D9): Remastered visual suite; inert in Original */
#include "bw_texrestore_d3d11.h" /* T1 (VFX_TEX_RESTORE): texture restoration; inert in Original */
#include "bw_pscache.h"          /* T1: persistent pixel-shader cache (every edition) */
#include "bw_gpu_failtest.h"     /* 0.2.5 item 25: BW_GPU_FAILTEST, BW_GPU_DEBUG (test aids) */
#include "bw_hdr.h"              /* 0.2.5 item 97: a screen in HDR mode, every edition */
#include "../bw_widescreen.h"       /* Remastered widescreen (D-015/D-016) */
#include "../bw_edition.h"          /* bw_edition_add_summary */
#include "bw_gfxcap.h"             /* BW_GFXCAP: frame capture and replay (test aid, lane gfx-cap) */
#include "core/bwg_core.h"         /* the API-neutral core (docs/gfx/arch.md 3.2, lane gfx-core) */
#include "core/bwg_pace.h"
#include "core/bwg_gate.h"         /* the latency mode's shared parts (lane gfx-lat11) */
#include "core/bwg_state.h"
#include "core/bwg_surf.h"
#include "core/bwg_tex.h"
#include "core/bwg_draw.h"
#include "bw_gfxcls.h"             /* BW_GFXCAP_CLASSES: the replay's region masks (test aid, lane gfx-capef) */
#include "core/bwg_ideas.h"        /* the adopted ideas I2, I3, I5 (arch.md 3.14, lane gfx-ideas) */
#include "bw_edgeprobe.h"          /* BW_GPU_EDGEPROBE: the edge-pixel residual's probe (test aid, lane gfx-ideas) */
#include "core/bwg_latch.h"        /* I1, the CPU-write latch (lane gfx-regen) */
#include "bw_vshpre.h"             /* the game's vertex programs compiled at load time (lane gfx-regen) */
extern unsigned g_bw_ctx_epoch;    /* I3 (BW_GPU_DELTA): defined with its shadow, below */

extern ptrdiff_t g_xbox_mem_offset;
extern void xbox_FramebufferWindowSet(uint32_t va, uint32_t pitch);
extern void xbox_FramebufferWindowPresent(uint32_t va, uint32_t pitch);
extern uint32_t xbox_ContiguousBlockSize(uint32_t va);

#define GUEST(va) BWG_GUEST(va)

/* (The guest block at va: bwg_block_size, the core's.) */

/* ---- device and pipeline ------------------------------------------------ */

static ID3D11Device *s_dev;
static ID3D11DeviceContext *s_ctx;
static ID3D11VertexShader *s_vs;
static ID3D11PixelShader *s_ps;
static ID3D11InputLayout *s_layout;
static ID3D11Buffer *s_cb, *s_vb;
static UINT s_vb_bytes;
static int s_failed;

/* (BwConsts, the screen-space path's constants: bwg_draw.h.) */

static const char s_hlsl[] =
"cbuffer C : register(b0) { float4 surf; float4 texinfo; float4 alpha; float4 depth;\n"
"#ifdef BW_WS\n"
"  float4 wsx;\n"
"#endif\n"
"};\n"
"struct VSIn { float4 p : POSITION; float4 c : COLOR; float2 uv : TEXCOORD; };\n"
"struct PSIn { float4 p : SV_Position; float4 c : COLOR; float2 uv : TEXCOORD; };\n"
"PSIn vs_main(VSIn i) {\n"
"  PSIn o;\n"
"  float w = (i.p.w > 0.0) ? 1.0 / i.p.w : 1.0;\n"
"  float x = i.p.x * surf.z * 2.0 - 1.0;\n"
BW_WS_REMAP_HLSL
"  float y = 1.0 - i.p.y * surf.w * 2.0;\n"
"  float zr = (i.p.z - depth.x) * depth.y;\n"
"  float z = depth.z > 0.5 ? saturate(zr) : zr;\n"
"  o.p = float4(x * w, y * w, z * w, w);\n"
"  o.c = i.c;\n"
"  o.uv = i.uv * texinfo.xy;\n"
"  return o;\n"
"}\n"
"Texture2D t0 : register(t0);\n"
"SamplerState s0 : register(s0);\n"
"float4 ps_main(PSIn i) : SV_Target {\n"
"  float4 c = i.c;\n"
"  if (texinfo.z > 0.5) c *= t0.Sample(s0, i.uv);\n"
"  if (alpha.z > 0.5) {\n"
"    float a = c.a, r = alpha.y; int f = (int)alpha.x;\n"
"    bool ok = (f == 7) || (f == 1 && a < r) || (f == 2 && abs(a - r) < 0.002)\n"
"           || (f == 3 && a <= r) || (f == 4 && a > r)\n"
"           || (f == 5 && abs(a - r) >= 0.002) || (f == 6 && a >= r);\n"
"    if (!ok) discard;\n"
"  }\n"
"  return c;\n"
"}\n";

/* ---- surfaces ----------------------------------------------------------- */

/* The guest surfaces and depth buffers are the core's (bwg_surf.c); their
 * D3D11 objects are these back end's (BwgSurf's and BwgDepth's tex, rtv,
 * srv, mtex, mrtv and tex, dsv, srv). */
typedef BwgSurf BwSurf;
static void latch_init(void);           /* I1 (lane gfx-regen), below the flip */
static void vshpre_known_ffp(void);     /* the load-time compile's fixed-function shapes, below get_ffp */
static int s_rb_force_sync;             /* I1's forced read: a waiting readback whatever the mode */
typedef BwgDepth BwDepth;
static ID3D11Texture2D *s_staging;
static uint32_t s_staging_w, s_staging_h, s_staging_bpp;

/* 0.2.5 item 36: a resource the device could not make (most likely out of
 * video memory: Remastered at a high render scale with MSAA on a small
 * card). Each caller releases what it had made and leaves its slot empty,
 * so nothing half-made is drawn with or leaked. The first failure of each
 * kind is logged with what happens instead; later ones are counted, and
 * the counts go into the BW_GPU_STATS line once there are any. */
enum { RF_SURFACE, RF_SURF_VIEW, RF_MSAA, RF_SEED, RF_DEPTH, RF_DEPTH_VIEW, RF_DEPTH_SRV,
       RF_TEXTURE, RF_TEX_VIEW, RF_SCENE, RF_N };
static unsigned s_rfail[RF_N];
static unsigned s_rfail_any;

static void res_failed(int kind, uint32_t va, uint32_t w, uint32_t h, HRESULT hr)
{
    static const char *const what[RF_N] = {
        "colour surface", "colour surface's view", "multisampled (MSAA) colour target",
        "seed copy of a colour surface", "depth buffer", "depth buffer's view",
        "depth buffer's texture view", "texture", "texture's view", "scene copy (Remastered effects)"
    };
    static const char *const then[RF_N] = {
        "draws into it are skipped; it is tried again next frame",
        "draws into it are skipped; it is tried again next frame",
        "that surface and its depth buffer are drawn without anti-aliasing",
        "the surface starts black instead of with what the game left in it",
        "draws that use it go without a depth buffer; it is tried again next frame",
        "draws that use it go without a depth buffer; it is tried again next frame",
        "ambient occlusion is skipped where that depth buffer is used",
        "draws with it are untextured; it is tried again when next used",
        "draws with it are untextured; it is tried again when next used",
        "bloom, ambient occlusion and OLED care skip that frame"
    };
    s_rfail_any++;
    if (s_rfail[kind]++)
        return;
    fprintf(stderr, "[GPU-D3D11] could not make a %s for %08X (%ux%u): error 0x%08lX%s; %s."
                    " Later failures of this kind are counted, not logged\n",
            what[kind], va, w, h, (unsigned long)hr,
            hr == E_OUTOFMEMORY ? " (out of video memory)" : "", then[kind]);
}

/* ---- textures ----------------------------------------------------------- */

/* The texture cache is the core's (bwg_tex.c); its D3D11 objects are this
 * back end's (BwgTex's tex and srv). */
typedef BwgTex BwTex;

/* ---- state caches ------------------------------------------------------- */

/* The API objects of the core's state tables (bwg_state.c), by its slots. */
static ID3D11BlendState *s_blend[BWG_MAX_STATE];
static ID3D11DepthStencilState *s_ds[BWG_MAX_STATE];
static ID3D11SamplerState *s_samp[BWG_MAX_STATE];
static ID3D11RasterizerState *s_rs;
static ID3D11RasterizerState *s_rs_cull[2][2];   /* [cull back][front is CCW] */
static ID3D11RasterizerState *s_rs_z[2][3][2];    /* [depth clip][none/front/back][ccw] */
static ID3D11RasterizerState *s_rs_bias[BWG_MAX_RS_BIAS];

/* ---- stats -------------------------------------------------------------- */

static unsigned s_st_xdraws;   /* combiner path, below */
static int s_nps;
static unsigned s_ps_failures;   /* item 41: combiner pixel shaders that failed (get_rc_shader) */
/* GPU vertex programs, below: draws, batches left to the CPU while their
 * shader compiles, constant uploads, shaders compiled and failed. */
static unsigned s_st_vdraws, s_st_vfall, s_st_kup, s_st_vraw, s_st_kskip;   /* vraw: P1 raw streams */
static unsigned s_st_spr_draws, s_st_spr_pts, s_st_spr_fall;   /* gfx-sprites: point batches on the GPU */
static volatile LONG s_nvsh, s_vsh_compiles, s_vsh_failures;
static double s_vsh_compile_ms;
static int s_stats = -1;
static unsigned s_st_draws, s_st_tris, s_st_flips, s_st_clears;   /* uploads, rtt: the core's (bwg_st_*) */
static unsigned s_st_presented, s_st_dropped;    /* gpu_present: shown, dropped (DO_NOT_WAIT) */
static unsigned s_st_rb_async_late;     /* VFX (D9): async readback frames dropped (ring full) */
static unsigned s_rb_dropped;           /* I2: the same, never zeroed (its [GPU-RBRING] line) */
/* Frame timing on the NV2A thread, per stats second (ms): the present and the
 * readback (sum and worst), and flip to flip (best, sum, worst). */
static double s_st_pres_ms, s_st_pres_max, s_st_rb_ms, s_st_rb_max;
static double s_st_ft_min, s_st_ft_sum, s_st_ft_max;
static unsigned s_st_ft_n;
static ULONGLONG s_st_t0;

/* T1 test aid, BW_TEXRESTORE_TRACE=<ms>: for every frame longer than that,
 * where the device thread's time went (Remastered only; inert unset). */
static double s_trc_on = -1.0;
static double s_trc_prep, s_trc_draw, s_trc_up, s_trc_pump, s_trc_pres, s_trc_rb;
static double s_trc_sh, s_trc_trs, s_trc_tex, s_trc_map, s_trc_reqt;
static unsigned s_trc_ndraw, s_trc_nup, s_trc_first, s_trc_req, s_trc_nsh;
static int trc_on(void)
{
    if (s_trc_on < 0.0) {
        const char *e = getenv("BW_TEXRESTORE_TRACE");
        s_trc_on = e ? atof(e) : 0.0;
    }
    return s_trc_on > 0.0;
}

/* 0.2.5 item 43: the back end's own CPU time on the NV2A thread, per flip
 * (BW_GPU_STATS's "draw cpu"): the thread's cycles inside clear and the
 * three draw entry points (QueryThreadCycleTime, so time the thread spends
 * preempted by other work on a busy PC isn't counted), in ms at the TSC's
 * rate, measured against QPC over each stats second. */
static ULONG64 s_st_draw_cyc;
static unsigned __int64 s_st_tsc0;
static double s_st_q0;
static unsigned s_st_xcbup;     /* item 43: combiner constant uploads (the rest were unchanged) */

/* (Item 43's per-draw shortcuts, BW_GPU_MEMO: bwg_memo_on, the core's.) */

static ULONG64 thread_cycles(void)
{
    ULONG64 c = 0;
    QueryThreadCycleTime(GetCurrentThread(), &c);
    return c;
}

/* One call in 8 is timed (and counted 8 times): QueryThreadCycleTime is a
 * system call, and timing every draw cost about as much as the draws. */
static unsigned s_dc_n;
static ULONG64 draw_cpu_begin(void)
{
    return (s_stats > 0 && !(++s_dc_n & 7)) ? thread_cycles() : 0;
}

static void draw_cpu_end(ULONG64 c0)
{
    if (c0)
        s_st_draw_cyc += (thread_cycles() - c0) * 8;
}

static double draw_cpu_ms_per_flip(void)
{
    unsigned __int64 t = __rdtsc();
    double q = bwg_qpc_ms(), ms = 0.0;
    if (s_st_q0 > 0.0 && q > s_st_q0 && s_st_flips) {
        double per_ms = (double)(t - s_st_tsc0) / (q - s_st_q0);
        if (per_ms > 0.0)
            ms = (double)s_st_draw_cyc / per_ms / s_st_flips;
    }
    s_st_tsc0 = t;
    s_st_q0 = q;
    s_st_draw_cyc = 0;
    return ms;
}

static void lat_stats_line(void);     /* P2b: BW_GPU_LATENCY, below */
static void pstat_line(void);          /* P2b: presentation statistics, below */
static void gate_screen(uint32_t flip); /* gfx-lat11: the latency mode's horizon, below */
static void ors_stats_line(void);      /* 0.2.5 item 49: the sea's single reflection, below */
static void native_stats_line(void);   /* gfx-ffp: GPU_FFP, GPU_VSH_SYNC, below */

static void stats_tick(void)
{
    ULONGLONG now;
    if (s_stats < 0) {
        const char *e = getenv("BW_GPU_STATS");
        s_stats = e && *e && *e != '0';
    }
    if (!s_stats)
        return;
    now = GetTickCount64();
    if (!s_st_t0)
        s_st_t0 = now;
    if (now - s_st_t0 >= 1000) {
        fprintf(stderr, "[GPU-D3D11] flips %u draws %u tris %u clears %u uploads %u rtt %u"
                        " surfaces %d textures %d | combiner draws %u shaders %d (%u failed) xup %u"
                        " | vsh draws %u raw %u cpu %u kup %u kskip %u programs %ld (%ld failed, %.0f ms)"
                        " | presented %u dropped %u | present %.2f/%.1f ms readback %.2f/%.1f ms"
                        " | draw cpu %.2f ms/flip"
                        " | ft %.1f/%.2f/%.1f ms\n",
                s_st_flips, s_st_draws, s_st_tris, s_st_clears, bwg_st_uploads, bwg_st_rtt,
                bwg_surf_count(), bwg_tex_count(), s_st_xdraws, s_nps, s_ps_failures, s_st_xcbup, s_st_vdraws, s_st_vraw, s_st_vfall, s_st_kup, s_st_kskip,
                (long)s_nvsh, (long)s_vsh_failures, s_vsh_compile_ms,
                s_st_presented, s_st_dropped,
                s_st_flips ? s_st_pres_ms / s_st_flips : 0.0, s_st_pres_max,
                s_st_flips ? s_st_rb_ms / s_st_flips : 0.0, s_st_rb_max,
                draw_cpu_ms_per_flip(),
                s_st_ft_n ? s_st_ft_min : 0.0, s_st_ft_n ? s_st_ft_sum / s_st_ft_n : 0.0,
                s_st_ft_max);
        if (bw_vfx_active())             /* VFX (D9) */
            fprintf(stderr, "[GPU-VFX] render scale %.3f, MSAA %ux, effects %.2f ms GPU per frame,"
                            " async readback dropped %u\n",
                    bw_vfx_scale(), bw_vfx_msaa(), bw_vfx_post_ms(), s_st_rb_async_late);
        {
            static unsigned told, told_reuse;   /* items 36-37: when there are new ones */
            if (bwg_surf_reused + bwg_depth_reused != told_reuse) {
                told_reuse = bwg_surf_reused + bwg_depth_reused;
                fprintf(stderr, "[GPU-D3D11] slots reused so far (tables full): colour surfaces %u,"
                                " depth buffers %u\n", bwg_surf_reused, bwg_depth_reused);
            }
            if (s_rfail_any != told) {
                told = s_rfail_any;
                fprintf(stderr, "[GPU-D3D11] resources not made so far: surfaces %u (views %u, MSAA %u,"
                                " seeds %u) depth %u (views %u, texture views %u) textures %u (views %u)"
                                " scene copies %u\n",
                        s_rfail[RF_SURFACE], s_rfail[RF_SURF_VIEW], s_rfail[RF_MSAA], s_rfail[RF_SEED],
                        s_rfail[RF_DEPTH], s_rfail[RF_DEPTH_VIEW], s_rfail[RF_DEPTH_SRV],
                        s_rfail[RF_TEXTURE], s_rfail[RF_TEX_VIEW], s_rfail[RF_SCENE]);
            }
        }
        lat_stats_line();                /* P2b */
        pstat_line();
        ors_stats_line();                /* item 49 (Remastered only: no-op otherwise) */
        if (s_st_spr_draws || s_st_spr_fall) {   /* gfx-sprites: only once a point batch went the GPU way */
            fprintf(stderr, "[GPU-SPRITES] point batches on the GPU %u (%u points), handed back to the CPU"
                            " %u (their shader still compiling)\n", s_st_spr_draws, s_st_spr_pts,
                    s_st_spr_fall);
            s_st_spr_draws = s_st_spr_pts = s_st_spr_fall = 0;
        }
        native_stats_line();             /* gfx-ffp (only with a switch on) */
        s_st_rb_async_late = 0;
        s_st_presented = s_st_dropped = 0;
        s_st_pres_ms = s_st_pres_max = s_st_rb_ms = s_st_rb_max = 0.0;
        s_st_ft_min = s_st_ft_sum = s_st_ft_max = 0.0;
        s_st_ft_n = 0;
        s_st_xcbup = 0;
        s_st_xdraws = s_st_vdraws = s_st_vfall = s_st_kup = s_st_vraw = s_st_kskip = 0;
        s_st_flips = s_st_draws = s_st_tris = bwg_st_uploads = bwg_st_rtt = s_st_clears = 0;
        s_st_t0 = now;
    }
}

/* ---- setup -------------------------------------------------------------- */

/* The vertex shaders' defines: BW_WS (the widescreen x remap) only when this
 * boot is widescreen, so Original compiles exactly the text it always did. */
static const D3D_SHADER_MACRO s_ws_defines[] = { { "BW_WS", "1" }, { NULL, NULL } };
#define WS_DEFINES (bwg_ws_k > 1.0f ? s_ws_defines : NULL)

static int compile(const char *entry, const char *target, ID3DBlob **out)
{
    ID3DBlob *err = NULL;
    HRESULT hr = D3DCompile(s_hlsl, sizeof(s_hlsl) - 1, "bw_d3d11", WS_DEFINES, NULL,
                            entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out, &err);
    if (FAILED(hr)) {
        fprintf(stderr, "[GPU-D3D11] shader %s failed: %s\n", entry,
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
        if (err) ID3D10Blob_Release(err);
        return 0;
    }
    if (err) ID3D10Blob_Release(err);
    return 1;
}

/* ---- when the GPU can't go on (0.2.5 items 26 and 28) --------------------
 * Before 0.2.5 a device that couldn't be made, or one the driver took away,
 * left every draw doing nothing: a black or frozen window and one stderr
 * line. Now the player is told in plain words and the boot ends cleanly:
 * one [GPU] FATAL line, the toolkit's exit-time reports (as closing the
 * window runs them), a message box (not with BW_UNATTENDED), then the
 * process ends with its own exit code, which the play host reports. */
extern void recomp_exit_hook_run(const char *why);     /* src/platform (toolkit) */

#define BW_EXIT_GPU_UNSUPPORTED 20      /* no usable D3D11 device (item 26) */
#define BW_EXIT_GPU_LOST        21      /* the driver removed or reset the device (item 28) */

static void gpu_fatal(unsigned code, const char *msg)
{
    static volatile LONG once;
    char line[600];
    size_t i;
    if (InterlockedExchange(&once, 1))
        return;
    s_failed = 1;                       /* draws stop here: ensure_device says no */
    for (i = 0; msg[i] && i < sizeof line - 1; i++)
        line[i] = msg[i] == '\n' ? ' ' : msg[i];
    line[i] = 0;
    fprintf(stderr, "[GPU] FATAL: %s (this boot ends, exit code %u)\n", line, code);
    fflush(stderr);
    recomp_exit_hook_run("gpu");
    if (!getenv("BW_UNATTENDED"))
        MessageBoxA(NULL, msg, "Blood Wake",
                    MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
    fflush(stdout);
    fflush(stderr);
    {
        extern void bw_log_drain(int wait_ms);   /* au-vp: the log's ring to the file first (main.c) */
        bw_log_drain(500);
    }
    TerminateProcess(GetCurrentProcess(), code);
}

/* "11_0" for D3D_FEATURE_LEVEL_11_0 */
static const char *fl_name(D3D_FEATURE_LEVEL fl, char buf[8])
{
    _snprintf(buf, 8, "%u_%u", ((unsigned)fl >> 12) & 0xF, ((unsigned)fl >> 8) & 0xF);
    buf[7] = 0;
    return buf;
}

/* The default adapter's name and the highest feature level it offers, for
 * the message when the device can't be made. */
static void gpu_best_level(char *name, size_t nlen, D3D_FEATURE_LEVEL *best)
{
    static const D3D_FEATURE_LEVEL all[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3, D3D_FEATURE_LEVEL_9_2, D3D_FEATURE_LEVEL_9_1
    };
    IDXGIFactory1 *f = NULL;
    IDXGIAdapter1 *a = NULL;
    *best = (D3D_FEATURE_LEVEL)0;
    _snprintf(name, nlen, "unknown");
    name[nlen - 1] = 0;
    if (SUCCEEDED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&f))) {
        DXGI_ADAPTER_DESC1 d;
        if (SUCCEEDED(IDXGIFactory1_EnumAdapters1(f, 0, &a)) && SUCCEEDED(IDXGIAdapter1_GetDesc1(a, &d))) {
            WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, (int)nlen, NULL, NULL);
            name[nlen - 1] = 0;
        }
        if (a) IDXGIAdapter1_Release(a);
        IDXGIFactory1_Release(f);
    }
    /* No device is made (NULL out): this only asks which level would be. A
     * runtime without 11_1 refuses the whole list, so ask again without it. */
    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, all, 7,
                                 D3D11_SDK_VERSION, NULL, best, NULL)))
        if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, all + 1, 6,
                                     D3D11_SDK_VERSION, NULL, best, NULL)))
            *best = (D3D_FEATURE_LEVEL)0;
}

/* Item 26: no device. Say why, in words a player can act on. */
static void gpu_unsupported(HRESULT hr)
{
    char name[256], lv[8], msg[700];
    D3D_FEATURE_LEVEL best;
    gpu_best_level(name, sizeof name, &best);
    fprintf(stderr, "[GPU] no Direct3D 11 device: 0x%08lX; adapter \"%s\", feature level %s at most\n",
            (unsigned long)hr, name, best ? fl_name(best, lv) : "none");
    if (hr == DXGI_ERROR_UNSUPPORTED || (best && best < D3D_FEATURE_LEVEL_11_0))
        _snprintf(msg, sizeof msg,
                  "Blood Wake can't start: it needs a graphics card that supports Direct3D 11"
                  " (feature level 11_0).\n\nThis PC's graphics: %s (feature level %s at most).\n\n"
                  "Updating the graphics driver may help.",
                  name, best ? fl_name(best, lv) : "unknown");
    else
        _snprintf(msg, sizeof msg,
                  "Blood Wake can't start its graphics (error 0x%08lX).\n\nThis PC's graphics: %s.\n\n"
                  "Updating the graphics driver may help.", (unsigned long)hr, name);
    msg[sizeof msg - 1] = 0;
    gpu_fatal(BW_EXIT_GPU_UNSUPPORTED, msg);
}

/* Item 28: the driver removed or reset the device (a driver update or
 * crash, a TDR, the GPU unplugged). Nothing can draw again on this device;
 * rebuilding it in place is a later item. Called with the result of the
 * calls that report it (Present, ResizeBuffers, GetBuffer, the swap chain's
 * creation, the readback's Map, the frame query); returns 0 for any other
 * result, so callers carry on as before. */
static const char *dxgi_err_name(HRESULT hr)
{
    switch ((unsigned long)hr) {
    case 0x00000000UL: return "S_OK";
    case 0x887A0005UL: return "DXGI_ERROR_DEVICE_REMOVED";
    case 0x887A0006UL: return "DXGI_ERROR_DEVICE_HUNG";
    case 0x887A0007UL: return "DXGI_ERROR_DEVICE_RESET";
    case 0x887A0020UL: return "DXGI_ERROR_DRIVER_INTERNAL_ERROR";
    case 0x887A0001UL: return "DXGI_ERROR_INVALID_CALL";
    case 0x8007000EUL: return "E_OUTOFMEMORY";
    default:           return "other";
    }
}

static int gpu_lost(HRESULT hr, const char *where)
{
    HRESULT why;
    if (hr != DXGI_ERROR_DEVICE_REMOVED && hr != DXGI_ERROR_DEVICE_RESET)
        return 0;
    why = s_dev ? ID3D11Device_GetDeviceRemovedReason(s_dev) : E_FAIL;
    fprintf(stderr, "[GPU] the graphics device was lost (%s returned 0x%08lX %s); removed reason"
                    " 0x%08lX %s%s\n", where, (unsigned long)hr, dxgi_err_name(hr),
            (unsigned long)why, dxgi_err_name(why),
            why == S_OK && getenv("BW_GPU_FAILTEST") ? " (simulated: BW_GPU_FAILTEST)" : "");
    gpu_fatal(BW_EXIT_GPU_LOST, "The graphics driver restarted. Please restart Blood Wake.");
    return 1;
}

/* ---- which GPU (0.2.5 item 27) --------------------------------------------
 * A laptop with an integrated and a dedicated GPU used to get the default
 * adapter, which the driver may make the integrated one. Now:
 *   - these two exports ask NVIDIA Optimus and AMD PowerXpress drivers for
 *     the dedicated GPU (they read them from the exe's export table; any
 *     file of the exe may hold them, call B1-3);
 *   - the device is made on DXGI's high-performance adapter
 *     (IDXGIFactory6::EnumAdapterByGpuPreference), and on the default one
 *     as before if that isn't there or refuses;
 *   - one line names the adapter (contract 8; A2's bug-report bundle copies it).
 * BW_GPU_ADAPTER (test aid, call B1-4): "warp" makes the device on
 * Microsoft's software GPU (WARP, crash and correctness checks: no AMD or
 * Intel GPU is here), "default" the default adapter as before 0.2.5. */
__declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;

static int s_adapter_mode = -1;         /* 0 high performance, 1 default (old), 2 WARP */

static int adapter_mode(void)
{
    if (s_adapter_mode < 0) {
        const char *e = getenv("BW_GPU_ADAPTER");
        s_adapter_mode = !e || !*e ? 0 : !_stricmp(e, "warp") ? 2 : !_stricmp(e, "default") ? 1 : 0;
        if (s_adapter_mode)
            fprintf(stderr, "[GPU] BW_GPU_ADAPTER=%s: the device goes on %s (test aid)\n", e,
                    s_adapter_mode == 2 ? "Microsoft's software GPU (WARP)" : "the default adapter");
    }
    return s_adapter_mode;
}

/* DXGI's high-performance adapter, or NULL (the default one, as before). */
static IDXGIAdapter1 *high_perf_adapter(void)
{
    IDXGIFactory6 *f6 = NULL;
    IDXGIAdapter1 *a = NULL;
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory6, (void **)&f6)))
        return NULL;                    /* before Windows 10 1803: the default adapter */
    if (FAILED(IDXGIFactory6_EnumAdapterByGpuPreference(f6, 0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                        &IID_IDXGIAdapter1, (void **)&a)))
        a = NULL;
    IDXGIFactory6_Release(f6);
    return a;
}

static HRESULT create_device_on(IDXGIAdapter1 *ad, D3D_DRIVER_TYPE dt, UINT flags,
                                D3D_FEATURE_LEVEL *got)
{
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    return D3D11CreateDevice((IDXGIAdapter *)ad, ad ? D3D_DRIVER_TYPE_UNKNOWN : dt, NULL, flags,
                             &fl, 1, D3D11_SDK_VERSION, &s_dev, got, &s_ctx);
}

/* Contract 8: [GPU] adapter "<name>" vendor 0x<id> device 0x<id> vram <MB> MB
 * feature level <x_y> driver <version> */
static void adapter_line(D3D_FEATURE_LEVEL got)
{
    IDXGIDevice *xd = NULL;
    IDXGIAdapter *ad = NULL;
    DXGI_ADAPTER_DESC d;
    LARGE_INTEGER umd;
    char name[256], drv[48], lv[8];
    if (FAILED(ID3D11Device_QueryInterface(s_dev, &IID_IDXGIDevice, (void **)&xd)))
        return;
    if (SUCCEEDED(IDXGIDevice_GetAdapter(xd, &ad)) && SUCCEEDED(IDXGIAdapter_GetDesc(ad, &d))) {
        WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, (int)sizeof name, NULL, NULL);
        name[sizeof name - 1] = 0;
        if (SUCCEEDED(IDXGIAdapter_CheckInterfaceSupport(ad, &IID_IDXGIDevice, &umd)))
            _snprintf(drv, sizeof drv, "%u.%u.%u.%u", (unsigned)HIWORD(umd.HighPart),
                      (unsigned)LOWORD(umd.HighPart), (unsigned)HIWORD(umd.LowPart),
                      (unsigned)LOWORD(umd.LowPart));
        else
            _snprintf(drv, sizeof drv, "unknown");
        drv[sizeof drv - 1] = 0;
        fprintf(stderr, "[GPU] adapter \"%s\" vendor 0x%04X device 0x%04X vram %u MB feature level %s driver %s\n",
                name, (unsigned)d.VendorId, (unsigned)d.DeviceId,
                (unsigned)(d.DedicatedVideoMemory / (1024 * 1024)), fl_name(got, lv), drv);
    }
    if (ad) IDXGIAdapter_Release(ad);
    IDXGIDevice_Release(xd);
}

/* BW_TEST_GPUPRIO=<-7..7>[:priv] (a test aid, inert unless set; gfx4 lane
 * gfx-proofs, asked for by the maintainer): DXGI's GPU thread priority for this
 * device (IDXGIDevice::SetGPUThreadPriority), the GPU's counterpart of the
 * process power-mode opt-out: how the GPU scheduler weighs this device's work
 * against other processes' work. Set right after the device is made, read back
 * with GetGPUThreadPriority, every step logged ([GPU-PRIO]). ":priv" first
 * enables SeIncreaseBasePriorityPrivilege in this process's own token, if the
 * token holds it (nothing is granted; nothing outside the process changes). */
static void gpu_prio_priv(void)
{
    HANDLE tok = NULL;
    TOKEN_PRIVILEGES tp;
    DWORD err;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) {
        fprintf(stderr, "[GPU-PRIO] the process token can't be opened (error %lu)\n", GetLastError());
        return;
    }
    memset(&tp, 0, sizeof tp);
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!LookupPrivilegeValueA(NULL, "SeIncreaseBasePriorityPrivilege", &tp.Privileges[0].Luid)) {
        fprintf(stderr, "[GPU-PRIO] SeIncreaseBasePriorityPrivilege: no such privilege (error %lu)\n",
                GetLastError());
    } else {
        AdjustTokenPrivileges(tok, FALSE, &tp, 0, NULL, NULL);
        err = GetLastError();
        fprintf(stderr, "[GPU-PRIO] SeIncreaseBasePriorityPrivilege: %s (error %lu)\n",
                err == ERROR_SUCCESS ? "enabled in this process's token"
                : err == ERROR_NOT_ALL_ASSIGNED ? "this process's token doesn't hold it" : "not enabled", err);
    }
    CloseHandle(tok);
}

static void gpu_prio_test(void)
{
    const char *e = getenv("BW_TEST_GPUPRIO");
    IDXGIDevice *xd = NULL;
    INT want, before = 99, after = 99;
    HRESULT hb, hs, ha;
    char *end;
    if (!e || !*e)
        return;
    want = (INT)strtol(e, &end, 10);
    if (end == e || want < -7 || want > 7 || (*end && _stricmp(end, ":priv"))) {
        fprintf(stderr, "[GPU-PRIO] BW_TEST_GPUPRIO=%s: want -7..7, optionally with :priv (ignored)\n", e);
        return;
    }
    if (*end)
        gpu_prio_priv();
    if (FAILED(ID3D11Device_QueryInterface(s_dev, &IID_IDXGIDevice, (void **)&xd))) {
        fprintf(stderr, "[GPU-PRIO] no IDXGIDevice on this device: nothing set\n");
        return;
    }
    hb = IDXGIDevice_GetGPUThreadPriority(xd, &before);
    hs = IDXGIDevice_SetGPUThreadPriority(xd, want);
    ha = IDXGIDevice_GetGPUThreadPriority(xd, &after);
    fprintf(stderr, "[GPU-PRIO] test aid BW_TEST_GPUPRIO=%s: priority before %d (0x%08lX); "
                    "SetGPUThreadPriority(%d) 0x%08lX; read back %d (0x%08lX)%s\n",
            e, before, (unsigned long)hb, want, (unsigned long)hs, after, (unsigned long)ha,
            SUCCEEDED(hs) && SUCCEEDED(ha) && after == want ? ": in effect" : ": NOT in effect");
    IDXGIDevice_Release(xd);
}

static int ensure_device(void)
{
    D3D_FEATURE_LEVEL got;
    ID3DBlob *vsb = NULL, *psb = NULL;
    HRESULT hr;
    D3D11_BUFFER_DESC bd;
    D3D11_RASTERIZER_DESC rd;
    D3D11_INPUT_ELEMENT_DESC il[3] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_B8G8R8A8_UNORM,     0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };

    if (s_failed)
        return 0;
    if (s_dev)
        return 1;
    if (bw_gpu_failtest("device")) {            /* item 25 test aid */
        hr = DXGI_ERROR_UNSUPPORTED;
    } else {
        UINT flags = D3D11_CREATE_DEVICE_SINGLETHREADED;
        int mode = adapter_mode();
        D3D_DRIVER_TYPE dt = mode == 2 ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE;
        IDXGIAdapter1 *ad = mode == 0 ? high_perf_adapter() : NULL;     /* item 27 */
        if (bw_gpu_debug_level())               /* item 25: BW_GPU_DEBUG, the debug layer */
            flags |= D3D11_CREATE_DEVICE_DEBUG;
        hr = create_device_on(ad, dt, flags, &got);
        if (FAILED(hr) && (flags & D3D11_CREATE_DEVICE_DEBUG)) {
            fprintf(stderr, "[GPU-DEBUG] no D3D11 debug layer (0x%08lX; it comes with the Windows"
                            " optional feature \"Graphics Tools\"); the device is made without it\n",
                    (unsigned long)hr);
            flags &= ~(UINT)D3D11_CREATE_DEVICE_DEBUG;
            hr = create_device_on(ad, dt, flags, &got);
        }
        if (FAILED(hr) && ad) {
            fprintf(stderr, "[GPU] the high-performance adapter refused a device (0x%08lX);"
                            " trying the default adapter\n", (unsigned long)hr);
            hr = create_device_on(NULL, D3D_DRIVER_TYPE_HARDWARE, flags, &got);
        }
        if (ad)
            IDXGIAdapter1_Release(ad);
        if (SUCCEEDED(hr)) {
            adapter_line(got);                  /* item 27, contract 8 */
            if (flags & D3D11_CREATE_DEVICE_DEBUG)
                bw_gpu_debug_attach(s_dev);
            gpu_prio_test();                    /* BW_TEST_GPUPRIO (test aid; inert unless set) */
        }
    }
    if (FAILED(hr)) {
        fprintf(stderr, "[GPU-D3D11] D3D11CreateDevice failed 0x%08lX\n", (unsigned long)hr);
        s_failed = 1;
        gpu_unsupported(hr);            /* item 26: the message, and the boot ends */
        return 0;
    }
    if (!compile("vs_main", "vs_5_0", &vsb) || !compile("ps_main", "ps_5_0", &psb)) {
        /* Nothing is drawn rather than drawn with half a pipeline. */
        if (vsb) ID3D10Blob_Release(vsb);
        ID3D11DeviceContext_Release(s_ctx);
        ID3D11Device_Release(s_dev);
        s_ctx = NULL;
        s_dev = NULL;
        s_failed = 1;
        gpu_fatal(BW_EXIT_GPU_UNSUPPORTED,      /* item 26: not a silent black window */
                  "Blood Wake can't start its graphics: its shaders didn't compile.\n\n"
                  "The details are in the game's log.");
        return 0;
    }
    ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(vsb),
                                    ID3D10Blob_GetBufferSize(vsb), NULL, &s_vs);
    ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(psb),
                                   ID3D10Blob_GetBufferSize(psb), NULL, &s_ps);
    ID3D11Device_CreateInputLayout(s_dev, il, 3, ID3D10Blob_GetBufferPointer(vsb),
                                   ID3D10Blob_GetBufferSize(vsb), &s_layout);
    ID3D10Blob_Release(vsb);
    ID3D10Blob_Release(psb);

    memset(&bd, 0, sizeof(bd));
    bd.ByteWidth = sizeof(BwConsts);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_cb);

    memset(&rd, 0, sizeof(rd));
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = FALSE;
    rd.ScissorEnable = TRUE;            /* the title's window clip, see apply_scissor */
    ID3D11Device_CreateRasterizerState(s_dev, &rd, &s_rs);
    fprintf(stderr, "[GPU-D3D11] device ready (feature level 0x%x)\n", (unsigned)got);
    bw_pscache_init();                  /* T1: compiled pixel shaders from earlier runs (every edition) */
    latch_init();                       /* I1, the CPU-write latch (lane gfx-regen) */
    bw_vshpre_start(bwg_sprites_on());  /* the game's vertex programs into the cache now (lane gfx-regen) */
    vshpre_known_ffp();                 /* and, with GPU_FFP, the fixed function's shapes */
    /* VFX (D9): the visual suite's settings and helpers; nothing in Original. */
    bw_vfx_init();
    bw_vfx_gpu_init(s_dev, s_ctx);
    if (bw_vfx_active() && bw_vfx()->tex_restore)
        bw_tr_init();                   /* T1: its shader, now rather than at the first 3D draw */
    bw_gpu_debug_drain();               /* item 25: BW_GPU_DEBUG's messages from the set-up */
    return 1;
}

/* ---- surfaces ----------------------------------------------------------- */

/* The rasteriser states, made from the core's descriptions (bwg_state.c:
 * the depth range, back-face culling and the polygon offset). */
static void d11_raster_desc(const BwgRaster *r, D3D11_RASTERIZER_DESC *rd)
{
    BwgRasterDesc g;
    bwg_raster_desc(r, &g);
    memset(rd, 0, sizeof(*rd));
    rd->FillMode = D3D11_FILL_SOLID;
    rd->CullMode = (D3D11_CULL_MODE)g.cull_mode;
    rd->FrontCounterClockwise = g.front_ccw ? TRUE : FALSE;
    rd->DepthClipEnable = g.depth_clip ? TRUE : FALSE;
    rd->ScissorEnable = TRUE;
    rd->DepthBias = (INT)g.depth_bias;
    rd->SlopeScaledDepthBias = g.slope_bias;
    rd->DepthBiasClamp = g.bias_clamp;
}

static ID3D11RasterizerState *get_rs_z(int clip, int cull, int ccw)
{
    if (!s_rs_z[clip][cull][ccw]) {
        D3D11_RASTERIZER_DESC rd;
        BwgRaster r;
        memset(&r, 0, sizeof(r));
        r.kind = BWG_RS_Z;
        r.clip = clip; r.cull = cull; r.ccw = ccw;
        d11_raster_desc(&r, &rd);
        ID3D11Device_CreateRasterizerState(s_dev, &rd, &s_rs_z[clip][cull][ccw]);
    }
    return s_rs_z[clip][cull][ccw];
}

static ID3D11RasterizerState *get_rs_bias(const BwgRaster *r)
{
    D3D11_RASTERIZER_DESC rd;
    ID3D11RasterizerState *o = NULL;
    int i = bwg_rs_bias_find(r);
    if (i >= 0)
        return s_rs_bias[i];
    if (i == -2)                        /* the table is full */
        return get_rs_z(r->clip, r->cull, r->ccw);
    d11_raster_desc(r, &rd);
    if (FAILED(ID3D11Device_CreateRasterizerState(s_dev, &rd, &o)))
        return get_rs_z(r->clip, r->cull, r->ccw);
    s_rs_bias[bwg_rs_bias_add(r)] = o;
    return o;
}

/* Returns NULL when the title culls both faces: nothing to draw. */
static ID3D11RasterizerState *get_rs(const Nv2aRenderState *rs)
{
    BwgRaster r;
    if (!bwg_raster(rs, &r))
        return NULL;
    switch (r.kind) {
    case BWG_RS_BIAS:
        return get_rs_bias(&r);
    case BWG_RS_Z:
        return get_rs_z(r.clip, r.cull, r.ccw);
    case BWG_RS_PLAIN:
        return s_rs;
    default:
        break;
    }
    if (!s_rs_cull[r.cull][r.ccw]) {
        D3D11_RASTERIZER_DESC rd;
        d11_raster_desc(&r, &rd);
        ID3D11Device_CreateRasterizerState(s_dev, &rd, &s_rs_cull[r.cull][r.ccw]);
    }
    return s_rs_cull[r.cull][r.ccw] ? s_rs_cull[r.cull][r.ccw] : s_rs;
}

static void release_surf(BwSurf *s)
{
    bwg_latch_forget(s);                /* I1: its next seed reads guest memory, CPU writes and all */
    if (s->srv) ID3D11ShaderResourceView_Release(s->srv);
    if (s->rtv) ID3D11RenderTargetView_Release(s->rtv);
    if (s->tex) ID3D11Texture2D_Release(s->tex);
    s->srv = NULL; s->rtv = NULL; s->tex = NULL;
    if (s->mrtv) ID3D11RenderTargetView_Release(s->mrtv);      /* VFX (D9) */
    if (s->mtex) ID3D11Texture2D_Release(s->mtex);
    s->mrtv = NULL; s->mtex = NULL; s->mdirty = 0;
}

static DXGI_FORMAT surf_format(uint32_t bpp)
{
    return bpp == 2 ? DXGI_FORMAT_B5G6R5_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
}

/* VFX (D9): a surface at its physical (render-scaled) size, in the suite's
 * target format, with an MSAA target when that is on; seeded from guest
 * memory through a stretch blit. Remastered only. */
static int create_surf_vfx(BwSurf *s)
{
    D3D11_TEXTURE2D_DESC td;
    UINT msaa = bw_vfx_msaa();
    ID3D11RenderTargetView *seed_rtv;
    HRESULT hr;
    int seeded = 0;
    memset(&td, 0, sizeof(td));
    td.Width = s->pw;
    td.Height = s->ph;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = bw_vfx_target_format(s->bpp);
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    hr = BW_GPU_FT("surface", E_OUTOFMEMORY,                    /* item 25 test aid */
                   ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s->tex));
    if (FAILED(hr)) {
        s->tex = NULL;
        res_failed(RF_SURFACE, s->va, s->pw, s->ph, hr);        /* item 36 */
        return 0;
    }
    /* item 36: both views, or the surface isn't made (no NULL view bound) */
    if (FAILED(hr = BW_GPU_FT("resource", E_OUTOFMEMORY,
                              ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)s->tex, NULL, &s->rtv)))
        || FAILED(hr = BW_GPU_FT("resource", E_OUTOFMEMORY,
                                 ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)s->tex, NULL, &s->srv)))) {
        res_failed(RF_SURF_VIEW, s->va, s->pw, s->ph, hr);
        release_surf(s);
        return 0;
    }
    if (s->noaa_epoch == bw_vfx_epoch() + 1)
        msaa = 1;               /* item 36: no MSAA target at this render scale; tried again at the next */
    if (msaa > 1) {
        td.SampleDesc.Count = msaa;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        /* item 36: the MSAA target counts as a surface for the failure switch */
        hr = BW_GPU_FT("surface", E_OUTOFMEMORY, ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s->mtex));
        if (SUCCEEDED(hr))
            hr = BW_GPU_FT("resource", E_OUTOFMEMORY,
                           ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)s->mtex, NULL, &s->mrtv));
        if (FAILED(hr) || !s->mrtv) {
            /* item 36: this surface, and its depth buffer with it (begin asks
             * get_depth for its sample count), are single-sample instead:
             * drawn without anti-aliasing rather than not at all, and never
             * a single-sample target with a multisampled depth buffer. */
            if (s->mrtv) ID3D11RenderTargetView_Release(s->mrtv);
            if (s->mtex) ID3D11Texture2D_Release(s->mtex);
            s->mrtv = NULL;
            s->mtex = NULL;
            s->noaa_epoch = bw_vfx_epoch() + 1;
            res_failed(RF_MSAA, s->va, s->pw, s->ph, FAILED(hr) ? hr : E_FAIL);
            fprintf(stderr, "[VFX] surface %08X (%ux%u): no %ux MSAA target, so it and its depth buffer"
                            " are drawn without anti-aliasing\n", s->va, s->pw, s->ph, msaa);
        }
    }
    seed_rtv = s->mrtv ? s->mrtv : s->rtv;
    /* Seed: whatever the title (or the boot) left in guest memory, stretched. */
    if (s->pitch >= s->w * s->bpp) {
        D3D11_SUBRESOURCE_DATA init;
        ID3D11Texture2D *t = NULL;
        ID3D11ShaderResourceView *v = NULL;
        void *tmp;
        const void *src = bwg_seed_src(s, &tmp);                    /* item 38: never past the block */
        td.Width = s->w;
        td.Height = s->h;
        td.Format = surf_format(s->bpp);
        td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        init.pSysMem = src;
        init.SysMemPitch = s->pitch;
        init.SysMemSlicePitch = 0;
        if (src && SUCCEEDED(hr = ID3D11Device_CreateTexture2D(s_dev, &td, &init, &t))
            && SUCCEEDED(hr = ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)t, NULL, &v))) {
            bw_vfx_blit(v, s->w, s->h, seed_rtv, s->pw, s->ph, 0);
            seeded = 1;
        } else
            res_failed(RF_SEED, s->va, s->w, s->h, src ? hr : E_OUTOFMEMORY);   /* item 36: cleared instead */
        if (v) ID3D11ShaderResourceView_Release(v);
        if (t) ID3D11Texture2D_Release(t);
        free(tmp);
    }
    if (!seeded) {
        static const float zero[4] = { 0, 0, 0, 0 };
        ID3D11DeviceContext_ClearRenderTargetView(s_ctx, seed_rtv, zero);
    }
    s->mdirty = s->mrtv != NULL;
    s->epoch = bw_vfx_epoch();
    if (bwg_cur == s)
        bwg_cur = NULL;           /* rebind: the blit changed the targets, and these are new */
    return 1;
}

static int create_surf(BwSurf *s)
{
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA init, *pinit = NULL;
    HRESULT hr;
    void *tmp = NULL;                       /* item 38: a clamped seed's copy */
    if (bw_vfx_active())
        return create_surf_vfx(s);          /* VFX (D9) */
    if (s->wsk <= 1.0f) {                   /* widescreen keeps its wider pw */
        s->pw = s->w;
        s->ph = s->h;
    }
    memset(&td, 0, sizeof(td));
    td.Width = s->pw;
    td.Height = s->ph;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = surf_format(s->bpp);
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    /* Seed from guest memory: whatever the title (or the boot) left there,
     * never read past the end of its guest block (item 38). */
    if (s->pitch >= s->w * s->bpp && s->pw == s->w && s->ph == s->h
            && (init.pSysMem = bwg_seed_src(s, &tmp)) != NULL) {
        init.SysMemPitch = s->pitch;
        init.SysMemSlicePitch = 0;
        pinit = &init;
    }
    hr = BW_GPU_FT("surface", E_OUTOFMEMORY,                    /* item 25 test aid */
                   ID3D11Device_CreateTexture2D(s_dev, &td, pinit, &s->tex));
    free(tmp);
    if (FAILED(hr)) {
        s->tex = NULL;
        res_failed(RF_SURFACE, s->va, s->pw, s->ph, hr);        /* item 36 */
        return 0;
    }
    /* item 36: both views, or the surface isn't made (no NULL view bound) */
    if (FAILED(hr = BW_GPU_FT("resource", E_OUTOFMEMORY,
                              ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)s->tex, NULL, &s->rtv)))
        || FAILED(hr = BW_GPU_FT("resource", E_OUTOFMEMORY,
                                 ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)s->tex, NULL, &s->srv)))) {
        res_failed(RF_SURF_VIEW, s->va, s->pw, s->ph, hr);
        release_surf(s);
        return 0;
    }
    return 1;
}

static void vfx_surf_forget(BwSurf *s);        /* the scene copy's pointers, below */

static void release_depth(BwDepth *d)
{
    if (d->dsv) ID3D11DepthStencilView_Release(d->dsv);
    if (d->srv) ID3D11ShaderResourceView_Release(d->srv);
    if (d->tex) ID3D11Texture2D_Release(d->tex);
    d->dsv = NULL; d->srv = NULL; d->tex = NULL;
}

/* The depth buffer's objects, w x h with its samples (1 in Original; the
 * MSAA count in Remastered, or 1 for a surface whose MSAA target couldn't
 * be made, item 36); the core (bwg_get_depth) decides when. 0: couldn't
 * (logged here; the core empties the slot). */
static int create_depth(BwDepth *d)
{
    D3D11_TEXTURE2D_DESC td;
    HRESULT hr;
    int kind = RF_DEPTH;
    uint32_t va = d->va, w = d->w, h = d->h;
    UINT samples = d->samples;
    memset(&td, 0, sizeof(td));
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    td.SampleDesc.Count = samples;              /* VFX (D9): matches the colour target; 1 in Original */
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (bw_vfx_active() && bw_vfx()->ao > 0.0f) {
        /* VFX (D9): ambient occlusion samples the depth: a typeless texture
         * with a D24S8 depth view and an R24 shader view. */
        D3D11_DEPTH_STENCIL_VIEW_DESC dv;
        D3D11_SHADER_RESOURCE_VIEW_DESC sv;
        int ms = td.SampleDesc.Count > 1;
        td.Format = DXGI_FORMAT_R24G8_TYPELESS;
        td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(hr = BW_GPU_FT("depth", E_OUTOFMEMORY,      /* item 25 test aid */
                                  ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &d->tex))))
            goto fail;
        memset(&dv, 0, sizeof dv);
        dv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dv.ViewDimension = ms ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;
        kind = RF_DEPTH_VIEW;
        if (FAILED(hr = BW_GPU_FT("resource", E_OUTOFMEMORY,   /* item 36 */
                                  ID3D11Device_CreateDepthStencilView(s_dev, (ID3D11Resource *)d->tex, &dv, &d->dsv))))
            goto fail;
        memset(&sv, 0, sizeof sv);
        sv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        sv.ViewDimension = ms ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = 1;
        if (FAILED(hr = BW_GPU_FT("resource", E_OUTOFMEMORY,   /* item 36: the depth stays; AO skips it */
                                  ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)d->tex, &sv, &d->srv)))) {
            d->srv = NULL;
            res_failed(RF_DEPTH_SRV, va, w, h, hr);
        }
        return 1;
    }
    if (FAILED(hr = BW_GPU_FT("depth", E_OUTOFMEMORY,          /* item 25 test aid */
                              ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &d->tex))))
        goto fail;
    kind = RF_DEPTH_VIEW;
    if (FAILED(hr = BW_GPU_FT("resource", E_OUTOFMEMORY,       /* item 36 */
                              ID3D11Device_CreateDepthStencilView(s_dev, (ID3D11Resource *)d->tex, NULL, &d->dsv))))
        goto fail;
    return 1;
fail:
    res_failed(kind, va, w, h, hr);
    return 0;
}

/* BW_GPU_FLIPTRACE=<n>: for the first n flips, what happened since the last
 * one, in order -- B<va> a surface bound, C<va> a colour clear, d<n> draws --
 * then which surface was presented and where the CRTC scans out
 * (PCRTC_START, NV2A register 0x600800). */
extern volatile uint32_t *xbox_Nv2aRegPtr(uint32_t offset);
static char s_ft[600];
static int s_ft_len, s_ft_on = -1;
static unsigned s_ft_draws, s_ft_left;

static int ft_on(void)
{
    if (s_ft_on < 0) {
        const char *e = getenv("BW_GPU_FLIPTRACE");
        s_ft_left = e ? (unsigned)atoi(e) : 0;
        s_ft_on = s_ft_left > 0;
    }
    return s_ft_on;
}

static void ft_note(char kind, uint32_t va)
{
    if (!ft_on() || s_ft_len > 540)
        return;
    if (s_ft_draws) {
        s_ft_len += _snprintf(s_ft + s_ft_len, sizeof s_ft - s_ft_len, " d%u", s_ft_draws);
        s_ft_draws = 0;
    }
    s_ft_len += _snprintf(s_ft + s_ft_len, sizeof s_ft - s_ft_len, " %c%08X", kind, va);
}

/* One draw, in the flip trace: path (s screen-space, x combiner), vertices,
 * the first vertex (x, y, z, rhw, colour), texture, alpha test, blend, depth,
 * colour mask, cull. */
static void ft_draw(char path, uint32_t n, float x, float y, float z, float rhw,
                    uint32_t argb, uint32_t tex, const Nv2aRenderState *rs)
{
    if (!ft_on() || !s_ft_left || s_ft_len > 400)
        return;
    s_ft_len += _snprintf(s_ft + s_ft_len, sizeof s_ft - s_ft_len,
                          " %c%u(%.0f,%.0f,%.3g,%.3g c%08X t%08X a%u/%X/%u b%u/%X/%X z%u/%X/%u m%X cu%u)",
                          path, n, x, y, z, rhw, argb, tex,
                          rs->alpha_test_enable, rs->alpha_func, rs->alpha_ref,
                          rs->blend_enable, rs->blend_src, rs->blend_dst,
                          rs->depth_test_enable, rs->depth_func, rs->depth_write,
                          rs->color_mask, rs->cull_enable);
    s_ft_draws = 0;
}

static void ft_flip(const BwSurf *shown)
{
    volatile uint32_t *crtc;
    if (!ft_on() || !s_ft_left)
        return;
    s_ft_left--;
    ft_note('|', 0);
    crtc = xbox_Nv2aRegPtr(0x600800);
    fprintf(stderr, "[FLIP] %u shown %08X (%ux%u) crtc %08X:%s\n", bwg_gen,
            shown ? shown->va : 0, shown ? shown->w : 0, shown ? shown->h : 0,
            crtc ? *crtc : 0, s_ft);
    s_ft_len = 0;
    s_ft[0] = 0;
}

static void bind_targets(BwSurf *s, BwDepth *d)
{
    D3D11_VIEWPORT vp;
    ID3D11ShaderResourceView *none = NULL;
    g_bw_ctx_epoch++;                   /* I3: targets change what the runtime keeps bound */
    /* A surface about to be drawn into must not stay bound as a texture. */
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &none);
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, s->mrtv ? &s->mrtv : &s->rtv, d ? d->dsv : NULL);
    vp.TopLeftX = 0; vp.TopLeftY = 0;
    vp.Width = (float)s->pw; vp.Height = (float)s->ph;      /* VFX (D9): physical; = w, h in Original */
    vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
    if (s != bwg_cur)
        ft_note('B', s->va);
    bwg_cur = s;
    bwg_cur_depth = d;
}

/* VFX (D9): the MSAA target's samples into the surface's texture, if it was
 * drawn since the last time. No-op in Original (no MSAA target). */
static void surf_resolve(BwSurf *s)
{
    if (s && s->mtex && s->mdirty) {
        ID3D11DeviceContext_ResolveSubresource(s_ctx, (ID3D11Resource *)s->tex, 0,
                                               (ID3D11Resource *)s->mtex, 0,
                                               bw_vfx_target_format(s->bpp));
        s->mdirty = 0;
    }
}

/* VFX (D9): a render-scaled or 16-bit surface is averaged down to the guest's
 * own size and format on the GPU before it is copied back, so guest memory
 * gets the frame exactly where and as large as before. */
static ID3D11Texture2D *s_rb_tex;
static ID3D11RenderTargetView *s_rb_rtv;
static uint32_t s_rb_w, s_rb_h, s_rb_bpp;

/* Widescreen (D5): guest memory gets the centre of a widened surface, the
 * part Original's picture covers, never the wide rows: a box at s_rb_crop_x
 * when nothing else is scaled, else the centre copied out and averaged down. */
static uint32_t s_rb_crop_x;
static ID3D11Texture2D *s_ws_crop_tex;
static ID3D11ShaderResourceView *s_ws_crop_srv;
static uint32_t s_ws_crop_w, s_ws_crop_h;
static DXGI_FORMAT s_ws_crop_f;

static ID3D11ShaderResourceView *ws_centre(BwSurf *s, uint32_t *cw)
{
    D3D11_TEXTURE2D_DESC td;
    D3D11_BOX box;
    DXGI_FORMAT f = bw_vfx_target_format(s->bpp);
    *cw = (uint32_t)((float)s->pw / s->wsk + 0.5f);
    if (*cw > s->pw)
        *cw = s->pw;
    if (!s_ws_crop_tex || s_ws_crop_w != *cw || s_ws_crop_h != s->ph || s_ws_crop_f != f) {
        if (s_ws_crop_srv) ID3D11ShaderResourceView_Release(s_ws_crop_srv);
        if (s_ws_crop_tex) ID3D11Texture2D_Release(s_ws_crop_tex);
        s_ws_crop_srv = NULL;
        s_ws_crop_tex = NULL;
        memset(&td, 0, sizeof(td));
        td.Width = *cw; td.Height = s->ph; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = f;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_ws_crop_tex))
            || FAILED(ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)s_ws_crop_tex,
                                                            NULL, &s_ws_crop_srv)))
            return NULL;
        s_ws_crop_w = *cw; s_ws_crop_h = s->ph; s_ws_crop_f = f;
    }
    box.left = (s->pw - *cw) / 2; box.right = box.left + *cw;
    box.top = 0; box.bottom = s->ph; box.front = 0; box.back = 1;
    ID3D11DeviceContext_CopySubresourceRegion(s_ctx, (ID3D11Resource *)s_ws_crop_tex, 0, 0, 0, 0,
                                              (ID3D11Resource *)s->tex, 0, &box);
    return s_ws_crop_srv;
}

static ID3D11Texture2D *readback_source(BwSurf *s)
{
    s_rb_crop_x = 0;
    if (s->wsk > 1.0f && !bw_vfx_active()) {        /* widescreen at scale 1: a box */
        s_rb_crop_x = (s->pw - s->w) / 2;
        return s->tex;
    }
    if (!bw_vfx_active() || (s->pw == s->w && s->ph == s->h && !s->mtex
                             && bw_vfx_target_format(s->bpp) == surf_format(s->bpp)))
        return s->tex;
    surf_resolve(s);
    if (!s_rb_tex || s_rb_w != s->w || s_rb_h != s->h || s_rb_bpp != s->bpp) {
        D3D11_TEXTURE2D_DESC td;
        if (s_rb_rtv) ID3D11RenderTargetView_Release(s_rb_rtv);
        if (s_rb_tex) ID3D11Texture2D_Release(s_rb_tex);
        s_rb_rtv = NULL;
        s_rb_tex = NULL;
        memset(&td, 0, sizeof(td));
        td.Width = s->w; td.Height = s->h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = surf_format(s->bpp);
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_rb_tex))
            || FAILED(ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)s_rb_tex, NULL, &s_rb_rtv)))
            return NULL;
        s_rb_w = s->w; s_rb_h = s->h; s_rb_bpp = s->bpp;
    }
    if (s->wsk > 1.0f) {                            /* widescreen: the centre */
        uint32_t cw;
        ID3D11ShaderResourceView *c = ws_centre(s, &cw);
        if (!c)
            return NULL;
        bw_vfx_blit(c, cw, s->ph, s_rb_rtv, s->w, s->h, 1);
    } else
        bw_vfx_blit(s->srv, s->pw, s->ph, s_rb_rtv, s->w, s->h, 1);
    if (bwg_cur)
        bind_targets(bwg_cur, bwg_cur_depth);
    return s_rb_tex;
}

/* VFX (D9): the readback without the wait, Remastered only.
 *
 * The readback below maps its staging copy at once, so the NV2A thread waits
 * at every flip until the GPU has finished the frame. At 1x that is a few
 * milliseconds; at native resolution with MSAA, on a GPU other programs share,
 * it grew to 13-21 ms and the game fell under 30 fps (runs
 * 20261027-023553-r6-gpu-d9lod1/-d9lod2/-d9fx, ten games on the GPU). Nothing
 * in the game is known to read the presented frame (D-003 R1 Q2: only debug
 * features, a screenshot and a shadow-map dump). So in Remastered the copy
 * goes into a ring of three staging textures and is written into guest memory
 * when the GPU has it ready (normally two flips later), never waiting.
 * Frame dumps and the peek file are then up to two frames old.
 * BW_VFX_SYNC_READBACK=1 restores the waiting readback. */
#define VFX_RB_RING 3
typedef struct {
    ID3D11Texture2D *t;
    uint32_t tw, th, tbpp;          /* the staging texture's size and format */
    uint32_t va, w, h, bpp, pitch;  /* the frame it holds, and where it goes */
    int pending;
    uint32_t gen;                   /* I2: the flip it was copied at (bwg_gen) */
} VfxRb;
static VfxRb s_vfx_rb[VFX_RB_RING];
static unsigned s_vfx_rb_next;

/* I2 (BW_GPU_RBRING, lane gfx-ideas; arch.md 3.14, gate IG2): the flip's
 * readback without the wait (Remastered and later; its check in any edition).
 *   unset/0  as before: Original (and Remastered without its suite) copies
 *            the flipped frame into guest memory at the flip and waits for
 *            the GPU to finish it; Remastered's suite uses the ring above
 *   1        Remastered and later (GA-13): the ring above wherever the
 *            waiting readback ran: the frame is written into guest memory
 *            when the GPU has it, normally one or two flips later, and the
 *            flip never waits. Not with the plain window (BW_GPU_PRESENT=0
 *            shows guest memory), a dumped flip or a capture (both stay
 *            waiting). Original keeps the waiting readback: delaying it
 *            changes what guest memory holds when the game reads it, and
 *            those reads can't be seen until I1's lock wrapper exists
 *   check    a test aid: the waiting readback as before, plus each flip's copy
 *            through a second ring that writes nothing: when its copy lands,
 *            its bytes are hashed and compared with what the waiting readback
 *            wrote for that flip ([GPU-RBRING] lines). It works in a
 *            capture's replay, so the corpus proves the ring hands guest
 *            memory the same bytes, only later.
 * Where a ring writes, the flips between copy and write are counted (the
 * lag, [GPU-RBRING] every 300 flips). The lock-forced waiting read that keeps
 * the game's own reads current needs I1's lock wrapper (design only). */
static int s_present_mode;          /* below: -1 unset, 0 off/failed, 1 on */
static int rb_ring_mode(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("BW_GPU_RBRING");
        v = !e || !*e ? 0 : !strcmp(e, "check") ? 2 : e[0] == '1' ? 1 : 0;
        if (v == 1 && !bw_edition_at_least(BW_ED_REMASTERED)) {
            fprintf(stderr, "[GPU-RBRING] Original keeps the waiting readback (GA-13): BW_GPU_RBRING=1 ignored\n");
            v = 0;
        }
        if (v)
            fprintf(stderr, "[GPU-RBRING] %s (BW_GPU_RBRING)\n",
                    v == 1 ? "the flip's readback through a ring of 3 copies, without waiting"
                           : "check: the waiting readback, and a ring of 3 copies hashed against it");
    }
    return v;
}
static unsigned s_rb_lag[5];        /* frames written by a ring, by flips late: 0, 1, 2, 3, 4+ */
static unsigned s_rb_ck_same, s_rb_ck_diff, s_rb_ck_lost;      /* check mode */
static uint64_t s_rb_ck_hash[64];   /* check mode: the waiting readback's hash, by flip % 64 */
static uint32_t s_rb_ck_gen[64];

/* The rows a readback writes (the latest binding's pitch, never past the end
 * of the guest block): *prow bytes in each of *prows rows at *ppitch. */
static void rb_rows(uint32_t va, uint32_t w, uint32_t h, uint32_t bpp, uint32_t pitch_in,
                    uint32_t *prow, uint32_t *prows, uint32_t *ppitch)
{
    uint32_t row = w * bpp, rows = h, pitch = pitch_in ? pitch_in : row;
    uint32_t room = bwg_block_size(va);
    if (row > pitch) row = pitch;
    if (room) {
        if (row > room) { row = room; rows = 1; }
        else if ((uint64_t)(rows - 1) * pitch + row > room) rows = (room - row) / pitch + 1;
    }
    *prow = row; *prows = rows; *ppitch = pitch;
}

static uint64_t rb_hash(const uint8_t *p, uint32_t rowpitch, uint32_t row, uint32_t rows)
{
    uint64_t h = 1469598103934665603ull;
    uint32_t y, x;
    for (y = 0; y < rows; y++) {
        const uint8_t *q = p + (size_t)y * rowpitch;
        for (x = 0; x < row; x++)
            h = (h ^ q[x]) * 0x100000001B3ull;
    }
    return h;
}

static void rb_lag_note(uint32_t gen)
{
    uint32_t k = bwg_gen - gen;
    s_rb_lag[k < 4 ? k : 4]++;
}

static void rb_ring_line(void)
{
    if (!rb_ring_mode() || (bwg_gen % 300) != 0)
        return;
    fprintf(stderr, "[GPU-RBRING] flip %u: written by a ring, by flips late: 0 %u, 1 %u, 2 %u, 3 %u, 4+ %u;"
                    " dropped (ring full) %u; check: same %u, different %u, never landed %u\n",
            bwg_gen, s_rb_lag[0], s_rb_lag[1], s_rb_lag[2], s_rb_lag[3], s_rb_lag[4],
            s_rb_dropped, s_rb_ck_same, s_rb_ck_diff, s_rb_ck_lost);
}

static void vfx_rb_write(VfxRb *r, const D3D11_MAPPED_SUBRESOURCE *m)
{
    /* As readback() below: the latest binding's pitch, never past the end
     * of the guest block (the mission crash of 2026-10-26). */
    uint32_t row = r->w * r->bpp, rows = r->h, pitch = r->pitch ? r->pitch : row, y;
    uint32_t room = bwg_block_size(r->va);
    if (row > pitch) row = pitch;
    if (room) {
        if (row > room) { row = room; rows = 1; }
        else if ((uint64_t)(rows - 1) * pitch + row > room) rows = (room - row) / pitch + 1;
    }
    for (y = 0; y < rows; y++)
        memcpy(GUEST(r->va) + (size_t)y * pitch, (uint8_t *)m->pData + (size_t)y * m->RowPitch, row);
    if (rb_ring_mode())
        rb_lag_note(r->gen);                /* I2: how late it landed */
}

extern int nv2a_pb_exec_flip_dumping(void);   /* carried nv2a_pb_exec.c (TK-39) */

static int vfx_rb_async(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("BW_VFX_SYNC_READBACK");
        v = !(e && e[0] == '1');
    }
    /* a flip being dumped is read back at once, so the dump is that frame */
    return ((v && bw_vfx_active())
            || (rb_ring_mode() == 1 && s_present_mode != 0))   /* I2: any edition, not the plain window */
        && !nv2a_pb_exec_flip_dumping()
        && !BW_GFXCAP_PINNED();             /* BW_GFXCAP: every flip's hash, at its flip */
}

static void vfx_readback_async(BwSurf *s, ID3D11Texture2D *src)
{
    unsigned k;
    VfxRb *r;
    D3D11_BOX box;
    /* Oldest first: write whatever the GPU has finished; stop at the first
     * that isn't ready (the later ones aren't either). */
    for (k = 0; k < VFX_RB_RING; k++) {
        D3D11_MAPPED_SUBRESOURCE m;
        HRESULT hr;
        r = &s_vfx_rb[(s_vfx_rb_next + k) % VFX_RB_RING];
        if (!r->pending)
            continue;
        hr = ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)r->t, 0, D3D11_MAP_READ,
                                     D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
            break;
        if (gpu_lost(hr, "the readback's Map"))     /* item 28 */
            return;
        if (SUCCEEDED(hr)) {
            vfx_rb_write(r, &m);
            ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)r->t, 0);
        }
        r->pending = 0;
    }
    r = &s_vfx_rb[s_vfx_rb_next];
    if (r->pending) {
        s_st_rb_async_late++;           /* the ring is full: this slot's old frame is dropped */
        s_rb_dropped++;                 /* I2's count (the stats line zeroes the other each second) */
        r->pending = 0;
    }
    if (!r->t || r->tw != s->w || r->th != s->h || r->tbpp != s->bpp) {
        D3D11_TEXTURE2D_DESC td;
        if (r->t) ID3D11Texture2D_Release(r->t);
        r->t = NULL;
        memset(&td, 0, sizeof(td));
        td.Width = s->w; td.Height = s->h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = surf_format(s->bpp);
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &r->t)))
            return;
        r->tw = s->w; r->th = s->h; r->tbpp = s->bpp;
    }
    box.left = s_rb_crop_x; box.top = 0; box.front = 0;       /* widescreen: the centre */
    box.right = s_rb_crop_x + s->w; box.bottom = s->h; box.back = 1;
    ID3D11DeviceContext_CopySubresourceRegion(s_ctx, (ID3D11Resource *)r->t, 0, 0, 0, 0,
                                              (ID3D11Resource *)src, 0, &box);
    r->va = s->va; r->w = s->w; r->h = s->h; r->bpp = s->bpp;
    r->pitch = s->cur_pitch ? s->cur_pitch : (s->pitch ? s->pitch : s->w * s->bpp);
    r->pending = 1;
    r->gen = bwg_gen;
    s_vfx_rb_next = (s_vfx_rb_next + 1) % VFX_RB_RING;
}

/* I2 check mode (BW_GPU_RBRING=check): a second ring of 3 staging copies
 * that writes nothing. Each landed copy is hashed (the rows the readback
 * writes) and compared with the hash the waiting readback took of the same
 * flip; a copy whose flip is no longer in the 64-flip table, or that is
 * overwritten before it lands, counts as never landed. */
static VfxRb s_rb_ck[VFX_RB_RING];
static unsigned s_rb_ck_next;

static void rb_check_poll(void)
{
    unsigned k;
    for (k = 0; k < VFX_RB_RING; k++) {
        VfxRb *r = &s_rb_ck[(s_rb_ck_next + k) % VFX_RB_RING];
        D3D11_MAPPED_SUBRESOURCE m;
        uint32_t row, rows, pitch;
        HRESULT hr;
        if (!r->pending)
            continue;
        hr = ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)r->t, 0, D3D11_MAP_READ,
                                     D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
            break;
        r->pending = 0;
        if (FAILED(hr))
            continue;
        rb_rows(r->va, r->w, r->h, r->bpp, r->pitch, &row, &rows, &pitch);
        if (s_rb_ck_gen[r->gen & 63] != r->gen)
            s_rb_ck_lost++;
        else if (rb_hash((const uint8_t *)m.pData, m.RowPitch, row, rows) == s_rb_ck_hash[r->gen & 63])
            s_rb_ck_same++;
        else if (s_rb_ck_diff++ < 16)
            fprintf(stderr, "[GPU-RBRING] check: flip %u's ring copy differs from its waiting readback\n",
                    r->gen);
        rb_lag_note(r->gen);
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)r->t, 0);
    }
}

static void rb_check_queue(BwSurf *s, ID3D11Texture2D *src)
{
    VfxRb *r = &s_rb_ck[s_rb_ck_next];
    D3D11_BOX box;
    if (r->pending) {
        s_rb_ck_lost++;
        r->pending = 0;
    }
    if (!r->t || r->tw != s->w || r->th != s->h || r->tbpp != s->bpp) {
        D3D11_TEXTURE2D_DESC td;
        if (r->t) ID3D11Texture2D_Release(r->t);
        r->t = NULL;
        memset(&td, 0, sizeof(td));
        td.Width = s->w; td.Height = s->h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = surf_format(s->bpp);
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &r->t)))
            return;
        r->tw = s->w; r->th = s->h; r->tbpp = s->bpp;
    }
    box.left = s_rb_crop_x; box.top = 0; box.front = 0;
    box.right = s_rb_crop_x + s->w; box.bottom = s->h; box.back = 1;
    ID3D11DeviceContext_CopySubresourceRegion(s_ctx, (ID3D11Resource *)r->t, 0, 0, 0, 0,
                                              (ID3D11Resource *)src, 0, &box);
    r->va = s->va; r->w = s->w; r->h = s->h; r->bpp = s->bpp;
    r->pitch = s->cur_pitch ? s->cur_pitch : (s->pitch ? s->pitch : s->w * s->bpp);
    r->pending = 1;
    r->gen = bwg_gen;
    s_rb_ck_next = (s_rb_ck_next + 1) % VFX_RB_RING;
}

/* Copy a rendered surface back into guest memory. */
static uint32_t s_gfxcap_rb_va, s_gfxcap_rb_pitch, s_gfxcap_rb_row, s_gfxcap_rb_rows;

static void readback(BwSurf *s)
{
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_BOX box;
    ID3D11Texture2D *src;
    HRESULT hr;
    if (!s || !s->dirty || !s->tex)
        return;
    if (!(src = readback_source(s)))       /* VFX (D9): s->tex in Original */
        return;
    if (vfx_rb_async() && !s_rb_force_sync) {   /* VFX (D9): Remastered, no wait */
        vfx_readback_async(s, src);
        s->dirty = 0;
        return;
    }
    for (unsigned k = 0; k < VFX_RB_RING; k++)
        s_vfx_rb[k].pending = 0;           /* VFX: older queued frames must not land after this one */
    if (rb_ring_mode() == 2) {             /* I2 check: the ring's copy, beside the waiting one */
        rb_check_poll();
        rb_check_queue(s, src);
    }
    if (!s_staging || s_staging_w < s->w || s_staging_h < s->h || s_staging_bpp != s->bpp) {
        D3D11_TEXTURE2D_DESC td;
        if (s_staging) ID3D11Texture2D_Release(s_staging);
        memset(&td, 0, sizeof(td));
        td.Width = s->w; td.Height = s->h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = surf_format(s->bpp);
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_staging))) {
            s_staging = NULL;
            return;
        }
        s_staging_w = s->w; s_staging_h = s->h; s_staging_bpp = s->bpp;
    }
    box.left = s_rb_crop_x; box.top = 0; box.front = 0;       /* widescreen: the centre */
    box.right = s_rb_crop_x + s->w; box.bottom = s->h; box.back = 1;
    ID3D11DeviceContext_CopySubresourceRegion(s_ctx, (ID3D11Resource *)s_staging, 0, 0, 0, 0,
                                              (ID3D11Resource *)src, 0, &box);
    if (SUCCEEDED(hr = ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_staging, 0,
                                               D3D11_MAP_READ, 0, &m))) {
        /* The surface's size only grows (one texture per address), and its
         * pitch used to be the one it had when the texture was last made.
         * The front buffer (805B4000, 1200 KB) is sometimes bound with the
         * AA surface's 5120-byte pitch: from then on every readback wrote
         * 480 rows at 5120 -- 2.4 MB -- over the two vertex buffers and the
         * scene-node pool after it, with clear-colour pixels. That was the
         * mission crash (sub_00036540 & co., node links reading 0x000B112B;
         * ANALYSIS "The mission crash"). Now: the latest binding's pitch, and
         * never past the end of the guest block. */
        uint32_t row = s->w * s->bpp, rows = s->h;
        uint32_t pitch = s->cur_pitch ? s->cur_pitch : (s->pitch ? s->pitch : row);
        uint32_t room = bwg_block_size(s->va);
        if (row > pitch) row = pitch;
        if (room) {
            if (row > room) {
                row = room;
                rows = 1;
            } else if ((uint64_t)(rows - 1) * pitch + row > room) {
                uint32_t fit = (room - row) / pitch + 1;
                if (!s->clamped++)
                    fprintf(stderr, "[GPU-D3D11] readback of %08X clamped: %u rows at pitch %u"
                                    " would pass the end of its %u-byte block; %u rows written\n",
                            s->va, rows, pitch, room, fit);
                rows = fit;
            }
        }
        for (uint32_t y = 0; y < rows; y++)
            memcpy(GUEST(s->va) + (size_t)y * pitch, (uint8_t *)m.pData + (size_t)y * m.RowPitch, row);
        if (rb_ring_mode() == 2) {         /* I2 check: what this flip wrote */
            s_rb_ck_hash[bwg_gen & 63] = rb_hash((const uint8_t *)m.pData, m.RowPitch, row, rows);
            s_rb_ck_gen[bwg_gen & 63] = bwg_gen;
        }
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_staging, 0);
        s_gfxcap_rb_va = s->va;         /* BW_GFXCAP: the rows just written, for the flip hash */
        s_gfxcap_rb_pitch = pitch;
        s_gfxcap_rb_row = row;
        s_gfxcap_rb_rows = rows;
    } else if (gpu_lost(hr, "the readback's Map")) {   /* item 28 */
        return;
    }
    s->dirty = 0;
}

/* ---- widescreen: the wide dumps -------------------------------------------
 * (The markers, the 2D tests and the per-batch x remap are the core's:
 * bwg_draw.c.) */

/* main.c, after bw_ws_init() and before the game runs (bwg_ws_start). */
void bw_d3d11_ws_start(void)
{
    bwg_ws_start();
}

/* The wide picture, at the flips the executor dumps (RECOMP_FB_DUMP_FLIPS:
 * "1" or while its trigger file exists), as <dump dir>\wide\wNNNNN.bmp: 480
 * lines at the picture's aspect, averaged down from the physical surface
 * whatever the render scale (2 x 2 samples per output pixel). The executor's
 * own dumps keep the guest-sized centre (readback). */
static void ws_px(const uint8_t *q, DXGI_FORMAT f, unsigned *r, unsigned *g, unsigned *b)
{
    switch (f) {
    case DXGI_FORMAT_R16G16B16A16_UNORM: {
        const uint16_t *w = (const uint16_t *)q;
        *r = w[0] >> 8; *g = w[1] >> 8; *b = w[2] >> 8;
        break;
    }
    case DXGI_FORMAT_R8G8B8A8_UNORM:
        *r = q[0]; *g = q[1]; *b = q[2];
        break;
    default:                                    /* B8G8R8A8 */
        *r = q[2]; *g = q[1]; *b = q[0];
        break;
    }
}

static void ws_dump(BwSurf *sf)
{
    static const char *spec = (const char *)-1, *prefix;
    static unsigned seq;
    static ID3D11Texture2D *stg;
    static uint32_t stg_w, stg_h;
    static DXGI_FORMAT stg_f;
    static uint8_t *row;
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_TEXTURE2D_DESC sd;
    char dir[512], path[600];
    const char *slash;
    uint32_t w, h, y, x, rowb, size, bpx;
    uint8_t hd[54];
    FILE *f;

    if (bwg_ws_k <= 1.0f || !sf || sf->wsk <= 1.0f || !sf->tex)
        return;
    if (spec == (const char *)-1) {
        spec = getenv("RECOMP_FB_DUMP_FLIPS");
        prefix = getenv("RECOMP_FB_DUMP");
    }
    if (!spec || !prefix || !*prefix)
        return;
    if (strcmp(spec, "1")) {
        FILE *t = fopen(spec, "rb");
        if (!t)
            return;
        fclose(t);
    }
    surf_resolve(sf);
    ID3D11Texture2D_GetDesc(sf->tex, &sd);
    bpx = sd.Format == DXGI_FORMAT_R16G16B16A16_UNORM ? 8u
        : (sd.Format == DXGI_FORMAT_B8G8R8A8_UNORM || sd.Format == DXGI_FORMAT_R8G8B8A8_UNORM) ? 4u : 0u;
    if (!bpx) {
        static int said;
        if (!said++)
            fprintf(stderr, "[WS] wide dumps: surface format %u not handled\n", (unsigned)sd.Format);
        return;
    }
    if (!stg || stg_w != sd.Width || stg_h != sd.Height || stg_f != sd.Format) {
        D3D11_TEXTURE2D_DESC td;
        if (stg) ID3D11Texture2D_Release(stg);
        stg = NULL;
        memset(&td, 0, sizeof(td));
        td.Width = sd.Width; td.Height = sd.Height; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = sd.Format;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &stg))) {
            stg = NULL;
            return;
        }
        stg_w = sd.Width; stg_h = sd.Height; stg_f = sd.Format;
    }
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)stg, (ID3D11Resource *)sf->tex);
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)stg, 0, D3D11_MAP_READ, 0, &m)))
        return;
    slash = strrchr(prefix, '\\');
    if (!slash || (strrchr(prefix, '/') && strrchr(prefix, '/') > slash))
        slash = strrchr(prefix, '/');
    _snprintf(dir, sizeof dir, "%.*swide", slash ? (int)(slash - prefix + 1) : 0, prefix);
    dir[sizeof dir - 1] = 0;
    CreateDirectoryA(dir, NULL);
    _snprintf(path, sizeof path, "%s\\w%05u.bmp", dir, seq++);
    path[sizeof path - 1] = 0;
    h = 480;
    w = (uint32_t)(480.0f * 4.0f / 3.0f * sf->wsk + 0.5f);
    rowb = (w * 3 + 3) & ~3u;
    if (!row)
        row = (uint8_t *)malloc(4096 * 3);
    f = row && w <= 4096 ? fopen(path, "wb") : NULL;
    if (f) {
        float fx = (float)sd.Width / w, fy = (float)sd.Height / h;
        memset(hd, 0, sizeof hd);
        size = 54 + rowb * h;
        hd[0] = 'B'; hd[1] = 'M';
        memcpy(hd + 2, &size, 4);
        hd[10] = 54; hd[14] = 40;
        memcpy(hd + 18, &w, 4);
        { int32_t hh = -(int32_t)h; memcpy(hd + 22, &hh, 4); }
        hd[26] = 1; hd[28] = 24;
        fwrite(hd, 1, 54, f);
        for (y = 0; y < h; y++) {
            memset(row, 0, rowb);
            for (x = 0; x < w; x++) {
                unsigned r = 0, g = 0, b = 0, i, j;
                for (j = 0; j < 2; j++)
                    for (i = 0; i < 2; i++) {
                        unsigned rr, gg, bb;
                        uint32_t sx = (uint32_t)((x + 0.25f + 0.5f * i) * fx);
                        uint32_t sy = (uint32_t)((y + 0.25f + 0.5f * j) * fy);
                        if (sx >= sd.Width) sx = sd.Width - 1;
                        if (sy >= sd.Height) sy = sd.Height - 1;
                        ws_px((const uint8_t *)m.pData + (size_t)sy * m.RowPitch + (size_t)sx * bpx,
                              sd.Format, &rr, &gg, &bb);
                        r += rr; g += gg; b += bb;
                    }
                row[x * 3 + 0] = (uint8_t)(b / 4);
                row[x * 3 + 1] = (uint8_t)(g / 4);
                row[x * 3 + 2] = (uint8_t)(r / 4);
            }
            fwrite(row, 1, rowb, f);
        }
        fclose(f);
    }
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)stg, 0);
}


/* ---- textures ----------------------------------------------------------- */

/* (The texture cache is the core's: bwg_tex.c.) */

/* BW_GPU_BLACKWATCH=<prefix>: what each frame sampled (the core's list,
 * bwg_tex_use), to explain a frame that suddenly goes black (see bw_flip). */
static const char *s_watch_prefix;

/* Giant triangles per frame: a vertex more than 4 surfaces away from the
 * surface, or with 1/w not positive and finite. [slot] as bwg_tex_use. */
static unsigned s_huge[2], s_tris_frame[2];
static BwXVertex s_huge_v[2][3];

static void note_huge(const BwXVertex *v, uint32_t n, uint32_t w, uint32_t h)
{
    int slot = bwg_gen & 1;
    uint32_t i, k;
    float lim_x = 4.0f * w, lim_y = 4.0f * h;
    s_tris_frame[slot] += n / 3;
    for (i = 0; i + 2 < n; i += 3) {
        int bad = 0;
        for (k = 0; k < 3; k++) {
            const BwXVertex *q = &v[i + k];
            if (!(q->rhw > 0.0f) || q->rhw - q->rhw != 0.0f
                    || q->x < -lim_x || q->x > lim_x + w || q->y < -lim_y || q->y > lim_y + h)
                bad = 1;
        }
        if (bad) {
            if (!s_huge[slot])
                memcpy(s_huge_v[slot], &v[i], sizeof s_huge_v[slot]);
            s_huge[slot]++;
        }
    }
}

static void print_huge(const char *what, int slot)
{
    int k;
    fprintf(stderr, "    %s: %u of %u triangles giant", what, s_huge[slot], s_tris_frame[slot]);
    if (s_huge[slot])
        for (k = 0; k < 3; k++)
            fprintf(stderr, " | v%d x %.4g y %.4g z %.4g rhw %.4g", k, s_huge_v[slot][k].x,
                    s_huge_v[slot][k].y, s_huge_v[slot][k].z, s_huge_v[slot][k].rhw);
    fprintf(stderr, "\n");
}

/* What a stage samples, as the core's texture cache decided (bwg_tex.c):
 * the entry's view, or the surface's (render to texture). */
static ID3D11ShaderResourceView *tex_view(BwgTexRef r)
{
    return r.rt ? r.rt->srv : r.e ? r.e->srv : NULL;
}

static ID3D11ShaderResourceView *get_texture(const Nv2aTexture *t)
{
    return tex_view(bwg_get_texture(t));
}

static ID3D11ShaderResourceView *get_cube_texture(const Nv2aTexture *t)
{
    return tex_view(bwg_get_cube_texture(t));
}

/* The core's uploads (BwgUpload) into D3D11: a 2D texture, or a
 * TextureCube (six faces), with its view. Item 36: a failure is logged and
 * nothing half-made is kept; the entry stays without an identity, so it is
 * tried again when next used. */
static int tex_create(BwTex *e, const Nv2aTexture *t, const BwgUpload *u)
{
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA init[6 * 12];
    uint32_t i;
    memset(&td, 0, sizeof(td));
    td.Width = u->w; td.Height = u->h; td.MipLevels = u->levels; td.ArraySize = u->faces;
    td.Format = (DXGI_FORMAT)u->format;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    memset(init, 0, sizeof(init));
    for (i = 0; i < u->faces * u->levels; i++) {
        init[i].pSysMem = u->mip[i].data;
        init[i].SysMemPitch = u->mip[i].pitch;
    }
    if (u->faces == 6) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd;
        HRESULT hr;
        td.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
        hr = BW_GPU_FT("texture", E_OUTOFMEMORY,       /* item 25 test aid */
                       ID3D11Device_CreateTexture2D(s_dev, &td, init, &e->tex));
        if (FAILED(hr)) {
            e->tex = NULL;
            res_failed(RF_TEXTURE, t->offset, t->width, t->height, hr);    /* item 36 */
            return 0;
        }
        memset(&sd, 0, sizeof(sd));
        sd.Format = td.Format;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
        sd.TextureCube.MostDetailedMip = 0;
        sd.TextureCube.MipLevels = u->levels;
        hr = BW_GPU_FT("resource", E_OUTOFMEMORY,
                       ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)e->tex, &sd, &e->srv));
        if (FAILED(hr)) {
            e->srv = NULL;
            ID3D11Texture2D_Release(e->tex);
            e->tex = NULL;
            res_failed(RF_TEX_VIEW, t->offset, t->width, t->height, hr);
            return 0;
        }
        return 1;
    }
    {
        double t0 = trc_on() ? bwg_qpc_ms() : 0.0;
        HRESULT hr = BW_GPU_FT("texture", E_OUTOFMEMORY,       /* item 25 test aid */
                               ID3D11Device_CreateTexture2D(s_dev, &td, init, &e->tex));
        if (FAILED(hr)) {
            e->tex = NULL;
            res_failed(RF_TEXTURE, t->offset, t->width, t->height, hr);    /* item 36 */
            return 0;
        }
        if (trc_on()) {                                     /* T1 test aid */
            s_trc_up += bwg_qpc_ms() - t0;
            s_trc_nup++;
        }
        hr = BW_GPU_FT("resource", E_OUTOFMEMORY,               /* item 36: checked */
                       ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)e->tex, NULL, &e->srv));
        if (FAILED(hr)) {
            e->srv = NULL;
            ID3D11Texture2D_Release(e->tex);
            e->tex = NULL;
            res_failed(RF_TEX_VIEW, t->offset, t->width, t->height, hr);
            return 0;
        }
    }
    return 1;
}

/* An entry's objects released before it is filled again (tex_slot). */
static void tex_release(BwTex *e)
{
    if (e->srv) { ID3D11ShaderResourceView_Release(e->srv); e->srv = NULL; }
    if (e->tex) { ID3D11Texture2D_Release(e->tex); e->tex = NULL; }
    if (e->tr.state)
        bw_tr_release(&e->tr);              /* T1: never set in Original */
}

/* T1: the menu and title photos and the Bink frames get the 8x8 deblock on
 * the GPU as they go up; the entry then holds the deblocked copy. */
static int tex_deblock(BwTex *e, const Nv2aTexture *t, int kind)
{
    ID3D11Texture2D *mt;
    ID3D11ShaderResourceView *ms;
    int ok = 0;
    if (bw_tr_deblock_gpu(s_dev, s_ctx, e->srv, t->width, t->height, 8, kind, &mt, &ms)) {
        ID3D11ShaderResourceView_Release(e->srv);
        ID3D11Texture2D_Release(e->tex);
        e->srv = ms;
        e->tex = mt;
        ok = 1;
    }
    if (bwg_cur)
        bind_targets(bwg_cur, bwg_cur_depth);
    return ok;
}


/* BW_GPU_STAGESTATS=1: every 10 s, how many combiner draws used each texture
 * stage mode (xemu psh.c names), how many of those had a cube map bound, and
 * the stage programs that use a mode neither renderer implements (bump-env
 * and dot-product modes read zero). Measures fidelity gaps; test aid. */
static void stage_stats(const BwXBatch *b, uint32_t prog, ID3D11ShaderResourceView *const *srv)
{
    static int on = -1;
    static uint32_t mode_n[32], cube_n[32], prog_v[16], prog_n[16], nprog;
    static uint32_t prog_dm[16], prog_osi[16], prog_cubed[16];
    static uint32_t border_n[2], border_v[4], nborder;
    static ULONGLONG t0;
    static const char *names[] = {
        "none", "2d", "3d", "cube", "passthru", "clipplane", "bumpenv", "bumpenv_lum",
        "brdf", "dot_st", "dot_zw", "dot_rflct_diff", "dot_rflct_spec", "dot_str_3d",
        "dot_str_cube", "dpndnt_ar", "dpndnt_gb", "dotproduct", "dot_rflct_spec_const" };
    int st, odd = 0;
    ULONGLONG now;
    if (on < 0) {
        const char *e = getenv("BW_GPU_STAGESTATS");
        on = e && *e == '1';
    }
    if (!on)
        return;
    for (st = 0; st < 4; st++) {
        uint32_t m = (prog >> (st * 5)) & 0x1F;
        mode_n[m]++;
        if (b->tex_valid[st] && b->tex_cube[st])
            cube_n[m]++;
        if (m >= 6)
            odd = 1;
        if (m && b->tex_valid[st] && (b->tex[st].addr_u == 4 || b->tex[st].addr_v == 4)) {
            uint32_t i;
            border_n[b->tex_border_src[st] ? 1 : 0]++;
            for (i = 0; i < nborder && border_v[i] != b->tex_border[st]; i++)
                ;
            if (i == nborder && nborder < 4 && b->tex_border_src[st])
                border_v[nborder++] = b->tex_border[st];
        }
    }
    if (odd) {
        uint32_t i;
        for (i = 0; i < nprog && prog_v[i] != prog; i++)
            ;
        if (i == nprog && nprog < 16) {
            prog_v[nprog] = prog;
            prog_dm[nprog] = b->dot_rgbmapping;
            prog_osi[nprog] = b->other_stage_input;
            nprog++;
        }
        if (i < nprog) {
            prog_n[i]++;
            if (srv[2] || srv[3])
                prog_cubed[i] += (srv[3] != NULL) || (srv[2] != NULL);
        }
    }
    now = GetTickCount64();
    if (!t0)
        t0 = now;
    if (now - t0 >= 10000) {
        uint32_t m, i;
        fprintf(stderr, "[GPU-D3D11] stage modes (stage uses since start, cube-bound in brackets):");
        for (m = 0; m < 32; m++)
            if (mode_n[m])
                fprintf(stderr, " %s %u[%u]", m < 19 ? names[m] : "?", mode_n[m], cube_n[m]);
        fprintf(stderr, "\n");
        fprintf(stderr, "[GPU-D3D11]   BORDER-addressed stages: %u with the border colour (colours",
                border_n[1]);
        for (i = 0; i < nborder; i++)
            fprintf(stderr, " %08X", border_v[i]);
        fprintf(stderr, "), %u with the texture's edge\n", border_n[0]);
        for (i = 0; i < nprog; i++)
            fprintf(stderr, "[GPU-D3D11]   bump/dot-mode program 0x%05X: %u draws (%u with a cube"
                            " bound; first seen with DOT_RGBMAPPING 0x%X OTHER_STAGE_INPUT 0x%X)\n",
                    prog_v[i], prog_n[i], prog_cubed[i], prog_dm[i], prog_osi[i]);
        t0 = now;
    }
}

/* ---- render state ------------------------------------------------------- */

static int s_vfx_a2c;       /* VFX (D9): this draw's blend state has alpha-to-coverage */

/* The blend, depth-stencil and sampler states, made from the core's
 * descriptions (bwg_state.c) in the slots it hands out; NULL for none (a
 * full table). Its numbers are D3D11's (checked below). */
static ID3D11BlendState *get_blend(const Nv2aRenderState *rs)
{
    BwgBlendDesc g;
    int made, i = bwg_blend(rs, s_vfx_a2c, &g, &made);
    if (i < 0)
        return NULL;
    if (!made) {
        D3D11_BLEND_DESC bd;
        D3D11_RENDER_TARGET_BLEND_DESC *t = &bd.RenderTarget[0];
        memset(&bd, 0, sizeof(bd));
        bd.AlphaToCoverageEnable = g.a2c ? TRUE : FALSE;     /* VFX (D9); FALSE in Original */
        t->BlendEnable = g.enable ? TRUE : FALSE;
        t->SrcBlend = (D3D11_BLEND)g.src;
        t->DestBlend = (D3D11_BLEND)g.dst;
        t->BlendOp = (D3D11_BLEND_OP)g.op;
        t->SrcBlendAlpha = (D3D11_BLEND)g.src_alpha;
        t->DestBlendAlpha = (D3D11_BLEND)g.dst_alpha;
        t->BlendOpAlpha = (D3D11_BLEND_OP)g.op_alpha;
        t->RenderTargetWriteMask = g.write_mask;
        ID3D11Device_CreateBlendState(s_dev, &bd, &s_blend[i]);
    }
    return s_blend[i];
}

static ID3D11DepthStencilState *get_ds(const Nv2aRenderState *rs, int has_depth)
{
    BwgDepthDesc g;
    int made, i = bwg_depth(rs, has_depth, &g, &made);
    if (i < 0)
        return NULL;
    if (!made) {
        D3D11_DEPTH_STENCIL_DESC dd;
        memset(&dd, 0, sizeof(dd));
        dd.DepthEnable = g.depth_enable ? TRUE : FALSE;
        dd.DepthFunc = (D3D11_COMPARISON_FUNC)g.depth_func;
        dd.DepthWriteMask = g.depth_write_all ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
        dd.StencilEnable = g.stencil_enable ? TRUE : FALSE;
        dd.StencilReadMask = g.read_mask;
        dd.StencilWriteMask = g.write_mask;
        dd.FrontFace.StencilFunc = (D3D11_COMPARISON_FUNC)g.func;
        dd.FrontFace.StencilFailOp = (D3D11_STENCIL_OP)g.fail;
        dd.FrontFace.StencilDepthFailOp = (D3D11_STENCIL_OP)g.zfail;
        dd.FrontFace.StencilPassOp = (D3D11_STENCIL_OP)g.pass;
        dd.BackFace = dd.FrontFace;
        ID3D11Device_CreateDepthStencilState(s_dev, &dd, &s_ds[i]);
    }
    return s_ds[i];
}

static ID3D11SamplerState *samp_obj(int i, int made, const BwgSamplerDesc *g)
{
    if (i < 0)
        return NULL;
    if (!made) {
        D3D11_SAMPLER_DESC sd;
        memset(&sd, 0, sizeof(sd));
        sd.Filter = (D3D11_FILTER)g->filter;
        sd.MaxAnisotropy = (UINT)g->max_aniso;
        sd.AddressU = (D3D11_TEXTURE_ADDRESS_MODE)g->addr_u;
        sd.AddressV = (D3D11_TEXTURE_ADDRESS_MODE)g->addr_v;
        sd.AddressW = (D3D11_TEXTURE_ADDRESS_MODE)g->addr_w;
        memcpy(sd.BorderColor, g->border, sizeof(sd.BorderColor));
        sd.MinLOD = g->min_lod;
        sd.MaxLOD = g->max_lod;
        sd.ComparisonFunc = (D3D11_COMPARISON_FUNC)g->cmp;
        ID3D11Device_CreateSamplerState(s_dev, &sd, &s_samp[i]);
    }
    return s_samp[i];
}

/* The screen-space path's old sampler: always bilinear (BW_GPU_SSFILTER=0). */
static ID3D11SamplerState *get_sampler(uint32_t addr_u, uint32_t addr_v)
{
    BwgSamplerDesc g;
    int made, i = bwg_sampler_ss(addr_u, addr_v, &g, &made);
    return samp_obj(i, made, &g);
}

/* The core's numbers (bwg_state.h) are Direct3D's. */
_Static_assert(D3D11_BLEND_ZERO == 1 && D3D11_BLEND_ONE == 2
               && D3D11_BLEND_SRC_COLOR == 3 && D3D11_BLEND_INV_SRC_COLOR == 4
               && D3D11_BLEND_SRC_ALPHA == 5 && D3D11_BLEND_INV_SRC_ALPHA == 6
               && D3D11_BLEND_DEST_ALPHA == 7 && D3D11_BLEND_INV_DEST_ALPHA == 8
               && D3D11_BLEND_DEST_COLOR == 9 && D3D11_BLEND_INV_DEST_COLOR == 10
               && D3D11_BLEND_SRC_ALPHA_SAT == 11
               && D3D11_BLEND_BLEND_FACTOR == 14
               && D3D11_BLEND_INV_BLEND_FACTOR == 15, "blend factors");
_Static_assert(D3D11_BLEND_OP_ADD == 1 && D3D11_BLEND_OP_SUBTRACT == 2
               && D3D11_BLEND_OP_REV_SUBTRACT == 3 && D3D11_BLEND_OP_MIN == 4
               && D3D11_BLEND_OP_MAX == 5, "blend ops");
_Static_assert(D3D11_COMPARISON_NEVER == 1 && D3D11_COMPARISON_LESS == 2
               && D3D11_COMPARISON_EQUAL == 3 && D3D11_COMPARISON_LESS_EQUAL == 4
               && D3D11_COMPARISON_GREATER == 5 && D3D11_COMPARISON_NOT_EQUAL == 6
               && D3D11_COMPARISON_GREATER_EQUAL == 7
               && D3D11_COMPARISON_ALWAYS == 8, "comparisons");
_Static_assert(D3D11_STENCIL_OP_KEEP == 1 && D3D11_STENCIL_OP_ZERO == 2
               && D3D11_STENCIL_OP_REPLACE == 3 && D3D11_STENCIL_OP_INCR_SAT == 4
               && D3D11_STENCIL_OP_DECR_SAT == 5 && D3D11_STENCIL_OP_INVERT == 6
               && D3D11_STENCIL_OP_INCR == 7 && D3D11_STENCIL_OP_DECR == 8, "stencil ops");
_Static_assert(D3D11_COLOR_WRITE_ENABLE_RED == 1 && D3D11_COLOR_WRITE_ENABLE_GREEN == 2
               && D3D11_COLOR_WRITE_ENABLE_BLUE == 4
               && D3D11_COLOR_WRITE_ENABLE_ALPHA == 8, "write masks");
_Static_assert(D3D11_CULL_NONE == 1 && D3D11_CULL_FRONT == 2
               && D3D11_CULL_BACK == 3, "cull modes");
_Static_assert(D3D11_FILTER_MIN_MAG_MIP_POINT == 0
               && D3D11_FILTER_MIN_MAG_MIP_LINEAR == 0x15
               && D3D11_FILTER_ANISOTROPIC == 0x55
               && D3D11_FILTER_MIN_LINEAR_MAG_MIP_POINT == 0x10 && D3D11_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT == 0x04
               && D3D11_FILTER_MIN_MAG_POINT_MIP_LINEAR == 0x01, "filters");
_Static_assert(D3D11_TEXTURE_ADDRESS_WRAP == 1 && D3D11_TEXTURE_ADDRESS_MIRROR == 2
               && D3D11_TEXTURE_ADDRESS_CLAMP == 3
               && D3D11_TEXTURE_ADDRESS_BORDER == 4, "address modes");
_Static_assert(BWG_MAX_STATE == 128 && BWG_MAX_RS_BIAS == 32, "the tables' sizes");

/* ---- the combiner path (bw_backend_ext.h) ------------------------------ */

static const char s_xvs_hlsl[] =
BW_RC_CBUFFER
"struct VSIn { float4 p : POSITION; float4 d0 : COLOR0; float4 d1 : COLOR1;\n"
"  float4 fog : TEXCOORD4; float4 tc0 : TEXCOORD0; float4 tc1 : TEXCOORD1;\n"
"  float4 tc2 : TEXCOORD2; float4 tc3 : TEXCOORD3; };\n"
"struct VSOut { float4 p : SV_Position; noperspective float4 d0 : COLOR0;\n"
"  noperspective float4 d1 : COLOR1; noperspective float fog : FOG;\n"
"  float4 tc0 : TEXCOORD0; float4 tc1 : TEXCOORD1;\n"
"  float4 tc2 : TEXCOORD2; float4 tc3 : TEXCOORD3; float fogp : FOG1; };\n"
"VSOut vs_main(VSIn i) {\n"
"  VSOut o;\n"
"  float w = (i.p.w > 0.0) ? 1.0 / i.p.w : 1.0;\n"
"  float x = i.p.x * surf.z * 2.0 - 1.0;\n"
BW_WS_REMAP_HLSL
"  float y = 1.0 - i.p.y * surf.w * 2.0;\n"
"  float zr = (i.p.z - depth.x) * depth.y;\n"
"  float z = depth.z > 0.5 ? saturate(zr) : zr;\n"
"  o.p = float4(x * w, y * w, z * w, w);\n"
/* FOG the factor clamped per vertex, FOG1 unclamped (BwXVertex pad[0],
 * BW_RC_PERSP) */
"  o.d0 = i.d0; o.d1 = i.d1; o.fog = i.fog.x; o.fogp = i.fog.y;\n"
"  o.tc0 = i.tc0; o.tc1 = i.tc1; o.tc2 = i.tc2; o.tc3 = i.tc3;\n"
"  return o;\n"
"}\n";

/* (BwXConsts, the combiner path's constants: bwg_draw.h.) */

#define BW_MAX_PS 512
typedef struct { BwRcKey key; ID3D11PixelShader *ps; } BwPs;
static BwPs s_ps_cache[BW_MAX_PS];
static int s_nps;
static unsigned s_ps_compiles, s_ps_failures;
static ID3D11VertexShader *s_xvs;
static ID3D11InputLayout *s_xlayout;
static ID3D11Buffer *s_xcb;
static int s_xready = -1;
static unsigned s_st_xdraws;

static int ensure_xpipeline(void)
{
    ID3DBlob *vsb = NULL, *err = NULL;
    D3D11_BUFFER_DESC bd;
    D3D11_INPUT_ELEMENT_DESC il[8] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,   D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 4, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 48,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 64,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 80,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 96,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 112, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    if (s_xready >= 0)
        return s_xready;
    s_xready = 0;
    if (FAILED(D3DCompile(s_xvs_hlsl, sizeof(s_xvs_hlsl) - 1, "bw_xvs", WS_DEFINES, NULL, "vs_main",
                          "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsb, &err))) {
        fprintf(stderr, "[GPU-D3D11] combiner vertex shader failed: %s\n",
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
        if (err) ID3D10Blob_Release(err);
        return 0;
    }
    ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(vsb),
                                    ID3D10Blob_GetBufferSize(vsb), NULL, &s_xvs);
    ID3D11Device_CreateInputLayout(s_dev, il, 8, ID3D10Blob_GetBufferPointer(vsb),
                                   ID3D10Blob_GetBufferSize(vsb), &s_xlayout);
    ID3D10Blob_Release(vsb);
    memset(&bd, 0, sizeof(bd));
    bd.ByteWidth = (sizeof(BwXConsts) + 15) & ~15u;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_xcb);
    s_xready = s_xvs && s_xlayout && s_xcb;
    return s_xready;
}


static ID3D11PixelShader *get_rc_shader(const BwRcKey *k)
{
    static char src[64 * 1024];
    ID3DBlob *psb = NULL, *err = NULL;
    size_t len;
    HRESULT hr;
    int rejected = 0;
    BwPs *e;
    static int last = -1;               /* item 43: consecutive draws mostly share one */
    if (last >= 0 && last < s_nps && bwg_memo_on() && !memcmp(&s_ps_cache[last].key, k, sizeof(*k)))
        return s_ps_cache[last].ps;
    for (int i = 0; i < s_nps; i++)
        if (!memcmp(&s_ps_cache[i].key, k, sizeof(*k))) {
            last = i;
            return s_ps_cache[i].ps;    /* item 41: NULL for one that failed, not tried again per draw */
        }
    if (s_nps == BW_MAX_PS) {
        static int told;                /* item 41: said once */
        if (!told++)
            fprintf(stderr, "[GPU-D3D11] all %d combiner pixel-shader slots are in use: draws that need"
                            " another are skipped from now on (this boot)\n", BW_MAX_PS);
        return NULL;
    }
    /* item 41: the entry is kept whatever happens below; a failure stays
     * NULL, as the vertex path keeps its failed programs */
    last = s_nps;
    e = &s_ps_cache[s_nps++];
    e->key = *k;
    e->ps = NULL;
    len = bw_rc_hlsl(k, src, sizeof(src));
    if (!len) {
        static int told;
        if (!told++)
            fprintf(stderr, "[GPU-D3D11] a combiner pixel shader's text is over %u bytes: its draws are"
                            " skipped\n", (unsigned)sizeof src);
        return NULL;
    }
    /* T1: the persistent cache (bw_pscache.c, every edition): the bytecode a
     * earlier run compiled from exactly this text, with this compiler and
     * these flags. Same bytes as a compile, so the same pixels. */
    double pt0 = bwg_qpc_ms();
    size_t csz = 0;
    const void *cblob = bw_pscache_find(src, len, &csz);
    if (cblob && !bw_pscache_verify()) {
        if (SUCCEEDED(hr = ID3D11Device_CreatePixelShader(s_dev, cblob, csz, NULL, &e->ps))) {
            bw_pscache_note(0, bwg_qpc_ms() - pt0);
            return e->ps;
        }
        /* item 41: bytes the driver won't take: compile, and store the
         * fresh bytes over them (bw_pscache_store) */
        e->ps = NULL;
        rejected = 1;
        {
            static int told;
            if (told++ < 3)
                fprintf(stderr, "[PSCACHE] the driver rejected a cached pixel shader (error 0x%08lX):"
                                " compiling it again\n", (unsigned long)hr);
        }
    }
    if (FAILED(D3DCompile(src, len, "bw_rc", NULL, NULL, "ps_main", "ps_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psb, &err))) {
        if (s_ps_failures++ < 3) {
            fprintf(stderr, "[GPU-D3D11] combiner shader failed (its draws are skipped; later failures"
                            " are counted in the stats line): %s\n",
                    err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
            fprintf(stderr, "%s\n", src);
        }
        if (err) ID3D10Blob_Release(err);
        return NULL;
    }
    if (err) {
        ID3D10Blob_Release(err);        /* item 41: the warnings, kept until now */
        err = NULL;
    }
    if (cblob && !rejected) {           /* BW_GPU_PSCACHE_VERIFY=1: the cached bytes must be these */
        static unsigned same, differ;
        int eq = csz == ID3D10Blob_GetBufferSize(psb)
                 && !memcmp(cblob, ID3D10Blob_GetBufferPointer(psb), csz);
        if (eq) same++; else differ++;
        fprintf(stderr, "[PSCACHE] verify: cached bytecode %s the compile (%u identical, %u different)\n",
                eq ? "identical to" : "DIFFERS from", same, differ);
    } else
        bw_pscache_store(src, len, ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb));
    hr = ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(psb),
                                        ID3D10Blob_GetBufferSize(psb), NULL, &e->ps);
    ID3D10Blob_Release(psb);
    if (FAILED(hr)) {                   /* item 41: checked */
        static int told;
        e->ps = NULL;
        s_ps_failures++;
        if (!told++)
            fprintf(stderr, "[GPU-D3D11] the device refused a compiled combiner pixel shader (error"
                            " 0x%08lX): its draws are skipped\n", (unsigned long)hr);
    }
    s_ps_compiles++;
    bw_pscache_note(1, bwg_qpc_ms() - pt0);
    return e->ps;
}


/* The texture's own filter, wrap modes, border and mips (bwg_sampler_f). */
static ID3D11SamplerState *get_sampler_f(uint32_t addr_u, uint32_t addr_v, uint32_t filter,
                                         int border_src, uint32_t border)
{
    BwgSamplerDesc g;
    int made, i = bwg_sampler_f(addr_u, addr_v, filter, border_src, border, &g, &made);
    return samp_obj(i, made, &g);
}

static float s_xlast_surf[4], s_xlast_depth[4];   /* the last draw's b0 surf, depth */
static float s_xlast_wsx[4];                        /* ... and its widescreen remap (gfx-sprites' check) */

static int tex_is_glyph(const Nv2aTexture *t)
{
    return bwg_tex_vfx_flags(t) & 1;
}

/* VFX (D9): set by bw_xdraw_vsh around an overlay (HUD) draw. */
static int s_vfx_overlay;
static int s_vfx_log_ov;        /* 0.2.5 item 50: the last draw's overlay decision, for BW_VFX_DRAWLOG */
/* T1: the next bw_xdraw is an overlay batch the vertex-program path handed
 * back (bw_xdraw_vsh_t1), and bw_xdraw's copy of it for xdraw_prepare. */
static int s_tr_fb_overlay, s_tr_skip;

/* T1 (VFX_TEX_RESTORE): the sampler for a stage bound to a restored chain.
 * A mipped stage keeps get_sampler_f's own (the chain is just two levels
 * longer at the top). A stage the game samples at level 0 only (MIN 1 or 2)
 * gets the same filters with linear blending between levels and MaxLOD at
 * the chain's copy of the game's level 0, so at or below painted size it
 * reads exactly that level, as the game does, and the enlarged levels only
 * above it. Remastered only (never called in Original). */
static ID3D11SamplerState *get_sampler_tr(uint32_t addr_u, uint32_t addr_v, uint32_t filter,
                                          int border_src, uint32_t border, int lk)
{
    BwgSamplerDesc g;
    int made, i = bwg_sampler_tr(addr_u, addr_v, filter, border_src, border, lk, &g, &made);
    return samp_obj(i, made, &g);
}

/* T1 (VFX_TEX_RESTORE): stage st of a 3D draw. If it samples a 2D DXT
 * texture with bilinear magnification, fetch the texture (into *srv) and,
 * the first time, build its restored chain (bw_texrestore.c); returns the
 * chain's number of enlarged levels with *srv the chain, or 0 with *srv the
 * original (or untouched when the stage doesn't qualify). The caller has
 * already ruled out overlays and the water's reflection program. */
static int tr_stage(const BwXBatch *b, uint32_t prog, int st, ID3D11ShaderResourceView **srv)
{
    uint32_t mode = (prog >> (st * 5)) & 0x1F, c = b->tex[st].color;
    uint32_t mag = (b->tex[st].filter >> 24) & 0xF;
    ID3D11ShaderResourceView *v;
    BwTex *e;
    if (!b->tex_valid[st] || b->tex_cube[st] || !b->tex_normalized[st] || (mode != 1 && mode != 2)
            || (c != 0x0C && c != 0x0E && c != 0x0F) || mag != 2)
        return 0;
    v = get_texture(&b->tex[st]);
    e = bwg_tex_last;
    *srv = v;
    if (!v || !e || e->cube || e->glyph)    /* a font page is overlay art, wherever it is drawn */
        return 0;
    if (e->tr.state == 0) {
        Nv2aTexture fixed;
        const Nv2aTexture *t = bwg_drop_stale_pitch(&b->tex[st], &fixed);
        if (e->tr_gen && bwg_gen - e->tr_gen < 120 && e->tr_churn < 255)
            e->tr_churn++;
        e->tr_gen = bwg_gen;
        if (e->tr_churn >= 3) {
            e->tr.state = 2;
            e->tr.why = BW_TR_WHY_CHURN;
        } else {
            /* copies the blocks and queues the work; the original draws
             * until the flip that finishes it (bw_tr_pump) */
            double t0 = trc_on() ? bwg_qpc_ms() : 0.0;
            bw_tr_request(&e->tr, t->color, GUEST(t->offset), t->width, t->height,
                          bwg_mip_count(t), t->addr_u, t->addr_v, t->offset);
            s_trc_req++;
            if (trc_on())
                s_trc_reqt += bwg_qpc_ms() - t0;
        }
    }
    {
        /* 0.2.5 item 48: the restored copy made for this draw's edge modes
         * (a second one is asked for at the first draw with other modes) */
        BwTrSlot *ts = bw_tr_slot_for(&e->tr, b->tex[st].addr_u, b->tex[st].addr_v, b->tex[st].offset);
        if (ts != &e->tr && ts->state == 0) {
            Nv2aTexture fixed;
            const Nv2aTexture *t = bwg_drop_stale_pitch(&b->tex[st], &fixed);
            bw_tr_request(ts, t->color, GUEST(t->offset), t->width, t->height,
                          bwg_mip_count(t), t->addr_u, t->addr_v, t->offset);
            s_trc_req++;
        }
        if (ts->state != 1 || !BW_TR_SRV(ts))
            return 0;
        if (!e->tr_bound) {
            e->tr_bound = 1;
            s_trc_first++;              /* T1 test aid: first draw with the restored chain */
        }
        *srv = BW_TR_SRV(ts);
        return ts->lk;
    }
}

/* What both combiner-path draws share: the targets, the pixel shader, the
 * b0 constants, textures and samplers, and the rasteriser, blend and depth
 * states. The caller then binds its vertex input and vertex shader and
 * draws. Returns 1 to draw, 0 if it cannot, -1 if both faces are culled. */
static int begin(const Nv2aSurface *s, const Nv2aRenderState *rs);   /* defined below */

/* I3 (BW_GPU_DELTA, lane gfx-ideas; arch.md 3.14, gate IG3): only the
 * bindings that changed are sent to D3D11.
 *   unset    on in Remastered and later (the lead's default), off in Original
 *   0        every draw sets every binding of its path, as before
 *   1        a binding equal to the one the draw paths last set is skipped
 *   verify   as 1, and each skip checked against the context's own state (its
 *            Get* calls); a mismatch is logged and the binding set after all
 * Everything else that sets pipeline state (the targets, the suite's passes
 * through bw_vfx_fullscreen_bs, the present, the resolve pass, the stream-
 * output check, OCEAN_REFLECT_SINGLE, the flip) bumps g_bw_ctx_epoch, and the
 * whole shadow is forgotten. Skipping a binding equal to the bound one leaves
 * the pipeline as it was, so the pictures are the same by construction;
 * verify proves the shadow equals the context at every skip. The counts:
 * [GPU-DELTA] every 300 flips. */
unsigned g_bw_ctx_epoch = 1;
enum { SH_PS, SH_PSCB0, SH_RS, SH_BS, SH_DS, SH_VS, SH_IL, SH_TOPO, SH_IB };
static struct {
    unsigned epoch;
    uint32_t valid;                 /* a bit per SH_ item */
    uint32_t srv_ok, smp_ok, vcb_ok, vsrv_ok, vb_ok;     /* a bit per slot */
    ID3D11PixelShader *ps;
    ID3D11Buffer *pscb0;
    ID3D11RasterizerState *rs;
    ID3D11BlendState *bs;
    FLOAT bf[4];
    UINT mask;
    ID3D11DepthStencilState *ds;
    UINT ref;
    ID3D11VertexShader *vs;
    ID3D11InputLayout *il;
    D3D11_PRIMITIVE_TOPOLOGY topo;
    ID3D11Buffer *ib;
    DXGI_FORMAT ibf;
    UINT ibo;
    ID3D11ShaderResourceView *srv[8];
    ID3D11SamplerState *smp[8];
    ID3D11Buffer *vcb[4];
    ID3D11ShaderResourceView *vsrv[4];
    ID3D11Buffer *vb[16];
    UINT vbs[16], vbo[16];
} s_sh;
static unsigned long long s_sh_asks, s_sh_skips, s_sh_bad;
static int s_sh_explicit;           /* BW_GPU_DELTA was set: its counts every 300 flips */

static int delta_mode(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("BW_GPU_DELTA");
        s_sh_explicit = e && *e;
        v = !e || !*e ? bw_edition_at_least(BW_ED_REMASTERED)
                      : !strcmp(e, "verify") ? 2 : e[0] == '1' ? 1 : 0;
        if (v)
            fprintf(stderr, "[GPU-DELTA] only changed bindings sent%s (BW_GPU_DELTA)\n",
                    v == 2 ? ", every skip checked against the context" : "");
    }
    return v;
}

/* The mode, with the shadow forgotten if anything else set state since. */
static int sh_mode(void)
{
    int m = delta_mode();
    if (m && s_sh.epoch != g_bw_ctx_epoch) {
        memset(&s_sh, 0, sizeof s_sh);
        s_sh.epoch = g_bw_ctx_epoch;
    }
    if (m)
        s_sh_asks++;
    return m;
}

static int sh_bad(const char *what)
{
    if (s_sh_bad++ < 16)
        fprintf(stderr, "[GPU-DELTA] verify: flip %u: the shadow's %s is not the context's\n", bwg_gen, what);
    return 1;
}

static void sh_line(void)
{
    if (!delta_mode() || !s_sh_explicit || (bwg_gen % 300) != 0)
        return;
    fprintf(stderr, "[GPU-DELTA] flip %u: bindings asked %llu, skipped %llu (%.1f%%), verify mismatches %llu\n",
            bwg_gen, s_sh_asks, s_sh_skips, s_sh_asks ? 100.0 * (double)s_sh_skips / (double)s_sh_asks : 0.0,
            s_sh_bad);
}

#define SH_HAVE(bit) (s_sh.valid & (1u << (bit)))

static void sh_ps(ID3D11PixelShader *ps)
{
    int m = sh_mode();
    if (m && SH_HAVE(SH_PS) && s_sh.ps == ps) {
        int bad = 0;
        if (m == 2) {
            ID3D11PixelShader *c = NULL;
            ID3D11DeviceContext_PSGetShader(s_ctx, &c, NULL, NULL);
            bad = c != ps && sh_bad("pixel shader");
            if (c) ID3D11PixelShader_Release(c);
        }
        if (!bad) { s_sh_skips++; return; }
    }
    ID3D11DeviceContext_PSSetShader(s_ctx, ps, NULL, 0);
    s_sh.ps = ps;
    s_sh.valid |= 1u << SH_PS;
}

static void sh_vs(ID3D11VertexShader *vs)
{
    int m = sh_mode();
    if (m && SH_HAVE(SH_VS) && s_sh.vs == vs) {
        int bad = 0;
        if (m == 2) {
            ID3D11VertexShader *c = NULL;
            ID3D11DeviceContext_VSGetShader(s_ctx, &c, NULL, NULL);
            bad = c != vs && sh_bad("vertex shader");
            if (c) ID3D11VertexShader_Release(c);
        }
        if (!bad) { s_sh_skips++; return; }
    }
    ID3D11DeviceContext_VSSetShader(s_ctx, vs, NULL, 0);
    s_sh.vs = vs;
    s_sh.valid |= 1u << SH_VS;
}

static void sh_ps_cb0(ID3D11Buffer *cb)
{
    int m = sh_mode();
    if (m && SH_HAVE(SH_PSCB0) && s_sh.pscb0 == cb) {
        int bad = 0;
        if (m == 2) {
            ID3D11Buffer *c = NULL;
            ID3D11DeviceContext_PSGetConstantBuffers(s_ctx, 0, 1, &c);
            bad = c != cb && sh_bad("pixel constant buffer");
            if (c) ID3D11Buffer_Release(c);
        }
        if (!bad) { s_sh_skips++; return; }
    }
    ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 1, &cb);
    s_sh.pscb0 = cb;
    s_sh.valid |= 1u << SH_PSCB0;
}

static void sh_vs_cb(UINT start, UINT n, ID3D11Buffer *const *cb)
{
    int m = sh_mode(), need = !m;
    UINT i;
    for (i = 0; i < n && !need; i++)
        if (!(s_sh.vcb_ok & (1u << (start + i))) || s_sh.vcb[start + i] != cb[i])
            need = 1;
    if (!need && m == 2) {
        ID3D11Buffer *c[4] = { NULL, NULL, NULL, NULL };
        ID3D11DeviceContext_VSGetConstantBuffers(s_ctx, start, n, c);
        for (i = 0; i < n; i++) {
            if (c[i] != cb[i] && !need)
                need = sh_bad("vertex constant buffers");
            if (c[i]) ID3D11Buffer_Release(c[i]);
        }
    }
    if (!need) { s_sh_skips++; return; }
    ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, start, n, cb);
    for (i = 0; i < n; i++) {
        s_sh.vcb[start + i] = cb[i];
        s_sh.vcb_ok |= 1u << (start + i);
    }
}

static void sh_vs_srv(UINT start, UINT n, ID3D11ShaderResourceView *const *v)
{
    int m = sh_mode(), need = !m;
    UINT i;
    for (i = 0; i < n && !need; i++)
        if (!(s_sh.vsrv_ok & (1u << (start + i))) || s_sh.vsrv[start + i] != v[i])
            need = 1;
    if (!need && m == 2) {
        ID3D11ShaderResourceView *c[4] = { NULL, NULL, NULL, NULL };
        ID3D11DeviceContext_VSGetShaderResources(s_ctx, start, n, c);
        for (i = 0; i < n; i++) {
            if (c[i] != v[i] && !need)
                need = sh_bad("vertex shader resources");
            if (c[i]) ID3D11ShaderResourceView_Release(c[i]);
        }
    }
    if (!need) { s_sh_skips++; return; }
    ID3D11DeviceContext_VSSetShaderResources(s_ctx, start, n, v);
    for (i = 0; i < n; i++) {
        s_sh.vsrv[start + i] = v[i];
        s_sh.vsrv_ok |= 1u << (start + i);
    }
}

static void sh_ps_srv(UINT start, UINT n, ID3D11ShaderResourceView *const *v)
{
    int m = sh_mode(), need = !m;
    UINT i;
    for (i = 0; i < n && !need; i++)
        if (!(s_sh.srv_ok & (1u << (start + i))) || s_sh.srv[start + i] != v[i])
            need = 1;
    if (!need && m == 2) {
        ID3D11ShaderResourceView *c[8] = { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };
        ID3D11DeviceContext_PSGetShaderResources(s_ctx, start, n, c);
        for (i = 0; i < n; i++) {
            if (c[i] != v[i] && !need)
                need = sh_bad("pixel shader resources");
            if (c[i]) ID3D11ShaderResourceView_Release(c[i]);
        }
    }
    if (!need) { s_sh_skips++; return; }
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, start, n, v);
    for (i = 0; i < n; i++) {
        s_sh.srv[start + i] = v[i];
        s_sh.srv_ok |= 1u << (start + i);
    }
}

static void sh_ps_smp(UINT start, UINT n, ID3D11SamplerState *const *v)
{
    int m = sh_mode(), need = !m;
    UINT i;
    for (i = 0; i < n && !need; i++)
        if (!(s_sh.smp_ok & (1u << (start + i))) || s_sh.smp[start + i] != v[i])
            need = 1;
    if (!need && m == 2) {
        ID3D11SamplerState *c[8] = { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };
        ID3D11DeviceContext_PSGetSamplers(s_ctx, start, n, c);
        for (i = 0; i < n; i++) {
            if (c[i] != v[i] && !need)
                need = sh_bad("samplers");
            if (c[i]) ID3D11SamplerState_Release(c[i]);
        }
    }
    if (!need) { s_sh_skips++; return; }
    ID3D11DeviceContext_PSSetSamplers(s_ctx, start, n, v);
    for (i = 0; i < n; i++) {
        s_sh.smp[start + i] = v[i];
        s_sh.smp_ok |= 1u << (start + i);
    }
}

static void sh_rs(ID3D11RasterizerState *rs)
{
    int m = sh_mode();
    if (m && SH_HAVE(SH_RS) && s_sh.rs == rs) {
        int bad = 0;
        if (m == 2) {
            ID3D11RasterizerState *c = NULL;
            ID3D11DeviceContext_RSGetState(s_ctx, &c);
            bad = c != rs && sh_bad("rasteriser state");
            if (c) ID3D11RasterizerState_Release(c);
        }
        if (!bad) { s_sh_skips++; return; }
    }
    ID3D11DeviceContext_RSSetState(s_ctx, rs);
    s_sh.rs = rs;
    s_sh.valid |= 1u << SH_RS;
}

static void sh_blend(ID3D11BlendState *bs, const FLOAT bf[4], UINT mask)
{
    int m = sh_mode();
    if (m && SH_HAVE(SH_BS) && s_sh.bs == bs && s_sh.mask == mask && !memcmp(s_sh.bf, bf, sizeof s_sh.bf)) {
        int bad = 0;
        if (m == 2) {
            ID3D11BlendState *c = NULL;
            FLOAT cf[4];
            UINT cm = 0;
            ID3D11DeviceContext_OMGetBlendState(s_ctx, &c, cf, &cm);
            bad = (c != bs || cm != mask || memcmp(cf, bf, sizeof cf)) && sh_bad("blend state");
            if (c) ID3D11BlendState_Release(c);
        }
        if (!bad) { s_sh_skips++; return; }
    }
    ID3D11DeviceContext_OMSetBlendState(s_ctx, bs, bf, mask);
    s_sh.bs = bs;
    memcpy(s_sh.bf, bf, sizeof s_sh.bf);
    s_sh.mask = mask;
    s_sh.valid |= 1u << SH_BS;
}

static void sh_ds(ID3D11DepthStencilState *ds, UINT ref)
{
    int m = sh_mode();
    if (m && SH_HAVE(SH_DS) && s_sh.ds == ds && s_sh.ref == ref) {
        int bad = 0;
        if (m == 2) {
            ID3D11DepthStencilState *c = NULL;
            UINT cr = 0;
            ID3D11DeviceContext_OMGetDepthStencilState(s_ctx, &c, &cr);
            bad = (c != ds || cr != ref) && sh_bad("depth-stencil state");
            if (c) ID3D11DepthStencilState_Release(c);
        }
        if (!bad) { s_sh_skips++; return; }
    }
    ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, ds, ref);
    s_sh.ds = ds;
    s_sh.ref = ref;
    s_sh.valid |= 1u << SH_DS;
}

static void sh_il(ID3D11InputLayout *il)
{
    int m = sh_mode();
    if (m && SH_HAVE(SH_IL) && s_sh.il == il) {
        int bad = 0;
        if (m == 2) {
            ID3D11InputLayout *c = NULL;
            ID3D11DeviceContext_IAGetInputLayout(s_ctx, &c);
            bad = c != il && sh_bad("input layout");
            if (c) ID3D11InputLayout_Release(c);
        }
        if (!bad) { s_sh_skips++; return; }
    }
    ID3D11DeviceContext_IASetInputLayout(s_ctx, il);
    s_sh.il = il;
    s_sh.valid |= 1u << SH_IL;
}

static void sh_topo(D3D11_PRIMITIVE_TOPOLOGY t)
{
    int m = sh_mode();
    if (m && SH_HAVE(SH_TOPO) && s_sh.topo == t) {
        int bad = 0;
        if (m == 2) {
            D3D11_PRIMITIVE_TOPOLOGY c = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
            ID3D11DeviceContext_IAGetPrimitiveTopology(s_ctx, &c);
            bad = c != t && sh_bad("topology");
        }
        if (!bad) { s_sh_skips++; return; }
    }
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, t);
    s_sh.topo = t;
    s_sh.valid |= 1u << SH_TOPO;
}

static void sh_ib(ID3D11Buffer *ib, DXGI_FORMAT f, UINT off)
{
    int m = sh_mode();
    if (m && SH_HAVE(SH_IB) && s_sh.ib == ib && s_sh.ibf == f && s_sh.ibo == off) {
        int bad = 0;
        if (m == 2) {
            ID3D11Buffer *c = NULL;
            DXGI_FORMAT cf = DXGI_FORMAT_UNKNOWN;
            UINT co = 0;
            ID3D11DeviceContext_IAGetIndexBuffer(s_ctx, &c, &cf, &co);
            bad = (c != ib || cf != f || co != off) && sh_bad("index buffer");
            if (c) ID3D11Buffer_Release(c);
        }
        if (!bad) { s_sh_skips++; return; }
    }
    ID3D11DeviceContext_IASetIndexBuffer(s_ctx, ib, f, off);
    s_sh.ib = ib;
    s_sh.ibf = f;
    s_sh.ibo = off;
    s_sh.valid |= 1u << SH_IB;
}

static void sh_vb(UINT start, UINT n, ID3D11Buffer *const *b, const UINT *strides, const UINT *offs)
{
    int m = sh_mode(), need = !m;
    UINT i;
    for (i = 0; i < n && !need; i++)
        if (!(s_sh.vb_ok & (1u << (start + i))) || s_sh.vb[start + i] != b[i]
                || s_sh.vbs[start + i] != strides[i] || s_sh.vbo[start + i] != offs[i])
            need = 1;
    if (!need && m == 2) {
        ID3D11Buffer *c[16];
        UINT cs[16], co[16];
        memset(c, 0, sizeof c);
        ID3D11DeviceContext_IAGetVertexBuffers(s_ctx, start, n, c, cs, co);
        for (i = 0; i < n; i++) {
            if ((c[i] != b[i] || cs[i] != strides[i] || co[i] != offs[i]) && !need)
                need = sh_bad("vertex buffers");
            if (c[i]) ID3D11Buffer_Release(c[i]);
        }
    }
    if (!need) { s_sh_skips++; return; }
    ID3D11DeviceContext_IASetVertexBuffers(s_ctx, start, n, b, strides, offs);
    for (i = 0; i < n; i++) {
        s_sh.vb[start + i] = b[i];
        s_sh.vbs[start + i] = strides[i];
        s_sh.vbo[start + i] = offs[i];
        s_sh.vb_ok |= 1u << (start + i);
    }
}


static int xdraw_prepare(const Nv2aSurface *s, const BwXBatch *b)
{
    const Nv2aRenderState *rs;
    const Nv2aCombiner *rc;
    BwRcKey key;
    BwXConsts c;
    D3D11_MAPPED_SUBRESOURCE m;
    ID3D11PixelShader *ps;
    ID3D11ShaderResourceView *srv[4] = { NULL, NULL, NULL, NULL };
    ID3D11SamplerState *ss[4] = { NULL, NULL, NULL, NULL };
    ID3D11RasterizerState *rst;
    float bf[4];
    int st;

    stats_tick();
    if (!b || !b->state || !b->rc || !begin(s, b->state) || !ensure_xpipeline())
        return 0;
    rs = b->state;
    rc = b->rc;

    {
        BwgTexRef tex[4];               /* the cube maps the key fetched */
        bwg_xdraw_key(b, s_vfx_overlay, &key, tex);
        for (st = 0; st < 4; st++)
            if (tex[st].e || tex[st].rt)
                srv[st] = tex_view(tex[st]);
    }
    /* T1 (VFX_TEX_RESTORE): 3D stages whose texture has a restored chain
     * (not overlays, and not the water's dot-product reflection program).
     * Test aid: BW_GPU_AB=texrestore draws odd frames with the originals. */
    int tr_lk[4] = { 0, 0, 0, 0 };
    double trc_t0 = trc_on() ? bwg_qpc_ms() : 0.0;
    if (bw_vfx_active() && bw_vfx()->tex_restore && !s_vfx_overlay && !s_tr_skip
            && !bwg_dot_program(rc->stage_program) && !bw_ab_off("texrestore"))
        for (st = 0; st < 4; st++)
            if (!srv[st] && (tr_lk[st] = tr_stage(b, rc->stage_program, st, &srv[st])) != 0
                    && bw_vfx()->tr_debug)
                key.vfx[st] |= BW_RC_VFX_TR_DEBUG;
    if (trc_on())
        s_trc_trs += bwg_qpc_ms() - trc_t0;
    int vfx_a2c = bwg_xdraw_a2c(rs, s_vfx_overlay, &key);
    stage_stats(b, rc->stage_program, srv);
    bwg_rc_key_trim(&key);                              /* item 41: unused stages zeroed */
    if (trc_on()) {                                     /* T1 test aid */
        double t0 = bwg_qpc_ms();
        unsigned n0 = s_ps_compiles;
        ps = get_rc_shader(&key);
        s_trc_sh += bwg_qpc_ms() - t0;
        s_trc_nsh += s_ps_compiles - n0;
    } else
    ps = get_rc_shader(&key);
    if (!ps)
        return 0;
    rst = get_rs(rs);
    {
        Nv2aRenderState t;
        if (bwg_ocean_culltest(&key, rs, &t))       /* P2a test aid (Remastered only) */
            rst = get_rs(&t);
    }
    if (!rst)
        return -1;              /* both faces culled */

    bwg_xdraw_consts(s, b, &c);
    for (st = 0; st < 4; st++) {
        if (!b->tex_valid[st])
            continue;
        if (!srv[st])
            srv[st] = get_texture(&b->tex[st]);
        if (tr_lk[st])                          /* T1: a restored chain (0 in Original) */
            ss[st] = get_sampler_tr(b->tex[st].addr_u, b->tex[st].addr_v, b->tex[st].filter,
                                    b->tex_border_src[st], b->tex_border[st], tr_lk[st]);
        else
            ss[st] = get_sampler_f(b->tex[st].addr_u, b->tex[st].addr_v, b->tex[st].filter,
                                   b->tex_border_src[st], b->tex_border[st]);
        bwg_xdraw_stage(b, st, &c);
    }
    if (trc_on())
        trc_t0 = bwg_qpc_ms();
    {
        /* item 43: upload only when the constants changed (a DISCARD map
         * per draw renames the buffer in the driver every time). c is
         * zeroed first, so its padding compares too. */
        static BwXConsts shadow;
        static int shadow_ok;
        if (!bwg_memo_on() || !shadow_ok || memcmp(&shadow, &c, sizeof c)) {
            shadow_ok = 0;
            if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_xcb, 0,
                                                  D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                memcpy(m.pData, &c, sizeof(c));
                ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_xcb, 0);
                shadow = c;
                shadow_ok = 1;
                s_st_xcbup++;
            }
        }
    }
    if (trc_on())
        s_trc_map += bwg_qpc_ms() - trc_t0;
    memcpy(s_xlast_surf, c.surf, sizeof s_xlast_surf);
    memcpy(s_xlast_depth, c.depth, sizeof s_xlast_depth);
    memcpy(s_xlast_wsx, c.wsx, sizeof s_xlast_wsx);
    bwg_blend_factor(rs, bf);

    sh_ps(ps);                                          /* I3: each sent only if it changed */
    sh_ps_cb0(s_xcb);
    sh_ps_srv(0, 4, srv);
    sh_ps_smp(0, 4, ss);
    if (key.vfx[0] | key.vfx[1] | key.vfx[2] | key.vfx[3]) {      /* VFX (D9) */
        ID3D11SamplerState *lin = bw_vfx_linear_sampler();
        sh_ps_smp(4, 1, &lin);
    }
    sh_rs(rst);
    s_vfx_a2c = vfx_a2c;
    sh_blend(get_blend(rs), bf, 0xFFFFFFFFu);
    s_vfx_a2c = 0;
    sh_ds(get_ds(rs, bwg_cur_depth != NULL), rs->stencil_ref & 0xFF);
    return 1;
}

/* A dynamic buffer used as a ring: each draw appends its data with
 * WRITE_NO_OVERWRITE, and only a wrap DISCARDs. Mapping a whole buffer with
 * DISCARD for every draw (as the first versions did) makes the driver rename
 * it each time; at 13,000 draws a second that cost the GPU thread about 40%,
 * spent inside the driver. */
typedef struct {
    ID3D11Buffer *buf;
    UINT size, pos;
    UINT bind;
} BwRing;

static BwRing s_vring = { NULL, 0, 0, D3D11_BIND_VERTEX_BUFFER };
static BwRing s_iring = { NULL, 0, 0, D3D11_BIND_INDEX_BUFFER };

/* Append `bytes` at an `align`-byte boundary. Returns the byte offset, or
 * ~0u on failure. */
/* P1: room for `bytes` at an `align`-byte boundary, mapped; *off is its
 * offset. The caller fills it and calls ring_end. NULL on failure. */
static uint8_t *ring_begin(BwRing *r, UINT bytes, UINT align, UINT *off_out)
{
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
    UINT off;

    if (!bytes)
        return NULL;
    if (!r->buf || bytes > r->size) {
        D3D11_BUFFER_DESC bd;
        UINT want = bytes * 4 > (32u << 20) ? bytes * 4 : (32u << 20);
        if (r->buf) ID3D11Buffer_Release(r->buf);
        r->buf = NULL;
        r->size = 0;
        memset(&bd, 0, sizeof(bd));
        bd.ByteWidth = want;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = r->bind;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &r->buf))) {
            r->buf = NULL;
            return NULL;
        }
        r->size = want;
        r->pos = want;                  /* the first map discards */
    }
    off = (r->pos + align - 1) / align * align;
    if (off > r->size || r->size - off < bytes) {
        mode = D3D11_MAP_WRITE_DISCARD;
        off = 0;
    }
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)r->buf, 0, mode, 0, &m)))
        return NULL;
    *off_out = off;
    return (uint8_t *)m.pData + off;
}

static void ring_end(BwRing *r, UINT off, UINT bytes)
{
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)r->buf, 0);
    r->pos = off + bytes;
}

static UINT ring_push(BwRing *r, const void *data, UINT bytes, UINT align)
{
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
    UINT off;

    if (!bytes)
        return ~0u;
    if (!r->buf || bytes > r->size) {
        D3D11_BUFFER_DESC bd;
        UINT want = bytes * 4 > (32u << 20) ? bytes * 4 : (32u << 20);
        if (r->buf) ID3D11Buffer_Release(r->buf);
        r->buf = NULL;
        r->size = 0;
        memset(&bd, 0, sizeof(bd));
        bd.ByteWidth = want;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = r->bind;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &r->buf))) {
            r->buf = NULL;
            return ~0u;
        }
        r->size = want;
        r->pos = want;                  /* the first map discards */
    }
    off = (r->pos + align - 1) / align * align;
    if (off > r->size || r->size - off < bytes) {
        mode = D3D11_MAP_WRITE_DISCARD;
        off = 0;
    }
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)r->buf, 0, mode, 0, &m)))
        return ~0u;
    memcpy((uint8_t *)m.pData + off, data, bytes);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)r->buf, 0);
    r->pos = off + bytes;
    return off;
}

/* VFX (D9): the scene before the HUD. In a mission the HUD, subtitles and
 * every overlay come last in the frame, through D3D's 4-instruction
 * pass-through program (screen-space vertices) with the depth test off; the
 * 3D before them never uses it with the depth test off (BW_VFX_DRAWLOG,
 * run 20261027-023734-r6-gpu-d9drawlog, frame 560: 602 draws, the 16
 * overlay draws are the last 16). At the first such draw in a frame the
 * target is copied: that is the scene without the HUD. Bloom reads the
 * scene (so text and HUD never glow), and OLED care dims only the pixels the
 * HUD changed. In menus the copy is taken before anything is drawn, so
 * nothing blooms there. Only when bloom or OLED care is on. */
static ID3D11Texture2D *s_vfx_scene;
static ID3D11ShaderResourceView *s_vfx_scene_srv;
static uint32_t s_vfx_scene_w, s_vfx_scene_h, s_vfx_scene_gen;
static DXGI_FORMAT s_vfx_scene_fmt;
static BwSurf *s_vfx_scene_surf;

/* The scene copy is built per view: each 3D draw widens a pending rectangle
 * (its scissor, which is the view's window clip, TK-30), and the first
 * overlay draw after 3D copies that rectangle of the target into the copy.
 * In split screen each view's 3D is copied just before its own HUD, so every
 * view's scene is in it; a frame starts with the copy cleared, so regions
 * with no 3D (menus) count as HUD. */
static D3D11_RECT s_vfx_scissor;            /* the last scissor applied (physical) */
static D3D11_RECT s_vfx_pend;               /* 3D drawn since the last copy */
static int s_vfx_pend_on;
static BwSurf *s_vfx_pend_surf;
static ID3D11RenderTargetView *s_vfx_scene_rtv;

/* item 37: a surface slot is being reused for another target: the scene
 * copy must not take it for the surface it copied. */
static void vfx_surf_forget(BwSurf *s)
{
    if (s_vfx_scene_surf == s) {
        s_vfx_scene_surf = NULL;
        s_vfx_scene_gen = 0;
    }
    if (s_vfx_pend_surf == s) {
        s_vfx_pend_surf = NULL;
        s_vfx_pend_on = 0;
    }
}

static int vfx_wants_scene(void)
{
    const BwVfx *v = bw_vfx();
    return v->active && (v->bloom > 0.0f || v->oled_dim > 0.0f || v->ao > 0.0f);
}

/* A 3D (non-overlay) draw into bwg_cur. */
static void vfx_scene_draw(void)
{
    if (!vfx_wants_scene() || !bwg_cur)
        return;
    if (s_vfx_pend_surf != bwg_cur) {
        s_vfx_pend_surf = bwg_cur;
        s_vfx_pend_on = 0;
    }
    if (!s_vfx_pend_on) {
        s_vfx_pend = s_vfx_scissor;
        s_vfx_pend_on = 1;
    } else {
        if (s_vfx_scissor.left < s_vfx_pend.left) s_vfx_pend.left = s_vfx_scissor.left;
        if (s_vfx_scissor.top < s_vfx_pend.top) s_vfx_pend.top = s_vfx_scissor.top;
        if (s_vfx_scissor.right > s_vfx_pend.right) s_vfx_pend.right = s_vfx_scissor.right;
        if (s_vfx_scissor.bottom > s_vfx_pend.bottom) s_vfx_pend.bottom = s_vfx_scissor.bottom;
    }
}

/* Ambient occlusion on the 3D drawn so far in this view (VFX_AO). It runs
 * when the water starts (the first draw with the dot-product reflection
 * stages, program 0x62E21: the ocean, ANALYSIS "water's reflection"), or at
 * the view's first HUD draw if there is no water. The ocean is blended and
 * writes no depth, so after it the depth buffer still holds the seabed, and
 * AO there traced the underwater shoreline across the water (run
 * 20261027-035040-r6-gpu-d9ao-on); before it, the water simply covers the seabed's
 * AO. The view's planes and focal length come from the game's own projection
 * (vfx_view_proj); without a usable one, near 0.5 m (camera +0x140, seen),
 * far = the fog-driven global 0x1A9A68 (850 m in mission 1, seen) and the
 * focal length of the game's 90 degree horizontal field, half the view's
 * 4:3 width (D5's Hor+ widens the view by wsk, not the focal length). */
static int s_vfx_ao_done;                       /* AO already applied since the last HUD copy ... */
static unsigned s_vfx_ao_gen;                   /* ... in frame s_vfx_ao_gen */
static unsigned s_vfx_ao_view;                  /* views given AO so far in frame s_vfx_ao_gen */

/* Renderer camera n (n = 1..4) keeps the projection of game view n-1 at
 * 0x1C70F0 + n*0xB0 + 0x40 (D5's layout, bw_remaster_hooks.c; seen), made by
 * Math_MatrixPerspectiveOffCenterLH: M11 = 2zn/(t-b), M22 = zf/(zf-zn),
 * M23 = 1, M32 = zn*zf/(zn-zf). A view h pixels tall then has focal length
 * 0.5*h*M11 whatever its field (the 110 degree boost) and D5's Hor+ (which
 * scales l and r only), and its planes come back exactly. The views of a
 * frame are taken in drawing order, view i on camera i+1 (inferred: one
 * view per player, drawn in player order). 0 when the slot holds no
 * plausible perspective matrix. */
static int vfx_view_proj(unsigned view, float *m11, float *zn, float *zf)
{
    const float *m;
    float a, b;
    if (view > 3)
        return 0;
    if (BW_GFXCAP_REC())                /* BW_GFXCAP: game state the suite reads */
        bw_gfxcap_read(0x001C70F0u + (view + 1u) * 0xB0u + 0x40u, 64);
    m = (const float *)GUEST(0x001C70F0u + (view + 1u) * 0xB0u + 0x40u);
    a = m[10];
    b = m[14];
    if (!(m[5] > 0.05f && m[5] < 20.0f) || m[11] != 1.0f || !(a > 1.0f) || !(b < 0.0f))
        return 0;
    *m11 = m[5];
    *zn = -b / a;
    *zf = b / (1.0f - a);
    return *zn > 0.01f && *zn < 50.0f && *zf > *zn * 2.0f && *zf < 100000.0f;
}

/* Does this draw start the sea, for AO (0.2.5 item 50)? The sea's first pass
 * is its lit pass (stage program 0x21081: 110 tiles in mission 1, seen in
 * BW_VFX_DRAWLOG, run 20261103-023251-r6-gpu-i50log12), then the reflection
 * pass (0x62E21, the first dot-product program) and a third. AO used to
 * start at the reflection pass, after the lit pass had already been drawn
 * over the seabed, so the seabed's occlusion darkened the lit water. */
static int vfx_sea_starts(const BwXBatch *xb)
{
    static int old = -1;
    if (old < 0) {
        /* undo: BW_VFX_AO_AT_REFLECTION=1, AO at the reflection pass as in 0.2.4 */
        const char *e = getenv("BW_VFX_AO_AT_REFLECTION");
        old = e && e[0] == '1';
        if (old)
            fprintf(stderr, "[VFX] ambient occlusion starts at the sea's reflection pass, as in 0.2.4"
                            " (BW_VFX_AO_AT_REFLECTION=1)\n");
    }
    if (!xb || !xb->rc)
        return 0;
    /* BW_GPU_AB=aotrigger (test aid): the old trigger on odd frames */
    return (!old && !bw_ab_off("aotrigger") && xb->rc->stage_program == 0x21081u)
           || bwg_dot_program(xb->rc->stage_program);
}

static void vfx_drawlog_note(const char *what);     /* BW_VFX_DRAWLOG, below */

static void vfx_ao_now(const D3D11_RECT *rect)
{
    BwSurf *sf = bwg_cur;
    float zf, zn, focal, m11, pzn, pzf;
    D3D11_RECT r;
    if ((s_vfx_ao_done && s_vfx_ao_gen == bwg_gen) || bw_vfx()->ao <= 0.0f || !sf
            || !bwg_cur_depth || !bwg_cur_depth->srv)
        return;
    r = *rect;
    if (r.left < 0) r.left = 0;
    if (r.top < 0) r.top = 0;
    if (r.right > (LONG)sf->pw) r.right = (LONG)sf->pw;
    if (r.bottom > (LONG)sf->ph) r.bottom = (LONG)sf->ph;
    if (r.right <= r.left || r.bottom <= r.top)
        return;
    if (s_vfx_ao_gen != bwg_gen)
        s_vfx_ao_view = 0;
    if (BW_GFXCAP_REC())                /* BW_GFXCAP: game state the suite reads */
        bw_gfxcap_read(0x1A9A68, 4);
    zf = *(const float *)GUEST(0x1A9A68);
    if (!(zf > 50.0f && zf < 5000.0f))
        zf = 850.0f;
    zn = 0.5f;
    focal = 0.5f * (float)(r.right - r.left) / (sf->wsk > 1.0f ? sf->wsk : 1.0f);
    if (vfx_view_proj(s_vfx_ao_view, &m11, &pzn, &pzf)) {
        float pf = 0.5f * (float)(r.bottom - r.top) * m11;
        static int logged[4];
        if (!logged[s_vfx_ao_view & 3]) {
            logged[s_vfx_ao_view & 3] = 1;
            fprintf(stderr, "[VFX] AO view %u: camera %u projection M11 %.4f, planes %.3f..%.1f m"
                            " (assumed 0.5..%.1f), focal %.1f px (assumed %.1f), view %ldx%ld wsk %.4f\n",
                    s_vfx_ao_view, s_vfx_ao_view + 1, m11, pzn, pzf, zf, pf, focal,
                    (long)(r.right - r.left), (long)(r.bottom - r.top), sf->wsk);
        }
        zn = pzn;
        zf = pzf;
        focal = pf;
    }
    s_vfx_ao_view++;
    vfx_drawlog_note("AO applied here");    /* item 50 test aid: where AO runs in the frame */
    bw_vfx_ao_apply(bwg_cur_depth->srv, bwg_cur_depth->samples > 1, sf->pw, sf->ph,   /* item 36: its own count */
                    sf->mrtv ? sf->mrtv : sf->rtv, &r, zn, zf, focal);
    bind_targets(sf, bwg_cur_depth);
    SURF_DRAWN(sf);
    s_vfx_ao_done = 1;
    s_vfx_ao_gen = bwg_gen;
}

static void vfx_overlay_draw(void)
{
    BwSurf *sf = bwg_cur;
    DXGI_FORMAT f;
    D3D11_BOX box;
    if (!vfx_wants_scene() || !sf || !sf->tex)
        return;
    f = bw_vfx_target_format(sf->bpp);
    if (!s_vfx_scene || s_vfx_scene_w != sf->pw || s_vfx_scene_h != sf->ph || s_vfx_scene_fmt != f) {
        static uint32_t fail_gen;           /* item 36: one try a frame after a failure */
        D3D11_TEXTURE2D_DESC td;
        if (fail_gen == bwg_gen)
            return;
        if (s_vfx_scene_srv) ID3D11ShaderResourceView_Release(s_vfx_scene_srv);
        if (s_vfx_scene_rtv) ID3D11RenderTargetView_Release(s_vfx_scene_rtv);
        if (s_vfx_scene) ID3D11Texture2D_Release(s_vfx_scene);
        s_vfx_scene_srv = NULL;
        s_vfx_scene_rtv = NULL;
        s_vfx_scene = NULL;
        memset(&td, 0, sizeof(td));
        td.Width = sf->pw; td.Height = sf->ph; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = f;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        {
            HRESULT hr;
            if (FAILED(hr = ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_vfx_scene))
                || FAILED(hr = ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)s_vfx_scene,
                                                                     NULL, &s_vfx_scene_srv))
                || FAILED(hr = ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)s_vfx_scene,
                                                                   NULL, &s_vfx_scene_rtv))) {
                /* item 36: nothing half-made kept (a texture with no views
                 * once passed the size check and was cleared through NULL) */
                if (s_vfx_scene_srv) ID3D11ShaderResourceView_Release(s_vfx_scene_srv);
                if (s_vfx_scene_rtv) ID3D11RenderTargetView_Release(s_vfx_scene_rtv);
                if (s_vfx_scene) ID3D11Texture2D_Release(s_vfx_scene);
                s_vfx_scene_srv = NULL;
                s_vfx_scene_rtv = NULL;
                s_vfx_scene = NULL;
                s_vfx_scene_w = s_vfx_scene_h = 0;
                s_vfx_scene_gen = 0;
                fail_gen = bwg_gen;
                res_failed(RF_SCENE, sf->va, sf->pw, sf->ph, hr);
                return;
            }
        }
        s_vfx_scene_w = sf->pw; s_vfx_scene_h = sf->ph; s_vfx_scene_fmt = f;
        s_vfx_scene_gen = 0;
    }
    if (s_vfx_scene_gen != bwg_gen || s_vfx_scene_surf != sf) {
        static const float clear0[4] = { 0, 0, 0, 0 };
        ID3D11DeviceContext_ClearRenderTargetView(s_ctx, s_vfx_scene_rtv, clear0);
        s_vfx_scene_gen = bwg_gen;
        s_vfx_scene_surf = sf;
    }
    if (!s_vfx_pend_on || s_vfx_pend_surf != sf)
        return;                                     /* no 3D since the last copy */
    s_vfx_pend_on = 0;
    if (s_vfx_pend.left < 0) s_vfx_pend.left = 0;
    if (s_vfx_pend.top < 0) s_vfx_pend.top = 0;
    if (s_vfx_pend.right > (LONG)sf->pw) s_vfx_pend.right = (LONG)sf->pw;
    if (s_vfx_pend.bottom > (LONG)sf->ph) s_vfx_pend.bottom = (LONG)sf->ph;
    if (s_vfx_pend.right <= s_vfx_pend.left || s_vfx_pend.bottom <= s_vfx_pend.top)
        return;
    vfx_ao_now(&s_vfx_pend);                        /* no water in this view: AO now (no-op if done) */
    s_vfx_ao_done = 0;
    surf_resolve(sf);                               /* the MSAA samples into sf->tex */
    box.left = (UINT)s_vfx_pend.left; box.top = (UINT)s_vfx_pend.top; box.front = 0;
    box.right = (UINT)s_vfx_pend.right; box.bottom = (UINT)s_vfx_pend.bottom; box.back = 1;
    ID3D11DeviceContext_CopySubresourceRegion(s_ctx, (ID3D11Resource *)s_vfx_scene, 0,
                                              box.left, box.top, 0,
                                              (ID3D11Resource *)sf->tex, 0, &box);
}

/* The scene copy for the surface being presented, if this frame has one
 * (3D drawn after the last overlay draw is copied in now). */
static ID3D11ShaderResourceView *vfx_scene_for(BwSurf *sf)
{
    if (s_vfx_scene_gen != bwg_gen || s_vfx_scene_surf != sf)
        return NULL;
    if (s_vfx_pend_on && s_vfx_pend_surf == sf) {
        BwSurf *keep = bwg_cur;
        bwg_cur = sf;
        vfx_overlay_draw();
        bwg_cur = keep;
    }
    return s_vfx_scene_srv;
}

/* VFX (D9) test aid, BW_VFX_DRAWLOG=<frame>: every draw of that frame (and
 * the next), one line each: path (x CPU-transformed, v vertex program),
 * count, first vertex, depth test/write, blend, alpha test, stage-0 texture
 * and whether it is a glyph sheet. Used to tell the HUD's draws from the
 * scene's. */
static void vfx_drawlog(char path, uint32_t n, const BwXBatch *b, const BwXVertex *v0,
                        uint32_t ninstr)
{
    static long want = -2;
    const Nv2aRenderState *rs = b->state;
    if (want == -2) {
        const char *e = getenv("BW_VFX_DRAWLOG");
        want = e ? atol(e) : -1;
    }
    if (want < 0 || (long)bwg_gen < want || (long)bwg_gen > want + 1 || !rs)
        return;
    fprintf(stderr, "[VFX-DRAW] f%u %c n%u surf %08X%s", bwg_gen, path, n, bwg_cur ? bwg_cur->va : 0,
            bwg_cur && bwg_cur_depth ? "+z" : "");
    if (v0)
        fprintf(stderr, " v0(%.1f,%.1f,%.4g,rhw %.4g)", v0->x, v0->y, v0->z, v0->rhw);
    else
        fprintf(stderr, " prog %u", ninstr);
    fprintf(stderr, " zt%u/%X zw%u bl%u %X/%X at%u/%u cm%X t0 %08X %ux%u c%02X filt %08X%s sp %05X%s\n",
            rs->depth_test_enable, rs->depth_func, rs->depth_write, rs->blend_enable,
            rs->blend_src, rs->blend_dst, rs->alpha_test_enable, rs->alpha_ref, rs->color_mask,
            b->tex_valid[0] ? b->tex[0].offset : 0, b->tex_valid[0] ? b->tex[0].width : 0,
            b->tex_valid[0] ? b->tex[0].height : 0, b->tex_valid[0] ? b->tex[0].color : 0,
            b->tex_valid[0] ? b->tex[0].filter : 0, b->tex_valid[0] && tex_is_glyph(&b->tex[0]) ? " GLYPH" : "",
            b->rc ? b->rc->stage_program : 0,                   /* 0.2.5 item 50: the stage program */
            s_vfx_log_ov ? " OVERLAY" : "");                    /* and the overlay decision */
}

/* Item 50 test aid: a marker line in the drawlog (where AO runs). */
static void vfx_drawlog_note(const char *what)
{
    static long want = -2;
    if (want == -2) {
        const char *e = getenv("BW_VFX_DRAWLOG");
        want = e ? atol(e) : -1;
    }
    if (want >= 0 && (long)bwg_gen >= want && (long)bwg_gen <= want + 1)
        fprintf(stderr, "[VFX-DRAW] f%u -- %s\n", bwg_gen, what);
}

/* V1 test aid (with BW_VFX_DRAWLOG=<frame>): for overlay draws (D3D's
 * pass-through program, 4 instructions or fewer) of that frame, the stage-0
 * addressing and every vertex's inputs (v0 position, v2 texture coordinate),
 * to tell which HUD quad is which and how its texture is mapped. */
static void vfx_drawlog_inputs(const BwXVsBatch *b)
{
    static long want = -2;
    uint32_t i, a;
    if (want == -2) {
        const char *e = getenv("BW_VFX_DRAWLOG");
        want = e ? atol(e) : -1;
    }
    if (want < 0 || (long)bwg_gen != want || b->ninstr > 4 || !b->inputs)
        return;
    fprintf(stderr, "[VFX-IN] t0 %08X addr %u/%u mask %04X nverts %u:",
            b->xb.tex_valid[0] ? b->xb.tex[0].offset : 0, b->xb.tex[0].addr_u, b->xb.tex[0].addr_v,
            b->input_mask, b->nverts);
    for (i = 0; i < b->nverts && i < 8; i++) {
        fprintf(stderr, " [");
        for (a = 0; a < b->ninputs && a < 4; a++)
            fprintf(stderr, "%s%.4g,%.4g", a ? " | " : "", b->inputs[(i * b->ninputs + a) * 4],
                    b->inputs[(i * b->ninputs + a) * 4 + 1]);
        fprintf(stderr, "]");
    }
    fprintf(stderr, "\n");
}

/* BW_GFXCAP_CLASSES (test aid, lane gfx-capef; the replay only, unset inert):
 * the texture a draw into sf renders into, and the draw just made into bwg_cur
 * classed for the comparison tool's region masks (bw_gfxcls.c). xb is NULL
 * for a screen-space batch, whose texture is t. */
static void *gfxcls_tex(const BwSurf *sf)
{
    return sf ? (void *)(sf->mrtv ? sf->mtex : sf->tex) : NULL;
}

static void gfxcls_drawn(const BwXBatch *xb, int overlay, const Nv2aTexture *t, const Nv2aRenderState *rs,
                         const BwXVsBatch *vb)
{
    BwClsDraw d;
    int st;
    memset(&d, 0, sizeof(d));
    d.overlay = overlay;
    d.gen = bwg_gen;
    d.points = bw_backend_batch_points();  /* gfx-sprites: the sprite class (BW_GFXCLS_SPRITES) */
    if (vb && vb->nstreams && vb->streams[vb->in[0].stream].data)    /* the first array, as a guest address */
        d.vtx = (uint32_t)((uintptr_t)vb->streams[vb->in[0].stream].data - (uintptr_t)g_xbox_mem_offset);
    d.depth_test = rs && rs->depth_test_enable;
    d.sea = xb && xb->rc && (xb->rc->stage_program == 0x21081u || bwg_dot_program(xb->rc->stage_program));
    for (st = 0; st < 4; st++) {
        const Nv2aTexture *x = xb ? (xb->tex_valid[st] && !xb->tex_cube[st] ? &xb->tex[st] : NULL)
                                  : (st ? NULL : t);
        if (!x)
            continue;
        d.tex[st].valid = 1;
        d.tex[st].surface = bwg_find_surf(x->offset) != NULL;
        d.tex[st].offset = x->offset;
        d.tex[st].width = x->width;
        d.tex[st].height = x->height;
        d.tex[st].color = x->color;
    }
    bw_gfxcls_draw(s_ctx, gfxcls_tex(bwg_cur), bwg_gen, bw_gfxcls_classify(&d));
}

static void bw_xdraw(const Nv2aSurface *s, const BwXBatch *b)
{
    UINT stride = sizeof(BwXVertex), offs;
    int ov;                                 /* item 50: a handed-back overlay draw */

    if (!b || !b->count)
        return;
    bwg_ws_xbatch(s, b);                /* widescreen (D5): the 2D test */
    /* 0.2.5 item 50: a vertex-program batch handed back to this path (its
     * shader still compiling) keeps its overlay decision. An overlay (HUD)
     * draw takes the scene copy before it and the HUD's own treatments, as
     * on the vertex-program path, and doesn't count as scene (it used to, so
     * the HUD could glow under bloom and escape OLED care until its shader
     * was ready). A 3D draw that starts the sea runs AO first, as there. */
    ov = bw_vfx_active() && s_tr_fb_overlay;
    if (ov) {
        static unsigned n_ov;
        if (++n_ov <= 3 || n_ov % 100 == 0)
            fprintf(stderr, "[VFX] HUD draw %u went the CPU way (its vertex program is still compiling),"
                            " frame %u: kept as HUD for the effects\n", n_ov, bwg_gen);
    }
    s_tr_skip = s_tr_fb_overlay;            /* T1 */
    s_vfx_log_ov = s_tr_fb_overlay;         /* item 50: for the drawlog */
    s_tr_fb_overlay = 0;
    if (bw_vfx_active() && b->state && begin(s, b->state)) {
        if (ov)
            vfx_overlay_draw();
        else if (vfx_sea_starts(b) && s_vfx_pend_on && s_vfx_pend_surf == bwg_cur)
            vfx_ao_now(&s_vfx_pend);
    }
    s_vfx_overlay = ov;
    {
        double t0 = trc_on() ? bwg_qpc_ms() : 0.0;
        int r = xdraw_prepare(s, b);
        if (trc_on())
            s_trc_prep += bwg_qpc_ms() - t0;
        s_tr_skip = 0;
        s_vfx_overlay = 0;
        if (r <= 0)
            return;
    }
    if (bwg_tex_watch > 0 && bwg_cur)
        note_huge(b->vertices, b->count, bwg_cur->w, bwg_cur->h);
    offs = ring_push(&s_vring, b->vertices, b->count * stride, stride);
    if (offs == ~0u)
        return;
    sh_il(s_xlayout);                   /* I3: each sent only if it changed */
    sh_topo(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    sh_vb(0, 1, &s_vring.buf, &stride, &offs);
    sh_vs(s_xvs);
    sh_vs_cb(0, 1, &s_xcb);
    if (trc_on()) {                                         /* T1 test aid */
        double t0 = bwg_qpc_ms();
        ID3D11DeviceContext_Draw(s_ctx, b->count, 0);
        s_trc_draw += bwg_qpc_ms() - t0;
        s_trc_ndraw++;
    } else
    ID3D11DeviceContext_Draw(s_ctx, b->count, 0);
    if (g_bw_gfxcls) gfxcls_drawn(b, s_vfx_log_ov, NULL, b->state, NULL);     /* BW_GFXCAP_CLASSES (test aid) */
    if (bw_edgeprobe_on())              /* BW_GPU_EDGEPROBE (test aid) */
        bw_edgeprobe_at(BW_EP_DRAW, s_ctx, (bwg_cur->mtex ? (void *)bwg_cur->mtex : (void *)bwg_cur->tex), bwg_gen);
    {
        const BwXVertex *v0 = &b->vertices[0];
        uint32_t c0 = ((uint32_t)(v0->d0[3] * 255.0f) << 24) | ((uint32_t)(v0->d0[0] * 255.0f) << 16)
                    | ((uint32_t)(v0->d0[1] * 255.0f) << 8) | (uint32_t)(v0->d0[2] * 255.0f);
        if (bw_vfx_active()) {
            vfx_drawlog('x', b->count, b, v0, 0);          /* VFX (D9) test aid */
            if (!ov)
                vfx_scene_draw();                          /* the CPU path's other draws are 3D */
        }
        ft_draw('x', b->count, v0->x, v0->y, v0->z, v0->rhw, c0,
                b->tex_valid[0] ? b->tex[0].offset : 0, b->state);
    }
    SURF_DRAWN(bwg_cur);
    s_st_draws++;
    s_st_xdraws++;
    s_st_tris += b->count / 3;
}

/* ---- vertex programs on the GPU (bw_vsh_hlsl.c) -------------------------- */

/* One translated program. Shaders compile on a worker thread (D3DCompile
 * needs no device, and the device is single-threaded), so a program seen for
 * the first time draws on the CPU path for the frames until its shader is
 * ready, instead of a hitch. */
#define BW_MAX_VSH 1024         /* P1: a program has a shader per raw input format set too */
#define BW_VSH_MAX_INSTR 136
#define BW_VSH_RAW_IL 8         /* P1: raw input layouts kept per shader */
typedef struct {
    uint64_t key;
    D3D11_INPUT_ELEMENT_DESC el[16];
    ID3D11InputLayout *il;
    int failed;                 /* 0.2.5 item 43: the device refused this one; not asked again */
} BwRawIl;
typedef struct {
    uint64_t hash;
    uint32_t n, mask, nin;
    uint8_t conv[16];           /* P1: the shader's conversions (in_conv of bw_vsh_hlsl.c); 0 = floats */
    uint8_t points;             /* gfx-sprites: the point variant (writes oPts and the sprite's centre) */
    BwRawIl raw[BW_VSH_RAW_IL]; /* P1: input layouts for raw streams, by formats/slots/offsets */
    uint32_t raw_next;
    uint32_t words[BW_VSH_MAX_INSTR][4];
    char *src;                  /* worker input */
    size_t len;
    ID3DBlob *blob;             /* worker output */
    volatile LONG state;        /* 0 queued, 1 compiled, 2 ready, -1 failed */
    ID3D11VertexShader *vs;
    ID3D11InputLayout *il;      /* ATTR0..nin-1, float4 each, one stream */
    void *code;                 /* BW_GPU_VSH_VERIFY: the bytecode, for ... */
    SIZE_T code_size;
    ID3D11GeometryShader *so;   /* ... its stream-output twin */
    int ffp;                    /* gfx-ffp: a fixed-function shader (fkey; n 0, words unused) */
    BwFfpKey fkey;
} BwVsh;

static BwVsh *s_vsh;            /* BW_MAX_VSH entries, allocated on first use */
static HANDLE s_vsh_wake, s_vsh_thread;
static ID3D11Buffer *s_kcb;
static BwVshConsts s_kshadow;
static int s_kvalid;
static int s_gpu_vsh = -1;      /* BW_GPU_VSH=0: vertex programs on the CPU */
static BwVsh *s_vlast;          /* the last draw_vsh, for vsh_readback */
static UINT s_vlast_voff, s_vlast_stride, s_vlast_nverts;
static UINT s_vlast_ioff, s_vlast_nidx;  /* gfx-sprites: the last batch's indices ... */
static int s_vlast_points;              /* ... and whether it was a point list */

static DWORD WINAPI vsh_worker(LPVOID arg)
{
    (void)arg;
    for (;;) {
        LONG i, n = s_nvsh;
        int did = 0;
        for (i = 0; i < n; i++) {
            BwVsh *v = &s_vsh[i];
            ID3DBlob *blob = NULL, *err = NULL;
            LARGE_INTEGER t0, t1, f;
            if (v->state != 0)
                continue;
            QueryPerformanceCounter(&t0);
            if (FAILED(D3DCompile(v->src, v->len, "bw_vsh", WS_DEFINES, NULL, "vs_main", "vs_5_0",
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_IEEE_STRICTNESS,
                                  0, &blob, &err))) {
                if (InterlockedIncrement(&s_vsh_failures) <= 3) {
                    fprintf(stderr, "[GPU-VSH] shader %ld failed: %s\n", (long)i,
                            err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
                    fprintf(stderr, "%s\n", v->src);
                }
                if (err) ID3D10Blob_Release(err);
                free(v->src);           /* item 42: the worker's until the state changes */
                v->src = NULL;
                InterlockedExchange(&v->state, -1);
            } else {
                if (err) ID3D10Blob_Release(err);
                v->blob = blob;
                QueryPerformanceCounter(&t1);
                QueryPerformanceFrequency(&f);
                s_vsh_compile_ms += (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
                InterlockedIncrement(&s_vsh_compiles);
                InterlockedExchange(&v->state, 1);
            }
            did = 1;
        }
        if (!did)
            WaitForSingleObject(s_vsh_wake, INFINITE);
    }
}

static uint64_t prog_hash(const uint32_t (*p)[4], uint32_t n)
{
    uint64_t h = 1469598103934665603ull;
    uint32_t i, j;
    for (i = 0; i < n; i++)
        for (j = 0; j < 4; j++)
            h = (h ^ p[i][j]) * 1099511628211ull;
    return h ^ n;
}

/* The shader and input layout from a program's bytecode, made here on the
 * device's thread (a compile's, or the shader cache's: item 43). 1 when
 * both are made; else nothing is kept and *pwhat / *phr say what failed. */
static int vsh_make(BwVsh *v, const void *code, SIZE_T size, const char **pwhat, HRESULT *phr)
{
    static const char *const sem = "ATTR";
    D3D11_INPUT_ELEMENT_DESC il[16];
    uint32_t k;
    int ok;

    static const uint8_t no_conv[16];
    HRESULT hr;
    const char *what = "shader";
    ok = SUCCEEDED(hr = ID3D11Device_CreateVertexShader(s_dev, code, size, NULL, &v->vs));
    if (ok && v->nin && !memcmp(v->conv, no_conv, sizeof no_conv)) {   /* P1: raw-only shaders have none */
        for (k = 0; k < v->nin; k++) {
            il[k].SemanticName = sem;
            il[k].SemanticIndex = k;
            il[k].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            il[k].InputSlot = 0;
            il[k].AlignedByteOffset = 16 * k;
            il[k].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
            il[k].InstanceDataStepRate = 0;
        }
        what = "input layout";
        ok = SUCCEEDED(hr = ID3D11Device_CreateInputLayout(s_dev, il, v->nin, code, size, &v->il));
    }
    if (!ok) {
        /* item 42: nothing half-made kept (the shader leaked when its
         * layout failed); the program stays on the CPU path */
        if (v->il) ID3D11InputLayout_Release(v->il);
        if (v->vs) ID3D11VertexShader_Release(v->vs);
        v->il = NULL;
        v->vs = NULL;
        *pwhat = what;
        *phr = hr;
        return 0;
    }
    /* kept for BW_GPU_VSH_VERIFY's stream-output twin and (P1) the raw
     * streams' input layouts, made per format set */
    if ((v->code = malloc(size)) != NULL) {
        memcpy(v->code, code, size);
        v->code_size = size;
    }
    return 1;
}

/* A compiled program becomes usable (the worker's bytecode); item 43:
 * its bytecode is kept in the shader cache for later runs. */
static void vsh_finish(BwVsh *v)
{
    const void *code = ID3D10Blob_GetBufferPointer(v->blob);
    SIZE_T size = ID3D10Blob_GetBufferSize(v->blob);
    const char *what = "shader";
    HRESULT hr = S_OK;
    int ok = vsh_make(v, code, size, &what, &hr);
    if (!ok) {
        if (InterlockedIncrement(&s_vsh_failures) <= 3)
            fprintf(stderr, "[GPU-VSH] the device refused a compiled vertex program's %s (error 0x%08lX):"
                            " that program runs on the CPU\n", what, (unsigned long)hr);
    } else if (v->src)
        bw_pscache_store_vs(v->src, v->len, bwg_ws_k > 1.0f, code, size);
    ID3D10Blob_Release(v->blob);
    v->blob = NULL;
    free(v->src);
    v->src = NULL;
    v->state = ok ? 2 : -1;
}

/* item 43: programs the worker has compiled are finished at each flip, not
 * only when next drawn, so one drawn once (the boot's first) reaches the
 * shader cache too; it was compiled again in every run. */
static void vsh_finish_compiled(void)
{
    LONG i, n = s_nvsh;
    if (!s_vsh)
        return;
    for (i = 0; i < n; i++)
        if (s_vsh[i].state == 1)
            vsh_finish(&s_vsh[i]);
}

/* The shader-compile worker, started at the first new program (0.2.5 item
 * 42: both checked. Without the event the worker's wait returned at once and
 * it spun a core; a failed thread leaked the event and was tried again on
 * every new program. Either way the vertex programs now stay on the CPU path
 * (as BW_GPU_VSH=0)). 1 when it runs. */
static int vsh_worker_start(void)
{
    if (!s_vsh_thread) {
        const char *what = "CreateEventA";
        DWORD le = 0;
        s_vsh_wake = CreateEventA(NULL, FALSE, FALSE, NULL);
        if (!s_vsh_wake)
            le = GetLastError();
        else {
            what = "CreateThread";
            s_vsh_thread = CreateThread(NULL, 0, vsh_worker, NULL, 0, NULL);
            if (!s_vsh_thread) {
                le = GetLastError();
                CloseHandle(s_vsh_wake);
                s_vsh_wake = NULL;
            }
        }
        if (!s_vsh_thread) {
            s_gpu_vsh = 0;
            fprintf(stderr, "[GPU-VSH] the shader-compile worker could not start (%s failed, error %lu):"
                            " vertex programs run on the CPU from now on, as with BW_GPU_VSH=0\n",
                    what, (unsigned long)le);
            return 0;
        }
    }
    return 1;
}

/* ---- gfx-ffp: GPU_FFP and GPU_VSH_SYNC (bw_fixes.def) -------------------
 * GPU_FFP is a port change in every edition since GA-13 (the maintainer,
 * 2026-11-05: "Original gets the 100% native treatment"; `port` below):
 * on by default everywhere, outside the registry as ORIG-1 is for the audio;
 * in Remastered and later the registry's GPU_FFP stays as an alias
 * (BW_FIX=-GPU_FFP: the CPU path), and BW_GPU_FFP = 0 or 1 sets it in every
 * edition. The CPU path stays as the reference and the undo.
 * GPU_VSH_SYNC too since the load-time compile (lane gfx-regen, bw_vshpre.c)
 * showed 0 misses after load (the five smoke scenes from empty caches, the
 * 26-scene corpus: docs/gfx/lanes/regen.md section 5): a port change in every
 * edition, Original included (GA-13), on by default; the registry's
 * GPU_VSH_SYNC stays as an alias in Remastered and later (BW_FIX=-GPU_VSH_SYNC),
 * and BW_GPU_VSH_SYNC = 0 or 1 sets it in every edition (0: a shader still
 * compiling draws on the CPU path for those frames, as before). */
static int native_switch(const char *name, bw_fix_id_t id, int port, int *cache)
{
    if (*cache < 0) {
        char any_name[64];
        const char *e = getenv(name), *any;
        int rx = bw_edition_at_least(BW_ED_REMASTERED);
        int on = port && !rx ? 1 : bw_fix_enabled(id), replay_any;
        _snprintf(any_name, sizeof any_name, "%s_ANY_EDITION", name);
        any_name[sizeof any_name - 1] = 0;
        any = getenv(any_name);
        replay_any = !port && g_bw_gfxcap == 2 && any && any[0] == '1';
        if (e && *e && (port || rx || replay_any))
            on = e[0] != '0';
        *cache = on;
        if (on || (e && *e))
            fprintf(stderr, "[GPU-NATIVE] %s: %s\n", name,
                    on ? (replay_any && !rx ? "on (an Original capture's replay: measuring aid)" : "on")
                       : port ? "off (the CPU path, the reference)" : "off (Original keeps today's path)");
    }
    return *cache;
}

static int s_ffp_sw = -1, s_vsync_sw = -1;
/* the fixed function as a generated vertex shader (bw_ffp_hlsl, get_ffp) */
static int ffp_on(void) { return native_switch("BW_GPU_FFP", BW_FIX_GPU_FFP, 1, &s_ffp_sw); }
/* no CPU fallback while a vertex shader compiles: the manifest's warm-up and
 * a miss compiled at once (vsh_adopt) */
static int vsh_sync_on(void) { return native_switch("BW_GPU_VSH_SYNC", BW_FIX_GPU_VSH_SYNC, 1, &s_vsync_sw); }

/* This run's first sights of a vertex shader, by where its shader came from
 * (BW_GPU_STATS' [GPU-NATIVE] line): the warm-up, the cache, a compile on
 * this thread (a miss, BW_GPU_VSH_SYNC), the worker (a miss without it) */
static unsigned s_vs_warm, s_vs_cache, s_vs_now, s_vs_queued, s_vs_nowfail;
static double s_vs_now_ms, s_vs_now_max, s_vs_first_ms;
/* lane gfx-regen: first sights against the load-time compile (bw_vshpre.c):
 * its texts, of them waited for (and the longest wait), not among them; the
 * misses (compiled now, or queued without GPU_VSH_SYNC) once its batch was
 * done, and the longest of those compiles */
static unsigned s_pre_cov, s_pre_waits, s_pre_not, s_vs_now_late, s_vs_queued_late, s_pre_said;
static double s_pre_wait_max, s_vs_late_max, s_vs_cache_max;

static void vshpre_note(const BwVsh *v, int pre, double wms)
{
    if (pre) {
        s_pre_cov++;
        if (wms > 0.0) {
            s_pre_waits++;
            if (wms > s_pre_wait_max)
                s_pre_wait_max = wms;
        }
        return;
    }
    if (s_pre_not++ < 16) {
        /* what it is, as hashes and shape only (no program words): a
         * program's word hash, or the fixed-function shape's key bytes */
        char kx[2 * sizeof(BwFfpKey) + 1];
        size_t j;
        const uint8_t *kb = (const uint8_t *)&v->fkey;
        for (j = 0; j < sizeof(BwFfpKey); j++)
            _snprintf(kx + 2 * j, 3, "%02X", kb[j]);
        kx[2 * sizeof(BwFfpKey)] = 0;
        fprintf(stderr, "[GPU-VSHPRE] first sight at frame %u of a %s's shader not made at load time"
                        " (%u instructions, inputs %04X, conv %02X%02X%02X%02X%02X%02X%02X%02X, points %d, %s %s)\n",
                bwg_gen, v->ffp ? "fixed-function state" : "program", v->n, v->mask, v->conv[0], v->conv[1],
                v->conv[2], v->conv[3], v->conv[4], v->conv[5], v->conv[6], v->conv[7], (int)v->points,
                v->ffp ? "key" : "hash", v->ffp ? kx : "");
        if (!v->ffp) {
            fprintf(stderr, "[GPU-VSHPRE]   program hash %016llX\n", (unsigned long long)v->hash);
            if (getenv("BW_GPU_VSHPRE_WORDS")) {    /* analysis aid: the words, into this run's log only */
                uint32_t i;
                for (i = 0; i < v->n; i++)
                    fprintf(stderr, "[GPU-VSHPRE]   %2u: %08X %08X %08X %08X\n", i, v->words[i][0],
                            v->words[i][1], v->words[i][2], v->words[i][3]);
            }
        }
    }
}

/* The first sights' line, at a flip once a second when a count moved. */
static void vshpre_flip_line(void)
{
    static double last;
    unsigned sum = s_pre_cov + s_pre_not + s_vs_now + s_vs_queued;
    double now;
    bw_vshpre_line(0);
    if (sum == s_pre_said || (now = bwg_qpc_ms()) - last < 1000.0)
        return;
    last = now;
    s_pre_said = sum;
    fprintf(stderr, "[GPU-VSHPRE] first sights: %u made at load time (%u waited for its compile, the longest"
                    " %.1f ms), %u not; misses %u compiled now (%u after the load-time batch, the longest %.1f ms),"
                    " %u to the worker (%u after it); from the cache %u (the longest %.2f ms)\n", s_pre_cov, s_pre_waits, s_pre_wait_max, s_pre_not,
            s_vs_now, s_vs_now_late, s_vs_late_max, s_vs_queued, s_vs_queued_late, s_vs_cache, s_vs_cache_max);
}

/* The manifest's warm-up (BW_GPU_VSH_SYNC): at the first new vertex shader
 * of the run, every shader the manifest lists and the cache holds is made on
 * the device, so the driver's own compile happens here, before the game's
 * first frame, and a program's first sight only finds it. */
typedef struct {
    uint64_t h1, h2;
    uint32_t textlen;
    ID3D11VertexShader *vs;
    const void *code;           /* the cache's bytes */
    size_t size;
} BwVsWarm;
static BwVsWarm *s_warm;
static unsigned s_nwarm;
static uint64_t s_vs_stamp;

/* The translator's name for the manifest: the cache keys of a probe program's
 * and a probe fixed-function state's shaders (no game data), so a build
 * whose translation changed starts a list of its own. */
static uint64_t vsh_stamp(void)
{
    static char src[16 * 1024];
    static const uint32_t probe[1][4] = { { 0, 0x0020001Bu, 0x0836106Cu, 0x2F100FF9u } };
    BwFfpKey k;
    uint64_t h1, h2, s = 0;
    size_t n = bw_vsh_hlsl(probe, 1, bw_vsh_inputs_used(probe, 1), NULL, src, sizeof src);
    bw_pscache_vs_key(src, n, bwg_ws_k > 1.0f, &h1, &h2);
    s ^= h1;
    memset(&k, 0, sizeof k);
    memset(k.in, -1, sizeof k.in);
    k.in[BW_FFP_POS] = 0;
    k.nin = 1;
    n = bw_ffp_hlsl(&k, src, sizeof src);
    bw_pscache_vs_key(src, n, bwg_ws_k > 1.0f, &h1, &h2);
    return s ^ (h1 * 31u) ^ (uint64_t)n;
}

static void vsh_warmup(void)
{
    static int done;
    BwVsManifestEntry *m = NULL;
    unsigned n, i, made = 0, absent = 0, refused = 0, other = 0;
    double t0;
    if (done)
        return;
    done = 1;
    t0 = bwg_qpc_ms();
    s_vs_stamp = vsh_stamp();
    n = bw_pscache_manifest_read(s_vs_stamp, &m);
    if (n && (s_warm = (BwVsWarm *)calloc(n, sizeof *s_warm)) != NULL)
        for (i = 0; i < n; i++) {
            BwVsWarm *w = &s_warm[s_nwarm];
            size_t sz = 0;
            const void *cb;
            if (m[i].ws != (bwg_ws_k > 1.0f)) {
                other++;                /* another settings group's (widescreen or not) */
                continue;
            }
            if (!(cb = bw_pscache_find_vs_key(m[i].h1, m[i].h2, m[i].textlen, &sz))) {
                absent++;
                continue;
            }
            if (FAILED(ID3D11Device_CreateVertexShader(s_dev, cb, sz, NULL, &w->vs))) {
                w->vs = NULL;
                refused++;
                continue;
            }
            w->h1 = m[i].h1;
            w->h2 = m[i].h2;
            w->textlen = m[i].textlen;
            w->code = cb;
            w->size = sz;
            s_nwarm++;
            made++;
        }
    free(m);
    fprintf(stderr, "[GPU-NATIVE] warm-up: %u vertex shaders made from the manifest in %.1f ms (%u listed:"
                    " %u not in the cache, %u refused, %u for other settings)\n",
            made, bwg_qpc_ms() - t0, n, absent, refused, other);
}

/* A new entry's shader is ready (made from bytecode): list it in the
 * manifest (BW_GPU_VSH_SYNC) unless the warm-up made it. */
static void vsh_manifest_note(const BwVsh *v, const char *src, int warm)
{
    BwVsManifestEntry e;
    if (!vsh_sync_on() || warm)
        return;
    memset(&e, 0, sizeof e);
    bw_pscache_vs_key(src, v->len, bwg_ws_k > 1.0f, &e.h1, &e.h2);
    e.textlen = (uint32_t)v->len;
    e.ws = bwg_ws_k > 1.0f;
    e.kind = v->ffp ? 'f' : 'p';
    bw_pscache_manifest_add(s_vs_stamp, &e);
}

/* The shader of a new entry n (filled in up to its text, `src`, v->len): the
 * warm-up's (BW_GPU_VSH_SYNC), the cache's (item 43: the bytecode an earlier
 * run compiled from this text with these flags, so the program draws on the
 * GPU from its first batch), a compile now (BW_GPU_VSH_SYNC: counted, never
 * the CPU path), else the worker's (the CPU path draws it until then, as
 * before). Returns v when it can draw now, else NULL; the entry is published
 * (s_nvsh) either way. */
static BwVsh *vsh_adopt(BwVsh *v, LONG n, const char *src)
{
    double t0 = bwg_qpc_ms();
    int sync = vsh_sync_on();
    if (!v->len || !(v->src = (char *)malloc(v->len + 1))) {
        v->state = -1;
        InterlockedExchange(&s_nvsh, n + 1);
        return NULL;
    }
    memcpy(v->src, src, v->len + 1);
    {
        /* lane gfx-regen: the game's programs compiled at load time
         * (bw_vshpre.c, through the cache below). A first sight whose
         * compile is still running waits for it with GPU_VSH_SYNC; one not
         * started yet is made here, as any other */
        double wms = 0.0;
        int pre = bw_vshpre_wait(v->src, v->len, bwg_ws_k > 1.0f, sync, &wms);
        vshpre_note(v, pre, wms);
    }
    if (sync) {
        uint64_t h1, h2;
        unsigned i;
        vsh_warmup();
        bw_pscache_vs_key(v->src, v->len, bwg_ws_k > 1.0f, &h1, &h2);
        for (i = 0; i < s_nwarm; i++) {
            BwVsWarm *w = &s_warm[i];
            static const uint8_t no_conv[16];
            if (!w->vs || w->h1 != h1 || w->h2 != h2 || w->textlen != (uint32_t)v->len)
                continue;
            /* the warm-up's shader; its input layout (vsh_make's) and the
             * bytecode copy now */
            v->vs = w->vs;
            w->vs = NULL;
            if (v->nin && !memcmp(v->conv, no_conv, sizeof no_conv)) {
                D3D11_INPUT_ELEMENT_DESC il[16];
                uint32_t k;
                for (k = 0; k < v->nin; k++) {
                    il[k].SemanticName = "ATTR";
                    il[k].SemanticIndex = k;
                    il[k].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
                    il[k].InputSlot = 0;
                    il[k].AlignedByteOffset = 16 * k;
                    il[k].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
                    il[k].InstanceDataStepRate = 0;
                }
                if (FAILED(ID3D11Device_CreateInputLayout(s_dev, il, v->nin, w->code, w->size, &v->il))) {
                    ID3D11VertexShader_Release(v->vs);
                    v->vs = NULL;
                    v->il = NULL;
                    break;              /* made again below, as any other */
                }
            }
            if ((v->code = malloc(w->size)) != NULL) {
                memcpy(v->code, w->code, w->size);
                v->code_size = w->size;
            }
            free(v->src);
            v->src = NULL;
            v->state = 2;
            s_vs_warm++;
            s_vs_first_ms += bwg_qpc_ms() - t0;
            InterlockedExchange(&s_nvsh, n + 1);
            return v;
        }
    }
    {
        size_t csz = 0;
        const void *cb = bw_pscache_find_vs(v->src, v->len, bwg_ws_k > 1.0f, &csz);
        if (cb) {
            const char *what = "shader";
            HRESULT hr = S_OK;
            static unsigned hits;
            if (vsh_make(v, cb, csz, &what, &hr)) {
                vsh_manifest_note(v, v->src, 0);
                free(v->src);
                v->src = NULL;
                v->state = 2;
                if (hits++ < 3 || hits % 50 == 0)
                    fprintf(stderr, "[PSCACHE] vertex shader from the cache (%u this run)\n", hits);
                s_vs_cache++;
                s_vs_first_ms += bwg_qpc_ms() - t0;
                {
                    /* lane gfx-regen: the longest first sight from the cache, and
                     * any over 2 ms (when, for the gate's "worst stall") */
                    double ms = bwg_qpc_ms() - t0;
                    static unsigned slow;
                    if (ms > s_vs_cache_max)
                        s_vs_cache_max = ms;
                    if (ms > 2.0 && slow++ < 12)
                        fprintf(stderr, "[GPU-VSHPRE] a first sight from the cache took %.2f ms (frame %u, %s,"
                                        " the load-time batch %s)\n", ms, bwg_gen, v->ffp ? "fixed function" : "program",
                                bw_vshpre_busy() ? "still going" : "done");
                }
                InterlockedExchange(&s_nvsh, n + 1);
                return v;
            }
            fprintf(stderr, "[PSCACHE] the driver rejected a cached vertex shader's %s (error 0x%08lX):"
                            " compiling it again\n", what, (unsigned long)hr);
        }
    }
    if (sync) {
        /* a miss: compiled here and now, with the worker's compiler and
         * flags, so the batch draws on the GPU (no CPU fallback; D-028) */
        ID3DBlob *blob = NULL, *err = NULL;
        double ms;
        if (FAILED(D3DCompile(v->src, v->len, "bw_vsh", WS_DEFINES, NULL, "vs_main", "vs_5_0",
                              D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_IEEE_STRICTNESS,
                              0, &blob, &err))) {
            if (InterlockedIncrement(&s_vsh_failures) <= 3) {
                fprintf(stderr, "[GPU-VSH] shader %ld failed: %s\n", (long)n,
                        err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
                fprintf(stderr, "%s\n", v->src);
            }
            if (err) ID3D10Blob_Release(err);
            free(v->src);
            v->src = NULL;
            v->state = -1;
            s_vs_nowfail++;
            InterlockedExchange(&s_nvsh, n + 1);
            return NULL;
        }
        if (err) ID3D10Blob_Release(err);
        v->blob = blob;
        ms = bwg_qpc_ms() - t0;
        s_vsh_compile_ms += ms;
        InterlockedIncrement(&s_vsh_compiles);
        vsh_manifest_note(v, v->src, 0);
        vsh_finish(v);                  /* made on the device, kept in the cache */
        s_vs_now++;
        s_vs_now_ms += ms;
        if (ms > s_vs_now_max)
            s_vs_now_max = ms;
        if (!bw_vshpre_busy()) {        /* lane gfx-regen: after the load-time batch */
            s_vs_now_late++;
            if (ms > s_vs_late_max)
                s_vs_late_max = ms;
        }
        if (s_vs_now <= 5 || s_vs_now % 20 == 0)
            fprintf(stderr, "[GPU-NATIVE] a %s's vertex shader was not in the cache: compiled now in %.1f ms"
                            " (%u this run, %.0f ms)\n", v->ffp ? "fixed-function state" : "program", ms,
                    s_vs_now, s_vs_now_ms);
        InterlockedExchange(&s_nvsh, n + 1);
        return v->state == 2 ? v : NULL;
    }
    s_vs_queued++;
    if (!bw_vshpre_busy())
        s_vs_queued_late++;             /* lane gfx-regen */
    InterlockedExchange(&s_nvsh, n + 1);
    SetEvent(s_vsh_wake);
    return NULL;
}

/* The program's entry, or NULL while its shader compiles (or if it failed).
 * The last one found is checked first: consecutive batches mostly run the
 * same program. */
/* P1: the conversions a raw batch's shader does (the rest read floats). */
static void vsh_conv_key(const BwXVsBatch *b, uint8_t conv[16])
{
    uint32_t k;
    memset(conv, 0, 16);
    if (!b->nstreams)
        return;
    for (k = 0; k < b->ninputs && k < 16; k++) {
        uint8_t c = b->in[k].conv;
        switch (c >> 4) {
        case BW_XVS_COLOR: case BW_XVS_UBYTE: case BW_XVS_SHORT: case BW_XVS_CMP:
            conv[k] = c;
            break;
        default:
            break;
        }
    }
}

static BwVsh *get_vsh(const BwXVsBatch *b)
{
    static char src[96 * 1024];
    static LONG last = -1;
    uint8_t conv[16];
    uint64_t h;
    LONG i, n = s_nvsh;
    BwVsh *v;

    vsh_conv_key(b, conv);
    if (last >= 0 && bwg_memo_on()) {
        /* item 43: the last program first, compared word for word without
         * hashing the whole program (up to 136 instructions) every draw */
        v = &s_vsh[last];
        if (v->n == b->ninstr && v->points == (b->points != 0) && !memcmp(v->conv, conv, sizeof conv)
                && !memcmp(v->words, b->program, b->ninstr * sizeof v->words[0])) {
            if (v->state == 1)
                vsh_finish(v);
            return v->state == 2 ? v : NULL;
        }
    }
    h = prog_hash(b->program, b->ninstr);
    for (i = -1; i < n; i++) {
        LONG j = i < 0 ? last : i;
        if (j < 0 || (i >= 0 && i == last))
            continue;
        v = &s_vsh[j];
        if (v->hash != h || v->n != b->ninstr || v->points != (b->points != 0)
            || memcmp(v->conv, conv, sizeof conv)
            || memcmp(v->words, b->program, b->ninstr * sizeof v->words[0]))
            continue;
        last = j;
        if (v->state == 1)
            vsh_finish(v);
        return v->state == 2 ? v : NULL;
    }
    if (n == BW_MAX_VSH || b->ninstr > BW_VSH_MAX_INSTR)
        return NULL;
    if (!s_vsh && !(s_vsh = (BwVsh *)calloc(BW_MAX_VSH, sizeof *s_vsh)))
        return NULL;
    if (!vsh_worker_start())
        return NULL;
    v = &s_vsh[n];
    memset(v, 0, sizeof *v);
    v->hash = h;
    v->n = b->ninstr;
    v->mask = b->input_mask;
    v->nin = b->ninputs;
    memcpy(v->conv, conv, sizeof conv);
    v->points = (uint8_t)(b->points != 0);
    memcpy(v->words, b->program, b->ninstr * sizeof v->words[0]);
    v->len = bw_vsh_hlsl_ex(b->program, b->ninstr, b->input_mask, conv, v->points, src, sizeof src);
    if (v->len && getenv("BW_GPU_VSH_DUMP") && n < 8)
        fprintf(stderr, "[GPU-VSH] shader %ld (%u instructions, inputs %04X):\n%s\n",
                (long)n, b->ninstr, b->input_mask, src);
    if (vsh_adopt(v, n, src)) {
        last = n;
        return v;
    }
    return NULL;
}

/* 0.2.5 item 43: the geometry shader this file last bound (NULL: none). It
 * was bound before and unbound after every vertex-program draw; now a
 * vertex-program draw leaves it bound, and anything else that draws unbinds
 * it first (gs_unbind): the combiner and screen-space draws, the flip, the
 * stream-output check. In Remastered the visual suite's passes (texture
 * restoration, the scene copy, AO, surface seeds) can draw in the middle of
 * a vertex-program draw's set-up, so there every vertex-program draw still
 * starts unbound, as before. BW_GPU_MEMO=0: bound and unbound every draw. */
static ID3D11GeometryShader *s_gs_bound;

static void gs_set(ID3D11GeometryShader *g)
{
    if (g == s_gs_bound && bwg_memo_on())
        return;
    ID3D11DeviceContext_GSSetShader(s_ctx, g, NULL, 0);
    s_gs_bound = g;
}

static void gs_unbind(void)
{
    if (s_gs_bound || !bwg_memo_on())
        gs_set(NULL);
}

/* The finite-vertex filter (bw_vsh_gs_hlsl), compiled once. BW_GPU_VSH_GS=0
 * leaves it out. */
static ID3D11GeometryShader *vsh_filter_gs(void)
{
    static ID3D11GeometryShader *gs;
    static int tried;
    ID3DBlob *blob = NULL, *err = NULL;
    const char *e;

    if (tried)
        return gs;
    tried = 1;
    e = getenv("BW_GPU_VSH_GS");
    if (e && e[0] == '0')
        return NULL;
    if (FAILED(D3DCompile(bw_vsh_gs_hlsl, strlen(bw_vsh_gs_hlsl), "bw_vsh_gs", NULL, NULL,
                          "gs_main", "gs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_IEEE_STRICTNESS,
                          0, &blob, &err))) {
        fprintf(stderr, "[GPU-VSH] filter shader failed: %s\n",
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
        if (err) ID3D10Blob_Release(err);
        return NULL;
    }
    if (err) ID3D10Blob_Release(err);
    if (FAILED(ID3D11Device_CreateGeometryShader(s_dev, ID3D10Blob_GetBufferPointer(blob),
                                                ID3D10Blob_GetBufferSize(blob), NULL, &gs)))
        gs = NULL;
    ID3D10Blob_Release(blob);
    return gs;
}

/* gfx-sprites (I6): the sprite geometry shader (bw_vsh_sprite_gs_hlsl),
 * compiled once, with the vertex shaders' widescreen define; and its b1,
 * the core's BwgSpriteConsts. NULL if it can't be made: point batches then
 * stay on the CPU path. */
static ID3D11GeometryShader *sprite_gs(void)
{
    static ID3D11GeometryShader *gs;
    static int tried;
    ID3DBlob *blob = NULL, *err = NULL;
    if (tried)
        return gs;
    tried = 1;
    if (FAILED(D3DCompile(bw_vsh_sprite_gs_hlsl, strlen(bw_vsh_sprite_gs_hlsl), "bw_sprite_gs", WS_DEFINES,
                          NULL, "gs_main", "gs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_IEEE_STRICTNESS, 0, &blob, &err))) {
        fprintf(stderr, "[GPU-SPRITES] the sprite geometry shader failed: %s\n",
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
        if (err) ID3D10Blob_Release(err);
        return NULL;
    }
    if (err) ID3D10Blob_Release(err);
    if (FAILED(ID3D11Device_CreateGeometryShader(s_dev, ID3D10Blob_GetBufferPointer(blob),
                                                ID3D10Blob_GetBufferSize(blob), NULL, &gs))) {
        fprintf(stderr, "[GPU-SPRITES] the device refused the sprite geometry shader: points stay on the CPU%c", 10);
        gs = NULL;
    }
    ID3D10Blob_Release(blob);
    return gs;
}

static ID3D11Buffer *s_scb;             /* the sprite GS's b1 */
static BwgSpriteConsts s_scb_shadow;
static int s_scb_valid;

static int sprite_consts(const BwXVsBatch *b)
{
    BwgSpriteConsts c;
    D3D11_MAPPED_SUBRESOURCE m;
    bwg_sprite_consts(b, &c);
    if (!s_scb) {
        D3D11_BUFFER_DESC bd;
        memset(&bd, 0, sizeof(bd));
        bd.ByteWidth = (sizeof(BwgSpriteConsts) + 15) & ~15u;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_scb))) {
            s_scb = NULL;
            return 0;
        }
    }
    if (s_scb_valid && !memcmp(&c, &s_scb_shadow, sizeof c))
        return 1;
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_scb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        return 0;
    memcpy(m.pData, &c, sizeof c);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_scb, 0);
    s_scb_shadow = c;
    s_scb_valid = 1;
    return 1;
}

static int gpu_vsh_on(void)
{
    if (s_gpu_vsh < 0) {
        const char *e = getenv("BW_GPU_VSH");
        s_gpu_vsh = !(e && e[0] == '0');
    }
    return s_gpu_vsh;
}

/* ---- P1: GPU-side vertex fetch (bw_backend_ext.h, BW_GPU_VFETCH) -------- */

/* The integer formats' values, computed with fetch_attr's own expressions
 * (nv2a_pb_exec.c), so a converted input is bit for bit what the gather
 * produced: x/1023 at x + 1024 (x = -1024..1023, the packed normal's 11-bit
 * fields), z/511 at z + 2560 (its 10-bit field), c/255 at c + 3072 (bytes). */
static ID3D11ShaderResourceView *vfetch_table(void)
{
    static ID3D11ShaderResourceView *srv;
    static int tried;
    static float t[3328];
    D3D11_BUFFER_DESC bd;
    D3D11_SUBRESOURCE_DATA sd;
    D3D11_SHADER_RESOURCE_VIEW_DESC vd;
    ID3D11Buffer *buf = NULL;
    int i;

    if (tried)
        return srv;
    tried = 1;
    for (i = -1024; i < 1024; i++)
        t[i + 1024] = (float)i / 1023.0f;
    for (i = -512; i < 512; i++)
        t[i + 2560] = (float)i / 511.0f;
    for (i = 0; i < 256; i++)
        t[i + 3072] = (float)i / 255.0f;
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = sizeof t;
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    memset(&sd, 0, sizeof sd);
    sd.pSysMem = t;
    if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, &sd, &buf)))
        return NULL;
    memset(&vd, 0, sizeof vd);
    vd.Format = DXGI_FORMAT_R32_FLOAT;
    vd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    vd.Buffer.FirstElement = 0;
    vd.Buffer.NumElements = 3328;
    if (FAILED(ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)buf, &vd, &srv)))
        srv = NULL;
    ID3D11Buffer_Release(buf);
    return srv;
}


/* The input layout for this batch's streams: one element per input, in its
 * stream's slot at its offset, in its own format. Kept per shader. */
static ID3D11InputLayout *vfetch_layout(BwVsh *v, const BwXVsBatch *b)
{
    D3D11_INPUT_ELEMENT_DESC el[16];
    BwgVElem ge[16];
    uint64_t key = 1469598103934665603ull;
    uint32_t k, i;
    BwRawIl *e;

    if (!v->code || !v->nin || v->nin > 16)
        return NULL;
    memset(el, 0, sizeof el);
    if (!bwg_vfetch_elements(b, v->nin, ge, &key))      /* the core's formats and key (P1) */
        return NULL;
    for (k = 0; k < v->nin; k++) {
        el[k].SemanticName = "ATTR";
        el[k].SemanticIndex = k;
        el[k].Format = (DXGI_FORMAT)ge[k].format;
        el[k].InputSlot = ge[k].slot;
        el[k].AlignedByteOffset = ge[k].offset;
        el[k].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
    }
    for (i = 0; i < BW_VSH_RAW_IL; i++) {
        e = &v->raw[i];
        if ((e->il || e->failed) && e->key == key && !memcmp(e->el, el, v->nin * sizeof el[0]))
            return e->il;               /* item 43: NULL for a refused one (the CPU path draws it) */
    }
    e = &v->raw[v->raw_next++ % BW_VSH_RAW_IL];
    if (e->il)
        ID3D11InputLayout_Release(e->il);
    e->il = NULL;
    e->failed = 0;
    e->key = key;
    memcpy(e->el, el, sizeof el);
    if (FAILED(ID3D11Device_CreateInputLayout(s_dev, el, v->nin, v->code, v->code_size, &e->il))) {
        static int shown;
        if (shown++ < 3)
            fprintf(stderr, "[GPU-VSH] raw input layout failed (%u inputs): those batches are drawn"
                            " on the CPU path\n", v->nin);
        e->il = NULL;
        e->failed = 1;                  /* item 43: remembered, not tried again every draw */
        return NULL;
    }
    return e->il;
}

/* BW_GPU_VFETCH's half here: raw streams are fine unless the widescreen 2D
 * test is about to read this batch's positions on the CPU. */
static int bw_vsh_raw_ok(void)
{
    return gpu_vsh_on() && !(bwg_ws_k > 1.0f && bwg_ws_2d > 0);
}

/* The last raw draw's binding, for vsh_readback. */
static int s_vlast_raw;
static UINT s_vlast_ns, s_vlast_strides[BW_XVS_MAX_STREAMS], s_vlast_offs[BW_XVS_MAX_STREAMS];
static ID3D11InputLayout *s_vlast_il;

/* OCEAN_REFLECT_SINGLE (Remastered; agent P2a, docs/day1/P2a.md sections
 * 7-8): the distant-water line. The sea's passes draw with depth writes off
 * and alpha blending, so where a wave folds over itself at a grazing angle
 * two front-facing layers cover the same pixels and the sky-reflection pass
 * (stage 3 DOT_RFLCT_SPEC on a cube) is blended twice. Here each draw of the
 * lit pass (the run of tile draws right before the reflection run) is drawn
 * a second time, depth only, into a private copy of the scene's depth taken
 * at the run's first draw (LESS_EQUAL, writing, a hair of extra depth bias:
 * ors_biased). The reflection draws are then drawn as the game draws them
 * but with a LESS_EQUAL, no-write test against that copy, so only the
 * nearest water layer of each pixel gets the reflection, once. The scene's
 * depth view is bound again right after each such draw and the scene's
 * depth is never written: nothing drawn after the sea sees a difference.
 * BW_OCEAN_REFLECT_SINGLE=0 turns it off; BW_GPU_AB=oceanreflect turns it
 * off on odd frames (same-run A/B). */
static ID3D11Texture2D *s_ors_tex;
static ID3D11DepthStencilView *s_ors_dsv;
static ID3D11BlendState *s_ors_bs;
static ID3D11DepthStencilState *s_ors_prime, *s_ors_eq;
static BwDepth *s_ors_src;          /* the depth copied for this run of reflection draws; NULL: stale */
static int s_ors_inlit, s_ors_primed;
/* 0.2.5 item 49: this run of the sea's draws is drawn as shipped (its copy
 * or its prime couldn't be made); until the next draw that isn't the sea's.
 * Before, a give-up reset the run, so every later tile copied the whole
 * depth buffer again. */
static int s_ors_gaveup;
static unsigned long s_ors_draws, s_ors_copies, s_ors_primes, s_ors_giveups;
static struct { ID3D11RasterizerState *from, *to; } s_ors_rs[8];
static int s_ors_nrs;

/* BW_GPU_STATS: the sea's single-reflection work in the last second (item 49:
 * copies per flip should equal the views drawn with sea). */
static void ors_stats_line(void)
{
    static unsigned long d0, c0, p0, g0;
    if (s_ors_draws == d0 && s_ors_copies == c0 && s_ors_primes == p0 && s_ors_giveups == g0)
        return;
    fprintf(stderr, "[GPU-ORS] ocean reflect single: %lu depth copies, %lu tiles primed, %lu reflection"
                    " draws tested, %lu runs drawn as shipped (in %u flips)\n",
            s_ors_copies - c0, s_ors_primes - p0, s_ors_draws - d0, s_ors_giveups - g0, s_st_flips);
    d0 = s_ors_draws; c0 = s_ors_copies; p0 = s_ors_primes; g0 = s_ors_giveups;
}

/* The prime is drawn with the lit pass's own rasteriser state (the game's
 * depth bias included) plus a little more bias (64 steps of the 24-bit depth
 * and 2x the slope), so it lies a hair behind the surface. The reflection
 * pass, whose vertex program computes the same position with its own
 * instructions and gets the game's same bias, then passes the LESS_EQUAL
 * test at the nearest layer whatever its last bits. Layers of a folded wave
 * are metres apart in depth, far more than that. */
static ID3D11RasterizerState *ors_biased(ID3D11RasterizerState *from)
{
    D3D11_RASTERIZER_DESC rd;
    int i;
    static int giveup_test = -1;
    if (giveup_test < 0) {
        /* item 49 test aid: BW_OCEAN_REFLECT_GIVEUP=1 acts as if the table
         * below were full, so every run of the sea gives up */
        const char *e = getenv("BW_OCEAN_REFLECT_GIVEUP");
        giveup_test = e && e[0] == '1';
    }
    if (!from || giveup_test)
        return NULL;
    for (i = 0; i < s_ors_nrs; i++)
        if (s_ors_rs[i].from == from)
            return s_ors_rs[i].to;
    if (s_ors_nrs == 8)
        return NULL;
    ID3D11RasterizerState_GetDesc(from, &rd);
    rd.DepthBias += 64;
    rd.SlopeScaledDepthBias += 2.0f;
    s_ors_rs[s_ors_nrs].to = NULL;
    if (FAILED(ID3D11Device_CreateRasterizerState(s_dev, &rd, &s_ors_rs[s_ors_nrs].to)))
        return NULL;
    ID3D11RasterizerState_AddRef(from);     /* keep the key alive */
    s_ors_rs[s_ors_nrs].from = from;
    return s_ors_rs[s_ors_nrs++].to;
}

static int ors_enabled(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("BW_OCEAN_REFLECT_SINGLE");
        v = bw_fix_enabled(BW_FIX_OCEAN_REFLECT_SINGLE) && !(e && e[0] == '0');
    }
    return v && !bw_ab_off("oceanreflect");
}

static int ors_prepare(BwDepth *d)
{
    D3D11_TEXTURE2D_DESC td, have;
    if (!s_ors_bs) {
        D3D11_BLEND_DESC bd;
        D3D11_DEPTH_STENCIL_DESC dd;
        memset(&bd, 0, sizeof(bd));
        bd.RenderTarget[0].RenderTargetWriteMask = 0;          /* depth only */
        ID3D11Device_CreateBlendState(s_dev, &bd, &s_ors_bs);
        memset(&dd, 0, sizeof(dd));
        dd.DepthEnable = TRUE;
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        ID3D11Device_CreateDepthStencilState(s_dev, &dd, &s_ors_prime);
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;     /* the game's own test, against the primed copy */
        ID3D11Device_CreateDepthStencilState(s_dev, &dd, &s_ors_eq);
        if (!s_ors_bs || !s_ors_prime || !s_ors_eq)
            return 0;
    }
    ID3D11Texture2D_GetDesc(d->tex, &td);
    if (s_ors_tex) {
        ID3D11Texture2D_GetDesc(s_ors_tex, &have);
        if (memcmp(&have, &td, sizeof(td))) {
            ID3D11DepthStencilView_Release(s_ors_dsv);
            ID3D11Texture2D_Release(s_ors_tex);
            s_ors_tex = NULL;
            s_ors_dsv = NULL;
            s_ors_src = NULL;
        }
    }
    if (!s_ors_tex) {
        D3D11_DEPTH_STENCIL_VIEW_DESC vd;
        ID3D11DepthStencilView_GetDesc(d->dsv, &vd);
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_ors_tex)))
            return 0;
        if (FAILED(ID3D11Device_CreateDepthStencilView(s_dev, (ID3D11Resource *)s_ors_tex, &vd, &s_ors_dsv))) {
            ID3D11Texture2D_Release(s_ors_tex);
            s_ors_tex = NULL;
            return 0;
        }
    }
    if (s_ors_src != d) {
        ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)s_ors_tex, (ID3D11Resource *)d->tex);
        s_ors_src = d;
        s_ors_copies++;
    }
    return 1;
}

static int bw_xdraw_vsh(const Nv2aSurface *s, const BwXVsBatch *b)
{
    BwVsh *v;
    ID3D11Buffer *cbs[2];
    UINT voff = 0, ioff, stride;
    ID3D11InputLayout *il;
    int r, ors;

    if (!gpu_vsh_on() || !b || !b->nindices || !ensure_device())
        return 0;
    if (b->points && (!bwg_sprites_on() || !sprite_gs()))
        return 0;               /* gfx-sprites: points stay on the CPU path */
    if (bw_vfx_active())
        gs_unbind();            /* item 43: the visual suite's passes below draw without one */
    if (!(v = get_vsh(b))) {
        s_st_vfall++;
        if (b->points)
            s_st_spr_fall++;
        return 0;
    }
    /* VFX (D9): an overlay (HUD) draw: D3D's pass-through program, no depth test */
    {
        int skip;                       /* BW_VFX_HUDSKIP (V1 test aid) */
        s_vfx_overlay = bwg_vsh_overlay(b, &skip);
        if (skip)
            return 1;
    }
    bwg_ws_vsbatch(s, b);               /* widescreen (D5): the 2D test */
    if (bw_vfx_active() && b->xb.state && begin(s, b->xb.state)) {
        /* the scene before the HUD (and AO), per view; before this draw's
         * own state is set, since the AO passes change the pipeline */
        if (s_vfx_overlay)
            vfx_overlay_draw();
        else {
            if (vfx_sea_starts(&b->xb) && s_vfx_pend_on && s_vfx_pend_surf == bwg_cur)
                vfx_ao_now(&s_vfx_pend);        /* the water starts: AO on what's under it */
            vfx_scene_draw();
        }
    }
    if (trc_on()) {                                         /* T1 test aid */
        double t0 = bwg_qpc_ms();
        r = xdraw_prepare(s, &b->xb);
        s_trc_prep += bwg_qpc_ms() - t0;
    } else
    r = xdraw_prepare(s, &b->xb);
    s_vfx_log_ov = s_vfx_overlay;           /* item 50: for the drawlog */
    s_vfx_overlay = 0;
    if (r < 0)
        return 1;               /* culled: nothing to draw, and nothing to fall back to */
    if (r == 0)
        return 0;
    if (!s_kcb) {
        D3D11_BUFFER_DESC bd;
        memset(&bd, 0, sizeof(bd));
        bd.ByteWidth = (sizeof(BwVshConsts) + 15) & ~15u;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_kcb))) {
            s_kcb = NULL;
            return 0;
        }
    }
    {
        /* The constants change between most batches (each object has its
         * own matrix), but not all: compare, and upload only on a change. */
        static BwVshConsts k;
        /* P1: the executor's constant-file generation; unchanged since the
         * shadow was last synced (and the same small fields) means the
         * shadow, and so the buffer, already holds these: skip the 3 KB copy
         * and compare. BW_GPU_KGEN=0 always compares. */
        static uint32_t kgen;
        static int kgen_ok, kgen_on = -1;
        float vsinfo[4], foginfo[4];
        bwg_vsh_info(b, vsinfo, foginfo);
        if (kgen_on < 0) {
            const char *e = getenv("BW_GPU_KGEN");
            kgen_on = !(e && e[0] == '0');
        }
        if (kgen_on && s_kvalid && kgen_ok && b->consts_gen == kgen
                && !memcmp(vsinfo, s_kshadow.vsinfo, sizeof vsinfo)
                && !memcmp(foginfo, s_kshadow.foginfo, sizeof foginfo)) {
            s_st_kskip++;
            goto bw_kdone;
        }
        memcpy(k.k, b->consts, sizeof k.k);
        memcpy(k.vsinfo, vsinfo, sizeof vsinfo);
        memcpy(k.foginfo, foginfo, sizeof foginfo);
        kgen_ok = 0;
        if (!s_kvalid || memcmp(&k, &s_kshadow, sizeof k)) {
            D3D11_MAPPED_SUBRESOURCE m;
            if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_kcb, 0,
                                               D3D11_MAP_WRITE_DISCARD, 0, &m)))
                return 0;
            memcpy(m.pData, &k, sizeof k);
            ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_kcb, 0);
            s_kshadow = k;
            s_kvalid = 1;
            s_st_kup++;
        }
        kgen = b->consts_gen;               /* the shadow now matches this generation */
        kgen_ok = 1;
    }
bw_kdone:
    stride = 16u * v->nin;
    il = v->il;
    if (b->nstreams && v->nin) {
        /* P1: the streams' spans, packed into one map of the ring (a second
         * map could discard the first), each in its own slot */
        ID3D11Buffer *bufs[BW_XVS_MAX_STREAMS];
        UINT pos[BW_XVS_MAX_STREAMS], total = 0, base = 0, k;
        uint8_t *dst;
        static int conv_bound;
        if (b->nstreams > BW_XVS_MAX_STREAMS || !(il = vfetch_layout(v, b)))
            return 0;
        for (k = 0; k < b->nstreams; k++) {
            total = (total + 15u) & ~15u;
            pos[k] = total;
            total += b->streams[k].bytes;
        }
        if (!(dst = ring_begin(&s_vring, total, 16, &base)))
            return 0;
        for (k = 0; k < b->nstreams; k++) {
            memcpy(dst + pos[k], b->streams[k].data, b->streams[k].bytes);
            bufs[k] = s_vring.buf;
            s_vlast_strides[k] = b->streams[k].stride;
            s_vlast_offs[k] = base + pos[k];
        }
        ring_end(&s_vring, base, total);
        if (!conv_bound) {
            /* the table, once: nothing else binds a vertex-shader resource */
            ID3D11ShaderResourceView *t = vfetch_table();
            if (!t)
                return 0;
            sh_vs_srv(0, 1, &t);        /* I3 */
            conv_bound = 1;
        }
        sh_vb(0, b->nstreams, bufs, s_vlast_strides, s_vlast_offs);     /* I3 */
        s_vlast_ns = b->nstreams;
        s_vlast_il = il;
        s_vlast_raw = 1;
        s_st_vraw++;
    } else {
        if (v->nin) {
            voff = ring_push(&s_vring, b->inputs, b->nverts * stride, stride);
            if (voff == ~0u)
                return 0;
        }
        s_vlast_raw = 0;
    }
    ioff = ring_push(&s_iring, b->indices, b->nindices * 4u, 4);
    if (ioff == ~0u)
        return 0;
    cbs[0] = s_xcb;
    cbs[1] = s_kcb;

    if (b->points && !sprite_consts(b))
        return 0;               /* gfx-sprites: no constants for the GS: the CPU path draws it */
    sh_il(il);                          /* I3: each sent only if it changed */
    sh_topo(b->points ? D3D11_PRIMITIVE_TOPOLOGY_POINTLIST : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    if (v->nin && !s_vlast_raw)
        sh_vb(0, 1, &s_vring.buf, &stride, &voff);
    sh_ib(s_iring.buf, DXGI_FORMAT_R32_UINT, 0);
    sh_vs(v->vs);
    sh_vs_cb(0, 2, cbs);
    if (b->points) {
        /* gfx-sprites (I6): each point's quad by bw_draw_points's rules; b0
         * the combiner constants (surf, wsx), b1 the sprite's. The geometry
         * stage's constant buffers are not in I3's shadow (only this branch
         * and the sprite check set them). */
        ID3D11Buffer *gcbs[2];
        gcbs[0] = s_xcb;
        gcbs[1] = s_scb;
        gs_set(sprite_gs());
        ID3D11DeviceContext_GSSetConstantBuffers(s_ctx, 0, 2, gcbs);
    } else
    gs_set(vsh_filter_gs());    /* item 43: only when it isn't bound already */
    ors = 0;
    if (!b->points) {           /* gfx-sprites: points leave the sea's state alone, as on the CPU path */
        /* OCEAN_REFLECT_SINGLE (see ors_prepare). The sea's lit pass (stage
         * program 0x21081, seen: the run of draws right before the
         * reflection run, one per tile) primes a private copy of the scene's
         * depth with every tile; the reflection draws then test against it. */
        uint32_t prog = b->xb.rc ? b->xb.rc->stage_program : 0xFFFFFFFFu;
        int refl = ((prog >> 15) & 0x1F) == 0x0C, lit = prog == 0x21081u;
        if (!refl && !lit) {
            s_ors_inlit = 0;            /* any other draw: the copy is stale */
            s_ors_primed = 0;
            s_ors_gaveup = 0;           /* item 49: the next run of the sea may try again */
        } else if (bwg_cur && bwg_cur_depth && bwg_cur_depth->tex && bwg_cur_depth->dsv && ors_enabled()
                   && !s_ors_gaveup) {
            if (lit) {
                if (!s_ors_inlit) {     /* a new run: copy the scene's depth at its first draw */
                    s_ors_src = NULL;
                    s_ors_primed = 0;
                }
                if (ors_prepare(bwg_cur_depth)) {
                    s_ors_inlit = 1;    /* item 49: only once the copy exists */
                    ors = 2;
                } else
                    s_ors_gaveup = 1;   /* item 49: this run as shipped, without copying again */
            } else if (s_ors_inlit || s_ors_primed) {
                ID3D11RenderTargetView *rtv = bwg_cur->mrtv ? bwg_cur->mrtv : bwg_cur->rtv;
                static int shown;
                if (!shown++ && b->xb.state)
                    fprintf(stderr, "[BW] ocean reflect single on: the game's depth state for the reflection"
                            " pass: test %u func 0x%X write %u stencil %u%c",
                            (unsigned)b->xb.state->depth_test_enable, (unsigned)b->xb.state->depth_func,
                            (unsigned)b->xb.state->depth_write, (unsigned)b->xb.state->stencil_enable, 10);
                s_ors_inlit = 0;
                s_ors_primed = 1;
                ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &rtv, s_ors_dsv);
                ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, s_ors_eq, 0);
                g_bw_ctx_epoch++;       /* I3: not the draw path's own state */
                ors = 1;
                s_ors_draws++;
            }
        }
    }
    if (trc_on()) {                                         /* T1 test aid */
        double t0 = bwg_qpc_ms();
        ID3D11DeviceContext_DrawIndexed(s_ctx, b->nindices, ioff / 4u, 0);
        s_trc_draw += bwg_qpc_ms() - t0;
        s_trc_ndraw++;
    } else
    ID3D11DeviceContext_DrawIndexed(s_ctx, b->nindices, ioff / 4u, 0);
    if (b->points) {            /* gfx-sprites: the other draws expect triangles */
        sh_topo(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);     /* I3: through the shadow, so it stays true */
        s_st_spr_draws++;
        s_st_spr_pts += b->nindices;
    }
    if (bw_edgeprobe_on())              /* BW_GPU_EDGEPROBE (test aid) */
        bw_edgeprobe_at(BW_EP_DRAW, s_ctx, (bwg_cur->mtex ? (void *)bwg_cur->mtex : (void *)bwg_cur->tex), bwg_gen);
    if (ors)
        g_bw_ctx_epoch++;               /* I3: OCEAN_REFLECT_SINGLE sets state of its own */
    if (ors == 2) {                 /* OCEAN_REFLECT_SINGLE: the lit tile again, depth only, into the copy */
        ID3D11RenderTargetView *rtv = bwg_cur->mrtv ? bwg_cur->mrtv : bwg_cur->rtv;
        ID3D11BlendState *obs = NULL;
        FLOAT obf[4] = { 0, 0, 0, 0 };
        UINT omask = 0xFFFFFFFFu, oref = 0;
        ID3D11DepthStencilState *ods = NULL;
        ID3D11PixelShader *ops = NULL;
        ID3D11RasterizerState *orst = NULL, *brst;
        ID3D11DeviceContext_RSGetState(s_ctx, &orst);
        brst = ors_biased(orst);
        ID3D11DeviceContext_OMGetBlendState(s_ctx, &obs, obf, &omask);
        ID3D11DeviceContext_OMGetDepthStencilState(s_ctx, &ods, &oref);
        ID3D11DeviceContext_PSGetShader(s_ctx, &ops, NULL, NULL);
        if (brst) {
            ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &rtv, s_ors_dsv);
            ID3D11DeviceContext_OMSetBlendState(s_ctx, s_ors_bs, NULL, 0xFFFFFFFFu);
            ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, s_ors_prime, 0);
            ID3D11DeviceContext_RSSetState(s_ctx, brst);
            ID3D11DeviceContext_PSSetShader(s_ctx, NULL, NULL, 0);
            ID3D11DeviceContext_DrawIndexed(s_ctx, b->nindices, ioff / 4u, 0);
            ID3D11DeviceContext_RSSetState(s_ctx, orst);
            ID3D11DeviceContext_PSSetShader(s_ctx, ops, NULL, 0);
            ID3D11DeviceContext_OMSetBlendState(s_ctx, obs, obf, omask);
            ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, ods, oref);
            s_ors_primes++;
        } else {
            /* no state to prime with: this run's reflection draws as shipped.
             * Item 49: the run is marked, not reset (a reset made every later
             * tile a "new run" that copied the whole depth buffer again). */
            static int told;
            s_ors_gaveup = 1;
            s_ors_inlit = 0;
            s_ors_primed = 0;
            s_ors_giveups++;
            if (!told++)
                fprintf(stderr, "[BW] ocean reflect single: a run of the sea is drawn as shipped (no"
                                " biased rasteriser state for its prime); counted in BW_GPU_STATS%c", 10);
        }
        if (orst) ID3D11RasterizerState_Release(orst);
        if (ops) ID3D11PixelShader_Release(ops);
        if (obs) ID3D11BlendState_Release(obs);
        if (ods) ID3D11DepthStencilState_Release(ods);
    }
    if (ors) {                      /* OCEAN_REFLECT_SINGLE: the scene's own depth view again */
        ID3D11RenderTargetView *rtv = bwg_cur->mrtv ? bwg_cur->mrtv : bwg_cur->rtv;
        ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &rtv, bwg_cur_depth->dsv);
    }
    if (g_bw_gfxcls) gfxcls_drawn(&b->xb, b->ninstr <= 4 && b->xb.state && !b->xb.state->depth_test_enable, NULL, b->xb.state, b);   /* BW_GFXCAP_CLASSES */
    if (bw_vfx_active()) {
        vfx_drawlog('v', b->nindices, &b->xb, NULL, b->ninstr);     /* VFX (D9) test aid */
        vfx_drawlog_inputs(b);                                      /* V1: the HUD quads' inputs */
    }
    ft_draw('v', b->nindices, (float)b->ninstr, (float)b->nverts, 0.0f, 0.0f, b->input_mask,
            b->xb.tex_valid[0] ? b->xb.tex[0].offset : 0, b->xb.state);
    if (!bwg_memo_on())
        gs_set(NULL);           /* BW_GPU_MEMO=0: unbound after every draw, as before */
    s_vlast = v;
    s_vlast_voff = voff;
    s_vlast_stride = stride;
    s_vlast_nverts = b->nverts;
    s_vlast_ioff = ioff;                /* gfx-sprites: for sprite_readback */
    s_vlast_nidx = b->nindices;
    s_vlast_points = b->points != 0;
    SURF_DRAWN(bwg_cur);
    s_st_draws++;
    s_st_vdraws++;
    s_st_tris += b->points ? 2u * b->nindices : b->nindices / 3;
    return 1;
}


/* ---- gfx-ffp: the fixed function on the GPU (GPU_FFP, BW_GPU_FFP) --------
 *
 * A fixed-function batch (the executor's gpu_ffp_batch: the sky dome, the
 * beach props) arrives untransformed, like a vertex-program batch, with its
 * state in b->ffp. Its shader is generated from that state (bw_ffp_hlsl), kept
 * in the same table, cache, worker and manifest as the programs' (s_vsh,
 * vsh_adopt), and drawn here: the CPU path's glue (bw_xdraw's, in its order:
 * these batches were drawn there, never as overlays, never by the sea's
 * single-reflection test) with the program path's vertex binding (the raw
 * streams or the gathered floats, the finite-vertex filter). Its constants
 * are its own: cbuffer V (b1) in a buffer of its own, so the programs' upload
 * shortcuts never see it, and BwFfpConsts (b2). Declined (back to the CPU
 * path, counted): while its shader compiles (without BW_GPU_VSH_SYNC), and
 * where the widescreen 2D test reads the CPU's positions
 * (bwg_ws_needs_positions: a 2D section, the 4:3-cutscene option). */
_Static_assert(sizeof(((BwFfpKey *)0)->in) == BW_FFP_ROLES, "BwFfpKey.in holds every BW_FFP_* role");
_Static_assert(sizeof(BwFfpConsts) == 50 * 16, "BwFfpConsts is bw_ffp_hlsl's cbuffer F");

static ID3D11Buffer *s_kcb_ffp, *s_fcb;
static unsigned s_st_fdraws, s_st_ffall, s_st_fdecl;            /* in the last second */
static unsigned long long s_ffp_draws, s_ffp_fall, s_ffp_decl;  /* this run */

static void ffp_key(const BwXVsBatch *b, BwFfpKey *k)
{
    const BwXFfp *f = b->ffp;
    memset(k, 0, sizeof *k);
    memcpy(k->in, f->in, sizeof k->in);
    vsh_conv_key(b, k->conv);
    k->nin = b->ninputs;
    k->lit = f->lit ? 1 : 0;
    k->light_mask = f->lit ? f->light_mask & 0xFFFFu : 0;     /* lit_color reads 8 lights */
    k->colmat = f->lit ? f->colmat & 3 : 0;
    k->fog_on = f->fog_on ? 1 : 0;
    k->fog_gen = f->fog_on ? f->fog_gen : 0;
}

/* Lane gfx-regen: the fixed-function shapes the game draws with (GPU_FFP),
 * for the load-time compile. The fixed function's shader is generated from
 * the state's shape (bw_ffp_hlsl), not from anything the game creates, so no
 * wrapper sees it; these are the shapes the corpus and the smoke scenes meet
 * (logged by vshpre_note at a first sight), each with its raw inputs and
 * gathered (no conversions). Descriptions of state only: which input each
 * role reads, the conversions, lit or not, the lights' kinds, colour
 * material, fog. Empty: the shapes compile at their first sight, as before. */
static const BwFfpKey s_known_ffp[] = {
    /* roles in BW_FFP_* order (POS, NRM, COL, TC0, TC1-3, FOG): input k or -1;
     * conversions per input; inputs; light mask; lit, colour material, fog,
     * fog generation. Seen at first sights in Remastered mission 1 (run
     * 20261105-...-regen-probe-m01r), the same 4 shapes gfx-ffp counted over
     * the corpus. */
    { { 0, -1, -1, 1, -1, -1, -1, -1 }, { 0 }, 2, 0, 0, 0, 0, 0 },            /* unlit, position + 1 texture */
    { { 0, -1, -1, 1, -1, -1, -1, -1 }, { 0 }, 2, 0, 0, 0, 1, 2 },            /* the same, with fog */
    { { 0, -1, 1, 2, -1, -1, -1, -1 }, { 0, BW_XVS_COLOR << 4 | 4 }, 3, 0, 0, 0, 0, 0 },  /* unlit, a D3DCOLOR colour */
    { { 0, 1, -1, 2, -1, -1, -1, -1 }, { 0, BW_XVS_CMP << 4 | 1 }, 3, 1, 1, 0, 1, 2 },    /* lit by light 0, a packed normal, fog */
    { { 0 } }                           /* end */
};

static void vshpre_known_ffp(void)
{
    static char src[96 * 1024];
    unsigned i;
    if (!ffp_on() || !gpu_vsh_on())
        return;
    for (i = 0; s_known_ffp[i].nin; i++) {
        BwFfpKey k = s_known_ffp[i];
        size_t n = bw_ffp_hlsl(&k, src, sizeof src);
        bw_vshpre_add_text(src, n, 'f');
        memset(k.conv, 0, sizeof k.conv);   /* the gathered batch's shader */
        n = bw_ffp_hlsl(&k, src, sizeof src);
        bw_vshpre_add_text(src, n, 'f');
    }
}

/* The fixed-function state's entry, or NULL while its shader compiles (or if
 * it failed). The last one found is checked first. */
static BwVsh *get_ffp(const BwXVsBatch *b)
{
    static char src[96 * 1024];
    static LONG last = -1;
    static unsigned dumped;
    BwFfpKey k;
    const uint8_t *p = (const uint8_t *)&k;
    uint64_t h = 1469598103934665603ull;
    LONG i, n = s_nvsh;
    size_t j;
    BwVsh *v;

    ffp_key(b, &k);
    for (j = 0; j < sizeof k; j++)
        h = (h ^ p[j]) * 1099511628211ull;
    for (i = -1; i < n; i++) {
        LONG x = i < 0 ? last : i;
        if (x < 0 || (i >= 0 && i == last))
            continue;
        v = &s_vsh[x];
        if (!v->ffp || v->hash != h || memcmp(&v->fkey, &k, sizeof k))
            continue;
        last = x;
        if (v->state == 1)
            vsh_finish(v);
        return v->state == 2 ? v : NULL;
    }
    if (n == BW_MAX_VSH)
        return NULL;
    if (!s_vsh && !(s_vsh = (BwVsh *)calloc(BW_MAX_VSH, sizeof *s_vsh)))
        return NULL;
    if (!vsh_worker_start())
        return NULL;
    v = &s_vsh[n];
    memset(v, 0, sizeof *v);
    v->ffp = 1;
    v->fkey = k;
    v->hash = h;
    v->mask = b->input_mask;
    v->nin = b->ninputs;
    memcpy(v->conv, k.conv, sizeof v->conv);
    v->len = bw_ffp_hlsl(&k, src, sizeof src);
    /* BW_GPU_FFP_DUMP (test aid): the first shaders' text */
    if (v->len && getenv("BW_GPU_FFP_DUMP") && dumped++ < 8)
        fprintf(stderr, "[GPU-FFP] shader %ld (lit %u lights %04X colmat %u fog %u/%u, %u inputs):\n%s\n",
                (long)n, k.lit, k.light_mask, k.colmat, k.fog_on, k.fog_gen, k.nin, src);
    if (vsh_adopt(v, n, src)) {
        last = n;
        return v;
    }
    return NULL;
}

/* BwFfpConsts from the batch's state (bw_ffp_hlsl's FL layout: per light
 * ambient + range, diffuse, direction, position, attenuation, from its
 * method block: 0x00, 0x0C, 0x24, 0x34, 0x5C, 0x68). */
static void ffp_consts(const BwXFfp *f, BwFfpConsts *c)
{
    int i;
    memset(c, 0, sizeof *c);
    memcpy(c->m, f->composite, sizeof c->m);
    memcpy(c->mv, f->modelview, sizeof c->mv);
    memcpy(c->ambient, f->ambient, sizeof c->ambient);
    memcpy(c->imm_fog, f->imm_fog, sizeof c->imm_fog);
    for (i = 0; i < 8; i++) {
        const float *l = f->light[i];
        if (!f->lit || !((f->light_mask >> (2 * i)) & 3))
            continue;
        c->light[i][0][0] = l[0];  c->light[i][0][1] = l[1];  c->light[i][0][2] = l[2];  c->light[i][0][3] = l[9];
        c->light[i][1][0] = l[3];  c->light[i][1][1] = l[4];  c->light[i][1][2] = l[5];
        c->light[i][2][0] = l[13]; c->light[i][2][1] = l[14]; c->light[i][2][2] = l[15];
        c->light[i][3][0] = l[23]; c->light[i][3][1] = l[24]; c->light[i][3][2] = l[25];
        c->light[i][4][0] = l[26]; c->light[i][4][1] = l[27]; c->light[i][4][2] = l[28];
    }
}

static ID3D11Buffer *ffp_cbuf(ID3D11Buffer **pb, UINT size)
{
    if (!*pb) {
        D3D11_BUFFER_DESC bd;
        memset(&bd, 0, sizeof(bd));
        bd.ByteWidth = (size + 15) & ~15u;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, pb)))
            *pb = NULL;
    }
    return *pb;
}

/* Upload only on a change (a DISCARD map per draw renames the buffer). */
static int ffp_upload(ID3D11Buffer *buf, void *shadow, int *ok, const void *data, size_t size)
{
    D3D11_MAPPED_SUBRESOURCE m;
    if (*ok && !memcmp(shadow, data, size))
        return 1;
    *ok = 0;
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)buf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        return 0;
    memcpy(m.pData, data, size);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)buf, 0);
    memcpy(shadow, data, size);
    *ok = 1;
    return 1;
}

static int bw_xdraw_ffp(const Nv2aSurface *s, const BwXVsBatch *b)
{
    static BwVshConsts k, kshadow;
    static BwFfpConsts fc, fshadow;
    static int kok, fok, conv_bound;
    BwVsh *v;
    ID3D11Buffer *cbs[3];
    ID3D11InputLayout *il;
    UINT voff = 0, ioff, stride;
    int ov, r;

    if (!ffp_on() || !gpu_vsh_on() || !b || !b->ffp || !b->nindices || !ensure_device())
        return 0;
    if (bwg_ws_needs_positions()) {
        s_st_fdecl++;
        s_ffp_decl++;
        return 0;
    }
    if (!(v = get_ffp(b))) {
        s_st_ffall++;
        s_ffp_fall++;
        return 0;
    }
    /* everything that can still send the batch back, before the glue */
    il = v->il;
    if (b->nstreams && v->nin
        && (b->nstreams > BW_XVS_MAX_STREAMS || !(il = vfetch_layout(v, b)) || !vfetch_table())) {
        s_st_fdecl++;
        s_ffp_decl++;
        return 0;
    }
    if (!ffp_cbuf(&s_kcb_ffp, sizeof(BwVshConsts)) || !ffp_cbuf(&s_fcb, sizeof(BwFfpConsts)))
        return 0;

    /* bw_xdraw's glue, in its order (bw_xdraw_m's unbind first) */
    gs_unbind();
    ov = bw_vfx_active() && s_tr_fb_overlay;
    s_tr_skip = s_tr_fb_overlay;
    s_vfx_log_ov = s_tr_fb_overlay;
    s_tr_fb_overlay = 0;
    if (bw_vfx_active() && b->xb.state && begin(s, b->xb.state)) {
        if (ov)
            vfx_overlay_draw();
        else if (vfx_sea_starts(&b->xb) && s_vfx_pend_on && s_vfx_pend_surf == bwg_cur)
            vfx_ao_now(&s_vfx_pend);
    }
    s_vfx_overlay = ov;
    r = xdraw_prepare(s, &b->xb);
    s_tr_skip = 0;
    s_vfx_overlay = 0;
    if (r <= 0)
        return 1;                       /* as bw_xdraw: nothing to draw */

    {
        float vsinfo[4], foginfo[4];
        bwg_vsh_info(b, vsinfo, foginfo);
        memcpy(k.k, b->consts, sizeof k.k);
        memcpy(k.vsinfo, vsinfo, sizeof vsinfo);
        memcpy(k.foginfo, foginfo, sizeof foginfo);
        ffp_consts(b->ffp, &fc);
        if (!ffp_upload(s_kcb_ffp, &kshadow, &kok, &k, sizeof k)
            || !ffp_upload(s_fcb, &fshadow, &fok, &fc, sizeof fc))
            return 1;
    }
    stride = 16u * v->nin;
    if (b->nstreams && v->nin) {
        /* P1's raw streams, as bw_xdraw_vsh binds them */
        ID3D11Buffer *bufs[BW_XVS_MAX_STREAMS];
        UINT pos[BW_XVS_MAX_STREAMS], total = 0, base = 0, q;
        uint8_t *dst;
        for (q = 0; q < b->nstreams; q++) {
            total = (total + 15u) & ~15u;
            pos[q] = total;
            total += b->streams[q].bytes;
        }
        if (!(dst = ring_begin(&s_vring, total, 16, &base)))
            return 1;
        for (q = 0; q < b->nstreams; q++) {
            memcpy(dst + pos[q], b->streams[q].data, b->streams[q].bytes);
            bufs[q] = s_vring.buf;
            s_vlast_strides[q] = b->streams[q].stride;
            s_vlast_offs[q] = base + pos[q];
        }
        ring_end(&s_vring, base, total);
        if (!conv_bound) {
            ID3D11ShaderResourceView *t = vfetch_table();
            sh_vs_srv(0, 1, &t);        /* I3 */
            conv_bound = 1;
        }
        sh_vb(0, b->nstreams, bufs, s_vlast_strides, s_vlast_offs);     /* I3 */
        s_vlast_ns = b->nstreams;
        s_vlast_il = il;
        s_vlast_raw = 1;
    } else {
        if (v->nin) {
            voff = ring_push(&s_vring, b->inputs, b->nverts * stride, stride);
            if (voff == ~0u)
                return 1;
        }
        s_vlast_raw = 0;
    }
    ioff = ring_push(&s_iring, b->indices, b->nindices * 4u, 4);
    if (ioff == ~0u)
        return 1;
    cbs[0] = s_xcb;
    cbs[1] = s_kcb_ffp;
    cbs[2] = s_fcb;
    /* I3 (gfx-03x's merge): the fourth draw path's bindings go through the
     * same shadow as the other three, so a later skip never trusts a binding
     * this draw replaced; with I3 off each is the plain call, as before */
    sh_il(il);
    sh_topo(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    if (v->nin && !s_vlast_raw)
        sh_vb(0, 1, &s_vring.buf, &stride, &voff);
    sh_ib(s_iring.buf, DXGI_FORMAT_R32_UINT, 0);
    sh_vs(v->vs);
    sh_vs_cb(0, 3, cbs);
    gs_set(vsh_filter_gs());
    ID3D11DeviceContext_DrawIndexed(s_ctx, b->nindices, ioff / 4u, 0);
    if (bw_edgeprobe_on())              /* BW_GPU_EDGEPROBE (test aid) */
        bw_edgeprobe_at(BW_EP_DRAW, s_ctx, (bwg_cur->mtex ? (void *)bwg_cur->mtex : (void *)bwg_cur->tex), bwg_gen);
    /* BW_GFXCAP_CLASSES (test aid): classed as bw_xdraw classes it */
    if (g_bw_gfxcls) gfxcls_drawn(&b->xb, s_vfx_log_ov, NULL, b->xb.state, NULL);
    /* bw_xdraw's tail */
    if (bw_vfx_active()) {
        vfx_drawlog('f', b->nindices, &b->xb, NULL, 0);
        if (!ov)
            vfx_scene_draw();
    }
    ft_draw('f', b->nindices, 0.0f, 0.0f, 0.0f, 0.0f, 0,
            b->xb.tex_valid[0] ? b->xb.tex[0].offset : 0, b->xb.state);
    if (!bwg_memo_on())
        gs_set(NULL);
    s_vlast = v;
    s_vlast_voff = voff;
    s_vlast_stride = stride;
    s_vlast_nverts = b->nverts;
    SURF_DRAWN(bwg_cur);
    s_st_draws++;
    s_st_fdraws++;
    s_ffp_draws++;
    s_st_tris += b->nindices / 3;
    return 1;
}

/* The executor asks before it builds a fixed-function batch. BW_GPU_AB=gpuffp
 * (test aid) leaves odd frames on the CPU path, for a same-run A/B. */
static int bw_ffp_ok(void)
{
    return ffp_on() && gpu_vsh_on() && !bw_ab_off("gpuffp");
}

/* BW_GPU_STATS: the native finish's line, when either switch is on. */
static void native_stats_line(void)
{
    unsigned long long ex[3];
    if (!ffp_on() && !vsh_sync_on())
        return;
    bw_exec_ffp_counts(ex);
    fprintf(stderr, "[GPU-NATIVE] fixed function: %u GPU draws, %u back to the CPU while compiling, %u declined"
                    " (this run: %llu GPU batches, %llu CPU, %llu of them a vertex fetch_attr couldn't read)"
                    " | vertex shaders first seen this run: %u from the warm-up, %u from the cache, %u compiled"
                    " now (%.0f ms, worst %.1f; %u failed), %u to the worker | CPU fallback draws %u\n",
            s_st_fdraws, s_st_ffall, s_st_fdecl, ex[0], ex[1], ex[2], s_vs_warm, s_vs_cache, s_vs_now,
            s_vs_now_ms, s_vs_now_max, s_vs_nowfail, s_vs_queued, s_st_vfall + s_st_ffall);
    s_st_fdraws = s_st_ffall = s_st_fdecl = 0;
}

/* BW_GPU_VSH_VERIFY: run the last batch's vertices through its shader again,
 * as points into a stream-output buffer, and read them back. The executor
 * compares them with the CPU interpreter's results. Slow (it waits for the
 * GPU), so only sampled batches are checked. */
static int bw_vsh_readback(BwXVsOut *out, uint32_t n)
{
    static ID3D11Buffer *sob, *stg;
    static UINT so_cap;
    g_bw_ctx_epoch++;                   /* I3: it sets state of its own */
    static const D3D11_SO_DECLARATION_ENTRY decl[] = {
        { 0, "SV_POSITION", 0, 0, 4, 0 },
        { 0, "COLOR", 0, 0, 4, 0 },
        { 0, "COLOR", 1, 0, 4, 0 },
        { 0, "FOG", 0, 0, 1, 0 },
        { 0, "SV_ClipDistance", 0, 0, 2, 0 },
        { 0, NULL, 0, 0, 1, 0 },                  /* pad */
        { 0, "TEXCOORD", 0, 0, 4, 0 },
        { 0, "TEXCOORD", 1, 0, 4, 0 },
        { 0, "TEXCOORD", 2, 0, 4, 0 },
        { 0, "TEXCOORD", 3, 0, 4, 0 },
    };
    UINT stride = sizeof(BwXVsOut), zero = 0, need = n * (UINT)sizeof(BwXVsOut), i;
    BwVsh *v = s_vlast;
    D3D11_MAPPED_SUBRESOURCE m;
    ID3D11Buffer *nullb = NULL;

    if (!v || !v->code || n != s_vlast_nverts || !n)
        return 0;
    if (!v->so && FAILED(ID3D11Device_CreateGeometryShaderWithStreamOutput(
                    s_dev, v->code, v->code_size, decl, 10, &stride, 1,
                    D3D11_SO_NO_RASTERIZED_STREAM, NULL, &v->so))) {
        v->so = NULL;
        return 0;
    }
    if (need > so_cap) {
        D3D11_BUFFER_DESC bd;
        if (sob) ID3D11Buffer_Release(sob);
        if (stg) ID3D11Buffer_Release(stg);
        sob = stg = NULL;
        so_cap = 0;
        memset(&bd, 0, sizeof(bd));
        bd.ByteWidth = need;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_STREAM_OUTPUT;
        if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &sob)))
            return 0;
        bd.Usage = D3D11_USAGE_STAGING;
        bd.BindFlags = 0;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &stg)))
            return 0;
        so_cap = need;
    }
    if (s_vlast_raw) {
        /* P1: the raw streams as they were bound; vertex i of the span */
        ID3D11Buffer *bufs[BW_XVS_MAX_STREAMS];
        for (i = 0; i < s_vlast_ns; i++)
            bufs[i] = s_vring.buf;
        ID3D11DeviceContext_IASetInputLayout(s_ctx, s_vlast_il);
        ID3D11DeviceContext_IASetVertexBuffers(s_ctx, 0, s_vlast_ns, bufs, s_vlast_strides, s_vlast_offs);
    } else {
        ID3D11DeviceContext_IASetInputLayout(s_ctx, v->il);
        if (v->nin)
            ID3D11DeviceContext_IASetVertexBuffers(s_ctx, 0, 1, &s_vring.buf, &s_vlast_stride,
                                                   &s_vlast_voff);
    }
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    ID3D11DeviceContext_VSSetShader(s_ctx, v->vs, NULL, 0);
    gs_set(v->so);                      /* item 43: through the tracker */
    ID3D11DeviceContext_SOSetTargets(s_ctx, 1, &sob, &zero);
    ID3D11DeviceContext_Draw(s_ctx, n, 0);
    ID3D11DeviceContext_SOSetTargets(s_ctx, 1, &nullb, &zero);
    gs_set(NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)stg, (ID3D11Resource *)sob);
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)stg, 0, D3D11_MAP_READ, 0, &m)))
        return 0;
    memcpy(out, m.pData, need);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)stg, 0);
    /* Back to the executor's terms: surface pixels and the title's z. */
    for (i = 0; i < n; i++) {
        float *p = out[i].pos, w = p[3];
        if (w != 0.0f) {
            float zd = p[2] / w;
            p[0] = (p[0] / w + 1.0f) * 0.5f * s_xlast_surf[0];
            p[1] = (1.0f - p[1] / w) * 0.5f * s_xlast_surf[1];
            p[2] = s_xlast_depth[1] != 0.0f ? zd / s_xlast_depth[1] + s_xlast_depth[0] : zd;
        }
    }
    return 1;
}

/* gfx-sprites (I6): the executor asks before it hands a point batch over. */
static int bw_points_ok(void)
{
    return gpu_vsh_on() && bwg_sprites_on() && ensure_device() && sprite_gs() != NULL;
}

/* BW_GPU_SPRITES_VERIFY (gfx-sprites; nv2a_pb_exec.c gpu_sprite_verify): run
 * the last point batch again through its vertex shader and the sprite
 * geometry shader compiled with BW_SPR_SO (the same text, plus each
 * vertex's point and corner), into a stream-output buffer, and read it back
 * in the executor's terms (surface pixels, the title's z): the widescreen
 * remap and the mapping undone as bw_vsh_readback does. Slow; sampled. */
static int bw_sprite_readback(BwXSpriteOut *out, uint32_t max, uint32_t *count)
{
    static ID3D11GeometryShader *sogs;
    static int tried;
    static ID3D11Buffer *sob, *stg;
    static ID3D11Query *q;
    static UINT so_cap;
    static const D3D11_SO_DECLARATION_ENTRY decl[] = {
        { 0, "SV_POSITION", 0, 0, 4, 0 },
        { 0, "TEXCOORD", 3, 0, 4, 0 },
        { 0, "SPRCHK", 0, 0, 4, 0 },
    };
    UINT stride = sizeof(BwXSpriteOut), zero = 0, need, i;
    BwVsh *v = s_vlast;
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_QUERY_DATA_SO_STATISTICS st;
    ID3D11Buffer *nullb = NULL, *gcbs[2];
    ID3D11GeometryShader *gs_was = s_gs_bound;

    *count = 0;
    if (!v || !v->vs || !s_vlast_points || !s_vlast_nidx || !s_scb)
        return 0;
    g_bw_ctx_epoch++;                   /* I3: it sets state of its own (gfx-03x's merge) */
    if (!tried) {
        D3D_SHADER_MACRO mac[3] = { { "BW_SPR_SO", "1" }, { NULL, NULL }, { NULL, NULL } };
        ID3DBlob *blob = NULL, *err = NULL;
        D3D11_QUERY_DESC qd;
        tried = 1;
        if (bwg_ws_k > 1.0f) {
            mac[1].Name = "BW_WS";
            mac[1].Definition = "1";
        }
        if (FAILED(D3DCompile(bw_vsh_sprite_gs_hlsl, strlen(bw_vsh_sprite_gs_hlsl), "bw_sprite_gs_so", mac,
                              NULL, "gs_main", "gs_5_0",
                              D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_IEEE_STRICTNESS, 0, &blob, &err))) {
            fprintf(stderr, "[SPR-VERIFY] the stream-output geometry shader failed: %s\n",
                    err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
            if (err) ID3D10Blob_Release(err);
            return 0;
        }
        if (err) ID3D10Blob_Release(err);
        if (FAILED(ID3D11Device_CreateGeometryShaderWithStreamOutput(
                        s_dev, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), decl, 3,
                        &stride, 1, D3D11_SO_NO_RASTERIZED_STREAM, NULL, &sogs)))
            sogs = NULL;
        ID3D10Blob_Release(blob);
        memset(&qd, 0, sizeof qd);
        qd.Query = D3D11_QUERY_SO_STATISTICS;
        if (FAILED(ID3D11Device_CreateQuery(s_dev, &qd, &q)))
            q = NULL;
    }
    if (!sogs || !q)
        return 0;
    need = s_vlast_nidx * 6u * stride;
    if (need > so_cap) {
        D3D11_BUFFER_DESC bd;
        if (sob) ID3D11Buffer_Release(sob);
        if (stg) ID3D11Buffer_Release(stg);
        sob = stg = NULL;
        so_cap = 0;
        memset(&bd, 0, sizeof(bd));
        bd.ByteWidth = need;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_STREAM_OUTPUT;
        if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &sob)))
            return 0;
        bd.Usage = D3D11_USAGE_STAGING;
        bd.BindFlags = 0;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &stg)))
            return 0;
        so_cap = need;
    }
    if (s_vlast_raw) {
        ID3D11Buffer *bufs[BW_XVS_MAX_STREAMS];
        for (i = 0; i < s_vlast_ns; i++)
            bufs[i] = s_vring.buf;
        ID3D11DeviceContext_IASetInputLayout(s_ctx, s_vlast_il);
        ID3D11DeviceContext_IASetVertexBuffers(s_ctx, 0, s_vlast_ns, bufs, s_vlast_strides, s_vlast_offs);
    } else {
        ID3D11DeviceContext_IASetInputLayout(s_ctx, v->il);
        if (v->nin)
            ID3D11DeviceContext_IASetVertexBuffers(s_ctx, 0, 1, &s_vring.buf, &s_vlast_stride,
                                                   &s_vlast_voff);
    }
    ID3D11DeviceContext_IASetIndexBuffer(s_ctx, s_iring.buf, DXGI_FORMAT_R32_UINT, 0);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    ID3D11DeviceContext_VSSetShader(s_ctx, v->vs, NULL, 0);
    gcbs[0] = s_xcb;
    gcbs[1] = s_scb;
    gs_set(sogs);
    ID3D11DeviceContext_GSSetConstantBuffers(s_ctx, 0, 2, gcbs);
    ID3D11DeviceContext_SOSetTargets(s_ctx, 1, &sob, &zero);
    ID3D11DeviceContext_Begin(s_ctx, (ID3D11Asynchronous *)q);
    ID3D11DeviceContext_DrawIndexed(s_ctx, s_vlast_nidx, s_vlast_ioff / 4u, 0);
    ID3D11DeviceContext_End(s_ctx, (ID3D11Asynchronous *)q);
    ID3D11DeviceContext_SOSetTargets(s_ctx, 1, &nullb, &zero);
    gs_set(gs_was);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)stg, (ID3D11Resource *)sob);
    while (ID3D11DeviceContext_GetData(s_ctx, (ID3D11Asynchronous *)q, &st, sizeof st, 0) == S_FALSE)
        ;
    if (st.NumPrimitivesWritten * 3u > max || st.NumPrimitivesWritten * 3u * stride > need)
        return 0;
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)stg, 0, D3D11_MAP_READ, 0, &m)))
        return 0;
    memcpy(out, m.pData, (size_t)st.NumPrimitivesWritten * 3u * stride);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)stg, 0);
    *count = (uint32_t)st.NumPrimitivesWritten * 3u;
    for (i = 0; i < *count; i++) {
        float *p = out[i].pos, w = p[3];
        float pf = ((float *)&out[i].point)[0], cf = ((float *)&out[i].corner)[0];
        out[i].point = (uint32_t)pf;
        out[i].corner = (uint32_t)cf;
        if (w != 0.0f) {
            float zd = p[2] / w, xn = p[0] / w;
            if (s_xlast_wsx[0] != 0.0f && bwg_ws_k > 1.0f)
                xn = (xn - s_xlast_wsx[1]) / s_xlast_wsx[0];
            p[0] = (xn + 1.0f) * 0.5f * s_xlast_surf[0];
            p[1] = (1.0f - p[1] / w) * 0.5f * s_xlast_surf[1];
            p[2] = s_xlast_depth[1] != 0.0f ? zd / s_xlast_depth[1] + s_xlast_depth[0] : zd;
        }
    }
    return 1;
}

/* T1 (VFX_TEX_RESTORE): a vertex-program batch handed back to the CPU path
 * (its shader still compiling) comes back as the next bw_xdraw; remember if
 * it was an overlay draw (D3D's pass-through program, no depth test), so the
 * texture restoration leaves its textures alone there too. Only a flag; the
 * draw itself is unchanged in every edition. */
static int bw_xdraw_vsh_t1(const Nv2aSurface *s, const BwXVsBatch *b)
{
    ULONG64 c0 = draw_cpu_begin();     /* item 43: draw cpu */
    int r;
    if (b && b->ffp) {                  /* gfx-ffp: never an overlay handed back */
        r = bw_xdraw_ffp(s, b);
        draw_cpu_end(c0);
        return r;
    }
    r = bw_xdraw_vsh(s, b);
    s_tr_fb_overlay = !r && b && b->ninstr <= 4 && b->xb.state && !b->xb.state->depth_test_enable;
    draw_cpu_end(c0);
    return r;
}

static void bw_xdraw_m(const Nv2aSurface *s, const BwXBatch *b)
{
    ULONG64 c0 = draw_cpu_begin();     /* item 43: draw cpu */
    gs_unbind();                        /* item 43: a vertex-program draw may have left it bound */
    bw_xdraw(s, b);
    draw_cpu_end(c0);
}

/* By name (gfx-ffp, the lead's ask): several lanes append entries. */
static const BwXBackend s_xbackend = {
    .draw = bw_xdraw_m,
    .draw_vsh = bw_xdraw_vsh_t1,
    .vsh_readback = bw_vsh_readback,
    .vsh_raw_ok = bw_vsh_raw_ok,
    .points_ok = bw_points_ok,              /* gfx-sprites (I6) */
    .sprite_readback = bw_sprite_readback,  /* gfx-sprites */
    .ffp_ok = bw_ffp_ok,                    /* gfx-ffp */
};

/* ---- the three back-end entry points ------------------------------------ */

/* The title's window clip as the scissor rectangle (the core's, bwg_scissor). */
static void apply_scissor(const Nv2aSurface *s)
{
    D3D11_RECT r;
    BwgRect g;
    bwg_scissor(s, bwg_cur->w, bwg_cur->h, bwg_cur->pw, bwg_cur->ph, &g);
    r.left = (LONG)g.left; r.top = (LONG)g.top; r.right = (LONG)g.right; r.bottom = (LONG)g.bottom;
    ID3D11DeviceContext_RSSetScissorRects(s_ctx, 1, &r);
    s_vfx_scissor = r;                      /* VFX (D9): the scene copy's per-view rectangle */
}

static int begin(const Nv2aSurface *s, const Nv2aRenderState *rs)
{
    BwSurf *surf;
    BwDepth *d;
    if (!ensure_device() || !s || !s->color_va)
        return 0;
    surf = bwg_get_surf(s);
    if (!surf)
        return 0;
    BWG_LATCH_USE(surf);                /* I1: a CPU write into it since its last use, up first */
    if (bwg_feedback_mode())
        bwg_fb_begin();                 /* I5 (BW_GPU_FEEDBACK): a new clear or draw */
    /* VFX (D9): pw, ph = w, h in Original; item 36: the depth buffer has the
     * colour target's sample count (1 in Original) */
    d = rs ? bwg_get_depth(rs->zeta_va, surf->pw, surf->ph, surf->mrtv ? bw_vfx_msaa() : 1) : NULL;
    surf->used_gen = bwg_gen;                 /* item 37: a full table reuses the oldest */
    if (d)
        d->used_gen = bwg_gen;
    if (surf != bwg_cur || d != bwg_cur_depth)
        bind_targets(surf, d);
    apply_scissor(s);
    if (g_bw_gfxcls) bw_gfxcls_sync(s_ctx, gfxcls_tex(bwg_cur), bwg_gen);  /* BW_GFXCAP_CLASSES (test aid) */
    if (bw_edgeprobe_on())              /* BW_GPU_EDGEPROBE (test aid) */
        bw_edgeprobe_at(BW_EP_BEGIN, s_ctx, (bwg_cur->mtex ? (void *)bwg_cur->mtex : (void *)bwg_cur->tex), bwg_gen);
    return 1;
}

static void bw_clear(const Nv2aSurface *s, const Nv2aRenderState *rs, uint32_t flags,
                     uint32_t argb, uint32_t zstencil)
{
    stats_tick();
    if (!begin(s, rs))
        return;
    if (flags & 0xF0) {
        float c[4] = { ((argb >> 16) & 0xFF) / 255.0f, ((argb >> 8) & 0xFF) / 255.0f,
                       (argb & 0xFF) / 255.0f, ((argb >> 24) & 0xFF) / 255.0f };
        ID3D11DeviceContext_ClearRenderTargetView(s_ctx, bwg_cur->mrtv ? bwg_cur->mrtv : bwg_cur->rtv, c);
        if (g_bw_gfxcls) bw_gfxcls_clear(s_ctx, gfxcls_tex(bwg_cur), bwg_gen);   /* BW_GFXCAP_CLASSES (test aid) */
        SURF_DRAWN(bwg_cur);
        ft_note('C', bwg_cur->va);
    }
    if ((flags & 0x3) && bwg_cur_depth && bwg_cur_depth->dsv) {         /* item 36: never a NULL view */
        UINT f = ((flags & 1) ? D3D11_CLEAR_DEPTH : 0) | ((flags & 2) ? D3D11_CLEAR_STENCIL : 0);
        ID3D11DeviceContext_ClearDepthStencilView(s_ctx, bwg_cur_depth->dsv, f,
                                                  (zstencil >> 8) / 16777215.0f,
                                                  (UINT8)(zstencil & 0xFF));
    }
    s_st_clears++;
}

static void bw_draw(const Nv2aSurface *s, const Nv2aBatch *b)
{
    const Nv2aRenderState *rs;
    D3D11_MAPPED_SUBRESOURCE m;
    BwConsts c;
    UINT stride = sizeof(Nv2aVertex), offs = 0, bytes;
    ID3D11ShaderResourceView *srv = NULL;
    ID3D11SamplerState *ss = NULL;
    float bf[4];

    stats_tick();
    if (!b || !b->count || !b->state || !begin(s, b->state))
        return;
    rs = b->state;
    bytes = b->count * sizeof(Nv2aVertex);
    if (bytes > s_vb_bytes) {
        D3D11_BUFFER_DESC bd;
        if (s_vb) ID3D11Buffer_Release(s_vb);
        s_vb_bytes = bytes * 2 > (1u << 20) ? bytes * 2 : (1u << 20);
        memset(&bd, 0, sizeof(bd));
        bd.ByteWidth = s_vb_bytes;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_vb))) {
            s_vb = NULL; s_vb_bytes = 0;
            return;
        }
    }
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_vb, 0,
                                       D3D11_MAP_WRITE_DISCARD, 0, &m)))
        return;
    memcpy(m.pData, b->vertices, bytes);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_vb, 0);

    if (b->texture) {
        srv = get_texture(b->texture);
        /* The texture's own filter, as the combiner path (BW_GPU_SSFILTER=0:
         * always bilinear, as before 2026-10-27). This interface carries no
         * border colour, so BORDER stays transparent black here. */
        static int ssf = -1;
        if (ssf < 0) {
            const char *e = getenv("BW_GPU_SSFILTER");
            ssf = !(e && e[0] == '0');
        }
        ss = ssf ? get_sampler_f(b->texture->addr_u, b->texture->addr_v, b->texture->filter, 1, 0)
                 : get_sampler(b->texture->addr_u, b->texture->addr_v);
    }
    bwg_ss_consts(s, b, srv != NULL, &c);
    if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_cb, 0,
                                          D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, &c, sizeof(c));
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_cb, 0);
    }
    bwg_blend_factor(rs, bf);

    sh_il(s_layout);                    /* I3: each sent only if it changed */
    sh_topo(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    sh_vb(0, 1, &s_vb, &stride, &offs);
    sh_vs(s_vs);
    sh_vs_cb(0, 1, &s_cb);
    sh_ps(s_ps);
    sh_ps_cb0(s_cb);
    sh_ps_srv(0, 1, &srv);
    if (ss)
        sh_ps_smp(0, 1, &ss);
    {
        ID3D11RasterizerState *rst = get_rs(rs);
        if (!rst)
            return;              /* both faces culled */
        sh_rs(rst);
    }
    sh_blend(get_blend(rs), bf, 0xFFFFFFFFu);
    sh_ds(get_ds(rs, bwg_cur_depth != NULL), rs->stencil_ref & 0xFF);
    ID3D11DeviceContext_Draw(s_ctx, b->count, 0);
    if (g_bw_gfxcls) gfxcls_drawn(NULL, 1, b->texture, rs, NULL);            /* BW_GFXCAP_CLASSES (test aid) */
    if (bw_edgeprobe_on())              /* BW_GPU_EDGEPROBE (test aid) */
        bw_edgeprobe_at(BW_EP_DRAW, s_ctx, (bwg_cur->mtex ? (void *)bwg_cur->mtex : (void *)bwg_cur->tex), bwg_gen);
    ft_draw('s', b->count, b->vertices[0].x, b->vertices[0].y, b->vertices[0].z,
            b->vertices[0].rhw, b->vertices[0].diffuse, b->texture ? b->texture->offset : 0, rs);
    SURF_DRAWN(bwg_cur);
    s_st_draws++;
    s_st_tris += b->count / 3;
}

/* Diagnostic: is the APU's trap hole (guest 0xFE800000..0xFE830000, left
 * unmapped so every access faults into the device model) still free? */
static void check_holes(void)
{
    static ULONGLONG next;
    ULONGLONG now = GetTickCount64();
    MEMORY_BASIC_INFORMATION mbi;
    uint32_t vas[3] = { 0xFE800000u, 0xFE820000u, 0xFE82F000u };
    if (s_stats <= 0 || now < next)
        return;
    next = now + 5000;
    for (int i = 0; i < 3; i++)
        if (VirtualQuery(GUEST(vas[i]), &mbi, sizeof(mbi)))
            fprintf(stderr, "[GPU-D3D11] hole %08X: state %s protect 0x%lx alloc base %p size 0x%llx\n",
                    vas[i], mbi.State == MEM_FREE ? "free" : mbi.State == MEM_RESERVE ? "reserved"
                    : "COMMITTED", (unsigned long)mbi.Protect, mbi.AllocationBase,
                    (unsigned long long)mbi.RegionSize);
}

/* 0.2.5 item 35: what of a surface's guest copy the debug watchers may read
 * (BW_GPU_BLACKWATCH, BW_GPU_FLASHWATCH, BW_GPU_PEEK): the rows readback()
 * wrote, at the pitch it wrote them with, never past the end of the guest
 * block -- the readback's own clamp, which the watchers lacked (the front
 * buffer is sometimes bound with the 2x surface's pitch). Returns the rows
 * (0: nothing safe to read) and the pitch. */
static uint32_t watch_rows(const BwSurf *sf, uint32_t *pitch_out)
{
    uint32_t row = sf->w * sf->bpp, rows = sf->h;
    uint32_t pitch = sf->cur_pitch ? sf->cur_pitch : (sf->pitch ? sf->pitch : row);
    uint32_t room = xbox_ContiguousBlockSize(sf->va);
    *pitch_out = pitch;
    if (!row || !rows || row > pitch)
        return 0;
    if (room) {
        if (row > room)
            return 0;
        if ((uint64_t)(rows - 1) * pitch + row > room)
            rows = (room - row) / pitch + 1;
    }
    return rows;
}

/* Fraction of near-black pixels in the lower 60% of the presented frame
 * (guest memory, after readback), sampled every 4th pixel and row. */
static float black_fraction(const BwSurf *sf)
{
    const uint8_t *base = GUEST(sf->va);
    uint32_t x, y, n = 0, k = 0, pitch, rows = watch_rows(sf, &pitch);
    if (sf->bpp != 4 || !rows)
        return 0.0f;
    for (y = sf->h * 2 / 5; y < rows; y += 4)
        for (x = 0; x < sf->w; x += 4) {
            uint32_t v = *(const uint32_t *)(base + (size_t)y * pitch + x * 4);
            n++;
            if (((v >> 16) & 0xFF) < 14 && ((v >> 8) & 0xFF) < 14 && (v & 0xFF) < 14)
                k++;
        }
    return n ? (float)k / n : 0.0f;
}

static void write_bmp(const char *path, const BwSurf *sf)
{
    uint32_t pitch, rows = sf->bpp == 4 ? watch_rows(sf, &pitch) : 0;
    uint32_t y, rowb = sf->w * 4, size = 54 + rowb * rows;
    uint8_t h[54];
    FILE *f;
    if (!rows)
        return;                         /* item 35: nothing safe to read (or 16-bit) */
    f = fopen(path, "wb");
    if (!f)
        return;
    memset(h, 0, sizeof h);
    h[0] = 'B'; h[1] = 'M';
    memcpy(h + 2, &size, 4);
    h[10] = 54; h[14] = 40;
    memcpy(h + 18, &sf->w, 4);
    { int32_t hh = -(int32_t)rows; memcpy(h + 22, &hh, 4); }
    h[26] = 1; h[28] = 32;
    fwrite(h, 1, 54, f);
    for (y = 0; y < rows; y++)
        fwrite(GUEST(sf->va) + (size_t)y * pitch, 1, rowb, f);
    fclose(f);
}

static void print_uses(const char *what, int slot)
{
    int i;
    fprintf(stderr, "    %s:", what);
    for (i = 0; i < bwg_tex_nuse[slot]; i++) {
        const BwgTexUse *u = &bwg_tex_use[slot][i];
        fprintf(stderr, " %c%08X/%ux%u/%02X", u->kind, u->offset, u->w, u->h, u->color);
        if (u->zero_pct >= 0)
            fprintf(stderr, "/z%d", u->zero_pct);
    }
    fprintf(stderr, "\n");
}

/* BW_GPU_PEEK=<file.bmp>: every BW_GPU_PEEK_EVERY flips (default 10) the
 * presented frame, in the title's own 640-wide pixels (the anti-aliasing
 * columns averaged), as a 24-bit BMP. Written to <file>.tmp and renamed over
 * <file>, so a reader (tools/bw_autopilot.py) never sees half a frame and the
 * disk holds one file, not a stream. */
/* BW_GPU_FLASHWATCH=<prefix>: single-frame flashes. At every flip the frame
 * is reduced to a grid of block brightnesses (16x16 of the title's pixels).
 * When frame i differs from both i-1 and i+1 in a block while i-1 and i+1
 * agree, the block flashed: not camera motion (that changes all three), but
 * something drawn for one frame only. Each event is logged with the blocks'
 * bounding box and whether they went brighter or darker; the first 12 are
 * saved as <prefix>_<frame>_{a,b,c}.bmp (before, the flash, after). Muzzle
 * flashes are flashes too: test where nobody fires. */
#define FW_GX 40
#define FW_GY 30
static void write_bmp(const char *path, const BwSurf *sf);

static void flash_watch(const BwSurf *sf)
{
    static const char *prefix;
    static int checked, dumps;
    static float g[3][FW_GY][FW_GX];
    static uint8_t *img[3];
    static size_t img_cap[3];           /* item 35: each copy's own size ... */
    static uint32_t img_w[3], img_h[3], img_pitch[3];   /* ... and what it holds */
    static uint32_t frame_no[3], have, events, frames, sec_events;
    static DWORD last_tick;
    uint32_t bx, by, x, y, slot, bw, bh, pitch, rows;
    int n = 0, x0 = FW_GX, y0 = FW_GY, x1 = -1, y1 = -1;
    float sign = 0;

    if (!checked) {
        checked = 1;
        prefix = getenv("BW_GPU_FLASHWATCH");
    }
    if (!prefix || !*prefix || sf->bpp != 4 || sf->w < FW_GX || sf->h < FW_GY)
        return;
    rows = watch_rows(sf, &pitch);      /* item 35: the readback's clamp */
    if (rows < FW_GY)
        return;
    /* shift the window: 0 oldest, 2 newest */
    {
        uint8_t *t = img[0];
        size_t tc = img_cap[0];
        memmove(g[0], g[1], sizeof g[0] * 2);
        img[0] = img[1]; img[1] = img[2]; img[2] = t;
        img_cap[0] = img_cap[1]; img_cap[1] = img_cap[2]; img_cap[2] = tc;
        img_w[0] = img_w[1]; img_w[1] = img_w[2];
        img_h[0] = img_h[1]; img_h[1] = img_h[2];
        img_pitch[0] = img_pitch[1]; img_pitch[1] = img_pitch[2];
        frame_no[0] = frame_no[1]; frame_no[1] = frame_no[2]; frame_no[2] = bwg_gen;
    }
    slot = 2;
    img_w[2] = img_h[2] = img_pitch[2] = 0;
    bw = sf->w / FW_GX;
    bh = rows / FW_GY;
    for (by = 0; by < FW_GY; by++)
        for (bx = 0; bx < FW_GX; bx++) {
            uint32_t sum = 0, cnt = 0;
            for (y = by * bh; y < by * bh + bh; y += 2) {
                const uint8_t *row = GUEST(sf->va) + (size_t)y * pitch;
                for (x = bx * bw; x < bx * bw + bw; x += 2) {
                    const uint8_t *p = row + (size_t)x * 4;
                    sum += p[0] + 2u * p[1] + p[2];
                    cnt += 4;
                }
            }
            g[slot][by][bx] = cnt ? (float)sum / cnt : 0.0f;
        }
    if (dumps < 12) {
        /* item 35: sized for this frame (the first surface's size used to
         * serve every later, larger one), and only the rows that are there */
        size_t sz = (size_t)(rows - 1) * pitch + (size_t)sf->w * 4;
        if (img_cap[2] < sz) {
            free(img[2]);
            img[2] = (uint8_t *)malloc(sz);
            img_cap[2] = img[2] ? sz : 0;
        }
        if (img[2]) {
            memcpy(img[2], GUEST(sf->va), sz);
            img_w[2] = sf->w; img_h[2] = rows; img_pitch[2] = pitch;
        }
    }
    frames++;
    if (have < 3) {
        have++;
        return;
    }
    for (by = 0; by < FW_GY; by++)
        for (bx = 0; bx < FW_GX; bx++) {
            float a = g[1][by][bx] - g[0][by][bx], b = g[1][by][bx] - g[2][by][bx];
            float c = g[0][by][bx] - g[2][by][bx];
            if (fabsf(a) > 24.0f && fabsf(b) > 24.0f && fabsf(c) < 8.0f && a * b > 0) {
                n++;
                sign += a;
                if ((int)bx < x0) x0 = bx;
                if ((int)bx > x1) x1 = bx;
                if ((int)by < y0) y0 = by;
                if ((int)by > y1) y1 = by;
            }
        }
    if (n >= 2) {
        events++;
        sec_events++;
        if (events <= 200)
            fprintf(stderr, "[BW-FLASH] frame %u: %d blocks %s, x %d-%d y %d-%d (of %dx%d)\n",
                    frame_no[1], n, sign > 0 ? "brighter" : "darker", x0, x1, y0, y1,
                    FW_GX, FW_GY);
        if (dumps < 12 && img[0] && img[1] && img[2] && img_w[0] && img_w[0] == img_w[1]
                && img_w[1] == img_w[2] && img_h[0] == img_h[1] && img_h[1] == img_h[2]) {
            char path[600];
            const char *tag = "abc";
            int k;
            for (k = 0; k < 3; k++) {
                /* our copies, each at the size and pitch it was taken with */
                _snprintf(path, sizeof path, "%s_%06u_%c.bmp", prefix, frame_no[1], tag[k]);
                path[sizeof path - 1] = 0;
                {
                    FILE *f = fopen(path, "wb");
                    uint32_t rowb = img_w[k] * 4, size = 54 + rowb * img_h[k], yy;
                    uint8_t hd[54];
                    if (!f)
                        continue;
                    memset(hd, 0, sizeof hd);
                    hd[0] = 'B'; hd[1] = 'M';
                    memcpy(hd + 2, &size, 4);
                    hd[10] = 54; hd[14] = 40;
                    memcpy(hd + 18, &img_w[k], 4);
                    { int32_t hh = -(int32_t)img_h[k]; memcpy(hd + 22, &hh, 4); }
                    hd[26] = 1; hd[28] = 32;
                    fwrite(hd, 1, 54, f);
                    for (yy = 0; yy < img_h[k]; yy++)
                        fwrite(img[k] + (size_t)yy * img_pitch[k], 1, rowb, f);
                    fclose(f);
                }
            }
            dumps++;
        }
    }
    if (GetTickCount() - last_tick >= 5000) {
        last_tick = GetTickCount();
        fprintf(stderr, "[BW-FLASH] %u flash frames in %u frames so far (%u in the last 5 s)\n",
                events, frames, sec_events);
        sec_events = 0;
    }
}

static void peek_write(const BwSurf *sf)
{
    static const char *path;
    static int checked, every = 10;
    static unsigned n;
    static uint8_t *row;
    static size_t row_cap;
    char tmp[600];
    FILE *f;
    uint32_t aa, w, h, y, x, rowb, size, pitch;
    uint8_t hd[54];

    if (!checked) {
        const char *e = getenv("BW_GPU_PEEK_EVERY");
        checked = 1;
        path = getenv("BW_GPU_PEEK");
        if (e && atoi(e) > 0)
            every = atoi(e);
    }
    if (!path || !*path || sf->bpp != 4 || ++n % every)
        return;
    aa = sf->w >= 2 * sf->h * 4 / 3 ? 2 : 1;        /* 1280x480 is 640x480 at 2x1 */
    w = sf->w / aa;
    h = watch_rows(sf, &pitch);         /* item 35: the readback's clamp */
    rowb = (w * 3 + 3) & ~3u;
    if (!h || !w)
        return;
    if (row_cap < rowb) {               /* item 35: was a fixed, unchecked 4096 * 3 */
        free(row);
        row = (uint8_t *)malloc(rowb);
        row_cap = row ? rowb : 0;
        if (!row)
            return;
    }
    _snprintf(tmp, sizeof tmp, "%s.tmp", path);
    tmp[sizeof tmp - 1] = 0;
    f = fopen(tmp, "wb");
    if (!f)
        return;
    memset(hd, 0, sizeof hd);
    size = 54 + rowb * h;
    hd[0] = 'B'; hd[1] = 'M';
    memcpy(hd + 2, &size, 4);
    hd[10] = 54; hd[14] = 40;
    memcpy(hd + 18, &w, 4);
    { int32_t hh = -(int32_t)h; memcpy(hd + 22, &hh, 4); }
    hd[26] = 1; hd[28] = 24;
    fwrite(hd, 1, 54, f);
    for (y = 0; y < h; y++) {
        const uint8_t *src = GUEST(sf->va) + (size_t)y * pitch;
        memset(row, 0, rowb);
        for (x = 0; x < w; x++) {
            const uint8_t *p = src + (size_t)x * aa * 4;
            if (aa == 2) {
                row[x * 3 + 0] = (uint8_t)((p[0] + p[4]) / 2);
                row[x * 3 + 1] = (uint8_t)((p[1] + p[5]) / 2);
                row[x * 3 + 2] = (uint8_t)((p[2] + p[6]) / 2);
            } else {
                row[x * 3 + 0] = p[0]; row[x * 3 + 1] = p[1]; row[x * 3 + 2] = p[2];
            }
        }
        fwrite(row, 1, rowb, f);
    }
    fclose(f);
    MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING);
}

static void black_watch(const BwSurf *sf)
{
    static float prev;
    static int dumps;
    float b;
    if (bwg_tex_watch < 0) {
        s_watch_prefix = getenv("BW_GPU_BLACKWATCH");
        bwg_tex_watch = s_watch_prefix && *s_watch_prefix;
    }
    if (bwg_tex_watch <= 0)
        return;
    b = black_fraction(sf);
    if (b > prev + 0.25f && b > 0.3f) {
        fprintf(stderr, "[BW-BLACK] frame %u: lower screen %.0f%% black (previous frame %.0f%%),"
                        " surface %08X %ux%u\n", bwg_gen, b * 100, prev * 100, sf->va, sf->w, sf->h);
        print_uses("this frame", bwg_gen & 1);
        print_uses("previous frame", (bwg_gen + 1) & 1);
        print_huge("this frame", bwg_gen & 1);
        print_huge("previous frame", (bwg_gen + 1) & 1);
        if (dumps < 12) {
            char path[512];
            _snprintf(path, sizeof path, "%s_%06u.bmp", s_watch_prefix, bwg_gen);
            path[sizeof path - 1] = 0;
            write_bmp(path, sf);
            dumps++;
        }
    }
    prev = b;
    {
        /* BW_GPU_HUGE=1: every frame with giant triangles, the first 60 */
        static int huge_log = -1, logged;
        if (huge_log < 0) {
            const char *e = getenv("BW_GPU_HUGE");
            huge_log = e && e[0] == '1';
        }
        if (huge_log && s_huge[bwg_gen & 1] && logged < 60) {
            logged++;
            fprintf(stderr, "[BW-HUGE] frame %u (lower screen %.0f%% black)\n", bwg_gen, b * 100);
            print_huge("frame", bwg_gen & 1);
        }
    }
    bwg_tex_nuse[(bwg_gen + 1) & 1] = 0;    /* the next frame's list */
    s_huge[(bwg_gen + 1) & 1] = 0;
    s_tris_frame[(bwg_gen + 1) & 1] = 0;
}

/* ---- presenting from the GPU (BW_GPU_PRESENT) ---------------------------- */

/* The flipped frame used to reach the window the long way: read back into
 * guest memory, converted on the CPU, stretched with GDI's HALFTONE on the
 * window thread (70% of that thread at 60 fps). Now the back end draws it
 * into the window itself: a flip-model swap chain on the framebuffer window
 * (carried fb_present.c hands over the HWND and stops painting), the frame
 * letterboxed and filtered bilinearly by the GPU, presented with interval 0
 * (the title's flips are already paced; DWM shows the newest frame at its own
 * vblank, so no tearing in a window). The readback into guest memory stays
 * for frame dumps and for anything the title reads back. BW_GPU_PRESENT=0
 * restores the GDI path. */
extern void *bw_fb_window(void);
extern void bw_fb_external_present(int on);

static IDXGISwapChain1 *s_swap;
static ID3D11RenderTargetView *s_swap_rtv;
static UINT s_swap_w, s_swap_h;
static HWND s_swap_hwnd;
static UINT s_swap_flags;           /* creation flags, passed again to ResizeBuffers */
static HANDLE s_swap_wait;          /* frame-latency waitable: a present that can't stall */
static int s_present_mode = -1;     /* -1 unset, 0 off/failed, 1 on */
static int s_present_wait_mode = -1; /* BW_GPU_PRESENT_WAIT=1: the old blocking present */
/* Remastered high frame rate (D-016, docs/remaster/120fps.md step 1; D7).
 * main.c calls bw_d3d11_present_config only for the editions after Original
 * gameplay boots with BW_HIFPS on; Original never does, and keeps sync
 * interval 0, no tearing flag and the readback.
 *   sync_interval  Present's interval: 0 (the flip hold paces; default), or
 *                  1 to wait for the display's vblank (BW_HIFPS_VSYNC=1)
 *   allow_tearing  create the swap chain with ALLOW_TEARING and present
 *                  with it at interval 0, so a variable-refresh display
 *                  (G-Sync/FreeSync, OLED screens) shows each frame as it
 *                  is ready in borderless fullscreen; in a desktop window
 *                  the compositor still shows whole frames
 *   readback       0: don't copy the flipped frame back into guest memory
 *                  (it waits for the GPU every flip: 4 ms on average in
 *                  uncapped mission 1, run 20261027-021008-r6-gpu-d7callers).
 *                  Nothing in the shipped game reads it (textures at a
 *                  surface address use the GPU copy, find_surf); the
 *                  readers are the debug screenshot (0x1E6902) and our frame
 *                  dumps, which force it back on (readback_wanted). */
static unsigned s_cfg_sync;
static int s_cfg_tear;
static int s_cfg_readback = 1;
static int s_cfg_hifps;             /* lat-q: this boot has Remastered's high frame rate */

void bw_d3d11_present_config(unsigned sync_interval, int allow_tearing, int readback)
{
    s_cfg_hifps = 1;
    s_cfg_sync = sync_interval > 1 ? 1 : sync_interval;
    s_cfg_tear = allow_tearing;
    s_cfg_readback = readback;
    fprintf(stderr, "[GPU-PRESENT] high frame rate: sync interval %u, tearing %s, readback %s\n",
            s_cfg_sync, s_cfg_tear ? "allowed (variable refresh)" : "off",
            s_cfg_readback ? "on" : "off");
}

/* The readback, unless configured off and nothing here needs guest pixels.
 * BW_GPU_READBACK=0/1 overrides. Asked again when the swap chain gives up
 * (item 29: the plain window shows the readback). */
static int s_rb_want = -1;
static int readback_wanted(void)
{
    int want = s_rb_want;
    if (want < 0) {
        const char *e = getenv("BW_GPU_READBACK");
        if (e && *e)
            want = e[0] != '0';
        else
            want = s_cfg_readback || s_present_mode == 0
                   || getenv("RECOMP_FB_DUMP") || getenv("RECOMP_FB_DUMP_FLIPS")
                   || getenv("BW_GPU_BLACKWATCH") || getenv("BW_GPU_FLASHWATCH")
                   || getenv("BW_GPU_PEEK");
        if (BW_GFXCAP_PINNED())
            want = 1;                   /* BW_GFXCAP: the flip records hash the readback */
        if (!want)
            fprintf(stderr, "[GPU-D3D11] readback off: the flipped frame stays on the GPU\n");
        s_rb_want = want;
    }
    return want;
}

static int tearing_supported(void)
{
    static int ok = -1;
    if (ok < 0) {
        IDXGIFactory5 *f5 = NULL;
        BOOL allow = FALSE;
        ok = 0;
        if (SUCCEEDED(CreateDXGIFactory1(&IID_IDXGIFactory5, (void **)&f5))) {
            if (SUCCEEDED(IDXGIFactory5_CheckFeatureSupport(f5, DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                                            &allow, sizeof allow)))
                ok = allow != FALSE;
            IDXGIFactory5_Release(f5);
        }
        fprintf(stderr, "[GPU-PRESENT] tearing (variable refresh) %ssupported\n", ok ? "" : "not ");
    }
    return ok;
}
static ID3D11VertexShader *s_blit_vs;
static ID3D11PixelShader *s_blit_ps;
static ID3D11SamplerState *s_blit_ss;
static ID3D11RasterizerState *s_blit_rs;
static ID3D11BlendState *s_blit_bs;
static ID3D11DepthStencilState *s_blit_ds;

static const char s_blit_hlsl[] =
"Texture2D t0 : register(t0);\n"
"SamplerState s0 : register(s0);\n"
"struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
"V vs_main(uint id : SV_VertexID) {\n"
"  V o; float2 uv = float2((id << 1) & 2, id & 2);\n"
"  o.p = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
"  o.uv = uv; return o; }\n"
/* The DAC gamma ramp (bw_gamma.c) as a 256x1 lookup, linear between entries
 * because the filtered picture falls between them. Identity when the title
 * set none, or with BW_GPU_GAMMA=0. */
"Texture2D lut : register(t1);\n"
"float ramp(float c, int ch) { float x = saturate(c) * 255.0, f = floor(x);\n"
"  int a = (int)f, b = min(a + 1, 255);\n"
"  return lerp(lut.Load(int3(a, 0, 0))[ch], lut.Load(int3(b, 0, 0))[ch], x - f); }\n"
"float4 ps_main(V i) : SV_Target { float3 c = t0.Sample(s0, i.uv).rgb;\n"
"  return float4(ramp(c.r, 0), ramp(c.g, 1), ramp(c.b, 2), 1.0); }\n";

/* The ramp's lookup texture, refreshed when the title sets a new ramp. */
static ID3D11Texture2D *s_lut_tex;
static ID3D11ShaderResourceView *s_lut_srv;
static uint32_t s_lut_seq = ~0u;
static int s_lut_was_on = -1;

static ID3D11ShaderResourceView *gamma_lut(void)
{
    uint8_t ramp[768], texels[256 * 4];
    uint32_t seq;
    int on, i;
    if (!s_lut_tex) {
        D3D11_TEXTURE2D_DESC td;
        memset(&td, 0, sizeof(td));
        td.Width = 256; td.Height = 1; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_lut_tex)))
            return NULL;
        ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)s_lut_tex, NULL, &s_lut_srv);
    }
    on = bw_gamma_lut(ramp);
    seq = on ? bw_gamma_get(ramp) : 0;
    if (BW_GFXCAP_REC()) {              /* BW_GFXCAP: the title's newest ramp, identity or not */
        uint8_t cr[768];
        uint32_t cs = bw_gamma_get(cr);
        bw_gfxcap_gamma(cs, cr);
    }
    if (seq != s_lut_seq || on != s_lut_was_on) {
        for (i = 0; i < 256; i++) {
            texels[i * 4 + 0] = on ? ramp[i] : (uint8_t)i;
            texels[i * 4 + 1] = on ? ramp[256 + i] : (uint8_t)i;
            texels[i * 4 + 2] = on ? ramp[512 + i] : (uint8_t)i;
            texels[i * 4 + 3] = 255;
        }
        ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)s_lut_tex, 0, NULL,
                                              texels, sizeof texels, 0);
        s_lut_seq = seq;
        s_lut_was_on = on;
    }
    return s_lut_srv;
}

static int blit_ensure(void)
{
    ID3DBlob *vsb = NULL, *psb = NULL, *err = NULL;
    D3D11_SAMPLER_DESC sd;
    D3D11_RASTERIZER_DESC rd;
    D3D11_BLEND_DESC bd;
    D3D11_DEPTH_STENCIL_DESC dd;

    if (s_blit_vs && s_blit_ps)
        return 1;
    if (FAILED(D3DCompile(s_blit_hlsl, sizeof(s_blit_hlsl) - 1, "bw_blit", NULL, NULL,
                          "vs_main", "vs_5_0", 0, 0, &vsb, &err))
        || FAILED(D3DCompile(s_blit_hlsl, sizeof(s_blit_hlsl) - 1, "bw_blit", NULL, NULL,
                             "ps_main", "ps_5_0", 0, 0, &psb, &err))) {
        fprintf(stderr, "[GPU-PRESENT] blit shader failed: %s\n",
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
        if (err) ID3D10Blob_Release(err);
        if (vsb) ID3D10Blob_Release(vsb);
        return 0;
    }
    ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(vsb),
                                    ID3D10Blob_GetBufferSize(vsb), NULL, &s_blit_vs);
    ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(psb),
                                   ID3D10Blob_GetBufferSize(psb), NULL, &s_blit_ps);
    ID3D10Blob_Release(vsb);
    ID3D10Blob_Release(psb);
    memset(&sd, 0, sizeof(sd));
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11Device_CreateSamplerState(s_dev, &sd, &s_blit_ss);
    memset(&rd, 0, sizeof(rd));
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    ID3D11Device_CreateRasterizerState(s_dev, &rd, &s_blit_rs);
    memset(&bd, 0, sizeof(bd));
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ID3D11Device_CreateBlendState(s_dev, &bd, &s_blit_bs);
    memset(&dd, 0, sizeof(dd));
    dd.DepthEnable = FALSE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    ID3D11Device_CreateDepthStencilState(s_dev, &dd, &s_blit_ds);
    return s_blit_vs && s_blit_ps && s_blit_ss && s_blit_rs && s_blit_bs && s_blit_ds;
}

/* 0.2.5 item 97: the same picture on a screen in HDR mode, through scRGB:
 * BT.2408's direct mapping (bw_hdr.c: the BT.1886 EOTF with the screen's
 * black, 100% at the Table 4 reference white). Its own shader, so the SDR
 * blit above stays exactly as it was. Built once, the first time it's needed;
 * its constants are refreshed every present (the screen can change). */
static ID3D11PixelShader *s_blit_hdr_ps;
static ID3D11Buffer *s_blit_hdr_cb;
static int s_blit_hdr_ok = -1;

static int blit_hdr_ensure(void)
{
    static const char tail[] =
        "cbuffer H : register(b0) { float4 hk; };\n"
        "float4 ps_hdr(V i) : SV_Target { float3 c = t0.Sample(s0, i.uv).rgb;\n"
        "  c = saturate(float3(ramp(c.r, 0), ramp(c.g, 1), ramp(c.b, 2)));\n"
        "  return float4(bwh_decode(c, hk) * hk.w, 1.0); }\n";
    size_t n1 = sizeof s_blit_hlsl - 1, n2 = strlen(bw_hdr_hlsl), n3 = sizeof tail - 1;
    char *src;
    ID3D10Blob *psb = NULL, *err = NULL;
    D3D11_BUFFER_DESC bd;
    if (s_blit_hdr_ok >= 0)
        return s_blit_hdr_ok;
    s_blit_hdr_ok = 0;
    if (!(src = (char *)malloc(n1 + n2 + n3)))
        return 0;
    memcpy(src, s_blit_hlsl, n1);
    memcpy(src + n1, bw_hdr_hlsl, n2);
    memcpy(src + n1 + n2, tail, n3);
    if (FAILED(D3DCompile(src, n1 + n2 + n3, "bw_blit_hdr", NULL, NULL, "ps_hdr", "ps_5_0", 0, 0, &psb, &err))) {
        fprintf(stderr, "[HDR] the HDR present shader failed: %s\n",
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
        if (err) ID3D10Blob_Release(err);
        free(src);
        return 0;
    }
    free(src);
    ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb),
                                   NULL, &s_blit_hdr_ps);
    ID3D10Blob_Release(psb);
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = 16;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_blit_hdr_cb);
    s_blit_hdr_ok = s_blit_hdr_ps && s_blit_hdr_cb;
    if (s_blit_hdr_ok)
        bw_hdr_log_mapping("the plain picture on a screen in HDR mode (BT.2408's direct mapping)");
    return s_blit_hdr_ok;
}

/* Item 29: a swap chain this window can't have, resize or read is given up
 * once, with one line, and the window goes back to showing the readback
 * through GDI (the toolkit's own window path), instead of all three creates
 * being retried and failing on every flip. s_present_mode goes to 0 ("off or
 * failed"), so the readback comes back on for the plain window even where
 * the high frame rate had turned it off. */
static int s_swap_dead;

static void swap_release(void)
{
    if (s_swap_rtv) { ID3D11RenderTargetView_Release(s_swap_rtv); s_swap_rtv = NULL; }
    if (s_swap) { IDXGISwapChain1_Release(s_swap); s_swap = NULL; }
    if (s_swap_wait) { CloseHandle(s_swap_wait); s_swap_wait = NULL; }
}

static void swap_give_up(const char *what, HRESULT hr)
{
    swap_release();
    s_swap_dead = 1;
    s_present_mode = 0;
    s_rb_want = -1;                     /* readback_wanted asks again */
    bw_fb_external_present(0);          /* the framebuffer window paints again */
    fprintf(stderr, "[GPU-PRESENT] %s failed (0x%08lX): the window shows the frame through GDI"
                    " from now on (the readback is on for it)\n", what, (unsigned long)hr);
}

static const char *swap_flags_name(UINT f, DXGI_SWAP_EFFECT e)
{
    if (e != DXGI_SWAP_EFFECT_FLIP_DISCARD)
        return "the blt model";
    if (f & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING)
        return "a waitable with tearing";
    return f & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT ? "a waitable" : "neither";
}

/* The swap chain on the framebuffer window, sized to its client area. */
static int swap_ensure(void)
{
    g_bw_ctx_epoch++;                   /* I3: it may set state of its own */
    HWND hwnd = (HWND)bw_fb_window();
    RECT c;
    UINT w, h;

    if (s_swap_dead)
        return 0;
    if (!hwnd || !GetClientRect(hwnd, &c))
        return 0;
    w = (UINT)(c.right - c.left);
    h = (UINT)(c.bottom - c.top);
    if (!w || !h)
        return 0;
    if (s_swap && hwnd != s_swap_hwnd)              /* a new window: start over */
        swap_release();
    if (!s_swap) {
        IDXGIDevice *xd = NULL;
        IDXGIAdapter *ad = NULL;
        IDXGIFactory2 *fac = NULL;
        DXGI_SWAP_CHAIN_DESC1 d;
        HRESULT hr = E_FAIL;
        char tried[200];
        size_t tl = 0;

        tried[0] = 0;
        memset(&d, 0, sizeof(d));
        if (SUCCEEDED(ID3D11Device_QueryInterface(s_dev, &IID_IDXGIDevice, (void **)&xd))
            && SUCCEEDED(IDXGIDevice_GetAdapter(xd, &ad))
            && SUCCEEDED(IDXGIAdapter_GetParent(ad, &IID_IDXGIFactory2, (void **)&fac))) {
            /* Item 29: one feature at a time, in this order: the waitable
             * with tearing (Remastered's variable refresh), the waitable
             * alone, neither, then the blt model (older systems: a child
             * window may refuse the flip model). Before, tearing and the
             * waitable were dropped together. */
            UINT wait = s_present_wait_mode ? 0 : DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
            UINT tear = wait && s_cfg_tear && tearing_supported() ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
            /* 0.2.5 item 97: scRGB on a screen in HDR mode, every edition
             * (bw_hdr.c); B8G8R8A8 otherwise, as always */
            DXGI_FORMAT fmt = bw_hdr_swap_format(hwnd, DXGI_FORMAT_B8G8R8A8_UNORM);
            int step;
            for (step = 0; step < 4; step++) {
                if ((step == 0 && !tear) || (step == 1 && !wait))
                    continue;
                d.Width = w;
                d.Height = h;
                d.Format = fmt;
                d.SampleDesc.Count = 1;
                d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                d.SwapEffect = step < 3 ? DXGI_SWAP_EFFECT_FLIP_DISCARD : DXGI_SWAP_EFFECT_DISCARD;
                d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
                d.Flags = step == 0 ? wait | tear : step == 1 ? wait : 0;
                d.BufferCount = step == 3 ? 1 : (d.Flags & wait) ? 3 : 2;
                hr = BW_GPU_FT("swapchain", E_ACCESSDENIED,     /* item 29 test aid (call B1-5) */
                               IDXGIFactory2_CreateSwapChainForHwnd(fac, (IUnknown *)s_dev, hwnd, &d,
                                                                    NULL, NULL, &s_swap));
                if (SUCCEEDED(hr))
                    break;
                s_swap = NULL;
                if (gpu_lost(hr, "CreateSwapChainForHwnd"))     /* item 28 */
                    break;
                if (tl < sizeof tried - 48)
                    tl += (size_t)_snprintf(tried + tl, sizeof tried - tl, "%s%s 0x%08lX",
                                            tl ? ", " : "", swap_flags_name(d.Flags, d.SwapEffect),
                                            (unsigned long)hr);
            }
            tried[sizeof tried - 1] = 0;
            if (SUCCEEDED(hr)) {
                bw_hdr_swap_created(s_swap);    /* item 97: scRGB's colour space, or made 8-bit */
                bw_vfx_swap_created(s_swap);    /* VFX (D9): the suite's own note; no-op otherwise */
            }
            if (SUCCEEDED(hr))
                IDXGIFactory2_MakeWindowAssociation(fac, hwnd, DXGI_MWA_NO_ALT_ENTER
                                                               | DXGI_MWA_NO_WINDOW_CHANGES);
        }
        if (fac) IDXGIFactory2_Release(fac);
        if (ad) IDXGIAdapter_Release(ad);
        if (xd) IDXGIDevice_Release(xd);
        if (FAILED(hr)) {
            char what[260];
            _snprintf(what, sizeof what, "making a swap chain (refused: %s)", tl ? tried : "no factory");
            what[sizeof what - 1] = 0;
            swap_give_up(what, hr);
            return 0;
        }
        s_swap_hwnd = hwnd;
        s_swap_w = w;
        s_swap_h = h;
        s_swap_flags = d.Flags;
        if (d.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) {
            IDXGISwapChain2 *s2 = NULL;
            if (SUCCEEDED(IDXGISwapChain1_QueryInterface(s_swap, &IID_IDXGISwapChain2,
                                                         (void **)&s2))) {
                /* lat-q: BW_GPU_FRAMELAT=<1..3> (Remastered high frame rate
                 * only; unset = 2, as always): how many presents may queue
                 * before the waitable stops signalling. 1 = the screen takes
                 * each frame before the next is presented. */
                UINT ml = 2;
                const char *fe = s_cfg_hifps ? getenv("BW_GPU_FRAMELAT") : NULL;
                if (fe && *fe) {
                    ml = (UINT)atoi(fe);
                    if (ml < 1) ml = 1;
                    if (ml > 3) ml = 3;
                    fprintf(stderr, "[GPU-PRESENT] maximum frame latency %u (BW_GPU_FRAMELAT)\n", ml);
                }
                IDXGISwapChain2_SetMaximumFrameLatency(s2, ml);
                s_swap_wait = IDXGISwapChain2_GetFrameLatencyWaitableObject(s2);
                IDXGISwapChain2_Release(s2);
            }
        }
        fprintf(stderr, "[GPU-PRESENT] swap chain %ux%u (%s, %u buffers%s)%s%s\n", w, h,
                d.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD ? "flip" : "blt", d.BufferCount,
                s_swap_wait ? ", presents only when that can't stall" : "",
                tl ? "; refused first: " : "", tl ? tried : "");
        {
            /* Item 33: under display scaling, say so (nothing at 100%).
             * GetDpiForWindow is Windows 10 1607 and later: looked up, so
             * the exe still starts on older systems. */
            typedef UINT (WINAPI *DpiFn)(HWND);
            DpiFn dpi_fn = (DpiFn)(void *)GetProcAddress(GetModuleHandleA("user32.dll"), "GetDpiForWindow");
            UINT dpi = dpi_fn ? dpi_fn(hwnd) : 96;
            if (dpi && dpi != 96)
                fprintf(stderr, "[GPU-PRESENT] window DPI %u (%u%% display scaling): the swap chain is in"
                                " the panel's own pixels\n", dpi, (dpi * 100 + 48) / 96);
        }
        bw_fb_external_present(1);
    } else if (bw_hdr_remake()) {
        /* 0.2.5 item 97: the screen entered or left HDR mode (bw_hdr.c,
         * logged there): the swap chain is made again in the format that
         * suits it, as the resize-failure path does below. Nothing may hold
         * the old back buffer, bound or queued. */
        ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 0, NULL, NULL);
        ID3D11DeviceContext_Flush(s_ctx);
        swap_release();
        return swap_ensure();
    } else if (w != s_swap_w || h != s_swap_h) {
        HRESULT hr;
        if (s_swap_rtv) { ID3D11RenderTargetView_Release(s_swap_rtv); s_swap_rtv = NULL; }
        /* Item 29: nothing may still hold the back buffer, bound or queued */
        ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 0, NULL, NULL);
        ID3D11DeviceContext_Flush(s_ctx);
        hr = BW_GPU_FT("swapchain", E_OUTOFMEMORY,             /* item 29 test aid (call B1-5) */
                       IDXGISwapChain1_ResizeBuffers(s_swap, 0, w, h, DXGI_FORMAT_UNKNOWN, s_swap_flags));
        if (FAILED(hr)) {
            if (gpu_lost(hr, "ResizeBuffers"))          /* item 28 */
                return 0;
            /* once: a new swap chain at the new size; if that fails, GDI */
            fprintf(stderr, "[GPU-PRESENT] ResizeBuffers to %ux%u failed (0x%08lX); making a new swap chain\n",
                    w, h, (unsigned long)hr);
            swap_release();
            return swap_ensure();
        }
        s_swap_w = w;
        s_swap_h = h;
    }
    if (!s_swap_rtv) {
        ID3D11Texture2D *bb = NULL;
        HRESULT hr = IDXGISwapChain1_GetBuffer(s_swap, 0, &IID_ID3D11Texture2D, (void **)&bb);
        if (FAILED(hr)) {
            if (!gpu_lost(hr, "GetBuffer"))             /* item 28 */
                swap_give_up("the swap chain's GetBuffer", hr);
            return 0;
        }
        hr = ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)bb, NULL, &s_swap_rtv);
        ID3D11Texture2D_Release(bb);
        if (FAILED(hr)) {
            s_swap_rtv = NULL;
            swap_give_up("the back buffer's view", hr);
            return 0;
        }
    }
    return s_swap_rtv != NULL;
}

/* 0.2.5 item 97 test aid, BW_GPU_SHOT_FLIPS=<n>[,<n>...] (with RECOMP_FB_DUMP
 * as the prefix): the presented picture at those game flips (the first presented
 * one at or after each) of the plain path,
 * 32-bit BMP (8-bit swap chain) or float PFM in cd/m2 (scRGB). It shows the
 * plain picture is unchanged where it should be. Off by default. */
static void present_shot(void)
{
    static int init;
    static unsigned want[16], nw, count;
    const char *prefix = getenv("RECOMP_FB_DUMP");
    unsigned i;
    if (!init) {
        const char *e = getenv("BW_GPU_SHOT_FLIPS");
        init = 1;
        while (e && *e && nw < 16) {
            want[nw++] = (unsigned)strtoul(e, (char **)&e, 10);
            if (*e == ',') e++; else break;
        }
    }
    count++;
    if (!nw || !prefix || !*prefix || !s_swap_rtv)
        return;
    for (i = 0; i < nw; i++)
        if (want[i] && bwg_gen >= want[i]) {          /* the game's flip number, first presented one at or after it */
            want[i] = 0;
            ID3D11Resource *res = NULL;
            ID3D11Texture2D *stg = NULL;
            D3D11_TEXTURE2D_DESC td;
            D3D11_MAPPED_SUBRESOURCE m;
            char path[600];
            FILE *f;
            ID3D11View_GetResource((ID3D11View *)s_swap_rtv, &res);
            if (!res)
                return;
            ID3D11Texture2D_GetDesc((ID3D11Texture2D *)res, &td);
            td.Usage = D3D11_USAGE_STAGING;
            td.BindFlags = 0;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            td.MiscFlags = 0;
            if (SUCCEEDED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &stg))) {
                ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)stg, res);
                if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)stg, 0, D3D11_MAP_READ, 0, &m))) {
                    int fp16 = td.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
                    UINT y, x;
                    _snprintf(path, sizeof path, "%sflip_%05u.%s", prefix, bwg_gen, fp16 ? "pfm" : "bmp");
                    path[sizeof path - 1] = 0;
                    if ((f = fopen(path, "wb")) != NULL) {
                        if (fp16) {
                            fprintf(f, "PF\n%u %u\n-1.0\n", td.Width, td.Height);
                            for (y = td.Height; y-- > 0; ) {
                                const uint16_t *p = (const uint16_t *)((const uint8_t *)m.pData + (size_t)y * m.RowPitch);
                                for (x = 0; x < td.Width; x++) {
                                    int k;
                                    for (k = 0; k < 3; k++) {
                                        uint32_t h = p[x * 4 + k], s = (h >> 15) & 1, ex = (h >> 10) & 31, mm = h & 1023;
                                        float v = ex == 0 ? mm / 1024.0f / 16384.0f : ex == 31 ? 65504.0f
                                                : (1.0f + mm / 1024.0f) * ldexpf(1.0f, (int)ex - 15);
                                        v = (s ? -v : v) * 80.0f;
                                        fwrite(&v, 4, 1, f);
                                    }
                                }
                            }
                        } else {
                            uint8_t hd[54];
                            uint32_t size = 54 + td.Width * td.Height * 4;
                            int32_t hh = -(int32_t)td.Height;
                            memset(hd, 0, sizeof hd);
                            hd[0] = 'B'; hd[1] = 'M';
                            memcpy(hd + 2, &size, 4);
                            hd[10] = 54; hd[14] = 40;
                            memcpy(hd + 18, &td.Width, 4);
                            memcpy(hd + 22, &hh, 4);
                            hd[26] = 1; hd[28] = 32;
                            fwrite(hd, 1, 54, f);
                            for (y = 0; y < td.Height; y++)
                                fwrite((const uint8_t *)m.pData + (size_t)y * m.RowPitch, 4, td.Width, f);
                        }
                        fclose(f);
                        fprintf(stderr, "[GPU-PRESENT] shot %s (flip %u, present %u, %ux%u, %s)\n", path, bwg_gen, count,
                                td.Width, td.Height, fp16 ? "scRGB, cd/m2" : "8-bit");
                    }
                    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)stg, 0);
                }
                ID3D11Texture2D_Release(stg);
            }
            ID3D11Resource_Release(res);
        }
}

/* ---- the picture: the title's anti-aliasing resolve; shown-frame dumps ---- */

/* Blood Wake renders into a 2x-wide surface (1280x480, SURFACE_FORMAT AA 1)
 * and D3D_SwapAntialiased (0x00129360) resolves it into the 640x480 scan-out
 * buffer right after FLIP_STALL, with one full-screen triangle sampled with
 * the title's resolve filter (D3DTEXF_QUINCUNX here: CONVOLUTION_2D, kernel
 * 1). With one scan-out buffer the console shows that resolve from the next
 * vblank, so the picture of frame N is the resolve of the surface flipped at
 * FLIP_STALL N. This back end presents at FLIP_STALL, so it resolves that
 * surface itself, with the filter the title's own resolve last used (seen in
 * xdraw_prepare): the same picture at the same time. Before 2026-10-27 the
 * present filtered the 2x surface bilinearly (a plain 2:1 average across
 * x only), which is still what BW_GPU_CONV=0 shows. */
static const char s_resolve_hlsl[] =
"Texture2D t0 : register(t0);\n"
"SamplerState s0 : register(s0);\n"
"cbuffer R : register(b0) { float4 rt; float4 rk; };\n"
"struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
"float4 ps_main(V i) : SV_Target { float2 uv = i.uv + rt.zw; float3 c = 0;\n"
"  if (rk.x < 0.5) return float4(t0.Sample(s0, uv).rgb, 1.0);\n"
"  [unroll] for (int y = -1; y <= 1; y++) [unroll] for (int x = -1; x <= 1; x++)\n"
"    c += t0.Sample(s0, uv + float2(x, y) * rt.xy).rgb * ((2 - abs(x)) * (2 - abs(y)) / 16.0);\n"
"  return float4(c, 1.0); }\n";

static ID3D11PixelShader *s_resolve_ps;
static ID3D11Buffer *s_resolve_cb;
static ID3D11Texture2D *s_pic_tex, *s_pic_staging;
static ID3D11RenderTargetView *s_pic_rtv;
static ID3D11ShaderResourceView *s_pic_srv;
static UINT s_pic_w, s_pic_h, s_stg_w, s_stg_h;

static int resolve_ensure(UINT w, UINT h)
{
    if (!s_resolve_ps) {
        ID3DBlob *psb = NULL, *err = NULL;
        D3D11_BUFFER_DESC bd;
        if (FAILED(D3DCompile(s_resolve_hlsl, sizeof(s_resolve_hlsl) - 1, "bw_resolve", NULL, NULL,
                              "ps_main", "ps_5_0", 0, 0, &psb, &err))) {
            fprintf(stderr, "[GPU-PRESENT] resolve shader failed: %s\n",
                    err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
            if (err) ID3D10Blob_Release(err);
            return 0;
        }
        ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(psb),
                                       ID3D10Blob_GetBufferSize(psb), NULL, &s_resolve_ps);
        ID3D10Blob_Release(psb);
        memset(&bd, 0, sizeof(bd));
        bd.ByteWidth = 32;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_resolve_cb);
    }
    if (!s_resolve_ps || !s_resolve_cb)
        return 0;
    if (!s_pic_tex || s_pic_w != w || s_pic_h != h) {
        D3D11_TEXTURE2D_DESC td;
        if (s_pic_srv) ID3D11ShaderResourceView_Release(s_pic_srv);
        if (s_pic_rtv) ID3D11RenderTargetView_Release(s_pic_rtv);
        if (s_pic_tex) ID3D11Texture2D_Release(s_pic_tex);
        s_pic_srv = NULL; s_pic_rtv = NULL; s_pic_tex = NULL;
        memset(&td, 0, sizeof(td));
        td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_pic_tex)))
            return 0;
        ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)s_pic_tex, NULL, &s_pic_rtv);
        ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)s_pic_tex, NULL, &s_pic_srv);
        s_pic_w = w; s_pic_h = h;
    }
    return s_pic_rtv && s_pic_srv;
}

/* BW_GPU_SHOWN_DUMP=<prefix>: every BW_GPU_SHOWN_EVERY-th flip (default 30),
 * the picture as the window shows it before scaling -- the resolve for a
 * 2x-wide surface, else the surface -- through the DAC gamma ramp, as
 * <prefix><flip>.bmp. BW_GPU_SHOWN_PAIRS=1 dumps the flip after it too (for
 * BW_GPU_AB's adjacent frames). BW_GPU_SHOWN_OLD=1 also writes old_<flip>.bmp:
 * the same frame as the port presented it before 2026-10-27 (a plain
 * bilinear 2:1 of the 2x-wide surface, no gamma ramp), an exact same-frame
 * A/B for the display fixes. Test aid; works with the display asleep. */
static const char *shown_due(void)
{
    static const char *prefix = (const char *)-1;
    static unsigned every, pairs;
    if (prefix == (const char *)-1) {
        const char *e = getenv("BW_GPU_SHOWN_EVERY"), *p = getenv("BW_GPU_SHOWN_PAIRS");
        prefix = getenv("BW_GPU_SHOWN_DUMP");
        every = e && atoi(e) > 0 ? (unsigned)atoi(e) : 30;
        pairs = p && p[0] == '1';
    }
    if (!prefix || !*prefix)
        return NULL;
    return (bwg_gen % every) == 0 || (pairs && (bwg_gen % every) == 1) ? prefix : NULL;
}

static int shown_old(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("BW_GPU_SHOWN_OLD");
        v = e && e[0] == '1';
    }
    return v;
}

static void shown_write(ID3D11Texture2D *tex, UINT w, UINT h, const char *prefix, int gamma)
{
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_BOX box;
    char path[512];
    uint32_t *row;
    FILE *f;
    UINT y;
    D3D11_TEXTURE2D_DESC sd;
    static DXGI_FORMAT stg_f;
    int is565;
    if (!prefix || !tex)
        return;
    /* item 35: a 16-bit surface used to be copied into a 32-bit staging
     * texture (a copy D3D11 refuses) and read as 32-bit; it is converted
     * now, and any other format is skipped with one line. */
    ID3D11Texture2D_GetDesc(tex, &sd);
    is565 = sd.Format == DXGI_FORMAT_B5G6R5_UNORM;
    if (!is565 && sd.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
        static int told;
        if (!told++)
            fprintf(stderr, "[GPU-D3D11] BW_GPU_SHOWN_DUMP: a surface in format %u isn't dumped\n",
                    (unsigned)sd.Format);
        return;
    }
    if (w > sd.Width) w = sd.Width;
    if (h > sd.Height) h = sd.Height;
    if (!s_pic_staging || s_stg_w != w || s_stg_h != h || stg_f != sd.Format) {
        D3D11_TEXTURE2D_DESC td;
        if (s_pic_staging) ID3D11Texture2D_Release(s_pic_staging);
        s_pic_staging = NULL;
        memset(&td, 0, sizeof(td));
        td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = sd.Format;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_pic_staging)))
            return;
        s_stg_w = w; s_stg_h = h; stg_f = sd.Format;
    }
    box.left = 0; box.top = 0; box.front = 0; box.right = w; box.bottom = h; box.back = 1;
    ID3D11DeviceContext_CopySubresourceRegion(s_ctx, (ID3D11Resource *)s_pic_staging, 0, 0, 0, 0,
                                              (ID3D11Resource *)tex, 0, &box);
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_pic_staging, 0, D3D11_MAP_READ, 0, &m)))
        return;
    snprintf(path, sizeof path, "%s%06u.bmp", prefix, (unsigned)bwg_gen);
    f = fopen(path, "wb");
    row = (uint32_t *)malloc((size_t)w * 4);
    if (f && row) {
        uint8_t hd[54];
        uint32_t rowb = w * 3 + ((4 - (w * 3) % 4) % 4), size = 54 + rowb * h, x;
        uint8_t *line = (uint8_t *)calloc(rowb, 1);
        memset(hd, 0, sizeof hd);
        hd[0] = 'B'; hd[1] = 'M';
        memcpy(hd + 2, &size, 4);
        hd[10] = 54; hd[14] = 40;
        memcpy(hd + 18, &w, 4);
        { int32_t hh = -(int32_t)h; memcpy(hd + 22, &hh, 4); }
        hd[26] = 1; hd[28] = 24;
        fwrite(hd, 1, 54, f);
        for (y = 0; line && y < h; y++) {
            if (is565) {
                const uint16_t *q = (const uint16_t *)((const uint8_t *)m.pData + (size_t)y * m.RowPitch);
                for (x = 0; x < w; x++) {
                    uint32_t c = q[x], r5 = c >> 11, g6 = (c >> 5) & 63, b5 = c & 31;
                    row[x] = ((r5 << 3 | r5 >> 2) << 16) | ((g6 << 2 | g6 >> 4) << 8) | (b5 << 3 | b5 >> 2);
                }
            } else {
                memcpy(row, (const uint8_t *)m.pData + (size_t)y * m.RowPitch, (size_t)w * 4);
            }
            if (gamma)
                bw_gamma_apply_xrgb(row, w);
            for (x = 0; x < w; x++) {
                line[x * 3 + 0] = (uint8_t)row[x];
                line[x * 3 + 1] = (uint8_t)(row[x] >> 8);
                line[x * 3 + 2] = (uint8_t)(row[x] >> 16);
            }
            fwrite(line, 1, rowb, f);
        }
        free(line);
    }
    if (f) fclose(f);
    free(row);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_pic_staging, 0);
}

/* Resolve the 2x-wide surface into s_pic with `kernel` (0: one bilinear tap
 * per pixel, the plain 2:1 average). */
static void resolve_pass(BwSurf *sf, int kernel)
{
    D3D11_VIEWPORT vp;
    D3D11_MAPPED_SUBRESOURCE m;
    float cb[8];
    g_bw_ctx_epoch++;                   /* I3: it sets state of its own */
    cb[0] = 1.0f / sf->w; cb[1] = 1.0f / sf->h;     /* one texel */
    cb[2] = 0.0f; cb[3] = 0.0f;                     /* offset (normalized) */
    cb[4] = (float)kernel; cb[5] = cb[6] = cb[7] = 0.0f;
    if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_resolve_cb, 0,
                                          D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, cb, sizeof cb);
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_resolve_cb, 0);
    }
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &s_pic_rtv, NULL);
    vp.TopLeftX = 0; vp.TopLeftY = 0; vp.Width = (float)s_pic_w; vp.Height = (float)s_pic_h;
    vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
    ID3D11DeviceContext_IASetInputLayout(s_ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(s_ctx, s_blit_vs, NULL, 0);
    ID3D11DeviceContext_GSSetShader(s_ctx, NULL, NULL, 0);
    ID3D11DeviceContext_PSSetShader(s_ctx, s_resolve_ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 1, &s_resolve_cb);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &sf->srv);
    ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 1, &s_blit_ss);
    ID3D11DeviceContext_RSSetState(s_ctx, s_blit_rs);
    ID3D11DeviceContext_OMSetBlendState(s_ctx, s_blit_bs, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, s_blit_ds, 0);
    ID3D11DeviceContext_Draw(s_ctx, 3, 0);
    {
        ID3D11ShaderResourceView *none = NULL;
        ID3D11RenderTargetView *nort = NULL;
        ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &none);
        ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &nort, NULL);
    }
}

/* The picture to present for the flipped surface: for a 2x-wide one, its
 * resolve into s_pic (the title's filter; plain bilinear with BW_GPU_CONV=0
 * or before the title's first resolve), else the surface. Leaves the render
 * targets changed; the caller rebinds. */
static ID3D11ShaderResourceView *shown_picture(BwSurf *sf)
{
    const char *due = shown_due();
    UINT w = sf->w / 2, h = sf->h;
    int kernel;
    if (sf->aa_sx != 2 || !resolve_ensure(w, h)) {
        if (due && shown_old())
            shown_write(sf->tex, sf->w, sf->h, "old_", 0);
        shown_write(due ? sf->tex : NULL, sf->w, sf->h, due, 1);
        return sf->srv;
    }
    if (due && shown_old()) {                       /* the same frame, presented as before */
        resolve_pass(sf, 0);
        shown_write(s_pic_tex, w, h, "old_", 0);
    }
    kernel = bwg_resolve_seen ? bwg_conv_kernel(bwg_resolve_filter) : 0;
    resolve_pass(sf, kernel);
    shown_write(due ? s_pic_tex : NULL, w, h, due, 1);
    return bwg_use_conv() ? s_pic_srv : sf->srv;
}

/* P2b: what the screen does with the presented frames (BW_GPU_STATS test
 * aid; reads only). After each present the swap chain's own statistics say
 * which present was last put on screen and at which refresh (SyncQPCTime):
 * the time from our Present call to that refresh is the present-to-screen
 * latency, and the refresh count's step says how many refreshes the screen
 * made meanwhile (with variable refresh, one per frame shown). Not every
 * mode reports them (a composed window may not): the line then says so. */
/* (The screen wait and the present side's wait flags for the high frame
 * rate, g_bw_present_waiting and the rest, are the core's: bwg_pace.c.) */
#define PST_N 256
static LARGE_INTEGER s_pst_t[PST_N];
static UINT s_pst_last_pc, s_pst_last_src;
static unsigned s_pst_ok, s_pst_fail, s_pst_shown, s_pst_refr;
static double s_pst_lat_sum, s_pst_lat_max, s_pst_lat_min;
static HRESULT s_pst_hr;

static void pstat_after_present(void)
{
    UINT pc = 0;
    DXGI_FRAME_STATISTICS fs;
    HRESULT hr;
    if (s_stats <= 0 || !s_swap)
        return;
    if (SUCCEEDED(IDXGISwapChain1_GetLastPresentCount(s_swap, &pc)))
        QueryPerformanceCounter(&s_pst_t[pc % PST_N]);
    memset(&fs, 0, sizeof fs);
    hr = IDXGISwapChain1_GetFrameStatistics(s_swap, &fs);
    if (FAILED(hr)) {
        s_pst_fail++;
        s_pst_hr = hr;
        return;
    }
    s_pst_ok++;
    if (fs.PresentCount && fs.PresentCount != s_pst_last_pc && pc - fs.PresentCount < PST_N) {
        LARGE_INTEGER f;
        double lat;
        QueryPerformanceFrequency(&f);
        lat = (double)(fs.SyncQPCTime.QuadPart - s_pst_t[fs.PresentCount % PST_N].QuadPart) * 1000.0
              / (double)f.QuadPart;
        if (lat > -1.0 && lat < 1000.0) {
            s_pst_lat_sum += lat;
            if (!s_pst_shown || lat < s_pst_lat_min) s_pst_lat_min = lat;
            if (lat > s_pst_lat_max) s_pst_lat_max = lat;
            s_pst_shown++;
        }
        if (s_pst_last_src)
            s_pst_refr += fs.SyncRefreshCount - s_pst_last_src;
        s_pst_last_src = fs.SyncRefreshCount;
        s_pst_last_pc = fs.PresentCount;
    }
}

static void pstat_line(void)
{
    if (!s_pst_ok && !s_pst_fail)
        return;
    if (s_pst_shown)
        fprintf(stderr, "[GPU-PRESENT] on screen: %u presents seen shown, present to screen %.2f/%.2f/%.2f ms,"
                        " %u refreshes meanwhile; statistics %u ok %u failed\n",
                s_pst_shown, s_pst_lat_min, s_pst_lat_sum / s_pst_shown, s_pst_lat_max, s_pst_refr,
                s_pst_ok, s_pst_fail);
    else
        fprintf(stderr, "[GPU-PRESENT] no frame statistics (%u ok, %u failed, last 0x%08lX)\n",
                s_pst_ok, s_pst_fail, (unsigned long)s_pst_hr);
    s_pst_ok = s_pst_fail = s_pst_shown = s_pst_refr = 0;
    s_pst_lat_sum = s_pst_lat_max = s_pst_lat_min = 0.0;
}

/* Item 31: what the visual suite's present returned; 1 = it drew nothing,
 * show the plain picture. One line when that starts and one when the suite
 * draws again (the first five of each). */
static int vfx_present_note(int drew)
{
    static int failing;
    static unsigned n[2], frames;
    if (!drew) {
        frames++;
        if (!failing && ++n[0] <= 5)
            fprintf(stderr, "[VFX] the visual suite couldn't draw frame %u: the plain picture is shown"
                            " until it can (visual effects off meanwhile)\n", (unsigned)bwg_gen);
        failing = 1;
        return 1;
    }
    if (failing && ++n[1] <= 5)
        fprintf(stderr, "[VFX] the visual suite draws again (frame %u, after %u plain frames)\n",
                (unsigned)bwg_gen, frames);
    failing = 0;
    frames = 0;
    return 0;
}

/* The finished surface, letterboxed into the window and presented. Returns 1
 * if it did. The executor's targets are rebound afterwards. */
static int gpu_present(BwSurf *sf)
{
    static const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    g_bw_ctx_epoch++;                   /* I3: the present sets state of its own */
    ID3D11ShaderResourceView *none = NULL, *pic;
    D3D11_VIEWPORT vp;
    float w, h;
    HRESULT hr;
    int waited = 0;                     /* item 30: the waitable said a buffer is free */

    if (s_present_mode < 0) {
        const char *e = getenv("BW_GPU_PRESENT");
        s_present_mode = !(e && e[0] == '0');
    }
    /* Never wait for the PC's display (a port fix, every edition).
     *
     * Seen 2026-10-27 ~01:55 with the monitor asleep (Win32_DesktopMonitor
     * Availability 8): the whole game ran at 3.8 fps (runs
     * 20261027-015648-r6-gpu-d7base30b, and -d7dayexe with the lead's build;
     * with BW_GPU_PRESENT=0, 30.0 fps: -015751-r6-gpu-d7nopres). Present
     * itself returned at once (0.1 ms); the readback's Map waited ~250 ms per
     * frame (run -020453-r6-gpu-d7tim), because the GPU queue stalled drawing
     * into a back buffer the compositor still held, and the readback waits
     * for the GPU. The console never waits on a TV, and nothing in the title
     * depends on the host showing a frame. So the swap chain has a
     * frame-latency waitable object, and a frame is drawn into it and
     * presented only when that object says a buffer is free; otherwise this
     * picture is dropped (counted in BW_GPU_STATS), the guest's frame is
     * unchanged, and the title's own pacing (the flip hold) is untouched.
     * Present also never blocks (DO_NOT_WAIT). With the display on and the
     * title at 30 or 60 fps a buffer is always free, so nothing changes.
     * BW_GPU_PRESENT_WAIT=1 restores the blocking present (no waitable,
     * two buffers, as before). */
    if (s_present_wait_mode < 0) {
        const char *e = getenv("BW_GPU_PRESENT_WAIT");
        s_present_wait_mode = e && e[0] == '1';
        if (s_present_wait_mode)
            fprintf(stderr, "[GPU-PRESENT] blocking present (BW_GPU_PRESENT_WAIT=1)\n");
    }
    if (bw_vfx_active())
        bw_vfx_set_scene(vfx_scene_for(sf));        /* VFX (D9): the scene before the HUD */
    if (!s_present_mode && !s_swap_dead && sf && sf->srv && bw_vfx_active()) {
        /* VFX (D9): nothing presents, but the suite still runs into its
         * off-screen output, so it can be measured and photographed. */
        surf_resolve(sf);
        bw_vfx_set_picture_aspect(4.0f / 3.0f * sf->wsk);     /* widescreen (D5) */
        bw_vfx_set_gamma_lut(gamma_lut());          /* the title's DAC ramp (TK-50) */
        bw_vfx_present(sf->srv, sf->pw, sf->ph, NULL, NULL);
        if (bwg_cur)
            bind_targets(bwg_cur, bwg_cur_depth);
        return 0;
    }
    if (!sf || !sf->srv || !blit_ensure())
        return 0;
    /* Original: the title's resolve and the shown-frame dumps; Remastered's
     * visual suite (D9) presents the surface itself, below. */
    pic = bw_vfx_active() ? sf->srv : shown_picture(sf);
    if (!s_present_mode || !swap_ensure()) {
        if (bwg_cur)
            bind_targets(bwg_cur, bwg_cur_depth);
        return 0;
    }
    bw_hdr_follow(s_swap_w, s_swap_h);              /* item 97: the screen, every edition */
    if (s_swap_wait) {
        /* P2b: the screen wait (the core's policy, bwg_screen_wait) */
        if (!bwg_screen_wait(s_swap_wait)) {
            s_st_dropped++;
            if (bwg_cur)
                bind_targets(bwg_cur, bwg_cur_depth);
            return 0;
        }
        waited = 1;
    }
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &s_swap_rtv, NULL);
    ID3D11DeviceContext_ClearRenderTargetView(s_ctx, s_swap_rtv, black);
    /* A 4:3 picture, as the console sends to the TV: the flipped surface is
     * often the 2x-wide anti-aliased 1280x480 one, which the GPU filters down.
     * The rectangle: the core's (bwg_letterbox). */
    bwg_letterbox(sf->wsk, s_swap_w, s_swap_h, &vp.TopLeftX, &vp.TopLeftY, &w, &h);
    vp.Width = w; vp.Height = h;
    vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
    /* VFX (D9): the render scale follows the picture's size (no-op in
     * Original); the suite draws the frame through its effects. */
    bw_vfx_set_output((unsigned)(w + 0.5f), (unsigned)(h + 0.5f));
    if (bw_vfx_active()) {
        surf_resolve(sf);
        bw_vfx_set_gamma_lut(gamma_lut());          /* the title's DAC ramp (TK-50) */
        if (vfx_present_note(bw_vfx_present(sf->srv, sf->pw, sf->ph, s_swap_rtv, &vp))) {
            /* Item 31: the suite drew nothing (a shader or target it needs
             * failed): the plain picture below instead of a black screen.
             * The suite may have changed the targets: the back buffer again,
             * cleared, as above. */
            ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &s_swap_rtv, NULL);
            ID3D11DeviceContext_ClearRenderTargetView(s_ctx, s_swap_rtv, black);
            goto plain;
        }
    } else {
    plain:
    ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
    ID3D11DeviceContext_IASetInputLayout(s_ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(s_ctx, s_blit_vs, NULL, 0);
    ID3D11DeviceContext_GSSetShader(s_ctx, NULL, NULL, 0);
    if (bw_hdr_scrgb() && blit_hdr_ensure()) {
        /* item 97: the screen is in HDR mode and the swap chain is scRGB */
        float hk[4];
        bw_hdr_direct_consts(hk, 1);
        ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)s_blit_hdr_cb, 0, NULL, hk, 0, 0);
        ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 1, &s_blit_hdr_cb);
        ID3D11DeviceContext_PSSetShader(s_ctx, s_blit_hdr_ps, NULL, 0);
    } else
    ID3D11DeviceContext_PSSetShader(s_ctx, s_blit_ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &pic);
    {
        ID3D11ShaderResourceView *lut = gamma_lut();     /* the DAC ramp, t1 */
        ID3D11DeviceContext_PSSetShaderResources(s_ctx, 1, 1, &lut);
    }
    ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 1, &s_blit_ss);
    ID3D11DeviceContext_RSSetState(s_ctx, s_blit_rs);
    ID3D11DeviceContext_OMSetBlendState(s_ctx, s_blit_bs, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, s_blit_ds, 0);
    ID3D11DeviceContext_Draw(s_ctx, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &none);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 1, 1, &none);
    present_shot();                                 /* item 97 test aid */
    if (bw_hdr_scrgb() && s_blit_hdr_ok > 0) {
        ID3D11Buffer *nocb = NULL;                  /* item 97: the HDR blit's constants off again */
        ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 1, &nocb);
    }
    }
    {
        /* Item 30: once the waitable has said a buffer is free, present
         * without DO_NOT_WAIT. With it, Present could still answer "still
         * drawing" (a resize, a busy compositor): that frame was dropped
         * but the wait had taken the waitable's count, and after two such
         * frames every wait timed out and every frame dropped for good
         * (seen in a Remastered edge-drag, run -i29drag: "presented 0
         * dropped 9" a second from then on). */
        UINT pf = (s_present_wait_mode || waited) ? 0 : DXGI_PRESENT_DO_NOT_WAIT;
        if (s_cfg_sync == 0 && (s_swap_flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING))
            pf |= DXGI_PRESENT_ALLOW_TEARING;
        /* item 25 test aid: BW_GPU_FAILTEST=present:<n> reports the device removed */
        hr = BW_GPU_FT("present", DXGI_ERROR_DEVICE_REMOVED, IDXGISwapChain1_Present(s_swap, s_cfg_sync, pf));
    }
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
        s_st_dropped++;
        hr = S_OK;
    } else if (SUCCEEDED(hr)) {
        s_st_presented++;
        pstat_after_present();           /* P2b: when presented frames reach the screen */
        if (bwg_gate_mode())
            gate_screen((uint32_t)g_bw_flips_done + 1);   /* gfx-lat11: the horizon */
    }
    if (FAILED(hr) && !gpu_lost(hr, "Present")) {      /* item 28: removed or reset ends the boot */
        static int shown;
        if (shown++ < 3)
            fprintf(stderr, "[GPU-PRESENT] Present failed 0x%08lX\n", (unsigned long)hr);
    }
    /* Back to what the executor had bound. */
    if (bwg_cur)
        bind_targets(bwg_cur, bwg_cur_depth);
    return SUCCEEDED(hr);
}

/* ---- the latency mode's GPU side (lane gfx-lat11, BW_HIFPS_LATMODE) -------
 * docs/gfx/arch.md 3.3, 3.8 and 4: in the latency mode (Remastered's high
 * frame rate, opt-in) the GPU limit is held at one gate before the game pass
 * (bw_hifps_hooks.c, jit_wait; the policy is the core's, bwg_gate.c), so the
 * walk never waits for the GPU. What D3D11 adds for it, per flip, at the end
 * of the flip (every wait in it, the pacing, the screen and the present,
 * unchanged):
 *   - a fence mark with the flip's number and the frame submitted at once
 *     (Flush): a watcher thread waits on the mark's event and tells the core
 *     when the GPU finished the flip. The device is single-threaded, so the
 *     watcher touches no D3D11 object, only its Win32 events (as lat-m's
 *     latency log does);
 *   - two GPU timestamps around the frame (TIMESTAMP_DISJOINT, read a frame
 *     or two later without flushing): the GPU's own time per frame, and the
 *     done watcher's lag (BW_HIFPS_LATMODE_TS=0 leaves them out);
 *   - on screen, DXGI's frame statistics: when each present reached the
 *     screen (the measured horizon).
 * The device check of the GPU limit's wait (item 28, at most every 50 ms
 * while the GPU has frames to finish) moves here with it, so a lost device
 * still ends the boot (BW_GPU_FAILTEST=gpuwait:<n>); BW_TEST_LATSLOW=<ms>
 * makes the watcher see a GPU that slow, as it does the limit's wait.
 * Parity mode (the default, and Original always) never reaches any of it. */
#define GT_R 64
static int s_gt_on = -1;                /* -1 not set up yet, 0 off (failed), 1 live */
static ID3D11Fence *s_gt_fence;
static ID3D11DeviceContext4 *s_gt_ctx4;
static ID3D11Device *s_gt_dev;
static HANDLE s_gt_ev[GT_R], s_gt_kick;
static uint32_t s_gt_flip[GT_R];
static LONGLONG s_gt_sim[GT_R], s_gt_sim_last;   /* BW_TEST_LATSLOW, QPC ticks */
static unsigned s_gt_slow_ms;
static volatile LONG s_gt_head, s_gt_tail;       /* head: marked (NV2A), tail: seen (watcher) */
static unsigned long s_gt_skipped;
static double s_gt_pres_ms;             /* this flip's present call returned, qpc ms */
static double s_gt_dev_check;
#define TS_R 8
static ID3D11Query *s_ts_dj[TS_R], *s_ts_b[TS_R], *s_ts_e[TS_R];
static uint32_t s_ts_flip[TS_R];
static unsigned char s_ts_st[TS_R];     /* 0 free, 1 begun, 2 ended (its data to read) */
static unsigned s_ts_cur = ~0u;         /* the slot of the frame in progress */
static int s_ts_off;
static uint32_t s_gt_pc_flip[256];      /* DXGI present count -> flip */
static UINT s_gt_last_spc;

static DWORD WINAPI gt_watch(LPVOID unused)
{
    (void)unused;
    for (;;) {
        LONG tail = s_gt_tail;
        unsigned i;
        if (tail == s_gt_head) {
            WaitForSingleObject(s_gt_kick, 100);
            continue;
        }
        i = (unsigned)tail % GT_R;
        if (WaitForSingleObject(s_gt_ev[i], 100) != WAIT_OBJECT_0)
            continue;                   /* a slow frame: wait on (the gate has its own guard) */
        while (s_gt_sim[i]) {           /* BW_TEST_LATSLOW, _GPUSTALL (test aids): not finished before then */
            LARGE_INTEGER c;
            QueryPerformanceCounter(&c);
            if (c.QuadPart >= s_gt_sim[i])
                break;
            Sleep(1);
        }
        bwg_gate_gpu_done(s_gt_flip[i], bwg_qpc_ms());
        InterlockedExchange(&s_gt_tail, tail + 1);
    }
}

/* 1 when the gate's GPU side is live on this device. */
static int gt_setup(void)
{
    ID3D11Device5 *d5 = NULL;
    unsigned i;
    const char *e;
    if (s_gt_on >= 0) {
        if (s_gt_on == 1 && s_gt_dev != s_dev) {
            /* a new device: the old fence's events never come (item 28 ends
             * the boot on a lost device, so this is a safety net only) */
            s_gt_on = 0;
            bwg_gate_live(0);
            fprintf(stderr, "[GPU-GATE] a new graphics device: the gate's GPU limit is off for this boot\n");
        }
        return s_gt_on;
    }
    if (!s_dev || !s_ctx)
        return 0;                       /* not yet: asked again at the next flip */
    s_gt_on = 0;
    s_gt_dev = s_dev;
    if (FAILED(ID3D11Device_QueryInterface(s_dev, &IID_ID3D11Device5, (void **)&d5))
        || FAILED(ID3D11Device5_CreateFence(d5, 0, D3D11_FENCE_FLAG_NONE, &IID_ID3D11Fence,
                                            (void **)&s_gt_fence))
        || FAILED(ID3D11DeviceContext_QueryInterface(s_ctx, &IID_ID3D11DeviceContext4, (void **)&s_gt_ctx4))) {
        fprintf(stderr, "[GPU-GATE] no D3D11 fence on this system: the latency mode keeps the GPU limit in"
                        " the flip (parity's wait)\n");
        if (d5)
            ID3D11Device5_Release(d5);
        return 0;
    }
    ID3D11Device5_Release(d5);
    for (i = 0; i < GT_R; i++)
        if (!(s_gt_ev[i] = CreateEventW(NULL, FALSE, FALSE, NULL)))
            return 0;
    s_gt_kick = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!s_gt_kick)
        return 0;
    {
        HANDLE th = CreateThread(NULL, 0, gt_watch, NULL, 0, NULL);
        if (!th)
            return 0;
        SetThreadPriority(th, THREAD_PRIORITY_HIGHEST);
        CloseHandle(th);
    }
    e = getenv("BW_TEST_LATSLOW");
    s_gt_slow_ms = e && *e ? (unsigned)atoi(e) : 0u;
    e = getenv("BW_HIFPS_LATMODE_TS");
    s_ts_off = e && e[0] == '0';
    s_gt_on = 1;
    bwg_gate_live(1);
    fprintf(stderr, "[GPU-GATE] the GPU's progress is watched (a fence per flip, each frame submitted at its"
                    " flip)%s%s\n", s_ts_off ? "; no GPU timestamps (BW_HIFPS_LATMODE_TS=0)" : "; GPU timestamps on",
            s_gt_slow_ms ? "; BW_TEST_LATSLOW: as if the GPU took that long per frame (test aid)" : "");
    return 1;
}

static int ts_make(unsigned i)
{
    D3D11_QUERY_DESC qd;
    qd.MiscFlags = 0;
    qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
    if (!s_ts_dj[i] && FAILED(ID3D11Device_CreateQuery(s_dev, &qd, &s_ts_dj[i])))
        return 0;
    qd.Query = D3D11_QUERY_TIMESTAMP;
    if (!s_ts_b[i] && FAILED(ID3D11Device_CreateQuery(s_dev, &qd, &s_ts_b[i])))
        return 0;
    if (!s_ts_e[i] && FAILED(ID3D11Device_CreateQuery(s_dev, &qd, &s_ts_e[i])))
        return 0;
    return 1;
}

/* The frame `flip` starts on the GPU here (after the flip before it). */
static void ts_begin(uint32_t flip)
{
    unsigned i = flip % TS_R;
    s_ts_cur = ~0u;
    if (s_ts_off || s_ts_st[i])
        return;                         /* that slot's data not read yet: this frame goes without */
    if (!ts_make(i)) {
        s_ts_off = 1;
        fprintf(stderr, "[GPU-GATE] GPU timestamp queries couldn't be made: the GPU's time per frame comes"
                        " from the done events\n");
        return;
    }
    ID3D11DeviceContext_Begin(s_ctx, (ID3D11Asynchronous *)s_ts_dj[i]);
    ID3D11DeviceContext_End(s_ctx, (ID3D11Asynchronous *)s_ts_b[i]);
    s_ts_flip[i] = flip;
    s_ts_st[i] = 1;
    s_ts_cur = i;
}

static void ts_end(uint32_t flip)
{
    unsigned i = s_ts_cur;
    if (i >= TS_R || s_ts_flip[i] != flip || s_ts_st[i] != 1)
        return;
    ID3D11DeviceContext_End(s_ctx, (ID3D11Asynchronous *)s_ts_e[i]);
    ID3D11DeviceContext_End(s_ctx, (ID3D11Asynchronous *)s_ts_dj[i]);
    s_ts_st[i] = 2;
    s_ts_cur = ~0u;
}

/* Hand the core every finished frame's timestamps, oldest first. */
static void ts_read(void)
{
    for (;;) {
        unsigned i, k = TS_R;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
        UINT64 b = 0, e = 0;
        HRESULT hr;
        for (i = 0; i < TS_R; i++)
            if (s_ts_st[i] == 2 && (k == TS_R || s_ts_flip[i] - s_ts_flip[k] > 0x80000000u))
                k = i;
        if (k == TS_R)
            return;
        hr = ID3D11DeviceContext_GetData(s_ctx, (ID3D11Asynchronous *)s_ts_dj[k], &dj, sizeof dj,
                                         D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr == S_FALSE)
            return;                     /* not yet: the next flip looks again */
        if (hr == S_OK)
            hr = ID3D11DeviceContext_GetData(s_ctx, (ID3D11Asynchronous *)s_ts_b[k], &b, sizeof b,
                                             D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr == S_OK)
            hr = ID3D11DeviceContext_GetData(s_ctx, (ID3D11Asynchronous *)s_ts_e[k], &e, sizeof e,
                                             D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr == S_FALSE)
            return;
        s_ts_st[k] = 0;
        if (hr == S_OK && !dj.Disjoint && dj.Frequency)
            bwg_gate_gpu_times(s_ts_flip[k], (double)b * 1000.0 / (double)dj.Frequency,
                               (double)e * 1000.0 / (double)dj.Frequency);
    }
}

/* On screen: when DXGI says each present reached the screen (the horizon). */
static void gate_screen(uint32_t flip)
{
    UINT pc = 0;
    DXGI_FRAME_STATISTICS fs;
    if (!s_swap || s_gt_on != 1)
        return;
    if (SUCCEEDED(IDXGISwapChain1_GetLastPresentCount(s_swap, &pc)))
        s_gt_pc_flip[pc % 256u] = flip;
    memset(&fs, 0, sizeof fs);
    if (SUCCEEDED(IDXGISwapChain1_GetFrameStatistics(s_swap, &fs)) && fs.PresentCount
            && fs.PresentCount != s_gt_last_spc && pc - fs.PresentCount < 256u) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        s_gt_last_spc = fs.PresentCount;
        bwg_gate_screen(s_gt_pc_flip[fs.PresentCount % 256u],
                        (double)fs.SyncQPCTime.QuadPart * 1000.0 / (double)f.QuadPart);
    }
}

/* At the end of every flip, in the latency mode (bw_flip). */
static void gate_flip(uint32_t flip)
{
    double pres, now;
    if (gt_setup() != 1)
        return;
    pres = s_gt_pres_ms > 0.0 ? s_gt_pres_ms : bwg_qpc_ms();
    s_gt_pres_ms = 0.0;
    ts_end(flip);
    if (s_gt_head - s_gt_tail >= GT_R - 1) {
        s_gt_skipped++;                 /* the watcher is behind: a later mark covers this flip */
        bwg_gate_submitted(flip, pres);
    } else {
        unsigned i = (unsigned)s_gt_head % GT_R;
        if (FAILED(ID3D11DeviceContext4_Signal(s_gt_ctx4, s_gt_fence, (UINT64)flip))
            || FAILED(ID3D11Fence_SetEventOnCompletion(s_gt_fence, (UINT64)flip, s_gt_ev[i]))) {
            s_gt_on = 0;
            bwg_gate_live(0);
            fprintf(stderr, "[GPU-GATE] the fence mark failed (flip %u): the latency mode keeps the GPU limit"
                            " in the flip from now on\n", (unsigned)flip);
            return;
        }
        s_gt_flip[i] = flip;
        {
            /* BW_TEST_LATSLOW for the whole run, or BW_TEST_GPUSTALL's stretch
             * (the core's): a GPU that slow, one frame after another */
            double st = bwg_gate_test_stall_ms();
            unsigned slow = s_gt_slow_ms ? s_gt_slow_ms : (unsigned)(st + 0.5);
            if (slow) {
                LARGE_INTEGER c, f;
                QueryPerformanceCounter(&c);
                QueryPerformanceFrequency(&f);
                s_gt_sim_last = (s_gt_sim_last > c.QuadPart ? s_gt_sim_last : c.QuadPart)
                                + f.QuadPart * (LONGLONG)slow / 1000;
                s_gt_sim[i] = s_gt_sim_last;
            } else {
                s_gt_sim[i] = 0;
            }
        }
        bwg_gate_submitted(flip, pres); /* before the watcher can see the mark */
        InterlockedIncrement(&s_gt_head);
        SetEvent(s_gt_kick);
    }
    ID3D11DeviceContext_Flush(s_ctx);  /* the frame and its mark go to the GPU now */
    ts_read();
    ts_begin(flip + 1);
    now = bwg_qpc_ms();
    if (bwg_gate_unfinished() && now - s_gt_dev_check >= 50.0) {
        /* item 28: a lost device never finishes its marks; ask it (as the
         * limit's wait does every 50 ms) */
        HRESULT why = BW_GPU_FT("gpuwait", DXGI_ERROR_DEVICE_REMOVED, ID3D11Device_GetDeviceRemovedReason(s_dev));
        s_gt_dev_check = now;
        if (FAILED(why))
            gpu_lost(DXGI_ERROR_DEVICE_REMOVED, "the latency gate's device check");
    }
}

/* BW_GPU_FLIPFLUSH=1 (test aid, a Remastered high-frame-rate boot, never in
 * a preset): parity mode with the frame submitted at every flip, to tell how
 * much of the measured present -> GPU done is the driver holding the frame
 * back (off screen nothing else submits it). */
static int flip_flush_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("BW_GPU_FLIPFLUSH");
        on = e && e[0] == '1';
        if (on)
            fprintf(stderr, "[GPU-GATE] every frame submitted at its flip (BW_GPU_FLIPFLUSH=1, test aid)\n");
    }
    return on;
}

/* P2b: how many flipped frames the GPU still has to finish, and a limit on
 * it (BW_GPU_LATENCY).
 *
 * Nothing in the present path waits for the GPU: the swap chain's waitable
 * is polled with a zero timeout and Present never blocks (D7's fix for the
 * sleeping display), and off screen (BW_GPU_PRESENT=0) nothing presents at
 * all. So when the GPU is the slower side, the NV2A thread can queue frame
 * after frame of draws until the driver runs out of something, and then
 * stalls for the whole backlog at once at an unrelated call. An event query
 * at each flip tells when the GPU has finished that frame; the limit makes
 * the flip wait (on the NV2A thread, after the frame's present) until at
 * most `max` flipped frames are unfinished. The wait holds the walk, so GET
 * stops and the title's KickOff waits, which is how the console's CPU waits
 * for its GPU. 0 = off (the old path, no queries). The edition sets the
 * default (bw_d3d11_latency_config: Remastered high frame rate only);
 * BW_GPU_LATENCY=<n> overrides, BW_GPU_LATENCY=m measures without limiting.
 * BW_GPU_STATS adds a line a second: frames in flight (mean/most) and the
 * time waited. */
#define LAT_Q 64
static ID3D11Query *s_lat_q[LAT_Q];
static LARGE_INTEGER s_lat_t[LAT_Q];
static unsigned s_lat_head, s_lat_tail;
static int s_lat_max = -1;          /* -1 unset, 0 off, n frames */
static int s_lat_cfg;               /* the edition's default (bw_d3d11_latency_config) */
static unsigned long s_lat_st_n, s_lat_st_sum, s_lat_st_most, s_lat_st_waits;
static double s_lat_st_wait_ms, s_lat_st_wait_max;
static HANDLE s_lat_timer;
/* lat-q: the limit holds at any frame time (on by default since the audio
 * release's gate, call INT-1; BW_GPU_LATENCY_HOLD=0 is 0.2.5's wait, given
 * up after 100 ms, with its 100 us naps). Seen by lat-m under heavy
 * GPU contention: with frames over 100 ms the old wait gave up every flip,
 * so ~21 frames queued and each reached the GPU ~3 s after its present (7
 * fps). Now the wait goes on while frames keep finishing; it ends only when
 * none has finished for LAT_STALL_MS (a stalled GPU or a lost query: the
 * old "never hang" guarantee, logged) or the device is lost (item 28's
 * fatal path: checked every 50 ms of waiting). BW_TEST_LATSLOW=<ms> (test
 * aid, never in a preset): a GPU that takes <ms> per frame, one frame after
 * another (a frame counts as finished no sooner than <ms> after the later of
 * its submit and the previous frame's simulated finish). */
#define LAT_STALL_MS 3000.0
static int s_lat_hold = -1;
static int s_lat_serial;            /* lat-q: BW_GPU_LATENCY=s */
static unsigned s_lat_slow_ms;
static LONGLONG s_lat_sim[LAT_Q], s_lat_sim_last;     /* BW_TEST_LATSLOW, QPC ticks */
static unsigned long s_lat_st_long, s_lat_st_stall;
static int s_lat_env;               /* BW_GPU_LATENCY was set */
static int s_lat_capped;            /* lat-q: the limit while the frame cap is reached */

void bw_d3d11_latency_config(int max_frames)
{
    s_lat_cfg = max_frames;
}

/* lat-q: the edition's limit while the frame cap is being reached
 * (g_bw_cap_reached; 0 = the same as otherwise). BW_GPU_LATENCY overrides
 * both. With BW_HIFPS_JIT=3 the cap's slack pays for the shorter queue. */
void bw_d3d11_latency_capped(int max_frames)
{
    s_lat_capped = max_frames;
}

static int lat_max(void)
{
    if (s_lat_max < 0) {
        const char *e = getenv("BW_GPU_LATENCY");
        if (e && *e == 'm')
            s_lat_max = LAT_Q - 2;              /* measure only */
        else if (e && *e == 's') {
            /* lat-q: "serial", each flip waits until the GPU has finished
             * that very frame (no frame left on the GPU: D-027's lowest
             * latency, at a frame-rate cost where the GPU is the limit) */
            s_lat_max = 1;
            s_lat_serial = 1;
        } else if (e && *e)
            s_lat_max = atoi(e);
        else
            s_lat_max = s_lat_cfg;
        s_lat_env = e && *e;
        if (s_lat_max < 0) s_lat_max = 0;
        if (s_lat_max > LAT_Q - 2) s_lat_max = LAT_Q - 2;
        if (s_lat_max)
            fprintf(stderr, "[GPU-LAT] at most %d flipped frames unfinished on the GPU%s (BW_GPU_LATENCY)\n",
                    s_lat_serial ? 0 : s_lat_max, s_lat_max >= LAT_Q - 2 ? " (measuring only)"
                    : s_lat_serial ? ": each flip waits for its own frame (serial)" : "");
        e = getenv("BW_GPU_LATENCY_HOLD");
        s_lat_hold = !(e && e[0] == '0');       /* on (the audio release's gate, call INT-1); =0 is 0.2.5's wait */
        e = getenv("BW_TEST_LATSLOW");
        s_lat_slow_ms = e && *e ? (unsigned)atoi(e) : 0u;
        if (s_lat_max && s_lat_max < LAT_Q - 2)
            fprintf(stderr, "[GPU-LAT] the limit %s (BW_GPU_LATENCY_HOLD)%s\n",
                    s_lat_hold ? "holds at any frame time; a wait ends only if no frame finishes for 3 s"
                               : "is given up after 100 ms of waiting (the old wait)",
                    s_lat_slow_ms ? "; BW_TEST_LATSLOW: as if the GPU took that long per frame (test aid)" : "");
    }
    return s_lat_max;
}

/* Retire finished frames from the oldest; flush = let the driver submit. */
static void lat_poll(int flush)
{
    while (s_lat_tail != s_lat_head) {
        unsigned i = s_lat_tail % LAT_Q;
        HRESULT hr = ID3D11DeviceContext_GetData(s_ctx, (ID3D11Asynchronous *)s_lat_q[i], NULL, 0,
                                                 flush ? 0 : D3D11_ASYNC_GETDATA_DONOTFLUSH);
        flush = 0;
        if (hr != S_OK) {
            /* item 28: a lost device never finishes its queries; lat_flip
             * used to wait its full 100 ms on every flip */
            if (FAILED(hr))
                gpu_lost(hr, "the frame query");
            break;
        }
        if (s_lat_slow_ms) {
            /* BW_TEST_LATSLOW (test aid, above): not finished until then */
            LARGE_INTEGER c;
            QueryPerformanceCounter(&c);
            if (c.QuadPart < s_lat_sim[i])
                break;
        }
        if (g_bw_fl_on)
            bw_fl_event(BW_FL_N_GPUDONE, s_lat_tail, bw_fl_us_since((unsigned long long)s_lat_t[i].QuadPart),
                        s_lat_head - s_lat_tail);
        s_lat_tail++;
    }
}

static void lat_flip(void)
{
    int max;
    unsigned i;
    if (bwg_gate_mode() && gt_setup() == 1)
        return;                         /* gfx-lat11: the latency mode holds the limit at its gate */
    max = lat_max();
    if (!max || !s_ctx)
        return;
    lat_poll(0);
    i = s_lat_head % LAT_Q;
    if (s_lat_head - s_lat_tail >= LAT_Q - 1)
        return;                                 /* full (cannot happen with max <= LAT_Q - 2) */
    if (!s_lat_q[i]) {
        D3D11_QUERY_DESC qd;
        qd.Query = D3D11_QUERY_EVENT;
        qd.MiscFlags = 0;
        if (FAILED(ID3D11Device_CreateQuery(s_dev, &qd, &s_lat_q[i]))) {
            s_lat_q[i] = NULL;
            s_lat_max = 0;
            return;
        }
    }
    ID3D11DeviceContext_End(s_ctx, (ID3D11Asynchronous *)s_lat_q[i]);
    QueryPerformanceCounter(&s_lat_t[i]);
    if (s_lat_slow_ms) {                        /* BW_TEST_LATSLOW: a serial GPU that slow */
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        s_lat_sim_last = (s_lat_sim_last > s_lat_t[i].QuadPart ? s_lat_sim_last : s_lat_t[i].QuadPart)
                         + f.QuadPart * (LONGLONG)s_lat_slow_ms / 1000;
        s_lat_sim[i] = s_lat_sim_last;
    }
    s_lat_head++;
    s_lat_st_n++;
    s_lat_st_sum += s_lat_head - s_lat_tail;
    if (s_lat_head - s_lat_tail > s_lat_st_most)
        s_lat_st_most = s_lat_head - s_lat_tail;
    if (s_lat_serial)
        max = 0;                                /* lat-q: wait for this frame too */
    else if (s_lat_capped && !s_lat_env && g_bw_cap_reached && max > s_lat_capped)
        max = s_lat_capped;                     /* lat-q: the cap's limit, while it is reached */
    if (s_lat_head - s_lat_tail > (unsigned)max) {
        double t0 = bwg_qpc_ms(), w;
        unsigned before = s_lat_head - s_lat_tail, n = 0;
        double t_prog = t0, t_dev = t0, now;
        unsigned tail0 = s_lat_tail;
        if (!s_lat_timer)
            s_lat_timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* HIGH_RESOLUTION */,
                                                 TIMER_ALL_ACCESS);
        lat_poll(1);                           /* submit what is queued */
        g_bw_present_waiting = 1;               /* lat-q */
        bwg_flip_waited = 1;
        while (s_lat_head - s_lat_tail > (unsigned)max) {
            LARGE_INTEGER due;
            if (++n < 4) {
                SwitchToThread();
            } else {
                /* 100 us naps; with the hold, 1 ms ones once the wait is long (lat-q) */
                due.QuadPart = s_lat_hold && bwg_qpc_ms() - t0 >= 20.0 ? -10000 : -1000;
                if (s_lat_timer && SetWaitableTimer(s_lat_timer, &due, 0, NULL, NULL, FALSE))
                    WaitForSingleObject(s_lat_timer, 2);
                else
                    Sleep(0);
            }
            lat_poll(0);
            now = bwg_qpc_ms();
            if (!s_lat_hold) {
                if (now - t0 > 100.0)
                    break;                      /* never hang on a lost query (the old wait) */
                continue;
            }
            /* lat-q: the limit holds while frames keep finishing */
            if (s_lat_tail != tail0) {
                tail0 = s_lat_tail;
                t_prog = now;
            }
            if (now - t_dev >= 50.0) {
                /* BW_GPU_FAILTEST=gpuwait:<n> (test aid) reports it removed */
                HRESULT why = BW_GPU_FT("gpuwait", DXGI_ERROR_DEVICE_REMOVED,
                                        ID3D11Device_GetDeviceRemovedReason(s_dev));
                t_dev = now;
                if (FAILED(why)) {              /* item 28's fatal path: ends the boot */
                    gpu_lost(DXGI_ERROR_DEVICE_REMOVED, "the frame-latency wait");
                    break;
                }
                lat_poll(1);                    /* and let the driver submit again */
            }
            if (now - t_prog > LAT_STALL_MS) {
                /* no frame finished for 3 s on a live device: a stalled GPU
                 * or a lost query; never hang (logged, the first five) */
                if (++s_lat_st_stall <= 5)
                    fprintf(stderr, "[GPU-LAT] no frame finished in %.0f ms (frame %u, %u unfinished): the wait"
                                    " gives up%s\n", LAT_STALL_MS, (unsigned)bwg_gen, s_lat_head - s_lat_tail,
                            s_lat_st_stall == 5 ? "; later ones aren't logged" : "");
                break;
            }
        }
        g_bw_present_waiting = 0;
        w = bwg_qpc_ms() - t0;
        if (w > 100.0 && s_lat_hold) {
            static int noted;
            s_lat_st_long++;
            if (!noted++)
                fprintf(stderr, "[GPU-LAT] the GPU took over 100 ms for a frame (frame %u, waited %.0f ms): the"
                                " limit holds (BW_GPU_LATENCY_HOLD; =0 gives 0.2.5's 100 ms give-up)\n",
                        (unsigned)bwg_gen, w);
        }
        s_lat_st_waits++;
        s_lat_st_wait_ms += w;
        if (w > s_lat_st_wait_max)
            s_lat_st_wait_max = w;
        if (g_bw_fl_on)
            bw_fl_event(BW_FL_N_GPUWAIT, (unsigned)(w * 1000.0), before, 0);
    }
}

static void lat_stats_line(void)
{
    bwg_pace_stats_line();
    bwg_gate_stats_line();              /* gfx-lat11: the latency mode only */
    if (!s_lat_max || !s_lat_st_n)
        return;
    fprintf(stderr, "[GPU-LAT] frames in flight %.2f mean, %lu most | waits %lu, %.1f ms (most %.1f)"
                    " | over 100 ms %lu, gave up %lu this boot\n",
            (double)s_lat_st_sum / s_lat_st_n, s_lat_st_most, s_lat_st_waits, s_lat_st_wait_ms,
            s_lat_st_wait_max, s_lat_st_long, s_lat_st_stall);
    s_lat_st_long = 0;
    s_lat_st_n = s_lat_st_sum = s_lat_st_most = s_lat_st_waits = 0;
    s_lat_st_wait_ms = s_lat_st_wait_max = 0.0;
}

/* P2b, lat-q: the present's pacing lives in the core (bwg_pace.c); these
 * are the names main.c and the high frame rate call. */
void bw_d3d11_beat_auto(int on)
{
    bwg_beat_auto(on);
}

void bw_d3d11_pace_config(unsigned hz)
{
    bwg_pace_config(hz);
}

void bw_d3d11_cap_watch(int on)
{
    bwg_cap_watch(on);
}

int bw_d3d11_present_due(double *due_ms)
{
    return bwg_present_due(due_ms);
}

double bw_d3d11_pace_period(void)
{
    return bwg_pace_period();
}

/* ---- BW_LATLOG (lat-m, test aid, every edition) --------------------------
 * Per flip: the present call, the readback, and when the GPU finished the
 * frame: a D3D11 fence is signalled after the present and a watcher thread
 * waits on an event per flip (the D3D calls stay on this thread; the watcher
 * only waits on Win32 events). On screen also DXGI's present count for the
 * flip and its frame statistics, which say at which vblank which present
 * reached the screen. Inert unless BW_LATLOG is set (bw_latlog.c). */
#define LATF_RING 256
static ID3D11Fence *s_latf;
static ID3D11DeviceContext4 *s_latf_ctx;
static ID3D11Device *s_latf_dev;
static int s_latf_failed;
static HANDLE s_latf_kick;
static struct { long k; HANDLE ev; } s_latf_q[LATF_RING];
static volatile LONG s_latf_head, s_latf_tail;     /* head: pushed (NV2A), tail: taken (watcher) */
static unsigned long s_latf_skipped;
static long s_lat_dumps;

static DWORD WINAPI latf_watch(LPVOID unused)
{
    (void)unused;
    for (;;) {
        LONG tail = s_latf_tail;
        if (tail == s_latf_head) {
            WaitForSingleObject(s_latf_kick, 100);
            continue;
        }
        {
            HANDLE ev = s_latf_q[tail % LATF_RING].ev;
            long k = s_latf_q[tail % LATF_RING].k;
            if (WaitForSingleObject(ev, 2000) == WAIT_OBJECT_0)
                bw_lat_ev(BW_LAT_GPU, bw_lat_now(), k, 0, 0, 0, 0, 0, 0.0, 0.0);
            CloseHandle(ev);
            InterlockedExchange(&s_latf_tail, tail + 1);
        }
    }
}

static void latf_signal(long k)
{
    HANDLE ev;
    if (s_latf_failed || !s_dev || !s_ctx)
        return;
    if (s_latf_dev != s_dev) {                  /* first use, or a new device */
        ID3D11Device5 *d5 = NULL;
        if (s_latf) { ID3D11Fence_Release(s_latf); s_latf = NULL; }
        if (s_latf_ctx) { ID3D11DeviceContext4_Release(s_latf_ctx); s_latf_ctx = NULL; }
        s_latf_dev = s_dev;
        if (FAILED(ID3D11Device_QueryInterface(s_dev, &IID_ID3D11Device5, (void **)&d5))
            || FAILED(ID3D11Device5_CreateFence(d5, 0, D3D11_FENCE_FLAG_NONE, &IID_ID3D11Fence,
                                                (void **)&s_latf))
            || FAILED(ID3D11DeviceContext_QueryInterface(s_ctx, &IID_ID3D11DeviceContext4,
                                                         (void **)&s_latf_ctx))) {
            fprintf(stderr, "[BW] latency log: no D3D11 fence here; no GPU-done times\n");
            s_latf_failed = 1;
            if (d5) ID3D11Device5_Release(d5);
            return;
        }
        ID3D11Device5_Release(d5);
        if (!s_latf_kick) {
            HANDLE th;
            s_latf_kick = CreateEventW(NULL, FALSE, FALSE, NULL);
            th = CreateThread(NULL, 0, latf_watch, NULL, 0, NULL);
            if (th) {
                SetThreadPriority(th, THREAD_PRIORITY_HIGHEST);
                CloseHandle(th);
            }
        }
    }
    if (s_latf_head - s_latf_tail >= LATF_RING - 1) {
        s_latf_skipped++;                       /* the watcher is behind: no time for this flip */
        return;
    }
    ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!ev)
        return;
    if (FAILED(ID3D11DeviceContext4_Signal(s_latf_ctx, s_latf, (UINT64)k))
        || FAILED(ID3D11Fence_SetEventOnCompletion(s_latf, (UINT64)k, ev))) {
        CloseHandle(ev);
        return;
    }
    s_latf_q[s_latf_head % LATF_RING].k = k;
    s_latf_q[s_latf_head % LATF_RING].ev = ev;
    InterlockedIncrement(&s_latf_head);
    SetEvent(s_latf_kick);
}

static void lat_present_stats(long k)
{
    static UINT last_pc = 0xFFFFFFFFu;
    UINT pc = 0;
    DXGI_FRAME_STATISTICS st;
    if (!s_swap)
        return;
    if (SUCCEEDED(IDXGISwapChain1_GetLastPresentCount(s_swap, &pc)))
        bw_lat_ev(BW_LAT_PCOUNT, bw_lat_now(), k, pc, 0, 0, 0, 0, 0.0, 0.0);
    memset(&st, 0, sizeof st);
    if (SUCCEEDED(IDXGISwapChain1_GetFrameStatistics(s_swap, &st)) && st.PresentCount != last_pc) {
        last_pc = st.PresentCount;
        bw_lat_ev(BW_LAT_STATS, bw_lat_now(), st.PresentCount, st.PresentRefreshCount,
                  st.SyncRefreshCount, st.SyncQPCTime.QuadPart, 0, 0, 0.0, 0.0);
    }
}

static void bw_flip_body(void);

/* ---- I1, the CPU-write latch: this back end's part (core/bwg_latch.c; lane
 * gfx-regen, docs/gfx/lanes/regen.md) -------------------------------------
 * upload: the marked guest rows into the surface's GPU copy. In Original
 * (scale 1, no MSAA, the guest's own format) the bytes go straight into the
 * texture, as the seed's do. At a render scale, with MSAA or widescreen
 * (Remastered), the rows become a small texture drawn over the target's
 * rectangle at its scale with the seed's bilinear stretch; widescreen maps
 * guest memory to the centre, as the readback does (readback_source). The
 * binding is restored, and the surface counts as drawn. */
static void latch_upload(BwgSurf *s, const uint32_t r[4])
{
    uint32_t sp = s->cur_pitch ? s->cur_pitch : s->pitch, w = r[2] - r[0], h = r[3] - r[1];
    const uint8_t *src = GUEST(s->va) + (size_t)r[1] * sp + (size_t)r[0] * s->bpp;
    if (!s->tex)
        return;
    if (!bw_vfx_active() && !s->mtex && s->pw == s->w && s->ph == s->h) {
        D3D11_BOX box;
        box.left = r[0]; box.top = r[1]; box.front = 0;
        box.right = r[2]; box.bottom = r[3]; box.back = 1;
        ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)s->tex, 0, &box, src, sp, 0);
    } else {
        D3D11_TEXTURE2D_DESC td;
        D3D11_SUBRESOURCE_DATA init;
        D3D11_VIEWPORT vp;
        ID3D11Texture2D *t = NULL;
        ID3D11ShaderResourceView *v = NULL;
        BwSurf *c = bwg_cur;
        BwDepth *d = bwg_cur_depth;
        float cw = s->wsk > 1.0f ? (float)s->pw / s->wsk : (float)s->pw;
        float sx = cw / (float)s->w, sy = (float)s->ph / (float)s->h;
        memset(&td, 0, sizeof td);
        td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = surf_format(s->bpp);
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        init.pSysMem = src;
        init.SysMemPitch = sp;
        init.SysMemSlicePitch = 0;
        if (SUCCEEDED(ID3D11Device_CreateTexture2D(s_dev, &td, &init, &t))
                && SUCCEEDED(ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)t, NULL, &v))) {
            vp.TopLeftX = ((float)s->pw - cw) * 0.5f + (float)r[0] * sx;
            vp.TopLeftY = (float)r[1] * sy;
            vp.Width = (float)w * sx;
            vp.Height = (float)h * sy;
            vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
            bw_vfx_blit_vp(v, w, h, s->mrtv ? s->mrtv : s->rtv, &vp);
        }
        if (v) ID3D11ShaderResourceView_Release(v);
        if (t) ID3D11Texture2D_Release(t);
        if (c)
            bind_targets(c, d);
        else
            g_bw_ctx_epoch++;
    }
    SURF_DRAWN(s);
}

/* Test aid, BW_GPU_LATCH_SHOTS=<n>[,<n>...] (with RECOMP_FB_DUMP as the
 * prefix): the surface shown again at those re-presents (1 the first), as a
 * 32-bit BMP of its GPU copy, latch_<n>.bmp; 8-bit surfaces at the guest's
 * size only (Original), others are skipped with a line. */
static void latch_shot(BwgSurf *s, unsigned k)
{
    static int init;
    static unsigned want[16], nw;
    const char *prefix = getenv("RECOMP_FB_DUMP");
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D *stg = NULL;
    D3D11_MAPPED_SUBRESOURCE m;
    unsigned i;
    int hit = 0;
    if (!init) {
        const char *e = getenv("BW_GPU_LATCH_SHOTS");
        init = 1;
        while (e && *e && nw < 16) {
            want[nw++] = (unsigned)strtoul(e, (char **)&e, 10);
            if (*e == ',') e++; else break;
        }
    }
    for (i = 0; i < nw; i++)
        if (want[i] == k)
            hit = 1;
    if (!hit || !prefix || !*prefix || !s->tex)
        return;
    {
        /* the guest-sized 8-bit picture the readback would write: the
         * surface itself in Original, the suite's box-filtered centre at a
         * render scale, with MSAA or widescreen (readback_source) */
        ID3D11Texture2D *src = readback_source(s);
        D3D11_BOX box;
        uint32_t cx = s_rb_crop_x;
        if (!src)
            return;
        ID3D11Texture2D_GetDesc(src, &td);
        if (td.Format != DXGI_FORMAT_B8G8R8A8_UNORM || td.SampleDesc.Count != 1) {
            fprintf(stderr, "[GPU-LATCH] shot %u skipped: surface format %u\n", k, (unsigned)td.Format);
            return;
        }
        td.Width = s->w;
        td.Height = s->h;
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        td.MiscFlags = 0;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &stg)))
            return;
        box.left = cx; box.top = 0; box.front = 0;
        box.right = cx + s->w; box.bottom = s->h; box.back = 1;
        ID3D11DeviceContext_CopySubresourceRegion(s_ctx, (ID3D11Resource *)stg, 0, 0, 0, 0,
                                                  (ID3D11Resource *)src, 0, &box);
    }
    if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)stg, 0, D3D11_MAP_READ, 0, &m))) {
        char path[600];
        FILE *f;
        _snprintf(path, sizeof path, "%slatch_%04u.bmp", prefix, k);
        path[sizeof path - 1] = 0;
        if ((f = fopen(path, "wb")) != NULL) {
            uint8_t hd[54];
            uint32_t size = 54 + td.Width * td.Height * 4, y;
            int32_t hh = -(int32_t)td.Height;
            memset(hd, 0, sizeof hd);
            hd[0] = 'B'; hd[1] = 'M';
            memcpy(hd + 2, &size, 4);
            hd[10] = 54; hd[14] = 40;
            memcpy(hd + 18, &td.Width, 4);
            memcpy(hd + 22, &hh, 4);
            hd[26] = 1; hd[28] = 32;
            fwrite(hd, 1, 54, f);
            for (y = 0; y < td.Height; y++)
                fwrite((const uint8_t *)m.pData + (size_t)y * m.RowPitch, 4, td.Width, f);
            fclose(f);
            fprintf(stderr, "[GPU-LATCH] shot %s (surface %08X, %ux%u, frame %u, shown again %u)\n", path,
                    s->va, td.Width, td.Height, bwg_gen, k);
        }
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)stg, 0);
    }
    ID3D11Texture2D_Release(stg);
}

/* represent: the shown surface presented again without a flip (the
 * front-buffer rule: the loading thread's progress bar), and the plain
 * window (BW_GPU_PRESENT=0 shows guest memory) told too. */
static int latch_represent(BwgSurf *s)
{
    static unsigned k;
    uint32_t pitch;
    int r;
    if (!s || !s->srv)
        return 0;
    r = gpu_present(s);
    latch_shot(s, ++k);
    pitch = s->cur_pitch ? s->cur_pitch : s->pitch;
    xbox_FramebufferWindowSet(s->va, pitch);
    xbox_FramebufferWindowPresent(s->va, pitch);
    return r;
}

/* read_now: the forced read, a waiting readback whatever the mode; the
 * surface's dirty mark is kept (it still presents at its flip). */
static void latch_read_now(BwgSurf *s)
{
    int d = s->dirty;
    s->dirty = 1;
    s_rb_force_sync = 1;
    readback(s);
    s_rb_force_sync = 0;
    s->dirty = d;
}

/* scanout: the made surface the display scans out, the frame buffer the
 * title gave AvSetDisplayMode (the runtime records it resolved to the
 * contiguous window; the title never writes PCRTC_START itself, and with
 * anti-aliasing it has one scan-out buffer, which D3D's swap resolves into).
 * The live run only (bwg_latch_idle never runs in a replay). */
extern uint32_t xbox_GetDisplayFramebuffer(uint32_t *pitch);
static BwgSurf *latch_scanout(void)
{
    uint32_t pitch = 0, fb = xbox_GetDisplayFramebuffer(&pitch);
    return fb ? bwg_find_surf(fb) : NULL;
}

static const BwgLatchOps s_latch_ops = { latch_upload, latch_represent, latch_read_now, latch_scanout };

static void latch_init(void)
{
    bwg_latch_init(&s_latch_ops);
}

/* lat-q: counts the flips the back end has finished (g_bw_flips_done) */
static void bw_flip(void)
{
    bw_flip_body();
    if (bwg_gate_mode())
        gate_flip((uint32_t)g_bw_flips_done + 1);   /* gfx-lat11: the latency mode's GPU side */
    else if (s_cfg_hifps && s_ctx && flip_flush_on())
        ID3D11DeviceContext_Flush(s_ctx);           /* BW_GPU_FLIPFLUSH (test aid) */
    InterlockedIncrement(&g_bw_flips_done);
}

static void bw_flip_body(void)
{
    g_bw_ctx_epoch++;                   /* I3: the flip's work sets state of its own */
    int64_t lat_t0 = 0, lat_tp = 0, lat_tp1 = 0, lat_trb = 0;
    long lat_k = 0;
    int lat_res = -1;
    if (g_bw_lat_on) {
        lat_t0 = bw_lat_now();
        lat_k = InterlockedIncrement(&g_bw_lat_flips);
        if (nv2a_pb_exec_flip_dumping())
            bw_lat_ev(BW_LAT_DUMPED, lat_t0, lat_k, ++s_lat_dumps, 0, 0, 0, 0, 0.0, 0.0);
    }
    if (g_bw_gfxcap)
        bw_gfxcap_frame(bwg_gen);       /* BW_GFXCAP: the flip in progress (the suite's pictures) */
    stats_tick();
    check_holes();
    s_st_flips++;
    if (!s_dev) {
        if (lat_t0)
            bw_lat_ev(BW_LAT_FLIP, lat_t0, lat_k, -2, 0, 0, 0, bw_lat_now(), 0.0, 0.0);
        return;
    }
    gs_unbind();                        /* item 43: the present and the effects draw without one */
    vsh_finish_compiled();              /* item 43: into the shader cache, drawn again or not */
    bw_gpu_debug_drain();               /* item 25: BW_GPU_DEBUG's messages, once a flip */
    if (bw_vfx_active() && bw_vfx()->tex_restore)
        bw_tr_frame();                  /* T1: a [TEXRESTORE] summary now and then */
    /* The surface the title just finished is the one it presents. */
    BWG_LATCH_USE(bwg_cur);             /* I1: a CPU write into it since its last use, up first */
    vshpre_flip_line();                 /* lane gfx-regen: the load-time compile's lines */
    {
        static double latch_last;       /* I1's line, once a second when it moved */
        double now = bwg_qpc_ms();
        if (now - latch_last >= 1000.0) {
            latch_last = now;
            bwg_latch_line();
        }
    }
    ft_flip(bwg_cur);
    if (s_stats > 0) {
        static double last;
        double now = bwg_qpc_ms();
        if (last) {
            double ft = now - last;
            if (!s_st_ft_n || ft < s_st_ft_min) s_st_ft_min = ft;
            if (ft > s_st_ft_max) s_st_ft_max = ft;
            s_st_ft_sum += ft;
            s_st_ft_n++;
        }
        last = now;
    }
    if (bwg_cur && bwg_cur->dirty) {
        BwSurf *shown = bwg_cur;
        double t0 = (s_stats > 0 || trc_on()) ? bwg_qpc_ms() : 0.0, t1 = 0.0;
        unsigned long long fl_t0;
        int fl_pres;
        bwg_flip_waited = 0;               /* lat-q: set by the waits below */
        g_bw_flip_ready_ms = bwg_qpc_ms();
        bwg_pace_present();              /* P2b: no sooner than 1/target after the last */
        fl_t0 = g_bw_fl_on ? bw_fl_ticks() : 0;
        if (lat_t0)
            lat_tp = bw_lat_now();
        if (bw_edgeprobe_on())          /* BW_GPU_EDGEPROBE (test aid): after the last draw */
            bw_edgeprobe_at(BW_EP_LAST, s_ctx, shown->mtex ? (void *)shown->mtex : (void *)shown->tex, bwg_gen);
        fl_pres = gpu_present(shown);    /* before the readback's wait */
        bwg_latch_flipped(shown);       /* I1: the front-buffer rule's surface and clock */
        if (bwg_gate_mode())
            s_gt_pres_ms = bwg_qpc_ms(); /* gfx-lat11: the present call's return */
        if (bw_edgeprobe_on())          /* BW_GPU_EDGEPROBE (test aid): after the present */
            bw_edgeprobe_at(BW_EP_POST, s_ctx, shown->mtex ? (void *)shown->mtex : (void *)shown->tex, bwg_gen);
        if (lat_t0) {
            lat_tp1 = bw_lat_now();
            lat_res = s_present_mode ? fl_pres : 2;
            latf_signal(lat_k);
            if (lat_res == 1)
                lat_present_stats(lat_k);
        }
        if (fl_t0)
            bw_fl_event(BW_FL_N_FLIP, bw_fl_us_since(fl_t0), s_present_mode ? (unsigned)fl_pres : 2u, bwg_gen);
        if (trc_on()) {
            double tp = bwg_qpc_ms();
            s_trc_pres += tp - t0;
            if (readback_wanted())
                s_trc_rb -= tp;         /* the readback's time is added below */
        }
        if (s_stats > 0) {
            t1 = bwg_qpc_ms();
            s_st_pres_ms += t1 - t0;
            if (t1 - t0 > s_st_pres_max) s_st_pres_max = t1 - t0;
        }
        if (readback_wanted()) {
            s_gfxcap_rb_rows = 0;
            readback(bwg_cur);
            if (g_bw_gfxcap && s_gfxcap_rb_rows)    /* BW_GFXCAP: the flipped surface's hash */
                bw_gfxcap_flip(bwg_gen, s_gfxcap_rb_va, s_gfxcap_rb_pitch, s_gfxcap_rb_row,
                               s_gfxcap_rb_rows, bwg_cur->bpp);
            if (trc_on())
                s_trc_rb += bwg_qpc_ms();
        } else
            bwg_cur->dirty = 0;
        if (g_bw_gfxcls) bw_gfxcls_flip(s_ctx, gfxcls_tex(shown), bwg_gen);     /* BW_GFXCAP_CLASSES (test aid) */
        if (bw_edgeprobe_on()) {        /* BW_GPU_EDGEPROBE (test aid): after the readback, and its measure */
            bw_edgeprobe_at(BW_EP_END, s_ctx, shown->mtex ? (void *)shown->mtex : (void *)shown->tex, bwg_gen);
            bw_edgeprobe_flip_end(s_ctx, shown->mtex, shown->tex,
                                  s_rb_tex && s_rb_w == shown->w && s_rb_h == shown->h ? (void *)s_rb_tex : NULL,
                                  bwg_gen);
        }
        if (s_stats > 0) {
            double t2 = bwg_qpc_ms();
            s_st_rb_ms += t2 - t1;
            if (t2 - t1 > s_st_rb_max) s_st_rb_max = t2 - t1;
        }
        if (lat_t0)
            lat_trb = bw_lat_now();
        lat_flip();                      /* P2b: GPU frames in flight (BW_GPU_LATENCY) */
        g_bw_last_flip_waited = bwg_flip_waited;   /* lat-q: BW_HIFPS_JIT=2 */
        ws_dump(shown);                 /* widescreen: the wide picture, when dumping */
        black_watch(bwg_cur);
        peek_write(bwg_cur);
        flash_watch(bwg_cur);
        {
            /* item 38: the latest binding's pitch, the one readback wrote
             * the rows at (the creation pitch can be the AA surface's) */
            uint32_t pitch = bwg_cur->cur_pitch ? bwg_cur->cur_pitch : bwg_cur->pitch;
            xbox_FramebufferWindowSet(bwg_cur->va, pitch);
            xbox_FramebufferWindowPresent(bwg_cur->va, pitch);
        }
    }
    if (bwg_ws_k > 1.0f)
        bwg_ws_frame_end();
    /* T1: finish the restored textures the worker has ready, within the
     * per-frame budget (never a stalled frame); the next draws use them */
    {
        double tq = trc_on() ? bwg_qpc_ms() : 0.0;
        if (bw_vfx_active() && bw_vfx()->tex_restore && bw_tr_pump(s_dev, s_ctx) && bwg_cur)
            bind_targets(bwg_cur, bwg_cur_depth);
        if (trc_on()) {                 /* T1 test aid: this frame's breakdown, if it was slow */
            static double last_end;
            double now = bwg_qpc_ms(), ft = last_end ? now - last_end : 0.0;
            s_trc_pump += now - tq;
            if (ft > s_trc_on)
                fprintf(stderr, "[TR-TRACE] f%u %.1f ms: prepare %.1f (shaders %.1f for %u compiles,"
                                " restore step %.1f, constants map %.1f), draws %.1f (%u), uploads %.1f (%u),"
                                " present %.1f, readback %.1f, restore finish %.1f, other %.1f;"
                                " %u restored chains drawn for the first time, %u requested (%.2f ms)\n",
                        bwg_gen, ft, s_trc_prep, s_trc_sh, s_trc_nsh, s_trc_trs, s_trc_map, s_trc_draw,
                        s_trc_ndraw, s_trc_up, s_trc_nup, s_trc_pres,
                        s_trc_rb, s_trc_pump, ft - s_trc_prep - s_trc_draw - s_trc_pres - s_trc_rb - s_trc_pump,
                        s_trc_first, s_trc_req, s_trc_reqt);
            s_trc_reqt = 0.0;
            s_trc_prep = s_trc_draw = s_trc_up = s_trc_pump = s_trc_pres = s_trc_rb = 0.0;
            s_trc_sh = s_trc_trs = s_trc_map = 0.0;
            s_trc_ndraw = s_trc_nup = s_trc_first = s_trc_req = s_trc_nsh = 0;
            last_end = bwg_qpc_ms();
        }
    }
    bwg_fb_flip();                      /* I5 (BW_GPU_FEEDBACK): its summary line, when set */
    rb_ring_line();                     /* I2 (BW_GPU_RBRING): its line, when set */
    sh_line();                          /* I3 (BW_GPU_DELTA): its line, when set */
    if (g_bw_gfxcap)
        bw_gfxcap_flip_end(bwg_gen);    /* BW_GFXCAP step B: a mid-scene open is decided here */
    bwg_gen++;
    bw_ab_frame(bwg_gen);              /* BW_GPU_AB: the next frame's parity */
    if (lat_t0)
        bw_lat_ev(BW_LAT_FLIP, lat_t0, lat_k, lat_res, lat_tp, lat_tp1, lat_trb, bw_lat_now(), 0.0, 0.0);
}

/* item 43: draw cpu (BW_GPU_STATS) around the clear and the screen-space draw */
static void bw_clear_m(const Nv2aSurface *s, const Nv2aRenderState *rs, uint32_t flags,
                       uint32_t argb, uint32_t zstencil)
{
    ULONG64 c0 = draw_cpu_begin();
    bw_clear(s, rs, flags, argb, zstencil);
    draw_cpu_end(c0);
}

static void bw_draw_m(const Nv2aSurface *s, const Nv2aBatch *b)
{
    ULONG64 c0 = draw_cpu_begin();
    gs_unbind();                        /* item 43 */
    bw_draw(s, b);
    draw_cpu_end(c0);
}

/* ---- BW_GFXCAP step B (lane gfx-capab): this back end's side of a capture
 * opened mid-scene (bw_gfxcap.h, docs/gfx/capture.md section 5). A test aid:
 * bw_gfxcap.c calls these only while recording or replaying a capture.
 *  - live, at the end of the flip before the open: every surface read back
 *    as the GPU holds it, with its table entry, into the capture; then the
 *    texture cache dropped, so every texture is read again as the replay will;
 *  - replay: the frame number, and each surface made again from the capture
 *    in its table slot, as the live table held it.
 * Two halves, so they can move apart: the table's (an entry to and from the
 * capture's record, the texture cache dropped, the frame number), which goes
 * with the surface table and the texture cache into the API-neutral core; and
 * the API's (a texture's bytes read, a texture made from bytes), which every
 * back end has its own of. Original's surfaces are exact this way;
 * Remastered's multisampled targets are carried as their resolved copy and
 * made again from it (exact in mission 1, whose frames draw them anew), see
 * docs/gfx/lanes/capab.md. */

/* -- the table's half -- */
static void gc_rec_from_entry(const BwSurf *s, uint32_t slot, BwGcSurf *g)
{
    memset(g, 0, sizeof *g);
    g->va = s->va; g->pitch = s->pitch; g->bpp = s->bpp; g->w = s->w; g->h = s->h;
    g->cur_pitch = s->cur_pitch; g->clamped = (uint32_t)s->clamped; g->dirty = (uint32_t)s->dirty;
    g->pw = s->pw; g->ph = s->ph; g->aa_x = s->aa_x; g->aa_y = s->aa_y;
    memcpy(&g->wsk_bits, &s->wsk, 4);
    g->aa_sx = s->aa_sx; g->used_gen = s->used_gen; g->slot = slot;
    g->fail_gen = s->fail_gen; g->noaa_epoch = s->noaa_epoch;
}

/* The entry in slot g->slot, emptied and filled from the record (the
 * table grows to it); NULL when the slot is out of range. */
static BwSurf *gc_entry_from_rec(const BwGcSurf *g)
{
    BwSurf *s;
    if (g->slot >= BWG_MAX_SURF)
        return NULL;
    s = bwg_surf_slot_at((int)g->slot);     /* the core's table, grown to the slot with zeroed entries */
    release_surf(s);
    memset(s, 0, sizeof *s);
    s->va = g->va; s->pitch = g->pitch; s->bpp = g->bpp; s->w = g->w; s->h = g->h;
    s->cur_pitch = g->cur_pitch; s->clamped = (int)g->clamped; s->dirty = (int)g->dirty;
    s->pw = g->pw; s->ph = g->ph; s->aa_x = g->aa_x; s->aa_y = g->aa_y;
    memcpy(&s->wsk, &g->wsk_bits, 4);
    s->aa_sx = (uint8_t)g->aa_sx; s->used_gen = g->used_gen;
    s->fail_gen = g->fail_gen; s->noaa_epoch = g->noaa_epoch;
    s->epoch = bw_vfx_epoch();
    return s;
}

static void gc_drop_textures(void)
{
    bwg_tex_drop_all();                 /* the core's cache: its views, and any restored chain, released */
}

static void gc_set_gen(uint32_t gen)
{
    bwg_gen = gen;
    bw_ab_frame(bwg_gen);               /* BW_GPU_AB: this frame's parity */
}

/* -- D3D11's half -- */
/* A surface's texture read back (as the GPU holds it) into the capture. */
static void gc_tex_read(ID3D11Texture2D *t, BwGcSurf *g)
{
    D3D11_TEXTURE2D_DESC td;
    D3D11_MAPPED_SUBRESOURCE m;
    ID3D11Texture2D *stg = NULL;
    uint32_t bpp;
    ID3D11Texture2D_GetDesc(t, &td);
    bpp = (td.Format == DXGI_FORMAT_R16G16B16A16_FLOAT || td.Format == DXGI_FORMAT_R16G16B16A16_UNORM) ? 8u
        : td.Format == DXGI_FORMAT_B5G6R5_UNORM ? 2u : 4u;     /* the formats surfaces are made in */
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &stg)))
        return;
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)stg, (ID3D11Resource *)t);
    if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)stg, 0, D3D11_MAP_READ, 0, &m))) {
        g->pw = td.Width; g->ph = td.Height;
        g->fmt = (uint32_t)td.Format;
        g->row_bytes = td.Width * bpp;
        bw_gfxcap_surface(g, (const uint8_t *)m.pData, m.RowPitch);
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)stg, 0);
    }
    ID3D11Texture2D_Release(stg);
}

/* A surface's texture and views made from the capture's bytes. */
static int gc_tex_make(BwSurf *s, const BwGcSurf *g, const uint8_t *bytes)
{
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA init;
    memset(&td, 0, sizeof td);
    td.Width = g->pw; td.Height = g->ph; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = (DXGI_FORMAT)g->fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    init.pSysMem = bytes;
    init.SysMemPitch = g->row_bytes;
    init.SysMemSlicePitch = 0;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, &init, &s->tex))) {
        s->tex = NULL;
        return 0;
    }
    if (FAILED(ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)s->tex, NULL, &s->rtv))
            || FAILED(ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)s->tex, NULL, &s->srv))) {
        release_surf(s);
        return 0;
    }
    if (g->msaa > 1) {
        /* VFX (D9): the live surface had an MSAA target. Its samples aren't
         * carried; it is made again and filled from the resolved copy, every
         * sample the same (create_surf_vfx seeds it the same way) */
        td.SampleDesc.Count = g->msaa;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (SUCCEEDED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s->mtex))
                && SUCCEEDED(ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)s->mtex, NULL, &s->mrtv))) {
            bw_vfx_blit(s->srv, s->pw, s->ph, s->mrtv, s->pw, s->ph, 0);
            s->mdirty = 1;
        } else {
            if (s->mrtv) ID3D11RenderTargetView_Release(s->mrtv);
            if (s->mtex) ID3D11Texture2D_Release(s->mtex);
            s->mrtv = NULL;
            s->mtex = NULL;
        }
    }
    return 1;
}

/* -- the interface bw_gfxcap.c calls (BwGfxcapBackend) -- */
static void gc_open_live(void)
{
    int i;
    for (i = 0; s_dev && i < bwg_surf_count(); i++) {
        BwSurf *si = bwg_surf_at(i);    /* the core's table */
        BwGcSurf g;
        if (!si->tex)
            continue;
        surf_resolve(si);               /* VFX (D9): no-op in Original */
        gc_rec_from_entry(si, (uint32_t)i, &g);
        if (si->mtex) {                 /* VFX (D9): its MSAA target's sample count */
            D3D11_TEXTURE2D_DESC md;
            ID3D11Texture2D_GetDesc(si->mtex, &md);
            g.msaa = md.SampleDesc.Count;
        }
        gc_tex_read(si->tex, &g);
    }
    gc_drop_textures();
}

static int gc_surface(const BwGcSurf *g, const uint8_t *bytes)
{
    BwSurf *s;
    if (!ensure_device() || !g->pw || !g->ph || !(s = gc_entry_from_rec(g)))
        return 0;
    return gc_tex_make(s, g, bytes);
}

static const BwGfxcapBackend s_gcb = { gc_open_live, gc_drop_textures, gc_set_gen, gc_surface };

static const Nv2aBackend s_backend = { bw_clear_m, bw_draw_m, bw_flip };

/* The core's surfaces and depth buffers call these for their objects. */
static const BwgSurfOps s_surf_ops = { release_surf, create_surf, vfx_surf_forget, release_depth, create_depth };
/* I5 (BW_GPU_FEEDBACK=copy, lane gfx-ideas): the bound target as it is now,
 * for a draw that samples it. With MSAA its samples are resolved into s->tex,
 * which isn't the bound target (s->mtex is), so the draw samples s itself;
 * otherwise a copy of s->tex, sampled through a record that differs from s
 * only in its view. Unset, nothing calls this. */
static BwSurf s_fb_surf;
static ID3D11Texture2D *s_fb_tex;
static ID3D11ShaderResourceView *s_fb_srv;
static D3D11_TEXTURE2D_DESC s_fb_td;

static BwSurf *tex_feedback(BwSurf *s)
{
    D3D11_TEXTURE2D_DESC td;
    if (!s || !s->tex || !s->srv)
        return NULL;
    if (s->mtex) {
        surf_resolve(s);
        return s;
    }
    ID3D11Texture2D_GetDesc(s->tex, &td);
    if (!s_fb_tex || memcmp(&td, &s_fb_td, sizeof td)) {
        D3D11_TEXTURE2D_DESC cd = td;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd;
        if (s_fb_srv) ID3D11ShaderResourceView_Release(s_fb_srv);
        if (s_fb_tex) ID3D11Texture2D_Release(s_fb_tex);
        s_fb_srv = NULL;
        s_fb_tex = NULL;
        cd.Usage = D3D11_USAGE_DEFAULT;
        cd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        cd.CPUAccessFlags = 0;
        cd.MiscFlags = 0;
        ID3D11ShaderResourceView_GetDesc(s->srv, &sd);
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &cd, NULL, &s_fb_tex))
                || FAILED(ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)s_fb_tex, &sd, &s_fb_srv))) {
            if (s_fb_tex) ID3D11Texture2D_Release(s_fb_tex);
            s_fb_tex = NULL;
            s_fb_srv = NULL;
            return NULL;
        }
        s_fb_td = td;
    }
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)s_fb_tex, (ID3D11Resource *)s->tex);
    s_fb_surf = *s;
    s_fb_surf.srv = s_fb_srv;
    return &s_fb_surf;
}

/* ... and the core's texture cache these. */
static const BwgTexOps s_tex_ops = { tex_release, tex_create, tex_deblock, surf_resolve, tex_feedback };

void bw_d3d11_register(void)
{
    const char *e = getenv("BW_GPU");
    if (!e || strcmp(e, "d3d11"))
        return;
    bwg_surf_set_ops(&s_surf_ops);
    bwg_tex_set_ops(&s_tex_ops);
    nv2a_backend_register(&s_backend);
    if (g_bw_gfxcap)
        bw_gfxcap_backend(&s_gcb);      /* BW_GFXCAP step B: the mid-scene open */
    {
        const char *rc = getenv("BW_GPU_RC");
        if (!(rc && rc[0] == '0'))
            bw_backend_ext_register(&s_xbackend);
        fprintf(stderr, "[GPU-D3D11] register combiners on the GPU: %s\n",
                rc && rc[0] == '0' ? "off (BW_GPU_RC=0)" : "on");
    }
    fprintf(stderr, "[GPU-D3D11] back end registered (BW_GPU=d3d11)\n");
}
