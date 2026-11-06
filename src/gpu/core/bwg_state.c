/*
 * bwg_state.c - the renderer's API-neutral core: NV2A states to blend,
 * depth, raster and sampler descriptions, the polygon offset, the depth
 * range, the border and convolution filters and the scissor (bwg_state.h).
 *
 * Lifted verbatim from bw_d3d11.c (lane gfx-core, gate GC: the same
 * decisions in the same order, the same floating-point expressions). The
 * descriptions carry Direct3D's numbers; the back end makes its objects from
 * them, by the slots of the tables kept here.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bwg_core.h"
#include "bwg_state.h"
#include "toolkit_patches/kernel/bw_backend_ext.h"
#include "../bw_vfx.h"                  /* bw_vfx_aniso (VFX, D9; 1 in Original) */

extern int bw_ab_off(const char *fix);      /* BW_GPU_AB (bw_ab.c) */

/* ---- the depth range, culling, the polygon offset -------------------------- */

static int s_zmode = -1;                         /* BW_GPU_ZMODE: 0 vertex 1 pixel 2 clip 3 auto */
static int s_cull = -1;                          /* BW_GPU_CULL: 0 off, 1 on, 2 flipped */

int bwg_zmode(void)
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
int bwg_zmode_vertex_clamp(void)
{
    zminmax_control();
    return bwg_zmode() == 0;
}

/* Depth clip in the rasteriser: on for "clip", or for "auto" when the
 * title selects cull. */
