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
#include <math.h>   /* fabsf (flash_watch): an implicit declaration fails to link on newer toolsets */
#include <windows.h>
#include <intrin.h> /* __rdtsc (item 43's draw cpu measure) */
#include <d3d11.h>
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
#include "bw_vfx.h"              /* VFX (D9): Remastered visual suite; inert in Original */
#include "bw_texrestore.h"       /* T1 (VFX_TEX_RESTORE): texture restoration; inert in Original */
#include "bw_pscache.h"          /* T1: persistent pixel-shader cache (every edition) */
#include "bw_gpu_failtest.h"     /* 0.2.5 item 25: BW_GPU_FAILTEST, BW_GPU_DEBUG (test aids) */
#include "bw_hdr.h"              /* 0.2.5 item 97: a screen in HDR mode, every edition */
#include "../bw_widescreen.h"       /* Remastered widescreen (D-015/D-016) */
#include "../bw_edition.h"          /* bw_edition_add_summary */

extern ptrdiff_t g_xbox_mem_offset;
extern void xbox_FramebufferWindowSet(uint32_t va, uint32_t pitch);
extern void xbox_FramebufferWindowPresent(uint32_t va, uint32_t pitch);
extern uint32_t xbox_ContiguousBlockSize(uint32_t va);

#define GUEST(va) ((uint8_t *)((uintptr_t)(va) + g_xbox_mem_offset))

/* ---- device and pipeline ------------------------------------------------ */

static ID3D11Device *s_dev;
static ID3D11DeviceContext *s_ctx;
static ID3D11VertexShader *s_vs;
static ID3D11PixelShader *s_ps;
static ID3D11InputLayout *s_layout;
static ID3D11Buffer *s_cb, *s_vb;
static UINT s_vb_bytes;
static int s_failed;
static float s_ws_k = 1.0f;     /* bw_ws_k() for this boot, read once (bw_d3d11_ws_start) */

typedef struct {
    float surf[4];      /* W, H, 1/W, 1/H */
    float tex[4];       /* 1/tw, 1/th, textured, - */
    float alpha[4];     /* func (0..7), ref 0..1, enabled, - */
    float depth[4];     /* zmin, 1/(zmax - zmin), - , - */
    float wsx[4];       /* widescreen x remap (BW_WS shaders only): a, b, -, - */
} BwConsts;

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

#define BW_MAX_SURF 32
typedef struct {
    uint32_t va, pitch, bpp, w, h;
    uint32_t cur_pitch;         /* the pitch of the latest binding */
    int clamped;                /* readback hit the end of the guest block (logged once) */
    ID3D11Texture2D *tex;
    ID3D11RenderTargetView *rtv;
    ID3D11ShaderResourceView *srv;
    int dirty;
    /* VFX (D9): the physical size (w, h above stay the guest's logical size,
     * which every shader constant and vertex keeps using), the render-scale
     * epoch it was made for, and the multisampled target drawn into when
     * MSAA is on (tex/srv are then its resolved copy). Original: pw = w,
     * ph = h, no mtex. */
    uint32_t pw, ph, aa_x, aa_y;
    unsigned epoch;
    /* Widescreen (D5): pw is this many times wider than the render scale
     * alone makes it (the device's full-screen targets in a widescreen boot;
     * surf_physical_size). 1 otherwise, and always in Original. */
    float wsk;
    ID3D11Texture2D *mtex;
    ID3D11RenderTargetView *mrtv;
    int mdirty;                 /* drawn since the last resolve */
    uint8_t aa_sx;              /* the latest binding's horizontal AA factor (gpu_present) */
    /* 0.2.5 item 36: the frame of the last failed (re)create, so a target
     * the device can't make is tried once a frame, not once a draw; and the
     * render-scale epoch (+1) at which its MSAA target couldn't be made (it
     * and its depth buffer are then single-sample; 0: no such failure). */
    uint32_t fail_gen;
    unsigned noaa_epoch;
    uint32_t used_gen;          /* item 37: the frame it was last drawn into or sampled (a full table reuses the oldest) */
} BwSurf;

typedef struct {
    uint32_t va, w, h;          /* physical size (VFX: the render-scaled size) */
    ID3D11Texture2D *tex;
    ID3D11DepthStencilView *dsv;
    unsigned epoch;             /* VFX (D9) */
    ID3D11ShaderResourceView *srv;   /* VFX (D9): the depth as a texture (AO only); NULL otherwise */
    UINT samples;               /* 0.2.5 item 36: its sample count (the colour target's; 1 in Original) */
    uint32_t used_gen;          /* item 37: the frame it was last drawn with */
} BwDepth;

/* VFX (D9): a draw into a surface; the MSAA copy then needs a resolve. */
#define SURF_DRAWN(sf) ((sf)->dirty = 1, (sf)->mdirty = 1)

static BwSurf s_surf[BW_MAX_SURF];
static int s_nsurf;
static BwDepth s_depth[BW_MAX_SURF];
static int s_ndepth;
static BwSurf *s_cur;           /* bound colour surface */
static BwDepth *s_cur_depth;
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
static unsigned s_surf_reused, s_depth_reused;     /* item 37: slots reused (surf_slot, depth_slot) */

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

#define BW_MAX_TEX 1024
typedef struct {
    uint32_t offset, w, h, pitch, color;
    uint64_t hash, sparse;
    uint32_t gen, stable;       /* frames the full hash has not changed */
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv;
    uint32_t last_use;
    uint8_t cube;               /* six faces (TextureCube view) */
    uint8_t glyph;              /* VFX (D9): a glyph sheet (bw_vfx_is_glyph_sheet); 0 in Original */
    uint8_t alpha1;             /* VFX (D9): 1-bit alpha in use (A1R5G5B5, DXT1 with transparency) */
    uint8_t hard;               /* VFX (V1): hard-edged alpha (tex_hard_alpha); 0 in Original */
    uint8_t jpeg;               /* T1: a menu/title photo screen texture, deblocked at upload; 0 in Original */
    uint8_t movie;              /* T1: a Bink frame, deblocked at upload (item 11); 0 in Original */
    uint8_t tr_churn;           /* T1: restorations soon after the last one (contents changing) */
    uint32_t tr_gen;            /* T1: frame of the last restoration of this texture */
    uint8_t tr_bound;           /* T1 test aid: the restored chain has been drawn with */
    BwTrSlot tr;                /* T1 (VFX_TEX_RESTORE): the restored chain; zero in Original */
} BwTex;

static BwTex s_tex[BW_MAX_TEX];
static int s_ntex;
static uint32_t s_gen = 1;      /* frame number */
static uint32_t *s_decode;
static size_t s_decode_cap;

/* ---- state caches ------------------------------------------------------- */

#define BW_MAX_STATE 128
typedef struct { uint32_t key[6]; ID3D11BlendState *bs; } BwBlend;
typedef struct { uint32_t key[10]; ID3D11DepthStencilState *ds; } BwDS;
typedef struct { uint32_t key[3]; ID3D11SamplerState *ss; } BwSamp;
static BwBlend s_blend[BW_MAX_STATE];
static int s_nblend;
static BwDS s_ds[BW_MAX_STATE];
static int s_nds;
static BwSamp s_samp[BW_MAX_STATE];
static int s_nsamp;
static ID3D11RasterizerState *s_rs;
static ID3D11RasterizerState *s_rs_cull[2][2];   /* [cull back][front is CCW] */
static ID3D11RasterizerState *s_rs_z[2][3][2];    /* [depth clip][none/front/back][ccw] */
static int s_zmode = -1;                         /* BW_GPU_ZMODE: 0 vertex 1 pixel 2 clip 3 auto */
static int s_cull = -1;                          /* BW_GPU_CULL: 0 off, 1 on, 2 flipped */

/* ---- stats -------------------------------------------------------------- */

static unsigned s_st_xdraws;   /* combiner path, below */
static int s_nps;
static unsigned s_ps_failures;   /* item 41: combiner pixel shaders that failed (get_rc_shader) */
/* GPU vertex programs, below: draws, batches left to the CPU while their
 * shader compiles, constant uploads, shaders compiled and failed. */
static unsigned s_st_vdraws, s_st_vfall, s_st_kup, s_st_vraw, s_st_kskip;   /* vraw: P1 raw streams */
static volatile LONG s_nvsh, s_vsh_compiles, s_vsh_failures;
static double s_vsh_compile_ms;
static int s_stats = -1;
static unsigned s_st_draws, s_st_tris, s_st_uploads, s_st_rtt, s_st_flips, s_st_clears;
static unsigned s_st_presented, s_st_dropped;    /* gpu_present: shown, dropped (DO_NOT_WAIT) */
static unsigned s_st_rb_async_late;     /* VFX (D9): async readback frames dropped (ring full) */
/* Frame timing on the NV2A thread, per stats second (ms): the present and the
 * readback (sum and worst), and flip to flip (best, sum, worst). */
static double s_st_pres_ms, s_st_pres_max, s_st_rb_ms, s_st_rb_max;
static double s_st_ft_min, s_st_ft_sum, s_st_ft_max;
static unsigned s_st_ft_n;
static ULONGLONG s_st_t0;

static double qpc_ms(void);

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

static double qpc_ms(void)
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

/* 0.2.5 item 43: the back end's own CPU time on the NV2A thread, per flip
 * (BW_GPU_STATS's "draw cpu"): the thread's cycles inside clear and the
 * three draw entry points (QueryThreadCycleTime, so time the thread spends
 * preempted by other work on a busy PC isn't counted), in ms at the TSC's
 * rate, measured against QPC over each stats second. */
static ULONG64 s_st_draw_cyc;
static unsigned __int64 s_st_tsc0;
static double s_st_q0;
static unsigned s_st_xcbup;     /* item 43: combiner constant uploads (the rest were unchanged) */

/* Item 43's per-draw shortcuts (memos of the last lookup, the constant
 * shadow, the index of texture entries): BW_GPU_MEMO=0 turns them all off,
 * as before 0.2.5, to compare or to rule them out. */
static int memo_on(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("BW_GPU_MEMO");
        v = !(e && e[0] == '0');
        if (!v)
            fprintf(stderr, "[GPU-D3D11] per-draw lookup shortcuts off (BW_GPU_MEMO=0)\n");
    }
    return v;
}

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
    double q = qpc_ms(), ms = 0.0;
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
static void pace_stats_line(void);    /* P2b: present pacing, below */
static void pstat_line(void);          /* P2b: presentation statistics, below */
static void ors_stats_line(void);      /* 0.2.5 item 49: the sea's single reflection, below */

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
                s_st_flips, s_st_draws, s_st_tris, s_st_clears, s_st_uploads, s_st_rtt,
                s_nsurf, s_ntex, s_st_xdraws, s_nps, s_ps_failures, s_st_xcbup, s_st_vdraws, s_st_vraw, s_st_vfall, s_st_kup, s_st_kskip,
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
            if (s_surf_reused + s_depth_reused != told_reuse) {
                told_reuse = s_surf_reused + s_depth_reused;
                fprintf(stderr, "[GPU-D3D11] slots reused so far (tables full): colour surfaces %u,"
                                " depth buffers %u\n", s_surf_reused, s_depth_reused);
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
        s_st_rb_async_late = 0;
        s_st_presented = s_st_dropped = 0;
        s_st_pres_ms = s_st_pres_max = s_st_rb_ms = s_st_rb_max = 0.0;
        s_st_ft_min = s_st_ft_sum = s_st_ft_max = 0.0;
        s_st_ft_n = 0;
        s_st_xcbup = 0;
        s_st_xdraws = s_st_vdraws = s_st_vfall = s_st_kup = s_st_vraw = s_st_kskip = 0;
        s_st_flips = s_st_draws = s_st_tris = s_st_uploads = s_st_rtt = s_st_clears = 0;
        s_st_t0 = now;
    }
}

/* ---- setup -------------------------------------------------------------- */

/* The vertex shaders' defines: BW_WS (the widescreen x remap) only when this
 * boot is widescreen, so Original compiles exactly the text it always did. */
static const D3D_SHADER_MACRO s_ws_defines[] = { { "BW_WS", "1" }, { NULL, NULL } };
#define WS_DEFINES (s_ws_k > 1.0f ? s_ws_defines : NULL)

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
    /* VFX (D9): the visual suite's settings and helpers; nothing in Original. */
    bw_vfx_init();
    bw_vfx_gpu_init(s_dev, s_ctx);
    if (bw_vfx_active() && bw_vfx()->tex_restore)
        bw_tr_init();                   /* T1: its shader, now rather than at the first 3D draw */
    bw_gpu_debug_drain();               /* item 25: BW_GPU_DEBUG's messages from the set-up */
    return 1;
}

/* ---- surfaces ----------------------------------------------------------- */

/* Back-face culling as the title set it. NV2A takes GL's values:
 * SET_CULL_FACE GL_FRONT 0x404 / GL_BACK 0x405 / GL_FRONT_AND_BACK 0x408,
 * SET_FRONT_FACE GL_CW 0x900 / GL_CCW 0x901. The executor hands over surface
 * pixels with y down, the same space D3D11's rasteriser judges winding in;
 * which way NV2A counts in that space was checked against frames
 * (BW_GPU_CULL=flip draws the other convention, ANALYSIS). Returns NULL
 * when the title culls both faces: nothing to draw. */
static int zmode(void)
{
    if (s_zmode < 0) {
        const char *e = getenv("BW_GPU_ZMODE");
        s_zmode = !e || !*e || e[0] == 'p' ? 1 : e[0] == 'v' ? 0 : e[0] == 'c' ? 2 : 3;   /* default pixel */
        if (s_zmode)
            fprintf(stderr, "[GPU-D3D11] depth range: %s\n",
                    s_zmode == 1 ? "clamped per pixel" : s_zmode == 2 ? "culled per pixel"
                    : "as the title sets SET_ZMIN_MAX_CONTROL");
    }
    return s_zmode;
}

/* The title's SET_ZMIN_MAX_CONTROL (0x1D78), logged when it changes. Its
 * ZCLAMP_EN field (bits 4-7) is 0 for cull, 1 for clamp (xemu's names). */
static uint32_t zminmax_control(void)
{
    static uint32_t last = 0xFFFFFFFFu;
    static int logged;
    uint32_t v = bw_backend_reg(0x1D78);
    if (v != last && logged < 8) {
        logged++;
        fprintf(stderr, "[GPU-D3D11] SET_ZMIN_MAX_CONTROL = 0x%08X (cull near/far %u, zclamp %u,"
                        " ignore w %u)\n", v, v & 0xF, (v >> 4) & 0xF, (v >> 8) & 0xF);
    }
    last = v;
    return v;
}

/* Clamp z per vertex in the shader (the old way), or leave it to the
 * rasteriser per pixel. */
static int zmode_vertex_clamp(void)
{
    zminmax_control();
    return zmode() == 0;
}

/* Depth clip in the rasteriser: on for "clip", or for "auto" when the
 * title selects cull. */
static int zmode_depth_clip(void)
{
    int m = zmode();
    return m == 2 || (m == 3 && ((zminmax_control() >> 4) & 0xF) == 0);
}

static ID3D11RasterizerState *get_rs_z(int clip, int cull, int ccw)
{
    if (!s_rs_z[clip][cull][ccw]) {
        D3D11_RASTERIZER_DESC rd;
        memset(&rd, 0, sizeof(rd));
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = cull == 0 ? D3D11_CULL_NONE : cull == 1 ? D3D11_CULL_FRONT : D3D11_CULL_BACK;
        rd.FrontCounterClockwise = ccw ? TRUE : FALSE;
        rd.DepthClipEnable = clip ? TRUE : FALSE;
        rd.ScissorEnable = TRUE;
        ID3D11Device_CreateRasterizerState(s_dev, &rd, &s_rs_z[clip][cull][ccw]);
    }
    return s_rs_z[clip][cull][ccw];
}

/* Polygon offset (SET_POLY_OFFSET_FILL_ENABLE 0x0338, SET_POLYGON_OFFSET_
 * SCALE_FACTOR 0x0384, _BIAS 0x0388), a port bug fixed in every edition:
 * neither renderer applied it, and Blood Wake enables it for about 44% of
 * mission draws (factor -0.25, units -1; BW_GPU_AUDIT, 2026-10-27). xemu adds
 * units + factor * max |dz/dx|, |dz/dy| to the 24-bit depth per pixel
 * (psh.c); D3D11's rasteriser bias is the same rule (DepthBias in 2^-24
 * steps of the D24 buffer, SlopeScaledDepthBias times the max slope), except
 * that its x slope is per pixel of the 2x-wide anti-aliased surface rather
 * than per logical pixel (with factor -0.25, a fraction of one depth step).
 * BW_GPU_POLYOFFSET=0 restores the old state (no offset). */
static int use_polyoffset(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("BW_GPU_POLYOFFSET");
        v = !(e && e[0] == '0');
    }
    return v && !bw_ab_off("polyoffset");
}

static int poly_offset(float *factor, float *units)
{
    uint32_t f = bw_backend_reg(0x0384), u = bw_backend_reg(0x0388);
    if (!use_polyoffset() || !bw_backend_reg(0x0338))
        return 0;
    memcpy(factor, &f, 4);
    memcpy(units, &u, 4);
    return (*factor != 0.0f || *units != 0.0f) && *factor == *factor && *units == *units;
}

#define BW_MAX_RS_BIAS 32
static struct { int clip, cull, ccw; float factor, units; ID3D11RasterizerState *rs; } s_rs_bias[BW_MAX_RS_BIAS];
static int s_nrs_bias;

static ID3D11RasterizerState *get_rs_bias(int clip, int cull, int ccw, float factor, float units)
{
    D3D11_RASTERIZER_DESC rd;
    int i;
    for (i = 0; i < s_nrs_bias; i++)
        if (s_rs_bias[i].clip == clip && s_rs_bias[i].cull == cull && s_rs_bias[i].ccw == ccw
                && s_rs_bias[i].factor == factor && s_rs_bias[i].units == units)
            return s_rs_bias[i].rs;
    if (s_nrs_bias == BW_MAX_RS_BIAS)
        return get_rs_z(clip, cull, ccw);
    memset(&rd, 0, sizeof(rd));
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = cull == 0 ? D3D11_CULL_NONE : cull == 1 ? D3D11_CULL_FRONT : D3D11_CULL_BACK;
    rd.FrontCounterClockwise = ccw ? TRUE : FALSE;
    rd.DepthClipEnable = clip ? TRUE : FALSE;
    rd.ScissorEnable = TRUE;
    rd.DepthBias = (INT)(units < 0.0f ? units - 0.5f : units + 0.5f);
    rd.SlopeScaledDepthBias = factor;
    rd.DepthBiasClamp = 0.0f;
    if (FAILED(ID3D11Device_CreateRasterizerState(s_dev, &rd, &s_rs_bias[s_nrs_bias].rs)))
        return get_rs_z(clip, cull, ccw);
    s_rs_bias[s_nrs_bias].clip = clip; s_rs_bias[s_nrs_bias].cull = cull;
    s_rs_bias[s_nrs_bias].ccw = ccw; s_rs_bias[s_nrs_bias].factor = factor;
    s_rs_bias[s_nrs_bias].units = units;
    return s_rs_bias[s_nrs_bias++].rs;
}

static ID3D11RasterizerState *get_rs(const Nv2aRenderState *rs)
{
    int back, ccw;
    if (zmode() != 0) {
        /* the depth experiment: one cache for clip x cull x winding */
        int clip = zmode_depth_clip(), cull = 0;
        ccw = 0;
        if (s_cull < 0) {
            const char *e = getenv("BW_GPU_CULL");
            s_cull = !e ? 1 : e[0] == '0' ? 0 : (e[0] == 'f' || e[0] == '2') ? 2 : 1;
        }
        if (s_cull && rs && rs->cull_enable) {
            if (rs->cull_face == 0x408)
                return NULL;
            cull = rs->cull_face != 0x404 ? 2 : 1;
            ccw = (rs->front_face == 0x901) ^ (s_cull == 2);
        }
        {
            float factor, units;
            if (poly_offset(&factor, &units))
                return get_rs_bias(clip, cull, ccw, factor, units);
        }
        return get_rs_z(clip, cull, ccw);
    }
    if (s_cull < 0) {
        const char *e = getenv("BW_GPU_CULL");
        s_cull = !e ? 1 : e[0] == '0' ? 0 : (e[0] == 'f' || e[0] == '2') ? 2 : 1;
    }
    if (!s_cull || !rs || !rs->cull_enable)
        return s_rs;
    if (rs->cull_face == 0x408)
        return NULL;
    back = rs->cull_face != 0x404;
    ccw = (rs->front_face == 0x901) ^ (s_cull == 2);
    if (!s_rs_cull[back][ccw]) {
        D3D11_RASTERIZER_DESC rd;
        memset(&rd, 0, sizeof(rd));
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = back ? D3D11_CULL_BACK : D3D11_CULL_FRONT;
        rd.FrontCounterClockwise = ccw ? TRUE : FALSE;
        rd.DepthClipEnable = FALSE;
        rd.ScissorEnable = TRUE;
        ID3D11Device_CreateRasterizerState(s_dev, &rd, &s_rs_cull[back][ccw]);
    }
    return s_rs_cull[back][ccw] ? s_rs_cull[back][ccw] : s_rs;
}

