/*
 * bwg_state.h - the renderer's API-neutral core: NV2A states to blend, depth,
 * raster and sampler descriptions (bwg_state.c), lifted verbatim from
 * bw_d3d11.c (lane gfx-core, gate GC).
 *
 * The descriptions use Direct3D's own numbers (D3D11's and D3D12's blend,
 * comparison, stencil, filter and address enums have the same values), so a
 * back end fills its API's description by casting; bw_d3d11.c checks every
 * one at compile time. The core also keeps the state tables' keys and their
 * capacities (BWG_MAX_STATE descriptions of each kind, 32 biased rasteriser
 * states), so every back end runs out of room, and falls back, the same way;
 * the back end keeps its API objects by the slot the core hands out.
 */
#ifndef BWG_STATE_H
#define BWG_STATE_H

#include <stdint.h>
#include "nv2a_backend.h"

/* ---- Direct3D's numbers ---------------------------------------------------- */
enum {
    BWG_BLEND_ZERO = 1, BWG_BLEND_ONE = 2, BWG_BLEND_SRC_COLOR = 3, BWG_BLEND_INV_SRC_COLOR = 4,
    BWG_BLEND_SRC_ALPHA = 5, BWG_BLEND_INV_SRC_ALPHA = 6, BWG_BLEND_DEST_ALPHA = 7,
    BWG_BLEND_INV_DEST_ALPHA = 8, BWG_BLEND_DEST_COLOR = 9, BWG_BLEND_INV_DEST_COLOR = 10,
    BWG_BLEND_SRC_ALPHA_SAT = 11, BWG_BLEND_BLEND_FACTOR = 14, BWG_BLEND_INV_BLEND_FACTOR = 15
};
enum { BWG_BLEND_OP_ADD = 1, BWG_BLEND_OP_SUBTRACT = 2, BWG_BLEND_OP_REV_SUBTRACT = 3,
       BWG_BLEND_OP_MIN = 4, BWG_BLEND_OP_MAX = 5 };
enum { BWG_CMP_NEVER = 1, BWG_CMP_LESS = 2, BWG_CMP_EQUAL = 3, BWG_CMP_LESS_EQUAL = 4,
       BWG_CMP_GREATER = 5, BWG_CMP_NOT_EQUAL = 6, BWG_CMP_GREATER_EQUAL = 7, BWG_CMP_ALWAYS = 8 };
enum { BWG_SOP_KEEP = 1, BWG_SOP_ZERO = 2, BWG_SOP_REPLACE = 3, BWG_SOP_INCR_SAT = 4,
       BWG_SOP_DECR_SAT = 5, BWG_SOP_INVERT = 6, BWG_SOP_INCR = 7, BWG_SOP_DECR = 8 };
enum { BWG_WRITE_RED = 1, BWG_WRITE_GREEN = 2, BWG_WRITE_BLUE = 4, BWG_WRITE_ALPHA = 8 };
enum { BWG_CULL_NONE = 1, BWG_CULL_FRONT = 2, BWG_CULL_BACK = 3 };
enum { BWG_FILTER_MIN_MAG_MIP_POINT = 0, BWG_FILTER_MIN_MAG_MIP_LINEAR = 0x15,
       BWG_FILTER_ANISOTROPIC = 0x55 };
enum { BWG_ADDR_WRAP = 1, BWG_ADDR_MIRROR = 2, BWG_ADDR_CLAMP = 3, BWG_ADDR_BORDER = 4 };
#define BWG_FLOAT32_MAX 3.402823466e+38f        /* D3D11_FLOAT32_MAX, D3D12_FLOAT32_MAX */

#define BWG_MAX_STATE 128       /* descriptions of each kind (blend, depth-stencil, sampler) */
#define BWG_MAX_RS_BIAS 32      /* biased rasteriser states */

/* ---- switches and the title's registers ------------------------------------ */
int bwg_zmode(void);                /* BW_GPU_ZMODE: 0 vertex 1 pixel 2 clip 3 auto */
int bwg_zmode_vertex_clamp(void);   /* clamp z per vertex in the shader (the old way) */
int bwg_use_mips(void);             /* BW_GPU_MIPS=0: level 0 only */
int bwg_use_border(void);           /* BW_GPU_BORDER=0: BORDER reads transparent black */
int bwg_use_conv(void);             /* BW_GPU_CONV=0: no convolution filter */
int bwg_use_persp(void);            /* BW_GPU_PERSP=0: affine colours and fog */
int bwg_conv_kernel(uint32_t filter);   /* the convolution kernel of a texture filter, or 0 */