static int zmode_depth_clip(void)
{
    int m = bwg_zmode();
    return m == 2 || (m == 3 && ((zminmax_control() >> 4) & 0xF) == 0);
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

/* Back-face culling as the title set it. NV2A takes GL's values:
 * SET_CULL_FACE GL_FRONT 0x404 / GL_BACK 0x405 / GL_FRONT_AND_BACK 0x408,
 * SET_FRONT_FACE GL_CW 0x900 / GL_CCW 0x901. The executor hands over surface
 * pixels with y down, the same space D3D11's rasteriser judges winding in;
 * which way NV2A counts in that space was checked against frames
 * (BW_GPU_CULL=flip draws the other convention, ANALYSIS). Returns 0 when
 * the title culls both faces: nothing to draw. */
int bwg_raster(const Nv2aRenderState *rs, BwgRaster *o)
{
    int back, ccw;
    memset(o, 0, sizeof *o);
    if (bwg_zmode() != 0) {
        /* the depth experiment: one cache for clip x cull x winding */
        int clip = zmode_depth_clip(), cull = 0;
        ccw = 0;
        if (s_cull < 0) {
            const char *e = getenv("BW_GPU_CULL");
            s_cull = !e ? 1 : e[0] == '0' ? 0 : (e[0] == 'f' || e[0] == '2') ? 2 : 1;
        }
        if (s_cull && rs && rs->cull_enable) {
            if (rs->cull_face == 0x408)
                return 0;
            cull = rs->cull_face != 0x404 ? 2 : 1;
            ccw = (rs->front_face == 0x901) ^ (s_cull == 2);
        }
        o->clip = clip; o->cull = cull; o->ccw = ccw;
        {
            float factor, units;
            if (poly_offset(&factor, &units)) {
                o->kind = BWG_RS_BIAS;
                o->factor = factor; o->units = units;
                return 1;
            }
        }
        o->kind = BWG_RS_Z;
        return 1;
    }
    if (s_cull < 0) {
        const char *e = getenv("BW_GPU_CULL");
        s_cull = !e ? 1 : e[0] == '0' ? 0 : (e[0] == 'f' || e[0] == '2') ? 2 : 1;
    }
    if (!s_cull || !rs || !rs->cull_enable) {
        o->kind = BWG_RS_PLAIN;
        return 1;
    }
    if (rs->cull_face == 0x408)
        return 0;
    back = rs->cull_face != 0x404;
    ccw = (rs->front_face == 0x901) ^ (s_cull == 2);
    o->kind = BWG_RS_CULL;
    o->cull = back; o->ccw = ccw;
    return 1;
}

void bwg_raster_desc(const BwgRaster *r, BwgRasterDesc *d)
{
    memset(d, 0, sizeof *d);
    if (r->kind == BWG_RS_CULL) {
        d->cull_mode = r->cull ? BWG_CULL_BACK : BWG_CULL_FRONT;
        d->front_ccw = r->ccw ? 1 : 0;
        d->depth_clip = 0;
        return;
    }
    d->cull_mode = r->cull == 0 ? BWG_CULL_NONE : r->cull == 1 ? BWG_CULL_FRONT : BWG_CULL_BACK;
    d->front_ccw = r->ccw ? 1 : 0;
    d->depth_clip = r->clip ? 1 : 0;
    if (r->kind == BWG_RS_BIAS) {
        float units = r->units;
        d->depth_bias = (int)(units < 0.0f ? units - 0.5f : units + 0.5f);
        d->slope_bias = r->factor;
        d->bias_clamp = 0.0f;
    }
}

static struct { int clip, cull, ccw; float factor, units; } s_rs_bias[BWG_MAX_RS_BIAS];
static int s_nrs_bias;

int bwg_rs_bias_find(const BwgRaster *r)
{
    int clip = r->clip, cull = r->cull, ccw = r->ccw;
    float factor = r->factor, units = r->units;
    int i;
    for (i = 0; i < s_nrs_bias; i++)
        if (s_rs_bias[i].clip == clip && s_rs_bias[i].cull == cull && s_rs_bias[i].ccw == ccw
                && s_rs_bias[i].factor == factor && s_rs_bias[i].units == units)
            return i;
    if (s_nrs_bias == BWG_MAX_RS_BIAS)
        return -2;
    return -1;
}

int bwg_rs_bias_add(const BwgRaster *r)
{
    s_rs_bias[s_nrs_bias].clip = r->clip; s_rs_bias[s_nrs_bias].cull = r->cull;
    s_rs_bias[s_nrs_bias].ccw = r->ccw; s_rs_bias[s_nrs_bias].factor = r->factor;
    s_rs_bias[s_nrs_bias].units = r->units;
    return s_nrs_bias++;
}

/* ---- render state ------------------------------------------------------- */

static int map_blend(uint32_t f)
{
    switch (f) {
    case 0: return BWG_BLEND_ZERO;
    case 1: return BWG_BLEND_ONE;
    case 0x300: return BWG_BLEND_SRC_COLOR;
    case 0x301: return BWG_BLEND_INV_SRC_COLOR;
    case 0x302: return BWG_BLEND_SRC_ALPHA;
    case 0x303: return BWG_BLEND_INV_SRC_ALPHA;
    case 0x304: return BWG_BLEND_DEST_ALPHA;
    case 0x305: return BWG_BLEND_INV_DEST_ALPHA;
    case 0x306: return BWG_BLEND_DEST_COLOR;
    case 0x307: return BWG_BLEND_INV_DEST_COLOR;
    case 0x308: return BWG_BLEND_SRC_ALPHA_SAT;
    case 0x8001: case 0x8003: return BWG_BLEND_BLEND_FACTOR;
    case 0x8002: case 0x8004: return BWG_BLEND_INV_BLEND_FACTOR;
    default: return BWG_BLEND_ONE;
    }
}

static int alpha_only(int b)
{
    /* colour factors are not allowed in the alpha blend */
    switch (b) {
    case BWG_BLEND_SRC_COLOR: return BWG_BLEND_SRC_ALPHA;
    case BWG_BLEND_INV_SRC_COLOR: return BWG_BLEND_INV_SRC_ALPHA;
    case BWG_BLEND_DEST_COLOR: return BWG_BLEND_DEST_ALPHA;
    case BWG_BLEND_INV_DEST_COLOR: return BWG_BLEND_INV_DEST_ALPHA;
    default: return b;
    }
}

static int map_blend_op(uint32_t e)
{
    switch (e) {
    case 0x8007: return BWG_BLEND_OP_MIN;
    case 0x8008: return BWG_BLEND_OP_MAX;
    case 0x800A: return BWG_BLEND_OP_SUBTRACT;
    case 0x800B: return BWG_BLEND_OP_REV_SUBTRACT;
    default: return BWG_BLEND_OP_ADD;
    }
}

static int map_cmp(uint32_t f)
{
    switch (f) {
    case 0x200: return BWG_CMP_NEVER;
    case 0x201: return BWG_CMP_LESS;
    case 0x202: return BWG_CMP_EQUAL;
    case 0x203: return BWG_CMP_LESS_EQUAL;
    case 0x204: return BWG_CMP_GREATER;
    case 0x205: return BWG_CMP_NOT_EQUAL;
    case 0x206: return BWG_CMP_GREATER_EQUAL;
    default: return BWG_CMP_ALWAYS;
    }
}

static int map_sop(uint32_t o)
{
    switch (o) {
    case 0: return BWG_SOP_ZERO;
    case 0x1E01: return BWG_SOP_REPLACE;
    case 0x1E02: return BWG_SOP_INCR_SAT;
    case 0x1E03: return BWG_SOP_DECR_SAT;
    case 0x150A: return BWG_SOP_INVERT;
    case 0x8507: return BWG_SOP_INCR;
    case 0x8508: return BWG_SOP_DECR;
    default: return BWG_SOP_KEEP;
    }
}

/* The tables: a description's key, in the order first asked for; a full
 * table hands out no slot (the back end binds none). A slot is kept even
 * when the back end couldn't make its object. */
static struct { uint32_t key[6]; } s_blend[BWG_MAX_STATE];
static int s_nblend;
static struct { uint32_t key[10]; } s_ds[BWG_MAX_STATE];
static int s_nds;
static struct { uint32_t key[3]; } s_samp[BWG_MAX_STATE];
static int s_nsamp;

int bwg_blend(const Nv2aRenderState *rs, int a2c, BwgBlendDesc *d, int *made)
{
    uint32_t key[6] = { rs->blend_enable, rs->blend_src, rs->blend_dst, rs->blend_eq,
                        rs->color_mask, (uint32_t)a2c };
    uint8_t mask = 0;
    *made = 1;
    for (int i = 0; i < s_nblend; i++)
        if (!memcmp(s_blend[i].key, key, sizeof(key)))
            return i;
    if (s_nblend == BWG_MAX_STATE)
        return -1;
    memset(d, 0, sizeof(*d));
    d->a2c = a2c ? 1 : 0;           /* VFX (D9); 0 in Original */
    d->enable = rs->blend_enable ? 1 : 0;
    d->src = map_blend(rs->blend_src);
    d->dst = map_blend(rs->blend_dst);
    d->op = map_blend_op(rs->blend_eq);
    d->src_alpha = alpha_only(d->src);
    d->dst_alpha = alpha_only(d->dst);
    d->op_alpha = d->op;
    if (rs->color_mask & 0x00010000u) mask |= BWG_WRITE_RED;
    if (rs->color_mask & 0x00000100u) mask |= BWG_WRITE_GREEN;
    if (rs->color_mask & 0x00000001u) mask |= BWG_WRITE_BLUE;
    if (rs->color_mask & 0x01000000u) mask |= BWG_WRITE_ALPHA;
    d->write_mask = mask;
    memcpy(s_blend[s_nblend].key, key, sizeof(key));
    *made = 0;
    return s_nblend++;
}

int bwg_depth(const Nv2aRenderState *rs, int has_depth, BwgDepthDesc *d, int *made)
{
    uint32_t key[10] = { has_depth && rs->depth_test_enable, rs->depth_func,
                         has_depth && rs->depth_write, has_depth && rs->stencil_enable,
                         rs->stencil_func, rs->stencil_func_mask, rs->stencil_write_mask,
                         rs->stencil_fail, rs->stencil_zfail, rs->stencil_zpass };
    *made = 1;
    for (int i = 0; i < s_nds; i++)
        if (!memcmp(s_ds[i].key, key, sizeof(key)))
            return i;
    if (s_nds == BWG_MAX_STATE)
        return -1;
    memset(d, 0, sizeof(*d));
    d->depth_enable = key[0] || key[2] ? 1 : 0;
    d->depth_func = key[0] ? map_cmp(rs->depth_func) : BWG_CMP_ALWAYS;
    d->depth_write_all = key[2] ? 1 : 0;
    d->stencil_enable = key[3] ? 1 : 0;
    d->read_mask = (uint8_t)rs->stencil_func_mask;
    d->write_mask = (uint8_t)rs->stencil_write_mask;
    d->func = map_cmp(rs->stencil_func);
    d->fail = map_sop(rs->stencil_fail);
    d->zfail = map_sop(rs->stencil_zfail);
    d->pass = map_sop(rs->stencil_zpass);
    memcpy(s_ds[s_nds].key, key, sizeof(key));
    *made = 0;
    return s_nds++;
}

void bwg_blend_factor(const Nv2aRenderState *rs, float bf[4])
{
    bf[0] = ((rs->blend_color >> 16) & 0xFF) / 255.0f;
    bf[1] = ((rs->blend_color >> 8) & 0xFF) / 255.0f;
    bf[2] = (rs->blend_color & 0xFF) / 255.0f;
    bf[3] = ((rs->blend_color >> 24) & 0xFF) / 255.0f;
    if (rs->blend_src == 0x8003 || rs->blend_src == 0x8004 ||
        rs->blend_dst == 0x8003 || rs->blend_dst == 0x8004)
        bf[0] = bf[1] = bf[2] = bf[3];
}

/* ---- samplers ----------------------------------------------------------- */

static int samp_slot(const uint32_t key[3], int *made)
{
    *made = 1;
    for (int i = 0; i < s_nsamp; i++)
        if (!memcmp(s_samp[i].key, key, 3 * sizeof(uint32_t)))
            return i;
    if (s_nsamp == BWG_MAX_STATE)
        return -1;
    return -2;          /* new: the caller fills the description, then samp_add */
}

static int samp_add(const uint32_t key[3], int *made)
{
    memcpy(s_samp[s_nsamp].key, key, 3 * sizeof(uint32_t));
    *made = 0;
    return s_nsamp++;
}

/* NV097 wrap modes: 1 wrap, 2 mirror, 3 clamp to edge, 4 border */
static int map_addr(uint32_t a)
{
    return a == 1 ? BWG_ADDR_WRAP : a == 2 ? BWG_ADDR_MIRROR
         : a == 4 ? BWG_ADDR_BORDER : BWG_ADDR_CLAMP;
}

/* The screen-space path's old sampler: always bilinear (BW_GPU_SSFILTER=0).
 * Its keys carry bit 31 so they never alias get_sampler_f's: they used to,
 * and a point-filtered combiner sampler and this bilinear one with the same
 * wrap modes were handed out for each other, whichever came first. */
int bwg_sampler_ss(uint32_t addr_u, uint32_t addr_v, BwgSamplerDesc *d, int *made)
{
    uint32_t key[3] = { addr_u | 0x80000000u, addr_v, 0 };
    int i = samp_slot(key, made);
    if (i != -2)
        return i;
    memset(d, 0, sizeof(*d));
    d->filter = BWG_FILTER_MIN_MAG_MIP_LINEAR;
    d->addr_u = map_addr(addr_u);
    d->addr_v = map_addr(addr_v);
    d->addr_w = BWG_ADDR_CLAMP;
    d->max_lod = BWG_FLOAT32_MAX;
    d->cmp = BWG_CMP_NEVER;
    return samp_add(key, made);
}

/* Upload without the per-texel decoder where the layout allows: DXT blocks
 * go to the GPU as they are (BC1-3 are the same S3TC blocks), the common
 * swizzled and linear 32-bit and R5G6B5 formats are unswizzled or copied in
 * bulk. Returns 0 to fall back to nv2a_backend_decode_texture. */
static int s_mips = -1;         /* BW_GPU_MIPS=0: level 0 only, as the CPU path */

int bwg_use_mips(void)
{
    if (s_mips < 0) {
        const char *e = getenv("BW_GPU_MIPS");
        s_mips = !(e && e[0] == '0');
    }
    return s_mips;
}

/* Point or bilinear, as the CPU path chooses (rc_texel: TENT when MAG is 2
 * or MIN is 2, 4 or 6). */
/* BW_GPU_BORDER=0: BORDER addressing reads transparent black whatever the
 * texture's FORMAT says, as before 2026-10-27. */
int bwg_use_border(void)
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
int bwg_use_conv(void)
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
int bwg_use_persp(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("BW_GPU_PERSP");
        v = !(e && e[0] == '0');
    }
    return v && !bw_ab_off("persp");
}

