/* bw_fix_hooks.c - D-011 wrappers that fix the original game's own bugs in
 * Remastered (and the editions built on it: D-015/D-016). Agent D11,
 * docs/day1/D11.md.
 *
 * Every wrapper here is sub_X calling the game's own generated body sub_X_gen.
 * With the edition switch on Original (or the fix turned off with
 * BW_FIX=-ID) the wrapper calls sub_X_gen and nothing else, so Original plays
 * exactly as shipped. In Remastered a fix may adjust the body's inputs or
 * result, or swap game state around the call and put it back afterwards (the
 * D-016 amendment to D-011); it never re-implements the game's logic. Each fix
 * is registered in src/bw_fixes.def and listed in docs/REMASTER_LOG.md.
 *
 * tools/run_pipeline.py hands this file to the generator with
 * src/recomp_manual.c (--exclude-manual), so every function wrapped here is
 * emitted as sub_X_gen and its callers reach the wrapper.
 *
 * Two kinds of test helpers live here too. Both do nothing unless their
 * environment variable is set, in every edition:
 *   BW_FIX_PROBE=1     read-only counters, printed with the [ED] summary: the
 *                      water impact sounds' decisions, wave names the sound
 *                      banks don't have. They call only Sound_IsPlaying (a
 *                      status query) and never write guest memory.
 *   BW_FIXTEST_MEET / BW_FIXTEST_SINK   test aids (like BW_SEED): force
 *                      named objectives met, or
 *                      zero the hitpoints of an objective's targets, at a
 *                      mission time, so a mission state can be reached
 *                      unattended. They write game state; never set them for
 *                      play.
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
#include <math.h>

#include "bw_edition.h"
#include "bw_guestcall.h"   /* the guest registers, recomp_lookup, calling the game (item 71) */

#define F_MEM32(va) (*(volatile uint32_t *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))
#define F_MEM16(va) (*(volatile uint16_t *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))
#define F_MEM8(va)  (*(volatile uint8_t  *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))
/* The runtime maps 128 MB of guest address space (main.c: "128 MB mapped (RAM
 * stays 64 MB)"), and the game's heap reaches above 64 MB: sound objects at
 * 0x0459B490 (seen: run -d11g12O3). */
#define F_OK(va)    ((uint32_t)(va) >= 0x00010000u && (uint32_t)(va) < 0x08000000u)
/* Render matrices live in the contiguous window 0x80000000+ (seen: run
 * -d11m10dbg, 0x8077CFA0..), which the runtime maps at the same offset. */
#define F_OKM(va)   (F_OK(va) || ((uint32_t)(va) >= 0x80010000u && (uint32_t)(va) < 0x84000000u))

static float f_f32(uint32_t va)
{
    float f;
    memcpy(&f, (const void *)((uintptr_t)va + g_xbox_mem_offset), 4);
    return f;
}

/* A guest string into a host buffer (always terminated). */
static const char *f_str(uint32_t va, char *buf, size_t n)
{
    size_t i;
    if (!F_OK(va)) {
        buf[0] = 0;
        return buf;
    }
    for (i = 0; i + 1 < n; i++) {
        char c = (char)F_MEM8(va + (uint32_t)i);
        if (!c)
            break;
        buf[i] = c;
    }
    buf[i] = 0;
    return buf;
}

static int f_streq(uint32_t va, const char *s)
{
    size_t i;
    if (!F_OK(va))
        return 0;
    for (i = 0;; i++) {
        char c = (char)F_MEM8(va + (uint32_t)i);
        if (c != s[i])
            return 0;
        if (!c)
            return 1;
    }
}

/* ── Calling the game's own functions from a wrapper: bw_guestcall.h
 * (shared with bw_visextra_hooks.c; item 71). */
typedef bw_gregs_t f_regs_t;
#define f_save    bw_gregs_save
#define f_restore bw_gregs_restore

static uint32_t f_call(uint32_t va, uint32_t this_ptr, int nargs, const uint32_t *args)
{
    return bw_guest_call("[FIX]", va, this_ptr, nargs, args);
}

#define VA_SOUND_ISPLAYING   0x000182C0u  /* thiscall (ecx = sound), al = playing */
#define VA_GAMEOBJ_DESTROYED 0x0008B240u  /* thiscall (ecx = object), al = destroyed */
#define VA_NODE_WORLDMATRIX  0x00030F40u  /* thiscall (ecx = render item), eax = matrix */

/* BW_FIX_PROBE=1: read once. */
static int f_probe(void)
{
    static int p = -1;
    if (p < 0) {
        const char *e = getenv("BW_FIX_PROBE");
        p = (e && *e && *e != '0') ? 1 : 0;
    }
    return p;
}

/* ════════════════════════════════════════════════════════════════════════
 * BOAT_IMPACT_SOUND_SELF: the hull impact sounds test their own sound
 * ════════════════════════════════════════════════════════════════════════
 *
 * The boat-sound object (BoatSound_Init 0x11BF0) holds four sounds of each
 * kind: WaterLap1-4 at +0x5C, WaterSmack1-4 at +0x6C, WaterSplash1-4 at
 * +0x7C, WaterPlunge1-4 at +0x8C (names/bw_names.json, B1). Each Play
 * function picks an index i at random (never the last one, +0xD8/+0xF8/
 * +0xFC/+0x100) and then (seen, 2026-10-27, disassembly):
 *   BoatSound_PlayWaterLap    0x12AC0: null test +0x5C[i], Sound_IsPlaying(+0x5C[i]), play +0x5C[i]
 *   BoatSound_PlayWaterSmack  0x12C40: null test +0x6C[i], Sound_IsPlaying(+0x5C[i]), play +0x6C[i]
 *   BoatSound_PlayWaterSplash 0x12D30: null test +0x7C[i], Sound_IsPlaying(+0x5C[i]), play +0x7C[i]
 *   BoatSound_PlayWaterPlunge 0x12E40: null test +0x8C[i], Sound_IsPlaying(+0x5C[i]), play +0x8C[i]
 * So a smack, splash or plunge is skipped while the water-lap loop with the
 * same random index plays, and is restarted over itself while it is still
 * playing. The intent is seen in the code: each function null-tests and plays
 * its own slot, the lap function tests the slot it plays, and an index-for-
 * index coupling between two unrelated sound families can't be a design (a
 * deliberate "no impacts over the lap" would test every lap sound). The three
 * share the slip because they were copied from PlayWaterLap (inferred).
 *
 * The fix: for the duration of the body, the four +0x5C slots hold the
 * function's own four sounds, then they are put back. The body reads +0x5C
 * only at that one test (seen), and nothing it calls (rand, ftol, the speed
 * getter 0x6B680, Sound_SetVolume/Play/SetFrequency) reads the boat-sound
 * object, so the swap changes exactly the sound that one test looks at.
 *
 * Hits: an impact that Original would have dropped because the lap sound with
 * its index was playing. BW_FIX_PROBE adds the full picture in both editions.
 */
#define BS_LAP 0x5Cu

typedef struct {
    const char   *name;
    uint32_t      own;        /* the function's own four sounds */
    uint32_t      last;       /* its last-index field */
    volatile long reached;    /* calls that got past the cooldown and picked i */
    volatile long played_ok;  /* ... whose own sound i exists */
    volatile long lap_on;     /* ... while lap[i] played (Original skips these) */
    volatile long own_on;     /* ... while own[i] played (Remastered skips these) */
} f_impact_t;

static f_impact_t s_imp[3] = {
    { "WaterSmack",  0x6Cu, 0xF8u },
    { "WaterSplash", 0x7Cu, 0xFCu },
    { "WaterPlunge", 0x8Cu, 0x100u },
};

static void impact_summary(void)
{
    int k;
    for (k = 0; k < 3; k++)
        fprintf(stderr, "[FIX] probe %s (%s, BOAT_IMPACT_SOUND_SELF %s): %ld picked, %ld with a "
                "sound, %ld while lap[i] played (Original skips), %ld while the same sound "
                "played (Remastered skips)\n", s_imp[k].name, bw_edition_name(bw_edition()),
                bw_fix_enabled(BW_FIX_BOAT_IMPACT_SOUND_SELF) ? "on" : "off", s_imp[k].reached,
                s_imp[k].played_ok, s_imp[k].lap_on, s_imp[k].own_on);
}

static int f_playing(uint32_t snd)
{
    return snd ? (int)(f_call(VA_SOUND_ISPLAYING, snd, 0, NULL) & 0xFFu) : 0;
}

static void impact_hook(int k, void (*gen)(void))
{
    f_impact_t *im = &s_imp[k];
    uint32_t bs = g_ecx, save[4], i;
    int fix = bw_fix_enabled(BW_FIX_BOAT_IMPACT_SOUND_SELF) && F_OK(bs);
    int probe = f_probe() && F_OK(bs);
    unsigned char lap_on[4] = { 0 }, own_on[4] = { 0 };
    uint32_t j, air = 0;
    float fx = 0.0f, fy = 0.0f, fz = 0.0f;
    int have_m = 0;

    if (!fix && !probe) {
        gen();
        return;
    }
    if (probe) {
        f_regs_t r;
        uint32_t item, node, m;
        static volatile long once;
        static ULONGLONG s_next_print;
        ULONGLONG now = GetTickCount64();
        if (!InterlockedExchange(&once, 1)) {
            bw_edition_add_summary(impact_summary);
            s_next_print = now + 15000u;
        }
        if (now >= s_next_print) {          /* running totals: no exit hook needed */
            s_next_print = now + 15000u;
            impact_summary();
            fflush(stderr);
        }
        f_save(&r);
        for (j = 0; j < 4; j++) {
            lap_on[j] = (unsigned char)f_playing(F_MEM32(bs + BS_LAP + 4u * j));
            own_on[j] = (unsigned char)f_playing(F_MEM32(bs + im->own + 4u * j));
        }
        /* Landing readout (F2): BoatSound_UpdateImpacts 0x13210 counts the
         * air time in ms at +0xE0 and calls Smack (< 400 ms) or, through
         * BoatSound_ShouldPlaySplash 0x13180, Splash or Plunge before it
         * resets it (seen). That test's v is the length of the horizontal
         * part of the boat's forward axis (world matrix row +0x10), cos of
         * the pitch (seen); fz > 0 is nose up (inferred: +z is up). */
        air = F_MEM32(bs + 0xE0);
        item = F_MEM32(bs + 0x28);
        node = F_OK(item) ? F_MEM32(item + 4) : 0;
        m = F_OK(node) ? f_call(VA_NODE_WORLDMATRIX, node, 0, NULL) : 0;
        if (F_OKM(m)) {
            fx = f_f32(m + 0x10);
            fy = f_f32(m + 0x14);
            fz = f_f32(m + 0x18);
            have_m = 1;
        }
        f_restore(&r);
    }
    if (fix)
        for (j = 0; j < 4; j++) {
            save[j] = F_MEM32(bs + BS_LAP + 4u * j);
            F_MEM32(bs + BS_LAP + 4u * j) = F_MEM32(bs + im->own + 4u * j);
        }
    gen();
    if (fix)
        for (j = 0; j < 4; j++)
            F_MEM32(bs + BS_LAP + 4u * j) = save[j];

    if (probe && have_m) {
        static volatile long s_land_lines;
        if (InterlockedIncrement(&s_land_lines) <= 400) {
            float hv = sqrtf(fx * fx + fy * fy);
            uint32_t pick = F_MEM32(bs + im->last), eax0 = g_eax;
            int now_playing = -1, spray = 0, laps = 0;
            double wav_s = -1.0;
            f_regs_t r2;
            const char *wav = getenv("RECOMP_AUDIO_WAV");
            WIN32_FILE_ATTRIBUTE_DATA fa;
            /* Is the picked sound playing now (it can't without a buffer)?
             * And how many spray and lap sounds play. Sound_IsPlaying only. */
            f_save(&r2);
            if ((eax0 & 0xFFu) && pick <= 3u)
                now_playing = f_playing(F_MEM32(bs + im->own + 4u * pick));
            for (j = 0; j < 4; j++) {
                spray += f_playing(F_MEM32(bs + 0x4Cu + 4u * j));
                laps += f_playing(F_MEM32(bs + BS_LAP + 4u * j));
            }
            f_restore(&r2);
            /* Where this is in the run's recording: the recorder appends
             * 48 kHz s16 stereo after a 44-byte header (apu_core.c). */
            if (wav && GetFileAttributesExA(wav, GetFileExInfoStandard, &fa) && fa.nFileSizeLow > 44)
                wav_s = (double)(fa.nFileSizeLow - 44u) / (48000.0 * 4.0);
            fprintf(stderr, "[FIX] probe landing (%s): %s air %u ms v %.3f pitch %+.1f deg %s; "
                    "sound playing %s; spray %d, lap %d playing; wav %.2f s\n",
                    bw_edition_name(bw_edition()), im->name, air, hv, atan2f(fz, hv) * 57.29578f,
                    (eax0 & 0xFFu) ? "picked" : "skipped (cooldown/speed)",
                    now_playing < 0 ? "-" : (now_playing ? "yes" : "NO"), spray, laps, wav_s);
        }
    }
    if (!(g_eax & 0xFFu))                    /* cooldown or speed gate: nothing picked */
        return;
    i = F_MEM32(bs + im->last);
    if (i > 3u || !F_MEM32(bs + im->own + 4u * i))
        return;
    if (probe) {
        InterlockedIncrement(&im->reached);
        InterlockedIncrement(&im->played_ok);
        if (lap_on[i])
            InterlockedIncrement(&im->lap_on);
        if (own_on[i])
            InterlockedIncrement(&im->own_on);
    }
    if (fix) {
        int lap;
        if (probe) {
            lap = lap_on[i];
        } else {
            f_regs_t r;
            f_save(&r);
            lap = f_playing(F_MEM32(bs + BS_LAP + 4u * i));   /* the body never touches lap[i] */
            f_restore(&r);
        }
        if (lap)
            bw_fix_note_hit(BW_FIX_BOAT_IMPACT_SOUND_SELF);
    }
}

