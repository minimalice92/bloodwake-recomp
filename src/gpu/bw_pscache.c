/*
 * bw_pscache.c - persistent cache of the combiner pixel shaders' bytecode
 * (agent T1, 2026-10-27; every edition, lead's call under D-014).
 *
 * Why: the D3D11 back end turns each new register-combiner state into an
 * HLSL pixel shader (bw_rc_hlsl.c) and compiles it with D3DCompile at O3 on
 * the device thread when a draw first needs it. Each compile takes 15-60 ms,
 * and a mission's first seconds need about 20 (mission 1, seen): the ~100 ms
 * frames at the start of gameplay (runs *-t1-q-seq-on / -off, T1.md "Second
 * task"). With this cache a shader compiles once per machine; later runs load
 * the bytecode.
 *
 * Same pixels: an entry holds exactly the bytes D3DCompile produced for that
 * shader text with the same compiler and flags; BW_GPU_PSCACHE_VERIFY=1
 * compiles on every hit too and compares.
 *
 * Keyed by the generated HLSL text itself (FNV-1a 64 plus a second 64-bit
 * hash and its length), which spells out every part of the combiner key,
 * the Remastered/VFX variant bits and any baked-in setting; and tagged with
 * the compiler (d3dcompiler_47.dll's size and date), target and flags. So a
 * code change, a new compiler or a different setting just misses and
 * compiles.
 *
 * Where: %LOCALAPPDATA%\xboxrecomp\shadercache\ps_<hash>.bin, one file per
 * shader, written to a temporary file and renamed (several copies of the
 * game may run at once). Derived from the game's own combiner state on this
 * machine: never in the repository, never shipped. BW_GPU_PSCACHE=<folder>
 * uses another folder, BW_GPU_PSCACHE=0 turns the cache off. A corrupt,
 * truncated or mismatched file is skipped (the shader compiles).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "bw_pscache.h"

#define PSC_MAGIC 0x53505742u           /* "BWPS" */
#define PSC_VERSION 1u

typedef struct {
    uint32_t magic, version;
    uint64_t tag;                       /* compiler, target, flags */
    uint64_t h1, h2;                    /* the shader text */
    uint32_t textlen, blobsize;
    uint64_t blobsum;
} PscHeader;

typedef struct {
    uint64_t h1, h2;
    uint32_t textlen, size;
    void *blob;
} PscEntry;

static PscEntry *s_ent;
static unsigned s_n, s_cap;
/* The folder, wide for the file calls and UTF-8 for the log: it sits under
 * the user's profile, whose name can hold any letter (0.2.5 item 53; the
 * ANSI calls lost the cache for a Cyrillic name on English Windows). */
static wchar_t s_dirw[MAX_PATH];
static char s_dir[3 * MAX_PATH];
static int s_on = -1, s_verify = -1;
static uint64_t s_tag;
static unsigned s_loaded, s_skipped, s_hits, s_compiled, s_stored, s_logged;
static double s_compile_ms, s_hit_ms;
/* 0.2.5 item 43: the vertex programs' shaders too (vs_<hash>.bin, their
 * own compiler tag; the text is hashed after a prefix naming the target
 * and the widescreen define, so no vertex entry ever matches a pixel one). */
static uint64_t s_tag_vs;
static unsigned s_vs_loaded, s_vs_hits, s_vs_stored;

static uint64_t fnv64(const void *p, size_t n, uint64_t h)
{
    const uint8_t *q = (const uint8_t *)p;
    size_t i;
    for (i = 0; i < n; i++)
        h = (h ^ q[i]) * 0x100000001B3ull;
    return h;
}

static uint64_t h_a(const void *p, size_t n) { return fnv64(p, n, 0xCBF29CE484222325ull); }
static uint64_t h_b(const void *p, size_t n) { return fnv64(p, n, 0x84222325CBF29CE4ull) ^ (uint64_t)n << 40; }

/* The compiler's identity, the target and the flags: get_rc_shader's
 * ("ps_5_0|ps_main|O3", the same string as before 0.2.5, so the caches
 * already on players' PCs stay valid) or the vertex programs'. */
