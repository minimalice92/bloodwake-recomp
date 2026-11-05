/* SPDX-License-Identifier: GPL-2.0-or-later (with xemu's DSP it hosts; see NOTICE.md) */
/*
 * bw_dsp_trace_fmt.h - the DSP trace file format (BW_DSP_TRACE, au-trace).
 *
 * Shared by the recorder in the game (src/toolkit_patches/apu/bw_dsp_trace.c)
 * and the offline replayer (tools/dsp_replay). The full description, with
 * every record, is docs/audio/trace_format.md; keep the two in step.
 *
 * A file is a 64-byte header, a text block, then chunks. Each chunk is whole
 * records, compressed on its own (Windows' XPRESS Huffman, buffer mode), so a
 * file cut short by the end of a run loses only its last chunk. A record is
 * a type byte, a 32-bit payload length, then the payload; every integer is
 * little-endian.
 */
#ifndef BW_DSP_TRACE_FMT_H
#define BW_DSP_TRACE_FMT_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "bw_dsp.h"

#define BW_DTR_MAGIC "BWDTRC01"
#define BW_DTR_VERSION 1
#define BW_DTR_PAGE 4096u
#define BW_DTR_MODEL_PAGES 4096u        /* the scratch model covers 16 MB */

typedef struct BwDtrHeader {
    char magic[8];                      /* BW_DTR_MAGIC */
    uint32_t version;                   /* BW_DTR_VERSION */
    uint32_t dsp;                       /* 0 GP, 1 EP */
    uint32_t state_words;               /* BW_DSP_STATE_WORDS */
    uint32_t core_words;                /* BW_DSP_CORE_WORDS */
    uint32_t flags;                     /* BW_DTR_F_* */
    uint32_t key_every;                 /* a KEY record every this many runs (0 none) */
    uint32_t pid;
    uint32_t kind;                      /* 0 a trace, 1 a snapshot file */
    uint32_t text_len;                  /* bytes of text after the header */
    uint32_t reserved[5];
} BwDtrHeader;                          /* 64 bytes */

#define BW_DTR_F_WRITE_DATA 1u          /* SWRITE records carry the data */

typedef struct BwDtrChunk {
    uint32_t raw_len;                   /* bytes of records */
    uint32_t stored_len;                /* bytes that follow */
    uint32_t codec;                     /* BW_DTR_CODEC_* */
    uint32_t check;                     /* bw_dtr_hash32 of the raw bytes */
} BwDtrChunk;

enum { BW_DTR_CODEC_STORED = 0, BW_DTR_CODEC_XPRESS_HUFF = 1 };

/* Record types. */
enum {
    BW_DTR_SNAP = 1,      /* u32 frame, u32 div, u32 why, state[state_words],
                             u32 nregions, regions (see trace_format.md) */
    BW_DTR_RUN = 2,       /* u32 run, u32 frame, u32 div, u32 max_cycles, u32 events */
    BW_DTR_DMEM = 3,      /* u32 space ('X','Y','P','M','R'), u32 nruns, runs of
                             (u32 addr, u32 n, u32 words[n]) */
    BW_DTR_DCORE = 4,     /* u32 n, n pairs of (u32 index, u32 value) */
    BW_DTR_SREAD = 5,     /* u32 addr, u32 len, u32 nruns, runs of
                             (u32 off, u32 n, u8 bytes[n]) */
    BW_DTR_SWRITE = 6,    /* u32 addr, u32 len, u64 hash [, u8 data[len]] */
    BW_DTR_FREAD = 7,     /* u32 index, u32 len, u8 data[len] */
    BW_DTR_FWRITE = 8,    /* u32 index, u32 len, u8 data[len] */
    BW_DTR_END = 9,       /* u32 used, u32 idle, u64 h_core, h_x, h_y, h_p, h_m */
    BW_DTR_KEY = 10,      /* u32 run, state[state_words] (after that run) */
    BW_DTR_EVENT = 11,    /* u32 frame, u32 ev (BW_DSP_EV_*) */
    BW_DTR_NOTE = 12,     /* text */
    BW_DTR_CLOSE = 13,    /* u32 runs, u32 why */
};