/* ---- the rasteriser ---------------------------------------------------------- */
enum { BWG_RS_PLAIN, BWG_RS_CULL, BWG_RS_Z, BWG_RS_BIAS };
typedef struct {
    int kind;           /* BWG_RS_*: PLAIN the device's own (no cull, no depth clip), CULL by cull
                         * (1 back, 0 front) and ccw, Z by clip, cull (0 none 1 front 2 back) and
                         * ccw, BIAS as Z plus the polygon offset */
    int clip, cull, ccw;
    float factor, units;
} BwgRaster;
typedef struct {
    int cull_mode;      /* BWG_CULL_* */
    int front_ccw, depth_clip;
    int depth_bias;     /* in steps of the D24 buffer */
    float slope_bias, bias_clamp;
} BwgRasterDesc;
/* The rasteriser this draw takes; 0 when the title culls both faces (draw nothing). */
int bwg_raster(const Nv2aRenderState *rs, BwgRaster *out);
/* Its description (every kind but PLAIN; the scissor is always on). */
void bwg_raster_desc(const BwgRaster *r, BwgRasterDesc *d);
/* The biased states' table: the slot of this one (>= 0), -1 when it isn't
 * there and there's room (make it, then bwg_rs_bias_add), -2 when the table
 * is full (draw with the Z state instead). */
int bwg_rs_bias_find(const BwgRaster *r);
int bwg_rs_bias_add(const BwgRaster *r);

/* ---- blend, depth-stencil ------------------------------------------------------ */
typedef struct {
    int a2c, enable;
    int src, dst, op, src_alpha, dst_alpha, op_alpha;
    uint8_t write_mask;
} BwgBlendDesc;
typedef struct {
    int depth_enable, depth_func, depth_write_all, stencil_enable;
    uint8_t read_mask, write_mask;
    int func, fail, zfail, pass;        /* both faces */
} BwgDepthDesc;
/* The slot of this draw's blend (a2c: with alpha-to-coverage) or depth-stencil
 * state, -1 when its table is full (bind none). *made: 0 when the slot is new
 * and *d its description (the back end makes the object); 1 when it is known. */
int bwg_blend(const Nv2aRenderState *rs, int a2c, BwgBlendDesc *d, int *made);
int bwg_depth(const Nv2aRenderState *rs, int has_depth, BwgDepthDesc *d, int *made);
/* The constant blend factor (D3D's blend factor, 0..1). */
void bwg_blend_factor(const Nv2aRenderState *rs, float bf[4]);

/* ---- samplers -------------------------------------------------------------------- */
typedef struct {
    int filter, max_aniso;
    int addr_u, addr_v, addr_w;
    float border[4];
    float min_lod, max_lod;
    int cmp;
} BwgSamplerDesc;
/* One table for all three kinds, as the slot and *made of bwg_blend:
 *   bwg_sampler_ss  the screen-space path's old sampler (always bilinear)
 *   bwg_sampler_f   the texture's own filter, wrap, border and mips
 *   bwg_sampler_tr  a stage bound to a restored chain (T1, Remastered), with
 *                   lk enlarged levels; for a mipped stage it is bwg_sampler_f's */
int bwg_sampler_ss(uint32_t addr_u, uint32_t addr_v, BwgSamplerDesc *d, int *made);
int bwg_sampler_f(uint32_t addr_u, uint32_t addr_v, uint32_t filter, int border_src, uint32_t border,
                  BwgSamplerDesc *d, int *made);
int bwg_sampler_tr(uint32_t addr_u, uint32_t addr_v, uint32_t filter, int border_src, uint32_t border,
                   int lk, BwgSamplerDesc *d, int *made);

/* ---- the scissor ------------------------------------------------------------------ */
typedef struct { long left, top, right, bottom; } BwgRect;
/* The title's window clip as the scissor rectangle on a surface of logical
 * w x h, drawn at pw x ph physical pixels. */
void bwg_scissor(const Nv2aSurface *s, uint32_t w, uint32_t h, uint32_t pw, uint32_t ph, BwgRect *r);

#endif