static void release_surf(BwSurf *s)
{
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

/* 0.2.5 item 38: the guest memory a new surface is seeded from. The seed
 * reads h rows at the surface's pitch from va; where that would run past
 * the end of the guest block at va (the front buffer, 1200 KB, bound at the
 * AA surface's 5120-byte pitch asks for 2.4 MB), the rows that fit are
 * copied and the rest left black, as readback clamps its writes. *tmp is
 * the copy to free (NULL when guest memory is used as it is). NULL: no
 * seed (no memory for the copy). */
static const void *seed_src(const BwSurf *s, void **tmp)
{
    uint32_t room = xbox_ContiguousBlockSize(s->va);
    uint64_t need = (uint64_t)(s->h ? s->h - 1 : 0) * s->pitch + (uint64_t)s->w * s->bpp;
    uint8_t *c;
    *tmp = NULL;
    if (!room || need <= room)
        return GUEST(s->va);
    c = (uint8_t *)calloc((size_t)s->pitch, s->h);     /* pitch x h >= need > room */
    if (!c)
        return NULL;
    memcpy(c, GUEST(s->va), room);
    {
        static unsigned n;
        if (n++ < 4)
            fprintf(stderr, "[GPU-D3D11] seed of surface %08X clamped: %u rows at pitch %u would read"
                            " past the end of its %u-byte guest block; the rest starts black\n",
                    s->va, s->h, s->pitch, room);
    }
    *tmp = c;
    return c;
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
        const void *src = seed_src(s, &tmp);                    /* item 38: never past the block */
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
    if (s_cur == s)
        s_cur = NULL;           /* rebind: the blit changed the targets, and these are new */
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
            && (init.pSysMem = seed_src(s, &tmp)) != NULL) {
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

static BwSurf *find_surf(uint32_t va)
{
    for (int i = 0; i < s_nsurf; i++)
        if (s_surf[i].va == va && s_surf[i].tex)
            return &s_surf[i];
    return NULL;
}

/* ---- the output size: one place -----------------------------------------
 * D9's bw_vfx_surface_size gives a guest surface's physical size at the
 * render scale (the guest's own size in Original). In a widescreen boot
 * (Remastered, bw_widescreen.h) the device's full-screen targets, the
 * 640x480 front buffer and the 2x-wide anti-aliased back buffer (1280x480),
 * are then made k times wider; their depth buffers follow (get_depth takes
 * pw, ph). Shadow maps, reflections and every other target keep their size.
 * Positions stay in guest units (the shaders' surf constant is w, h) and the
 * D3D11 viewport covers pw, ph, so 3D stretches over the wider surface; 2D is
 * pillarboxed by the wsx remap (ws_remap). The scissor scales by pw/w. */
static int ws_is_device_target(uint32_t w, uint32_t h)
{
    return h == 480 && (w == 640 || w == 1280);
}

static void surf_physical_size(BwSurf *s, const Nv2aSurface *in)
{
    bw_vfx_surface_size(s->w, s->h, in->aa_sx, in->aa_sy, &s->pw, &s->ph);   /* VFX (D9) */
    s->wsk = 1.0f;
    if (s_ws_k > 1.0f && ws_is_device_target(s->w, s->h)) {
        uint32_t base = s->pw;
        s->pw = ((uint32_t)((float)base * s_ws_k + 1.0f)) & ~1u;
        if (s->pw > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION)      /* item 36: D3D11's limit, 16384 */
            s->pw = D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
        s->wsk = (float)s->pw / (float)base;
    }
}

/* 0.2.5 item 37: the slot tables (BW_MAX_SURF colour surfaces and as many
 * depth buffers) used to fill for good, and every draw into a new target
 * was dropped from then on, silently. Now a new target takes a free slot,
 * else the one used longest ago (never the bound one), released; the first
 * time is logged, later ones are counted in the BW_GPU_STATS line. A
 * reused colour surface's guest address seeds it again if the game comes
 * back to it, as at its first use. */
static void vfx_surf_forget(BwSurf *s);        /* the scene copy's pointers, below */

/* Test aid, BW_GPU_SLOTS=<n> (2..32): both tables hold only n, so a short
 * run exercises the reuse. Unset: BW_MAX_SURF, as always. */
static int slot_cap(void)
{
    static int cap;
    if (!cap) {
        const char *e = getenv("BW_GPU_SLOTS");
        int n = e ? atoi(e) : 0;
        cap = (n >= 2 && n < BW_MAX_SURF) ? n : BW_MAX_SURF;
        if (cap != BW_MAX_SURF)
            fprintf(stderr, "[GPU-D3D11] test aid: %d colour surface and depth buffer slots"
                            " (BW_GPU_SLOTS)\n", cap);
    }
    return cap;
}

static BwSurf *surf_slot(uint32_t want_va)
{
    BwSurf *old = NULL;
    if (s_nsurf < slot_cap())
        return &s_surf[s_nsurf++];
    for (int i = 0; i < s_nsurf; i++)
        if (!s_surf[i].tex && &s_surf[i] != s_cur)
            return &s_surf[i];              /* empty: one item 36 left to retry */
    for (int i = 0; i < s_nsurf; i++) {
        BwSurf *c = &s_surf[i];
        if (c == s_cur)
            continue;
        if (!old || c->used_gen < old->used_gen)
            old = c;
    }
    if (!old)
        return NULL;
    if (!s_surf_reused++)
        fprintf(stderr, "[GPU-D3D11] all %d colour surface slots are in use: %08X (%ux%u, last used"
                        " frame %u, now %u) makes room for %08X. Later reuses are counted\n",
                slot_cap(), old->va, old->w, old->h, old->used_gen, s_gen, want_va);
    release_surf(old);
    vfx_surf_forget(old);
    return old;
}

static BwDepth *depth_slot(uint32_t want_va)
{
    BwDepth *old = NULL;
    for (int i = 0; i < s_ndepth; i++)
        if (!s_depth[i].va)
            return &s_depth[i];             /* item 36: a slot a failure left empty */
    if (s_ndepth < slot_cap())
        return &s_depth[s_ndepth++];
    for (int i = 0; i < s_ndepth; i++) {
        BwDepth *c = &s_depth[i];
        if (c == s_cur_depth)
            continue;
        if (!old || c->used_gen < old->used_gen)
            old = c;
    }
    if (!old)
        return NULL;
    if (!s_depth_reused++)
        fprintf(stderr, "[GPU-D3D11] all %d depth buffer slots are in use: %08X (%ux%u, last used"
                        " frame %u, now %u) makes room for %08X. Later reuses are counted\n",
                slot_cap(), old->va, old->w, old->h, old->used_gen, s_gen, want_va);
    if (old->dsv) ID3D11DepthStencilView_Release(old->dsv);
    if (old->srv) ID3D11ShaderResourceView_Release(old->srv);
    if (old->tex) ID3D11Texture2D_Release(old->tex);
    memset(old, 0, sizeof(*old));
    return old;
}

static BwSurf *get_surf(const Nv2aSurface *in)
{
    uint32_t bpp = in->bytes_per_pixel ? in->bytes_per_pixel : 4;
    uint32_t w = in->pitch && in->pitch / bpp >= in->width ? in->pitch / bpp : in->width;
    uint32_t h = in->height;
    BwSurf *s = NULL;
    for (int i = 0; i < s_nsurf; i++)
        if (s_surf[i].va == in->color_va) { s = &s_surf[i]; break; }
    if (s && (s->bpp != bpp || s->w < w || s->h < h || !s->tex
              || (bw_vfx_active() && s->epoch != bw_vfx_epoch()))) {   /* VFX (D9): rescaled */
        uint32_t ow = s->w, oh = s->h, obpp = s->bpp, opitch = s->pitch;
        if (!s->tex && s->fail_gen == s_gen)
            return NULL;                    /* item 36: it couldn't be made this frame; next frame */
        release_surf(s);
        /* 0.2.5 item 38 (a port bug, every edition): the bound surface,
         * re-made, must be bound again. begin only binds when the surface
         * changes, so the context kept the released target and the draws
         * after a re-make (the front buffer grows when the game binds it at
         * the AA surface's pitch) went into the old texture, never shown. */
        if (s_cur == s)
            s_cur = NULL;
        s->bpp = bpp;
        s->w = w > s->w ? w : s->w;
        s->h = h > s->h ? h : s->h;
        s->pitch = in->pitch;
        surf_physical_size(s, in);
        if (!create_surf(s)) {
            s->fail_gen = s_gen;            /* item 36: released; kept by address to retry */
            return NULL;
        }
        if (s->w != ow || s->h != oh || s->bpp != obpp) {
            static unsigned n;              /* item 38: a fact for bug reports (a few, then quiet) */
            if (n++ < 8)
                fprintf(stderr, "[GPU-D3D11] surface %08X re-made at frame %u: %ux%u %u-byte"
                                " (pitch %u) -> %ux%u %u-byte (pitch %u), bound again\n",
                        s->va, s_gen, ow, oh, obpp, opitch, s->w, s->h, s->bpp, s->pitch);
        }
    }
    if (!s) {
        if (!(s = surf_slot(in->color_va)))  /* item 37: a free slot, or the oldest one's */
            return NULL;
        memset(s, 0, sizeof(*s));
        s->va = in->color_va;
        s->pitch = in->pitch;
        s->bpp = bpp;
        s->w = w;
        s->h = h;
        surf_physical_size(s, in);          /* VFX (D9) render scale, widescreen k */
        if (!create_surf(s)) {
            /* item 36: kept (empty, never drawn with: find_surf wants a
             * texture) so it is tried once a frame, not once a draw */
            s->fail_gen = s_gen;
            return NULL;
        }
        if (s->wsk > 1.0f)
            fprintf(stderr, "[WS] surface %08X %ux%u -> %ux%u on the GPU (widescreen x%.4f)\n",
                    s->va, s->w, s->h, s->pw, s->ph, s->wsk);
    }
    s->cur_pitch = in->pitch;
    s->aa_sx = (uint8_t)(in->aa_sx ? in->aa_sx : 1);
    return s;
}

static void release_depth(BwDepth *d)
{
    if (d->dsv) ID3D11DepthStencilView_Release(d->dsv);
    if (d->srv) ID3D11ShaderResourceView_Release(d->srv);
    if (d->tex) ID3D11Texture2D_Release(d->tex);
    d->dsv = NULL; d->srv = NULL; d->tex = NULL;
}

/* The depth buffer at guest address va for a colour target of w x h with
 * `samples` samples per pixel (1 in Original; the MSAA count in Remastered,
 * or 1 for a surface whose MSAA target couldn't be made, item 36). */
static BwDepth *get_depth(uint32_t va, uint32_t w, uint32_t h, UINT samples)
{
    D3D11_TEXTURE2D_DESC td;
    BwDepth *d = NULL;
    HRESULT hr;
    int kind = RF_DEPTH;
    static uint32_t fail_va, fail_gen;     /* item 36: the last one that couldn't be made, and when */
    if (!va)
        return NULL;
    for (int i = 0; i < s_ndepth; i++) {
        BwDepth *c = &s_depth[i];
        if (c->va != va)
            continue;
        if (!bw_vfx_active()) {
            d = c;                          /* Original: one per address, grown to the largest */
            break;
        }
        /* 0.2.5 item 39, Remastered: one per address, size and sample
         * count, so a depth address used with two sizes of target keeps one
         * of each instead of being re-made at every switch. One made for an
         * earlier render scale is released here. */
        if (c->epoch != bw_vfx_epoch()) {
            if (c == s_cur_depth) {
                s_cur_depth = NULL;
                s_cur = NULL;
            }
            release_depth(c);
            memset(c, 0, sizeof(*c));
            continue;
        }
        if (c->w == w && c->h == h && c->samples == samples) {
            d = c;
            break;
        }
    }
    if (d && d->samples == samples && d->w >= w && d->h >= h
            && (!bw_vfx_active() || (d->epoch == bw_vfx_epoch() && d->w == w && d->h == h)))
        return d;
    if (va == fail_va && s_gen == fail_gen)
        return NULL;                        /* item 36: tried this frame already; next frame */
    if (d) {
        release_depth(d);
        if (d == s_cur_depth) {
            /* item 38: the bound depth buffer, re-made, is bound again (begin
             * binds when the pair changes; clearing s_cur makes it) */
            s_cur_depth = NULL;
            s_cur = NULL;
        }
        if (!bw_vfx_active()) {
            w = w > d->w ? w : d->w;
            h = h > d->h ? h : d->h;
        }
        {
            static unsigned n;              /* item 39: a fact for bug reports (a few, then quiet) */
            if (n++ < 8)
                fprintf(stderr, "[GPU-D3D11] depth buffer %08X re-made at frame %u: %ux%u x%u -> %ux%u x%u"
                                " samples\n", va, s_gen, d->w, d->h, d->samples, w, h, samples);
        }
    } else if (!(d = depth_slot(va)))       /* items 36-37: a free slot, or the oldest one's */
        return NULL;
    memset(d, 0, sizeof(*d));
    d->va = va; d->w = w; d->h = h; d->samples = samples;
    memset(&td, 0, sizeof(td));
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    td.SampleDesc.Count = samples;              /* VFX (D9): matches the colour target; 1 in Original */
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    d->epoch = bw_vfx_epoch();
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
        return d;
    }
    if (FAILED(hr = BW_GPU_FT("depth", E_OUTOFMEMORY,          /* item 25 test aid */
                              ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &d->tex))))
        goto fail;
    kind = RF_DEPTH_VIEW;
    if (FAILED(hr = BW_GPU_FT("resource", E_OUTOFMEMORY,       /* item 36 */
                              ID3D11Device_CreateDepthStencilView(s_dev, (ID3D11Resource *)d->tex, NULL, &d->dsv))))
        goto fail;
    return d;
fail:
    /* item 36: nothing half-made stays in a live slot (a NULL view used to
     * be released and cleared later); the slot is left empty for reuse */
    res_failed(kind, va, w, h, hr);
    release_depth(d);
    memset(d, 0, sizeof(*d));
    if (d == &s_depth[s_ndepth - 1])
        s_ndepth--;
    fail_va = va;
    fail_gen = s_gen;
    return NULL;
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
    fprintf(stderr, "[FLIP] %u shown %08X (%ux%u) crtc %08X:%s\n", s_gen,
            shown ? shown->va : 0, shown ? shown->w : 0, shown ? shown->h : 0,
            crtc ? *crtc : 0, s_ft);
    s_ft_len = 0;
    s_ft[0] = 0;
}

static void bind_targets(BwSurf *s, BwDepth *d)
{
    D3D11_VIEWPORT vp;
    ID3D11ShaderResourceView *none = NULL;
    /* A surface about to be drawn into must not stay bound as a texture. */
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &none);
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, s->mrtv ? &s->mrtv : &s->rtv, d ? d->dsv : NULL);
    vp.TopLeftX = 0; vp.TopLeftY = 0;
    vp.Width = (float)s->pw; vp.Height = (float)s->ph;      /* VFX (D9): physical; = w, h in Original */
    vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
    if (s != s_cur)
        ft_note('B', s->va);
    s_cur = s;
    s_cur_depth = d;
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
    if (s_cur)
        bind_targets(s_cur, s_cur_depth);
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
} VfxRb;
static VfxRb s_vfx_rb[VFX_RB_RING];
static unsigned s_vfx_rb_next;

static void vfx_rb_write(VfxRb *r, const D3D11_MAPPED_SUBRESOURCE *m)
{
    /* As readback() below: the latest binding's pitch, never past the end
     * of the guest block (the mission crash of 2026-10-26). */
    uint32_t row = r->w * r->bpp, rows = r->h, pitch = r->pitch ? r->pitch : row, y;
    uint32_t room = xbox_ContiguousBlockSize(r->va);
    if (row > pitch) row = pitch;
    if (room) {
        if (row > room) { row = room; rows = 1; }
        else if ((uint64_t)(rows - 1) * pitch + row > room) rows = (room - row) / pitch + 1;
    }
    for (y = 0; y < rows; y++)
        memcpy(GUEST(r->va) + (size_t)y * pitch, (uint8_t *)m->pData + (size_t)y * m->RowPitch, row);
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
    return v && bw_vfx_active() && !nv2a_pb_exec_flip_dumping();
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
    s_vfx_rb_next = (s_vfx_rb_next + 1) % VFX_RB_RING;
}

/* Copy a rendered surface back into guest memory. */
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
    if (vfx_rb_async()) {                  /* VFX (D9): Remastered, no wait */
        vfx_readback_async(s, src);
        s->dirty = 0;
        return;
    }
    for (unsigned k = 0; k < VFX_RB_RING; k++)
        s_vfx_rb[k].pending = 0;           /* VFX: older queued frames must not land after this one */
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
        uint32_t room = xbox_ContiguousBlockSize(s->va);
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
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_staging, 0);
    } else if (gpu_lost(hr, "the readback's Map")) {   /* item 28 */
        return;
    }
    s->dirty = 0;
}

/* ---- widescreen: 2D markers, the per-batch x remap, wide dumps ------------
 *
 * The game-side hooks (bw_remaster_hooks.c) write a tagged NV097 NOP before
 * and after each overlay layer and each movie frame; the carried executor
 * hands them here in stream order (bw_set_nop_marker_handler). Inside them a
 * draw is 2D: pillarboxed around its view's centre, so it keeps its shape,
 * except an untextured quad covering the whole view (a fade, a flash), which
 * stretches to fill the wide view. Everything else is 3D and stretches
 * (wsx = 1, 0); the game's own projection is already k times wider (Hor+).
 * All of it runs on the executor's thread, as the draws do. */
extern void bw_set_nop_marker_handler(void (*fn)(uint32_t));

/* 2D is a flag, not a nesting depth: BEGIN sets it, END clears it, and every
 * flip clears it too (no frame ends inside an overlay flush), so a marker
 * that goes missing can't turn later 3D into 2D for longer than one frame.
 * A lost END was seen (run ...-mk10, flip 301) before TK-38 was fixed. */
static int s_ws_2d;             /* inside 2D markers */
static int s_ws_cut43;          /* this view's 3D is 4:3 (the 4:3-cutscene option) */
static int s_ws_full;           /* this batch: an untextured full-view 2D quad */
static uint32_t s_ws_what;      /* the last BEGIN marker's argument (BW_WS_WHAT_*) */
static unsigned s_ws_markers, s_ws_2d_draws;
static unsigned s_ws_rx[4], s_ws_unpaired;   /* markers received per op; frames ending in 2D */
static int s_ws_log = -1;       /* BW_WS_LOG=<n>: the first n 2D batches, for checking */

static void ws_marker(uint32_t p)
{
    uint32_t op = (p >> 8) & 0xFF, arg = p & 0xFF;
    if (!s_ws_markers++)
        fprintf(stderr, "[WS] first in-stream marker %08X\n", p);
    if (s_ws_log > 0 && getenv("BW_WS_LOG_MARKERS"))
        fprintf(stderr, "[WS] f%u marker op %u arg %u in2d %d\n", s_gen, op, arg, s_ws_2d);
    if (op < 4)
        s_ws_rx[op]++;
    if (op == BW_WS_OP_2D_BEGIN) {
        s_ws_2d = 1;
        s_ws_what = arg;
    } else if (op == BW_WS_OP_2D_END) {
        s_ws_2d = 0;
    } else if (op == BW_WS_OP_3D_43)
        s_ws_cut43 = arg != 0;
}

/* At each flip: no frame ends in 2D; a summary every 30 s when logging. */
static void ws_frame_end(void)
{
    static unsigned next = 900;
    if (s_ws_2d) {
        s_ws_unpaired++;
        s_ws_2d = 0;
    }
    if (s_ws_k > 1.0f && s_gen >= next) {
        next = s_gen + 900;
        fprintf(stderr, "[WS] frame %u: markers received BEGIN %u END %u, unpaired %u, "
                "2D draws %u\n", s_gen, s_ws_rx[1], s_ws_rx[2], s_ws_unpaired, s_ws_2d_draws);
    }
}

static void ws_summary(void)
{
    fprintf(stderr, "[WS] markers received: BEGIN %u END %u, frames ending in 2D %u, "
            "2D draws %u\n", s_ws_rx[1], s_ws_rx[2], s_ws_unpaired, s_ws_2d_draws);
}

/* main.c, after bw_ws_init() and before the game runs: this boot's factor.
 * k == 1 (Original, the menus, -WIDESCREEN) registers nothing. */
void bw_d3d11_ws_start(void)
{
    s_ws_k = bw_ws_k();
    if (s_ws_k > 1.0f) {
        bw_set_nop_marker_handler(ws_marker);
        bw_edition_add_summary(ws_summary);
    }
}

/* The surface's width in logical pixels: the bound surface's guest width
 * (what the shaders normalise by) once it is bound, else the width get_surf
 * will give it. */
static float ws_logical_w(const Nv2aSurface *s)
{
    uint32_t sx = s->aa_sx ? s->aa_sx : 1;
    uint32_t bpp = s->bytes_per_pixel ? s->bytes_per_pixel : 4;
    uint32_t w = s->pitch && s->pitch / bpp >= s->width ? s->pitch / bpp : s->width;
    if (s_cur && s_cur->va == s->color_va)
        w = s_cur->w;
    return (float)w / (float)sx;
}

/* The current view's horizontal extent in logical pixels: the window clip D3D
 * sets at every SetViewport (as apply_scissor reads it), else the surface. */
static void ws_view(const Nv2aSurface *s, float *x0, float *x1)
{
    uint32_t h = bw_backend_reg(0x02C0);
    float lw = ws_logical_w(s);
    *x0 = 0.0f;
    *x1 = lw;
    if (h && bw_backend_reg(0x02B4) == 0) {
        float a = (float)(h & 0xFFF), b = (float)((h >> 16) & 0xFFF) + 1.0f;
        if (b > a) {
            *x0 = a;
            *x1 = b < lw ? b : lw;
        }
    }
}

