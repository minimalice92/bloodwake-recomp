#ifndef BW_HOOKCHECK_H
#define BW_HOOKCHECK_H

/* The hook check (item 62, docs/025/CONTRACTS.md contract 7).
 *
 * The port attaches to the game by address: D-011 wrappers (sub_X around the
 * game's own sub_X_gen), game functions our code calls by address, and the
 * bytes the IMAGE fixes patch. On a copy whose program differs from the
 * retail one, an address can hold other code. Every such place is a row of
 * src/bw_hookcheck_table.h with the FNV-1a 64 hash of its shipped bytes
 * (tools/bw_hookcheck_table.py writes and checks the table).
 *
 * At start-up the check hashes each row in the program file's bytes (the
 * XBE as read from disc, before any IMAGE fix) and compares:
 *   - every row matches (the retail copy): one line, "[HOOKCHECK] n of n
 *     match", and nothing changes;
 *   - a row differs: each fix it names is turned off for this boot (as
 *     BW_FIX=-ID would), with one log line per fix. The port's own wrappers
 *     (PORT) stay attached, because every edition needs them; their mismatch
 *     is logged as a warning. TEST rows are test aids only.
 * It reads only the rows' own bytes.
 *
 * Test switch (every edition, off by default, never in a preset):
 *   BW_HOOKCHECK_TEST=<hex address>[,<hex address>...]   treat those rows
 *       as differing (their expected hash is falsified), to prove the
 *       mismatch path without a changed copy. It applies to the
 *       --settings-schema answer too. */

#include <stddef.h>

/* main.c, once per boot, right after the program file is read and before
 * the IMAGE fixes and the entry point (no guest thread runs yet). */
void bw_hookcheck_run(const void *xbe, size_t xbe_size);

/* 1 = every address the feature uses matched. feature: a fix ID from
 * src/bw_fixes.def ("HIFPS"; the check also turns it off itself), or a
 * named feature that isn't a fix: "PORT", "TEST", or one whose own code
 * asks here and skips its work ("CHEATS"). A feature with no row is 1.
 * Names a row may use: tools/bw_hookcheck_table.py (NAMED).
 * Before bw_hookcheck_run (e.g. in --settings-schema) the check runs on the
 * program file (BW_GAME_DIR\default.xbe, else game\disc\default.xbe),
 * silently; if that can't be read, every feature answers 1. */
int bw_hook_ok(const char *feature);

/* For the launcher's table (--settings-schema): NULL when fine, else
 * "off: this copy differs". id: a fix ID ("M17_CRATE_RESPAWN"), or a
 * setting's effect string, in which every "fix:+ID" token is tested
 * ("fix:+HIFPS;env:BW_HIFPS=240"). */
const char *bw_hook_unavailable(const char *id);

#endif
