/*
 * bw_gpu_audit.c - what Blood Wake asks of the graphics chip, counted.
 *
 * BW_GPU_AUDIT=1 (test aid; off by default, no effect on the game). The
 * executor (carried nv2a_pb_exec.c) calls in for every pushbuffer method,
 * every draw, clear and flip. Everything is decoded from the executor's own
 * NV097 register mirror, independently of what either renderer models, so
 * the counts say what the title uses rather than what we implemented:
 *   - every (subchannel, method) seen, how often, its distinct values, and
 *     whether the CPU rasteriser and the D3D11 back end use it (the class
 *     table below, from reading both, 2026-10-27);
 *   - per draw, weighted by draws and by vertices: the path the executor
 *     took, primitive, render states, fixed-function features, the
 *     texture-stage program, every combiner input/output feature, every
 *     bound texture's format, addressing, filter and control bits, and the
 *     vertex program's opcodes and outputs;
 *   - clears (flags, rectangle) and flips (the presented surface's format),
 *     and the DAC gamma ramps D3D writes (a display-chain step the
 *     pushbuffer never shows).
 * The summary is rewritten every BW_GPU_AUDIT_EVERY seconds (default 20) to
 * BW_GPU_AUDIT_FILE (default gpu_audit.txt in the working directory, which
 * is the run folder under bw_run --own-cwd), so a run that is killed still
 * leaves its counts; and once more at exit.
 *
 * Single-threaded: everything except bw_audit_gamma runs on the NV2A
 * executor's thread. The gamma hook copies into a slot the flip picks up.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bw_gpu_audit.h"
#include "bw_gpu_methods.h"

/* ---- switch -------------------------------------------------------------- */

static int s_on = -1;

int bw_audit_on(void)
{
    if (s_on < 0) {
        const char *e = getenv("BW_GPU_AUDIT");
        s_on = e && e[0] == '1';
    }
    return s_on;
}

/* ---- method counts ------------------------------------------------------- */

#define AUD_NVAL 6
typedef struct {
    uint64_t count;
    uint32_t val[AUD_NVAL];
    uint64_t vcount[AUD_NVAL];
    uint32_t nval;
    uint64_t other;             /* writes of a value past the AUD_NVAL kept */
} AudMethod;

static AudMethod s_m[8][2048];
static uint32_t s_prog_gen;     /* bumped by every TRANSFORM_PROGRAM / _LOAD write */

/* Methods whose values are data rather than state: counted, values not kept. */
static int bulk_method(uint32_t m)
{
    return (m >= 0x0B00 && m < 0x0C00)         /* transform program, constants */
        || (m >= 0x1500 && m < 0x1720)         /* immediate vertex data */
        || (m >= 0x1800 && m < 0x1B00);        /* array elements, inline, vertex data */
}

void bw_audit_method(uint32_t subch, uint32_t method, uint32_t param)
{
    AudMethod *a = &s_m[subch & 7][(method & 0x1FFC) / 4];
    uint32_t i;
    a->count++;
    if (subch == 0 && ((method >= 0x0B00 && method < 0x0B80) || method == 0x1E9C))
        s_prog_gen++;
    if (subch == 0 && bulk_method(method))
        return;
    for (i = 0; i < a->nval; i++)
        if (a->val[i] == param) {
            a->vcount[i]++;
            return;
        }
    if (a->nval < AUD_NVAL) {
        a->val[a->nval] = param;
        a->vcount[a->nval++] = 1;
    } else {
        a->other++;
    }
}

/* Who uses each NV097 method, read off the code (2026-10-27, branch
 * day/gpu at 1e435ba): cpu = the executor's CPU rasteriser path
 * (nv2a_pb_exec.c), gpu = the D3D11 back end (bw_d3d11.c and its shader
 * generators, through the executor's interfaces). y modelled, p partly
 * (see note), - ignored, n not a rendering state (DMA objects, reports,
 * synchronisation). Unlisted methods print "?". */
