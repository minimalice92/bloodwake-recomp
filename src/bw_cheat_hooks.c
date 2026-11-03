/* bw_cheat_hooks.c - the launcher's Cheats tab in the game (settings.ini
 * [cheats]; docs/025/CONTRACTS.md contract 3; release 0.2.5 item 77, lane C3).
 *
 * The cheat codes typed at the title (docs/GAME.md, "Cheat codes") all set
 * bits of one dword, 0x1E5F14 (seen: Cheats_MatchCodes 0x3F9E0 ORs them in,
 * the only direct writer in the program). It sits in the game object at
 * 0x1E5CF0, whose +8.. Title_Boot (0xAA9C0) fills from a "SPLASH" boot's
 * launch data (seen: rep movsd of 0x2EE dwords to [ecx+8]), so launch data
 * +540 lands on it, and FrontEnd_CopyLaunchData hands it to the next boot:
 * that is how a typed code rides every reboot.
 *
 * With [cheats] enabled = 1 the bits are ORed into that dword once, on the
 * session's first boot, right after Title_Boot has read the launch data and
 * before anything reads the cheats: the end of System_Init (0xAB970, called
 * once, from Title_Boot, after the launch data; seen). That is the state
 * typing the codes gives, so from there the game carries them itself, and
 * the game's own rules apply (visiting Game Demos clears them, as on the
 * console). Codes typed at the title still work and add to them.
 *
 * "First boot": the session's first boot, not one the game rebooted into.
 * The port's launchers start those with BW_LAUNCH_DATA_FILE set to the game's
 * own relaunch record (bw_launch_next.bin from the host window and play.py
 * --windows, bw_relaunch.bin from bw_run.py's later stages); every other boot
 * (none set, or a chosen launch file) is a first boot.
 *
 * Every edition: the codes are the game's own. The hidden level select
 * (0x008, no code sets it, and saving is off while it's on) only outside
 * Original: Original masks it off, with a log line.
 *
 * tools/run_pipeline.py hands this file to the generator (bw_*_hooks.c), so
 * System_Init is emitted as sub_000AB970_gen and its caller reaches the
 * wrapper. With [cheats] off or missing the wrapper calls the body and
 * nothing else. The hook check (item 62) has System_Init and
 * Cheats_MatchCodes as rows tagged CHEATS: on a copy that differs there, the
 * launcher's cheats stay off.
 *
 * Claims are labelled as in the docs: seen, inferred, unknown.
 */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "bw_edition.h"
#include "bw_settings.h"
#include "bw_hookcheck.h"       /* item 62: rows tagged CHEATS (System_Init, Cheats_MatchCodes) */

extern ptrdiff_t g_xbox_mem_offset;
#define C_MEM32(va) (*(volatile uint32_t *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))

#define BW_CHEAT_DWORD   0x001E5F14u     /* game object 0x1E5CF0 + 8 + 540 */
#define BW_CHEAT_KNOWN   0x00000FFFu     /* the twelve bits GAME.md names */
#define BW_CHEAT_LEVELS  0x00000008u     /* the hidden level select */

static const struct { uint32_t bit; const char *name; } k_cheats[] = {
    { 0x001, "Unlimited Ammo" }, { 0x002, "Unlimited Turbo" }, { 0x004, "Invincibility" },
    { 0x008, "level select" },   { 0x010, "All Boats" },       { 0x020, "All Battle Modes" },
    { 0x040, "All Arenas" },     { 0x080, "Rubber Duck (boat)" }, { 0x100, "Import" },
    { 0x200, "Blood Ball" },     { 0x400, "Rubber Duck (mode)" }, { 0x800, "Puffer Fish" },
};

/* Not a boot the game rebooted into (see the top). */
static int first_boot(void)
{
    const char *lf = getenv("BW_LAUNCH_DATA_FILE");
    const char *base;
    if (!lf || !*lf)
        return 1;
    base = lf + strlen(lf);
    while (base > lf && base[-1] != '\\' && base[-1] != '/')
        base--;
    return _stricmp(base, "bw_launch_next.bin") && _stricmp(base, "bw_relaunch.bin");
}