extern void sub_00012C40_gen(void);
extern void sub_00012D30_gen(void);
extern void sub_00012E40_gen(void);

void sub_00012C40(void)            /* BoatSound_PlayWaterSmack */
{
    impact_hook(0, sub_00012C40_gen);
}

void sub_00012D30(void)            /* BoatSound_PlayWaterSplash */
{
    impact_hook(1, sub_00012D30_gen);
}

void sub_00012E40(void)            /* BoatSound_PlayWaterPlunge */
{
    impact_hook(2, sub_00012E40_gen);
}

/* ════════════════════════════════════════════════════════════════════════
 * Probe: wave names the sound banks don't have (BW_FIX_PROBE)
 * ════════════════════════════════════════════════════════════════════════
 * Audio_FindWave 0x15260 (seen): thiscall, one argument (the name), ret 4,
 * eax = the wave record or 0. Read-only: logs each name not found, once. This
 * is how SOUND_SHIELDHITS_NAME is checked (Original misses
 * "ShieldHitsDreadnought"; Remastered finds it). */
extern void sub_00015260_gen(void);

void sub_00015260(void)
{
    uint32_t name;
    if (!f_probe()) {
        sub_00015260_gen();
        return;
    }
    name = F_MEM32(g_esp + 4);
    sub_00015260_gen();
    if (!g_eax) {
        static char seen[64][40];
        static int nseen;
        char buf[40];
        int k;
        f_str(name, buf, sizeof buf);
        for (k = 0; k < nseen; k++)
            if (!strcmp(seen[k], buf))
                return;
        if (nseen < 64)
            strcpy(seen[nseen++], buf);
        fprintf(stderr, "[FIX] probe Audio_FindWave (%s): no wave named '%s'\n",
                bw_edition_name(bw_edition()), buf);
        fflush(stderr);
    }
}

/* ════════════════════════════════════════════════════════════════════════
 * AUTOCANNON_HIT_SOUND: no silent autocannon hits
 * ════════════════════════════════════════════════════════════════════════
 *
 * Audio_WeaponSoundsInit 0x647C0 (seen) gives each turret three hit/explosion
 * sounds at +0xC8/+0xCC/+0xD0, named "<weapon>_exp1..3". For the autocannon
 * (weapon type 7, "auto_exp%d") the banks have auto_exp1 and auto_exp2 only
 * (the probe above: "no wave named 'auto_exp3'"; tools/bw_sound.py), and
 * Sound_CreateFromWave 0x18760 returns at once for a missing wave, so the
 * third sound object never gets a buffer (+8 stays 0). Sfx_OnProjectileHitBoat
 * (3D 0x8A72C, the player's own boat 2D 0x8A800) and
 * Sfx_OnProjectileHitStructure (0x89B56) pick it with rand() % 3, so one
 * autocannon hit in three on a boat or a building plays nothing; the world,
 * prop and explosion paths pick rand() & 1 and never reach it (seen). Every
 * path plays a sound for every hit, and the init asks for three, so a third
 * recording was planned and never made (inferred); a silent hit is not the
 * design (seen: no branch skips the sound on purpose).
 *
 * The fix: when Audio_PlayObjectSound3D 0x14710 (n, emitter, obj; ret 0xC) or
 * Audio_PlayObjectSound2D 0x14790 (n, obj; ret 8) is asked for a sound that
 * has no buffer, its argument n becomes one of the lower variants that has
 * one, alternating, so the game's own recordings play and nothing is added.
 * The alternation uses our own counter, never the game's rand(), so the
 * game's random sequence is untouched. Only auto_exp3 is affected in the
 * shipped data (the other missing variant, rock_exp3, is never asked for). */
static volatile long s_objs_calls, s_objs_silent, s_objs_fixed;
/* BW_FIX_PROBE: per weapon type (turret definition +0xA8, as Sfx_OnProjectileHit* reads it) and slot */
static volatile long s_objs_bytype[16][3], s_objs_emptytype[16][3];
static unsigned s_objs_alt;

static void objsound_summary(void)
{
    fprintf(stderr, "[FIX] probe object sounds (%s, AUTOCANNON_HIT_SOUND %s): %ld played, %ld "
            "asked for a sound with no recording, %ld of those given another variant\n",
            bw_edition_name(bw_edition()), bw_fix_enabled(BW_FIX_AUTOCANNON_HIT_SOUND) ? "on" : "off",
            s_objs_calls, s_objs_silent, s_objs_fixed);
    {
        int ty, k;
        for (ty = 0; ty < 16; ty++)
            if (s_objs_bytype[ty][0] + s_objs_bytype[ty][1] + s_objs_bytype[ty][2])
                for (k = 0; k < 3; k++)
                    fprintf(stderr, "[FIX] probe object sounds: weapon type %d slot %d: %ld played "
                            "(%ld without a recording)\n", ty, k, s_objs_bytype[ty][k],
                            s_objs_emptytype[ty][k]);
    }
}

static void objsound_fix(uint32_t arg_n, uint32_t obj)
{
    int fix = bw_fix_enabled(BW_FIX_AUTOCANNON_HIT_SOUND), probe = f_probe();
    uint32_t n, s, k, good[4], ng = 0;
    if ((!fix && !probe) || !F_OK(obj))
        return;
    n = F_MEM32(arg_n);
    if (n > 2u)
        return;                          /* three slots per turret (seen) */
    s = F_MEM32(obj + 0xC8u + 4u * n);
    if (probe) {
        static volatile long once;
        static ULONGLONG s_next;
        ULONGLONG now = GetTickCount64();
        if (!InterlockedExchange(&once, 1)) {
            bw_edition_add_summary(objsound_summary);
            s_next = now + 15000u;
        }
        if (InterlockedIncrement(&s_objs_calls) == 1) {
            fprintf(stderr, "[FIX] probe object sounds: first sound object at %08X\n", s);
            fflush(stderr);
        }
        if (F_OKM(s) && !F_MEM32(s + 8))
            InterlockedIncrement(&s_objs_silent);
        {
            uint32_t def = F_MEM32(obj + 0x54), ty = F_OK(def) ? F_MEM32(def + 0xA8) : 15u;
            if (ty > 15u)
                ty = 15u;
            InterlockedIncrement(&s_objs_bytype[ty][n]);
            if (!F_OKM(s) || !F_MEM32(s + 8))
                InterlockedIncrement(&s_objs_emptytype[ty][n]);
        }
        if (now >= s_next) {
            s_next = now + 15000u;
            objsound_summary();
            fflush(stderr);
        }
    }
    if (!fix || n == 0 || !F_OKM(s) || F_MEM32(s + 8))
        return;
    for (k = 0; k < n; k++) {
        uint32_t t = F_MEM32(obj + 0xC8u + 4u * k);
        if (F_OKM(t) && F_MEM32(t + 8))
            good[ng++] = k;
    }
    if (!ng)
        return;
    F_MEM32(arg_n) = good[s_objs_alt++ % ng];
    InterlockedIncrement(&s_objs_fixed);
    bw_fix_note_hit(BW_FIX_AUTOCANNON_HIT_SOUND);
}

extern void sub_00014710_gen(void);
extern void sub_00014790_gen(void);

void sub_00014710(void)            /* Audio_PlayObjectSound3D(n, emitter, obj) */
{
    objsound_fix(g_esp + 4, F_MEM32(g_esp + 0xC));
    sub_00014710_gen();
}

void sub_00014790(void)            /* Audio_PlayObjectSound2D(n, obj) */
{
    objsound_fix(g_esp + 4, F_MEM32(g_esp + 8));
    sub_00014790_gen();
}

/* ════════════════════════════════════════════════════════════════════════
 * VOICE_PLACEHOLDER_BORROW: voice lines whose recording is an empty
 * placeholder in this mission's pack play the same-named recording
 * (agent F2, docs/day1/F2.md)
 * ════════════════════════════════════════════════════════════════════════
 *
 * Pak_TokenSound 0x93110 hands each 44-byte Sound entry (name[32], offset,
 * size, kind) to Audio_LoadPakItem 0x14FA0 (seen): thiscall, ecx = the audio
 * manager 0x1A4358, arguments (data, size, kind, entry), ret 0x10. The body
 * returns at once for size 0 (`test edx, edx` at 0x14FAD), so an empty item
 * never becomes a wave record, Audio_FindWave fails, and SpotterVO_Play
 * 0x8F9D0 plays nothing while the subtitle stays up for the record's 3.0 s
 * (docs/formats/sound.md 3, 7).
 *
 * The shipped data (seen, 2026-10-27, every mission's voice pack against its
 * own mission records): 15 empty items in six missions, and every one of them
 * is referenced by its mission: M02 01G02 (mission 1's "Don't push me boy,
 * you will regret it!"); M07 the five edge lines 99G098-100, 99G102, 99G103;
 * M08 99G099, 99G102, 99G103; M10 the friendly-fire lines 99G105, 99G106,
 * 99G108, 99G109; M14 99G103; M19 99G100. Each name has a recording in other
 * missions' packs (6 to 23 of them), byte-identical in every pack, and each
 * name is paired with one subtitle in all 28 missions (the same text id in the
 * placeholder's mission and in every mission that plays the recording). So
 * the line and its subtitle are the intent, seen in the data; the empty item
 * is a packaging slip (inferred: the generic lines were packed per mission).
 *
 * The fix: when the body is given an empty wave item (kind 11, size 0), the
 * same-named recording is read from the first mission voice pack that has one
 * (missions/NN_voice_english.pak, NN 01..28, on the disc; the voice packs are
 * stored, not compressed: seen, all 29), staged in a buffer from the game's
 * own allocator (Mem_AllocZeroed 0xAB6C0), and the body runs with that data
 * and size; Audio_ParseWav 0x17DC0 copies the data into its own allocation
 * (seen: alloc into record +0x24, rep movsd), so the buffer is freed
 * (Mem_Free 0xAB6E0) right after. The pak in memory, its entry and the files
 * stay as shipped. A name with no recording anywhere (none ships) stays
 * silent. Only the game's own recordings play; nothing new is added. */
#define VA_MEM_ALLOC 0x000AB6C0u        /* cdecl (size), eax = zeroed block */
#define VA_MEM_FREE  0x000AB6E0u        /* cdecl (ptr) */

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* The bytes of the first non-empty wave item called `name` in the mission
 * voice packs, or NULL. *pak_out is the pack's mission number. */
