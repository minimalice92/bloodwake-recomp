/* bw_pscache.h - persistent cache of the combiner pixel shaders' bytecode
 * (agent T1, every edition; see bw_pscache.c). */
#ifndef BW_PSCACHE_H
#define BW_PSCACHE_H

#include <stddef.h>
#include <stdint.h>

/* At device setup: read every valid entry from the cache folder. */
void bw_pscache_init(void);

/* The compiled bytecode for this shader text (ps_5_0, O3), if the cache has
 * it; NULL otherwise. The bytes stay owned by the cache. */
const void *bw_pscache_find(const char *src, size_t len, size_t *size);

/* A shader just compiled: keep it for later runs. An entry already held for
 * this text with other bytes (cached bytecode the driver rejected) is
 * replaced, in memory and on disk (0.2.5 item 41). */
void bw_pscache_store(const char *src, size_t len, const void *blob, size_t size);

/* 0.2.5 item 43: the same for the vertex programs' shaders (vs_5_0, O3 with
 * IEEE strictness; ws = compiled with the BW_WS define), kept as
 * vs_<hash>.bin beside the pixel shaders. Called on the device's thread
 * only, as the pixel-shader calls are. */
const void *bw_pscache_find_vs(const char *src, size_t len, int ws, size_t *size);
void bw_pscache_store_vs(const char *src, size_t len, int ws, const void *blob, size_t size);
void bw_pscache_vs_counts(unsigned *hits, unsigned *stored);   /* this run's, for the log */
/* gfx-regen: whether the cache holds this vertex shader, from any thread:
 * the load-time compile's workers (bw_vshpre.c) look with this and store
 * with bw_pscache_store_vs; the table is locked inside. */
int bw_pscache_has_vs(const char *src, size_t len, int ws);

/* gfx-ffp (BW_GPU_VSH_SYNC, the warm-up): a vertex shader's cache key, and
 * its bytecode by that key (not counted as a hit). */
void bw_pscache_vs_key(const char *src, size_t len, int ws, uint64_t *h1, uint64_t *h2);
const void *bw_pscache_find_vs_key(uint64_t h1, uint64_t h2, uint32_t textlen, size_t *size);

/* gfx-ffp: the manifest of the vertex shaders this machine has drawn with,
 * beside the cache (vs_manifest_<stamp>.txt): each one's cache key and kind,
 * hashes and numbers only (no shader text, no program, nothing of the
 * game). <stamp> is the caller's name for its translator, so a build whose
 * translation changed starts a list of its own. Read returns the count and a
 * malloc'd array (the caller frees it); add appends one line. Nothing when
 * the cache is off. */
typedef struct {
    uint64_t h1, h2;
    uint32_t textlen;
    int ws;
    char kind;                  /* 'p' a vertex program, 'f' the fixed function */
} BwVsManifestEntry;
unsigned bw_pscache_manifest_read(uint64_t stamp, BwVsManifestEntry **out);
void bw_pscache_manifest_add(uint64_t stamp, const BwVsManifestEntry *e);

/* BW_GPU_PSCACHE_VERIFY=1: compile on hits too and compare (test aid). */
int bw_pscache_verify(void);

/* A line for the log: the shader came from the cache (compiled 0) or was
 * compiled (1) in `ms`, with this run's totals. */
void bw_pscache_note(int compiled, double ms);

#endif