/* lo..hi (logical pixels, or surface pixels when real) covers the view. */
static int ws_covers_view(const Nv2aSurface *s, float lo, float hi, int real)
{
    float x0, x1, sx = s->aa_sx ? (float)s->aa_sx : 1.0f;
    if (real) {
        lo /= sx;
        hi /= sx;
    }
    ws_view(s, &x0, &x1);
    return lo <= x0 + 1.0f && hi >= x1 - 1.0f;
}

/* The view's height in logical pixels (window clip, else 480). */
static float ws_view_h(const Nv2aSurface *s)
{
    uint32_t v = bw_backend_reg(0x02E0);
    (void)s;
    if (v && bw_backend_reg(0x02B4) == 0) {
        float a = (float)(v & 0xFFF), b = (float)((v >> 16) & 0xFFF) + 1.0f;
        if (b > a)
            return b - a;
    }
    return 480.0f;
}

/* A full-view 2D quad is a backdrop (stretch it to the wide view) unless it
 * shows a picture. A picture (a loading screen, a menu background) maps its
 * texels about square onto the screen, at a texel or so per pixel. A backdrop
 * is untextured, or a swatch (at most one texel per 10 pixels across), or a
 * texture squeezed far out of square. The pause screen's darkening is both:
 * a 640x480 quad over 15 x 120 texels of a 512x1024 overlay page, and every
 * 4th frame over the whole page (seen, runs ...-p15, -p17uw). u/v ranges in
 * texels, or in 0..1 when normalized. */
static int ws_backdrop(const Nv2aSurface *s, int textured, int normalized, uint32_t tw,
                       uint32_t th, float u0, float u1, float v0, float v1)
{
    float su = u1 - u0, sv = v1 - v0, x0, x1, vw, vh, r;
    if (!textured)
        return 1;
    if (normalized) {
        su *= (float)tw;
        sv *= (float)th;
    }
    ws_view(s, &x0, &x1);
    vw = x1 - x0;
    vh = ws_view_h(s);
    if (su <= 8.0f || su <= 0.1f * vw)
        return 1;                               /* a swatch */
    if (sv <= 0.0f || vh <= 0.0f)
        return 0;
    r = (su / sv) / (vw / vh);                  /* 1: square texels on screen */
    return r < 0.8f || r > 1.25f;
}

static float s_ws_span_u, s_ws_span_v;   /* the last full-view batch's texel span (log) */

/* A 2D draw that samples a widened surface is a copy of the screen (the
 * pause screen's frozen scene: a full-screen triangle over the 1280x480 back
 * buffer, seen in run ...-pause8): it stretches like the 3D it copies. */
static int ws_samples_wide(int textured, uint32_t offset)
{
    BwSurf *rt;
    if (!textured)
        return 0;
    rt = find_surf(offset);
    return rt && rt->wsk > 1.0f;
}

/* wsx for this batch: (1, 0) stretches; (1/kx, c(1 - 1/kx)) pillarboxes
 * around the view's centre c (NDC), which maps guest x to host x + c'(kx - 1)
 * at the guest's own scale. */
static void ws_remap(const Nv2aSurface *s, float out[4], int full)
{
    float kx, x0, x1, c;
    out[0] = 1.0f;
    out[1] = out[2] = out[3] = 0.0f;
    if (s_ws_k <= 1.0f || !s_cur || s_cur->wsk <= 1.0f)
        return;
    if (!((s_ws_2d > 0 && !full) || s_ws_cut43))
        return;
    kx = s_cur->wsk;
    ws_view(s, &x0, &x1);
    c = (x0 + x1) / ws_logical_w(s) - 1.0f;
    out[0] = 1.0f / kx;
    out[1] = c * (1.0f - out[0]);
    s_ws_2d_draws++;
}

static void ws_note(char path, uint32_t n, int tex, uint32_t tw, uint32_t th, float lo, float hi,
                    const Nv2aSurface *s)
{
    static unsigned from;               /* BW_WS_LOG_FROM=<flip>: start there */
    float x0, x1;
    if (s_ws_log < 0) {
        const char *e = getenv("BW_WS_LOG");
        s_ws_log = e && *e ? atoi(e) : 0;
        e = getenv("BW_WS_LOG_FROM");
        from = e && *e ? (unsigned)atoi(e) : 0;
    }
    if (s_ws_log <= 0 || s_ws_2d <= 0 || s_gen < from)
        return;
    s_ws_log--;
    ws_view(s, &x0, &x1);
    fprintf(stderr, "[WS] f%u 2D L%u %c n%u tex%d %ux%u uv span %.3g x %.3g x %.1f..%.1f view %.0f..%.0f"
            " aa%u full%d surf %08X\n", s_gen, s_ws_what, path, n, tex, tex ? tw : 0, tex ? th : 0,
            s_ws_span_u, s_ws_span_v, lo, hi, x0, x1, s->aa_sx, s_ws_full, s_cur ? s_cur->va : 0);
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

    if (s_ws_k <= 1.0f || !sf || sf->wsk <= 1.0f || !sf->tex)
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

/* Bits per texel of the NV097 colour formats this title is likely to use; 0
 * for DXT (handled by block size). Unknown codes hash as 32 bpp. */
static uint32_t tex_bpp(uint32_t color, int *dxt)
{
    *dxt = 0;
    switch (color) {
    case 0x00: case 0x01: case 0x0B: case 0x13: case 0x19: case 0x1B: case 0x1F:
        return 8;
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x10: case 0x11: case 0x1A:
    case 0x1C: case 0x1D: case 0x20: case 0x27: case 0x28: case 0x29: case 0x30:
        return 16;
    case 0x0C: *dxt = 8; return 0;        /* DXT1: 8 bytes per 4x4 */
    case 0x0E: case 0x0F: *dxt = 16; return 0;
    default:
        return 32;
    }
}

static uint64_t hash_bytes(const uint8_t *p, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t v;
        memcpy(&v, p + i, 8);
        h = (h ^ v) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    for (; i < n; i++)
        h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

static size_t tex_bytes(const Nv2aTexture *t)
{
    int dxt;
    uint32_t bpp = tex_bpp(t->color, &dxt);
    size_t n;
    if (t->pitch)
        n = (size_t)t->pitch * t->height;
    else if (dxt)
        n = (size_t)((t->width + 3) / 4) * ((t->height + 3) / 4) * dxt;
    else
        n = (size_t)t->width * t->height * bpp / 8;
    return n > (64u << 20) ? 0 : n;
}

static uint64_t tex_hash(const Nv2aTexture *t)
{
    size_t n = tex_bytes(t);
    if (!n)
        return 0;
    return hash_bytes(GUEST(t->offset), n) ^ ((uint64_t)t->color << 56);
}

/* 128 eight-byte samples spread over the texture: what a texture that has
 * not changed for a while is checked with each frame, instead of hashing all
 * of it. A full check still runs every 32 frames. */
static uint64_t tex_sparse(const Nv2aTexture *t)
{
    size_t n = tex_bytes(t), i;
    const uint8_t *p = GUEST(t->offset);
    uint64_t h = 1469598103934665603ull;
    if (n < 8)
        return 0;
    for (i = 0; i < 128; i++) {
        uint64_t v;
        size_t off = ((n - 8) * i / 127) & ~(size_t)7;
        memcpy(&v, p + off, 8);
        h = (h ^ v) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    return h;
}

/* Upload without the per-texel decoder where the layout allows: DXT blocks
 * go to the GPU as they are (BC1-3 are the same S3TC blocks), the common
 * swizzled and linear 32-bit and R5G6B5 formats are unswizzled or copied in
 * bulk. Returns 0 to fall back to nv2a_backend_decode_texture. */
static int s_mips = -1;         /* BW_GPU_MIPS=0: level 0 only, as the CPU path */

static int use_mips(void)
{
    if (s_mips < 0) {
        const char *e = getenv("BW_GPU_MIPS");
        s_mips = !(e && e[0] == '0');
    }
    return s_mips;
}

/* How many levels to upload: what the texture says it carries, down to 1x1,
 * at most 12. */
static uint32_t mip_count(const Nv2aTexture *t)
{
    uint32_t n = 1, w = t->width, h = t->height;
    uint32_t want = (use_mips() && !t->pitch && t->levels > 1) ? t->levels : 1;
    while (n < want && n < 12 && (w > 1 || h > 1)) {
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
        n++;
    }
    return n;
}

/* VFX (V1, VFX_HUD_EDGES): is a DXT3/DXT5 texture's alpha hard-edged:
 * opaque shapes with only a thin anti-aliased rim (the radar's compass ring,
 * weapon and bar icons), as opposed to glows and translucent art (meters, the
 * radar lens). Decided from level 0's alpha blocks: at least 3% of the texels
 * opaque (alpha >= 215) and at most half as many partial ones (41..214).
 * Checked on the 53 DXT5 overlays: 23 qualify (docs/day1/V1.md). */
static int tex_hard_alpha(uint32_t color, const uint8_t *q, uint32_t w, uint32_t h)
{
    uint32_t nb, i, k, opaque = 0, partial = 0, total;
    if ((color != 0x0E && color != 0x0F) || !q || w < 4 || h < 4 || w > 512 || h > 512)
        return 0;
    nb = ((w + 3) / 4) * ((h + 3) / 4);
    total = nb * 16;
    for (i = 0; i < nb; i++, q += 16) {
        uint8_t pal[8];
        uint64_t bits = 0;
        for (k = 0; k < 16; k++) {
            uint32_t a;
            if (color == 0x0E) {                    /* DXT3: explicit 4-bit alpha */
                a = (q[k >> 1] >> ((k & 1) * 4)) & 0xF;
                a = a * 17;
            } else {                                /* DXT5: interpolated alpha */
                if (k == 0) {
                    uint32_t a0 = q[0], a1 = q[1], j;
                    pal[0] = (uint8_t)a0; pal[1] = (uint8_t)a1;
                    if (a0 > a1)
                        for (j = 1; j < 7; j++) pal[j + 1] = (uint8_t)(((7 - j) * a0 + j * a1) / 7);
                    else {
                        for (j = 1; j < 5; j++) pal[j + 1] = (uint8_t)(((5 - j) * a0 + j * a1) / 5);
                        pal[6] = 0; pal[7] = 255;
                    }
                    for (j = 0; j < 6; j++) bits |= (uint64_t)q[2 + j] << (8 * j);
                }
                a = pal[(bits >> (3 * k)) & 7];
            }
            if (a >= 215) opaque++;
            else if (a > 40) partial++;
        }
    }
    return opaque * 32 >= total && partial * 2 <= opaque;
}

/* Upload without the per-texel decoder where the layout allows: DXT blocks
 * go to the GPU as they are (BC1-3 are the same S3TC blocks), the common
 * swizzled and linear 32-bit and R5G6B5 formats are unswizzled or copied in
 * bulk. Swizzled and DXT textures keep their mip levels after level 0, each
 * laid out like a texture of its own size; those are uploaded too.
 * Fills init[] and td->MipLevels; returns 0 to fall back to
 * nv2a_backend_decode_texture (level 0 only). */
static int fast_texture(const Nv2aTexture *t, D3D11_TEXTURE2D_DESC *td,
                        D3D11_SUBRESOURCE_DATA *init)
{
    uint32_t w = t->width, h = t->height, x, y, lv, levels = mip_count(t);
    const uint8_t *src = GUEST(t->offset);
    uint8_t *dst = (uint8_t *)s_decode;
    switch (t->color) {
    case 0x0C: case 0x0E: case 0x0F: {              /* DXT1, DXT3, DXT5 */
        uint32_t bb = t->color == 0x0C ? 8 : 16;
        if ((w & 3) || (h & 3))
            return 0;
        td->Format = t->color == 0x0C ? DXGI_FORMAT_BC1_UNORM
                   : t->color == 0x0E ? DXGI_FORMAT_BC2_UNORM : DXGI_FORMAT_BC3_UNORM;
        for (lv = 0; lv < levels; lv++) {
            uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
            init[lv].pSysMem = src;
            init[lv].SysMemPitch = bw * bb;
            init[lv].SysMemSlicePitch = 0;
            src += (size_t)bw * bh * bb;
            w = w > 1 ? w / 2 : 1;
            h = h > 1 ? h / 2 : 1;
        }
        td->MipLevels = levels;
        return 1;
    }
    case 0x06: case 0x07: case 0x05: {             /* A8R8G8B8, X8R8G8B8, R5G6B5 (swizzled) */
        uint32_t bpp = t->color == 0x05 ? 2 : 4;
        if ((w & (w - 1)) || (h & (h - 1)))
            return 0;
        for (lv = 0; lv < levels; lv++) {
            xbox_unswizzle_rect(dst, src, w, h, bpp);
            if (t->color == 0x07)
                for (x = 0; x < w * h; x++)
                    ((uint32_t *)dst)[x] |= 0xFF000000u;
            init[lv].pSysMem = dst;
            init[lv].SysMemPitch = w * bpp;
            init[lv].SysMemSlicePitch = 0;
            src += (size_t)w * h * bpp;
            dst += (size_t)w * h * bpp;
            w = w > 1 ? w / 2 : 1;
            h = h > 1 ? h / 2 : 1;
        }
        td->Format = bpp == 2 ? DXGI_FORMAT_B5G6R5_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
        td->MipLevels = levels;
        return 1;
    }
    case 0x12: case 0x1E:                           /* LIN_A8R8G8B8, LIN_X8R8G8B8 */
        if (!t->pitch || t->pitch < w * 4)
            return 0;
        for (y = 0; y < h; y++) {
            memcpy(s_decode + (size_t)y * w, src + (size_t)y * t->pitch, w * 4);
            if (t->color == 0x1E)
                for (x = 0; x < w; x++)
                    s_decode[(size_t)y * w + x] |= 0xFF000000u;
        }
        td->Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        init[0].pSysMem = s_decode;
        init[0].SysMemPitch = w * 4;
        td->MipLevels = 1;
        return 1;
    default:
        return 0;
    }
}

/* BW_GPU_BLACKWATCH=<prefix>: what each frame sampled, to explain a frame
 * that suddenly goes black (see bw_flip). Kinds: R a surface this back end
 * drew (render to texture), F the current target (feedback, read from guest
 * memory), U uploaded this frame, H cached (full hash equal), S cached
 * (sampled check), G cached (already checked this frame). */
typedef struct { uint32_t offset, w, h, color; char kind; int zero_pct; } BwTexUse;
#define BW_MAX_USE 96
static BwTexUse s_use[2][BW_MAX_USE];
static int s_nuse[2];
static int s_watch = -1;
static const char *s_watch_prefix;

/* Giant triangles per frame: a vertex more than 4 surfaces away from the
 * surface, or with 1/w not positive and finite. [slot] as s_use. */
static unsigned s_huge[2], s_tris_frame[2];
static BwXVertex s_huge_v[2][3];

static void note_huge(const BwXVertex *v, uint32_t n, uint32_t w, uint32_t h)
{
    int slot = s_gen & 1;
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

static void note_use(const Nv2aTexture *t, char kind)
{
    BwTexUse *u = s_use[s_gen & 1];
    int *n = &s_nuse[s_gen & 1], i;
    if (s_watch <= 0)
        return;
    for (i = 0; i < *n; i++)
        if (u[i].offset == t->offset && u[i].kind == kind)
            return;
    if (*n < BW_MAX_USE) {
        u[*n].offset = t->offset; u[*n].w = t->width; u[*n].h = t->height;
        u[*n].color = t->color; u[*n].kind = kind;
        u[*n].zero_pct = -1;
        if (kind == 'U') {
            /* how much of a fresh upload is zero bytes (a texture caught
             * mid-rewrite?), sampled at 256 points of level 0 */
            size_t nb = tex_bytes(t), k, z = 0;
            const uint8_t *q = GUEST(t->offset);
            if (nb >= 256) {
                for (k = 0; k < 256; k++)
                    z += q[(nb - 1) * k / 255] == 0;
                u[*n].zero_pct = (int)(z * 100 / 256);
            }
        }
        (*n)++;
    }
}

static ID3D11ShaderResourceView *get_texture_(const Nv2aTexture *t, char *kind);
static ID3D11ShaderResourceView *get_cube_texture_(const Nv2aTexture *t, char *kind);
/* T1: the cache entry get_texture_ last returned (NULL for a render target
 * or a failure); read only by the texture restoration (Remastered). */
static BwTex *s_tex_last;
static int tr_is_movie(const Nv2aTexture *t);

/* 0.2.5 item 43: the cache entry with this texture's identity (address,
 * size, format, pitch; 2D or cube), or NULL. It was a scan of every entry
 * for every stage of every draw (about 125 in mission 1); now a
 * direct-mapped index of the last hit for each identity is tried first. An
 * index slot is only a hint, checked against the entry it names, so a slot
 * left by an entry since released or refilled just misses (and the scan
 * runs, as before). There are never two entries with one identity: a new
 * entry is made only when the scan finds none. */
#define TEX_IDX 1024
static int16_t s_tex_idx[TEX_IDX];      /* entry + 1; 0: none */

static int tex_is(const BwTex *c, const Nv2aTexture *t, int cube)
{
    return c->offset == t->offset && c->w == t->width && c->h == t->height
        && c->color == t->color && c->pitch == t->pitch && c->cube == (uint8_t)cube;
}

static BwTex *tex_find(const Nv2aTexture *t, int cube)
{
    uint32_t h = (t->offset >> 7) ^ (t->offset >> 19) ^ (t->width * 31u) ^ (t->height * 131u)
               ^ (t->color << 5) ^ (t->pitch * 7u) ^ (uint32_t)cube;
    int k;
    h &= TEX_IDX - 1;
    k = s_tex_idx[h];
    if (k && memo_on() && k <= s_ntex && tex_is(&s_tex[k - 1], t, cube))
        return &s_tex[k - 1];
    for (int i = 0; i < s_ntex; i++)
        if (tex_is(&s_tex[i], t, cube)) {
            s_tex_idx[h] = (int16_t)(i + 1);
            return &s_tex[i];
        }
    return NULL;
}

/* A cache entry to (re)fill: `e` itself, a free one, or the least recently
 * used, with its D3D objects released. */
static BwTex *tex_slot(BwTex *e)
{
    if (!e) {
        if (s_ntex == BW_MAX_TEX) {
            int lru = 0;
            for (int i = 1; i < s_ntex; i++)
                if (s_tex[i].last_use < s_tex[lru].last_use) lru = i;
            e = &s_tex[lru];
        } else {
            e = &s_tex[s_ntex++];
            memset(e, 0, sizeof(*e));
        }
    }
    if (e->srv) { ID3D11ShaderResourceView_Release(e->srv); e->srv = NULL; }
    if (e->tex) { ID3D11Texture2D_Release(e->tex); e->tex = NULL; }
    if (e->tr.state)
        bw_tr_release(&e->tr);              /* T1: never set in Original */
    /* 0.2.5 item 36: and no identity until the caller fills it, so a
     * texture that can't be decoded or made isn't found again with no view
     * (it drew blank, for good) but is tried again when next used. A
     * filled entry sets every one of these. */
    e->offset = 0; e->w = 0; e->h = 0; e->pitch = 0; e->color = 0;
    e->hash = 0; e->sparse = 0; e->gen = 0; e->stable = 0; e->last_use = 0; e->cube = 0;
    return e;
}

/* Swizzled and DXT textures have no rows; CONTROL1's pitch can be stale
 * (see get_texture). */
static const Nv2aTexture *drop_stale_pitch(const Nv2aTexture *t, Nv2aTexture *fixed)
{
    static int stale = -1;
    if (stale < 0) {
        const char *e = getenv("BW_GPU_STALEPITCH");
        stale = e && *e == '1';
    }
    if (t && t->pitch && !stale
            && (d3d8_format_is_swizzled(t->color) || d3d8_format_dxt_block_bytes(t->color))) {
        *fixed = *t;
        fixed->pitch = 0;
        return fixed;
    }
    return t;
}

static ID3D11ShaderResourceView *get_texture(const Nv2aTexture *t)
{
    char kind = '?';
    ID3D11ShaderResourceView *v;
    Nv2aTexture fixed;
    /* Only linear formats have rows. CONTROL1's pitch stays set from earlier
     * linear work while swizzled and DXT textures are bound (seen 2026-10-26,
     * agent R2: DXT uploads with pitch 5120), and a stale pitch made the cache
     * hash pitch x height (2.6 MB for a 512x512 DXT5), so the texture was
     * re-created about every frame, and made mip_count() upload level 0 only.
     * BW_GPU_STALEPITCH=1 restores the old behaviour for comparison. */
    t = drop_stale_pitch(t, &fixed);
    v = get_texture_(t, &kind);
    if (t)
        note_use(t, v ? kind : 'X');
    return v;
}

static ID3D11ShaderResourceView *get_texture_(const Nv2aTexture *t, char *kind)
{
    BwTex *e = NULL;
    BwSurf *rt;
    uint64_t h;
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA init[12];
    size_t need;

    s_tex_last = NULL;
    if (!t || !t->width || !t->height || t->width > 4096 || t->height > 4096)
        return NULL;
    /* Render to texture: a surface this back end drew. */
    rt = find_surf(t->offset);
    if (rt && rt != s_cur) {
        surf_resolve(rt);                   /* VFX (D9): no-op in Original */
        rt->used_gen = s_gen;               /* item 37: sampled counts as used */
        s_st_rtt++;
        *kind = 'R';
        return rt->srv;
    }
    *kind = rt ? 'F' : 'G';
    e = tex_find(t, 0);                     /* item 43: indexed */
    if (e && e->gen == s_gen) {
        e->last_use = s_gen;
        s_tex_last = e;
        return e->srv;
    }
    /* A texture unchanged for 30 frames: a sampled check, with a full one
     * every 32 frames (staggered by address). */
    if (e && e->stable >= 30 && ((s_gen + (e->offset >> 12)) & 31) != 0
            && tex_sparse(t) == e->sparse) {
        *kind = 'S';
        e->gen = s_gen;
        e->last_use = s_gen;
        s_tex_last = e;
        return e->srv;
    }
    h = tex_hash(t);
    *kind = 'H';
    if (e && e->hash == h && h) {
        if (e->stable < 0xFFFFFFFFu)
            e->stable++;
        if (e->stable == 30)
            e->sparse = tex_sparse(t);
        e->gen = s_gen;
        e->last_use = s_gen;
        s_tex_last = e;
        return e->srv;
    }
    {
        int same = e != NULL;               /* T1: re-created with new contents, or a new entry */
        e = tex_slot(e);
        if (!same) {
            e->tr_churn = 0;
            e->tr_gen = 0;
        }
        e->tr_bound = 0;
    }
    e->cube = 0;
    need = (size_t)t->width * t->height * 2;    /* level 0, and room for its mips */
    if (need > s_decode_cap) {
        free(s_decode);
        s_decode = (uint32_t *)malloc(need * 4);
        s_decode_cap = s_decode ? need : 0;
        if (!s_decode)
            return NULL;
    }
    memset(&td, 0, sizeof(td));
    td.Width = t->width; td.Height = t->height; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    memset(init, 0, sizeof(init));
    if (!fast_texture(t, &td, init)) {
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.MipLevels = 1;
        init[0].pSysMem = s_decode;
        init[0].SysMemPitch = t->width * 4;
        if (!nv2a_backend_decode_texture(t, s_decode))
            return NULL;
    }
    {
        double t0 = trc_on() ? qpc_ms() : 0.0;
        HRESULT hr = BW_GPU_FT("texture", E_OUTOFMEMORY,       /* item 25 test aid */
                               ID3D11Device_CreateTexture2D(s_dev, &td, init, &e->tex));
        if (FAILED(hr)) {
            e->tex = NULL;
            res_failed(RF_TEXTURE, t->offset, t->width, t->height, hr);    /* item 36 */
            return NULL;
        }
        if (trc_on()) {                                     /* T1 test aid */
            s_trc_up += qpc_ms() - t0;
            s_trc_nup++;
        }
        hr = BW_GPU_FT("resource", E_OUTOFMEMORY,               /* item 36: checked */
                       ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)e->tex, NULL, &e->srv));
        if (FAILED(hr)) {
            e->srv = NULL;
            ID3D11Texture2D_Release(e->tex);
            e->tex = NULL;
            res_failed(RF_TEX_VIEW, t->offset, t->width, t->height, hr);
            return NULL;
        }
    }
    /* T1 (VFX_TEX_RESTORE): the menu and title photos (BW_TEXRESTORE_MENUS:
     * the game decodes each JPEG into a 640x480 LIN_A8R8G8B8 screen texture,
     * docs/remaster/texture_matching.md section 3) and the Bink frames
     * (BW_TEXRESTORE_MOVIES, item 11) get the 8x8 deblock on the GPU as they
     * go up; the entry then holds the deblocked copy. */
    e->jpeg = 0;
    e->movie = 0;
    if (bw_vfx_active() && e->srv && t->width == 640 && t->height == 480) {
        int kind = (bw_vfx()->tr_menus && t->color == 0x12 && td.MipLevels == 1) ? 1
                 : (bw_vfx()->tr_movies && tr_is_movie(t)) ? 2 : 0;
        if (kind) {
            ID3D11Texture2D *mt;
            ID3D11ShaderResourceView *ms;
            if (bw_tr_deblock_gpu(s_dev, s_ctx, e->srv, t->width, t->height, 8, kind, &mt, &ms)) {
                ID3D11ShaderResourceView_Release(e->srv);
                ID3D11Texture2D_Release(e->tex);
                e->srv = ms;
                e->tex = mt;
                e->jpeg = kind == 1;
                e->movie = kind == 2;
            }
            if (s_cur)
                bind_targets(s_cur, s_cur_depth);
        }
    }
    e->offset = t->offset; e->w = t->width; e->h = t->height;
    e->color = t->color; e->pitch = t->pitch;
    e->hash = h; e->gen = s_gen; e->last_use = s_gen;
    e->stable = 0; e->sparse = 0;
    /* VFX (D9): is it one of the game's glyph sheets (text) */
    e->glyph = (uint8_t)(t->color == 0x0C && bw_vfx_is_glyph_sheet(GUEST(t->offset), t->width, t->height));
    e->alpha1 = 0;
    if (bw_vfx_active() && bw_vfx()->text) {
        if (t->color == 0x02 || t->color == 0x10)               /* A1R5G5B5, swizzled or linear */
            e->alpha1 = 1;
        else if (t->color == 0x0C) {                            /* DXT1: any 3-colour block with index 3 */
            const uint8_t *q = GUEST(t->offset);
            uint32_t nb = ((t->width + 3) / 4) * ((t->height + 3) / 4), i;
            for (i = 0; i < nb && !e->alpha1; i++, q += 8) {
                uint32_t c0 = q[0] | (q[1] << 8), c1 = q[2] | (q[3] << 8);
                uint32_t ix = q[4] | (q[5] << 8) | (q[6] << 16) | ((uint32_t)q[7] << 24), k;
                if (c0 <= c1)
                    for (k = 0; k < 16; k++)
                        if (((ix >> (2 * k)) & 3) == 3) { e->alpha1 = 1; break; }
            }
        }
    }
    /* VFX (V1, VFX_HUD_EDGES): hard-edged alpha (only overlay draws use it) */
    e->hard = (uint8_t)(bw_vfx_active() && bw_vfx()->hud_edges
                        && tex_hard_alpha(t->color, GUEST(t->offset), t->width, t->height));
    if (e->hard) {
        static int told_hard;
        if (told_hard++ < 48)
            fprintf(stderr, "[VFX] hard-edged alpha %08X %ux%u (HUD edges contour when drawn as an overlay)\n",
                    t->offset, t->width, t->height);
    }
    if (e->glyph) {
        static int told;
        if (told++ < 24)
            fprintf(stderr, "[VFX] glyph sheet %08X %ux%u: text drawn with the contour filter\n",
                    t->offset, t->width, t->height);
    }
    s_st_uploads++;
    *kind = 'U';
    s_tex_last = e;
    /* BW_GPU_TEXLOG=<file>: one line per upload with a plain FNV-1a 64 of the
     * level-0 bytes as the game hands them to the GPU, so a script can match
     * them against the texture files (Remastered texture packs). Test aid. */
    {
        static int texlog_init;
        static FILE *texlog;
        if (!texlog_init) {
            const char *p = getenv("BW_GPU_TEXLOG");
            texlog_init = 1;
            if (p && *p)
                texlog = fopen(p, "a");
        }
        if (texlog) {
            /* Level 0 as stored: DXT blocks, or width x height x bpp (the
             * pitch field is set even for DXT here, so it isn't used). For
             * non-DXT textures with a pitch, a second hash covers pitch x
             * height (linear images). */
            int dxt;
            uint32_t bpp = tex_bpp(t->color, &dxt);
            size_t n = dxt ? (size_t)((t->width + 3) / 4) * ((t->height + 3) / 4) * dxt
                           : (size_t)t->width * t->height * bpp / 8;
            size_t nl = (!dxt && t->pitch) ? (size_t)t->pitch * t->height : 0, i;
            const uint8_t *q = GUEST(t->offset);
            uint64_t f = 1469598103934665603ull, fl = 1469598103934665603ull;
            if (n > (64u << 20)) n = 0;
            if (nl > (64u << 20)) nl = 0;
            for (i = 0; i < n; i++)
                f = (f ^ q[i]) * 1099511628211ull;
            for (i = 0; i < nl; i++)
                fl = (fl ^ q[i]) * 1099511628211ull;
            fprintf(texlog, "T %u color 0x%02X %ux%u %s%u pitch %u l0bytes %zu fnv1a64 %016llX linbytes %zu linfnv %016llX at %08X\n",
                    (unsigned)s_gen, (unsigned)t->color, (unsigned)t->width, (unsigned)t->height,
                    dxt ? "dxtblock " : "bpp ", dxt ? (unsigned)dxt : bpp, (unsigned)t->pitch,
                    n, (unsigned long long)f, nl, (unsigned long long)(nl ? fl : 0), (unsigned)t->offset);
            fflush(texlog);
        }
    }
    return e->srv;
}

/* ---- cube maps ------------------------------------------------------------ */

/* BW_GPU_CUBE=0: sample a cube map's first face as a 2D texture, as this back
 * end did before 2026-10-27 (reflections came out wrong). */
static int use_cube(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("BW_GPU_CUBE");
        v = !(e && e[0] == '0');
    }
    return v;
}

/* BW_GPU_DOTMODES=0: the dot-product reflection stages read zero, as before
 * 2026-10-27 (and as the CPU path still does). */
static int use_dotmodes(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("BW_GPU_DOTMODES");
        v = !(e && e[0] == '0');
    }
    return v;
}