static uint8_t *voice_donor(const char *name, uint32_t *size_out, int *pak_out)
{
    const char *dir = getenv("BW_GAME_DIR");      /* as main.c finds the disc */
    int nn;
    if (!dir || !*dir)
        dir = "game\\disc";
    for (nn = 1; nn <= 28; nn++) {
        char path[MAX_PATH];
        uint8_t hdr[112], *tab = NULL, *data = NULL;
        uint32_t unp, pk, tsize, i, start, snd = 0, cnt, k;
        FILE *f;
        _snprintf(path, sizeof path, "%s\\missions\\%02d_voice_english.pak", dir, nn);
        path[sizeof path - 1] = 0;
        f = fopen(path, "rb");
        if (!f)
            continue;
        if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr || memcmp(hdr, "spak", 4))
            goto next;
        unp = rd32(hdr + 8);
        pk = rd32(hdr + 16);
        if (unp != pk || unp < 64)
            goto next;                            /* compressed: no voice pack is (seen) */
        /* The section table (tools/bw_pak.py sections()): u32 table size, then
         * name[12] + u32 end (counted from that dword) per section; the first
         * section starts at the table's end, the last one has no end dword. */
        {
            uint8_t t4[4];
            if (fread(t4, 1, 4, f) != 4)
                goto next;
            tsize = rd32(t4);
        }
        if (tsize < 16 || tsize > 4096 || tsize > unp || (tsize - 4) % 16 != 12)
            goto next;
        tab = (uint8_t *)malloc(tsize);
        if (!tab || fseek(f, 112, SEEK_SET) || fread(tab, 1, tsize, f) != tsize)
            goto next;
        start = tsize;
        for (i = 4; i < tsize; i += 16) {
            uint32_t end = (i + 16 <= tsize) ? i + 12 + rd32(tab + i + 12) : unp;
            if (!strncmp((const char *)tab + i, "Sound", 12)) {
                snd = start;
                break;
            }
            start = end;
        }
        if (!snd || snd + 16 > unp)
            goto next;
        {
            uint8_t ch[16];
            uint8_t *ents;
            if (fseek(f, (long)(112 + snd), SEEK_SET) || fread(ch, 1, 16, f) != 16 || memcmp(ch, "CHNK", 4))
                goto next;
            cnt = (uint32_t)ch[12] | ((uint32_t)ch[13] << 8);
            if (!cnt || cnt > 1024 || snd + 16 + cnt * 44u > unp)
                goto next;
            ents = (uint8_t *)malloc(cnt * 44u);
            if (!ents || fread(ents, 1, cnt * 44u, f) != cnt * 44u) {
                free(ents);
                goto next;
            }
            for (k = 0; k < cnt; k++) {
                const uint8_t *e = ents + k * 44u;
                uint32_t off = rd32(e + 32), size = rd32(e + 36), kind = rd32(e + 40);
                uint32_t at = snd + 16 + cnt * 44u + off;
                if (kind != 11u || !size || strncmp((const char *)e, name, 32))
                    continue;
                if (size > (4u << 20) || at + size > unp)
                    break;
                data = (uint8_t *)malloc(size);
                if (!data || fseek(f, (long)(112 + at), SEEK_SET) || fread(data, 1, size, f) != size
                    || memcmp(data, "RIFF", 4)) {
                    free(data);
                    data = NULL;
                    break;
                }
                *size_out = size;
                *pak_out = nn;
                break;
            }
            free(ents);
        }
    next:
        free(tab);
        fclose(f);
        if (data)
            return data;
    }
    return NULL;
}

extern void sub_00014FA0_gen(void);

/* ════════════════════════════════════════════════════════════════════════
 * HEADPHONES_AT_BOOT: a saved Sound Mode of Headphones reaches the chip
 * ════════════════════════════════════════════════════════════════════════
 *
 * Seen (docs/audio/trace.md, the au-trace follow-up): Audio_Init 0x17500
 * passes the saved mode (0x1E7B2C) to IDirectSound_EnableHeadphones before
 * the sound pak's effects image is downloaded. CDirectSound_EnableHeadphones
 * 0xFA0F4 then sets bit 31 of its speaker config ([settings]+8) but writes
 * its crosstalk effect (the GP's X:0x02AE, 3 speakers, 1 headphones) only
 * when the effect index [settings]+0x10 isn't 0xFFFFFFFF, which it is until
 * the image is in. Nothing applies it later, so the effect keeps the speakers
 * value until the player changes the mode in a menu. It returns at once when
 * the value equals bit 31, so after the image (Audio_LoadPakItem kind 7) the
 * game's own call runs off and then on: the same two writes a pause-menu
 * switch from Speakers makes. With Speakers saved nothing runs. */
#define VA_DSOUND_ENABLE_HEADPHONES 0x000FAA1Du   /* IDirectSound_EnableHeadphones(pDS, on), stdcall */
#define X_AUDIO_MGR_DSOUND          0x001A4394u   /* Audio +0x3C: the game's IDirectSound* */
/* The crosstalk word in the GP's X window (0xFE830000 + 4 * 0x2AE): what the
 * effect write changes. The GP loads the effect (its code and this data) by
 * its own DMA a frame or so after the download; DirectSound sends effect data
 * through the GP too, so a write before that load is lost or worse (seen: a
 * call straight after the download gave the GP a bad DMA block and the run
 * crashed). So the call waits until the word shows the effect loaded. */
#define GP_WIN_X_CROSSTALK          (0xFE830000u + 4u * 0x2AEu)
extern int bw_xdsp_enabled(void);         /* the DSPs run (src/toolkit_patches/apu/bw_apu_xdsp.c) */

static void headphones_at_boot(void)
{
    uint32_t ds, settings, cfg, effect, args[2];
    ULONGLONG waited = 0;
    f_regs_t r;
    if (!bw_fix_enabled(BW_FIX_HEADPHONES_AT_BOOT))
        return;
    ds = F_MEM32(X_AUDIO_MGR_DSOUND);
    settings = F_OK(ds) ? F_MEM32(ds) : 0;  /* CDirectSound = ds - 8; its settings at +8 */
    if (!F_OK(settings))
        return;
    cfg = F_MEM32(settings + 8u);
    effect = F_MEM32(settings + 0x10u);
    if (!(cfg & 0x80000000u) || effect == 0xFFFFFFFFu) {
        fprintf(stderr, "[FIX] HEADPHONES_AT_BOOT: the effects image is in; DirectSound's mode is %s"
                "%s: nothing to apply\n", cfg & 0x80000000u ? "headphones" : "speakers",
                effect == 0xFFFFFFFFu ? " and it has no crosstalk effect" : "");
        fflush(stderr);
        return;
    }
    if (bw_xdsp_enabled()) {
        /* on this thread (the one that downloaded the image), a few ms */
        ULONGLONG t0 = GetTickCount64();
        while (!(F_MEM32(GP_WIN_X_CROSSTALK) & 0x00FFFFFFu) && GetTickCount64() - t0 < 250)
            Sleep(1);
        if (!(F_MEM32(GP_WIN_X_CROSSTALK) & 0x00FFFFFFu)) {
            fprintf(stderr, "[FIX] HEADPHONES_AT_BOOT: the GP didn't show its crosstalk effect"
                    " within 250 ms of the download: not applied\n");
            fflush(stderr);
            return;
        }
        waited = GetTickCount64() - t0;
    }
    f_save(&r);
    args[0] = ds;
    args[1] = 0;
    f_call(VA_DSOUND_ENABLE_HEADPHONES, 0, 2, args);
    args[1] = 1;
    f_call(VA_DSOUND_ENABLE_HEADPHONES, 0, 2, args);
    f_restore(&r);
    bw_fix_note_hit(BW_FIX_HEADPHONES_AT_BOOT);
    fprintf(stderr, "[FIX] HEADPHONES_AT_BOOT: the saved Headphones mode applied to DirectSound's"
            " crosstalk effect (index %u) now that the effects image is in (the GP had it after"
            " %llu ms)\n", effect, (unsigned long long)waited);
    fflush(stderr);
}

void sub_00014FA0(void)            /* Audio_LoadPakItem(data, size, kind, entry) */
{
    uint32_t data = F_MEM32(g_esp + 4), size = F_MEM32(g_esp + 8), kind = F_MEM32(g_esp + 0xC);
    uint32_t entry = F_MEM32(g_esp + 0x10), dsize = 0, buf;
    uint8_t *bytes;
    char name[33];
    int pak = 0;
    f_regs_t r;

    if (size != 0u || kind != 11u || !data || !F_OK(entry)
        || !bw_fix_enabled(BW_FIX_VOICE_PLACEHOLDER_BORROW)) {
        sub_00014FA0_gen();                 /* Original, and every non-empty item */
        if (kind == 7u && size)
            headphones_at_boot();           /* the DSP effects image is in */
        return;
    }
    f_str(entry, name, sizeof name);
    bytes = name[0] ? voice_donor(name, &dsize, &pak) : NULL;
    if (!bytes) {
        fprintf(stderr, "[FIX] VOICE_PLACEHOLDER_BORROW: '%s' is empty here and has no "
                "recording in any voice pack: stays silent\n", name);
        fflush(stderr);
        sub_00014FA0_gen();
        return;
    }
    f_save(&r);
    buf = f_call(VA_MEM_ALLOC, 0, 1, &dsize);
    f_restore(&r);
    if (!F_OK(buf) || !F_OK(buf + dsize - 1u)) {
        free(bytes);
        sub_00014FA0_gen();
        return;
    }
    memcpy((void *)((uintptr_t)buf + g_xbox_mem_offset), bytes, dsize);
    free(bytes);
    F_MEM32(g_esp + 4) = buf;
    F_MEM32(g_esp + 8) = dsize;
    sub_00014FA0_gen();                     /* ret 0x10: esp is past the arguments now */
    f_save(&r);
    f_call(VA_MEM_FREE, 0, 1, &buf);        /* Audio_ParseWav kept its own copy */
    f_restore(&r);
    bw_fix_note_hit(BW_FIX_VOICE_PLACEHOLDER_BORROW);
    fprintf(stderr, "[FIX] VOICE_PLACEHOLDER_BORROW: '%s' (an empty placeholder in this "
            "mission's voice pack) plays the recording from %02d_voice_english.pak (%u bytes)\n",
            name, pak, dsize);
    fflush(stderr);
}

/* ── M23_MISSING_TEXTURES (agent V1, docs/day1/V1.md) ───────────────────────
 * missions/23.pak's Textures table has two common references, Chin_Grate
 * (entry 5: the grates over the octagonal openings in the ceiling of the
 * arena hallway ArenaHlwy, 4 submeshes) and O_Brass (entry 36: the rods and
 * finials of the banners LungBanner and LungBannerH, 8 submeshes), that exist in no archive (seen in data). The
 * Textures handler Pak_TokenTextures 0x92CC0 (seen) treats an entry with size
 * +8 <= 0 as a common reference: Pak_FindCommonTexture 0x92A20 (ecx = the
 * 32-byte name field) searches only common/BoatText.pak; on a hit the handler
 * copies the found entry's +0/+4 (texture) and +0xC/+0xE (width, height) and
 * sets the found entry's flag 0x10 (borrowed, so Pak_FreeUnborrowedCommon-
 * Textures keeps it); on a miss it logs a missing-texture message and
 * zeroes +0/+4, and those submeshes draw with a null texture.
 *
 * The handler's argument (seen): ecx points at the section's offset word
 * (section = [ecx] + ecx; u16 count at +0xC; 0x38-byte entries from +0x10,
 * name at +0x18). Pak_RelocateSections 0x936A0 runs after every handler and
 * turns the parts' texture indices into these entries' pointers, and a
 * texture with flag 2 (cut-out) makes the relocator alpha-test the parts
 * that bind it (docs/formats/textures_catalogue.md). So after the original
 * body, each still-empty reference of the two names gets a stand-in's
 * texture, size and cut-out bit: first from the same pak (mission 23's own
 * textures are all created by then), else from BoatText through the game's
 * own lookup (the name field holds the stand-in's name for the call only),
 * marking it borrowed as the game does. Pak_FreeGpuResources 0x92A90 skips
 * entries with size <= 0 (seen), so a shared texture is released once.
 *
 *   BW_M23_GRATE=<texture name>   stand-in for Chin_Grate (default below)
 *   BW_M23_BRASS=<texture name>   stand-in for O_Brass
 *   BW_M23_CUTOUT=0|1             grate drawn cut-out (1) or opaque (0);
 *                                 default: the stand-in's own flag 2
 * Not in Original, which keeps the null textures. */
#define VA_PAK_FIND_COMMON_TEXTURE 0x00092A20u  /* ecx = name field; eax = BoatText entry or 0 */
#define M23_GRATE_DEFAULT "Chin_GirderMetal"  /* the maintainer's pick (2026-10-27): the riveted girder lattice, cut-out by its own flag */
#define M23_BRASS_DEFAULT "pittedcopper"

