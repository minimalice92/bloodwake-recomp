/*
 * bw_backend_ext.h - Blood Wake extension of the toolkit's render back-end
 * interface (nv2a_backend.h), carried with the patched nv2a_pb_exec.c.
 *
 * The toolkit's interface hands a back end stage 0 and the diffuse colour, so
 * a GPU back end cannot reproduce the register combiners the CPU rasteriser
 * evaluates (nv2a_combiner.c): the water's sunset reflection, lit walls. For
 * transformed (vertex-program / fixed-function) batches this hands over
 * everything the CPU path's per-pixel loop reads instead:
 *   - per vertex: position (surface pixels, rhw), both colours, the fog
 *     factor after the fog unit, and all four texture coordinate sets;
 *   - per batch: all four texture stages, the stage programs, the combiner
 *     registers, the fog colour and the clip-plane mode.
 * Screen-space (pre-transformed) batches still use the toolkit's draw().
 */
#ifndef BW_BACKEND_EXT_H
#define BW_BACKEND_EXT_H

#include <stdint.h>
#include "nv2a_backend.h"
#include "nv2a_combiner.h"

typedef struct {
    float x, y, z, rhw;         /* surface pixels, z as produced, 1/w */
    float d0[4], d1[4];         /* diffuse, specular: r, g, b, a in [0,1] */
    float fog;                  /* fog factor (1 = no fog) */
    float pad[3];
    float tc[4][4];             /* texture coordinates, stages 0-3 (s, t, r, q) */
} BwXVertex;                    /* 32 floats */

typedef struct {
    const BwXVertex *vertices;  /* triangle list */
    uint32_t count;
    const Nv2aRenderState *state;
    const Nv2aCombiner *rc;
    Nv2aTexture tex[4];         /* valid only where tex_valid[i] */
    uint8_t tex_valid[4];
    uint8_t tex_normalized[4];  /* coordinates are 0..1 (swizzled, DXT); else texels */
    uint8_t tex_cube[4];
    float fogc[4];              /* fog colour r, g, b (a unused) */
    uint32_t clip_plane_mode;   /* SET_SHADER_CLIP_PLANE_MODE */
    uint32_t dot_rgbmapping;    /* SET_DOT_RGBMAPPING: stage i's input mapping in bits 4(i-1)..+3 */
    uint32_t other_stage_input; /* SET_SHADER_OTHER_STAGE_INPUT: stage 2's source in 16-19, 3's in 20-23 */
    uint32_t tex_border[4];     /* SET_TEXTURE_BORDER_COLOR (0xAARRGGBB) per stage */
    uint8_t tex_border_src[4];  /* FORMAT's border source: 1 = that colour, 0 = the texture's edge */
} BwXBatch;

/* Vertex programs on the GPU. Instead of running the title's vertex program
 * on the CPU once per vertex (nv2a_vsh_interp.c), the executor hands the
 * batch over untransformed: the program, the constants, each distinct
 * vertex's raw inputs as floats, and a triangle list indexing them. The back
 * end runs the program as a vertex shader with the interpreter's semantics,
 * then does what the executor does after it: w = +0 taken as 1, the
 * anti-aliasing scale, near-plane clipping and the fog unit. */
/* Blood Wake P1: GPU-side vertex fetch (BW_GPU_VFETCH). Instead of `inputs`
 * (each distinct vertex's attributes, read and converted to floats on the CPU
 * by fetch_attr), the batch's index range of each vertex stream as the
 * title's own bytes. The back end's input layout and shader prologue convert
 * each format exactly as fetch_attr does: floats are copied, the integer
 * formats go through a table the CPU filled with fetch_attr's expressions. */
#define BW_XVS_MAX_STREAMS 16
enum {                            /* BwXVsInput.conv >> 4; the low nibble is the size */
    BW_XVS_GATHERED = 0,          /* `inputs` (no streams) */
    BW_XVS_FLOAT = 1,             /* type 2, 1-4 floats */
    BW_XVS_COLOR = 2,             /* type 0, D3DCOLOR: bytes B,G,R,A -> r,g,b,a / 255 */
    BW_XVS_UBYTE = 3,             /* type 4, 1-4 bytes / 255 */
    BW_XVS_SHORT = 4,             /* type 5, 1-4 shorts as they are */
    BW_XVS_CMP = 5,               /* type 6, packed 11:11:10 normal */
    BW_XVS_CONST = 6              /* no array: the current value (4 floats, stride 0) */
};
typedef struct {
    const void *data;             /* the span: vertex `first` of the stream's lowest attribute */
    uint32_t bytes;
    uint32_t stride;              /* 0: one value for every vertex */
} BwXVsStream;
typedef struct {
    uint8_t stream;               /* index into streams */
    uint8_t conv;                 /* BW_XVS_* << 4 | components */
    uint16_t offset;              /* the attribute's byte offset in a vertex of its stream */
} BwXVsInput;