int bwg_conv_kernel(uint32_t filter)
{
    uint32_t k = (filter >> 13) & 7;
    if (!bwg_use_conv() || !(((filter >> 16) & 0xFF) == 7 || ((filter >> 24) & 0xF) == 4))
        return 0;
    return k ? (int)k : 1;
}

int bwg_sampler_f(uint32_t addr_u, uint32_t addr_v, uint32_t filter, int border_src, uint32_t border,
                  BwgSamplerDesc *d, int *made)
{
    uint32_t mag = (filter >> 24) & 0xF, min = (filter >> 16) & 0xFF;
    int linear = mag == 2 || min == 2 || min == 4 || min == 6 || bwg_conv_kernel(filter);
    /* NV097 MIN: 1 nearest, 2 linear (level 0 only), 3/4 nearest/linear with
     * the nearest mip, 5/6 nearest/linear blending two mips. */
    int mipped = bwg_use_mips() && min >= 3 && min <= 6;
    int min_lin = min == 2 || min == 4 || min == 6;
    int mag_lin = mag == 2;
    int mip_lin = min == 5 || min == 6;
    int bordered, i;
    uint32_t key[3];
    if (bwg_use_border() && !border_src) {
        if (addr_u == 4) addr_u = 3;
        if (addr_v == 4) addr_v = 3;
    }
    bordered = bwg_use_border() && border_src && (addr_u == 4 || addr_v == 4);
    key[0] = addr_u | (linear ? 0x100u : 0u) | (mipped ? 0x1000u | (min << 16) | (mag << 24) : 0u);
    key[1] = addr_v | (bordered ? 0x100u : 0u);
    key[2] = bordered ? border : 0;
    i = samp_slot(key, made);
    if (i != -2)
        return i;
    memset(d, 0, sizeof(*d));
    if (mipped && bw_vfx_aniso() > 1 && (min_lin || mag_lin)) {
        /* VFX (D9): anisotropic for the mipped, filtered textures (the
         * 3D world); point-sampled ones stay as the game set them. */
        d->filter = BWG_FILTER_ANISOTROPIC;
        d->max_aniso = (int)bw_vfx_aniso();
    } else if (mipped) {
        /* D3D filter bits: MIN linear 0x10, MAG linear 0x04, MIP linear 0x01. */
        d->filter = (min_lin ? 0x10 : 0) | (mag_lin ? 0x04 : 0) | (mip_lin ? 0x01 : 0);
    } else {
        d->filter = linear ? BWG_FILTER_MIN_MAG_MIP_LINEAR : BWG_FILTER_MIN_MAG_MIP_POINT;
    }
    d->addr_u = map_addr(addr_u);
    d->addr_v = map_addr(addr_v);
    d->addr_w = BWG_ADDR_CLAMP;
    if (bordered) {
        d->border[0] = ((border >> 16) & 0xFF) / 255.0f;
        d->border[1] = ((border >> 8) & 0xFF) / 255.0f;
        d->border[2] = (border & 0xFF) / 255.0f;
        d->border[3] = (border >> 24) / 255.0f;
    }
    d->max_lod = mipped ? BWG_FLOAT32_MAX : 0.0f;
    d->cmp = BWG_CMP_NEVER;
    return samp_add(key, made);
}