static int dot_program(uint32_t prog)
{
    int st;
    for (st = 1; st < 4; st++) {
        uint32_t m = (prog >> (st * 5)) & 0x1F;
        if (m == 0x11 || m == 0x0B || m == 0x0C)
            return 1;
    }
    return 0;
}

/* Does stage `st` of this program sample a cube map: CUBEMAP, or the
 * dot-product reflections (diffuse at stage 2, specular at stage 3). */
static int cube_stage(uint32_t prog, int st)
{
    uint32_t m = (prog >> (st * 5)) & 0x1F;
    if (m == 3)
        return use_cube();
    if ((m == 0x0B && st == 2) || (m == 0x0C && st == 3))
        return use_cube() && use_dotmodes();
    return 0;
}

/* Bytes between cube faces, as the CPU path (nv2a_pb_exec.c tex_face_stride,
 * after xemu): one face with every level the texture says it stores, rounded
 * up to the NV2A's 128-byte face alignment. */
static uint32_t cube_face_stride(const Nv2aTexture *t)
{
    uint32_t w = t->width, h = t->height, lv, total = 0;
    uint32_t levels = t->levels ? t->levels : 1, bpp;
    int dxt;
    if (t->pitch)
        return (t->pitch * t->height + 127u) & ~127u;
    bpp = tex_bpp(t->color, &dxt);
    for (lv = 0; lv < levels; lv++) {
        uint32_t lw = (w >> lv) ? (w >> lv) : 1, lh = (h >> lv) ? (h >> lv) : 1;
        total += dxt ? ((lw + 3) / 4) * ((lh + 3) / 4) * (uint32_t)dxt : lw * lh * bpp / 8;
    }
    return (total + 127u) & ~127u;
}

static uint32_t *s_cube_decode;
static size_t s_cube_cap;       /* uint32s */

static ID3D11ShaderResourceView *get_cube_texture(const Nv2aTexture *t)
{
    char kind = '?';
    ID3D11ShaderResourceView *v;
    Nv2aTexture fixed;
    t = drop_stale_pitch(t, &fixed);
    v = get_cube_texture_(t, &kind);
    if (t)
        note_use(t, v ? kind : 'X');
    return v;
}

/* Six faces, one after another at cube_face_stride, each laid out like a 2D
 * texture of its own (with its mips): uploaded as one TEXTURECUBE resource.
 * The face order (+X, -X, +Y, -Y, +Z, -Z) and the direction-to-face mapping
 * are D3D's, which the CPU path's rc_stage_fetch follows too. */
static ID3D11ShaderResourceView *get_cube_texture_(const Nv2aTexture *t, char *kind)
{
    BwTex *e = NULL;
    uint32_t stride, f, lv, levels = 1;
    size_t face_texels;
    uint64_t h;
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA init[6 * 12];
    D3D11_SHADER_RESOURCE_VIEW_DESC sd;
    uint32_t *keep;
    size_t keep_cap;
    int fast = 1, ok = 1, i;

    if (!t || !t->width || t->width != t->height || t->width > 2048)
        return NULL;
    stride = cube_face_stride(t);
    if (!stride || (size_t)stride * 6 > (64u << 20))
        return NULL;
    *kind = 'G';
    e = tex_find(t, 1);                     /* item 43: indexed */
    if (e && e->gen == s_gen) {
        e->last_use = s_gen;
        return e->srv;
    }
    /* Cube maps are few and small here: a full hash of all six faces once a
     * frame, no sampled check. */
    h = hash_bytes(GUEST(t->offset), (size_t)stride * 6) ^ ((uint64_t)t->color << 56) ^ 0xC0BEull;
    *kind = 'H';
    if (e && e->hash == h) {
        e->gen = s_gen;
        e->last_use = s_gen;
        return e->srv;
    }
    e = tex_slot(e);
    face_texels = (size_t)t->width * t->height * 2;  /* level 0 and room for its mips */
    if (face_texels * 6 > s_cube_cap) {
        free(s_cube_decode);
        s_cube_decode = (uint32_t *)malloc(face_texels * 6 * 4);
        s_cube_cap = s_cube_decode ? face_texels * 6 : 0;
        if (!s_cube_decode)
            return NULL;
    }
    memset(&td, 0, sizeof(td));
    memset(init, 0, sizeof(init));
    keep = s_decode;
    keep_cap = s_decode_cap;
    for (f = 0; f < 6 && ok; f++) {
        Nv2aTexture ft = *t;
        D3D11_TEXTURE2D_DESC fd;
        D3D11_SUBRESOURCE_DATA fi[12];
        ft.offset = t->offset + f * stride;
        memset(&fd, 0, sizeof(fd));
        memset(fi, 0, sizeof(fi));
        fd.MipLevels = 1;
        fd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        s_decode = s_cube_decode + f * face_texels;   /* fast_texture's scratch */
        s_decode_cap = face_texels;
        if (fast && fast_texture(&ft, &fd, fi)) {
            if (f == 0) {
                levels = fd.MipLevels;
                td.Format = fd.Format;
            } else if (fd.MipLevels != levels || fd.Format != td.Format) {
                ok = 0;
                break;
            }
            for (lv = 0; lv < levels; lv++)
                init[f * levels + lv] = fi[lv];
        } else {
            if (f > 0 && fast) {        /* the format decides; it can't differ by face */
                ok = 0;
                break;
            }
            fast = 0;
            levels = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            if (!nv2a_backend_decode_texture(&ft, s_decode)) {
                ok = 0;
                break;
            }
            init[f].pSysMem = s_decode;
            init[f].SysMemPitch = t->width * 4;
        }
    }
    s_decode = keep;
    s_decode_cap = keep_cap;
    if (!ok)
        return NULL;
    td.Width = t->width; td.Height = t->height; td.MipLevels = levels; td.ArraySize = 6;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
    {
        HRESULT hr = BW_GPU_FT("texture", E_OUTOFMEMORY,       /* item 25 test aid */
                               ID3D11Device_CreateTexture2D(s_dev, &td, init, &e->tex));
        if (FAILED(hr)) {
            e->tex = NULL;
            res_failed(RF_TEXTURE, t->offset, t->width, t->height, hr);    /* item 36 */
            return NULL;
        }
        memset(&sd, 0, sizeof(sd));
        sd.Format = td.Format;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
        sd.TextureCube.MostDetailedMip = 0;
        sd.TextureCube.MipLevels = levels;
        hr = BW_GPU_FT("resource", E_OUTOFMEMORY,
                       ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)e->tex, &sd, &e->srv));
        if (FAILED(hr)) {
            e->srv = NULL;
            ID3D11Texture2D_Release(e->tex);
            e->tex = NULL;
            res_failed(RF_TEX_VIEW, t->offset, t->width, t->height, hr);
            return NULL;
        }
    }
    e->offset = t->offset; e->w = t->width; e->h = t->height;
    e->color = t->color; e->pitch = t->pitch; e->cube = 1;
    e->hash = h; e->gen = s_gen; e->last_use = s_gen;
    e->stable = 0; e->sparse = 0;
    s_st_uploads++;
    *kind = 'U';
    /* BW_GPU_TEXLOG: one "C" line per face (same hash as the T lines), for
     * texture packs that replace cube maps. */
    {
        const char *p = getenv("BW_GPU_TEXLOG");
        FILE *fp = (p && *p) ? fopen(p, "a") : NULL;
        if (fp) {
            int dxt;
            uint32_t bpp = tex_bpp(t->color, &dxt);
            size_t n = dxt ? (size_t)((t->width + 3) / 4) * ((t->height + 3) / 4) * dxt
                           : (size_t)t->width * t->height * bpp / 8, k;
            for (f = 0; f < 6; f++) {
                const uint8_t *q = GUEST(t->offset + f * stride);
                uint64_t fh = 1469598103934665603ull;
                for (k = 0; k < n; k++)
                    fh = (fh ^ q[k]) * 1099511628211ull;
                fprintf(fp, "C %u color 0x%02X %ux%u face %u stride %u levels %u l0bytes %zu fnv1a64 %016llX at %08X\n",
                        (unsigned)s_gen, (unsigned)t->color, (unsigned)t->width, (unsigned)t->height,
                        (unsigned)f, (unsigned)stride, (unsigned)levels, n,
                        (unsigned long long)fh, (unsigned)(t->offset + f * stride));
            }
            fclose(fp);
        }
    }
    return e->srv;
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

static D3D11_BLEND map_blend(uint32_t f)
{
    switch (f) {
    case 0: return D3D11_BLEND_ZERO;
    case 1: return D3D11_BLEND_ONE;
    case 0x300: return D3D11_BLEND_SRC_COLOR;
    case 0x301: return D3D11_BLEND_INV_SRC_COLOR;
    case 0x302: return D3D11_BLEND_SRC_ALPHA;
    case 0x303: return D3D11_BLEND_INV_SRC_ALPHA;
    case 0x304: return D3D11_BLEND_DEST_ALPHA;
    case 0x305: return D3D11_BLEND_INV_DEST_ALPHA;
    case 0x306: return D3D11_BLEND_DEST_COLOR;
    case 0x307: return D3D11_BLEND_INV_DEST_COLOR;
    case 0x308: return D3D11_BLEND_SRC_ALPHA_SAT;
    case 0x8001: case 0x8003: return D3D11_BLEND_BLEND_FACTOR;
    case 0x8002: case 0x8004: return D3D11_BLEND_INV_BLEND_FACTOR;
    default: return D3D11_BLEND_ONE;
    }
}

static D3D11_BLEND alpha_only(D3D11_BLEND b)
{
    /* colour factors are not allowed in the alpha blend */
    switch (b) {
    case D3D11_BLEND_SRC_COLOR: return D3D11_BLEND_SRC_ALPHA;
    case D3D11_BLEND_INV_SRC_COLOR: return D3D11_BLEND_INV_SRC_ALPHA;
    case D3D11_BLEND_DEST_COLOR: return D3D11_BLEND_DEST_ALPHA;
    case D3D11_BLEND_INV_DEST_COLOR: return D3D11_BLEND_INV_DEST_ALPHA;
    default: return b;
    }
}

static D3D11_BLEND_OP map_blend_op(uint32_t e)
{
    switch (e) {
    case 0x8007: return D3D11_BLEND_OP_MIN;
    case 0x8008: return D3D11_BLEND_OP_MAX;
    case 0x800A: return D3D11_BLEND_OP_SUBTRACT;
    case 0x800B: return D3D11_BLEND_OP_REV_SUBTRACT;
    default: return D3D11_BLEND_OP_ADD;
    }
}

static D3D11_COMPARISON_FUNC map_cmp(uint32_t f)
{
    switch (f) {
    case 0x200: return D3D11_COMPARISON_NEVER;
    case 0x201: return D3D11_COMPARISON_LESS;
    case 0x202: return D3D11_COMPARISON_EQUAL;
    case 0x203: return D3D11_COMPARISON_LESS_EQUAL;
    case 0x204: return D3D11_COMPARISON_GREATER;
    case 0x205: return D3D11_COMPARISON_NOT_EQUAL;
    case 0x206: return D3D11_COMPARISON_GREATER_EQUAL;
    default: return D3D11_COMPARISON_ALWAYS;
    }
}

static D3D11_STENCIL_OP map_sop(uint32_t o)
{
    switch (o) {
    case 0: return D3D11_STENCIL_OP_ZERO;
    case 0x1E01: return D3D11_STENCIL_OP_REPLACE;
    case 0x1E02: return D3D11_STENCIL_OP_INCR_SAT;
    case 0x1E03: return D3D11_STENCIL_OP_DECR_SAT;
    case 0x150A: return D3D11_STENCIL_OP_INVERT;
    case 0x8507: return D3D11_STENCIL_OP_INCR;
    case 0x8508: return D3D11_STENCIL_OP_DECR;
    default: return D3D11_STENCIL_OP_KEEP;
    }
}

static int s_vfx_a2c;       /* VFX (D9): this draw's blend state has alpha-to-coverage */