typedef struct { uint16_t lo, hi; char cpu, gpu; const char *note; } AudClass;
static const AudClass s_class[] = {
    { 0x0000, 0x0000, 'n', 'n', "object binding" },
    { 0x0100, 0x0100, 'p', 'p', "only the swap NOP's interval (flip pacing) and TK-31's hook" },
    { 0x0110, 0x0110, 'n', 'n', "" },
    { 0x0120, 0x0130, 'y', 'y', "flip" },
    { 0x0180, 0x01A0, 'n', 'n', "DMA objects (offsets taken as physical)" },
    { 0x0200, 0x0204, 'y', 'y', "surface clip" },
    { 0x0208, 0x0208, 'p', 'p', "colour bpp, swizzle, AA scale; zeta format ignored (D3D11 depth is D24S8)" },
    { 0x020C, 0x0210, 'y', 'y', "" },
    { 0x0214, 0x0214, '-', 'y', "CPU keeps its own z buffer" },
    { 0x0260, 0x027C, 'y', 'y', "alpha ICW" },
    { 0x0288, 0x028C, 'y', 'y', "final combiner" },
    { 0x0290, 0x0290, '-', '-', "CONTROL0: stencil write enable, z format (float), z perspective" },
    { 0x0294, 0x0294, '-', '-', "FFP specular / local eye" },
    { 0x0298, 0x0298, 'p', 'p', "FFP lighting: ambient/diffuse sources only" },
    { 0x029C, 0x029C, 'y', 'y', "" },
    { 0x02A0, 0x02A0, 'y', 'y', "fog coordinate source (FFP; TK-42)" },
    { 0x02A4, 0x02A8, 'y', 'y', "" },
    { 0x02B4, 0x02B4, '-', 'y', "" },
    { 0x02C0, 0x02FC, '-', 'p', "rect 0 as the scissor; rects 1-7 ignored" },
    { 0x0300, 0x0300, 'y', 'y', "" },
    { 0x0304, 0x0304, 'y', 'y', "" },
    { 0x0308, 0x0308, '-', 'y', "CPU draws both faces" },
    { 0x030C, 0x030C, 'y', 'y', "" },
    { 0x0310, 0x0310, '-', '-', "dither" },
    { 0x0314, 0x0314, 'p', 'p', "FFP only" },
    { 0x0318, 0x031C, 'y', 'y', "point params, point smooth = sprites (TK-40)" },
    { 0x0320, 0x0324, '-', '-', "line/poly smooth" },
    { 0x0328, 0x0328, '-', '-', "skinning (FFP)" },
    { 0x032C, 0x032C, '-', 'y', "no stencil on the CPU" },
    { 0x0330, 0x0338, 'p', 'p', "polygon offset: fill enable applied (TK-41)" },
    { 0x033C, 0x0340, 'y', 'y', "" },
    { 0x0344, 0x0350, 'y', 'y', "blend factors, colour, equation" },
    { 0x0354, 0x0354, 'y', 'y', "" },
    { 0x0358, 0x0358, 'p', 'y', "CPU: all channels or none" },
    { 0x035C, 0x035C, 'y', 'y', "" },
    { 0x0360, 0x0378, '-', 'y', "stencil" },
    { 0x037C, 0x037C, '-', '-', "shade model (flat/smooth)" },
    { 0x0380, 0x0380, '-', '-', "line width" },
    { 0x0384, 0x0388, 'y', 'y', "polygon offset factor/bias (TK-41)" },
    { 0x038C, 0x0390, '-', '-', "polygon fill modes" },
    { 0x0394, 0x0398, 'y', 'y', "depth range" },
    { 0x039C, 0x03A0, '-', 'y', "cull face / front face" },
    { 0x03A4, 0x03A4, '-', '-', "normalize (FFP)" },
    { 0x03A8, 0x03B0, '-', '-', "material emission" },
    { 0x03B4, 0x03B4, 'p', 'p', "material alpha (FFP)" },
    { 0x03B8, 0x03B8, '-', '-', "specular enable" },
    { 0x03BC, 0x03BC, 'p', 'p', "lights (FFP)" },
    { 0x03C0, 0x03FC, '-', '-', "texgen" },
    { 0x0420, 0x042C, '-', '-', "texture matrix enable" },
    { 0x043C, 0x043C, 'y', 'y', "point size (TK-40)" },
    { 0x0440, 0x047C, '-', '-', "projection matrix (FFP)" },
    { 0x0480, 0x04BC, 'p', 'p', "model-view 0 (FFP lighting)" },
    { 0x04C0, 0x067C, '-', '-', "model-view 1-3, inverse model-view" },
    { 0x0680, 0x06BC, 'y', 'y', "composite matrix (FFP)" },
    { 0x06C0, 0x07BC, '-', '-', "texture matrices" },
    { 0x0840, 0x093C, '-', '-', "texgen planes" },
    { 0x09C0, 0x09C4, 'y', 'y', "fog params 0-1" },
    { 0x09C8, 0x09C8, '-', '-', "fog param 2" },
    { 0x09D0, 0x09DC, 'y', 'y', "fog plane = c[57]" },
    { 0x09E0, 0x09F4, '-', '-', "specular params" },
    { 0x09F8, 0x09F8, '-', '-', "provoking vertex" },
    { 0x0A10, 0x0A18, 'p', 'p', "scene ambient (FFP)" },
    { 0x0A20, 0x0A2C, 'y', 'y', "viewport offset = c[59]" },
    { 0x0A30, 0x0A4C, 'p', 'p', "point params (fixed-function attenuation; TK-40)" },
    { 0x0A50, 0x0A5C, 'y', 'y', "eye position = c[56]" },
    { 0x0A60, 0x0ADC, 'y', 'y', "combiner factors, alpha OCW, colour ICW" },
    { 0x0AE0, 0x0AEC, '-', '-', "colour key colours" },
    { 0x0AF0, 0x0AFC, 'y', 'y', "viewport scale = c[58]" },
    { 0x0B00, 0x0BFC, 'y', 'y', "vertex program, constants" },
    { 0x0C00, 0x0FFC, '-', '-', "back light colours (FFP)" },
    { 0x1000, 0x13FC, 'p', 'p', "lights (FFP: no specular, no spot)" },
    { 0x1500, 0x1528, 'y', 'y', "immediate position" },
    { 0x1530, 0x16FC, '-', '-', "immediate normal/colour/texcoord methods" },
    { 0x1720, 0x179C, 'y', 'y', "vertex arrays" },
    { 0x17A0, 0x17B8, '-', '-', "back specular / material" },
    { 0x17BC, 0x17C0, '-', '-', "logic op" },
    { 0x17C4, 0x17C4, '-', '-', "two-sided lighting" },
    { 0x17C8, 0x17D0, 'y', 'y', "visibility reports" },
    { 0x17F8, 0x17F8, 'y', 'y', "texture clip planes" },
    { 0x17FC, 0x17FC, 'p', 'p', "lines are dropped (points: sprites, TK-40)" },
    { 0x1800, 0x1818, 'y', 'y', "indices, DRAW_ARRAYS, INLINE_ARRAY" },
    { 0x1880, 0x1A3C, 'y', 'y', "immediate vertex data" },
    { 0x1D60, 0x1D74, 'n', 'n', "semaphores" },
    { 0x1D78, 0x1D78, '-', 'p', "depth cull/clamp (BW_GPU_ZMODE)" },
    { 0x1D7C, 0x1D7C, '-', '-', "anti-aliasing control (sample mask)" },
    { 0x1D80, 0x1D88, 'n', 'n', "z compression / occlusion" },
    { 0x1D8C, 0x1D90, 'y', 'y', "clear values" },
    { 0x1D94, 0x1D94, 'p', 'p', "clear: GPU clears all colour channels, whole target" },
    { 0x1D98, 0x1D9C, 'p', '-', "clear rectangle" },
    { 0x1E20, 0x1E24, 'y', 'y', "final combiner constants" },
    { 0x1E28, 0x1E3C, '-', '-', "back specular params" },
    { 0x1E40, 0x1E60, 'y', 'y', "colour OCW, combiner control" },
    { 0x1E68, 0x1E6C, '-', '-', "shadow z slope / depth func" },
    { 0x1E70, 0x1E78, 'y', 'y', "stage program, dot mapping, other stage input" },
    { 0x1E94, 0x1EA4, 'y', 'y', "transform mode, program/constant load" },
};