static uint64_t make_tag(const char *what)
{
    wchar_t path[MAX_PATH];
    char buf[MAX_PATH + 128];
    WIN32_FILE_ATTRIBUTE_DATA fa;
    HMODULE m = GetModuleHandleW(L"d3dcompiler_47.dll");
    memset(&fa, 0, sizeof fa);
    path[0] = 0;
    if (m && GetModuleFileNameW(m, path, MAX_PATH))
        GetFileAttributesExW(path, GetFileExInfoStandard, &fa);
    _snprintf(buf, sizeof buf, "bwps%u|%s|%lu|%lu|%lu|%lu", PSC_VERSION, what,
              (unsigned long)fa.nFileSizeLow, (unsigned long)fa.nFileSizeHigh,
              (unsigned long)fa.ftLastWriteTime.dwLowDateTime,
              (unsigned long)fa.ftLastWriteTime.dwHighDateTime);
    buf[sizeof buf - 1] = 0;
    return h_a(buf, strlen(buf));
}

/* item 43: a vertex shader's text hashed after its prefix */
static void vs_hash(const char *src, size_t len, int ws, uint64_t *h1, uint64_t *h2)
{
    const char *pre = ws ? "vs_5_0|BW_WS|" : "vs_5_0|";
    size_t pl = strlen(pre);
    *h1 = fnv64(src, len, fnv64(pre, pl, 0xCBF29CE484222325ull));
    *h2 = fnv64(src, len, fnv64(pre, pl, 0x84222325CBF29CE4ull)) ^ (uint64_t)len << 40;
}

static int add(uint64_t h1, uint64_t h2, uint32_t textlen, const void *blob, uint32_t size)
{
    void *copy;
    if (s_n == s_cap) {
        unsigned nc = s_cap ? s_cap * 2 : 256;
        PscEntry *ne = (PscEntry *)realloc(s_ent, nc * sizeof *ne);
        if (!ne)
            return 0;
        s_ent = ne;
        s_cap = nc;
    }
    copy = malloc(size);
    if (!copy)
        return 0;
    memcpy(copy, blob, size);
    s_ent[s_n].h1 = h1; s_ent[s_n].h2 = h2;
    s_ent[s_n].textlen = textlen; s_ent[s_n].size = size;
    s_ent[s_n].blob = copy;
    s_n++;
    return 1;
}

static int load_file(const wchar_t *path, uint64_t tag)
{
    FILE *f = _wfopen(path, L"rb");
    PscHeader h;
    void *blob = NULL;
    int ok = 0;
    if (!f)
        return 0;
    if (fread(&h, 1, sizeof h, f) == sizeof h && h.magic == PSC_MAGIC && h.version == PSC_VERSION
            && h.tag == tag && h.blobsize > 0 && h.blobsize < (4u << 20)
            && (blob = malloc(h.blobsize)) != NULL
            && fread(blob, 1, h.blobsize, f) == h.blobsize
            && fnv64(blob, h.blobsize, 0xCBF29CE484222325ull) == h.blobsum)
        ok = add(h.h1, h.h2, h.textlen, blob, h.blobsize);
    free(blob);
    fclose(f);
    return ok;
}

/* Every valid file of one kind ("ps" or "vs") with this tag, loaded. */
static void load_dir(const char *kind, uint64_t tag, unsigned *loaded, unsigned *skipped)
{
    wchar_t pat[MAX_PATH + 16];
    WIN32_FIND_DATAW fd;
    HANDLE fh;
    _snwprintf(pat, MAX_PATH + 16, L"%ls\\%hs_*.bin", s_dirw, kind);
    pat[MAX_PATH + 15] = 0;
    fh = FindFirstFileW(pat, &fd);
    if (fh == INVALID_HANDLE_VALUE)
        return;
    do {
        wchar_t path[MAX_PATH * 2];
        _snwprintf(path, MAX_PATH * 2, L"%ls\\%ls", s_dirw, fd.cFileName);
        path[MAX_PATH * 2 - 1] = 0;
        if (load_file(path, tag))
            (*loaded)++;
        else
            (*skipped)++;
    } while (FindNextFileW(fh, &fd));
    FindClose(fh);
}