static ID3D11BlendState *get_blend(const Nv2aRenderState *rs)
{
    uint32_t key[6] = { rs->blend_enable, rs->blend_src, rs->blend_dst, rs->blend_eq,
                        rs->color_mask, (uint32_t)s_vfx_a2c };
    D3D11_BLEND_DESC bd;
    D3D11_RENDER_TARGET_BLEND_DESC *t = &bd.RenderTarget[0];
    UINT8 mask = 0;
    for (int i = 0; i < s_nblend; i++)
        if (!memcmp(s_blend[i].key, key, sizeof(key)))
            return s_blend[i].bs;
    if (s_nblend == BW_MAX_STATE)
        return NULL;
    memset(&bd, 0, sizeof(bd));
    bd.AlphaToCoverageEnable = s_vfx_a2c ? TRUE : FALSE;     /* VFX (D9); FALSE in Original */
    t->BlendEnable = rs->blend_enable ? TRUE : FALSE;
    t->SrcBlend = map_blend(rs->blend_src);
    t->DestBlend = map_blend(rs->blend_dst);
    t->BlendOp = map_blend_op(rs->blend_eq);
    t->SrcBlendAlpha = alpha_only(t->SrcBlend);
    t->DestBlendAlpha = alpha_only(t->DestBlend);
    t->BlendOpAlpha = t->BlendOp;
    if (rs->color_mask & 0x00010000u) mask |= D3D11_COLOR_WRITE_ENABLE_RED;
    if (rs->color_mask & 0x00000100u) mask |= D3D11_COLOR_WRITE_ENABLE_GREEN;
    if (rs->color_mask & 0x00000001u) mask |= D3D11_COLOR_WRITE_ENABLE_BLUE;
    if (rs->color_mask & 0x01000000u) mask |= D3D11_COLOR_WRITE_ENABLE_ALPHA;
    t->RenderTargetWriteMask = mask;
    memcpy(s_blend[s_nblend].key, key, sizeof(key));
    ID3D11Device_CreateBlendState(s_dev, &bd, &s_blend[s_nblend].bs);
    return s_blend[s_nblend++].bs;
}

static ID3D11DepthStencilState *get_ds(const Nv2aRenderState *rs, int has_depth)
{
    uint32_t key[10] = { has_depth && rs->depth_test_enable, rs->depth_func,
                         has_depth && rs->depth_write, has_depth && rs->stencil_enable,
                         rs->stencil_func, rs->stencil_func_mask, rs->stencil_write_mask,
                         rs->stencil_fail, rs->stencil_zfail, rs->stencil_zpass };
    D3D11_DEPTH_STENCIL_DESC dd;
    for (int i = 0; i < s_nds; i++)
        if (!memcmp(s_ds[i].key, key, sizeof(key)))
            return s_ds[i].ds;
    if (s_nds == BW_MAX_STATE)
        return NULL;
    memset(&dd, 0, sizeof(dd));
    dd.DepthEnable = key[0] || key[2] ? TRUE : FALSE;
    dd.DepthFunc = key[0] ? map_cmp(rs->depth_func) : D3D11_COMPARISON_ALWAYS;
    dd.DepthWriteMask = key[2] ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.StencilEnable = key[3] ? TRUE : FALSE;
    dd.StencilReadMask = (UINT8)rs->stencil_func_mask;
    dd.StencilWriteMask = (UINT8)rs->stencil_write_mask;
    dd.FrontFace.StencilFunc = map_cmp(rs->stencil_func);
    dd.FrontFace.StencilFailOp = map_sop(rs->stencil_fail);
    dd.FrontFace.StencilDepthFailOp = map_sop(rs->stencil_zfail);
    dd.FrontFace.StencilPassOp = map_sop(rs->stencil_zpass);
    dd.BackFace = dd.FrontFace;
    memcpy(s_ds[s_nds].key, key, sizeof(key));
    ID3D11Device_CreateDepthStencilState(s_dev, &dd, &s_ds[s_nds].ds);
    return s_ds[s_nds++].ds;
}

/* The screen-space path's old sampler: always bilinear (BW_GPU_SSFILTER=0).
 * Its keys carry bit 31 so they never alias get_sampler_f's: they used to,
 * and a point-filtered combiner sampler and this bilinear one with the same
 * wrap modes were handed out for each other, whichever came first. */
static ID3D11SamplerState *get_sampler(uint32_t addr_u, uint32_t addr_v)
{
    uint32_t key[3] = { addr_u | 0x80000000u, addr_v, 0 };
    D3D11_SAMPLER_DESC sd;
    for (int i = 0; i < s_nsamp; i++)
        if (!memcmp(s_samp[i].key, key, sizeof(key)))
            return s_samp[i].ss;
    if (s_nsamp == BW_MAX_STATE)
        return NULL;
    memset(&sd, 0, sizeof(sd));
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    /* NV097 wrap modes: 1 wrap, 2 mirror, 3 clamp to edge, 4 border */
    sd.AddressU = addr_u == 1 ? D3D11_TEXTURE_ADDRESS_WRAP : addr_u == 2 ? D3D11_TEXTURE_ADDRESS_MIRROR
                : addr_u == 4 ? D3D11_TEXTURE_ADDRESS_BORDER : D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = addr_v == 1 ? D3D11_TEXTURE_ADDRESS_WRAP : addr_v == 2 ? D3D11_TEXTURE_ADDRESS_MIRROR
                : addr_v == 4 ? D3D11_TEXTURE_ADDRESS_BORDER : D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    memcpy(s_samp[s_nsamp].key, key, sizeof(key));
    ID3D11Device_CreateSamplerState(s_dev, &sd, &s_samp[s_nsamp].ss);
    return s_samp[s_nsamp++].ss;
}

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

typedef struct {
    float surf[4], depth[4], alpha[4], fogc[4];
    float c0[8][4], c1[8][4], fc0[4], fc1[4];
    float texinfo[4][4];
    float wsx[4];                       /* widescreen x remap (BW_WS shaders only) */
} BwXConsts;

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

/* 0.2.5 item 41: a key's parts the generated text never reads are zeroed,
 * so states that differ only in them share one shader (one table slot, one
 * create) instead of one each: combiner stages past the count (control &
 * 0xFF), and the texture fields of a stage whose program doesn't sample
 * (bw_rc_hlsl's rc_stage_fetch: only modes 1-3 read tex_valid, those with a
 * texture read tex_normalized unless a cube is bound, modes 1-2 tex_conv;
 * only mode-5 stages read their clip_plane_mode bits). A stage with any
 * Remastered variant bit is left as it is. The text, so the bytecode and the
 * pixels, are the same. Test aid BW_GPU_RCKEY_CHECK=1 generates the text
 * from the whole key too, once per distinct key, and logs any difference. */
static void rc_key_trim(BwRcKey *k)
{
    static int check = -1;
    BwRcKey full;
    uint32_t n = k->control & 0xFF, s, st, clip = 0;
    if (check < 0) {
        const char *e = getenv("BW_GPU_RCKEY_CHECK");
        check = e && e[0] == '1';
        if (check)
            fprintf(stderr, "[GPU-D3D11] test aid: each combiner key's shader text checked whole and"
                            " trimmed (BW_GPU_RCKEY_CHECK)\n");
    }
    if (check)
        full = *k;
    if (n > 8)
        n = 8;
    for (s = n; s < 8; s++)
        k->color_icw[s] = k->alpha_icw[s] = k->color_ocw[s] = k->alpha_ocw[s] = 0;
    for (st = 0; st < 4; st++) {
        uint32_t mode = (k->stage_program >> (st * 5)) & 0x1F;
        if (mode == 5)
            clip |= k->clip_plane_mode & (0xFu << (st * 4));
        if (k->vfx[st])
            continue;
        if (mode < 1 || mode > 3)
            k->tex_valid[st] = 0;
        if (!k->tex_valid[st] || (mode == 3 && k->tex_cube[st]))
            k->tex_normalized[st] = 0;
        if (!k->tex_valid[st] || mode == 3)
            k->tex_conv[st] = 0;
    }
    k->clip_plane_mode = clip;
    if (check && memcmp(&full, k, sizeof *k)) {
        static uint64_t seen[4096];
        static char a[64 * 1024], b[64 * 1024];
        static unsigned keys, differ;
        uint64_t h = 1469598103934665603ull;
        const uint8_t *p = (const uint8_t *)&full;
        size_t i, la, lb;
        for (i = 0; i < sizeof full; i++)
            h = (h ^ p[i]) * 1099511628211ull;
        h |= 1;
        for (i = h & 4095; seen[i] && seen[i] != h; i = (i + 1) & 4095)
            ;
        if (seen[i] == h || keys >= 3500)
            return;
        seen[i] = h;
        keys++;
        la = bw_rc_hlsl(&full, a, sizeof a);
        lb = bw_rc_hlsl(k, b, sizeof b);
        if (la != lb || memcmp(a, b, la)) {
            if (differ++ < 3)
                fprintf(stderr, "[GPU-D3D11] rc key check: a trimmed key's shader text DIFFERS (control 0x%X"
                                " stage_program 0x%X, %u vs %u bytes)\n", full.control, full.stage_program,
                        (unsigned)la, (unsigned)lb);
        }
        if (keys <= 40 || keys % 50 == 0)
            fprintf(stderr, "[GPU-D3D11] rc key check: %u trimmed keys checked, %u with different text\n",
                    keys, differ);
    }
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
    if (last >= 0 && last < s_nps && memo_on() && !memcmp(&s_ps_cache[last].key, k, sizeof(*k)))
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
    double pt0 = qpc_ms();
    size_t csz = 0;
    const void *cblob = bw_pscache_find(src, len, &csz);
    if (cblob && !bw_pscache_verify()) {
        if (SUCCEEDED(hr = ID3D11Device_CreatePixelShader(s_dev, cblob, csz, NULL, &e->ps))) {
            bw_pscache_note(0, qpc_ms() - pt0);
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
    bw_pscache_note(1, qpc_ms() - pt0);
    return e->ps;
}

/* Point or bilinear, as the CPU path chooses (rc_texel: TENT when MAG is 2
 * or MIN is 2, 4 or 6). */
/* BW_GPU_BORDER=0: BORDER addressing reads transparent black whatever the
 * texture's FORMAT says, as before 2026-10-27. */
static int use_border(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("BW_GPU_BORDER");
        v = !(e && e[0] == '0');
    }
    return v;
}

/* BORDER (4) reads SET_TEXTURE_BORDER_COLOR when FORMAT's border source is
 * "colour" (border_src 1); with the source "texture" the CPU path clamps to
 * the edge texel, and so does this (nv2a_pb_exec.c sample_tex). Projected
 * shadow maps rely on the colour. */
/* CONVOLUTION_2D_LOD0 (MIN 7 / MAG 4), with SET_TEXTURE_FILTER's kernel
 * (bits 13-15: 1 quincunx, 2 gaussian 3x3). Blood Wake's anti-aliasing
 * resolve (D3D_SwapAntialiased, D3DTEXF_QUINCUNX) samples the 2x-wide surface
 * this way; the sampler took MIN 7 as nearest, so the resolve point-sampled
 * one of each pair of columns. As xemu, the taps are bilinear and the
 * combiner shader sums a 3x3 Gaussian of them (bw_rc_hlsl.c); the present
 * resolves the same way (gpu_present). BW_GPU_CONV=0: a single nearest tap,
 * as before 2026-10-27. Returns the kernel, or 0. */
static int use_conv(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("BW_GPU_CONV");
        v = !(e && e[0] == '0');
    }
    return v && !bw_ab_off("conv");
}

/* Colours and fog as xemu interpolates them (a port fix, every edition):
 * perspective-correct, with the fog factor clamped per pixel rather than per
 * vertex. Both renderers interpolated colours and fog affinely in screen space
 * and clamped fog at the vertices, which on a large triangle reaching past the
 * fog's end (the sea towards the horizon) spreads the haze differently. xemu
 * declares them smooth (common.c pgraph_glsl_get_vtx_header) and clamps fog
 * in the pixel shader (psh.c), noting the per-vertex fog is only clamped to
 * the float range "to match HW interpolation". BW_GPU_PERSP=0 restores the
 * old rule (inferred hardware behaviour; xemu is the reference). */
static int use_persp(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("BW_GPU_PERSP");
        v = !(e && e[0] == '0');
    }
    return v && !bw_ab_off("persp");
}

/* The filter of the title's last anti-aliasing resolve (xdraw_prepare). */
static uint32_t s_resolve_filter;
static int s_resolve_seen;

static int conv_kernel(uint32_t filter)
{
    uint32_t k = (filter >> 13) & 7;
    if (!use_conv() || !(((filter >> 16) & 0xFF) == 7 || ((filter >> 24) & 0xF) == 4))
        return 0;
    return k ? (int)k : 1;
}

static ID3D11SamplerState *get_sampler_f(uint32_t addr_u, uint32_t addr_v, uint32_t filter,
                                         int border_src, uint32_t border)
{
    uint32_t mag = (filter >> 24) & 0xF, min = (filter >> 16) & 0xFF;
    int linear = mag == 2 || min == 2 || min == 4 || min == 6 || conv_kernel(filter);
    /* NV097 MIN: 1 nearest, 2 linear (level 0 only), 3/4 nearest/linear with
     * the nearest mip, 5/6 nearest/linear blending two mips. */
    int mipped = use_mips() && min >= 3 && min <= 6;
    int min_lin = min == 2 || min == 4 || min == 6;
    int mag_lin = mag == 2;
    int mip_lin = min == 5 || min == 6;
    int bordered;
    uint32_t key[3];
    if (use_border() && !border_src) {
        if (addr_u == 4) addr_u = 3;
        if (addr_v == 4) addr_v = 3;
    }
    bordered = use_border() && border_src && (addr_u == 4 || addr_v == 4);
    key[0] = addr_u | (linear ? 0x100u : 0u) | (mipped ? 0x1000u | (min << 16) | (mag << 24) : 0u);
    key[1] = addr_v | (bordered ? 0x100u : 0u);
    key[2] = bordered ? border : 0;
    D3D11_SAMPLER_DESC sd;
    for (int i = 0; i < s_nsamp; i++)
        if (!memcmp(s_samp[i].key, key, sizeof(key)))
            return s_samp[i].ss;
    if (s_nsamp == BW_MAX_STATE)
        return NULL;
    memset(&sd, 0, sizeof(sd));
    if (mipped && bw_vfx_aniso() > 1 && (min_lin || mag_lin)) {
        /* VFX (D9): anisotropic for the mipped, filtered textures (the
         * 3D world); point-sampled ones stay as the game set them. */
        sd.Filter = D3D11_FILTER_ANISOTROPIC;
        sd.MaxAnisotropy = bw_vfx_aniso();
    } else if (mipped) {
        /* D3D11 filter bits: MIN linear 0x10, MAG linear 0x04, MIP linear 0x01. */
        sd.Filter = (D3D11_FILTER)((min_lin ? 0x10 : 0) | (mag_lin ? 0x04 : 0) | (mip_lin ? 0x01 : 0));
    } else {
        sd.Filter = linear ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_MIP_POINT;
    }
    sd.AddressU = addr_u == 1 ? D3D11_TEXTURE_ADDRESS_WRAP : addr_u == 2 ? D3D11_TEXTURE_ADDRESS_MIRROR
                : addr_u == 4 ? D3D11_TEXTURE_ADDRESS_BORDER : D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = addr_v == 1 ? D3D11_TEXTURE_ADDRESS_WRAP : addr_v == 2 ? D3D11_TEXTURE_ADDRESS_MIRROR
                : addr_v == 4 ? D3D11_TEXTURE_ADDRESS_BORDER : D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    if (bordered) {
        sd.BorderColor[0] = ((border >> 16) & 0xFF) / 255.0f;
        sd.BorderColor[1] = ((border >> 8) & 0xFF) / 255.0f;
        sd.BorderColor[2] = (border & 0xFF) / 255.0f;
        sd.BorderColor[3] = (border >> 24) / 255.0f;
    }
    sd.MaxLOD = mipped ? D3D11_FLOAT32_MAX : 0.0f;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    memcpy(s_samp[s_nsamp].key, key, sizeof(key));
    ID3D11Device_CreateSamplerState(s_dev, &sd, &s_samp[s_nsamp].ss);
    return s_samp[s_nsamp++].ss;
}

static void unpack_argb(uint32_t c, float o[4])
{
    o[0] = ((c >> 16) & 0xFF) / 255.0f;
    o[1] = ((c >> 8) & 0xFF) / 255.0f;
    o[2] = (c & 0xFF) / 255.0f;
    o[3] = (c >> 24) / 255.0f;
}

static float s_xlast_surf[4], s_xlast_depth[4];   /* the last draw's b0 surf, depth */

/* VFX (D9): the cached texture's flags (set at upload): 1 glyph sheet,
 * 2 1-bit alpha in use, 4 hard-edged alpha (V1). */
static int tex_vfx_flags(const Nv2aTexture *t)
{
    for (int i = 0; i < s_ntex; i++)
        if (s_tex[i].offset == t->offset && s_tex[i].w == t->width && s_tex[i].h == t->height
                && s_tex[i].color == t->color && !s_tex[i].cube)
            return (s_tex[i].glyph ? 1 : 0) | (s_tex[i].alpha1 ? 2 : 0) | (s_tex[i].hard ? 4 : 0)
                 | (s_tex[i].jpeg ? 8 : 0);
    return 0;
}

static int tex_is_glyph(const Nv2aTexture *t)
{
    return tex_vfx_flags(t) & 1;
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
    uint32_t mag = (filter >> 24) & 0xF, min = (filter >> 16) & 0xFF;
    int mipped = use_mips() && min >= 3 && min <= 6;
    int min_lin = min == 2 || min == 4 || min == 6, bordered;
    uint32_t key[3];
    D3D11_SAMPLER_DESC sd;
    if (mipped)
        return get_sampler_f(addr_u, addr_v, filter, border_src, border);
    if (use_border() && !border_src) {
        if (addr_u == 4) addr_u = 3;
        if (addr_v == 4) addr_v = 3;
    }
    bordered = use_border() && border_src && (addr_u == 4 || addr_v == 4);
    key[0] = addr_u | 0x2000u | ((uint32_t)lk << 14) | (min_lin ? 0x100u : 0u) | (mag << 24);
    key[1] = addr_v | (bordered ? 0x100u : 0u);
    key[2] = bordered ? border : 0;
    for (int i = 0; i < s_nsamp; i++)
        if (!memcmp(s_samp[i].key, key, sizeof(key)))
            return s_samp[i].ss;
    if (s_nsamp == BW_MAX_STATE)
        return NULL;
    memset(&sd, 0, sizeof(sd));
    sd.Filter = (D3D11_FILTER)((min_lin ? 0x10 : 0) | (mag == 2 ? 0x04 : 0) | 0x01);
    sd.AddressU = addr_u == 1 ? D3D11_TEXTURE_ADDRESS_WRAP : addr_u == 2 ? D3D11_TEXTURE_ADDRESS_MIRROR
                : addr_u == 4 ? D3D11_TEXTURE_ADDRESS_BORDER : D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = addr_v == 1 ? D3D11_TEXTURE_ADDRESS_WRAP : addr_v == 2 ? D3D11_TEXTURE_ADDRESS_MIRROR
                : addr_v == 4 ? D3D11_TEXTURE_ADDRESS_BORDER : D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    if (bordered) {
        sd.BorderColor[0] = ((border >> 16) & 0xFF) / 255.0f;
        sd.BorderColor[1] = ((border >> 8) & 0xFF) / 255.0f;
        sd.BorderColor[2] = (border & 0xFF) / 255.0f;
        sd.BorderColor[3] = (border >> 24) / 255.0f;
    }
    sd.MinLOD = 0.0f;
    sd.MaxLOD = (float)lk;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    memcpy(s_samp[s_nsamp].key, key, sizeof(key));
    ID3D11Device_CreateSamplerState(s_dev, &sd, &s_samp[s_nsamp].ss);
    return s_samp[s_nsamp++].ss;
}

/* T1 (item 11): is this the Bink movie texture: Movie_CreateTexture's
 * LIN_X8R8G8B8 640x480 (seen, T3: one per movie, rewritten every decode;
 * docs/remaster/texture_matching.md: the only 0x1E 640x480 uploads), from
 * guest memory (not a surface this back end drew), and, when the widescreen
 * markers run, inside Movie_DrawQuad's (BW_WS_WHAT_MOVIE). */
static int tr_is_movie(const Nv2aTexture *t)
{
    if (t->color != 0x1E || t->width != 640 || t->height != 480 || find_surf(t->offset))
        return 0;
    if (s_ws_k > 1.0f)
        return s_ws_2d > 0 && s_ws_what == BW_WS_WHAT_MOVIE;
    return 1;
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
    e = s_tex_last;
    *srv = v;
    if (!v || !e || e->cube || e->glyph)    /* a font page is overlay art, wherever it is drawn */
        return 0;
    if (e->tr.state == 0) {
        Nv2aTexture fixed;
        const Nv2aTexture *t = drop_stale_pitch(&b->tex[st], &fixed);
        if (e->tr_gen && s_gen - e->tr_gen < 120 && e->tr_churn < 255)
            e->tr_churn++;
        e->tr_gen = s_gen;
        if (e->tr_churn >= 3) {
            e->tr.state = 2;
            e->tr.why = BW_TR_WHY_CHURN;
        } else {
            /* copies the blocks and queues the work; the original draws
             * until the flip that finishes it (bw_tr_pump) */
            double t0 = trc_on() ? qpc_ms() : 0.0;
            bw_tr_request(&e->tr, t->color, GUEST(t->offset), t->width, t->height,
                          mip_count(t), t->addr_u, t->addr_v, t->offset);
            s_trc_req++;
            if (trc_on())
                s_trc_reqt += qpc_ms() - t0;
        }
    }
    {
        /* 0.2.5 item 48: the restored copy made for this draw's edge modes
         * (a second one is asked for at the first draw with other modes) */
        BwTrSlot *ts = bw_tr_slot_for(&e->tr, b->tex[st].addr_u, b->tex[st].addr_v, b->tex[st].offset);
        if (ts != &e->tr && ts->state == 0) {
            Nv2aTexture fixed;
            const Nv2aTexture *t = drop_stale_pitch(&b->tex[st], &fixed);
            bw_tr_request(ts, t->color, GUEST(t->offset), t->width, t->height,
                          mip_count(t), t->addr_u, t->addr_v, t->offset);
            s_trc_req++;
        }
        if (ts->state != 1 || !ts->srv)
            return 0;
        if (!e->tr_bound) {
            e->tr_bound = 1;
            s_trc_first++;              /* T1 test aid: first draw with the restored chain */
        }
        *srv = ts->srv;
        return ts->lk;
    }
}

