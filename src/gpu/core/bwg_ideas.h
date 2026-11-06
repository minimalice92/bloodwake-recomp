/*
 * bwg_ideas.h - the renderer's API-neutral core: the adopted ideas of
 * docs/gfx/arch.md 3.14 that this batch builds (lane gfx-ideas,
 * docs/agents/BRIEF_gfx3.md), each behind its own switch, every one off
 * unless set:
 *
 *   I5, BW_GPU_FEEDBACK   a draw that samples the target it draws into
 *       (unset: copy in Remastered and later, today's rule in Original;
 *       the lead's default, never triggered over the corpus)
 *       0        today's rule: such a stage reads the guest bytes at the
 *                target's address (the texture cache), as before
 *       count    today's rule, and every such draw counted and logged
 *                ([GPU-FEEDBACK]): an exact one (the texture starts where
 *                the bound colour target starts), an overlapping one (it
 *                starts elsewhere but its bytes cross the target's), and a
 *                texture over the bound depth buffer
 *       copy     an exact one samples the target as it is before the draw
 *                (the back end's feedback op: the resolve of its MSAA
 *                samples, or a copy), counted as with count
 *   I2, BW_GPU_RBRING     the flip's readback without the wait (bw_d3d11.c)
 *   I3, BW_GPU_DELTA      only changed bindings sent to the API (bw_d3d11.c;
 *                         unset: on in Remastered and later, off in Original)
 *
 * The switches are read once.
 */
#ifndef BWG_IDEAS_H
#define BWG_IDEAS_H

#include <stdint.h>
#include "nv2a_backend.h"

enum { BWG_FB_OFF = 0, BWG_FB_COUNT = 1, BWG_FB_COPY = 2 };
int bwg_feedback_mode(void);

/* A clear or draw starts (the back end's begin, after it bound its targets):
 * the stages that follow belong to it. */
void bwg_fb_begin(void);
/* A stage of the current draw samples t (the texture cache's lookups): 1 when
 * it is an exact feedback (t starts where the bound colour target starts),
 * after counting; 0 otherwise. Only called with the mode on. */
int bwg_fb_stage(const Nv2aTexture *t, uint32_t span);
/* I5 copy: the back end's copy of the bound target cur for this draw, made
 * once a draw however many stages sample it (op: BwgTexOps.feedback);
 * counted; NULL when there is no op or it failed. */
struct BwgSurf;
struct BwgSurf *bwg_fb_copy(struct BwgSurf *cur, struct BwgSurf *(*op)(struct BwgSurf *));
/* The flip: a summary line every 60 flips and whenever a count moved (so the
 * last line holds the final counts). */
void bwg_fb_flip(void);

/* The counts so far (exact draws, overlapping draws, depth draws, copies made,
 * draws seen). */
typedef struct {
    uint64_t draws, exact, overlap, depth, copies, copy_failed;
} BwgFbCounts;
extern BwgFbCounts bwg_fb;

#endif