static const AudClass *method_class(uint32_t m)
{
    size_t i;
    if (m >= 0x1B00 && m < 0x1C00) {
        /* texture stages: by register within the stage */
        static const AudClass tex[16] = {
            { 0, 0, 'y', 'y', "offset" },
            { 0, 0, 'p', 'p', "format: 2D and cube; 3D (dimensionality 3) not modelled" },
            { 0, 0, 'p', 'p', "address: U and V; P (r) and cylindrical wrap ignored" },
            { 0, 0, '-', '-', "CONTROL0: enable, LOD clamps, anisotropy, alpha kill, colour key" },
            { 0, 0, 'y', 'y', "CONTROL1: pitch" },
            { 0, 0, 'p', 'p', "filter: min/mag/mip, convolution (TK-51); LOD bias, signed channels ignored" },
            { 0, 0, '?', '?', "" },
            { 0, 0, 'y', 'y', "image rect" },
            { 0, 0, 'p', 'p', "palette (I8 only)" },
            { 0, 0, 'y', 'y', "border colour" },
            { 0, 0, '-', '-', "bump-env matrix" }, { 0, 0, '-', '-', "bump-env matrix" },
            { 0, 0, '-', '-', "bump-env matrix" }, { 0, 0, '-', '-', "bump-env matrix" },
            { 0, 0, '-', '-', "bump-env luminance scale" },
            { 0, 0, '-', '-', "bump-env luminance offset" },
        };
        return &tex[((m - 0x1B00) & 0x3F) / 4];
    }
    for (i = 0; i < sizeof s_class / sizeof s_class[0]; i++)
        if (m >= s_class[i].lo && m <= s_class[i].hi)
            return &s_class[i];
    return NULL;
}

static const char *method_name(uint32_t m)
{
    switch (m) {
    case 0x1E74: return "SET_DOT_RGBMAPPING";
    case 0x17C4: return "SET_TWO_SIDE_LIGHT_EN";
    case 0x0380: return "SET_LINE_WIDTH";
    default: break;
    }
    return s_nv097_names[(m & 0x1FFC) / 4] ? s_nv097_names[(m & 0x1FFC) / 4] : "?";
}

/* ---- feature counts ------------------------------------------------------ */

enum {
    F_PATH, F_SOURCE, F_PRIM, F_XFMODE, F_COMBINERS,
    F_SURF_FMT, F_SURF_SIZE,
    F_BLEND, F_BLEND_EQ, F_BLEND_COLOR, F_LOGIC_OP, F_COLOR_MASK, F_DITHER,
    F_ALPHA_TEST, F_DEPTH, F_DEPTH_WRITE, F_ZMINMAX, F_CONTROL0, F_CLIP_RANGE,
    F_STENCIL, F_STENCIL_OPS, F_STENCIL_MASKS,
    F_POLY_OFFSET, F_POLY_OFFSET_VAL, F_POLY_MODE, F_CULL, F_SHADE, F_SMOOTH,
    F_POINT_SIZE, F_POINT_PARAMS,
    F_LIGHTING, F_LIGHT_MASK, F_SPECULAR, F_LIGHT_CTRL, F_COLOR_MAT, F_TWO_SIDE, F_NORMALIZE,
    F_SKIN, F_FOG, F_FOG_GEN, F_FOG_COLOR, F_FOG_PARAMS,
    F_TEXGEN, F_TEXMATRIX, F_WINDOW_CLIP, F_WINDOW_RECTS, F_CLIP_PLANE_MODE, F_AA_CONTROL,
    F_STAGE_PROGRAM, F_STAGE_MODE, F_DOT_MAPPING, F_OTHER_STAGE,
    F_RC_CONTROL, F_RC_IN, F_RC_OUT, F_RC_AOUT, F_FC_IN, F_FC_FLAGS,
    F_TEX_FMT, F_TEX_DIM, F_TEX_LEVELS, F_TEX_ADDR, F_TEX_FILTER, F_TEX_LODBIAS, F_TEX_KERNEL,
    F_TEX_SIGNED, F_TEX_ENABLE, F_TEX_ALPHAKILL, F_TEX_COLORKEY, F_TEX_ANISO, F_TEX_LODCLAMP,
    F_TEX_BORDER, F_TEX_RTT, F_RTT_SAMPLE, F_TEX_BUMPENV, F_TEX_SIZE,
    F_VSH_LEN, F_VSH_MAC, F_VSH_ILU, F_VSH_OUT, F_VSH_CWRITE, F_VSH_A0, F_VATTR, F_FFP,
    F_CLEAR, F_CLEAR_RECT, F_FLIP_FMT, F_GAMMA,
    F_N
};

static const char *const s_fname[F_N] = {
    "path (G vsh on GPU, C vsh on CPU, F fixed-function, H fixed-function on GPU, S screen space, U dropped, ? none)",
    "vertex source (A arrays, I inline, M immediate)",
    "primitive (1 points 2 lines 3 loop 4 lstrip 5 tris 6 tstrip 7 fan 8 quads 9 qstrip 10 polygon)",
    "transform execution mode (0x1E94 & 3: 0 fixed, 2 program)",
    "combiners (1: COMBINER_CONTROL seen, the extended path)",
    "surface format (SET_SURFACE_FORMAT)",
    "surface clip w<<16|h",
    "blend sfactor<<16|dfactor (FFFFFFFF off)",
    "blend equation (when on)",
    "blend colour (constant factors only)",
    "logic op (FFFFFFFF off)",
    "colour mask",
    "dither enable",
    "alpha test func<<8|ref (FFFFFFFF off)",
    "depth test func (FFFFFFFF off)",
    "depth write",
    "ZMIN_MAX_CONTROL",
    "CONTROL0 (stencil write en, z format, z perspective)",
    "clip range (min==0 && max==FFFFFF ? 0 : 1)",
    "stencil func<<8|ref (FFFFFFFF off)",
    "stencil fail<<16^zfail<<8^zpass (when on)",
    "stencil write mask<<8|func mask (when on)",
    "polygon offset enables (point|line<<1|fill<<2)",
    "polygon offset factor/bias bits (when on)",
    "polygon mode front<<16|back",
    "cull face<<16|front face (FFFFFFFF off)",
    "shade model",
    "smooth enables (point|line<<1|poly<<2)",
    "point size",
    "point params enable",
    "lighting enable",
    "light enable mask (lighting on)",
    "specular enable (lighting on)",
    "light control (lighting on)",
    "colour material (lighting on)",
    "two-sided lighting",
    "normalize (lighting on)",
    "skin mode",
    "fog mode (FFFFFFFF off)",
    "fog gen mode (fog on)",
    "fog colour (fog on)",
    "fog params[2] bits (fog on)",
    "texgen stage<<28|coord<<24|mode (non-zero)",
    "texture matrix enable stage<<28|value (non-zero)",
    "window clip type<<16 | rect0 set",
    "window clip rects 1-7 set (count)",
    "texture clip plane mode",
    "anti-aliasing control",
    "shader stage program",
    "stage mode stage<<8|mode",
    "dot RGB mapping (dot programs)",
    "other stage input (dot programs)",
    "combiner control: count | msb<<8 | uc0<<9 | uc1<<10",
    "combiner input: alpha<<16|arep<<12|map<<8|reg",
    "combiner colour output flags (ocw>>12 & 0xFF: 1 cd.dot 2 ab.dot 4 mux 38 map 40 cd>a 80 ab>a)",
    "combiner alpha output flags",
    "final combiner input: letter<<16|arep<<12|map<<8|reg",
    "final combiner flags (80 clamp 40 inv v1 20 inv r0)",
    "texture colour format (stage modes != 0)",
    "texture dimensionality<<4|cube",
    "texture mip levels",
    "texture address (raw)",
    "texture filter min<<8|mag",
    "texture LOD bias (raw 13 bits, non-zero)",
    "texture convolution kernel (min 7 or mag 4: CONVOLUTION_2D)",
    "texture signed channels (filter >> 28, non-zero)",
    "texture CONTROL0 enable",
    "texture alpha kill",
    "texture colour key op (non-zero)",
    "texture max anisotropy (log2, non-zero)",
    "texture LOD clamps min<<16|max (raw 12-bit)",
    "texture border source colour (1) / texture (0), BORDER-addressed",
    "texture is a surface rendered earlier (render to texture)",
    "render-to-texture sample: stage<<28|kernel<<24|min<<16|mag<<12|target AA<<8|src fmt",
    "texture bump-env matrix non-zero (bump modes)",
    "texture size w<<16|h",
    "vertex program length",
    "vertex program MAC op",
    "vertex program ILU op",
    "vertex program output written (0 pos 3 d0 4 d1 5 fog 6 pts 7 b0 8 b1 9-12 t0-t3)",
    "vertex program writes constants",
    "vertex program uses a0 (1 arl, 2 relative read)",
    "vertex attribute attr<<16|type<<8|size",
    "fixed-function draw: lighting<<28|fog on<<27|fog gen<<24|attribute mask (bit i: v<i> has an array)",
    "clear flags",
    "clear rect (0 none set, 1 full, 2 partial)",
    "flip surface format",
    "gamma ramp (0 identity, 1 other; value = max |ramp - identity|)",
};