/* What both combiner-path draws share: the targets, the pixel shader, the
 * b0 constants, textures and samplers, and the rasteriser, blend and depth
 * states. The caller then binds its vertex input and vertex shader and
 * draws. Returns 1 to draw, 0 if it cannot, -1 if both faces are culled. */
static int begin(const Nv2aSurface *s, const Nv2aRenderState *rs);   /* defined below */

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

    memset(&key, 0, sizeof(key));
    memcpy(key.color_icw, rc->color_icw, sizeof(key.color_icw));
    memcpy(key.alpha_icw, rc->alpha_icw, sizeof(key.alpha_icw));
    memcpy(key.color_ocw, rc->color_ocw, sizeof(key.color_ocw));
    memcpy(key.alpha_ocw, rc->alpha_ocw, sizeof(key.alpha_ocw));
    key.final0 = rc->final0;
    key.final1 = rc->final1;
    key.control = rc->control;
    key.stage_program = rc->stage_program;
    key.clip_plane_mode = b->clip_plane_mode;
    key.flags[0] = (uint8_t)(use_persp() ? BW_RC_PERSP : 0);     /* colour/fog interpolation */
    for (st = 0; st < 4; st++) {
        key.tex_valid[st] = b->tex_valid[st];
        key.tex_normalized[st] = b->tex_normalized[st];
        key.tex_conv[st] = (uint8_t)(b->tex_valid[st] ? conv_kernel(b->tex[st].filter) : 0);
        /* A cube map sampled by a CUBEMAP stage goes up as six faces; in any
         * other case (or BW_GPU_CUBE=0, or if that fails) its first face is
         * sampled as a 2D texture. */
        key.tex_cube[st] = 0;
        if (b->tex_valid[st] && b->tex_cube[st] && cube_stage(rc->stage_program, st)) {
            srv[st] = get_cube_texture(&b->tex[st]);
            key.tex_cube[st] = srv[st] != NULL;
        }
    }
    /* The dot-product stages' inputs, only where a program has them (so other
     * programs don't compile once per stale register value). */
    if (dot_program(rc->stage_program)) {
        key.dot_rgbmapping = b->dot_rgbmapping;
        key.other_stage_input = b->other_stage_input;
    }
    if (s_vfx_overlay && (bw_vfx()->text || bw_vfx()->hud_edges || bw_vfx()->tr_menus))  /* VFX (D9): text and HUD art (overlay draws) */
        for (st = 0; st < 4; st++) {
            int fl = 0;
            uint32_t c = b->tex[st].color;
            if (!b->tex_valid[st] || b->tex_cube[st])
                continue;
            if (c == 0x0C || c == 0x02 || c == 0x10 || c == 0x0E || c == 0x0F)
                fl = tex_vfx_flags(&b->tex[st]);
            else if (c == 0x12 && bw_vfx()->tr_menus)
                fl = tex_vfx_flags(&b->tex[st]) & 8;
            if (fl & 8) {                               /* T1: a menu/title photo: Lanczos-2 */
                key.vfx[st] |= BW_RC_VFX_OVERLAY_L2;
                continue;
            }
            if ((fl & 4) && bw_vfx()->hud_edges)        /* V1: hard-edged HUD art */
                key.vfx[st] |= BW_RC_VFX_OVERLAY_HARD;
            else if (!bw_vfx()->text)
                continue;
            else if (fl & 1)
                key.vfx[st] |= BW_RC_VFX_GLYPH;
            else if (fl & 2)
                key.vfx[st] |= BW_RC_VFX_OVERLAY;
            else
                key.vfx[st] |= BW_RC_VFX_OVERLAY_CR;
        }
    /* T1 (BW_TEXRESTORE_MOVIES, item 11): a Bink frame, wherever it is drawn
     * (its quad isn't always caught by the overlay test): Lanczos-2 when
     * magnified, the game's own sample at 1:1. */
    if (bw_vfx_active() && bw_vfx()->tr_movies)
        for (st = 0; st < 4; st++)
            if (b->tex_valid[st] && !b->tex_cube[st] && tr_is_movie(&b->tex[st]))
                key.vfx[st] = (uint8_t)((key.vfx[st] & BW_RC_VFX_A2C) | BW_RC_VFX_OVERLAY_L2);
    /* T1 (VFX_TEX_RESTORE): 3D stages whose texture has a restored chain
     * (not overlays, and not the water's dot-product reflection program).
     * Test aid: BW_GPU_AB=texrestore draws odd frames with the originals. */
    int tr_lk[4] = { 0, 0, 0, 0 };
    double trc_t0 = trc_on() ? qpc_ms() : 0.0;
    if (bw_vfx_active() && bw_vfx()->tex_restore && !s_vfx_overlay && !s_tr_skip
            && !dot_program(rc->stage_program) && !bw_ab_off("texrestore"))
        for (st = 0; st < 4; st++)
            if (!srv[st] && (tr_lk[st] = tr_stage(b, rc->stage_program, st, &srv[st])) != 0
                    && bw_vfx()->tr_debug)
                key.vfx[st] |= BW_RC_VFX_TR_DEBUG;
    if (trc_on())
        s_trc_trs += qpc_ms() - trc_t0;
    /* VFX (D9): alpha-tested, unblended draws on an MSAA target (foliage,
     * fences, rigging) get alpha-to-coverage instead of a hard test. */
    int vfx_a2c = bw_vfx_active() && bw_vfx()->a2c && !s_vfx_overlay && rs->alpha_test_enable
               && !rs->blend_enable && (rs->alpha_func == 0x204 || rs->alpha_func == 0x206)
               && s_cur && s_cur->mtex;
    if (vfx_a2c)
        key.vfx[0] |= BW_RC_VFX_A2C;
    stage_stats(b, rc->stage_program, srv);
    rc_key_trim(&key);                                  /* item 41: unused stages zeroed */
    if (trc_on()) {                                     /* T1 test aid */
        double t0 = qpc_ms();
        unsigned n0 = s_ps_compiles;
        ps = get_rc_shader(&key);
        s_trc_sh += qpc_ms() - t0;
        s_trc_nsh += s_ps_compiles - n0;
    } else
    ps = get_rc_shader(&key);
    if (!ps)
        return 0;
    rst = get_rs(rs);
    {
        /* P2a test aid (docs/day1/P2a.md section 7), Remastered only:
         * BW_OCEAN_CULLTEST=1|2 forces back-face culling with the front face
         * CW (1, the game's own state, seen) or CCW (2: the game's front
         * faces culled, only the back faces drawn) for the sea's reflection
         * pass only (stage 3 DOT_RFLCT_SPEC on a cube). */
        static int ct = -1;
        if (ct < 0) {
            const char *e = getenv("BW_OCEAN_CULLTEST");
            ct = (e && (e[0] == '1' || e[0] == '2') && bw_edition_at_least(BW_ED_REMASTERED)) ? e[0] - '0' : 0;
        }
        if (ct && ((key.stage_program >> 15) & 0x1F) == 0x0C && key.tex_cube[3]) {
            static int shown;
            Nv2aRenderState t = *rs;
            if (!shown++)
                fprintf(stderr, "[BW] ocean cull test: the game's own state for the reflection pass:"
                        " cull_enable %u cull_face 0x%X front_face 0x%X%c",
                        (unsigned)rs->cull_enable, (unsigned)rs->cull_face, (unsigned)rs->front_face, 10);
            t.cull_enable = 1;
            t.cull_face = 0x405;                        /* GL_BACK */
            t.front_face = ct == 1 ? 0x900 : 0x901;     /* GL_CW / GL_CCW */
            rst = get_rs(&t);
        }
    }
    if (!rst)
        return -1;              /* both faces culled */

    memset(&c, 0, sizeof(c));
    c.surf[0] = (float)s_cur->w; c.surf[1] = (float)s_cur->h;
    c.surf[2] = 1.0f / s_cur->w; c.surf[3] = 1.0f / s_cur->h;
    ws_remap(s, c.wsx, s_ws_full);
    {
        float zmin = rs->depth_min, zmax = rs->depth_max;
        if (!(zmax > zmin)) { zmin = 0.0f; zmax = 16777215.0f; }
        c.depth[0] = zmin;
        c.depth[1] = 1.0f / (zmax - zmin);
        c.depth[2] = zmode_vertex_clamp() ? 1.0f : 0.0f;
    }
    if (rs->alpha_test_enable) {
        c.alpha[0] = (float)(rs->alpha_func >= 0x200 && rs->alpha_func <= 0x207
                             ? rs->alpha_func - 0x200 : 7);
        c.alpha[1] = (float)(rs->alpha_ref & 0xFF);
        c.alpha[2] = 1.0f;
    }
    c.fogc[0] = b->fogc[0]; c.fogc[1] = b->fogc[1]; c.fogc[2] = b->fogc[2];
    for (st = 0; st < 8; st++) {
        unpack_argb(rc->factor0[st], c.c0[st]);
        unpack_argb(rc->factor1[st], c.c1[st]);
    }
    unpack_argb(rc->final_c0, c.fc0);
    unpack_argb(rc->final_c1, c.fc1);
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
        c.texinfo[st][0] = 1.0f / b->tex[st].width;
        c.texinfo[st][1] = 1.0f / b->tex[st].height;
        /* The title's anti-aliasing resolve: a 2x-wide surface sampled into a
         * 1x one (render targets are linear images). Its filter is how
         * gpu_present turns the 2x-wide surface into the picture. */
        if (!b->tex_normalized[st] && s_cur && s_cur->aa_sx == 1) {
            BwSurf *rt = find_surf(b->tex[st].offset);
            if (rt && rt != s_cur && rt->aa_sx == 2) {
                s_resolve_filter = b->tex[st].filter;
                s_resolve_seen = 1;
            }
        }
    }
    if (trc_on())
        trc_t0 = qpc_ms();
    {
        /* item 43: upload only when the constants changed (a DISCARD map
         * per draw renames the buffer in the driver every time). c is
         * zeroed first, so its padding compares too. */
        static BwXConsts shadow;
        static int shadow_ok;
        if (!memo_on() || !shadow_ok || memcmp(&shadow, &c, sizeof c)) {
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
        s_trc_map += qpc_ms() - trc_t0;
    memcpy(s_xlast_surf, c.surf, sizeof s_xlast_surf);
    memcpy(s_xlast_depth, c.depth, sizeof s_xlast_depth);
    bf[0] = ((rs->blend_color >> 16) & 0xFF) / 255.0f;
    bf[1] = ((rs->blend_color >> 8) & 0xFF) / 255.0f;
    bf[2] = (rs->blend_color & 0xFF) / 255.0f;
    bf[3] = ((rs->blend_color >> 24) & 0xFF) / 255.0f;
    if (rs->blend_src == 0x8003 || rs->blend_src == 0x8004 ||
        rs->blend_dst == 0x8003 || rs->blend_dst == 0x8004)
        bf[0] = bf[1] = bf[2] = bf[3];

    ID3D11DeviceContext_PSSetShader(s_ctx, ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 1, &s_xcb);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 4, srv);
    ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 4, ss);
    if (key.vfx[0] | key.vfx[1] | key.vfx[2] | key.vfx[3]) {      /* VFX (D9) */
        ID3D11SamplerState *lin = bw_vfx_linear_sampler();
        ID3D11DeviceContext_PSSetSamplers(s_ctx, 4, 1, &lin);
    }
    ID3D11DeviceContext_RSSetState(s_ctx, rst);
    s_vfx_a2c = vfx_a2c;
    ID3D11DeviceContext_OMSetBlendState(s_ctx, get_blend(rs), bf, 0xFFFFFFFFu);
    s_vfx_a2c = 0;
    ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, get_ds(rs, s_cur_depth != NULL),
                                               rs->stencil_ref & 0xFF);
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

/* A 3D (non-overlay) draw into s_cur. */
static void vfx_scene_draw(void)
{
    if (!vfx_wants_scene() || !s_cur)
        return;
    if (s_vfx_pend_surf != s_cur) {
        s_vfx_pend_surf = s_cur;
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
           || dot_program(xb->rc->stage_program);
}

static void vfx_drawlog_note(const char *what);     /* BW_VFX_DRAWLOG, below */

static void vfx_ao_now(const D3D11_RECT *rect)
{
    BwSurf *sf = s_cur;
    float zf, zn, focal, m11, pzn, pzf;
    D3D11_RECT r;
    if ((s_vfx_ao_done && s_vfx_ao_gen == s_gen) || bw_vfx()->ao <= 0.0f || !sf
            || !s_cur_depth || !s_cur_depth->srv)
        return;
    r = *rect;
    if (r.left < 0) r.left = 0;
    if (r.top < 0) r.top = 0;
    if (r.right > (LONG)sf->pw) r.right = (LONG)sf->pw;
    if (r.bottom > (LONG)sf->ph) r.bottom = (LONG)sf->ph;
    if (r.right <= r.left || r.bottom <= r.top)
        return;
    if (s_vfx_ao_gen != s_gen)
        s_vfx_ao_view = 0;
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
    bw_vfx_ao_apply(s_cur_depth->srv, s_cur_depth->samples > 1, sf->pw, sf->ph,   /* item 36: its own count */
                    sf->mrtv ? sf->mrtv : sf->rtv, &r, zn, zf, focal);
    bind_targets(sf, s_cur_depth);
    SURF_DRAWN(sf);
    s_vfx_ao_done = 1;
    s_vfx_ao_gen = s_gen;
}

static void vfx_overlay_draw(void)
{
    BwSurf *sf = s_cur;
    DXGI_FORMAT f;
    D3D11_BOX box;
    if (!vfx_wants_scene() || !sf || !sf->tex)
        return;
    f = bw_vfx_target_format(sf->bpp);
    if (!s_vfx_scene || s_vfx_scene_w != sf->pw || s_vfx_scene_h != sf->ph || s_vfx_scene_fmt != f) {
        static uint32_t fail_gen;           /* item 36: one try a frame after a failure */
        D3D11_TEXTURE2D_DESC td;
        if (fail_gen == s_gen)
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
                fail_gen = s_gen;
                res_failed(RF_SCENE, sf->va, sf->pw, sf->ph, hr);
                return;
            }
        }
        s_vfx_scene_w = sf->pw; s_vfx_scene_h = sf->ph; s_vfx_scene_fmt = f;
        s_vfx_scene_gen = 0;
    }
    if (s_vfx_scene_gen != s_gen || s_vfx_scene_surf != sf) {
        static const float clear0[4] = { 0, 0, 0, 0 };
        ID3D11DeviceContext_ClearRenderTargetView(s_ctx, s_vfx_scene_rtv, clear0);
        s_vfx_scene_gen = s_gen;
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
    if (s_vfx_scene_gen != s_gen || s_vfx_scene_surf != sf)
        return NULL;
    if (s_vfx_pend_on && s_vfx_pend_surf == sf) {
        BwSurf *keep = s_cur;
        s_cur = sf;
        vfx_overlay_draw();
        s_cur = keep;
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
    if (want < 0 || (long)s_gen < want || (long)s_gen > want + 1 || !rs)
        return;
    fprintf(stderr, "[VFX-DRAW] f%u %c n%u surf %08X%s", s_gen, path, n, s_cur ? s_cur->va : 0,
            s_cur && s_cur_depth ? "+z" : "");
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
    if (want >= 0 && (long)s_gen >= want && (long)s_gen <= want + 1)
        fprintf(stderr, "[VFX-DRAW] f%u -- %s\n", s_gen, what);
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
    if (want < 0 || (long)s_gen != want || b->ninstr > 4 || !b->inputs)
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

static void bw_xdraw(const Nv2aSurface *s, const BwXBatch *b)
{
    UINT stride = sizeof(BwXVertex), offs;
    int ov;                                 /* item 50: a handed-back overlay draw */

    if (!b || !b->count)
        return;
    if (s_ws_k > 1.0f) {
        float lo = b->vertices[0].x, hi = lo;
        float u0 = b->vertices[0].tc[0][0], u1 = u0, v0 = b->vertices[0].tc[0][1], v1 = v0;
        for (uint32_t i = 1; i < b->count; i++) {
            const BwXVertex *q = &b->vertices[i];
            if (q->x < lo) lo = q->x;
            if (q->x > hi) hi = q->x;
            if (q->tc[0][0] < u0) u0 = q->tc[0][0];
            if (q->tc[0][0] > u1) u1 = q->tc[0][0];
            if (q->tc[0][1] < v0) v0 = q->tc[0][1];
            if (q->tc[0][1] > v1) v1 = q->tc[0][1];
        }
        s_ws_span_u = u1 - u0;
        s_ws_span_v = v1 - v0;
        s_ws_full = (ws_covers_view(s, lo, hi, 1)
                     && ws_backdrop(s, b->tex_valid[0], b->tex_normalized[0], b->tex[0].width,
                                    b->tex[0].height, u0, u1, v0, v1))
                    || ws_samples_wide(b->tex_valid[0], b->tex[0].offset);
        ws_note('x', b->count, b->tex_valid[0], b->tex[0].width, b->tex[0].height, lo, hi, s);
    }
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
                            " frame %u: kept as HUD for the effects\n", n_ov, s_gen);
    }
    s_tr_skip = s_tr_fb_overlay;            /* T1 */
    s_vfx_log_ov = s_tr_fb_overlay;         /* item 50: for the drawlog */
    s_tr_fb_overlay = 0;
    if (bw_vfx_active() && b->state && begin(s, b->state)) {
        if (ov)
            vfx_overlay_draw();
        else if (vfx_sea_starts(b) && s_vfx_pend_on && s_vfx_pend_surf == s_cur)
            vfx_ao_now(&s_vfx_pend);
    }
    s_vfx_overlay = ov;
    {
        double t0 = trc_on() ? qpc_ms() : 0.0;
        int r = xdraw_prepare(s, b);
        if (trc_on())
            s_trc_prep += qpc_ms() - t0;
        s_tr_skip = 0;
        s_vfx_overlay = 0;
        if (r <= 0)
            return;
    }
    if (s_watch > 0 && s_cur)
        note_huge(b->vertices, b->count, s_cur->w, s_cur->h);
    offs = ring_push(&s_vring, b->vertices, b->count * stride, stride);
    if (offs == ~0u)
        return;
    ID3D11DeviceContext_IASetInputLayout(s_ctx, s_xlayout);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetVertexBuffers(s_ctx, 0, 1, &s_vring.buf, &stride, &offs);
    ID3D11DeviceContext_VSSetShader(s_ctx, s_xvs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 0, 1, &s_xcb);
    if (trc_on()) {                                         /* T1 test aid */
        double t0 = qpc_ms();
        ID3D11DeviceContext_Draw(s_ctx, b->count, 0);
        s_trc_draw += qpc_ms() - t0;
        s_trc_ndraw++;
    } else
    ID3D11DeviceContext_Draw(s_ctx, b->count, 0);
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
    SURF_DRAWN(s_cur);
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
} BwVsh;

