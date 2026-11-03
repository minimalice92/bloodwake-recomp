/*
 * bw_keys.h - the player's keyboard and mouse bindings, steering ramp and
 * strength, and the pads' stick deadzones, from settings.ini [keys]
 * (docs/025/CONTRACTS.md contract 2; release 0.2.5 item 76, lane C3).
 *
 * The launcher (tools/launcher.pyw, lane A2) writes the section; it shows the
 * default table from `bloodwake.exe --keys-schema`, so it never keeps a copy.
 * With no [keys] section nothing changes: the toolkit's own map, no ramp, full
 * strength, no deadzones. Every edition (controls, not the game).
 */
#ifndef BW_KEYS_H
#define BW_KEYS_H

#include <stdio.h>

/* Read [keys] from the settings file in use (bw_settings_path()) and apply it.
 * Call once per boot, before the game polls a pad (main.c). Logs one "[KEYS]"
 * line saying what it applied, and one per name it didn't know. */
void bw_keys_apply(void);

/* The default table as JSON (bloodwake.exe --keys-schema). */
void bw_keys_print_schema(FILE *f);

/* First thing in WinMain, before --settings-schema and any game set-up: with
 * "--keys-schema" on the process's command line, prints the table to stdout,
 * flushes and returns 1 (the caller exits 0); else 0. */
int bw_keys_schema_main(void);

#endif