void bw_pscache_init(void)
{
    const char *e = getenv("BW_GPU_PSCACHE");
    wchar_t ew[MAX_PATH];
    DWORD en = GetEnvironmentVariableW(L"BW_GPU_PSCACHE", ew, MAX_PATH);
    LARGE_INTEGER t0, t1, fq;
    unsigned vs_skipped = 0;
    if (s_on >= 0)
        return;
    s_on = 0;
    if (e && e[0] == '0' && !e[1]) {
        fprintf(stderr, "[PSCACHE] off (BW_GPU_PSCACHE=0): every pixel shader compiles\n");
        return;
    }
    if (en && en < MAX_PATH)
        _snwprintf(s_dirw, MAX_PATH, L"%ls", ew);
    else if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, s_dirw)))
        wcsncat(s_dirw, L"\\xboxrecomp\\shadercache", MAX_PATH - wcslen(s_dirw) - 1);
    else
        return;
    s_dirw[MAX_PATH - 1] = 0;
    if (!WideCharToMultiByte(CP_UTF8, 0, s_dirw, -1, s_dir, sizeof s_dir, NULL, NULL))
        s_dir[0] = 0;
    SHCreateDirectoryExW(NULL, s_dirw, NULL);
    s_tag = make_tag("ps_5_0|ps_main|O3");
    s_tag_vs = make_tag("vs_5_0|vs_main|O3|IEEE");     /* item 43: get_vsh's compile */
    s_on = 1;
    QueryPerformanceFrequency(&fq);
    QueryPerformanceCounter(&t0);
    load_dir("ps", s_tag, &s_loaded, &s_skipped);
    load_dir("vs", s_tag_vs, &s_vs_loaded, &vs_skipped);
    QueryPerformanceCounter(&t1);
    fprintf(stderr, "[PSCACHE] %u pixel shaders loaded from %s (%u skipped: other compiler or"
                    " version, or damaged) in %.1f ms\n", s_loaded, s_dir, s_skipped,
            (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)fq.QuadPart);
    fprintf(stderr, "[PSCACHE] %u vertex shaders loaded (%u skipped)\n", s_vs_loaded, vs_skipped);
}

int bw_pscache_verify(void)
{
    if (s_verify < 0) {
        const char *e = getenv("BW_GPU_PSCACHE_VERIFY");
        s_verify = e && e[0] == '1';
    }
    return s_verify;
}

/* gfx-regen: the table is shared with the load-time vertex-shader compile's
 * workers (bw_vshpre.c), which look and store from their own threads; every
 * reader takes this lock shared and every writer exclusive. A blob handed
 * out stays valid: an entry's bytes are only replaced by different bytes for
 * the same text (item 41's rejected bytecode), which the workers never store
 * (they store only a text the table lacked). */
static SRWLOCK s_lock = SRWLOCK_INIT;

static const void *find_h_locked(uint64_t h1, uint64_t h2, uint32_t textlen, size_t *size)
{
    unsigned i;
    if (s_on <= 0 || !s_n)
        return NULL;
    for (i = 0; i < s_n; i++)
        if (s_ent[i].h1 == h1 && s_ent[i].h2 == h2 && s_ent[i].textlen == textlen) {
            *size = s_ent[i].size;
            return s_ent[i].blob;
        }
    return NULL;
}

static const void *find_h(uint64_t h1, uint64_t h2, uint32_t textlen, size_t *size)
{
    const void *p;
    AcquireSRWLockShared(&s_lock);
    p = find_h_locked(h1, h2, textlen, size);
    ReleaseSRWLockShared(&s_lock);
    return p;
}

const void *bw_pscache_find(const char *src, size_t len, size_t *size)
{
    if (s_on <= 0 || !s_n)
        return NULL;
    return find_h(h_a(src, len), h_b(src, len), (uint32_t)len, size);
}

/* Keep one shader's bytes, in memory and in its file (kind "ps" or "vs").
 * 1 when the file was written. */