static int m23_fill(uint32_t ent, uint32_t first, uint32_t n, const char *want, int cutout,
                    const char *pakname)
{
    uint32_t i, src = 0;
    const char *from = "";
    char ref[33];
    f_str(ent + 0x18, ref, sizeof ref);
    for (i = 0; i < n && !src; i++) {           /* mission 23's own textures */
        uint32_t e = first + i * 0x38u;
        if (e != ent && (int32_t)F_MEM32(e + 8) > 0 && F_MEM32(e) && f_streq(e + 0x18, want)) {
            src = e;
            from = pakname;
        }
    }
    if (!src) {                                 /* common/BoatText.pak, as the game looks */
        uint8_t save[32], name[32];
        f_regs_t r;
        memcpy(save, (const void *)((uintptr_t)(ent + 0x18) + g_xbox_mem_offset), 32);
        memset(name, 0, sizeof name);
        strncpy((char *)name, want, 31);
        memcpy((void *)((uintptr_t)(ent + 0x18) + g_xbox_mem_offset), name, 32);
        f_save(&r);
        src = f_call(VA_PAK_FIND_COMMON_TEXTURE, ent + 0x18, 0, NULL);
        f_restore(&r);
        memcpy((void *)((uintptr_t)(ent + 0x18) + g_xbox_mem_offset), save, 32);
        if (src && (!F_OK(src) || !F_MEM32(src)))
            src = 0;
        if (src) {
            F_MEM8(src + 0x10) |= 0x10;         /* borrowed (the handler's own rule) */
            from = "common/BoatText";
        }
    }
    if (!src) {
        fprintf(stderr, "[FIX] M23_MISSING_TEXTURES: no texture '%s' in %s or BoatText; "
                "%s keeps its null texture\n", want, pakname, ref);
        fflush(stderr);
        return 0;
    }
    F_MEM32(ent) = F_MEM32(src);
    F_MEM32(ent + 4) = F_MEM32(src + 4);
    F_MEM16(ent + 0xC) = F_MEM16(src + 0xC);
    F_MEM16(ent + 0xE) = F_MEM16(src + 0xE);
    if (cutout > 0 || (cutout < 0 && (F_MEM16(src + 0x10) & 2)))
        F_MEM16(ent + 0x10) = (uint16_t)(F_MEM16(ent + 0x10) | 2);
    else
        F_MEM16(ent + 0x10) = (uint16_t)(F_MEM16(ent + 0x10) & ~2u);
    fprintf(stderr, "[FIX] M23_MISSING_TEXTURES: %s's '%s' now draws with '%s' from %s "
            "(%ux%u%s)\n", pakname, ref, want, from, F_MEM16(ent + 0xC), F_MEM16(ent + 0xE),
            (F_MEM16(ent + 0x10) & 2) ? ", cut-out" : "");
    fflush(stderr);
    return 1;
}

extern void sub_00092CC0_gen(void);

void sub_00092CC0(void)            /* Pak_TokenTextures (ecx = section offset word, edx = pak) */
{
    uint32_t secp = g_ecx, pak = g_edx, sec, first, n, i;
    char pakname[24];
    const char *grate, *brass, *e;
    int cutout = -1;
    f_regs_t r;

    sub_00092CC0_gen();
    if (!bw_fix_enabled(BW_FIX_M23_MISSING_TEXTURES) || !F_OK(secp))
        return;
    sec = secp + F_MEM32(secp);
    if (!F_OK(sec) || F_MEM32(sec) != 0x4B4E4843u)       /* "CHNK" */
        return;
    n = F_MEM16(sec + 0xC);
    first = sec + 0x10;
    for (i = 0; i < n; i++) {                   /* any unresolved reference of ours? */
        uint32_t ent = first + i * 0x38u;
        if ((int32_t)F_MEM32(ent + 8) <= 0 && !F_MEM32(ent)
                && (f_streq(ent + 0x18, "Chin_Grate") || f_streq(ent + 0x18, "O_Brass")))
            break;
    }
    if (i == n)
        return;
    f_save(&r);
    f_str(pak + 0x14, pakname, sizeof pakname);
    grate = (e = getenv("BW_M23_GRATE")) && *e ? e : M23_GRATE_DEFAULT;
    brass = (e = getenv("BW_M23_BRASS")) && *e ? e : M23_BRASS_DEFAULT;
    if ((e = getenv("BW_M23_CUTOUT")) && *e)
        cutout = e[0] == '1';
    for (i = 0; i < n; i++) {
        uint32_t ent = first + i * 0x38u;
        if ((int32_t)F_MEM32(ent + 8) > 0 || F_MEM32(ent))
            continue;
        if (f_streq(ent + 0x18, "Chin_Grate")) {
            if (m23_fill(ent, first, n, grate, cutout, pakname))
                bw_fix_note_hit(BW_FIX_M23_MISSING_TEXTURES);
        } else if (f_streq(ent + 0x18, "O_Brass")) {
            if (m23_fill(ent, first, n, brass, -1, pakname))
                bw_fix_note_hit(BW_FIX_M23_MISSING_TEXTURES);
        }
    }
    f_restore(&r);                              /* the handler's result (al = 1) stands */
}

/* ════════════════════════════════════════════════════════════════════════
 * BATTLE_WATER_SOUNDS: Battle Mode boats get the game's own water sounds
 * (agent F2, follow-up; docs/day1/F2.md section 5)
 * ════════════════════════════════════════════════════════════════════════
 *
 * Seen (F2.md section 4): Audio_InitBoatAndTurretSounds 0x142E0 calls
 * Audio_PlayerBoatInit 0x11F10 for every human player's boat in every game
 * type, and that asks for WaterSpray/Lap/Smack/Splash/Plunge/Wash 1-4 (and the
 * sluice loops). The 26 Water* recordings ship only in the Sound section of
 * the 28 story mission paks (missions/NN.pak; 27 of 28 byte-identical,
 * mission 19's WaterLap1-4 differ), which a battle never loads (its mission
 * pak is missions/sm_<arena>.pak, with no Sound section; the 14 audio banks
 * are the same in both). So in Battle Mode every water sound object is
 * created without a buffer and the boats are silent on the water.
 *
 * The fix (Remastered, on by default: the maintainer's call, 2026-10-27): before the
 * body runs, when the wave table has no WaterLap1 (never true in a story
 * mission, whose own bank has it), the Sound section of one story pak is
 * read with the game's own zlib (1.1.3, statically linked; inflateInit_
 * 0xD7FC0, inflate 0xD7FE0, inflateEnd 0xD7E80, register arguments as
 * zlib_uncompress 0xD8C30 calls them) in small chunks, so only a 64 KB input
 * and a 256 KB output buffer live in guest memory, and each Water* item goes
 * through the game's own Audio_LoadPakItem body, exactly as a story mission's
 * bank does (Audio_ParseWav copies it). The pak is mission 3's (its Sound
 * section comes soonest; the set is the canonical one), from the disc folder
 * as main.c finds it; never mission 19. Cost: the 26 wave records and their
 * 1,035,492 bytes, the same as every story mission carries. */
#define VA_ZLIB_INFLATEINIT 0x000D7FC0u /* ecx = stream, edx = version, [esp+4] = stream size */
#define VA_ZLIB_INFLATE     0x000D7FE0u /* ecx = stream, edx = flush; eax = 0 ok, 1 end, < 0 error */
#define VA_ZLIB_INFLATEEND  0x000D7E80u /* ecx = stream */
#define X_ZLIB_VERSION      0x0017D080u /* "1.1.3", as zlib_uncompress passes it */
#define X_AUDIO_MGR         0x001A4358u /* wave table +0x70, count +0x6C, 0x4C records, name +0 */
#define BW_IN_CHUNK         0x10000u
#define BW_OUT_CHUNK        0x40000u

/* f_call with edx set as well (the game's zlib takes its first two
 * arguments in ecx and edx). */
static uint32_t f_call_cd(uint32_t va, uint32_t ecx, uint32_t edx, int nargs, const uint32_t *args)
{
    g_edx = edx;
    return f_call(va, ecx, nargs, args);
}

/* 1 when the audio manager's wave table holds a wave of that name, 0 when
 * not, -1 when the table can't be read. Read-only. */
static int wave_table_has(const char *name)
{
    uint32_t tab = F_MEM32(X_AUDIO_MGR + 0x70), n = F_MEM32(X_AUDIO_MGR + 0x6C), i;
    if (!F_OK(tab) || n > 1024)
        return -1;
    for (i = 0; i < n; i++)
        if (f_streq(tab + i * 0x4Cu, name))
            return 1;
    return 0;
}

/* The unpacked bytes of the "Sound" section of a story pak, read with the
 * game's own inflate; NULL on any failure. *len_out = its length. */
static uint8_t *story_sound_section(int mission, uint32_t *len_out, double *ms_out)
{
    const char *dir = getenv("BW_GAME_DIR");
    char path[MAX_PATH];
    uint8_t hdr[112], head[4096], *snd = NULL, *fbuf = NULL;
    uint32_t unp, pk, z = 0, in = 0, out = 0, sz, total = 0, head_n = 0;
    uint32_t snd_start = 0, snd_end = 0, got_snd = 0, fpos = 0;
    int ok = 0, inited = 0, r;
    FILE *f;
    LARGE_INTEGER t0, t1, fq;
    f_regs_t regs;

    QueryPerformanceCounter(&t0);
    if (!dir || !*dir)
        dir = "game\\disc";
    _snprintf(path, sizeof path, "%s\\missions\\%02d.pak", dir, mission);
    path[sizeof path - 1] = 0;
    f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr || memcmp(hdr, "spak", 4))
        goto done;
    unp = rd32(hdr + 8);
    pk = rd32(hdr + 16);
    if (unp == pk || pk < 16 || pk > (64u << 20) || unp > (128u << 20))
        goto done;                             /* story paks are zlib (seen) */
    fbuf = (uint8_t *)malloc(BW_IN_CHUNK);
    if (!fbuf)
        goto done;
    f_save(&regs);
    sz = 0x38u;
    z = f_call(VA_MEM_ALLOC, 0, 1, &sz);
    sz = BW_IN_CHUNK;
    in = f_call(VA_MEM_ALLOC, 0, 1, &sz);
    sz = BW_OUT_CHUNK;
    out = f_call(VA_MEM_ALLOC, 0, 1, &sz);
    if (!F_OK(z) || !F_OK(in) || !F_OK(out))
        goto freeg;
    sz = 0x38u;                                /* zalloc/zfree 0: zlib's defaults */
    if (f_call_cd(VA_ZLIB_INFLATEINIT, z, X_ZLIB_VERSION, 1, &sz) != 0u)
        goto freeg;
    inited = 1;
    for (;;) {
        uint32_t produced, a, b;
        if (F_MEM32(z + 4) == 0u) {            /* avail_in: refill from the file */
            size_t n = 0;
            if (fpos < pk)
                n = fread(fbuf, 1, (pk - fpos) < BW_IN_CHUNK ? (pk - fpos) : BW_IN_CHUNK, f);
            if (!n)
                break;
            memcpy((void *)((uintptr_t)in + g_xbox_mem_offset), fbuf, n);
            fpos += (uint32_t)n;
            F_MEM32(z + 0) = in;
            F_MEM32(z + 4) = (uint32_t)n;
        }
        F_MEM32(z + 0xC) = out;
        F_MEM32(z + 0x10) = BW_OUT_CHUNK;
        r = (int)f_call_cd(VA_ZLIB_INFLATE, z, 0 /* Z_NO_FLUSH */, 0, NULL);
        produced = BW_OUT_CHUNK - F_MEM32(z + 0x10);
        if (produced > BW_OUT_CHUNK)
            break;
        /* The section table at the start: u32 size, then name[12] + u32 end
         * (counted from that dword) per section; the last one has no end
         * and runs to the end of the pak (tools/bw_pak.py sections()). */
        if (!snd && head_n < sizeof head) {
            uint32_t k = produced < sizeof head - head_n ? produced : (uint32_t)sizeof head - head_n;
            memcpy(head + head_n, (const void *)((uintptr_t)out + g_xbox_mem_offset), k);
            head_n += k;
            if (head_n >= 4) {
                uint32_t ts = rd32(head), i, start;
                if (ts < 16 || ts > sizeof head || (ts - 4) % 16 != 12)
                    break;
                if (head_n >= ts) {
                    start = ts;
                    for (i = 4; i < ts; i += 16) {
                        uint32_t end = (i + 16 <= ts) ? i + 12 + rd32(head + i + 12) : unp;
                        if (!strncmp((const char *)head + i, "Sound", 12)) {
                            snd_start = start;
                            snd_end = end;
                            break;
                        }
                        start = end;
                    }
                    if (!snd_end || snd_end > unp || snd_start >= snd_end
                        || snd_end - snd_start > (32u << 20))
                        break;
                    snd = (uint8_t *)malloc(snd_end - snd_start);
                    if (!snd)
                        break;
                }
            }
        }
        /* Keep the overlap of [total, total + produced) with the section. */
        if (snd) {
            a = total > snd_start ? total : snd_start;
            b = (total + produced) < snd_end ? total + produced : snd_end;
            if (a < b) {
                memcpy(snd + (a - snd_start),
                       (const void *)((uintptr_t)(out + (a - total)) + g_xbox_mem_offset), b - a);
                got_snd += b - a;
            }
        }
        total += produced;
        if (snd && total >= snd_end) {
            ok = got_snd == snd_end - snd_start;
            break;
        }
        if (r != 0)                            /* 1 = stream end, < 0 = error */
            break;
    }