typedef struct { uint32_t feat, val; uint64_t draws, verts; } AudCnt;
#define AUD_SLOTS 32768
static AudCnt s_c[AUD_SLOTS];
static uint32_t s_nc;
static uint64_t s_draws, s_verts, s_flips, s_clears, s_dropped;

static void add(uint32_t feat, uint32_t val, uint32_t verts)
{
    uint32_t h = (feat * 0x9E3779B1u) ^ (val * 0x85EBCA77u), i;
    h ^= h >> 15;
    for (i = 0; i < AUD_SLOTS; i++) {
        AudCnt *c = &s_c[(h + i) & (AUD_SLOTS - 1)];
        if (c->draws && c->feat == feat && c->val == val) {
            c->draws++;
            c->verts += verts;
            return;
        }
        if (!c->draws) {
            if (s_nc >= AUD_SLOTS * 3 / 4) {
                s_dropped++;
                return;
            }
            c->feat = feat;
            c->val = val;
            c->draws = 1;
            c->verts = verts;
            s_nc++;
            return;
        }
    }
}

/* ---- per-draw decode ----------------------------------------------------- */

#define R(m) (d->reg[(m) / 4])

static uint32_t s_rt[32];           /* colour surfaces drawn into, newest first */

static void note_rt(uint32_t off)
{
    int i;
    if (s_rt[0] == off)
        return;
    for (i = 0; i < 31 && s_rt[i] != off; i++)
        ;
    memmove(&s_rt[1], &s_rt[0], (size_t)i * sizeof s_rt[0]);
    s_rt[0] = off;
}

static int is_rt(uint32_t off)
{
    int i;
    for (i = 0; i < 32; i++)
        if (s_rt[i] == off && off)
            return 1;
    return 0;
}

/* Vertex-program summary, cached for the program last seen at a start slot
 * (reset whenever program memory is written). */
typedef struct {
    uint32_t gen, start, len;
    uint32_t mac, ilu, out, cwrite, a0;     /* bit sets */
} AudProg;
static AudProg s_pc[16];

static const AudProg *prog_summary(const BwAuditDraw *d)
{
    AudProg *p = &s_pc[d->prog_start & 15];
    uint32_t i;
    if (p->gen == s_prog_gen + 1 && p->start == d->prog_start && p->len == d->prog_len)
        return p;
    memset(p, 0, sizeof *p);
    p->gen = s_prog_gen + 1;
    p->start = d->prog_start;
    p->len = d->prog_len;
    for (i = 0; i < d->prog_len; i++) {
        const uint32_t *w = d->prog[i];
        uint32_t mac = (w[1] >> 21) & 0xF, ilu = (w[1] >> 25) & 7;
        uint32_t omask = (w[3] >> 12) & 0xF, orb = (w[3] >> 11) & 1, oaddr = (w[3] >> 3) & 0xFF;
        if (mac) p->mac |= 1u << mac;
        if (ilu) p->ilu |= 1u << ilu;
        if (mac == 13) p->a0 |= 1;
        if ((w[3] >> 1) & 1) p->a0 |= 2;            /* A0X: c[a0.x + n] */
        if (omask) {
            if (orb)
                p->out |= 1u << (oaddr & 31);
            else
                p->cwrite = 1;
        }
    }
    return p;
}

static void decode_rc_input(uint32_t feat, uint32_t byte, uint32_t alpha, uint32_t letter,
                            uint32_t verts)
{
    uint32_t reg = byte & 0xF, arep = (byte >> 4) & 1, map = (byte >> 5) & 7;
    if (feat == F_FC_IN)
        add(F_FC_IN, (letter << 16) | (arep << 12) | (map << 8) | reg, verts);
    else
        add(F_RC_IN, (alpha << 16) | (arep << 12) | (map << 8) | reg, verts);
}