static int store_h(const char *kind, uint64_t tag, uint64_t h1, uint64_t h2, uint32_t textlen,
                   const void *blob, size_t size)
{
    PscHeader h;
    wchar_t tmp[MAX_PATH * 2], fin[MAX_PATH * 2];
    size_t have = 0;
    FILE *f;
    int ok;
    if (s_on <= 0 || !blob || !size || size >= (4u << 20))
        return 0;
    memset(&h, 0, sizeof h);
    h.magic = PSC_MAGIC;
    h.version = PSC_VERSION;
    h.tag = tag;
    h.h1 = h1;
    h.h2 = h2;
    h.textlen = textlen;
    h.blobsize = (uint32_t)size;
    h.blobsum = fnv64(blob, size, 0xCBF29CE484222325ull);
    AcquireSRWLockExclusive(&s_lock);   /* gfx-regen: the workers store too */
    {
        const void *old = find_h_locked(h1, h2, textlen, &have);
        if (old == NULL)
            add(h.h1, h.h2, h.textlen, blob, h.blobsize);
        else if (have != size || memcmp(old, blob, size)) {
            /* 0.2.5 item 41: bytes the driver rejected (the caller then
             * compiles): the fresh ones replace them here and in the file
             * below, so the next run loads good bytes. */
            unsigned i;
            void *copy = malloc(size);
            for (i = 0; copy && i < s_n; i++)
                if (s_ent[i].blob == old) {
                    memcpy(copy, blob, size);
                    free(s_ent[i].blob);
                    s_ent[i].blob = copy;
                    s_ent[i].size = h.blobsize;
                    copy = NULL;
                    break;
                }
            free(copy);
            fprintf(stderr, "[PSCACHE] a cached %s shader (%s_%016llX) was replaced by a fresh compile\n",
                    kind[0] == 'v' ? "vertex" : "pixel", kind, (unsigned long long)h.h1);
        }
    }
    ReleaseSRWLockExclusive(&s_lock);
    _snwprintf(fin, MAX_PATH * 2, L"%ls\\%hs_%016llX.bin", s_dirw, kind, (unsigned long long)h.h1);
    _snwprintf(tmp, MAX_PATH * 2, L"%ls\\%hs_%016llX.%lu.%lu.tmp", s_dirw, kind,
               (unsigned long long)h.h1, (unsigned long)GetCurrentProcessId(),
               (unsigned long)GetTickCount());
    fin[MAX_PATH * 2 - 1] = 0;
    tmp[MAX_PATH * 2 - 1] = 0;
    f = _wfopen(tmp, L"wb");
    if (!f)
        return 0;
    ok = fwrite(&h, 1, sizeof h, f) == sizeof h && fwrite(blob, 1, h.blobsize, f) == h.blobsize;
    ok = (fclose(f) == 0) && ok;
    if (!ok || !MoveFileExW(tmp, fin, MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp);
        return 0;
    }
    return 1;
}

void bw_pscache_store(const char *src, size_t len, const void *blob, size_t size)
{
    if (s_on <= 0)
        return;
    if (store_h("ps", s_tag, h_a(src, len), h_b(src, len), (uint32_t)len, blob, size))
        s_stored++;
}

const void *bw_pscache_find_vs(const char *src, size_t len, int ws, size_t *size)
{
    uint64_t h1, h2;
    const void *p;
    if (s_on <= 0 || !s_n)
        return NULL;
    vs_hash(src, len, ws, &h1, &h2);
    p = find_h(h1, h2, (uint32_t)len, size);
    if (p)
        s_vs_hits++;
    return p;
}

void bw_pscache_store_vs(const char *src, size_t len, int ws, const void *blob, size_t size)
{
    uint64_t h1, h2;
    if (s_on <= 0)
        return;
    vs_hash(src, len, ws, &h1, &h2);
    if (store_h("vs", s_tag_vs, h1, h2, (uint32_t)len, blob, size))
        InterlockedIncrement((volatile LONG *)&s_vs_stored);
}

/* gfx-regen: whether the cache holds this vertex shader, from any thread
 * (bw_vshpre.c's workers), without counting a hit. */
int bw_pscache_has_vs(const char *src, size_t len, int ws)
{
    uint64_t h1, h2;
    size_t sz = 0;
    if (s_on <= 0)
        return 0;
    vs_hash(src, len, ws, &h1, &h2);
    return find_h(h1, h2, (uint32_t)len, &sz) != NULL;
}

