/* bw_visextra_hooks.c - Remastered visual extras on the game side (D-016):
 * D-011 wrappers that change what the game itself draws, never the art.
 * Agent T3, docs/day1/T3.md.
 *
 * Every wrapper here is sub_X calling the game's own generated body sub_X_gen.
 * In Original (or with its registry switch off) the wrapper calls sub_X_gen
 * and nothing else, so Original plays exactly as shipped. In Remastered (and
 * the editions built on it) a wrapper may adjust the body's inputs, or swap
 * one game value around the call and put it back at once (the D-016
 * amendment to D-011); it never re-implements the game's logic. Each change
 * is registered in src/bw_fixes.def and listed in docs/REMASTER_LOG.md.
 *
 * tools/run_pipeline.py hands this file to the generator together with
 * src/recomp_manual.c and the other bw_*_hooks.c (--exclude-manual), so every
 * function wrapped here is emitted as sub_X_gen and its callers reach the
 * wrapper. "#if 0" is the only conditional that scan understands.
 *
 * 1. SPLIT_FULL_DETAIL (on by default): split screen at single-player detail.
 *    - The sea: Ocean_Render 0x38EC0 picks a level-of-detail factor from the
 *      split-screen view count [0x1A9A78] (1 view 1.0, 2 views 2.0, 3-4 views
 *      4.0; seen at 0x3902F..0x39058) and passes it as the last argument of
 *      its one call to Ocean_CullQuadtree 0x38720 (return address 0x390C0).
 *      The quadtree walk subdivides a tile while (distance - size*k1 +
 *      (factor-1)*k2) / size is under a threshold and 2*factor < size (seen,
 *      0x38841..0x388A3), so a bigger factor means bigger, fewer tiles. The
 *      wrapper hands that call a smaller factor of the game's own (1.0 the
 *      one-view sea, or 2.0 its 2-player sea; BW_SPLIT_WATER below). The
 *      recursive calls pass the factor on unchanged (seen, 0x388A3), so the
 *      whole walk uses it. Nothing else reads the factor, and [0x1A9A78] is
 *      never changed.
 *    - Animations: Title_Boot calls AnimLib_CheckSplitScreen 0x26040 on the
 *      animation library 0x187B68, which sets its disabled flag +0x20 when
 *      [0x1E5D00] == 1 (the gameplay boot) and the player count [0x1E5F78] is
 *      above 2 (seen). With the flag set AnimLib_FindInstance and
 *      AnimLib_FindSetByName return 0 and AnimLib_Update and
 *      AnimLib_UpdateAttachments return at once (seen), so crews, gunners,
 *      turrets and props stand still with 3-4 players. The wrapper puts the
 *      flag back as it was before the call. AnimLib_Update 0x26090 has the
 *      same test again for its instance loop (0x260A3..0x260B3, seen); its
 *      wrapper shows the body a player count of 2 for the call and puts the
 *      real count back. The body and everything it calls read [0x1E5F78]
 *      only in that test (seen: a static walk of its 116 callees, and of the
 *      audio thread, the only other game thread running in play, finds no
 *      other read of it; tmp/T3_work/calltree.py).
 * 2. OCEAN_MIPALPHA_FIX (an original bug fixed: on by default in Remastered
 *    since 2026-10-27, the maintainer's call; launcher Fixes, "Distant wake
 *    foam": As shipped / Fixed / Smooth):
 *    Ocean_MergeFrameAlpha 0x38210 (the tail of Ocean_Init) writes each wake
 *    mask into the alpha of its ocean frame for levels 0-6, walking the
 *    swizzled level-0 mask with a stride and row skip written for a linear
 *    image, so levels 1-6 get a scrambled mask (seen in the code; docs/remaster
 *    texture_matching.md section 2). With the fix the body runs unchanged,
 *    then levels 1-6 of each frame's alpha are written again from its own
 *    level-0 alpha (which the body left correct), properly down-sampled:
 *      BW_OCEAN_MIPALPHA_FIX=1 (the default while the fix is on): the texel
 *          at the top-left of each block, the developers' own sampling (the
 *          stride and row skip read every 2^L-th texel of every 2^L-th row of
 *          a linear image), done in swizzled order;
 *      BW_OCEAN_MIPALPHA_FIX=2: the average of each block (a box filter; ours);
 *      BW_OCEAN_MIPALPHA_FIX=0: off, the shipped mips (as BW_FIX=-OCEAN_MIPALPHA_FIX).
 *    Unset (or any other value) follows the registry. The registry is the
 *    master switch: with the fix off (BW_FIX, or the hook check) no value
 *    turns it on. Original keeps the scramble.
 *
 * SPLIT_FULL_DETAIL's sea, BW_SPLIT_WATER (the launcher's "Split-screen
 * sea"): full (default since the native ocean, P2a: the one-player sea in
 * every view; with 4 players about 15% below balanced, was half), balanced
 * (2 players the one-player sea, 3-4 players the game's own 2-player sea;
 * for slower PCs), 0 (the console's).
 * BW_SPLIT_ANIMS=0 keeps the animations frozen (test aid; they cost nothing
 * measurable).
 *
 * Test aids (read-only; every edition, they never write guest memory):
 *   BW_SPLIT_LOG=1             a line every 5 s: ocean walks by factor, tiles
 *                              drawn per walk, animation updates let through
 *   BW_OCEAN_MIPALPHA_DUMP=<dir>  the ocean frames' alpha, every level, as
 *                              PGM images (unswizzled), before and after the
 *                              switch's rewrite (frames 0 and 15 only)
 *
 * Claims are labelled as in the docs: seen (in code, data or a run),
 * inferred, unknown. Addresses are the XBE's (base 0x00010000).
 */

