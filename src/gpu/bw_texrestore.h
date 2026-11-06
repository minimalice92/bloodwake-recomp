/* bw_texrestore.h - Remastered texture restoration (agent T1, VFX_TEX_RESTORE;
 * docs/day1/T1.md, design in docs/agents/BRIEF_T1T2.md).
 *
 * For a DXT1/3/5 texture drawn by 3D (non-overlay) draws, a second texture
 * whose mip chain is
 *   level 0      the game's level 0 enlarged k times (k = 4, or 2),
 *   level 1      (k = 4 only) the same enlarged 2 times,
 *   level lk...  the game's own level 0 and its own mips, decoded and
 *                deblocked ("Plan A": small steps across the 4x4 block edges
 *                between two flat sides are smoothed; real edges stay),
 * so anything drawn at or below its painted size samples the shipped detail
 * (no new shimmer, by construction) and the enlarged copies are only sampled
 * where the texture is drawn larger than painted. The enlargement is
 * Lanczos-2 (4x4 texels) held within the 2x2 texels around each point (no
 * ringing), no sharpening; cut-out alpha stays hard, smooth alpha is
 * enlarged the same way. Done once per texture, at its first 3D draw, on the
 * back end's device thread. Guest memory is never touched.
 *
 * Not restored: normal maps (pixel statistics), the water's textures and the
 * effect sprites and atlas (by content key), cube maps, 32-bit and linear
 * textures, render targets, textures smaller than 16 texels across, and
 * anything an overlay (HUD, menu) draw samples: those keep the original.
 *
 * This header names no graphics API (the shared core, src/gpu/core, keeps a
 * BwTrSlot in each texture record; GC-3): the slot holds its chain's back-end
 * objects untyped, and the D3D11 side (bw_texrestore_d3d11.h) names them. */
#ifndef BW_TEXRESTORE_H
#define BW_TEXRESTORE_H

#define COBJMACROS
#include <windows.h>
#include <stdint.h>
#include <stddef.h>

/* Per cached texture. Zeroed = not tried yet. */
typedef struct BwTrSlot {
    void *api[2];               /* the restored chain's back-end objects (D3D11: the
                                 * texture and its view, BW_TR_TEX and BW_TR_SRV) */
    uint32_t bytes;             /* VRAM of tex (estimated from its levels) */
    uint32_t ticket;            /* the request being worked on (state 3) */
    uint8_t state;              /* 0 untried, 1 restored, 2 left as shipped, 3 being made */
    uint8_t why;                /* BW_TR_WHY_* when state 2 */
    uint8_t lk;                 /* restored: number of enlarged levels (1 for 2x, 2 for 4x) */
    uint8_t alpha;              /* restored: 0 opaque, 1 hard (cut-out), 2 smooth */
    uint8_t au, av;             /* the edge modes it was made with (1 wrap, 2 mirror, 0 clamp) */
    uint8_t mixed;              /* item 48: also drawn with other edge modes */
    struct BwTrSlot *alt;       /* item 48: the copy for the second edge modes (a pool slot), or NULL */
} BwTrSlot;

enum {
    BW_TR_WHY_NONE = 0,
    BW_TR_WHY_NORMAL,           /* a tangent-space normal map */
    BW_TR_WHY_EXCLUDED,         /* water or effect texture (content key) */
    BW_TR_WHY_SMALL,            /* under 16 texels across, or odd size */
    BW_TR_WHY_BUDGET,           /* the VRAM budget is used up */
    BW_TR_WHY_CHURN,            /* its contents keep changing */
    BW_TR_WHY_FAIL,             /* a D3D call failed */
    BW_TR_WHY_N
};

/* Compile the enlargement shader (device setup, Remastered with the switch on). */
void bw_tr_init(void);

/* Ask for a DXT texture's restored chain (color 0x0C/0x0E/0x0F, w x h,
 * `levels` levels in guest memory at `src`, each level's blocks after the
 * previous). addr_u/addr_v: the NV097 address modes of the draw that asked
 * (1 wrap, 2 mirror, else clamp): how the enlargement treats the edges.
 * addr: the guest address, for the log. Copies the blocks now and queues the
 * work: `s` becomes state 3 (draw the original meanwhile), or 2. Device
 * thread. */
void bw_tr_request(BwTrSlot *s, uint32_t color, const uint8_t *src, uint32_t w, uint32_t h,
                   uint32_t levels, uint32_t addr_u, uint32_t addr_v, uint32_t addr);
void bw_tr_release(BwTrSlot *s);

/* 0.2.5 item 48: the slot for a 3D draw of this texture with these NV097
 * address modes. The enlargement treats the outer texels as the edge mode it
 * was made for, so a texture also drawn with other modes gets a second copy
 * for them (s->alt, untried at first: the caller asks for it with those
 * modes). A third combination uses the first copy. The first such draw of
 * each texture is counted and logged. Returns s itself when the modes match
 * or s isn't restored (or being made). Device thread. */
BwTrSlot *bw_tr_slot_for(BwTrSlot *s, uint32_t addr_u, uint32_t addr_v, uint32_t addr);

/* A [TEXRESTORE] summary line now and then (call once per frame). */
void bw_tr_frame(void);

#endif