void bw_pscache_vs_counts(unsigned *hits, unsigned *stored)
{
    *hits = s_vs_hits;
    *stored = s_vs_stored;
}

/* ---- gfx-ffp: keys and the manifest (BW_GPU_VSH_SYNC's warm-up) ---------- */

void bw_pscache_vs_key(const char *src, size_t len, int ws, uint64_t *h1, uint64_t *h2)
{
    vs_hash(src, len, ws, h1, h2);
}

const void *bw_pscache_find_vs_key(uint64_t h1, uint64_t h2, uint32_t textlen, size_t *size)
{
    return find_h(h1, h2, textlen, size);
}

static void manifest_path(uint64_t stamp, wchar_t *out, size_t n)
{
    _snwprintf(out, n, L"%ls\\vs_manifest_%016llX.txt", s_dirw, (unsigned long long)stamp);
    out[n - 1] = 0;
}

unsigned bw_pscache_manifest_read(uint64_t stamp, BwVsManifestEntry **out)
{
    wchar_t path[MAX_PATH * 2];
    char line[160];
    BwVsManifestEntry *v = NULL;
    unsigned n = 0, cap = 0;
    FILE *f;
    *out = NULL;
    if (s_on <= 0)
        return 0;
    manifest_path(stamp, path, MAX_PATH * 2);
    if (!(f = _wfopen(path, L"r")))
        return 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long long a, b;
        unsigned len, ws;
        char kind;
        unsigned i;
        /* "<h1> <h2> <textlen> <ws> <kind>"; anything else (a line cut short
         * by another copy of the game writing at the same moment) is skipped */
        if (sscanf(line, "%llx %llx %u %u %c", &a, &b, &len, &ws, &kind) != 5
                || (kind != 'p' && kind != 'f') || ws > 1)
            continue;
        for (i = 0; i < n && !(v[i].h1 == a && v[i].h2 == b && v[i].textlen == len && v[i].ws == (int)ws); i++)
            ;
        if (i < n)
            continue;                   /* listed twice (two copies met it in the same run) */
        if (n == cap) {
            unsigned nc = cap ? cap * 2 : 64;
            BwVsManifestEntry *nv = (BwVsManifestEntry *)realloc(v, nc * sizeof *nv);
            if (!nv)
                break;
            v = nv;
            cap = nc;
        }
        v[n].h1 = a;
        v[n].h2 = b;
        v[n].textlen = len;
        v[n].ws = (int)ws;
        v[n].kind = kind;
        n++;
    }
    fclose(f);
    *out = v;
    return n;
}

void bw_pscache_manifest_add(uint64_t stamp, const BwVsManifestEntry *e)
{
    wchar_t path[MAX_PATH * 2];
    char line[96];
    FILE *f;
    int len;
    if (s_on <= 0)
        return;
    manifest_path(stamp, path, MAX_PATH * 2);
    len = _snprintf(line, sizeof line, "%016llX %016llX %u %d %c\n", (unsigned long long)e->h1,
                    (unsigned long long)e->h2, e->textlen, e->ws ? 1 : 0, e->kind);
    if (len <= 0 || len >= (int)sizeof line)
        return;
    /* one write of one short line, appended (several copies of the game may
     * append at once; a torn line is skipped when read) */
    if ((f = _wfopen(path, L"ab")) != NULL) {
        fwrite(line, 1, (size_t)len, f);
        fclose(f);
    }
}

void bw_pscache_note(int compiled, double ms)
{
    if (compiled) {
        s_compiled++;
        s_compile_ms += ms;
    } else {
        s_hits++;
        s_hit_ms += ms;
    }
    if (s_logged++ < 200 || (s_compiled + s_hits) % 100 == 0)
        fprintf(stderr, "[PSCACHE] pixel shader %s in %.1f ms; this run: %u compiled in game (%.0f ms),"
                        " %u from the cache (%.1f ms), %u stored\n",
                compiled ? "compiled" : "from the cache", ms, s_compiled, s_compile_ms, s_hits, s_hit_ms,
                s_stored);
}