/* settings.ini [cheats]: 1 with *bits set when enabled = 1, else 0. */
static int read_cheats(uint32_t *bits, const char **path_out)
{
    const char *path = bw_settings_path();
    char line[512];
    int in = 0, enabled = 0, have_bits = 0;
    FILE *f;
    *bits = 0;
    *path_out = path;
    /* wide: bw_settings_path() is UTF-8 since 0.2.5 item 18 (a user name like Zoë) */
    if (!path || !(f = _wfopen(bw_settings_path_w(), L"r")))
        return 0;
    while (fgets(line, sizeof line, f)) {
        char *p = line, *eq, *e;
        while (isspace((unsigned char)*p))
            p++;
        e = p + strlen(p);
        while (e > p && isspace((unsigned char)e[-1]))
            *--e = 0;
        if (!*p || *p == ';' || *p == '#')
            continue;
        if (*p == '[') {
            in = !_strnicmp(p, "[cheats]", 8);
            continue;
        }
        if (!in || !(eq = strchr(p, '=')))
            continue;
        *eq = 0;
        for (e = eq; e > p && isspace((unsigned char)e[-1]); )
            *--e = 0;
        eq++;
        while (isspace((unsigned char)*eq))
            eq++;
        if (!_stricmp(p, "enabled"))
            enabled = atoi(eq) == 1;
        else if (!_stricmp(p, "bits")) {
            *bits = (uint32_t)strtoul(eq, NULL, 0);
            have_bits = 1;
        }
    }
    fclose(f);
    return enabled && have_bits;
}

static void bw_cheats_apply(void)
{
    uint32_t bits, before, extra;
    const char *path;
    char names[256];
    size_t used = 0, i;

    if (!read_cheats(&bits, &path))
        return;                                  /* off or missing: nothing */
    if (!bw_hook_ok("CHEATS")) {
        /* The code that defines the cheat dword isn't the retail program's
         * here, so 0x1E5F14 may be something else: leave it alone. */
        fprintf(stderr, "[CHEATS] settings [cheats] on, but this copy of the game differs where "
                "the cheats live; the launcher's cheats are off (typed codes still work)\n");
        return;
    }
    if (!first_boot()) {
        fprintf(stderr, "[CHEATS] settings [cheats] on; a boot the game rebooted into: its own "
                "launch data carries the cheats (0x%03X now)\n",
                (unsigned)C_MEM32(BW_CHEAT_DWORD));
        return;
    }
    extra = bits & ~BW_CHEAT_KNOWN;
    if (extra) {
        fprintf(stderr, "[CHEATS] bits 0x%X are no cheat the game is known to have; ignored\n",
                (unsigned)extra);
        bits &= BW_CHEAT_KNOWN;
    }
    if ((bits & BW_CHEAT_LEVELS) && bw_is_original()) {
        fprintf(stderr, "[CHEATS] the level select (0x008) is not part of Original "
                "(no code sets it); left off\n");
        bits &= ~BW_CHEAT_LEVELS;
    }
    names[0] = 0;
    for (i = 0; i < sizeof k_cheats / sizeof k_cheats[0] && used + 32 < sizeof names; i++)
        if (bits & k_cheats[i].bit)
            used += (size_t)_snprintf(names + used, sizeof names - used, "%s%s",
                                      used ? ", " : "", k_cheats[i].name);
    before = C_MEM32(BW_CHEAT_DWORD);
    C_MEM32(BW_CHEAT_DWORD) = before | bits;
    fprintf(stderr, "[CHEATS] settings [cheats] on: 0x%03X (%s) added to this first boot's "
            "cheats (0x%03X before, 0x%03X now)\n", (unsigned)bits, used ? names : "none",
            (unsigned)before, (unsigned)C_MEM32(BW_CHEAT_DWORD));
}

/* System_Init, 0x000AB970 (thiscall, ecx = 0x1E6980, no stack arguments),
 * called once per boot by Title_Boot after the launch data is in place. */
extern void sub_000AB970_gen(void);
void sub_000AB970(void)
{
    sub_000AB970_gen();
    bw_cheats_apply();
}