freeg:
    if (inited)
        f_call(VA_ZLIB_INFLATEEND, z, 0, NULL);
    if (F_OK(out))
        f_call(VA_MEM_FREE, 0, 1, &out);
    if (F_OK(in))
        f_call(VA_MEM_FREE, 0, 1, &in);
    if (F_OK(z))
        f_call(VA_MEM_FREE, 0, 1, &z);
    f_restore(&regs);
done:
    free(fbuf);
    fclose(f);
    if (!ok) {
        free(snd);
        return NULL;
    }
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&fq);
    *ms_out = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)fq.QuadPart;
    *len_out = snd_end - snd_start;
    return snd;
}

static void battle_water_load(void)
{
    static const int donors[] = { 3, 1, 2, 4, 6, 7, 8 };   /* the canonical set (seen); never 19 */
    uint8_t *s = NULL;
    uint32_t len = 0, cnt, k, bytes = 0, fnv = 2166136261u;
    int d, added = 0, pak = 0;
    double ms = 0.0;
    f_regs_t regs;

    if (wave_table_has("WaterLap1") != 0)
        return;                                /* a story mission: its own bank has the set */
    for (d = 0; d < (int)(sizeof donors / sizeof donors[0]) && !s; d++) {
        s = story_sound_section(donors[d], &len, &ms);
        pak = donors[d];
    }
    if (!s || len < 16 || memcmp(s, "CHNK", 4)) {
        fprintf(stderr, "[FIX] BATTLE_WATER_SOUNDS: no story pak's Sound section could be read; "
                "the boats stay silent on the water\n");
        fflush(stderr);
        free(s);
        return;
    }
    cnt = (uint32_t)s[12] | ((uint32_t)s[13] << 8);
    f_save(&regs);
    for (k = 0; k < cnt && 16u + (k + 1u) * 44u <= len; k++) {
        const uint8_t *e = s + 16u + k * 44u;
        uint32_t off = rd32(e + 32), size = rd32(e + 36), kind = rd32(e + 40);
        uint32_t at = 16u + cnt * 44u + off, blk, alloc, args[4], i, sp0;
        char name[33];
        int j;
        memcpy(name, e, 32);
        name[32] = 0;
        if (kind != 11u || !size || strncmp(name, "Water", 5) || at + size > len
            || wave_table_has(name) != 0)
            continue;
        alloc = 44u + size;
        blk = f_call(VA_MEM_ALLOC, 0, 1, &alloc);
        if (!F_OK(blk) || !F_OK(blk + alloc - 1u))
            break;
        memcpy((void *)((uintptr_t)blk + g_xbox_mem_offset), e, 44);
        memcpy((void *)((uintptr_t)(blk + 44u) + g_xbox_mem_offset), s + at, size);
        args[0] = blk + 44u;                   /* data */
        args[1] = size;
        args[2] = 11u;                         /* a wave */
        args[3] = blk;                         /* the entry: Audio_ParseWav copies its name */
        sp0 = g_esp;                           /* the game's own body, not the voice wrapper */
        for (j = 3; j >= 0; j--) {
            g_esp -= 4;
            F_MEM32(g_esp) = args[j];
        }
        g_esp -= 4;
        F_MEM32(g_esp) = 0;
        g_ecx = X_AUDIO_MGR;
        sub_00014FA0_gen();
        g_esp = sp0;
        f_call(VA_MEM_FREE, 0, 1, &blk);       /* Audio_ParseWav kept its own copy */
        for (i = 0; i < size; i++)
            fnv = (fnv ^ s[at + i]) * 16777619u;
        bytes += size;
        added++;
    }
    f_restore(&regs);
    free(s);
    if (added)
        bw_fix_note_hit(BW_FIX_BATTLE_WATER_SOUNDS);
    fprintf(stderr, "[FIX] BATTLE_WATER_SOUNDS: %d water waves (%u bytes, FNV-1a %08X) loaded from "
            "missions/%02d.pak's Sound section with the game's own inflate (%.0f ms); the wave "
            "table now holds %u records\n", added, bytes, fnv, pak, ms, F_MEM32(X_AUDIO_MGR + 0x6C));
    fflush(stderr);
}

/* BW_FIX_PROBE: what this level boot has loaded, read-only, any edition. The
 * pak set 0x1D5C68 (Pak_Load's ecx) is handed to Pak_ReadAndParse 0x92460,
 * which counts paks at +8 and adds each pak's size to +0xC (seen); the audio
 * manager adds each sound item's size to +0x7C (Audio_LoadPakItem, seen) and
 * counts wave records at +0x6C (of 450 made by Audio_Init). */
static void level_memory_probe(void)
{
    fprintf(stderr, "[FIX] probe level memory (%s, BATTLE_WATER_SOUNDS %s): rule type %u, %u human "
            "players, %u paks holding %u bytes, sound items %u bytes, %u wave records of 450\n",
            bw_edition_name(bw_edition()), bw_fix_enabled(BW_FIX_BATTLE_WATER_SOUNDS) ? "on" : "off",
            F_MEM32(0x001E5F48u), F_MEM32(0x001CEE58u + 0xC0u), F_MEM32(0x001D5C70u),
            F_MEM32(0x001D5C74u), F_MEM32(X_AUDIO_MGR + 0x7C), F_MEM32(X_AUDIO_MGR + 0x6C));
    fflush(stderr);
}

extern void sub_000142E0_gen(void);

void sub_000142E0(void)            /* Audio_InitBoatAndTurretSounds */
{
    if (bw_fix_enabled(BW_FIX_BATTLE_WATER_SOUNDS))
        battle_water_load();
    if (f_probe())
        level_memory_probe();
    sub_000142E0_gen();
}

/* ── Test aid BW_FIXTEST_START=<x>,<y>,<heading degrees> (F2) ─────────────
 * Puts the player's boat at a chosen start, to reach a map edge unattended.
 * Mission_LoadPaks 0x80EA0 (seen) loads the mission, env and voice paks, then
 * calls Mission_ComputeVoiceDurations 0x80610 (thiscall, ecx = the mission
 * object 0x1D4058, no arguments) and only then Characters_Load 0x717D0, which
 * places the story boats from the mission's placement table (mission data
 * +0x13D0 count, +0x13D4 records of 0xEC: x +0x20, y +0x24, heading in
 * degrees +0x28, +0x2C == 0 for the player; seen). So
 * after 0x80610 the player's record is rewritten in the loaded mission block.
 * Writes game state: never set it for play. Any edition, only when set. */
extern void sub_00080610_gen(void);

void sub_00080610(void)            /* Mission_ComputeVoiceDurations */
{
    const char *e = getenv("BW_FIXTEST_START");
    sub_00080610_gen();
    if (e && *e) {
        float x, y, h;
        uint32_t m = F_MEM32(0x001D405Cu), n, tab, i;   /* mission data (0x1D4058 +4) */
        if (sscanf(e, "%f,%f,%f", &x, &y, &h) != 3 || !F_OK(m))
            return;
        n = F_MEM32(m + 0x13D0);
        tab = F_MEM32(m + 0x13D4);
        if (!n || n > 256 || !F_OK(tab))
            return;
        for (i = 0; i < n; i++) {
            uint32_t r = tab + i * 0xECu;
            if (F_MEM32(r + 0x2C) == 0u) {
                char nm[40];
                memcpy((void *)((uintptr_t)(r + 0x20) + g_xbox_mem_offset), &x, 4);
                memcpy((void *)((uintptr_t)(r + 0x24) + g_xbox_mem_offset), &y, 4);
                memcpy((void *)((uintptr_t)(r + 0x28) + g_xbox_mem_offset), &h, 4);
                fprintf(stderr, "[FIXTEST] BW_FIXTEST_START: player boat '%s' starts at (%.1f, "
                        "%.1f), heading %.0f\n", f_str(r, nm, sizeof nm), x, y, h);
                fflush(stderr);
                break;
            }
        }
    }
}

/* ════════════════════════════════════════════════════════════════════════
 * AI_MOOD_WAVE_PERIOD: the AI's slow "mood" wave runs at its own speed
 * ════════════════════════════════════════════════════════════════════════
 *
 * RandomWave_Init 0xB7250 / RandomWave_InitDefault 0xB7340 (seen): +0 type,
 * +4 the period in seconds (600 for the default), +0xC the frequency: pi/period
 * for type 1 (sine), 1/period for type 0 (triangle), +8/+0x10 phases, +0x14 a
 * second frequency ratio 0.5..1.
 * RandomWave_Value 0xB73D0 (seen): type 0 multiplies the clock by +0xC; type 1
 * multiplies it by +4, the raw period, so the default "600 s" wave turns at
 * 600 rad/s (about 95 Hz) and, sampled every 0.25-0.5 s by the AI's
 * evaluators, is noise in [-1, 1] instead of a 20-minute swing (C5.md). In
 * AIEvalAttackBoat_Compute that noise scales the attack score by up to +-25 %
 * at Ensign. The intent is seen: Init computes pi/period at +0xC for exactly
 * this branch, and nothing else reads it; the triangle branch uses its +0xC.
 *
 * The fix: for a type-1 wave, +4 and +0xC swap places for the body and are put
 * back; the sine branch reads +4 once and +0xC not at all (seen). */
extern void sub_000B73D0_gen(void);
/* The x87 stack of the runtime; the body returns its value in st(0). */
extern __declspec(thread) double g_fp_stack[8];
extern __declspec(thread) int g_fp_top;
#define X_AI_CLOCK 0x001F2E18u          /* the AI scheduler's clock (s), read by the body */

/* BW_FIX_PROBE: the first 120 values of the first type-1 wave asked for, with
 * the AI clock; read-only. */
static void wave_probe(uint32_t w)
{
    static uint32_t s_wave;
    static int s_n;
    if (!s_wave && F_OK(w) && F_MEM32(w) == 1u)
        s_wave = w;
    if (w != s_wave || s_n >= 120)
        return;
    s_n++;
    fprintf(stderr, "[FIX] probe RandomWave (%s) t %.3f v %.4f\n", bw_edition_name(bw_edition()),
            f_f32(X_AI_CLOCK), g_fp_stack[g_fp_top & 7]);
    if (s_n == 120)
        fflush(stderr);
}

void sub_000B73D0(void)
{
    uint32_t w = g_ecx, period, freq;
    if (!bw_fix_enabled(BW_FIX_AI_MOOD_WAVE_PERIOD) || !F_OK(w) || F_MEM32(w) != 1u) {
        sub_000B73D0_gen();
        if (f_probe())
            wave_probe(w);
        return;
    }
    period = F_MEM32(w + 4);
    freq = F_MEM32(w + 0xC);
    F_MEM32(w + 4) = freq;
    sub_000B73D0_gen();
    F_MEM32(w + 4) = period;
    bw_fix_note_hit(BW_FIX_AI_MOOD_WAVE_PERIOD);
    if (f_probe())
        wave_probe(w);
}