void bw_audit_draw(const BwAuditDraw *d)
{
    uint32_t v = d->count, st, i;
    uint32_t prog = R(0x1E70);
    int rc = R(0x1E60) != 0;

    s_draws++;
    s_verts += v;
    note_rt(d->color_offset);
    add(F_PATH, (uint32_t)(unsigned char)d->path, v);
    add(F_SOURCE, (uint32_t)(unsigned char)d->source, v);
    add(F_PRIM, d->prim, v);
    add(F_XFMODE, R(0x1E94) & 3, v);
    add(F_COMBINERS, rc, v);
    add(F_SURF_FMT, R(0x0208), v);
    add(F_SURF_SIZE, ((R(0x0200) >> 16) << 16) | (R(0x0204) >> 16), v);

    add(F_BLEND, R(0x0304) ? (R(0x0344) << 16) | (R(0x0348) & 0xFFFF) : 0xFFFFFFFFu, v);
    if (R(0x0304)) {
        add(F_BLEND_EQ, R(0x0350), v);
        if ((R(0x0344) >= 0x8001 && R(0x0344) <= 0x8004) || (R(0x0348) >= 0x8001 && R(0x0348) <= 0x8004))
            add(F_BLEND_COLOR, R(0x034C), v);
    }
    add(F_LOGIC_OP, R(0x17BC) ? R(0x17C0) : 0xFFFFFFFFu, v);
    add(F_COLOR_MASK, R(0x0358), v);
    add(F_DITHER, R(0x0310), v);
    add(F_ALPHA_TEST, R(0x0300) ? (R(0x033C) << 8) | (R(0x0340) & 0xFF) : 0xFFFFFFFFu, v);
    add(F_DEPTH, R(0x030C) ? R(0x0354) : 0xFFFFFFFFu, v);
    add(F_DEPTH_WRITE, R(0x035C), v);
    add(F_ZMINMAX, R(0x1D78), v);
    add(F_CONTROL0, R(0x0290), v);
    add(F_CLIP_RANGE, (R(0x0394) == 0 && R(0x0398) == 0x4B7FFFFFu) ? 0 : R(0x0398), v);
    if (R(0x032C)) {
        add(F_STENCIL, (R(0x0364) << 8) | (R(0x0368) & 0xFF), v);
        add(F_STENCIL_OPS, (R(0x0370) << 16) ^ (R(0x0374) << 8) ^ R(0x0378), v);
        add(F_STENCIL_MASKS, ((R(0x0360) & 0xFF) << 8) | (R(0x036C) & 0xFF), v);
    } else {
        add(F_STENCIL, 0xFFFFFFFFu, v);
    }
    {
        uint32_t po = (R(0x0330) ? 1 : 0) | (R(0x0334) ? 2 : 0) | (R(0x0338) ? 4 : 0);
        add(F_POLY_OFFSET, po, v);
        if (po) {
            add(F_POLY_OFFSET_VAL, R(0x0384), v);
            add(F_POLY_OFFSET_VAL, R(0x0388) ^ 0x80000000u, v);
        }
    }
    add(F_POLY_MODE, (R(0x038C) << 16) | (R(0x0390) & 0xFFFF), v);
    add(F_CULL, R(0x0308) ? (R(0x039C) << 16) | (R(0x03A0) & 0xFFFF) : 0xFFFFFFFFu, v);
    add(F_SHADE, R(0x037C), v);
    add(F_SMOOTH, (R(0x031C) ? 1 : 0) | (R(0x0320) ? 2 : 0) | (R(0x0324) ? 4 : 0), v);
    if (d->prim <= 4) {
        add(F_POINT_SIZE, R(0x043C), v);
        add(F_POINT_PARAMS, R(0x0318), v);
    }
    add(F_LIGHTING, R(0x0314), v);
    if (R(0x0314)) {
        add(F_LIGHT_MASK, R(0x03BC), v);
        add(F_SPECULAR, R(0x03B8), v);
        add(F_LIGHT_CTRL, R(0x0294), v);
        add(F_COLOR_MAT, R(0x0298), v);
        add(F_NORMALIZE, R(0x03A4), v);
    }
    add(F_TWO_SIDE, R(0x17C4), v);
    add(F_SKIN, R(0x0328), v);
    add(F_FOG, R(0x02A4) ? R(0x029C) : 0xFFFFFFFFu, v);
    if (R(0x02A4)) {
        add(F_FOG_GEN, R(0x02A0), v);
        add(F_FOG_COLOR, R(0x02A8), v);
        add(F_FOG_PARAMS, R(0x09C8), v);
    }
    for (st = 0; st < 4; st++) {
        for (i = 0; i < 4; i++) {
            uint32_t g = R(0x03C0 + st * 16 + i * 4);
            if (g)
                add(F_TEXGEN, (st << 28) | (i << 24) | (g & 0xFFFFFF), v);
        }
        if (R(0x0420 + st * 4))
            add(F_TEXMATRIX, (st << 28) | (R(0x0420 + st * 4) & 0xFFFFFFF), v);
    }
    {
        uint32_t n = 0;
        add(F_WINDOW_CLIP, (R(0x02B4) << 16) | ((R(0x02C0) || R(0x02E0)) ? 1 : 0), v);
        for (i = 1; i < 8; i++)
            n += (R(0x02C0 + i * 4) || R(0x02E0 + i * 4)) ? 1 : 0;
        add(F_WINDOW_RECTS, n, v);
    }
    add(F_CLIP_PLANE_MODE, R(0x17F8), v);
    add(F_AA_CONTROL, R(0x1D7C), v);

    /* The pixel side. */
    if (rc) {
        uint32_t ctl = R(0x1E60), n = ctl & 0xFF, s;
        int dot = 0;
        add(F_STAGE_PROGRAM, prog, v);
        for (st = 0; st < 4; st++) {
            uint32_t m = (prog >> (st * 5)) & 0x1F;
            add(F_STAGE_MODE, (st << 8) | m, v);
            if (m >= 6)
                dot = 1;
        }
        if (dot) {
            add(F_DOT_MAPPING, R(0x1E74), v);
            add(F_OTHER_STAGE, R(0x1E78), v);
        }
        add(F_RC_CONTROL, n | (((ctl >> 8) & 1) << 8) | (((ctl >> 12) & 1) << 9)
                          | (((ctl >> 16) & 1) << 10), v);
        if (n > 8)
            n = 8;
        for (s = 0; s < n; s++) {
            uint32_t cicw = R(0x0AC0 + s * 4), aicw = R(0x0260 + s * 4);
            uint32_t cocw = R(0x1E40 + s * 4), aocw = R(0x0AA0 + s * 4);
            for (i = 0; i < 4; i++) {
                decode_rc_input(F_RC_IN, cicw >> (24 - 8 * i), 0, i, v);
                decode_rc_input(F_RC_IN, aicw >> (24 - 8 * i), 1, i, v);
            }
            add(F_RC_OUT, (cocw >> 12) & 0xFF, v);
            add(F_RC_AOUT, (aocw >> 12) & 0xFF, v);
        }
        if (R(0x0288) || R(0x028C)) {
            uint32_t f0 = R(0x0288), f1 = R(0x028C);
            for (i = 0; i < 4; i++)
                decode_rc_input(F_FC_IN, f0 >> (24 - 8 * i), 0, 'A' + i, v);
            for (i = 0; i < 3; i++)
                decode_rc_input(F_FC_IN, f1 >> (24 - 8 * i), 0, 'E' + i, v);
            add(F_FC_FLAGS, f1 & 0xFF, v);
        } else {
            add(F_FC_FLAGS, 0xFFFFFFFFu, v);
        }
    }

    /* Textures: every stage the stage program uses (stage 0 for paths
     * without the combiners, when it has an image). */
    for (st = 0; st < 4; st++) {
        uint32_t m = rc ? (prog >> (st * 5)) & 0x1F : (st == 0 && R(0x1B00));
        uint32_t b = 0x1B00 + st * 0x40, fmt, addr, c0, flt;
        if (!m || m == 4 || m == 5 || m == 0x11)   /* passthru, clip plane, dot: no texture read */
            continue;
        fmt = R(b + 0x04); addr = R(b + 0x08); c0 = R(b + 0x0C); flt = R(b + 0x14);
        add(F_TEX_FMT, (fmt >> 8) & 0xFF, v);
        add(F_TEX_DIM, (((fmt >> 4) & 0xF) << 4) | ((fmt >> 2) & 1), v);
        add(F_TEX_LEVELS, (fmt >> 16) & 0xF, v);
        add(F_TEX_ADDR, addr, v);
        add(F_TEX_FILTER, (((flt >> 16) & 0xFF) << 8) | ((flt >> 24) & 0xF), v);
        if (flt & 0x1FFF) add(F_TEX_LODBIAS, flt & 0x1FFF, v);
        if (((flt >> 16) & 0xFF) == 7 || ((flt >> 24) & 0xF) == 4)   /* the kernel only acts here */
            add(F_TEX_KERNEL, (flt >> 13) & 7, v);
        if (flt >> 28) add(F_TEX_SIGNED, flt >> 28, v);
        add(F_TEX_ENABLE, (c0 >> 30) & 1, v);
        add(F_TEX_ALPHAKILL, (c0 >> 2) & 1, v);
        if (c0 & 3) add(F_TEX_COLORKEY, c0 & 3, v);
        if ((c0 >> 4) & 3) add(F_TEX_ANISO, (c0 >> 4) & 3, v);
        add(F_TEX_LODCLAMP, (((c0 >> 18) & 0xFFF) << 16) | ((c0 >> 6) & 0xFFF), v);
        if ((addr & 0xF) == 4 || ((addr >> 8) & 0xF) == 4 || ((addr >> 16) & 0xF) == 4)
            add(F_TEX_BORDER, (fmt >> 3) & 1, v);
        add(F_TEX_RTT, is_rt(R(b)), v);
        if (is_rt(R(b)))
            add(F_RTT_SAMPLE, (st << 28) | (((flt >> 13) & 7) << 24) | (((flt >> 16) & 0xFF) << 16)
                              | (((flt >> 24) & 0xF) << 12) | ((d->aa & 0xF) << 8) | ((fmt >> 8) & 0xFF), v);
        if (m >= 6 && m <= 8)
            add(F_TEX_BUMPENV, (R(b + 0x28) | R(b + 0x2C) | R(b + 0x30) | R(b + 0x34)) != 0, v);
        if ((fmt >> 20) & 0xF || (fmt >> 24) & 0xF)
            add(F_TEX_SIZE, ((1u << ((fmt >> 20) & 0xF)) << 16) | (1u << ((fmt >> 24) & 0xF)), v);
        else
            add(F_TEX_SIZE, R(b + 0x1C), v);
    }

    /* The vertex side. */
    if (d->prog && d->prog_len) {
        const AudProg *p = prog_summary(d);
        add(F_VSH_LEN, d->prog_len, v);
        for (i = 1; i < 16; i++)
            if (p->mac & (1u << i)) add(F_VSH_MAC, i, v);
        for (i = 1; i < 8; i++)
            if (p->ilu & (1u << i)) add(F_VSH_ILU, i, v);
        for (i = 0; i < 16; i++)
            if (p->out & (1u << i)) add(F_VSH_OUT, i, v);
        add(F_VSH_CWRITE, p->cwrite, v);
        add(F_VSH_A0, p->a0, v);
    }
    if ((d->path == 'F' || d->path == 'H') && d->vattr_fmt) {
        uint32_t mask = 0;
        for (i = 0; i < 16; i++)
            if ((d->vattr_fmt[i] >> 4) & 0xF)
                mask |= 1u << i;
        add(F_FFP, ((R(0x0314) ? 1u : 0u) << 28) | ((R(0x02A4) ? 1u : 0u) << 27)
                   | ((R(0x02A0) & 7) << 24) | mask, v);
    }
    if (d->vattr_fmt)
        for (i = 0; i < 16; i++) {
            uint32_t f = d->vattr_fmt[i];
            if ((f >> 4) & 0xF)
                add(F_VATTR, (i << 16) | ((f & 0xF) << 8) | ((f >> 4) & 0xF), v);
        }
}