/* RUN events: what happened to the DSP since its previous run. */
enum { BW_DTR_RUN_RESET = 1, BW_DTR_RUN_BOOT = 2, BW_DTR_RUN_FIRST = 4 };
/* SNAP why. */
enum { BW_DTR_WHY_TRACE = 0, BW_DTR_WHY_EDGE = 1, BW_DTR_WHY_ASKED = 2, BW_DTR_WHY_PLOAD = 3 };
/* SNAP regions. */
enum { BW_DTR_REGION_SCRATCH = 0, BW_DTR_REGION_FIFO = 1 };

/* ---- hashing: FNV-1a over 32-bit words (one multiply each). Any single word
 * changed changes the hash (each step is a bijection of the running value). */
#define BW_DTR_FNV_OFFSET 0xCBF29CE484222325ull
#define BW_DTR_FNV_PRIME 0x00000100000001B3ull

static __inline uint64_t bw_dtr_hash_words(uint64_t h, const uint32_t *w, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        h = (h ^ w[i]) * BW_DTR_FNV_PRIME;
    return h;
}

static __inline uint64_t bw_dtr_hash_bytes(const uint8_t *p, size_t n)
{
    uint64_t h = BW_DTR_FNV_OFFSET ^ n;
    size_t i;
    for (i = 0; i + 4 <= n; i += 4) {
        uint32_t w;
        memcpy(&w, p + i, 4);
        h = (h ^ w) * BW_DTR_FNV_PRIME;
    }
    for (; i < n; i++)
        h = (h ^ p[i]) * BW_DTR_FNV_PRIME;
    return h;
}

static __inline uint32_t bw_dtr_hash32(const uint8_t *p, size_t n)
{
    uint64_t h = bw_dtr_hash_bytes(p, n);
    return (uint32_t)(h ^ (h >> 32));
}

/* Core-section words that are not part of the state compared from frame to
 * frame: each instruction sets them before use (instr_cycle, cur_inst,
 * cur_inst_len). */
static __inline int bw_dtr_core_skip(size_t index)
{
    return index == offsetof(BwDspState, instr_cycle) / 4 ||
           index == offsetof(BwDspState, cur_inst) / 4 ||
           index == offsetof(BwDspState, cur_inst_len) / 4;
}

/* The five state hashes of an END record. */
typedef struct BwDtrHashes {
    uint64_t core, x, y, p, m;
} BwDtrHashes;

static __inline void bw_dtr_hash_state(const BwDspState *s, BwDtrHashes *h)
{
    const uint32_t *w = (const uint32_t *)s;
    uint64_t c = BW_DTR_FNV_OFFSET;
    size_t i;
    for (i = 0; i < BW_DSP_CORE_WORDS; i++)
        c = (c ^ (bw_dtr_core_skip(i) ? 0u : w[i])) * BW_DTR_FNV_PRIME;
    h->core = bw_dtr_hash_words(c, s->periph, 128);
    h->x = bw_dtr_hash_words(BW_DTR_FNV_OFFSET ^ 'X', s->x, 4096);
    h->y = bw_dtr_hash_words(BW_DTR_FNV_OFFSET ^ 'Y', s->y, 2048);
    h->p = bw_dtr_hash_words(BW_DTR_FNV_OFFSET ^ 'P', s->p, 4096);
    h->m = bw_dtr_hash_words(BW_DTR_FNV_OFFSET ^ 'M', s->mix, 1024);
}

/* The chunk codec (bw_dsp_trace_fmt.c). compress: out needs
 * bw_dtr_bound(len) bytes; returns the stored length and sets *codec.
 * decompress: returns 0 on success. Not thread-safe per handle: each thread
 * keeps its own (bw_dtr_codec_new). */
typedef struct BwDtrCodec BwDtrCodec;
BwDtrCodec *bw_dtr_codec_new(void);
void bw_dtr_codec_free(BwDtrCodec *c);
size_t bw_dtr_bound(size_t len);
size_t bw_dtr_compress(BwDtrCodec *c, const uint8_t *in, size_t len, uint8_t *out, size_t cap,
                       uint32_t *codec);
int bw_dtr_decompress(BwDtrCodec *c, uint32_t codec, const uint8_t *in, size_t len,
                      uint8_t *out, size_t raw_len);

#endif