static BwVsh *s_vsh;            /* BW_MAX_VSH entries, allocated on first use */
static HANDLE s_vsh_wake, s_vsh_thread;
static ID3D11Buffer *s_kcb;
static BwVshConsts s_kshadow;
static int s_kvalid;
static int s_gpu_vsh = -1;      /* BW_GPU_VSH=0: vertex programs on the CPU */
static BwVsh *s_vlast;          /* the last draw_vsh, for vsh_readback */
static UINT s_vlast_voff, s_vlast_stride, s_vlast_nverts;

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
        bw_pscache_store_vs(v->src, v->len, s_ws_k > 1.0f, code, size);
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
    if (last >= 0 && memo_on()) {
        /* item 43: the last program first, compared word for word without
         * hashing the whole program (up to 136 instructions) every draw */
        v = &s_vsh[last];
        if (v->n == b->ninstr && !memcmp(v->conv, conv, sizeof conv)
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
        if (v->hash != h || v->n != b->ninstr || memcmp(v->conv, conv, sizeof conv)
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
    if (!s_vsh_thread) {
        /* 0.2.5 item 42: both checked. Without the event the worker's wait
         * returned at once and it spun a core; a failed thread leaked the
         * event and was tried again on every new program. Either way the
         * vertex programs now stay on the CPU path (as BW_GPU_VSH=0). */
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
            return NULL;
        }
    }
    v = &s_vsh[n];
    memset(v, 0, sizeof *v);
    v->hash = h;
    v->n = b->ninstr;
    v->mask = b->input_mask;
    v->nin = b->ninputs;
    memcpy(v->conv, conv, sizeof conv);
    memcpy(v->words, b->program, b->ninstr * sizeof v->words[0]);
    v->len = bw_vsh_hlsl(b->program, b->ninstr, b->input_mask, conv, src, sizeof src);
    if (!v->len || !(v->src = (char *)malloc(v->len + 1))) {
        v->state = -1;
    } else {
        size_t csz = 0;
        const void *cb;
        memcpy(v->src, src, v->len + 1);
        if (getenv("BW_GPU_VSH_DUMP") && n < 8)
            fprintf(stderr, "[GPU-VSH] shader %ld (%u instructions, inputs %04X):\n%s\n",
                    (long)n, b->ninstr, b->input_mask, v->src);
        /* item 43: the bytecode an earlier run compiled from this text with
         * these flags (bw_pscache.c), so the program draws on the GPU from
         * its first batch instead of on the CPU until the worker is done */
        cb = bw_pscache_find_vs(v->src, v->len, s_ws_k > 1.0f, &csz);
        if (cb) {
            const char *what = "shader";
            HRESULT hr = S_OK;
            static unsigned hits;
            if (vsh_make(v, cb, csz, &what, &hr)) {
                free(v->src);
                v->src = NULL;
                v->state = 2;
                if (hits++ < 3 || hits % 50 == 0)
                    fprintf(stderr, "[PSCACHE] vertex shader from the cache (%u this run)\n", hits);
                InterlockedExchange(&s_nvsh, n + 1);
                last = n;
                return v;
            }
            fprintf(stderr, "[PSCACHE] the driver rejected a cached vertex shader's %s (error 0x%08lX):"
                            " compiling it again\n", what, (unsigned long)hr);
        }
    }
    InterlockedExchange(&s_nvsh, n + 1);
    SetEvent(s_vsh_wake);
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
    if (g == s_gs_bound && memo_on())
        return;
    ID3D11DeviceContext_GSSetShader(s_ctx, g, NULL, 0);
    s_gs_bound = g;
}

static void gs_unbind(void)
{
    if (s_gs_bound || !memo_on())
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

static DXGI_FORMAT vfetch_format(uint8_t conv)
{
    uint32_t s = conv & 15u;
    switch (conv >> 4) {
    case BW_XVS_FLOAT:
        return s == 1 ? DXGI_FORMAT_R32_FLOAT : s == 2 ? DXGI_FORMAT_R32G32_FLOAT
             : s == 3 ? DXGI_FORMAT_R32G32B32_FLOAT : DXGI_FORMAT_R32G32B32A32_FLOAT;
    case BW_XVS_CONST: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case BW_XVS_COLOR: return DXGI_FORMAT_R8G8B8A8_UINT;
    case BW_XVS_UBYTE:
        return s == 1 ? DXGI_FORMAT_R8_UINT : s == 2 ? DXGI_FORMAT_R8G8_UINT : DXGI_FORMAT_R8G8B8A8_UINT;
    case BW_XVS_SHORT:
        return s == 1 ? DXGI_FORMAT_R16_SINT : s == 2 ? DXGI_FORMAT_R16G16_SINT
                                                     : DXGI_FORMAT_R16G16B16A16_SINT;
    case BW_XVS_CMP:   return DXGI_FORMAT_R32_UINT;
    default:           return DXGI_FORMAT_UNKNOWN;
    }
}

/* The input layout for this batch's streams: one element per input, in its
 * stream's slot at its offset, in its own format. Kept per shader. */
static ID3D11InputLayout *vfetch_layout(BwVsh *v, const BwXVsBatch *b)
{
    D3D11_INPUT_ELEMENT_DESC el[16];
    uint64_t key = 1469598103934665603ull;
    uint32_t k, i;
    BwRawIl *e;

    if (!v->code || !v->nin || v->nin > 16)
        return NULL;
    memset(el, 0, sizeof el);
    for (k = 0; k < v->nin; k++) {
        const BwXVsInput *in = &b->in[k];
        el[k].SemanticName = "ATTR";
        el[k].SemanticIndex = k;
        el[k].Format = vfetch_format(in->conv);
        el[k].InputSlot = in->stream;
        el[k].AlignedByteOffset = in->offset;
        el[k].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
        if (el[k].Format == DXGI_FORMAT_UNKNOWN)
            return NULL;
        key = (key ^ ((uint64_t)el[k].Format | (uint64_t)in->stream << 8
                      | (uint64_t)in->offset << 16)) * 1099511628211ull;
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
    return gpu_vsh_on() && !(s_ws_k > 1.0f && s_ws_2d > 0);
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
    if (bw_vfx_active())
        gs_unbind();            /* item 43: the visual suite's passes below draw without one */
    if (!(v = get_vsh(b))) {
        s_st_vfall++;
        return 0;
    }
    /* VFX (D9): an overlay (HUD) draw: D3D's pass-through program, no depth test */
    s_vfx_overlay = bw_vfx_active() && b->ninstr <= 4 && b->xb.state && !b->xb.state->depth_test_enable;
    if (s_vfx_overlay && b->xb.tex_valid[0]) {
        /* V1 test aid: BW_VFX_HUDSKIP=<hex texture address> leaves out the
         * overlay draws of that texture (which HUD draw is which). */
        static int skip_init;
        static uint32_t skip;           /* 0: none */
        if (!skip_init) {
            const char *e = getenv("BW_VFX_HUDSKIP");
            skip_init = 1;
            skip = e ? (uint32_t)strtoul(e, NULL, 16) : 0;
        }
        if (skip && b->xb.tex[0].offset == skip) {
            s_vfx_overlay = 0;
            return 1;
        }
    }
    if (s_ws_k > 1.0f) {
        /* Position is input 0 when the program reads it; for 2D (D3D's
         * pass-through program) it is in logical pixels. */
        s_ws_full = 0;
        if (s_ws_2d > 0 && (b->input_mask & 1u) && b->ninputs && b->nverts && b->inputs) {
            /* The overlay system's quads read v0 position (logical pixels),
             * v1 colour, v2 texture coordinate 0 (normalized) (seen, run
             * ...-r6-gpu-in7: mask 000F); D3D's own fixed-function layout
             * would put it in v9. Inputs arrive in ascending order of the
             * mask (bw_backend_ext.h). */
            int tci = -1, tca = (b->input_mask & (1u << 2)) ? 2 : 9;
            float lo = b->inputs[0], hi = lo, u0 = 0, u1 = 0, v0 = 0, v1 = 0;
            if (b->input_mask & (1u << tca)) {
                uint32_t m = b->input_mask & ((1u << tca) - 1u);
                tci = 0;
                while (m) { tci += (int)(m & 1u); m >>= 1; }
                u0 = u1 = b->inputs[tci * 4];
                v0 = v1 = b->inputs[tci * 4 + 1];
            }
            for (uint32_t i = 1; i < b->nverts; i++) {
                const float *in = &b->inputs[(size_t)i * b->ninputs * 4];
                if (in[0] < lo) lo = in[0];
                if (in[0] > hi) hi = in[0];
                if (tci >= 0) {
                    float u = in[tci * 4], vv = in[tci * 4 + 1];
                    if (u < u0) u0 = u;
                    if (u > u1) u1 = u;
                    if (vv < v0) v0 = vv;
                    if (vv > v1) v1 = vv;
                }
            }
            s_ws_span_u = u1 - u0;
            s_ws_span_v = v1 - v0;
            if (s_ws_log > 0) {
                static uint32_t seen[8];
                int k;
                for (k = 0; k < 8 && seen[k] && seen[k] != b->input_mask; k++)
                    ;
                if (k < 8 && !seen[k]) {
                    seen[k] = b->input_mask;
                    fprintf(stderr, "[WS] 2D vertex-program inputs: mask %04X (%u), first vertex",
                            b->input_mask, b->ninputs);
                    for (uint32_t a = 0; a < b->ninputs && a < 6; a++)
                        fprintf(stderr, " (%g %g %g %g)", b->inputs[a * 4], b->inputs[a * 4 + 1],
                                b->inputs[a * 4 + 2], b->inputs[a * 4 + 3]);
                    fprintf(stderr, "\n");
                }
            }
            /* Textured with no known texture coordinate: not a backdrop. */
            s_ws_full = (ws_covers_view(s, lo, hi, 0)
                         && (b->xb.tex_valid[0] ? tci >= 0 : 1)
                         && ws_backdrop(s, b->xb.tex_valid[0], b->xb.tex_normalized[0],
                                        b->xb.tex[0].width, b->xb.tex[0].height, u0, u1, v0, v1))
                        || ws_samples_wide(b->xb.tex_valid[0], b->xb.tex[0].offset);
            ws_note('v', b->nindices, b->xb.tex_valid[0], b->xb.tex[0].width, b->xb.tex[0].height,
                    lo, hi, s);
        }
    }
    if (bw_vfx_active() && b->xb.state && begin(s, b->xb.state)) {
        /* the scene before the HUD (and AO), per view; before this draw's
         * own state is set, since the AO passes change the pipeline */
        if (s_vfx_overlay)
            vfx_overlay_draw();
        else {
            if (vfx_sea_starts(&b->xb) && s_vfx_pend_on && s_vfx_pend_surf == s_cur)
                vfx_ao_now(&s_vfx_pend);        /* the water starts: AO on what's under it */
            vfx_scene_draw();
        }
    }
    if (trc_on()) {                                         /* T1 test aid */
        double t0 = qpc_ms();
        r = xdraw_prepare(s, &b->xb);
        s_trc_prep += qpc_ms() - t0;
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
        vsinfo[0] = b->aa[0]; vsinfo[1] = b->aa[1];
        vsinfo[2] = b->near_clip ? 1.0f : 0.0f; vsinfo[3] = 0.0f;
        foginfo[0] = b->fog_enable ? 1.0f : 0.0f;
        foginfo[1] = (float)b->fog_mode;
        foginfo[2] = b->fog_param[0]; foginfo[3] = b->fog_param[1];
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
            ID3D11DeviceContext_VSSetShaderResources(s_ctx, 0, 1, &t);
            conv_bound = 1;
        }
        ID3D11DeviceContext_IASetVertexBuffers(s_ctx, 0, b->nstreams, bufs, s_vlast_strides, s_vlast_offs);
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

    ID3D11DeviceContext_IASetInputLayout(s_ctx, il);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    if (v->nin && !s_vlast_raw)
        ID3D11DeviceContext_IASetVertexBuffers(s_ctx, 0, 1, &s_vring.buf, &stride, &voff);
    ID3D11DeviceContext_IASetIndexBuffer(s_ctx, s_iring.buf, DXGI_FORMAT_R32_UINT, 0);
    ID3D11DeviceContext_VSSetShader(s_ctx, v->vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 0, 2, cbs);
    gs_set(vsh_filter_gs());    /* item 43: only when it isn't bound already */
    ors = 0;
    {
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
        } else if (s_cur && s_cur_depth && s_cur_depth->tex && s_cur_depth->dsv && ors_enabled()
                   && !s_ors_gaveup) {
            if (lit) {
                if (!s_ors_inlit) {     /* a new run: copy the scene's depth at its first draw */
                    s_ors_src = NULL;
                    s_ors_primed = 0;
                }
                if (ors_prepare(s_cur_depth)) {
                    s_ors_inlit = 1;    /* item 49: only once the copy exists */
                    ors = 2;
                } else
                    s_ors_gaveup = 1;   /* item 49: this run as shipped, without copying again */
            } else if (s_ors_inlit || s_ors_primed) {
                ID3D11RenderTargetView *rtv = s_cur->mrtv ? s_cur->mrtv : s_cur->rtv;
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
                ors = 1;
                s_ors_draws++;
            }
        }
    }
    if (trc_on()) {                                         /* T1 test aid */
        double t0 = qpc_ms();
        ID3D11DeviceContext_DrawIndexed(s_ctx, b->nindices, ioff / 4u, 0);
        s_trc_draw += qpc_ms() - t0;
        s_trc_ndraw++;
    } else
    ID3D11DeviceContext_DrawIndexed(s_ctx, b->nindices, ioff / 4u, 0);
    if (ors == 2) {                 /* OCEAN_REFLECT_SINGLE: the lit tile again, depth only, into the copy */
        ID3D11RenderTargetView *rtv = s_cur->mrtv ? s_cur->mrtv : s_cur->rtv;
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
        ID3D11RenderTargetView *rtv = s_cur->mrtv ? s_cur->mrtv : s_cur->rtv;
        ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &rtv, s_cur_depth->dsv);
    }
    if (bw_vfx_active()) {
        vfx_drawlog('v', b->nindices, &b->xb, NULL, b->ninstr);     /* VFX (D9) test aid */
        vfx_drawlog_inputs(b);                                      /* V1: the HUD quads' inputs */
    }
    ft_draw('v', b->nindices, (float)b->ninstr, (float)b->nverts, 0.0f, 0.0f, b->input_mask,
            b->xb.tex_valid[0] ? b->xb.tex[0].offset : 0, b->xb.state);
    if (!memo_on())
        gs_set(NULL);           /* BW_GPU_MEMO=0: unbound after every draw, as before */
    s_vlast = v;
    s_vlast_voff = voff;
    s_vlast_stride = stride;
    s_vlast_nverts = b->nverts;
    SURF_DRAWN(s_cur);
    s_st_draws++;
    s_st_vdraws++;
    s_st_tris += b->nindices / 3;
    return 1;
}


/* BW_GPU_VSH_VERIFY: run the last batch's vertices through its shader again,
 * as points into a stream-output buffer, and read them back. The executor
 * compares them with the CPU interpreter's results. Slow (it waits for the
 * GPU), so only sampled batches are checked. */
static int bw_vsh_readback(BwXVsOut *out, uint32_t n)
{
    static ID3D11Buffer *sob, *stg;
    static UINT so_cap;
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

/* T1 (VFX_TEX_RESTORE): a vertex-program batch handed back to the CPU path
 * (its shader still compiling) comes back as the next bw_xdraw; remember if
 * it was an overlay draw (D3D's pass-through program, no depth test), so the
 * texture restoration leaves its textures alone there too. Only a flag; the
 * draw itself is unchanged in every edition. */
static int bw_xdraw_vsh_t1(const Nv2aSurface *s, const BwXVsBatch *b)
{
    ULONG64 c0 = draw_cpu_begin();     /* item 43: draw cpu */
    int r = bw_xdraw_vsh(s, b);
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

static const BwXBackend s_xbackend = { bw_xdraw_m, bw_xdraw_vsh_t1, bw_vsh_readback, bw_vsh_raw_ok };

/* ---- the three back-end entry points ------------------------------------ */

/* The title's window clip as the scissor rectangle. NV097 window clip 0
 * (SET_WINDOW_CLIP_HORIZONTAL 0x02C0 / _VERTICAL 0x02E0: min in bits 0-11,
 * max in bits 16-27, inclusive, in logical pixels) is how split screen keeps
 * each player's view in its half (2-player battle: y 0..239, then 240..479).
 * The executor never applied it; the CPU rasteriser clamps to the surface
 * clip only. Scaled by the AA factor, clamped to the surface. Unset (both 0)
 * or BW_GPU_SCISSOR=0: the whole surface. */
static void apply_scissor(const Nv2aSurface *s)
{
    static int on = -1;
    D3D11_RECT r;
    uint32_t h = bw_backend_reg(0x02C0), v = bw_backend_reg(0x02E0);
    uint32_t sx = s->aa_sx ? s->aa_sx : 1, sy = s->aa_sy ? s->aa_sy : 1;

    if (on < 0) {
        const char *e = getenv("BW_GPU_SCISSOR");
        on = !(e && e[0] == '0');
    }
    r.left = 0; r.top = 0;
    r.right = (LONG)s_cur->w; r.bottom = (LONG)s_cur->h;
    if (on && (h || v) && bw_backend_reg(0x02B4) == 0) {
        LONG x0 = (LONG)(h & 0xFFF) * sx, x1 = ((LONG)((h >> 16) & 0xFFF) + 1) * sx;
        LONG y0 = (LONG)(v & 0xFFF) * sy, y1 = ((LONG)((v >> 16) & 0xFFF) + 1) * sy;
        if (x0 > r.left) r.left = x0;
        if (y0 > r.top) r.top = y0;
        if (x1 < r.right) r.right = x1;
        if (y1 < r.bottom) r.bottom = y1;
        if (r.right < r.left) r.right = r.left;
        if (r.bottom < r.top) r.bottom = r.top;
    }
    if (s_cur->pw != s_cur->w || s_cur->ph != s_cur->h) {
        /* VFX (D9): logical pixels to the render-scaled surface's */
        float fx = (float)s_cur->pw / s_cur->w, fy = (float)s_cur->ph / s_cur->h;
        r.left = (LONG)(r.left * fx + 0.5f); r.right = (LONG)(r.right * fx + 0.5f);
        r.top = (LONG)(r.top * fy + 0.5f); r.bottom = (LONG)(r.bottom * fy + 0.5f);
    }
    ID3D11DeviceContext_RSSetScissorRects(s_ctx, 1, &r);
    s_vfx_scissor = r;                      /* VFX (D9): the scene copy's per-view rectangle */
}

static int begin(const Nv2aSurface *s, const Nv2aRenderState *rs)
{
    BwSurf *surf;
    BwDepth *d;
    if (!ensure_device() || !s || !s->color_va)
        return 0;
    surf = get_surf(s);
    if (!surf)
        return 0;
    /* VFX (D9): pw, ph = w, h in Original; item 36: the depth buffer has the
     * colour target's sample count (1 in Original) */
    d = rs ? get_depth(rs->zeta_va, surf->pw, surf->ph, surf->mrtv ? bw_vfx_msaa() : 1) : NULL;
    surf->used_gen = s_gen;                 /* item 37: a full table reuses the oldest */
    if (d)
        d->used_gen = s_gen;
    if (surf != s_cur || d != s_cur_depth)
        bind_targets(surf, d);
    apply_scissor(s);
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
        ID3D11DeviceContext_ClearRenderTargetView(s_ctx, s_cur->mrtv ? s_cur->mrtv : s_cur->rtv, c);
        SURF_DRAWN(s_cur);
        ft_note('C', s_cur->va);
    }
    if ((flags & 0x3) && s_cur_depth && s_cur_depth->dsv) {         /* item 36: never a NULL view */
        UINT f = ((flags & 1) ? D3D11_CLEAR_DEPTH : 0) | ((flags & 2) ? D3D11_CLEAR_STENCIL : 0);
        ID3D11DeviceContext_ClearDepthStencilView(s_ctx, s_cur_depth->dsv, f,
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
    memset(&c, 0, sizeof(c));
    c.surf[0] = (float)s_cur->w; c.surf[1] = (float)s_cur->h;
    c.surf[2] = 1.0f / s_cur->w; c.surf[3] = 1.0f / s_cur->h;
    if (s_ws_k > 1.0f) {
        float lo = b->vertices[0].x, hi = lo;
        for (uint32_t i = 1; i < b->count; i++) {
            if (b->vertices[i].x < lo) lo = b->vertices[i].x;
            if (b->vertices[i].x > hi) hi = b->vertices[i].x;
        }
        {
            float u0 = b->vertices[0].u, u1 = u0, v0 = b->vertices[0].v, v1 = v0;
            for (uint32_t i = 1; i < b->count; i++) {
                if (b->vertices[i].u < u0) u0 = b->vertices[i].u;
                if (b->vertices[i].u > u1) u1 = b->vertices[i].u;
                if (b->vertices[i].v < v0) v0 = b->vertices[i].v;
                if (b->vertices[i].v > v1) v1 = b->vertices[i].v;
            }
            s_ws_span_u = u1 - u0;
            s_ws_span_v = v1 - v0;
            s_ws_full = (ws_covers_view(s, lo, hi, 1)
                         && ws_backdrop(s, b->texture != NULL, 0, 0, 0, u0, u1, v0, v1))
                        || ws_samples_wide(b->texture != NULL, b->texture ? b->texture->offset : 0);
        }
        ws_note('s', b->count, b->texture != NULL, b->texture ? b->texture->width : 0,
                b->texture ? b->texture->height : 0, lo, hi, s);
    }
    ws_remap(s, c.wsx, s_ws_full);
    if (srv) {
        c.tex[0] = 1.0f / b->texture->width;
        c.tex[1] = 1.0f / b->texture->height;
        c.tex[2] = 1.0f;
    }
    if (rs->alpha_test_enable) {
        c.alpha[0] = (float)(rs->alpha_func >= 0x200 && rs->alpha_func <= 0x207
                             ? rs->alpha_func - 0x200 : 7);
        c.alpha[1] = rs->alpha_ref / 255.0f;
        c.alpha[2] = 1.0f;
    }
    {
        float zmin = rs->depth_min, zmax = rs->depth_max;
        if (!(zmax > zmin)) { zmin = 0.0f; zmax = 16777215.0f; }
        c.depth[0] = zmin;
        c.depth[1] = 1.0f / (zmax - zmin);
        c.depth[2] = zmode_vertex_clamp() ? 1.0f : 0.0f;
    }
    if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_cb, 0,
                                          D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, &c, sizeof(c));
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_cb, 0);
    }
    bf[0] = ((rs->blend_color >> 16) & 0xFF) / 255.0f;
    bf[1] = ((rs->blend_color >> 8) & 0xFF) / 255.0f;
    bf[2] = (rs->blend_color & 0xFF) / 255.0f;
    bf[3] = ((rs->blend_color >> 24) & 0xFF) / 255.0f;
    if (rs->blend_src == 0x8003 || rs->blend_src == 0x8004 ||
        rs->blend_dst == 0x8003 || rs->blend_dst == 0x8004)
        bf[0] = bf[1] = bf[2] = bf[3];

    ID3D11DeviceContext_IASetInputLayout(s_ctx, s_layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetVertexBuffers(s_ctx, 0, 1, &s_vb, &stride, &offs);
    ID3D11DeviceContext_VSSetShader(s_ctx, s_vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 0, 1, &s_cb);
    ID3D11DeviceContext_PSSetShader(s_ctx, s_ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 1, &s_cb);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &srv);
    if (ss)
        ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 1, &ss);
    {
        ID3D11RasterizerState *rst = get_rs(rs);
        if (!rst)
            return;              /* both faces culled */
        ID3D11DeviceContext_RSSetState(s_ctx, rst);
    }
    ID3D11DeviceContext_OMSetBlendState(s_ctx, get_blend(rs), bf, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, get_ds(rs, s_cur_depth != NULL),
                                               rs->stencil_ref & 0xFF);
    ID3D11DeviceContext_Draw(s_ctx, b->count, 0);
    ft_draw('s', b->count, b->vertices[0].x, b->vertices[0].y, b->vertices[0].z,
            b->vertices[0].rhw, b->vertices[0].diffuse, b->texture ? b->texture->offset : 0, rs);
    SURF_DRAWN(s_cur);
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
    for (i = 0; i < s_nuse[slot]; i++) {
        const BwTexUse *u = &s_use[slot][i];
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
        frame_no[0] = frame_no[1]; frame_no[1] = frame_no[2]; frame_no[2] = s_gen;
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
    if (s_watch < 0) {
        s_watch_prefix = getenv("BW_GPU_BLACKWATCH");
        s_watch = s_watch_prefix && *s_watch_prefix;
    }
    if (s_watch <= 0)
        return;
    b = black_fraction(sf);
    if (b > prev + 0.25f && b > 0.3f) {
        fprintf(stderr, "[BW-BLACK] frame %u: lower screen %.0f%% black (previous frame %.0f%%),"
                        " surface %08X %ux%u\n", s_gen, b * 100, prev * 100, sf->va, sf->w, sf->h);
        print_uses("this frame", s_gen & 1);
        print_uses("previous frame", (s_gen + 1) & 1);
        print_huge("this frame", s_gen & 1);
        print_huge("previous frame", (s_gen + 1) & 1);
        if (dumps < 12) {
            char path[512];
            _snprintf(path, sizeof path, "%s_%06u.bmp", s_watch_prefix, s_gen);
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
        if (huge_log && s_huge[s_gen & 1] && logged < 60) {
            logged++;
            fprintf(stderr, "[BW-HUGE] frame %u (lower screen %.0f%% black)\n", s_gen, b * 100);
            print_huge("frame", s_gen & 1);
        }
    }
    s_nuse[(s_gen + 1) & 1] = 0;    /* the next frame's list */
    s_huge[(s_gen + 1) & 1] = 0;
    s_tris_frame[(s_gen + 1) & 1] = 0;
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

void bw_d3d11_present_config(unsigned sync_interval, int allow_tearing, int readback)
{
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
                IDXGISwapChain2_SetMaximumFrameLatency(s2, 2);
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
        if (want[i] && s_gen >= want[i]) {          /* the game's flip number, first presented one at or after it */
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
                    _snprintf(path, sizeof path, "%sflip_%05u.%s", prefix, s_gen, fp16 ? "pfm" : "bmp");
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
                        fprintf(stderr, "[GPU-PRESENT] shot %s (flip %u, present %u, %ux%u, %s)\n", path, s_gen, count,
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
    return (s_gen % every) == 0 || (pairs && (s_gen % every) == 1) ? prefix : NULL;
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
    snprintf(path, sizeof path, "%s%06u.bmp", prefix, (unsigned)s_gen);
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
    kernel = s_resolve_seen ? conv_kernel(s_resolve_filter) : 0;
    resolve_pass(sf, kernel);
    shown_write(due ? s_pic_tex : NULL, w, h, due, 1);
    return use_conv() ? s_pic_srv : sf->srv;
}

/* P2b: what the screen does with the presented frames (BW_GPU_STATS test
 * aid; reads only). After each present the swap chain's own statistics say
 * which present was last put on screen and at which refresh (SyncQPCTime):
 * the time from our Present call to that refresh is the present-to-screen
 * latency, and the refresh count's step says how many refreshes the screen
 * made meanwhile (with variable refresh, one per frame shown). Not every
 * mode reports them (a composed window may not): the line then says so. */
static DWORD s_screen_wait_ms;          /* P2b: bw_d3d11_pace_config */
static unsigned s_screen_timeouts, s_screen_st_n;
static double s_screen_st_ms;
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

/* The screen wait's fallback, made visible (0.2.5, for C2's item 64): one
 * line when four timeouts in a row turn the wait to zero (frames are then
 * dropped at once until the screen takes one), one when it takes one again.
 * The first ten of each. */
static void screen_wait_note(int engaged)
{
    static unsigned n[2];
    if (++n[engaged] <= 10)
        fprintf(stderr, engaged
                ? "[GPU-PRESENT] the screen didn't take a frame in 4 waits in a row (frame %u): frames are"
                  " dropped without waiting until it takes one%s\n"
                : "[GPU-PRESENT] the screen takes frames again (frame %u)%s\n",
                (unsigned)s_gen, n[engaged] == 10 ? "; later ones aren't logged" : "");
}

/* BW_TEST_SCREENWAIT=<from_frame>:<count> (test aid, every edition, off by
 * default, never in a preset): from flip <from_frame> on, the next <count>
 * screen waits are treated as timed out (the timeout's time is slept, the
 * waitable isn't touched), so the fallback above can be tested on a screen
 * that is awake. */
static int screen_test_timeout(void)
{
    static int parsed;
    static unsigned from, left;
    if (!parsed) {
        const char *e = getenv("BW_TEST_SCREENWAIT");
        parsed = 1;
        if (e && *e && sscanf(e, "%u:%u", &from, &left) == 2 && left)
            fprintf(stderr, "[GPU-PRESENT] BW_TEST_SCREENWAIT: %u screen waits from flip %u treated as"
                            " timed out (test aid)\n", left, from);
        else
            left = 0;
    }
    if (!left || s_gen < from)
        return 0;
    left--;
    return 1;
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
                            " until it can (visual effects off meanwhile)\n", (unsigned)s_gen);
        failing = 1;
        return 1;
    }
    if (failing && ++n[1] <= 5)
        fprintf(stderr, "[VFX] the visual suite draws again (frame %u, after %u plain frames)\n",
                (unsigned)s_gen, frames);
    failing = 0;
    frames = 0;
    return 0;
}

/* The finished surface, letterboxed into the window and presented. Returns 1
 * if it did. The executor's targets are rebound afterwards. */
static int gpu_present(BwSurf *sf)
{
    static const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    ID3D11ShaderResourceView *none = NULL, *pic;
    D3D11_VIEWPORT vp;
    float fw, fh, cw, ch, w, h;
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
        if (s_cur)
            bind_targets(s_cur, s_cur_depth);
        return 0;
    }
    if (!sf || !sf->srv || !blit_ensure())
        return 0;
    /* Original: the title's resolve and the shown-frame dumps; Remastered's
     * visual suite (D9) presents the surface itself, below. */
    pic = bw_vfx_active() ? sf->srv : shown_picture(sf);
    if (!s_present_mode || !swap_ensure()) {
        if (s_cur)
            bind_targets(s_cur, s_cur_depth);
        return 0;
    }
    bw_hdr_follow(s_swap_w, s_swap_h);              /* item 97: the screen, every edition */
    if (s_swap_wait) {
        /* P2b (Remastered high frame rate): wait a little for the screen
         * to take the next frame instead of dropping it at once (seen on
         * A 360 Hz screen: 13-54 frames a second dropped near the cap,
         * each a skipped image). A screen that keeps not taking frames
         * (asleep: D7's case) gets the old zero wait after a few timeouts,
         * until it signals again. BW_HIFPS_SCREENWAIT=0 restores. */
        DWORD to = (s_screen_wait_ms && s_screen_timeouts < 4) ? s_screen_wait_ms : 0, r;
        double tw = to ? qpc_ms() : 0.0;
        if (screen_test_timeout()) {    /* BW_TEST_SCREENWAIT (test aid): as if timed out */
            if (to)
                Sleep(to);
            r = WAIT_TIMEOUT;
        } else {
            r = WaitForSingleObject(s_swap_wait, to);
        }
        if (to) {
            double w = qpc_ms() - tw;
            if (w > 0.05) {
                s_screen_st_n++;
                s_screen_st_ms += w;
            }
        }
        if (r != WAIT_OBJECT_0) {
            if (to && ++s_screen_timeouts == 4)
                screen_wait_note(1);    /* the zero-wait fallback engages */
            s_st_dropped++;
            if (s_cur)
                bind_targets(s_cur, s_cur_depth);
            return 0;
        }
        if (s_screen_timeouts >= 4)
            screen_wait_note(0);        /* the screen takes frames again */
        s_screen_timeouts = 0;
        waited = 1;
    }
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &s_swap_rtv, NULL);
    ID3D11DeviceContext_ClearRenderTargetView(s_ctx, s_swap_rtv, black);
    /* A 4:3 picture, as the console sends to the TV: the flipped surface is
     * often the 2x-wide anti-aliased 1280x480 one, which the GPU filters down. */
    fw = 4.0f * sf->wsk; fh = 3.0f;      /* widescreen (D5): the picture's own shape */
    cw = (float)s_swap_w; ch = (float)s_swap_h;
    if (cw * fh <= ch * fw) { w = cw; h = cw * fh / fw; }
    else { h = ch; w = ch * fw / fh; }
    vp.TopLeftX = (float)(int)((cw - w) * 0.5f);
    vp.TopLeftY = (float)(int)((ch - h) * 0.5f);
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
    }
    if (FAILED(hr) && !gpu_lost(hr, "Present")) {      /* item 28: removed or reset ends the boot */
        static int shown;
        if (shown++ < 3)
            fprintf(stderr, "[GPU-PRESENT] Present failed 0x%08lX\n", (unsigned long)hr);
    }
    /* Back to what the executor had bound. */
    if (s_cur)
        bind_targets(s_cur, s_cur_depth);
    return SUCCEEDED(hr);
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

void bw_d3d11_latency_config(int max_frames)
{
    s_lat_cfg = max_frames;
}

static int lat_max(void)
{
    if (s_lat_max < 0) {
        const char *e = getenv("BW_GPU_LATENCY");
        if (e && *e == 'm')
            s_lat_max = LAT_Q - 2;              /* measure only */
        else if (e && *e)
            s_lat_max = atoi(e);
        else
            s_lat_max = s_lat_cfg;
        if (s_lat_max < 0) s_lat_max = 0;
        if (s_lat_max > LAT_Q - 2) s_lat_max = LAT_Q - 2;
        if (s_lat_max)
            fprintf(stderr, "[GPU-LAT] at most %d flipped frames unfinished on the GPU%s (BW_GPU_LATENCY)\n",
                    s_lat_max, s_lat_max >= LAT_Q - 2 ? " (measuring only)" : "");
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
        if (g_bw_fl_on)
            bw_fl_event(BW_FL_N_GPUDONE, s_lat_tail, bw_fl_us_since((unsigned long long)s_lat_t[i].QuadPart),
                        s_lat_head - s_lat_tail);
        s_lat_tail++;
    }
}

static void lat_flip(void)
{
    int max = lat_max();
    unsigned i;
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
    s_lat_head++;
    s_lat_st_n++;
    s_lat_st_sum += s_lat_head - s_lat_tail;
    if (s_lat_head - s_lat_tail > s_lat_st_most)
        s_lat_st_most = s_lat_head - s_lat_tail;
    if (s_lat_head - s_lat_tail > (unsigned)max) {
        double t0 = qpc_ms(), w;
        unsigned before = s_lat_head - s_lat_tail, n = 0;
        if (!s_lat_timer)
            s_lat_timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* HIGH_RESOLUTION */,
                                                 TIMER_ALL_ACCESS);
        lat_poll(1);                            /* submit what is queued */
        while (s_lat_head - s_lat_tail > (unsigned)max) {
            LARGE_INTEGER due;
            if (++n < 4) {
                SwitchToThread();
            } else {
                due.QuadPart = -1000;           /* 100 us */
                if (s_lat_timer && SetWaitableTimer(s_lat_timer, &due, 0, NULL, NULL, FALSE))
                    WaitForSingleObject(s_lat_timer, 2);
                else
                    Sleep(0);
            }
            lat_poll(0);
            if (qpc_ms() - t0 > 100.0)
                break;                          /* never hang on a lost query */
        }
        w = qpc_ms() - t0;
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
    pace_stats_line();
    if (!s_lat_max || !s_lat_st_n)
        return;
    fprintf(stderr, "[GPU-LAT] frames in flight %.2f mean, %lu most | waits %lu, %.1f ms (most %.1f)\n",
            (double)s_lat_st_sum / s_lat_st_n, s_lat_st_most, s_lat_st_waits, s_lat_st_wait_ms,
            s_lat_st_wait_max);
    s_lat_st_n = s_lat_st_sum = s_lat_st_most = s_lat_st_waits = 0;
    s_lat_st_wait_ms = s_lat_st_wait_max = 0.0;
}

/* P2b: even presents at the frame cap (Remastered high frame rate).
 *
 * The flip hold paces the walk on a tick of the host clock at the target
 * rate, but the present itself is made at the FLIP_STALL, before the hold,
 * and a frame that came late is let through at once: so near the cap two
 * presents could come well under one refresh apart (seen: 40% of present
 * intervals under 2.78 ms in a battle at 282 fps, 64% in mission 10 at 335,
 * off screen). On a 360 Hz variable-refresh screen a frame that comes
 * sooner than the screen can refresh tears or waits. Here a present is made
 * no sooner than 1/target s after the previous one (a short wait on the
 * NV2A thread, which holds the walk and so the game, as the hold does).
 * bw_d3d11_pace_config sets the rate; BW_HIFPS_PACE=0 turns it off. */
static double s_pace_period_ms, s_pace_last_ms, s_pace_st_ms;
static unsigned s_pace_st_n;
static HANDLE s_pace_timer;

void bw_d3d11_pace_config(unsigned hz)
{
    const char *e = getenv("BW_HIFPS_PACE");
    s_pace_period_ms = (hz && !(e && e[0] == '0')) ? 1000.0 / (double)hz : 0.0;
    e = getenv("BW_HIFPS_SCREENWAIT");
    s_screen_wait_ms = hz && !(e && e[0] == '0') ? 8u : 0u;
    if (s_pace_period_ms > 0.0)
        fprintf(stderr, "[GPU-PRESENT] presents at least %.3f ms apart (%u per second; BW_HIFPS_PACE=0 turns it off)\n",
                s_pace_period_ms, hz);
}

static void pace_present(void)
{
    double now, due;
    if (s_pace_period_ms <= 0.0)
        return;
    now = qpc_ms();
    due = s_pace_last_ms + s_pace_period_ms;
    if (s_pace_last_ms > 0.0 && now < due) {
        double t0 = now;
        /* sleep on a high-resolution timer to about 0.25 ms before, then
         * spin the rest: a timer can only end late, so the gap is never short */
        if (due - now > 0.35) {
            LARGE_INTEGER d;
            if (!s_pace_timer)
                s_pace_timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* HIGH_RESOLUTION */,
                                                      TIMER_ALL_ACCESS);
            d.QuadPart = -(LONGLONG)((due - now - 0.25) * 10000.0);
            if (s_pace_timer && SetWaitableTimer(s_pace_timer, &d, 0, NULL, NULL, FALSE))
                WaitForSingleObject(s_pace_timer, 20);
        }
        while ((now = qpc_ms()) < due)
            YieldProcessor();
        s_pace_st_ms += now - t0;
        s_pace_st_n++;
    }
    s_pace_last_ms = now;
}

