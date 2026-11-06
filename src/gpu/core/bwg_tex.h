/*
 * bwg_tex.h - the renderer's API-neutral core: the texture cache policy
 * (bwg_tex.c), lifted verbatim from bw_d3d11.c (lane gfx-core, gate GC): the
 * cache's identities and their index (item 43), the once-a-frame full hash
 * and the sparse check of a stable texture, the stale pitch, the decode into
 * upload-ready mips and a format, cube faces, render to texture, and the
 * glyph, 1-bit alpha, hard-alpha, menu-photo and movie classification (VFX,
 * T1; nothing of it in Original).
 *
 * The back end makes the API texture from a BwgUpload and keeps its objects
 * in the entry (BwgTexOps); the core never looks inside them.
 */
#ifndef BWG_TEX_H
#define BWG_TEX_H

#include <stddef.h>
#include <stdint.h>
#include "nv2a_backend.h"
#include "../bw_texrestore.h"           /* BwTrSlot: texture restoration's per-texture slot (T1) */
#include "bwg_surf.h"

#define BWG_MAX_TEX 1024

typedef struct BwgTex {
    uint32_t offset, w, h, pitch, color;
    uint64_t hash, sparse;
    uint32_t gen, stable;       /* frames the full hash has not changed */
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
    /* The back end's objects: D3D11's by name, another back end's in api[]
     * (api[1], srv, the view the draws sample). */
    union {
        void *api[2];
        struct {
            struct ID3D11Texture2D *tex;
            struct ID3D11ShaderResourceView *srv;
        };
    };
} BwgTex;

/* A texture ready to go up: its format (a DXGI_FORMAT), size, levels and
 * faces (1, or 6 for a cube: +X, -X, +Y, -Y, +Z, -Z), and each level's bytes
 * with their row pitch, face by face (mip[face * levels + level]). */
typedef struct { const void *data; uint32_t pitch; } BwgMip;
typedef struct {
    int format;
    uint32_t w, h, levels, faces;
    BwgMip mip[6 * 12];
} BwgUpload;

/* The back end's part. */
typedef struct {
    void (*release)(BwgTex *e);         /* its objects (and its restored chain's) released, cleared */
    int  (*create)(BwgTex *e, const Nv2aTexture *t, const BwgUpload *u);   /* 0: couldn't (logged) */
    /* T1: menu photos (kind 1) and movie frames (kind 2) deblocked on the
     * GPU in place of the entry's objects; 1 if it did. Afterwards the
     * bound targets are bound again. */
    int  (*deblock)(BwgTex *e, const Nv2aTexture *t, int kind);
    void (*surf_resolve)(BwgSurf *s);   /* VFX (D9): a surface's MSAA samples into its texture */
    /* I5 (BW_GPU_FEEDBACK=copy, lane gfx-ideas): the bound target s as it is
     * now, as a surface record whose view the draw samples (its MSAA samples
     * resolved, or a copy); NULL: couldn't (the stage keeps today's rule).
     * Optional: NULL here means no copies. */
    BwgSurf *(*feedback)(BwgSurf *s);
} BwgTexOps;
void bwg_tex_set_ops(const BwgTexOps *ops);

/* What a stage samples: a cache entry, or a surface this back end drew
 * (render to texture). Both NULL: nothing (draw untextured). */
typedef struct { BwgTex *e; BwgSurf *rt; } BwgTexRef;
BwgTexRef bwg_get_texture(const Nv2aTexture *t);
BwgTexRef bwg_get_cube_texture(const Nv2aTexture *t);
/* 1 when the reference has a view to sample (the back end's view non-NULL). */
int bwg_tex_ref_view(BwgTexRef r);

/* T1: the cache entry bwg_get_texture last returned (NULL for a render
 * target or a failure); read only by the texture restoration (Remastered). */
extern BwgTex *bwg_tex_last;
/* The cache's size, and the uploads and render-to-texture samplings so far
 * (BW_GPU_STATS; the back end zeroes the counts each second). */
int bwg_tex_count(void);
/* The cache by index, and dropped whole: every entry emptied as a refill
 * empties it (the back end releases its objects), the count and the index
 * reset (BW_GFXCAP's mid-scene open, lane gfx-capab: the cache then starts
 * from no entries, as the replay's does). */
BwgTex *bwg_tex_at(int i);
void bwg_tex_drop_all(void);
extern unsigned bwg_st_uploads, bwg_st_rtt;

/* BW_GPU_BLACKWATCH: what each frame sampled (see bw_d3d11.c black_watch).
 * Kinds: R a surface this back end drew (render to texture), F the current
 * target (feedback, read from guest memory), U uploaded this frame, H cached
 * (full hash equal), S cached (sampled check), G cached (already checked
 * this frame), X none. [frame & 1]. */
typedef struct { uint32_t offset, w, h, color; char kind; int zero_pct; } BwgTexUse;
#define BWG_MAX_USE 96
extern BwgTexUse bwg_tex_use[2][BWG_MAX_USE];
extern int bwg_tex_nuse[2];
extern int bwg_tex_watch;               /* -1 unread, 0 off, 1 on (the back end sets it) */

/* Swizzled and DXT textures have no rows: CONTROL1's stale pitch dropped. */
const Nv2aTexture *bwg_drop_stale_pitch(const Nv2aTexture *t, Nv2aTexture *fixed);
/* How many levels to upload: what the texture says it carries, down to 1x1, at most 12. */
uint32_t bwg_mip_count(const Nv2aTexture *t);
/* VFX (D9): the cached texture's flags: 1 glyph sheet, 2 1-bit alpha in
 * use, 4 hard-edged alpha (V1), 8 a menu/title photo (T1). */
int bwg_tex_vfx_flags(const Nv2aTexture *t);
/* T1 (item 11): is this the Bink movie texture. */
int bwg_tr_is_movie(const Nv2aTexture *t);
/* Does stage `st` of this stage program sample a cube map (BW_GPU_CUBE,
 * BW_GPU_DOTMODES), and does the program have a dot-product stage. */
int bwg_cube_stage(uint32_t prog, int st);
int bwg_dot_program(uint32_t prog);

#endif