void bw_audit_clear(uint32_t flags, uint32_t surf_w, uint32_t surf_h, const uint32_t *rect)
{
    s_clears++;
    add(F_CLEAR, flags, 0);
    if (!rect)
        add(F_CLEAR_RECT, 0, 0);
    else
        add(F_CLEAR_RECT, (rect[0] == 0 && rect[1] == 0 && rect[2] >= surf_w && rect[3] >= surf_h)
                          ? 1 : 2, 0);
}

/* ---- gamma (from the D3DDevice_SetGammaRamp hook, the game's thread) ----- */

static uint8_t s_gamma[768];
static volatile LONG s_gamma_seq, s_gamma_taken;

void bw_audit_gamma(const uint8_t ramp[768])
{
    memcpy(s_gamma, ramp, 768);
    InterlockedIncrement(&s_gamma_seq);
}

/* ---- report -------------------------------------------------------------- */

/* What the GPU back end does with a colour format (mirrors bw_d3d11.c
 * fast_texture and get_texture_): "fast" bulk upload with mips, "decoder"
 * the executor's per-texel decoder (level 0 only), "none" neither. */
static const char *gpu_fmt_path(uint32_t c)
{
    switch (c) {
    case 0x0C: case 0x0E: case 0x0F: return "fast (BC1-3, mips)";
    case 0x05: case 0x06: case 0x07: return "fast (unswizzle, mips)";
    case 0x12: case 0x1E: return "fast (linear copy, level 0)";
    default: break;
    }
    switch (c) {     /* sample_tex in nv2a_pb_exec.c, through linear_twin */
    case 0x00: case 0x02: case 0x03: case 0x04: case 0x0B: case 0x19:
    case 0x10: case 0x11: case 0x13: case 0x1C: case 0x1D: case 0x1F:
    case 0x24: case 0x25: case 0x3F: case 0x40: case 0x41:
        return "decoder (level 0 only, no mips)";
    default:
        return "NONE (not decoded: draws untextured)";
    }
}