static void pace_stats_line(void)
{
    if (s_screen_st_n) {
        fprintf(stderr, "[GPU-PRESENT] waited for the screen %u times, %.1f ms\n", s_screen_st_n, s_screen_st_ms);
        s_screen_st_n = 0;
        s_screen_st_ms = 0.0;
    }
    if (!s_pace_st_n)
        return;
    fprintf(stderr, "[GPU-PRESENT] paced %u presents, %.1f ms waited\n", s_pace_st_n, s_pace_st_ms);
    s_pace_st_n = 0;
    s_pace_st_ms = 0.0;
}

static void bw_flip(void)
{
    stats_tick();
    check_holes();
    s_st_flips++;
    if (!s_dev)
        return;
    gs_unbind();                        /* item 43: the present and the effects draw without one */
    vsh_finish_compiled();              /* item 43: into the shader cache, drawn again or not */
    bw_gpu_debug_drain();               /* item 25: BW_GPU_DEBUG's messages, once a flip */
    if (bw_vfx_active() && bw_vfx()->tex_restore)
        bw_tr_frame();                  /* T1: a [TEXRESTORE] summary now and then */
    /* The surface the title just finished is the one it presents. */
    ft_flip(s_cur);
    if (s_stats > 0) {
        static double last;
        double now = qpc_ms();
        if (last) {
            double ft = now - last;
            if (!s_st_ft_n || ft < s_st_ft_min) s_st_ft_min = ft;
            if (ft > s_st_ft_max) s_st_ft_max = ft;
            s_st_ft_sum += ft;
            s_st_ft_n++;
        }
        last = now;
    }
    if (s_cur && s_cur->dirty) {
        BwSurf *shown = s_cur;
        double t0 = (s_stats > 0 || trc_on()) ? qpc_ms() : 0.0, t1 = 0.0;
        unsigned long long fl_t0;
        int fl_pres;
        pace_present();                  /* P2b: no sooner than 1/target after the last */
        fl_t0 = g_bw_fl_on ? bw_fl_ticks() : 0;
        fl_pres = gpu_present(shown);    /* before the readback's wait */
        if (fl_t0)
            bw_fl_event(BW_FL_N_FLIP, bw_fl_us_since(fl_t0), s_present_mode ? (unsigned)fl_pres : 2u, s_gen);
        if (trc_on()) {
            double tp = qpc_ms();
            s_trc_pres += tp - t0;
            if (readback_wanted())
                s_trc_rb -= tp;         /* the readback's time is added below */
        }
        if (s_stats > 0) {
            t1 = qpc_ms();
            s_st_pres_ms += t1 - t0;
            if (t1 - t0 > s_st_pres_max) s_st_pres_max = t1 - t0;
        }
        if (readback_wanted()) {
            readback(s_cur);
            if (trc_on())
                s_trc_rb += qpc_ms();
        } else
            s_cur->dirty = 0;
        if (s_stats > 0) {
            double t2 = qpc_ms();
            s_st_rb_ms += t2 - t1;
            if (t2 - t1 > s_st_rb_max) s_st_rb_max = t2 - t1;
        }
        lat_flip();                      /* P2b: GPU frames in flight (BW_GPU_LATENCY) */
        ws_dump(shown);                  /* widescreen: the wide picture, when dumping */
        black_watch(s_cur);
        peek_write(s_cur);
        flash_watch(s_cur);
        {
            /* item 38: the latest binding's pitch, the one readback wrote
             * the rows at (the creation pitch can be the AA surface's) */
            uint32_t pitch = s_cur->cur_pitch ? s_cur->cur_pitch : s_cur->pitch;
            xbox_FramebufferWindowSet(s_cur->va, pitch);
            xbox_FramebufferWindowPresent(s_cur->va, pitch);
        }
    }
    if (s_ws_k > 1.0f)
        ws_frame_end();
    /* T1: finish the restored textures the worker has ready, within the
     * per-frame budget (never a stalled frame); the next draws use them */
    {
        double tq = trc_on() ? qpc_ms() : 0.0;
        if (bw_vfx_active() && bw_vfx()->tex_restore && bw_tr_pump(s_dev, s_ctx) && s_cur)
            bind_targets(s_cur, s_cur_depth);
        if (trc_on()) {                 /* T1 test aid: this frame's breakdown, if it was slow */
            static double last_end;
            double now = qpc_ms(), ft = last_end ? now - last_end : 0.0;
            s_trc_pump += now - tq;
            if (ft > s_trc_on)
                fprintf(stderr, "[TR-TRACE] f%u %.1f ms: prepare %.1f (shaders %.1f for %u compiles,"
                                " restore step %.1f, constants map %.1f), draws %.1f (%u), uploads %.1f (%u),"
                                " present %.1f, readback %.1f, restore finish %.1f, other %.1f;"
                                " %u restored chains drawn for the first time, %u requested (%.2f ms)\n",
                        s_gen, ft, s_trc_prep, s_trc_sh, s_trc_nsh, s_trc_trs, s_trc_map, s_trc_draw,
                        s_trc_ndraw, s_trc_up, s_trc_nup, s_trc_pres,
                        s_trc_rb, s_trc_pump, ft - s_trc_prep - s_trc_draw - s_trc_pres - s_trc_rb - s_trc_pump,
                        s_trc_first, s_trc_req, s_trc_reqt);
            s_trc_reqt = 0.0;
            s_trc_prep = s_trc_draw = s_trc_up = s_trc_pump = s_trc_pres = s_trc_rb = 0.0;
            s_trc_sh = s_trc_trs = s_trc_map = 0.0;
            s_trc_ndraw = s_trc_nup = s_trc_first = s_trc_req = s_trc_nsh = 0;
            last_end = qpc_ms();
        }
    }
    s_gen++;
    bw_ab_frame(s_gen);              /* BW_GPU_AB: the next frame's parity */
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

static const Nv2aBackend s_backend = { bw_clear_m, bw_draw_m, bw_flip };

void bw_d3d11_register(void)
{
    const char *e = getenv("BW_GPU");
    if (!e || strcmp(e, "d3d11"))
        return;
    nv2a_backend_register(&s_backend);
    {
        const char *rc = getenv("BW_GPU_RC");
        if (!(rc && rc[0] == '0'))
            bw_backend_ext_register(&s_xbackend);
        fprintf(stderr, "[GPU-D3D11] register combiners on the GPU: %s\n",
                rc && rc[0] == '0' ? "off (BW_GPU_RC=0)" : "on");
    }
    fprintf(stderr, "[GPU-D3D11] back end registered (BW_GPU=d3d11)\n");
}
