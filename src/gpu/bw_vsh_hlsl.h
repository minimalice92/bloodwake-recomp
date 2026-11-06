/* bw_vsh_hlsl.h - HLSL vertex shaders from NV2A vertex programs.
 * See bw_vsh_hlsl.c. */
#ifndef BW_VSH_HLSL_H
#define BW_VSH_HLSL_H

#include <stddef.h>
#include <stdint.h>

/* The second constant buffer the generated shader reads (register b1).
 * K is the program's c[0..191]. */
typedef struct {
    float k[192][4];
    float vsinfo[4];    /* aa scale x, aa scale y, near-plane clip on, - */
    float foginfo[4];   /* fog enabled, fog mode (0x800..), param 0, param 1 */
} BwVshConsts;

/* Write the vertex shader (entry point vs_main) for the `n` instructions at
 * `prog` (the last one FINAL). The inputs in `input_mask` arrive in
 * ascending order as float4 attributes ATTR0, ATTR1, ... Returns the length,
 * or 0 if it did not fit. */
/* The geometry shader (entry point gs_main) that drops triangles with a
 * non-finite vertex, as the CPU path's clipper does. */
extern const char bw_vsh_gs_hlsl[];

/* conv (Blood Wake P1; NULL = all gathered floats): per input in that order,
 * its BwXVsInput.conv (bw_backend_ext.h). The integer formats then arrive
 * raw as ATTRk and the shader converts them as fetch_attr does. */
size_t bw_vsh_hlsl(const uint32_t (*prog)[4], uint32_t n, uint32_t input_mask,
                   const uint8_t *conv, char *out, size_t cap);

/* gfx-sprites (I6): the same shader for a point batch drawn on the GPU
 * (points 1): it also writes oPts (o[6]) and hands the sprite geometry
 * shader the point's centre in surface pixels and its size, as an extra
 * output after VSOut's (SPRITE0 = x, y, z as transform_vertex leaves them,
 * oPts.x). points 0 writes exactly bw_vsh_hlsl's text. */
size_t bw_vsh_hlsl_ex(const uint32_t (*prog)[4], uint32_t n, uint32_t input_mask,
                      const uint8_t *conv, int points, char *out, size_t cap);

/* gfx-sprites (I6): the geometry shader (entry point gs_main) that makes
 * each point's quad by bw_draw_points's rules (nv2a_pb_exec.c). It reads
 * b0 (BW_RC_CBUFFER: surf and, with BW_WS, wsx) and its own b1 (the core's
 * bwg_sprite_consts: BwgSpriteConsts). Defining BW_SPR_SO adds the stream
 * output's check element (SPRCHK: point, corner) for BW_GPU_SPRITES_VERIFY. */
extern const char bw_vsh_sprite_gs_hlsl[];

/* gfx-ffp (BW_GPU_FFP): the fixed function as a generated vertex shader.
 * Everything that shapes its text; the values it reads are constants: the
 * program shaders' cbuffer V (b1; it reads K[57], K[59], vsinfo, foginfo)
 * and BwFfpConsts (b2). */
typedef struct {
    int8_t in[8];               /* BW_FFP_* roles (bw_backend_ext.h): input k (ATTRk), -1 none */
    uint8_t conv[16];           /* per input, as bw_vsh_hlsl's conv (P1); 0 = floats */
    uint32_t nin;               /* inputs */
    uint32_t light_mask;        /* lit: 2 bits a light (1 infinite, 2-3 local); 0 when unlit */
    uint8_t lit, colmat, fog_on, fog_gen;
} BwFfpKey;

/* The fixed-function shader's own constants (register b2). */
typedef struct {
    float m[4][4];              /* composite rows: clip[i] = dot(m[i], position) */
    float mv[4][4];             /* model-view rows */
    float ambient[4];           /* scene ambient rgb, material alpha */
    float imm_fog[4];           /* v4 / v5 without an array */
    float light[8][5][4];       /* ambient rgb + range; diffuse rgb; direction; position; attenuation */
} BwFfpConsts;

/* Write the fixed-function vertex shader (entry point vs_main) for `k`.
 * Returns the length, or 0 if it did not fit. */
size_t bw_ffp_hlsl(const BwFfpKey *k, char *out, size_t cap);

#endif