/* T1 (VFX_TEX_RESTORE): the sampler for a stage bound to a restored chain.
 * A mipped stage keeps get_sampler_f's own (the chain is just two levels
 * longer at the top). A stage the game samples at level 0 only (MIN 1 or 2)
 * gets the same filters with linear blending between levels and MaxLOD at
 * the chain's copy of the game's level 0, so at or below painted size it
 * reads exactly that level, as the game does, and the enlarged levels only
 * above it. Remastered only (never called in Original). */
int bwg_sampler_tr(uint32_t addr_u, uint32_t addr_v, uint32_t filter, int border_src, uint32_t border,
                   int lk, BwgSamplerDesc *d, int *made)
{
    uint32_t mag = (filter >> 24) & 0xF, min = (filter >> 16) & 0xFF;
    int mipped = bwg_use_mips() && min >= 3 && min <= 6;
    int min_lin = min == 2 || min == 4 || min == 6, bordered, i;
    uint32_t key[3];
    if (mipped)
        return bwg_sampler_f(addr_u, addr_v, filter, border_src, border, d, made);
    if (bwg_use_border() && !border_src) {
        if (addr_u == 4) addr_u = 3;
        if (addr_v == 4) addr_v = 3;
    }
    bordered = bwg_use_border() && border_src && (addr_u == 4 || addr_v == 4);
    key[0] = addr_u | 0x2000u | ((uint32_t)lk << 14) | (min_lin ? 0x100u : 0u) | (mag << 24);
    key[1] = addr_v | (bordered ? 0x100u : 0u);
    key[2] = bordered ? border : 0;
    i = samp_slot(key, made);
    if (i != -2)
        return i;
    memset(d, 0, sizeof(*d));
    d->filter = (min_lin ? 0x10 : 0) | (mag == 2 ? 0x04 : 0) | 0x01;
    d->addr_u = map_addr(addr_u);
    d->addr_v = map_addr(addr_v);
    d->addr_w = BWG_ADDR_CLAMP;
    if (bordered) {
        d->border[0] = ((border >> 16) & 0xFF) / 255.0f;
        d->border[1] = ((border >> 8) & 0xFF) / 255.0f;
        d->border[2] = (border & 0xFF) / 255.0f;
        d->border[3] = (border >> 24) / 255.0f;
    }
    d->min_lod = 0.0f;
    d->max_lod = (float)lk;
    d->cmp = BWG_CMP_NEVER;
    return samp_add(key, made);
}

