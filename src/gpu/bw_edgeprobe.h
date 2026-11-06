/*
 * bw_edgeprobe.h - a test aid for the edge-pixel residual (lane gfx-ideas,
 * docs/gfx/lanes/ideas.md; gfx-capef's report section 4): extra GPU copies of
 * the scene target, placed where BW_GPU_EDGEPROBE says, to find the
 * operation whose result they change; and, at chosen flips, a per-row hash of
 * every stage's output written to a file, to see where two replays part.
 * Unset, nothing here runs (bw_edgeprobe_on() is 0).
 *
 *   BW_GPU_EDGEPROBE=<points>[,churn]   where the target is copied:
 *       begin   before every clear and draw (after the targets are bound)
 *       draw    after every draw
 *       last    at the flip, before the present (after the frame's last draw)
 *       post    at the flip, after the present, before the readback
 *       end     at the flip, after the readback
 *       alloc   no copy: a texture the target's size made and freed each
 *               flip (memory churn alone)
 *       churn   the copies go into textures made at the frame's first copy
 *               and freed at its flip (as gfx-capef's tracker did), not one
 *               kept texture
 *       none    no copies (to measure only)
 *   BW_GPU_EDGEPROBE_FLIPS=A-B           only in flips A..B (default every flip)
 *   BW_GPU_EDGEPROBE_MEASURE=all|A-B     at the end of those flips (after the
 *       readback and the present), the shown surface's MSAA samples, its
 *       resolved texture and the readback's box-filtered copy are hashed row by
 *       row on the GPU into <BW_GPU_EDGEPROBE_OUT>/edge_fNNNNN.rows, with one
 *       [EDGEPROBE] line a flip
 *   BW_GPU_EDGEPROBE_DUMP=1              at those flips, the resolved texture's
 *       bytes too (edge_fNNNNN_resolved.px: "BWEDGEPX", width, height, bytes a
 *       pixel, DXGI format, then the rows)
 */
#ifndef BW_EDGEPROBE_H
#define BW_EDGEPROBE_H

#include <stdint.h>

enum { BW_EP_BEGIN = 1, BW_EP_DRAW = 2, BW_EP_LAST = 4, BW_EP_POST = 8, BW_EP_END = 16, BW_EP_ALLOC = 32 };

extern int g_bw_edgeprobe;              /* -1 unread, 0 off, 1 on */
int bw_edgeprobe_read(void);
static inline int bw_edgeprobe_on(void)
{
    return g_bw_edgeprobe < 0 ? bw_edgeprobe_read() : g_bw_edgeprobe;
}

/* At a point (BW_EP_*): ctx the ID3D11DeviceContext, tex the target (the MSAA
 * texture when there is one), gen the frame. */
void bw_edgeprobe_at(int point, void *ctx, void *tex, uint32_t gen);
/* The flip's end: the measure (mtex may be NULL; rb is the readback's
 * box-filtered texture when it made one, else NULL), and churn's frees. */
void bw_edgeprobe_flip_end(void *ctx, void *mtex, void *tex, void *rb, uint32_t gen);

#endif