static const char *fmt_name(uint32_t c)
{
    static const char *n[0x42] = {
        [0x00] = "SZ_Y8", [0x01] = "SZ_AY8", [0x02] = "SZ_A1R5G5B5", [0x03] = "SZ_X1R5G5B5",
        [0x04] = "SZ_A4R4G4B4", [0x05] = "SZ_R5G6B5", [0x06] = "SZ_A8R8G8B8",
        [0x07] = "SZ_X8R8G8B8", [0x0B] = "SZ_I8_A8R8G8B8", [0x0C] = "L_DXT1_A1R5G5B5",
        [0x0E] = "L_DXT23_A8R8G8B8", [0x0F] = "L_DXT45_A8R8G8B8", [0x10] = "LU_IMAGE_A1R5G5B5",
        [0x11] = "LU_IMAGE_R5G6B5", [0x12] = "LU_IMAGE_A8R8G8B8", [0x13] = "LU_IMAGE_Y8",
        [0x14] = "LU_IMAGE_SY8", [0x15] = "LU_IMAGE_X7SY8", [0x16] = "LU_IMAGE_R8B8",
        [0x17] = "LU_IMAGE_G8B8", [0x18] = "LU_IMAGE_SG8SB8", [0x19] = "SZ_A8",
        [0x1A] = "SZ_A8Y8", [0x1B] = "LU_IMAGE_AY8", [0x1C] = "LU_IMAGE_X1R5G5B5",
        [0x1D] = "LU_IMAGE_A4R4G4B4", [0x1E] = "LU_IMAGE_X8R8G8B8", [0x1F] = "LU_IMAGE_A8",
        [0x20] = "LU_IMAGE_A8Y8", [0x24] = "LC_IMAGE_CR8YB8CB8YA8", [0x25] = "LC_IMAGE_YB8CR8YA8CB8",
        [0x26] = "LU_IMAGE_A8CR8CB8Y8", [0x27] = "SZ_R6G5B5", [0x28] = "SZ_G8B8", [0x29] = "SZ_R8B8",
        [0x2A] = "SZ_DEPTH_X8_Y24_FIXED", [0x2B] = "SZ_DEPTH_X8_Y24_FLOAT", [0x2C] = "SZ_DEPTH_Y16_FIXED",
        [0x2D] = "SZ_DEPTH_Y16_FLOAT", [0x2E] = "LU_IMAGE_DEPTH_X8_Y24_FIXED",
        [0x2F] = "LU_IMAGE_DEPTH_X8_Y24_FLOAT", [0x30] = "LU_IMAGE_DEPTH_Y16_FIXED",
        [0x31] = "LU_IMAGE_DEPTH_Y16_FLOAT", [0x32] = "SZ_Y16", [0x35] = "LU_IMAGE_Y16",
        [0x3A] = "SZ_A8B8G8R8", [0x3B] = "SZ_B8G8R8A8", [0x3C] = "SZ_R8G8B8A8",
        [0x3F] = "LU_IMAGE_A8B8G8R8", [0x40] = "LU_IMAGE_B8G8R8A8", [0x41] = "LU_IMAGE_R8G8B8A8",
    };
    return c < 0x42 && n[c] ? n[c] : "?";
}

static const char *stage_mode_name(uint32_t m)
{
    static const char *n[] = {
        "none", "2D", "3D", "cube", "passthru", "clipplane", "bumpenv", "bumpenv_lum",
        "brdf", "dot_st", "dot_zw", "dot_rflct_diff", "dot_rflct_spec", "dot_str_3d",
        "dot_str_cube", "dpndnt_ar", "dpndnt_gb", "dotproduct", "dot_rflct_spec_const" };
    return m < 19 ? n[m] : "?";
}

/* Our renderers' status for one feature value; "" if nothing to say. */
static const char *status(uint32_t f, uint32_t v)
{
    switch (f) {
    case F_PATH:
        return v == 'U' ? "DROPPED" : v == '?' ? "not drawn (see primitive)" : "";
    case F_PRIM:
        return v == 1 ? "sprites (TK-40, BW_GPU_POINTS)" : v >= 2 && v <= 4 ? "DROPPED by both renderers" : "";
    case F_STAGE_MODE: {
        uint32_t m = v & 0xFF;
        if (m <= 1 || m == 3 || m == 4 || m == 5 || m == 11 || m == 12 || m == 0x11)
            return m == 3 ? "ok (cube faces when bound as a cube)" : "ok";
        return m == 2 ? "GAP: 3D textures sampled as 2D" : "GAP: reads 0";
    }
    case F_POLY_OFFSET:
        return v ? "applied (TK-41, BW_GPU_POLYOFFSET)" : "";
    case F_TEX_KERNEL:
        return "3x3 Gaussian as xemu (TK-51, BW_GPU_CONV)";
    case F_LOGIC_OP: case F_TEXGEN: case F_TEXMATRIX: case F_SKIN:
    case F_TWO_SIDE: case F_TEX_LODBIAS: case F_TEX_SIGNED:
    case F_TEX_ALPHAKILL: case F_TEX_COLORKEY: case F_TEX_ANISO:
        return (v == 0 || v == 0xFFFFFFFFu) ? "" : "GAP: ignored by both";
    case F_SHADE:
        return v == 0x1D00 ? "GAP: flat shading drawn smooth" : "";
    case F_POLY_MODE:
        return v == ((0x1B02u << 16) | 0x1B02u) ? "" : "GAP: only fill";
    case F_SPECULAR:
        return v ? "GAP: FFP specular ignored" : "";
    case F_DITHER:
        return v ? "ignored (no dithering on 32-bit targets?)" : "";
    case F_VSH_OUT:
        return v == 6 ? "ignored (points not drawn)" : (v == 7 || v == 8) ? "ignored (back colours)" : "";
    case F_WINDOW_RECTS:
        return v ? "GAP: only rect 0 applied" : "";
    case F_TEX_LODCLAMP:
        return ((v >> 16) == 0 && ((v & 0xFFF) == 0xFFF || (v & 0xFFF) == 0xFFC)) ? "" : "check: LOD clamps ignored";
    case F_CLEAR_RECT:
        return v == 2 ? "GAP on the GPU path: whole target cleared" : "";
    case F_CLEAR:
        return (v & 0xF0) && (v & 0xF0) != 0xF0 ? "GAP on the GPU path: channel mask ignored" : "";
    case F_GAMMA:
        return v ? "applied at present (TK-50, BW_GPU_GAMMA)" : "";
    case F_FC_IN:
        return ((v >> 8) & 7) > 1 ? "check: signed mapping in the final combiner" : "";
    default:
        return "";
    }
}