/* ---- the scissor ------------------------------------------------------------ */

/* The title's window clip as the scissor rectangle. NV097 window clip 0
 * (SET_WINDOW_CLIP_HORIZONTAL 0x02C0 / _VERTICAL 0x02E0: min in bits 0-11,
 * max in bits 16-27, inclusive, in logical pixels) is how split screen keeps
 * each player's view in its half (2-player battle: y 0..239, then 240..479).
 * The executor never applied it; the CPU rasteriser clamps to the surface
 * clip only. Scaled by the AA factor, clamped to the surface. Unset (both 0)
 * or BW_GPU_SCISSOR=0: the whole surface. */
void bwg_scissor(const Nv2aSurface *s, uint32_t cur_w, uint32_t cur_h, uint32_t cur_pw, uint32_t cur_ph,
                 BwgRect *pr)
{
    static int on = -1;
    BwgRect r;
    uint32_t h = bw_backend_reg(0x02C0), v = bw_backend_reg(0x02E0);
    uint32_t sx = s->aa_sx ? s->aa_sx : 1, sy = s->aa_sy ? s->aa_sy : 1;

    if (on < 0) {
        const char *e = getenv("BW_GPU_SCISSOR");
        on = !(e && e[0] == '0');
    }
    r.left = 0; r.top = 0;
    r.right = (long)cur_w; r.bottom = (long)cur_h;
    if (on && (h || v) && bw_backend_reg(0x02B4) == 0) {
        long x0 = (long)(h & 0xFFF) * sx, x1 = ((long)((h >> 16) & 0xFFF) + 1) * sx;
        long y0 = (long)(v & 0xFFF) * sy, y1 = ((long)((v >> 16) & 0xFFF) + 1) * sy;
        if (x0 > r.left) r.left = x0;
        if (y0 > r.top) r.top = y0;
        if (x1 < r.right) r.right = x1;
        if (y1 < r.bottom) r.bottom = y1;
        if (r.right < r.left) r.right = r.left;
        if (r.bottom < r.top) r.bottom = r.top;
    }
    if (cur_pw != cur_w || cur_ph != cur_h) {
        /* VFX (D9): logical pixels to the render-scaled surface's */
        float fx = (float)cur_pw / cur_w, fy = (float)cur_ph / cur_h;
        r.left = (long)(r.left * fx + 0.5f); r.right = (long)(r.right * fx + 0.5f);
        r.top = (long)(r.top * fy + 0.5f); r.bottom = (long)(r.bottom * fy + 0.5f);
    }
    *pr = r;
}