/* ════════════════════════════════════════════════════════════════════════
 * AI_RULES_AND_AS_OR: "NAME=X1 AND NAME=X2" rules mean either one
 * ════════════════════════════════════════════════════════════════════════
 *
 * A mission's AI rules (table +0x13E8 count / +0x13EC records of 0x24 bytes:
 * name[32], condition tree pointer) classify the objects that team scripts
 * target and follow. MisAIRule_EvalCondition 0xC68D0 (seen): node +0 type
 * (0 AND, 1 OR, 2 NOT, 3 NAME string at +8, 4 SQUAD and 5 CLASS strings at
 * +4, 6 VALUE and 7 SIDE integers at +4, 8 IN_GROUP), children +0x28/+0x2C;
 * AND is a strict "both".
 * Every AND in the shipped data (17 rules: M09 3, M10 1, M14 1, M24 1, M26 2,
 * M27 9; tools/bw_missiondata.py) joins leaves of one single-valued kind with
 * different values ("SQUAD=BtyKerangEntrance AND SQUAD=BtyKerangFort",
 * "SIDE=1 AND SIDE=3"), so it can match nothing, and 7 of them are named in
 * scripts' target or follow lists, which are dead as shipped
 * (docs/formats/missions.md, finding 5). The intent is seen in the data: the
 * rules' own names ("BtyKerang", "Seahawks", "EscortA", "Pillboxes") name the
 * set, and M21 writes the same construct with OR.
 *
 * The fix: before MisAIRules_Load 0xC7AE0 copies the rules and resolves the
 * team scripts (which classifies every member, MisAIRules_ClassifyTeam), each
 * AND sub-tree whose leaves are all one single-valued kind with at least two
 * different values becomes an OR sub-tree, in the loaded mission block (guest
 * memory; the files and the Z: cache copy stay as shipped). An AND that could
 * match (different kinds, or IN_GROUP) is left alone; none ships. */
#define X_MISSION_DATA_PTR 0x001D405Cu

typedef struct {
    int      kind;            /* -1: not yet */
    int      ok;
    int      distinct;
    uint32_t first;           /* the first leaf node */
} f_andscan_t;

static int leaf_equal(uint32_t a, uint32_t b, int kind)
{
    uint32_t off = kind == 3 ? 8u : 4u;
    if (kind == 6 || kind == 7)
        return F_MEM32(a + 4) == F_MEM32(b + 4);
    {
        char sa[40], sb[40];
        f_str(a + off, sa, sizeof sa);
        f_str(b + off, sb, sizeof sb);
        return !strcmp(sa, sb);
    }
}

static void and_scan(uint32_t node, f_andscan_t *s, int depth)
{
    uint32_t t;
    if (!s->ok)
        return;
    if (!F_OK(node) || depth > 32) {
        s->ok = 0;
        return;
    }
    t = F_MEM32(node);
    if (t == 0) {
        and_scan(F_MEM32(node + 0x28), s, depth + 1);
        and_scan(F_MEM32(node + 0x2C), s, depth + 1);
        return;
    }
    if (t < 3 || t > 7) {                /* OR, NOT, IN_GROUP or unknown: satisfiable or unsure */
        s->ok = 0;
        return;
    }
    if (s->kind < 0) {
        s->kind = (int)t;
        s->first = node;
    } else if (s->kind != (int)t) {
        s->ok = 0;                        /* NAME=X AND SIDE=1 can match: leave it */
    } else if (!leaf_equal(s->first, node, (int)t)) {
        s->distinct = 1;
    }
}

static int and_to_or(uint32_t node, int depth)
{
    int n = 0;
    if (!F_OK(node) || depth > 32 || F_MEM32(node) != 0u)
        return 0;
    F_MEM32(node) = 1u;
    n = 1;
    n += and_to_or(F_MEM32(node + 0x28), depth + 1);
    n += and_to_or(F_MEM32(node + 0x2C), depth + 1);
    return n;
}

static int rule_fix(uint32_t node, int depth)
{
    uint32_t t;
    if (!F_OK(node) || depth > 32)
        return 0;
    t = F_MEM32(node);
    if (t == 0) {
        f_andscan_t s = { -1, 1, 0, 0 };
        and_scan(node, &s, depth);
        if (s.ok && s.distinct)
            return and_to_or(node, depth);
    }
    if (t == 0 || t == 1)
        return rule_fix(F_MEM32(node + 0x28), depth + 1) + rule_fix(F_MEM32(node + 0x2C), depth + 1);
    if (t == 2)
        return rule_fix(F_MEM32(node + 0x28), depth + 1);
    return 0;
}

/* M21_PATROL_RESPAWN (G4; OFF by default, intent inferred). Squad records
 * (mission +0x13F8 count, +0x13FC records of 0x134: name +0, member count
 * +0x24, members +0x28, repeat count +0x128 with 0 = forever; seen,
 * tools/bw_missiondata.py and TeamScript_Resolve 0xC6F53, which reads +0x128
 * inside MisAIRules_Load). M21's SeahawkE is the only one of the 100 squads
 * that use the "PlaceMembers + ResetObjective" respawn pattern with repeat 0,
 * so once the destroyer TigerA is sunk it comes back after every sinking
 * (docs/formats/missions.md finding 2). Every other respawner runs once. With
 * the fix its count is 1, like theirs: it comes back once. That the endless
 * patrol is a slip and not a design is inferred (the RGD video calls it a
 * glitch), so the maintainer decides. */
static void m21_patrol_fix(uint32_t m)
{
    uint32_t n = F_MEM32(m + 0x13F8), recs = F_MEM32(m + 0x13FC), i;
    if (!n || n > 512 || !F_OK(recs))
        return;
    for (i = 0; i < n; i++) {
        uint32_t r = recs + i * 0x134u;
        if (f_streq(r, "SeahawkE") && F_MEM32(r + 0x24) == 1u && f_streq(r + 0x28, "SeahawkE")
            && F_MEM32(r + 0x128) == 0u) {
            F_MEM32(r + 0x128) = 1u;
            bw_fix_note_hit(BW_FIX_M21_PATROL_RESPAWN);
            fprintf(stderr, "[FIX] M21_PATROL_RESPAWN: squad SeahawkE runs its script once "
                    "(was forever), so the patrol boat comes back once\n");
            fflush(stderr);
        }
    }
}

extern void sub_000C7AE0_gen(void);

void sub_000C7AE0(void)
{
    if (bw_fix_enabled(BW_FIX_M21_PATROL_RESPAWN) && F_OK(F_MEM32(X_MISSION_DATA_PTR)))
        m21_patrol_fix(F_MEM32(X_MISSION_DATA_PTR));
    if (bw_fix_enabled(BW_FIX_AI_RULES_AND_AS_OR)) {
        uint32_t m = F_MEM32(X_MISSION_DATA_PTR), n, recs, i;
        int rules = 0, nodes = 0;
        char names[256];
        names[0] = 0;
        n = F_OK(m) ? F_MEM32(m + 0x13E8) : 0;
        recs = F_OK(m) ? F_MEM32(m + 0x13EC) : 0;
        if (n > 0 && n < 512 && F_OK(recs)) {
            for (i = 0; i < n; i++) {
                uint32_t rec = recs + i * 0x24u;
                int k = rule_fix(F_MEM32(rec + 0x20), 0);
                if (k > 0) {
                    char nm[40];
                    rules++;
                    nodes += k;
                    f_str(rec, nm, sizeof nm);
                    if (strlen(names) + strlen(nm) + 3 < sizeof names) {
                        if (names[0])
                            strcat(names, ", ");
                        strcat(names, nm);
                    }
                    bw_fix_note_hit(BW_FIX_AI_RULES_AND_AS_OR);
                }
            }
            if (rules) {
                fprintf(stderr, "[FIX] AI_RULES_AND_AS_OR: %d of %u rules read as OR (%d AND "
                        "nodes): %s\n", rules, n, nodes, names);
                fflush(stderr);
            }
        }
    }
    sub_000C7AE0_gen();
}

/* ════════════════════════════════════════════════════════════════════════
 * M17_CRATE_RESPAWN (G11; OFF by default, intent inferred)
 * ════════════════════════════════════════════════════════════════════════
 * Dynamic objects (mission +0x1400 count, +0x1404 records of 0x88: name +0,
 * x/y/z +0x20, class name +0x2C, start delay +0x50, respawn delay +0x54,
 * appearances +0x58 with 0 = infinite; seen, docs/formats/missions.md 2.9;
 * Powerup_Init 0x8D160 reads +0x58 at 0x8D1CB). World_LoadPlacedObjects
 * 0x73F40 (seen: [ebp+8] = the mission block) creates them. In M17 the health
 * crate repair25b at (1378.6, -232.5) has 0 appearances and a respawn delay
 * of 0 (clamped to 1 s), so it refills every second, forever; the other five
 * crates have 1 (never respawn), and it's the only infinite power-up in the
 * story missions (Death Row's arena crate of the same name is infinite by
 * design, 30 s). With the fix it has 1 like the others. Inferred intent: a
 * designer might have meant one endless crate. The match is exact (name,
 * class, count 0, delay 0, position), so no other crate can be touched. */
extern void sub_00073F40_gen(void);

void sub_00073F40(void)
{
    if (bw_fix_enabled(BW_FIX_M17_CRATE_RESPAWN)) {
        uint32_t blk = F_MEM32(g_esp + 4), n, recs, i;
        n = F_OK(blk) ? F_MEM32(blk + 0x1400) : 0;
        recs = F_OK(blk) ? F_MEM32(blk + 0x1404) : 0;
        if (n && n < 4096 && F_OK(recs))
            for (i = 0; i < n; i++) {
                uint32_t r = recs + i * 0x88u;
                float x = f_f32(r + 0x20), y = f_f32(r + 0x24);
                if (f_streq(r, "repair25b") && f_streq(r + 0x2C, "HealthA")
                    && F_MEM32(r + 0x58) == 0u && f_f32(r + 0x54) == 0.0f
                    && x > 1377.6f && x < 1379.6f && y > -233.5f && y < -231.5f) {
                    F_MEM32(r + 0x58) = 1u;
                    bw_fix_note_hit(BW_FIX_M17_CRATE_RESPAWN);
                    fprintf(stderr, "[FIX] M17_CRATE_RESPAWN: crate repair25b appears once "
                            "(was forever, every second)\n");
                    fflush(stderr);
                }
            }
    }
    sub_00073F40_gen();
}

/* ════════════════════════════════════════════════════════════════════════
 * Objective_EvaluateCondition 0x7F520 (seen: stdcall-like thiscall, ecx =
 * the objectives manager, one argument = the objective, ret 4, al = met):
 * M10_SECOND_STRIKE, and the test aids BW_FIXTEST_MEET / BW_FIXTEST_SINK.
 * ════════════════════════════════════════════════════════════════════════
 *
 * Runtime objective (0xEC bytes; array [0x1D3FFC], count [0x1D3FE0], src/
 * bw_state.c): name +0, enabled +0x24, type +0x38, met +0x48, failed +0x49,
 * condition +0x50, target count (byte) +0x58 and list +0x5C (4-byte entries:
 * two latch bytes, then the s16 instance index into [0x1D4004], 12-byte
 * entries with the game object at +8), MOVE_TO point +0xAC/+0xB0. Mission time
 * in ms at [0x1D3FB8] (ObjCond_TimeElapsed 0x7D4E0). Game object: hitpoints
 * +0x20, position +0x84/+0x88. All seen (disassembly 2026-10-27).
 *
 * M10_SECOND_STRIKE (G1, "Ships in the Night" can't be finished):
 *  - The data (seen, analysis/missions/10.txt; ANALYSIS "Mission 10's
 *    soft-lock"): KoiA3/A4 and StormA3/A4, once sunk, run ResetObjective
 *    on the strike-force objective (A14), PlaceMembers at about (400-500,
 *    3500-3700), north of the FAIL_EdgeNorth line (y 3207) where the player
 *    can't go, then Suspend until StrikeNearKerang. StrikeNearKerang is a
 *    MOVE_TO on the Strike Force group: met once any member has crossed a band
 *    near Kerang (fraction 0; ObjCond_MoveTo 0x7E330 tests positions, alive or
 *    not). The radio line M10A12 (the second strike force's warning)
 *    plays 10 s after it; the batteries and the allies wait on it too.
 *  - So if the player sinks every boat of the first force before one reaches
 *    the band, the respawned four wait forever off the map, the reset primary
 *    needs them sunk, and the mission can't be finished (the RGD video; seen
 *    in a run, docs/day1/D11.md).
 *  - The fix gives the game's own second-wave sequence a trigger it lacks in
 *    exactly that state: when no Strike Force member is afloat inside the
 *    mission area (each is sunk or north of the north fail line) and at least
 *    one is afloat north of it, StrikeNearKerang counts as met. The game then
 *    runs its own chain: the radio line, the second force sails in, the
 *    batteries and allies engage. In every other state the objective is
 *    evaluated as shipped, so a normal playthrough is untouched.
 *  - Intent: that the second force must come and the mission be finishable is
 *    seen in the data (the respawn scripts, the radio line, the primary that
 *    needs every boat sunk); the trigger for the soft-locked state is ours.
 */