/* Blood Wake (gfx-ffp, BW_GPU_FFP): a fixed-function batch on the GPU. The
 * executor hands it over untransformed, like a vertex-program batch (the
 * same inputs, raw streams or gathered floats, and triangle list), with
 * `program` NULL and `ffp` describing the fixed-function state that
 * ffp_vertex, lit_color and ffp_fog_distance read. The back end draws it
 * with a vertex shader generated from that state, rule for rule as those
 * three functions (bw_ffp_hlsl, bw_vsh_hlsl.c). */
enum {                            /* BwXFfp.in[]: what each input is for */
    BW_FFP_POS, BW_FFP_NRM, BW_FFP_COL, BW_FFP_TC0, BW_FFP_TC1, BW_FFP_TC2, BW_FFP_TC3,
    BW_FFP_FOG, BW_FFP_ROLES
};
typedef struct {
    int8_t in[BW_FFP_ROLES];      /* the input k (ATTRk, the batch's input order) each role reads; -1: none */
    uint8_t lit;                  /* lit_color's path: lighting on, with a normal array */
    uint8_t colmat;               /* lit: bit 0 ambient, bit 1 diffuse from the vertex colour (COLOR_MATERIAL 1) */
    uint8_t fog_on;               /* ffp_fog_distance computes a distance (else 0) */
    uint8_t fog_gen;              /* SET_FOG_GEN_MODE (with fog_on) */
    uint32_t light_mask;          /* lit: SET_LIGHT_ENABLE_MASK (2 bits a light); else 0 */
    float composite[16];          /* the composite matrix, as the executor holds it */
    float modelview[16];          /* model-view 0 (method 0x0480) */
    float ambient[4];             /* SCENE_AMBIENT_COLOR rgb (0x0A10), MATERIAL_ALPHA (0x03B4) */
    float imm_fog[4];             /* fog generation 0 or 6 with no array: v4's or v5's current value */
    float light[8][32];           /* lit: each light's method block (0x1000 + 0x80 i), as floats */
} BwXFfp;

typedef struct {
    BwXBatch xb;                  /* everything except vertices and count */
    const uint32_t (*program)[4]; /* from the start slot ... */
    uint32_t ninstr;              /* ... to the FINAL instruction, inclusive */
    const float (*consts)[4];     /* c[0..191] */
    const float *inputs;          /* nverts x ninputs float4s; NULL with streams */
    uint32_t input_mask;          /* bit i: v<i> is read; ascending order */
    uint32_t ninputs, nverts;
    const uint32_t *indices;      /* triangle list into the vertices */
    uint32_t nindices;
    float aa[2];                  /* surface pixels per logical pixel */
    int near_clip;                /* also clip at the near plane */
    uint32_t fog_enable, fog_mode;
    float fog_param[2];
    /* Blood Wake P1: with nstreams > 0 the vertices are the streams' spans
     * (vertex i at data + i * stride), and in[k] says where input k is. */
    uint32_t nstreams;
    BwXVsStream streams[BW_XVS_MAX_STREAMS];
    BwXVsInput in[16];
    /* P1: bumped by every write to c[0..191] (nv2a_vsh_constants_gen); the
     * same value as the last batch's means `consts` did not change */
    uint32_t consts_gen;
    /* gfx-sprites (I6): point sprites on the GPU. Set only for a point batch
     * the back end took (points_ok below); 0 for every other batch. `indices`
     * is then a point list (one index per point, nindices points), and the
     * back end makes each point's quad by bw_draw_points's rules
     * (nv2a_pb_exec.c): the size in logical pixels, stage 3's sprite
     * coordinates, the winding from the cull state, points behind the eye
     * dropped. */
    uint32_t points;              /* 1: a point list */
    float point_size;             /* > 0: every point's size in logical pixels
                                     (SET_POINT_SIZE / 8, clamped to 1..63.875);
                                     0: the program's oPts.x, clamped the same way */
    uint32_t point_sprite;        /* SET_POINT_SMOOTH_ENABLE: stage 3's coordinates
                                     run (0,0) top-left to (1,1) bottom-right */
    uint32_t point_cw;            /* the quads' winding: clockwise on the surface (y down) */
    /* gfx-ffp: non-NULL for a fixed-function batch (program NULL, ninstr 0);
     * sent only to a back end whose ffp_ok() says yes */
    const BwXFfp *ffp;
} BwXVsBatch;

/* gfx-sprites (I6), for checking: one vertex of the sprite expansion of the
 * last point batch, as the back end's geometry stage made it, mapped back
 * to the executor's terms (pos as bw_draw_points leaves it: surface pixels,
 * z as produced, w). */
typedef struct {
    float pos[4];
    float tc3[4];               /* stage 3's coordinates */
    uint32_t point;             /* the point's position in the batch (0..nindices-1) */
    uint32_t corner;            /* 0 TL, 1 TR, 2 BL, 3 BR (bw_draw_points's q[k]) */
    float pad[2];
} BwXSpriteOut;                 /* 12 floats */

