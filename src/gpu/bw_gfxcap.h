/*
 * bw_gfxcap.h - the GPU frame capture and its replay (BW_GFXCAP; lane
 * gfx-cap, docs/gfx/capture.md). A test aid in every edition.
 *
 * Recording (BW_GFXCAP=<dir>) writes what enters the GPU side: the method
 * stream the pushbuffer walk hands the executor, every byte of guest memory
 * the GPU side reads (as it reads it, against a model of guest memory), the
 * gamma ramp the present uploads and the semaphore target. Replaying
 * (BW_GFXCAP_REPLAY=<file>) boots the runtime as for a run and, instead of the
 * game, feeds the capture to the executor and the back end, checking every
 * flipped surface against the hash the live run wrote.
 *
 * Unset, g_bw_gfxcap stays 0 and every hook is one test of it.
 */
#ifndef BW_GFXCAP_H
#define BW_GFXCAP_H

#include <stddef.h>
#include <stdint.h>

/* 0 off, 1 recording, 2 replaying; set once by bw_gfxcap_init. */
extern volatile int g_bw_gfxcap;
#define BW_GFXCAP_REC() (g_bw_gfxcap == 1)

/* main.c, before the back end registers: reads the switches. */
void bw_gfxcap_init(void);
/* main.c, instead of the game's entry point when replaying: the exit code. */
int bw_gfxcap_replay_main(void);

/* Recorder hooks (only while BW_GFXCAP_REC()). On the NV2A thread. */
/* A method as it enters nv2a_pb_exec_method; sem_va is the executor's
 * semaphore target at that moment. */
void bw_gfxcap_method(uint32_t subch, uint32_t method, uint32_t param, uint32_t sem_va);
/* A run of ARRAY_ELEMENT16 words nv2a_pb_exec_idx16 has taken. */
void bw_gfxcap_idx16(const uint32_t *p, uint32_t n);
/* The GPU side is about to read guest memory [va, va + len). */
void bw_gfxcap_read(uint32_t va, uint32_t len);
/* The present is about to use the title's ramp with this sequence number. */
void bw_gfxcap_gamma(uint32_t seq, const uint8_t ramp[768]);
/* Lane gfx-regen, I1 (core/bwg_latch.c): a lock the latch drained (LOCK; at
 * the idle point, after: the held-back method has run and goes first), the
 * idle re-present (LATCHSHOW), and settle (the held-back method written, as
 * it has run). A replay feeds LOCK records to the latch and makes the same
 * re-present at each LATCHSHOW; older replays skip both. */
void bw_gfxcap_lock(uint32_t va, uint32_t pitch, uint32_t flags, uint32_t has_rect,
                    const int32_t r[4], int after);
void bw_gfxcap_latchshow(uint32_t va);
void bw_gfxcap_settle(void);

/* Both sides: the runtime's contiguous allocator, which the GPU side asks
 * (dma_resolve: its high-water mark; the seed and the readback: the size of
 * the block at va). Recording, the live answer is recorded when it changes
 * and returned; replaying, the recorded answer is returned. Call only when
 * g_bw_gfxcap is set (else call the runtime directly). */
uint32_t bw_gfxcap_contig_hwm(uint32_t live);
uint32_t bw_gfxcap_block(uint32_t va, uint32_t live);

/* Both sides: the flipped surface as the back end read it back into guest
 * memory (rows of row bytes at pitch from va): hashed and, at the flips
 * BW_GFXCAP_SHOTS names, written to a file. */
void bw_gfxcap_flip(uint32_t gen, uint32_t va, uint32_t pitch, uint32_t row, uint32_t rows,
                    uint32_t bpp);

/* ---- step A (lane gfx-capab): Remastered's pins, capture.md 4.2 ----------
 * The deterministic mode, on both sides while recording or replaying (and
 * only then): texture restoration does each request's work at the request and
 * finishes it at the flip of the frame that asked (bw_texrestore.c); the
 * readback is on and synchronous, so every flip has its hash (bw_d3d11.c);
 * the suite's present clock is an input (bw_gfxcap_dt). */