#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "bw_edition.h"
#include "bw_guestcall.h"   /* the guest registers, recomp_lookup, calling the game (item 71) */

#define V_MEM32(va) (*(volatile uint32_t *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))
#define V_MEM16(va) (*(volatile uint16_t *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))
#define V_MEM8(va)  (*(volatile uint8_t  *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))
#define V_MEMF(va)  (*(volatile float    *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))
/* The runtime maps 128 MB of guest space; textures live in the contiguous
 * window 0x80000000+ (the same offset), as in bw_fix_hooks.c. */
#define V_OK(va)    (((uint32_t)(va) >= 0x00010000u && (uint32_t)(va) < 0x08000000u) || \
                     ((uint32_t)(va) >= 0x80010000u && (uint32_t)(va) < 0x84000000u))

static int env_int(const char *name, int dflt)
{
    const char *e = getenv(name);
    return (e && *e) ? atoi(e) : dflt;
}

/* ════════════════════════════════════════════════════════════════════════
 * 1. SPLIT_FULL_DETAIL
 * ════════════════════════════════════════════════════════════════════════ */
#define X_SPLIT_VIEWS     0x001A9A78u   /* split-screen view count (Ocean_Render reads it) */
#define X_GAMEPLAY_BOOT   0x001E5D00u   /* 1 on the gameplay boot (FV6, inferred meaning) */
#define X_PLAYER_COUNT    0x001E5F78u   /* launch record +0x30: players (FV2) */
#define X_ANIMLIB         0x00187B68u   /* the animation library (Title_Boot's ecx) */
#define ANIMLIB_DISABLED  0x20u
#define RA_OCEAN_CULL     0x000390C0u   /* Ocean_Render's call of Ocean_CullQuadtree */
#define OCEAN_TILES       0x94u         /* Ocean: tiles recorded by the walk (limit 400) */

static volatile long s_sp_walks[3];     /* top-level walks by factor 1 / 2 / 4+ */
static volatile long s_sp_tiles[3];     /* tiles they recorded */
static volatile long s_sp_lifted;       /* walks handed a smaller factor */
static volatile long s_sp_anim_kept;    /* CheckSplitScreen flags put back */
static volatile long s_sp_anim_runs;    /* AnimLib_Update calls let through */
static DWORD s_sp_last_log;

static int split_on(void)
{
    return bw_fix_enabled(BW_FIX_SPLIT_FULL_DETAIL);
}

/* Its two parts. The sea, BW_SPLIT_WATER (the launcher's "Split-screen
 * detail" sets it):
 *   full (or 1)   every view gets the one-player sea (factor 1);
 *   balanced (2)  2 players get the one-player sea, 3-4 players the game's
 *                 own 2-player sea (factor 2): with 4 players the one-player
 *                 sea costs about half the frame rate (T3.md section 1.4);
 *   0 / off       the console's sea.
 * Only the game's own factors are ever handed over. The animations,
 * BW_SPLIT_ANIMS=0 keeps them frozen (they cost nothing measurable). */