/* What the back end's vertex shader produced for one vertex, mapped back to
 * the executor's terms (pos as transform_vertex leaves it: surface pixels,
 * z as produced, w), for BW_GPU_VSH_VERIFY. */
typedef struct {
    float pos[4];
    float d0[4], d1[4];
    float fog, clip[2], pad;    /* fog factor; clip distances (w, near plane) */
    float tc[4][4];
} BwXVsOut;                     /* 32 floats */

typedef struct {
    void (*draw)(const Nv2aSurface *s, const BwXBatch *b);
    /* Optional. Returns 1 if it drew the batch, 0 to have the executor
     * transform it on the CPU instead. */
    int (*draw_vsh)(const Nv2aSurface *s, const BwXVsBatch *b);
    /* Optional, for checking: re-run the last draw_vsh batch's vertices
     * through its shader and read the results back (slow). Returns 1 on
     * success. */
    int (*vsh_readback)(BwXVsOut *out, uint32_t nverts);
    /* Optional (Blood Wake P1): 1 if the back end takes the next batch as
     * raw streams, 0 while it needs the gathered floats (it reads them on
     * the CPU). Without it, streams are never sent. */
    int (*vsh_raw_ok)(void);
    /* Optional (gfx-sprites, I6): 1 if the back end draws the next point
     * batch of a vertex program on the GPU (draw_vsh with `points` set), 0
     * to keep today's path (the executor transforms and expands it on the
     * CPU, bw_draw_points). Without it, points stay on the CPU. */
    int (*points_ok)(void);
    /* Optional, for checking (gfx-sprites): run the last point batch through
     * its vertex shader and the sprite expansion again, and read the
     * expansion back: 6 vertices (two triangles, in drawing order) per
     * point it kept. Returns 1 on success, with *count vertices in out
     * (at most max). */
    int (*sprite_readback)(BwXSpriteOut *out, uint32_t max, uint32_t *count);
    /* Optional (gfx-ffp): 1 if draw_vsh takes fixed-function batches (ffp
     * set). Without it, or 0, they stay on the CPU path as before. Asked once
     * per fixed-function batch; the answer may change only at start-up. */
    int (*ffp_ok)(void);
} BwXBackend;

/* One instruction field, (dword, lowest bit, width), as nv2a_vsh_interp.c. */
static inline uint32_t bw_vsh_field(const uint32_t *ins, int dw, int lo, int width)
{
    return (ins[dw] >> lo) & ((1u << width) - 1u);
}

/* The inputs a program reads (bit i for v<i>), counting only the sources
 * each op uses: A for any MAC op, B for mul, mad, dp3/dph/dp4, dst, min, max,
 * slt, sge; C for add, mad and every ILU op. All three share one input index.
 * The executor fetches exactly these and the shader reads them in this order,
 * so both sides call this. */
static inline uint32_t bw_vsh_inputs_used(const uint32_t (*p)[4], uint32_t n)
{
    uint32_t m = 0, i;
    for (i = 0; i < n; i++) {
        const uint32_t *ins = p[i];
        uint32_t mac = bw_vsh_field(ins, 1, 21, 4), ilu = bw_vsh_field(ins, 1, 25, 3);
        int ua = mac != 0;
        int ub = mac == 2 || (mac >= 4 && mac <= 12);
        int uc = mac == 3 || mac == 4 || ilu != 0;
        if ((ua && bw_vsh_field(ins, 2, 26, 2) == 2)
         || (ub && bw_vsh_field(ins, 2, 11, 2) == 2)
         || (uc && bw_vsh_field(ins, 3, 28, 2) == 2))
            m |= 1u << bw_vsh_field(ins, 1, 9, 4);
    }
    return m;
}

/* Register (or remove) the extended entry. The executor uses it for
 * transformed batches while a toolkit back end is also registered. */
void bw_backend_ext_register(const BwXBackend *backend);

/* gfx-ffp: fixed-function batches so far: [0] drawn on the GPU (ffp_ok),
 * [1] drawn on the CPU path, [2] of those, sent back because a gathered
 * vertex couldn't be read. */
void bw_exec_ffp_counts(unsigned long long out[3]);

/* During clear(): the title's clear rectangle (SET_CLEAR_RECT_HORIZONTAL /
 * _VERTICAL) as x0, y0, x1, y1 with x1/y1 exclusive, in the title's
 * (logical, non-anti-aliased) pixels. Returns 0 if it never sent both. */
int bw_backend_clear_rect(uint32_t r[4]);

/* The last value the title wrote to NV097 method `method` (the executor's
 * register mirror), e.g. 0x1D78 SET_ZMIN_MAX_CONTROL. */
uint32_t bw_backend_reg(uint32_t method);

/* gfx-sprites: during a draw, 1 if its batch is a point batch, whichever
 * path draws it (the CPU's quads through draw, or draw_vsh's point list);
 * for the region masks' sprite class (BW_GFXCLS_SPRITES, a test aid). */
int bw_backend_batch_points(void);

#endif