static int cmp_cnt(const void *a, const void *b)
{
    const AudCnt *x = *(const AudCnt *const *)a, *y = *(const AudCnt *const *)b;
    if (x->feat != y->feat)
        return x->feat < y->feat ? -1 : 1;
    if (x->draws != y->draws)
        return x->draws > y->draws ? -1 : 1;
    return x->val < y->val ? -1 : 1;
}

static ULONGLONG s_t0;

static void report(const char *why)
{
    static const char *path;
    static AudCnt *sorted[AUD_SLOTS];
    FILE *f;
    uint32_t i, n = 0, s, last = ~0u;
    if (!path) {
        path = getenv("BW_GPU_AUDIT_FILE");
        if (!path || !*path)
            path = "gpu_audit.txt";
    }
    f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "GPU audit (BW_GPU_AUDIT=1, %s): %.1f s, %llu flips, %llu draws, %llu vertices,"
               " %llu clears%s\n", why, (GetTickCount64() - s_t0) / 1000.0,
            (unsigned long long)s_flips, (unsigned long long)s_draws,
            (unsigned long long)s_verts, (unsigned long long)s_clears,
            s_dropped ? " (feature table full: some values dropped)" : "");
    fprintf(f, "\n== Methods, 3D class (subchannel 0): addr count cpu gpu name [values: count] note\n");
    for (i = 0; i < 2048; i++) {
        const AudMethod *a = &s_m[0][i];
        const AudClass *c;
        uint32_t k;
        if (!a->count)
            continue;
        c = method_class(i * 4);
        fprintf(f, "  %04X %10llu  %c %c  %-36s", i * 4, (unsigned long long)a->count,
                c ? c->cpu : '?', c ? c->gpu : '?', method_name(i * 4));
        for (k = 0; k < a->nval; k++)
            fprintf(f, " %X:%llu", a->val[k], (unsigned long long)a->vcount[k]);
        if (a->other)
            fprintf(f, " +%llu more", (unsigned long long)a->other);
        if (c && c->note && *c->note)
            fprintf(f, "   | %s", c->note);
        fputc('\n', f);
    }
    fprintf(f, "\n== Methods on other subchannels (the executor ignores them all, except the"
               " KickOff marker 5:0310)\n");
    for (s = 1; s < 8; s++)
        for (i = 0; i < 2048; i++) {
            const AudMethod *a = &s_m[s][i];
            uint32_t k;
            if (!a->count)
                continue;
            fprintf(f, "  %u:%04X %10llu ", s, i * 4, (unsigned long long)a->count);
            for (k = 0; k < a->nval; k++)
                fprintf(f, " %X:%llu", a->val[k], (unsigned long long)a->vcount[k]);
            if (a->other)
                fprintf(f, " +%llu more", (unsigned long long)a->other);
            fputc('\n', f);
        }
    for (i = 0; i < AUD_SLOTS; i++)
        if (s_c[i].draws)
            sorted[n++] = &s_c[i];
    qsort(sorted, n, sizeof sorted[0], cmp_cnt);
    fprintf(f, "\n== Features (per draw; clears and flips per event): value draws %%draws"
               " vertices [status]\n");
    for (i = 0; i < n; i++) {
        const AudCnt *c = sorted[i];
        const char *st = status(c->feat, c->val);
        if (c->feat != last) {
            fprintf(f, "-- %s\n", s_fname[c->feat]);
            last = c->feat;
        }
        fprintf(f, "  %8X %10llu %6.2f%% %12llu", c->val, (unsigned long long)c->draws,
                s_draws ? 100.0 * (double)c->draws / (double)s_draws : 0.0,
                (unsigned long long)c->verts);
        if (c->feat == F_TEX_FMT)
            fprintf(f, "  %s; GPU: %s", fmt_name(c->val), gpu_fmt_path(c->val));
        else if (c->feat == F_STAGE_MODE)
            fprintf(f, "  stage %u %s", c->val >> 8, stage_mode_name(c->val & 0xFF));
        if (*st)
            fprintf(f, "  [%s]", st);
        fputc('\n', f);
    }
    fclose(f);
}

static void at_exit(void)
{
    report("at exit");
}

void bw_audit_flip(uint32_t offset, uint32_t surface_format)
{
    static ULONGLONG last;
    static uint32_t every_ms;
    ULONGLONG now = GetTickCount64();
    (void)offset;
    if (!s_t0) {
        const char *e = getenv("BW_GPU_AUDIT_EVERY");
        s_t0 = now;
        last = now;
        every_ms = (e && atoi(e) > 0 ? (uint32_t)atoi(e) : 20) * 1000u;
        atexit(at_exit);
        fprintf(stderr, "[GPU-AUDIT] counting every method, draw, clear and flip"
                        " (BW_GPU_AUDIT=1); summary every %u s\n", every_ms / 1000);
    }
    s_flips++;
    add(F_FLIP_FMT, surface_format, 0);
    if (s_gamma_seq != s_gamma_taken) {
        uint32_t k, dev = 0;
        s_gamma_taken = s_gamma_seq;
        for (k = 0; k < 768; k++) {
            int dlt = (int)s_gamma[k] - (int)(k & 0xFF);
            if (dlt < 0) dlt = -dlt;
            if ((uint32_t)dlt > dev) dev = (uint32_t)dlt;
        }
        add(F_GAMMA, dev, 0);
    }
    if (now - last >= every_ms) {
        last = now;
        report("periodic");
    }
}