#define SPLIT_WATER_OFF       0
#define SPLIT_WATER_FULL      1
#define SPLIT_WATER_BALANCED  2
/* Full since the native ocean (lead, D-014, 2026-10-28): 4 players at 3440x1440
 * run 113 fps full vs 132 balanced (was 66 vs 132; P2a.md). BW_SPLIT_WATER=balanced
 * or the launcher's "Balanced" gives the old default. */
#define SPLIT_WATER_DEFAULT   SPLIT_WATER_FULL

static int split_water_mode(void)
{
    static int s = -1;
    if (s < 0) {
        const char *e = getenv("BW_SPLIT_WATER");
        if (!e || !*e)
            s = SPLIT_WATER_DEFAULT;
        else if (!_stricmp(e, "full") || !strcmp(e, "1"))
            s = SPLIT_WATER_FULL;
        else if (!_stricmp(e, "balanced") || !strcmp(e, "2"))
            s = SPLIT_WATER_BALANCED;
        else
            s = SPLIT_WATER_OFF;
    }
    return split_on() ? s : SPLIT_WATER_OFF;
}

/* The factor handed to Ocean_Render's walk when the game's own is larger. */
static float split_water_target(float game_factor)
{
    int m = split_water_mode();
    if (m == SPLIT_WATER_BALANCED && game_factor > 2.0f)
        return 2.0f;
    return m == SPLIT_WATER_OFF ? game_factor : 1.0f;
}

static int split_part(const char *name)
{
    return env_int(name, 1) != 0;
}

static int split_anims(void)
{
    static int s = -1;
    if (s < 0)
        s = split_part("BW_SPLIT_ANIMS");
    return split_on() && s;
}

static int split_log(void)
{
    static int s = -1;
    if (s < 0)
        s = env_int("BW_SPLIT_LOG", 0) != 0;
    return s;
}

static void split_report(const char *why)
{
    fprintf(stderr, "[T3] split %s (%s, fix %s): views %u players %u; ocean walks by factor "
            "1/2/4: %ld/%ld/%ld, tiles %ld/%ld/%ld, handed a smaller factor: %ld; anim flag kept %ld, "
            "anim updates let through %ld\n",
            why, bw_edition_name(bw_edition()), split_on() ? "on" : "off",
            V_MEM32(X_SPLIT_VIEWS), V_MEM32(X_PLAYER_COUNT),
            s_sp_walks[0], s_sp_walks[1], s_sp_walks[2],
            s_sp_tiles[0], s_sp_tiles[1], s_sp_tiles[2], s_sp_lifted,
            s_sp_anim_kept, s_sp_anim_runs);
    fflush(stderr);
}

static void split_summary(void)
{
    split_report("summary");
}

/* Ocean_CullQuadtree 0x00038720 (seen): thiscall, ecx = the ocean (0x1C6990),
 * five stack arguments (centre x, y, z, half size, LOD factor), ret 0x14; it
 * calls itself for the four children with the same factor. */
extern void sub_00038720_gen(void);
static void ocean_cull(void)
{
    uint32_t ocean, ra;
    float f;
    int k;
    if (!split_on() && !split_log()) {
        sub_00038720_gen();
        return;
    }
    ra = V_MEM32(g_esp);
    if (ra != RA_OCEAN_CULL) {                  /* a recursive call: as it comes */
        sub_00038720_gen();
        return;
    }
    ocean = g_ecx;
    f = V_MEMF(g_esp + 0x14);
    k = f > 3.0f ? 2 : (f > 1.5f ? 1 : 0);
    if (f > split_water_target(f)) {
        V_MEMF(g_esp + 0x14) = split_water_target(f);  /* the call's own argument; the body pops it */
        InterlockedIncrement(&s_sp_lifted);
        bw_fix_note_hit(BW_FIX_SPLIT_FULL_DETAIL);
    }
    sub_00038720_gen();
    if (split_log()) {
        static int s_reg;
        DWORD now = GetTickCount();
        InterlockedIncrement(&s_sp_walks[k]);
        InterlockedExchangeAdd(&s_sp_tiles[k], (long)V_MEM32(ocean + OCEAN_TILES));
        if (!s_reg) {
            s_reg = 1;
            s_sp_last_log = now;
            bw_edition_add_summary(split_summary);
        } else if (now - s_sp_last_log >= 5000u) {
            s_sp_last_log = now;
            split_report("5 s");
        }
    }
}