#define BW_GFXCAP_PINNED() (g_bw_gfxcap != 0)

/* Both sides: the back end's frame number, at the start of its flip. */
void bw_gfxcap_frame(uint32_t gen);
/* Both sides: the suite's time since its last present (OLED care's clock,
 * bw_vfx_post.c). Recording: the live value, recorded (PRESENT) and returned;
 * replaying: the recorded one. */
float bw_gfxcap_dt(float live);
/* Both sides: 1 when the suite's output picture of this flip is wanted
 * (BW_GFXCAP_PICS), with path its file name. */
int bw_gfxcap_pic_path(char *path, size_t cap);

/* ---- step B (lane gfx-capab): the mid-scene open, capture.md 5 -----------
 * BW_GFXCAP_START=<flip>|file:<path> (recording): the capture opens at that
 * flip (its first frame), or at the first flip after the file appears, with
 * an OPEN that carries the executor's state (as a block and as a method
 * preamble), the allocator's mark, the semaphore target, the frame number and
 * every surface the back end holds, read back. The live back end drops its
 * texture cache there, so it re-reads every texture as the replay will.
 * BW_GFXCAP_MIDOPEN=<flip> (recording, test aid): a capture from boot that
 * also opens mid-scene at that flip, so the file's tail is a mid-scene capture
 * of the same run (step B's gate).
 * Replaying: BW_GFXCAP_REPLAY_FROM=open starts at the first mid-scene OPEN;
 * BW_GFXCAP_STATE=preamble loads the executor's state through its preamble
 * instead of its block; BW_GFXCAP_IGNORE_DROP=1 (a full replay) leaves the
 * texture cache alone at a mid-scene OPEN. */

/* A surface as the back end's table holds it (96 bytes) */
typedef struct {
    uint32_t va, pitch, bpp, w, h, cur_pitch, clamped, dirty;
    uint32_t pw, ph, aa_x, aa_y, wsk_bits, aa_sx, used_gen, slot;
    uint32_t fmt, row_bytes, fail_gen, noaa_epoch;   /* fmt: DXGI; rows of row_bytes, ph of them */
    uint32_t msaa, reserved[3];                      /* msaa: its MSAA target's samples (0: none) */
} BwGcSurf;

/* The back end's side of a mid-scene open (the D3D11 back end registers it) */
typedef struct {
    /* recording, at the end of the flip before the open: every surface to
     * bw_gfxcap_surface, then the texture cache dropped */
    void (*open_live)(void);
    /* replaying, a mid-scene OPEN met in a replay from boot: the texture
     * cache dropped, as the live side did */
    void (*drop_textures)(void);
    /* replaying, loading a mid-scene OPEN: the frame number, then each
     * surface as the live table held it (1 if made) */
    void (*set_gen)(uint32_t gen);
    int (*surface)(const BwGcSurf *s, const uint8_t *bytes);
} BwGfxcapBackend;

void bw_gfxcap_backend(const BwGfxcapBackend *b);
/* Recording, from open_live: one surface, ph rows of row_bytes at row_pitch. */
void bw_gfxcap_surface(const BwGcSurf *s, const uint8_t *rows, uint32_t row_pitch);
/* Both sides: the end of the back end's flip gen (before its frame number
 * moves on); recording, the mid-scene open is decided here. */
void bw_gfxcap_flip_end(uint32_t gen);

/* The executor's side (carried nv2a_pb_exec.c) */
size_t bw_exec_state_save(uint8_t *out);
int bw_exec_state_load(const uint8_t *in, size_t len);
int bw_exec_state_compare(const uint8_t *in, size_t len, char *why, size_t cap);
uint32_t bw_exec_preamble(uint32_t *pairs, uint32_t max);

#endif