#define X_OBJ_COUNT     0x001D3FE0u
#define X_OBJ_ARRAY     0x001D3FFCu
#define X_OBJ_STRIDE    0xECu
#define X_INSTANCES     0x001D4004u
#define X_MISSION_MS    0x001D3FB8u

static uint32_t find_objective(const char *name)
{
    uint32_t n = F_MEM32(X_OBJ_COUNT), arr = F_MEM32(X_OBJ_ARRAY), i;
    if (n > 1024 || !F_OK(arr))
        return 0;
    for (i = 0; i < n; i++)
        if (f_streq(arr + i * X_OBJ_STRIDE, name))
            return arr + i * X_OBJ_STRIDE;
    return 0;
}

/* The game object of an objective's k-th target, or 0. */
static uint32_t objective_target(uint32_t obj, int k)
{
    uint32_t list = F_MEM32(obj + 0x5C), inst = F_MEM32(X_INSTANCES), o;
    int16_t idx;
    if (!F_OK(list) || !F_OK(inst))
        return 0;
    idx = (int16_t)F_MEM16(list + 4u * (uint32_t)k + 2u);
    if (idx < 0)
        return 0;
    o = F_MEM32(inst + 12u * (uint32_t)idx + 8u);
    return F_OK(o) ? o : 0;
}

static int obj_destroyed(uint32_t o)
{
    return (int)(f_call(VA_GAMEOBJ_DESTROYED, o, 0, NULL) & 0xFFu);
}

/* BW_FIX_PROBE or a test aid: every 5 s of mission time, each target of the
 * objective being watched, by the world matrix (read-only). */
static void targets_report(uint32_t obj, uint32_t now)
{
    static uint32_t s_next;
    int n, k;
    char line[1024];
    if (now < s_next)
        return;
    s_next = now + 5000u;
    n = (int)(int8_t)F_MEM8(obj + 0x58);
    _snprintf(line, sizeof line, "[FIX] targets at %u ms:", now);
    for (k = 0; k < n; k++) {
        uint32_t o = objective_target(obj, k), node, m;
        char nm[40], one[96];
        if (!o)
            continue;
        node = F_MEM32(o + 4);
        m = F_OK(node) ? f_call(VA_NODE_WORLDMATRIX, node, 0, NULL) : 0;
        f_str(o + 0x34, nm, 16);
        _snprintf(one, sizeof one, " %s hp %d%s (%.0f, %.0f)", nm, (int)F_MEM32(o + 0x20),
                  obj_destroyed(o) ? " sunk" : "", F_OKM(m) ? f_f32(m + 0x30) : 0.0f,
                  F_OKM(m) ? f_f32(m + 0x34) : 0.0f);
        if (strlen(line) + strlen(one) + 1 < sizeof line)
            strcat(line, one);
    }
    fprintf(stderr, "%s\n", line);
    fflush(stderr);
}

/* 1 when M10's StrikeNearKerang can no longer be met by the game itself. */
static int m10_strike_stranded(uint32_t snk)
{
    static uint32_t s_edge_obj;
    static float s_edge_y;
    uint32_t edge;
    int n, k, afloat_in = 0, afloat_north = 0;

    edge = find_objective("FAIL_EdgeNorth");
    if (!edge)
        return 0;
    if (edge != s_edge_obj) {
        s_edge_obj = edge;
        s_edge_y = f_f32(edge + 0xB0);    /* the line's centre: y 3206.7 (data) */
        fprintf(stderr, "[FIX] M10_SECOND_STRIKE: watching the Strike Force (north fail line "
                "at y %.1f)\n", s_edge_y);
        fflush(stderr);
    }
    n = (int)(int8_t)F_MEM8(snk + 0x58);
    for (k = 0; k < n; k++) {
        uint32_t o = objective_target(snk, k), node, m;
        if (!o || obj_destroyed(o))
            continue;
        /* Where the game itself looks (ObjCond_MoveTo -> GameObj_IsInsideQuad
         * 0x8B000: the render item's world matrix, x/y at +0x30/+0x34). The
         * object's own +0x84/+0x88 stays where a boat sank until its physics
         * runs again (seen: run -d11m10r, a respawned KoiA3 "at" its wreck). */
        node = F_MEM32(o + 4);
        m = F_OK(node) ? f_call(VA_NODE_WORLDMATRIX, node, 0, NULL) : 0;
        if (!F_OKM(m)) {
            afloat_in++;                  /* can't tell: never fire on a guess */
            continue;
        }
        if (f_f32(m + 0x34) > s_edge_y)
            afloat_north++;
        else
            afloat_in++;
    }
    return afloat_in == 0 && afloat_north > 0;
}

/* ── Test aids ──────────────────────────────────────────────────────────
 * BW_FIXTEST_MEET=<s>:<name>|<name>[,<s>:<name>...]  from mission time <s>
 *   seconds, the named objectives count as met (if the game says not yet).
 * BW_FIXTEST_SINK=<s>:<objective>[,...]  once, at mission time <s>, every
 *   afloat target of that objective gets 0 hitpoints, and the game sinks it
 *   as if it were shot. */
typedef struct {
    uint32_t ms;
    char     names[256];
    int      done;
} f_test_t;

static f_test_t s_meet[8], s_sink[8], s_pick[8];
static int s_nmeet, s_nsink, s_npick, s_test_init;

static int parse_tests(const char *env, f_test_t *t, int max)
{
    const char *e = getenv(env);
    int n = 0;
    while (e && *e && n < max) {
        const char *c = strchr(e, ':'), *end = strchr(e, ',');
        size_t len;
        if (!c || (end && c > end))
            break;
        t[n].ms = (uint32_t)(atof(e) * 1000.0);
        c++;
        len = end ? (size_t)(end - c) : strlen(c);
        if (len >= sizeof t[n].names)
            len = sizeof t[n].names - 1;
        memcpy(t[n].names, c, len);
        t[n].names[len] = 0;
        t[n].done = 0;
        fprintf(stderr, "[FIXTEST] %s: at %u ms: %s\n", env, t[n].ms, t[n].names);
        n++;
        e = end ? end + 1 : NULL;
    }
    return n;
}

static int name_in_list(const char *list, uint32_t name_va)
{
    char nm[40];
    const char *p = list;
    size_t n;
    f_str(name_va, nm, sizeof nm);
    n = strlen(nm);
    while (*p) {
        const char *bar = strchr(p, '|');
        size_t len = bar ? (size_t)(bar - p) : strlen(p);
        if (len == n && n && !memcmp(p, nm, n))
            return 1;
        if (!bar)
            break;
        p = bar + 1;
    }
    return 0;
}

static void run_sinks(uint32_t now)
{
    int t;
    for (t = 0; t < s_nsink; t++) {
        uint32_t obj;
        int n, k, sunk = 0;
        if (s_sink[t].done || now < s_sink[t].ms)
            continue;
        s_sink[t].done = 1;
        obj = find_objective(s_sink[t].names);
        if (!obj) {
            fprintf(stderr, "[FIXTEST] sink: no objective '%s'\n", s_sink[t].names);
            continue;
        }
        n = (int)(int8_t)F_MEM8(obj + 0x58);
        for (k = 0; k < n; k++) {
            uint32_t o = objective_target(obj, k);
            if (o && !obj_destroyed(o) && F_MEM32(o + 0x20) > 0) {
                F_MEM32(o + 0x20) = 0;
                sunk++;
            }
        }
        fprintf(stderr, "[FIXTEST] sink at %u ms: %d of %d targets of '%s' set to 0 hitpoints\n",
                now, sunk, n, s_sink[t].names);
        fflush(stderr);
    }
}

/* BW_FIXTEST_PICKUP=<s>:<objective>[,...] (F2): once, at mission time <s>,
 * every target of that objective that is a power-up (instance type 5) and not
 * yet collected (+0xA4) is collected by the player's boat through the game's
 * own handler, Powerup_OnCollected 0x8D5A0 (thiscall, ecx = the power-up, one
 * argument = the collecting boat; World_OnContact 0x8C0A0 calls it on
 * contact; seen). Used to pick up M14's floating shield out of order. */
#define X_P1_BOAT_F2  0x001CEE60u
#define VA_POWERUP_ONCOLLECTED 0x0008D5A0u
static void run_picks(uint32_t now)
{
    int t;
    for (t = 0; t < s_npick; t++) {
        uint32_t obj, boat = F_MEM32(X_P1_BOAT_F2), list, inst;
        int n, k, got = 0;
        if (s_pick[t].done || now < s_pick[t].ms)
            continue;
        s_pick[t].done = 1;
        obj = find_objective(s_pick[t].names);
        list = obj ? F_MEM32(obj + 0x5C) : 0;
        inst = F_MEM32(X_INSTANCES);
        if (!obj || !F_OK(list) || !F_OK(inst) || !F_OK(boat)) {
            fprintf(stderr, "[FIXTEST] pickup: no objective '%s' or no player boat\n", s_pick[t].names);
            continue;
        }
        n = (int)(int8_t)F_MEM8(obj + 0x58);
        for (k = 0; k < n; k++) {
            int16_t idx = (int16_t)F_MEM16(list + 4u * (uint32_t)k + 2u);
            uint32_t e, o;
            if (idx < 0)
                continue;
            e = inst + 12u * (uint32_t)idx;
            o = F_MEM32(e + 8);
            if (F_MEM32(e + 4) != 5u || !F_OK(o) || F_MEM8(o + 0xA4))
                continue;
            f_call(VA_POWERUP_ONCOLLECTED, o, 1, &boat);
            got++;
        }
        fprintf(stderr, "[FIXTEST] pickup at %u ms: the player's boat collected %d of %d targets of "
                "'%s' (Powerup_OnCollected)\n", now, got, n, s_pick[t].names);
        fflush(stderr);
    }
}

/* M20_EDGE_GAP (G6; OFF by default, the maintainer's call: some players like exploring;
 * agent F2, docs/day1/F2.md).
 * Mission 20 has one edge fail line, EdgeFailSouth: a MOVE_TO box centred at
 * (-4.83, -1796.53), 5000 x 5 m (seen, analysis/missions/20.txt). The
 * mission area is 5000 m wide (x -2500..2500), so the line was meant to span
 * it, but sits 4.83 m off centre: x -2504.8..2495.2 (inferred: the width
 * equals the area's, and EdgeWarningSouth shares the off-centre x). Every
 * other side is land, except the south-east corner, where water reaches the
 * area's east edge just north of the line (seen, navmap). In runs (seen, F2,
 * test aid BW_FIXTEST_START): driving east at y -1700 the boat leaves the area
 * past x 2500 and sails on to x 4921 with nothing failing (-f2g6east);
 * driving south at x 2498 it crosses the line's latitude unseen and reaches
 * y -4100 (-f2g6gap); at x 2470 the same drive fails the mission (-f2g6ctl).
 * The fix: this EdgeFailSouth (name and centre must match) also counts as
 * met when the player boat is east of the mission area (x > 2500) or past the
 * line's latitude beyond its east end (x > 2495.2, y < -1794.0), so the game's
 * own out-of-bounds mission fail follows. Positions from the boat's
 * render matrix, as ObjCond_MoveTo reads them. */
static int m20_edge_gap(uint32_t obj)
{
    uint32_t o, node, m;
    float x, y;
    if (!F_OK(obj) || F_MEM8(obj) != 'E' || !f_streq(obj, "EdgeFailSouth"))
        return 0;
    if (fabsf(f_f32(obj + 0xAC) - (-4.8349f)) > 0.1f || fabsf(f_f32(obj + 0xB0) - (-1796.5308f)) > 0.1f)
        return 0;                          /* M08's and M19's lines have other centres */
    o = objective_target(obj, 0);          /* the 'Player' group: the player's boat */
    if (!o || obj_destroyed(o))
        return 0;
    node = F_MEM32(o + 4);
    m = F_OK(node) ? f_call(VA_NODE_WORLDMATRIX, node, 0, NULL) : 0;
    if (!F_OKM(m))
        return 0;
    x = f_f32(m + 0x30);
    y = f_f32(m + 0x34);
    if (x > 2500.0f || (x > 2495.2f && y < -1794.0f)) {
        fprintf(stderr, "[FIX] M20_EDGE_GAP: the player is at (%.1f, %.1f), outside the mission "
                "area past the south line's east end: EdgeFailSouth counts as met\n", x, y);
        fflush(stderr);
        return 1;
    }
    return 0;
}