/* HIFPS_EYE (bw_hifps_hooks.c): Ocean_Render copies the eye to the ocean's
 * +0x30 just before this walk; the walk picks the tiles by it, and the pass
 * constants (c[146] in Ocean_SetPass1Constants and _SetPass2Constants) are made
 * from it after. So the outermost walk runs with the step's eye, as before,
 * and the drawn eye is back for the constants. */
int bw_hifps_eye_ocean(uint32_t ocean, int to_step);
void sub_00038720(void)
{
    uint32_t ocean = g_ecx;
    if (V_MEM32(g_esp) == RA_OCEAN_CULL && bw_hifps_eye_ocean(ocean, 1)) {
        ocean_cull();
        bw_hifps_eye_ocean(ocean, 0);
        return;
    }
    ocean_cull();
}

/* AnimLib_CheckSplitScreen 0x00026040 (seen): thiscall on the library, no
 * arguments, plain ret; writes only +0x20. */
extern void sub_00026040_gen(void);
void sub_00026040(void)
{
    uint32_t lib = g_ecx;
    uint8_t before;
    if (!split_anims()) {
        sub_00026040_gen();
        return;
    }
    before = V_MEM8(lib + ANIMLIB_DISABLED);
    sub_00026040_gen();
    if (V_MEM8(lib + ANIMLIB_DISABLED) != before) {
        V_MEM8(lib + ANIMLIB_DISABLED) = before;
        InterlockedIncrement(&s_sp_anim_kept);
        bw_fix_note_hit(BW_FIX_SPLIT_FULL_DETAIL);
    }
}

/* AnimLib_Update 0x00026090 (seen): thiscall on the library, [esp+4] = dt,
 * ret 4. Skips its instance loop when [0x1E5D00] == 1 and [0x1E5F78] > 2. */
extern void sub_00026090_gen(void);
void sub_00026090(void)
{
    uint32_t players;
    if (!split_anims() || V_MEM32(X_GAMEPLAY_BOOT) != 1u
        || (int32_t)(players = V_MEM32(X_PLAYER_COUNT)) <= 2) {
        sub_00026090_gen();
        return;
    }
    V_MEM32(X_PLAYER_COUNT) = 2u;
    sub_00026090_gen();
    V_MEM32(X_PLAYER_COUNT) = players;
    InterlockedIncrement(&s_sp_anim_runs);
}

/* ════════════════════════════════════════════════════════════════════════
 * 2. OCEAN_MIPALPHA_FIX
 * ════════════════════════════════════════════════════════════════════════
 * Layout (seen in Ocean_MergeFrameAlpha 0x38210..0x38353): PakSet_GetSection
 * 0x91F30 (ecx = the pak set 0x1D5C68; 0x21, 0x19) gives the water section;
 * its records are 0x38 bytes from +0x10: +0 the D3D texture, +0xC width,
 * +0xE height (16-bit). Records 0-29 are the ocean frames (swizzled
 * A8R8G8B8, alpha in byte 3), 30-59 the wake masks (LIN_L8). The body locks
 * each level with D3DTexture_LockRect 0x128630 (texture, level, &locked,
 * NULL, 0; stdcall; pBits at locked+4) and writes levels 0..6 while
 * height >> level is not 0. */
#define X_PAKSET          0x001D5C68u
#define VA_GETSECTION     0x00091F30u
#define VA_LOCKRECT       0x00128630u
#define VA_LEVELCOUNT     0x00128780u   /* D3DBaseTexture_GetLevelCount (texture), stdcall */
#define WATER_FRAMES      30u
#define WATER_REC         0x38u

/* Calling a game function: bw_guestcall.h (shared with bw_fix_hooks.c;
 * item 71). */
typedef bw_gregs_t v_regs_t;
#define v_save    bw_gregs_save
#define v_restore bw_gregs_restore

static uint32_t v_call(uint32_t va, uint32_t this_ptr, int nargs, const uint32_t *args)
{
    return bw_guest_call("[T3]", va, this_ptr, nargs, args);
}

/* pBits of one level, or 0. The D3DLOCKED_RECT goes on the guest stack below
 * esp (free space: the wrapper's caller is above it). */
