/*
 * bwg_surf.h - the renderer's API-neutral core: the guest surfaces and depth
 * buffers by address (bwg_surf.c), lifted verbatim from bw_d3d11.c (lane
 * gfx-core, gate GC): their logical and physical sizes, AA factors,
 * render-scale epochs, the slot tables and their reuse (item 37), the
 * remakes (items 36, 38, 39) and the seed's guest memory (item 38).
 *
 * The back end makes and releases the API objects (BwgSurfOps); the core
 * decides when, and holds them in each record without looking inside: it
 * only asks whether the target exists (tex, the first of them, non-NULL).
 */
#ifndef BWG_SURF_H
#define BWG_SURF_H

#include <stdint.h>
#include "nv2a_backend.h"

/* The back ends' object types, by name only (the core never calls them). */
struct ID3D11Texture2D;
struct ID3D11RenderTargetView;
struct ID3D11ShaderResourceView;
struct ID3D11DepthStencilView;

#define BWG_MAX_SURF 32
#define BWG_MAX_TEXTURE_DIM 16384       /* D3D11's and D3D12's 2D limit (item 36) */

typedef struct BwgSurf {
    uint32_t va, pitch, bpp, w, h;
    uint32_t cur_pitch;         /* the pitch of the latest binding */
    int clamped;                /* readback hit the end of the guest block (logged once) */
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
    int mdirty;                 /* drawn since the last resolve */
    uint8_t aa_sx;              /* the latest binding's horizontal AA factor (gpu_present) */
    /* 0.2.5 item 36: the frame of the last failed (re)create, so a target
     * the device can't make is tried once a frame, not once a draw; and the
     * render-scale epoch (+1) at which its MSAA target couldn't be made (it
     * and its depth buffer are then single-sample; 0: no such failure). */
    uint32_t fail_gen;
    unsigned noaa_epoch;
    uint32_t used_gen;          /* item 37: the frame it was last drawn into or sampled (a full table reuses the oldest) */
    /* The back end's objects: D3D11's by name, another back end's in api[]
     * (api[0], tex, its target: non-NULL when the surface is made). */
    union {
        void *api[5];
        struct {
            struct ID3D11Texture2D *tex;
            struct ID3D11RenderTargetView *rtv;
            struct ID3D11ShaderResourceView *srv;
            struct ID3D11Texture2D *mtex;
            struct ID3D11RenderTargetView *mrtv;
        };
    };
} BwgSurf;

typedef struct BwgDepth {
    uint32_t va, w, h;          /* physical size (VFX: the render-scaled size) */
    unsigned epoch;             /* VFX (D9) */
    unsigned samples;           /* 0.2.5 item 36: its sample count (the colour target's; 1 in Original) */
    uint32_t used_gen;          /* item 37: the frame it was last drawn with */
    /* The back end's objects, as BwgSurf's (D3D11: srv is the depth as a
     * texture, AO only; NULL otherwise). */
    union {
        void *api[3];
        struct {
            struct ID3D11Texture2D *tex;
            struct ID3D11DepthStencilView *dsv;
            struct ID3D11ShaderResourceView *srv;
        };
    };
} BwgDepth;

/* VFX (D9): a draw into a surface; the MSAA copy then needs a resolve. */
#define SURF_DRAWN(sf) ((sf)->dirty = 1, (sf)->mdirty = 1)

/* The back end's part. */
typedef struct {
    void (*release)(BwgSurf *s);        /* its objects released, the pointers cleared */
    int  (*create)(BwgSurf *s);         /* made at pw x ph and seeded; 0: couldn't (item 36) */
    void (*forget)(BwgSurf *s);         /* a slot being reused: drop the back end's pointers to it */
    void (*release_depth)(BwgDepth *d);
    int  (*create_depth)(BwgDepth *d);  /* made at w x h with its samples; 0: couldn't */
} BwgSurfOps;
void bwg_surf_set_ops(const BwgSurfOps *ops);

/* The bound colour surface and depth buffer (the back end binds them). */
extern BwgSurf *bwg_cur;
extern BwgDepth *bwg_cur_depth;
/* Item 37: slots reused so far (the stats line). */
extern unsigned bwg_surf_reused, bwg_depth_reused;
int bwg_surf_count(void);
/* The table by index (BW_GFXCAP's mid-scene open, lane gfx-capab):
 * bwg_surf_at for i < bwg_surf_count(); bwg_surf_slot_at grows the table to
 * slot k with zeroed entries, as it grows when a new surface takes a slot. */
BwgSurf *bwg_surf_at(int i);
BwgSurf *bwg_surf_slot_at(int k);

/* The surface for the executor's colour target, made or remade as needed;
 * NULL when it can't be made this frame. */
BwgSurf *bwg_get_surf(const Nv2aSurface *in);
/* The depth buffer at va for a colour target of w x h with `samples`
 * samples per pixel; NULL for none. */
BwgDepth *bwg_get_depth(uint32_t va, uint32_t w, uint32_t h, unsigned samples);
/* A made surface at this address (render to texture), or NULL. */
BwgSurf *bwg_find_surf(uint32_t va);
/* Item 38: the guest memory a new surface is seeded from (see bwg_surf.c);
 * *tmp is a copy to free; NULL: no seed. */
const void *bwg_seed_src(const BwgSurf *s, void **tmp);

#endif