extern void sub_0007F520_gen(void);

void sub_0007F520(void)
{
    uint32_t obj = F_MEM32(g_esp + 4);
    int fix = bw_fix_enabled(BW_FIX_M10_SECOND_STRIKE);
    int edge = bw_fix_enabled(BW_FIX_M20_EDGE_GAP);

    if (!s_test_init) {
        s_test_init = 1;
        s_nmeet = parse_tests("BW_FIXTEST_MEET", s_meet, 8);
        s_nsink = parse_tests("BW_FIXTEST_SINK", s_sink, 8);
        s_npick = parse_tests("BW_FIXTEST_PICKUP", s_pick, 8);
    }
    if (!fix && !edge && !s_nmeet && !s_nsink && !s_npick) {
        sub_0007F520_gen();                    /* Original: the body, nothing else */
        return;
    }
    sub_0007F520_gen();
    if (g_eax & 0xFFu)
        return;                                /* met as shipped */
    {
        f_regs_t r;
        uint32_t now = F_MEM32(X_MISSION_MS);
        int met = 0, t;
        f_save(&r);
        if (s_nsink)
            run_sinks(now);
        if (s_npick)
            run_picks(now);
        for (t = 0; t < s_nmeet && !met; t++)
            if (now >= s_meet[t].ms && name_in_list(s_meet[t].names, obj)) {
                char nm[40];
                met = 1;
                fprintf(stderr, "[FIXTEST] meet '%s' at %u ms\n", f_str(obj, nm, sizeof nm), now);
                fflush(stderr);
            }
        if (!met && (f_probe() || s_nmeet || s_nsink) && F_OK(obj) && F_MEM8(obj) == 'S'
            && f_streq(obj, "StrikeNearKerang"))
            targets_report(obj, now);
        if (!met && fix && F_OK(obj) && F_MEM8(obj) == 'S' && f_streq(obj, "StrikeNearKerang")
            && m10_strike_stranded(obj)) {
            met = 1;
            bw_fix_note_hit(BW_FIX_M10_SECOND_STRIKE);
            fprintf(stderr, "[FIX] M10_SECOND_STRIKE: no Strike Force boat is afloat in the "
                    "mission area and the second force waits north of it: StrikeNearKerang "
                    "counts as met at %u ms (the game's own second wave follows)\n", now);
            fflush(stderr);
        }
        if (!met && edge && m20_edge_gap(obj)) {
            met = 1;
            bw_fix_note_hit(BW_FIX_M20_EDGE_GAP);
        }
        f_restore(&r);
        if (met)
            g_eax = (g_eax & ~0xFFu) | 1u;
    }
}

/* ════════════════════════════════════════════════════════════════════════
 * M14_SHIELD_CUTSCENE (G10): a pickup's cutscene plays when the pickup
 * happens, even out of order (agent F2, docs/day1/F2.md section 7)
 * ════════════════════════════════════════════════════════════════════════
 *
 * Cinematics_Update 0x90D90 (seen): thiscall, ecx = the cinematics manager
 * 0x1D5BB8 (+0 playing byte, +0x24 count, +0x28 entries of 12 bytes: record,
 * trigger objective index or -1, played byte; record +0x40 bit 0 =
 * repeatable), one argument (the step), ret 4. When nothing plays, it plays
 * (Cinematics_Play 0x909F0, thiscall, the record) the first entry not yet
 * played whose trigger objective is met or failed, and only at the end of its
 * body. Objectives_UpdateAll only evaluates an objective that
 * Objectives_IsPrevStepMet 0x7DB30 (stdcall: order, display byte; ret 8)
 * lets through.
 * M14 (seen in the data): PRI_Pickup Talisman (PICKUP, order 3) waits for
 * Destroy_MunitionsDumpA (order 2), which waits for DumpB (order 1); its
 * cutscene Pickup_Sheild ("Worth the trouble", the camera on the shield's
 * spot) is triggered by it. The shield itself has no linked objective, so it
 * can be collected first; the objective then stays gated, and when depot A
 * dies it completes at once and the cutscene plays at the shield's empty spot
 * while the player is at the depot. The only PICKUP-triggered cutscene in the
 * game (seen: all 28 missions).
 * The fix (ON by default: the maintainer's call, 2026-10-27): after the body, when
 * nothing plays, an unplayed, non-repeatable cutscene whose trigger is a
 * PICKUP objective that is enabled, not met or failed, has all its power-ups
 * collected (the fraction ObjCond_Pickup 0x7E630 computes from +0xA4) and is
 * held by the order gate, is played then, as the body itself would have
 * played it: so it shows the pickup it was made for. The objective keeps its
 * order (the mission still needs the depots first); when it completes later,
 * the cutscene is already played and is not shown again. */
#define VA_CIN_PLAY        0x000909F0u
#define VA_PREV_STEP_MET   0x0007DB30u

static int pickup_complete(uint32_t obj)
{
    uint32_t list = F_MEM32(obj + 0x5C), inst = F_MEM32(X_INSTANCES);
    int n = (int)(int8_t)F_MEM8(obj + 0x58), k, got = 0;
    if (n <= 0 || !F_OK(list) || !F_OK(inst))
        return 0;
    for (k = 0; k < n; k++) {
        int16_t idx = (int16_t)F_MEM16(list + 4u * (uint32_t)k + 2u);
        uint32_t e, o;
        if (idx < 0)
            continue;
        e = inst + 12u * (uint32_t)idx;
        o = F_MEM32(e + 8);
        if (F_MEM32(e + 4) == 5u && F_OK(o) && F_MEM8(o + 0xA4))
            got++;
    }
    return got > 0 && (float)got / (float)n >= f_f32(obj + 0xAC);
}

extern void sub_00090D90_gen(void);

void sub_00090D90(void)            /* Cinematics_Update */
{
    uint32_t mgr = g_ecx, n, arr, objs, nobj, i;
    f_regs_t r;
    sub_00090D90_gen();
    if (!bw_fix_enabled(BW_FIX_M14_SHIELD_CUTSCENE) || !F_OK(mgr) || F_MEM8(mgr))
        return;                                  /* Original, or a cutscene is playing */
    n = F_MEM32(mgr + 0x24);
    arr = F_MEM32(mgr + 0x28);
    objs = F_MEM32(X_OBJ_ARRAY);
    nobj = F_MEM32(X_OBJ_COUNT);
    if (!n || n > 64 || !F_OK(arr) || !F_OK(objs) || nobj > 1024)
        return;
    f_save(&r);
    for (i = 0; i < n; i++) {
        uint32_t e = arr + 12u * i, rec = F_MEM32(e), idx = F_MEM32(e + 4), obj, args[2];
        char cn[40], on[40];
        if (idx == 0xFFFFFFFFu || idx >= nobj || F_MEM8(e + 8) || !F_OK(rec) || (F_MEM8(rec + 0x40) & 1u))
            continue;
        obj = objs + idx * X_OBJ_STRIDE;
        if (F_MEM32(obj + 0x50) != 4u || !F_MEM8(obj + 0x24) || F_MEM8(obj + 0x48) || F_MEM8(obj + 0x49))
            continue;                            /* not an enabled, open PICKUP trigger */
        if (!pickup_complete(obj))
            continue;
        args[0] = F_MEM32(obj + 0x3C);           /* order */
        args[1] = F_MEM8(obj + 0x4A);            /* display class */
        if (f_call(VA_PREV_STEP_MET, 0, 2, args) & 0xFFu)
            continue;                            /* the gate is open: the game meets it itself */
        f_call(VA_CIN_PLAY, mgr, 1, &rec);
        bw_fix_note_hit(BW_FIX_M14_SHIELD_CUTSCENE);
        fprintf(stderr, "[FIX] M14_SHIELD_CUTSCENE: the pickup for '%s' was collected out of order "
                "(the objective is still held by its order gate): its cutscene '%s' plays now, at "
                "the pickup, at %u ms\n",
                f_str(obj, on, sizeof on), f_str(rec, cn, sizeof cn), F_MEM32(X_MISSION_MS));
        fflush(stderr);
        break;
    }
    f_restore(&r);
}

/* ════════════════════════════════════════════════════════════════════════
 * M27_SHIELD_HIT_LINE: the shield's returned shot reports its own bit
 * ════════════════════════════════════════════════════════════════════════
 * Projectile_GetWeaponTypeMask 0x62670 (seen): ecx = the projectile; it reads
 * the weapon def ([ecx+0x38]) behaviour type +0xA8 and returns one bit per
 * type (torpedo 8, rocket 0x10, mine 0x20, cannon 0x40, wave 0x80, electric
 * 0x100, Dragon's Claw 0x200); every other type, TheShield (12) included,
 * gets 4. Its three callers (Projectile_HitBoat, Projectile_BlastHitsBoat,
 * Projectile_HitObject) pass the result straight to Objectives_NotifyContact
 * 0x7F3A0 and use it for nothing else (seen). Mission 27's
 * "M27A24_hit flagship" is a CONTACT on the Dragon from the Player with mask
 * 0x400, a bit nothing produces, so the shield-hit radio call
 * (27A24) never plays. 0x400 is the next bit after the Claw's and TheShield
 * the next type after the Claw: the bit meant for the shield's returned shot
 * (inferred). Fix: a TheShield shot reports 4 | 0x400, so every objective
 * that matched it still does and the 0x400 ones match too. Of the game's 119
 * CONTACT objectives only M27A24 changes (M05A15, M20A10 and M21A05 also
 * carry 0x400 but carry 4 as well; seen in the mission data). Original: the
 * body's result, untouched. BW_FIX_PROBE=1 logs the shield shots' masks in
 * any edition (read-only).
 * Seen (runs -m27fixS2, -m27fixS3): the returned blast's projectile def and
 * its turret's def are both type 12 and its owner is the player's boat; it
 * reported 0x4, now 0x404, M27A24 was met and "You hit the Dragon!" is heard
 * (template match 0.48-0.49, against 0.10-0.11 in runs without the hit). */
#define X_WTYPE_SHIELD 12u
extern void sub_00062670_gen(void);

void sub_00062670(void)                /* Projectile_GetWeaponTypeMask */
{
    static int nlog, nprobe;
    uint32_t proj = g_ecx, def, tur, tdef, ptype = 0, ttype = 0, mask;
    int fix = bw_fix_enabled(BW_FIX_M27_SHIELD_HIT_LINE), probe = f_probe();
    sub_00062670_gen();
    if (!fix && !probe)
        return;
    if (!F_OK(proj))
        return;
    /* the projectile's own weapon def (what the mask reads) and the firing
     * turret's def (what Projectile_GetWeaponType 0x62500 reads) */
    def = F_MEM32(proj + 0x38);
    if (F_OK(def))
        ptype = F_MEM32(def + 0xA8);
    tur = F_MEM32(proj + 0x98);
    if (F_OK(tur)) {
        tdef = F_MEM32(tur + 0x54);
        if (F_OK(tdef))
            ttype = F_MEM32(tdef + 0xA8);
    }
    mask = g_eax;
    if (probe && (ptype == 11u || ptype == 12u || ttype == 11u || ttype == 12u) && nprobe < 60) {
        nprobe++;
        fprintf(stderr, "[FIX] probe weapon mask (%s): projectile type %u, turret type %u, owner %08X, "
                "mask 0x%X at %u ms\n", bw_edition_name(bw_edition()), ptype, ttype,
                F_MEM32(proj + 0xA4), mask, F_MEM32(X_MISSION_MS));
        fflush(stderr);
    }
    if (ptype != X_WTYPE_SHIELD && ttype != X_WTYPE_SHIELD)
        return;
    if (fix) {
        g_eax = mask | 0x400u;
        bw_fix_note_hit(BW_FIX_M27_SHIELD_HIT_LINE);
    }
    if (nlog < 12) {
        nlog++;
        fprintf(stderr, "[FIX] M27_SHIELD_HIT_LINE (%s, %s): a shield shot hit: weapon mask 0x%X -> 0x%X "
                "at %u ms\n", bw_edition_name(bw_edition()), fix ? "on" : "off", mask, g_eax,
                F_MEM32(X_MISSION_MS));
        fflush(stderr);
    }
}