static uint32_t v_lock(uint32_t tex, uint32_t level)
{
    uint32_t lr, args[5];
    g_esp -= 0x20;
    lr = g_esp + 0x10;
    V_MEM32(lr) = 0;
    V_MEM32(lr + 4) = 0;
    args[0] = tex; args[1] = level; args[2] = lr; args[3] = 0; args[4] = 0;
    v_call(VA_LOCKRECT, g_ecx, 5, args);
    g_esp += 0x20;
    return V_MEM32(lr + 4);
}

/* The Xbox swizzle (Morton order; x and y take the index bits in turn, x
 * first, while each has bits left: xboxrecomp d3d8_swizzle.h, xemu). */
static uint32_t swz(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    uint32_t off = 0, bit = 0, s;
    for (s = 1; s < w || s < h; s <<= 1) {
        if (s < w) {
            if (x & s)
                off |= 1u << bit;
            bit++;
        }
        if (s < h) {
            if (y & s)
                off |= 1u << bit;
            bit++;
        }
    }
    return off;
}

static int is_pow2(uint32_t v)
{
    return v && !(v & (v - 1));
}

static void pgm_dump(const char *dir, const char *tag, uint32_t frame, uint32_t level,
                     const uint8_t *img, uint32_t w, uint32_t h)
{
    char path[MAX_PATH];
    FILE *f;
    _snprintf(path, sizeof path, "%s\\ocean%02u_L%u_%s.pgm", dir, frame, level, tag);
    path[sizeof path - 1] = 0;
    f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P5\n%u %u\n255\n", w, h);
    fwrite(img, 1, (size_t)w * h, f);
    fclose(f);
}

/* Unswizzled alpha of one level into img (w*h bytes). */
static void read_alpha(uint32_t bits, uint32_t w, uint32_t h, uint8_t *img)
{
    uint32_t x, y;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            img[y * w + x] = V_MEM8(bits + 4u * swz(x, y, w, h) + 3u);
}

static void mipalpha_rebuild(int mode)
{
    const char *dump = getenv("BW_OCEAN_MIPALPHA_DUMP");
    uint32_t args[2], sec, i;
    long frames = 0, levels = 0, skipped = 0;
    args[0] = 0x21; args[1] = 0x19;
    sec = v_call(VA_GETSECTION, X_PAKSET, 2, args);
    if (!V_OK(sec)) {
        fprintf(stderr, "[T3] mipalpha: no water section (0x%08X); nothing changed\n", sec);
        fflush(stderr);
        return;
    }
    for (i = 0; i < WATER_FRAMES; i++) {
        uint32_t rec = sec + 0x10u + i * WATER_REC, wrec = rec + WATER_FRAMES * WATER_REC;
        uint32_t tex = V_MEM32(rec), w = V_MEM16(rec + 0xC), h = V_MEM16(rec + 0xE);
        uint32_t ww = V_MEM16(wrec + 0xC), wh = V_MEM16(wrec + 0xE);
        uint32_t bits0, L, nlev;
        uint8_t *base;
        int want_dump = dump && *dump && (i == 0 || i == 15);
        /* Only the layout the body itself handles correctly at level 0: frame
         * and mask the same power-of-two size. Anything else is left alone. */
        if (!V_OK(tex) || !is_pow2(w) || !is_pow2(h) || w != ww || h != wh || w > 1024u || h > 1024u
            || !V_OK(bits0 = v_lock(tex, 0))) {
            skipped++;
            continue;
        }
        base = (uint8_t *)malloc((size_t)w * h);
        if (!base) {
            skipped++;
            continue;
        }
        read_alpha(bits0, w, h, base);
        if (want_dump)
            pgm_dump(dump, "level0", i, 0, base, w, h);
        nlev = v_call(VA_LEVELCOUNT, g_ecx, 1, &tex);
        for (L = 1; L < 7u; L++) {
            uint32_t wl = w >> L, hl = h >> L, bits, x, y, n = 1u << L;
            uint8_t *img;
            if (!hl || !wl || L >= nlev)  /* the body's own bound: h >> L, and w >> L texels a row */
                break;
            bits = v_lock(tex, L);
            if (!V_OK(bits))
                break;
            img = (uint8_t *)malloc((size_t)wl * hl);
            if (!img)
                break;
            if (want_dump) {
                read_alpha(bits, wl, hl, img);
                pgm_dump(dump, "before", i, L, img, wl, hl);
            }
            for (y = 0; y < hl; y++)
                for (x = 0; x < wl; x++) {
                    uint32_t v;
                    if (mode == 2) {
                        uint32_t sx, sy, sum = 0;
                        for (sy = 0; sy < n; sy++)
                            for (sx = 0; sx < n; sx++)
                                sum += base[(y * n + sy) * w + x * n + sx];
                        v = (sum + (n * n) / 2u) / (n * n);
                    } else {
                        v = base[(y * n) * w + x * n];
                    }
                    img[y * wl + x] = (uint8_t)v;
                    V_MEM8(bits + 4u * swz(x, y, wl, hl) + 3u) = (uint8_t)v;
                }
            if (want_dump)
                pgm_dump(dump, mode == 2 ? "box" : "point", i, L, img, wl, hl);
            free(img);
            levels++;
        }
        free(base);
        frames++;
    }
    fprintf(stderr, "[T3] mipalpha (%s): ocean frames' alpha levels 1-6 rebuilt from level 0 "
            "(%s): %ld frames, %ld levels, %ld skipped\n", bw_edition_name(bw_edition()),
            mode == 2 ? "box average" : "top-left texel, the developers' sampling",
            frames, levels, skipped);
    fflush(stderr);
}

/* 0 off, 1 the developers' sampling, 2 the box average (see the header).
 * Item 66 (MR2 #6): an explicit 0 used to fall back to the registry, so
 * "=0" still gave the fixed mips; and 1 or 2 turned it on even with the fix
 * off. */
static int mipalpha_mode(void)
{
    static int s = -1;
    if (s < 0) {
        int m = env_int("BW_OCEAN_MIPALPHA_FIX", -1);
        if (m != 0 && m != 1 && m != 2)
            m = 1;
        s = bw_edition_at_least(BW_ED_REMASTERED) && bw_fix_enabled(BW_FIX_OCEAN_MIPALPHA_FIX)
            ? m : 0;
    }
    return s;
}

/* A read-only look at the body's own result (BW_OCEAN_MIPALPHA_DUMP without
 * the switch, any edition): what Original writes into the mips. */
static void mipalpha_dump_only(void)
{
    const char *dump = getenv("BW_OCEAN_MIPALPHA_DUMP");
    uint32_t args[2], sec, i, L;
    if (!dump || !*dump)
        return;
    args[0] = 0x21; args[1] = 0x19;
    sec = v_call(VA_GETSECTION, X_PAKSET, 2, args);
    if (!V_OK(sec))
        return;
    for (i = 0; i < WATER_FRAMES; i += 15) {
        uint32_t rec = sec + 0x10u + i * WATER_REC;
        uint32_t tex = V_MEM32(rec), w = V_MEM16(rec + 0xC), h = V_MEM16(rec + 0xE);
        if (!V_OK(tex) || !is_pow2(w) || !is_pow2(h) || w > 1024u || h > 1024u)
            continue;
        for (L = 0; L < 7u && (h >> L) && (w >> L); L++) {
            uint32_t bits = v_lock(tex, L), wl = w >> L, hl = h >> L;
            uint8_t *img;
            if (!V_OK(bits) || !(img = (uint8_t *)malloc((size_t)wl * hl)))
                break;
            read_alpha(bits, wl, hl, img);
            pgm_dump(dump, "game", i, L, img, wl, hl);
            free(img);
        }
    }
    fprintf(stderr, "[T3] mipalpha: dumped the game's own ocean frame alpha to %s\n", dump);
    fflush(stderr);
}

/* Ocean_MergeFrameAlpha 0x00038210 (seen): no arguments, plain ret; reached
 * by Ocean_Init's tail jump (0x3870B). */
extern void sub_00038210_gen(void);
void sub_00038210(void)
{
    v_regs_t r;
    int mode = mipalpha_mode();
    if (!mode) {
        sub_00038210_gen();
        if (getenv("BW_OCEAN_MIPALPHA_DUMP")) {
            v_save(&r);
            mipalpha_dump_only();
            v_restore(&r);
        }
        return;
    }
    sub_00038210_gen();
    v_save(&r);
    mipalpha_rebuild(mode);
    v_restore(&r);
    bw_fix_note_hit(BW_FIX_OCEAN_MIPALPHA_FIX);
}
