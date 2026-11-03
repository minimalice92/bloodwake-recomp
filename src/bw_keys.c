/*
 * bw_keys.c - settings.ini [keys]: the player's keyboard and mouse bindings,
 * the steering ramp and strength, and the pads' stick deadzones (bw_keys.h;
 * docs/025/CONTRACTS.md contract 2; release 0.2.5 item 76, lane C3).
 *
 *   [keys]
 *   <control> = <key>[, <key>...]   up to 4 keys, or none; a missing line is
 *                                   the shipped default
 *   ramp_ms = 0..2000               a held steering key's time to full turn
 *   strength_pct = 10..100          how far a steering key turns
 *   deadzone_left = 0..95           percent, real pads only; 0 = none
 *   deadzone_right = 0..95
 *
 * Controls and keys are the toolkit's names (input_map.c), case-insensitive,
 * plus mouse_left, mouse_right and mouse_middle for mouse1-mouse3 (the names
 * the launcher writes; mouse4 and mouse5 are the toolkit's own). The
 * bindings go to the carried keyboard.c (xbox_InputKeyboardConfigure), the
 * deadzones to the carried xinput_device.c (xbox_InputSetDeadzones).
 *
 * "Steering keys" are the keys bound to left_stick_left and left_stick_right:
 * the rudder in every control scheme (docs/GAME.md, "Controls"). The ramp and
 * strength leave the throttle (left stick up/down in scheme A), the D-pad and
 * the right stick alone (lane C3 call; docs/025/C3.md).
 *
 * The keyboard itself is still switched on by "Keyboard as a controller"
 * (RECOMP_KEYBOARD=1); these bindings say what its keys do.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "bw_keys.h"
#include "bw_settings.h"
#include "input_map.h"                       /* toolkit src/input */
#include "toolkit_patches/input/bw_input_ext.h"

/* The launcher's mouse names (contract 2) and the toolkit's for the same
 * codes. */
static const struct { const char *ours, *toolkit; int vk; } k_mouse[] = {
    { "mouse_left",   "mouse1", XBOX_VK_LBUTTON },
    { "mouse_right",  "mouse2", XBOX_VK_RBUTTON },
    { "mouse_middle", "mouse3", XBOX_VK_MBUTTON },
};

/* The guest control each line names, as the toolkit's README names it
 * (keyboard.c's map), for the launcher's list. In the toolkit's order. */
static const char *const k_labels[INPUT_CONTROL_COUNT] = {
    "D-pad up", "D-pad down", "D-pad left", "D-pad right",
    "START", "BACK", "Left stick click", "Right stick click",
    "A", "B", "X", "Y", "Black", "White",
    "Left trigger", "Right trigger",
    "Left stick up", "Left stick down", "Left stick left", "Left stick right",
    "Right stick up", "Right stick down", "Right stick left", "Right stick right",
};

/* A key's name as the launcher writes it. */
static const char *key_name(int vk)
{
    size_t i;
    for (i = 0; i < sizeof k_mouse / sizeof k_mouse[0]; i++)
        if (k_mouse[i].vk == vk)
            return k_mouse[i].ours;
    return xbox_InputMap_KeyName(vk);
}

/* A key's code from either spelling, or -1. */
static int key_code(const char *name)
{
    size_t i;
    for (i = 0; i < sizeof k_mouse / sizeof k_mouse[0]; i++)
        if (!_stricmp(name, k_mouse[i].ours))
            return k_mouse[i].vk;
    return xbox_InputMap_KeyFromName(name);
}

static void trim(char *s)
{
    char *e;
    while (*s && isspace((unsigned char)*s))
        memmove(s, s + 1, strlen(s));
    e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1]))
        *--e = 0;
}

/* "Z, space" -> the control's keys. Unknown names are logged and skipped; a
 * line with names but none this build knows keeps the shipped keys (a newer
 * launcher's name shouldn't leave a control dead). "none" or "" is no key. */
static void parse_keys(InputBindings *b, const InputBindings *def, int control,
                       const char *text, int *unknown)
{
    char buf[256], *tok, *ctx = NULL;
    int before = *unknown, named = 0;
    b->count[control] = 0;
    _snprintf(buf, sizeof buf, "%s", text);
    buf[sizeof buf - 1] = 0;
    for (tok = strtok_s(buf, ",", &ctx); tok; tok = strtok_s(NULL, ",", &ctx)) {
        int vk, j, dup = 0;
        trim(tok);
        if (!*tok || !_stricmp(tok, "none"))
            continue;
        named++;
        vk = key_code(tok);
        if (vk <= 0 || vk > 255) {
            fprintf(stderr, "[KEYS] %s: no key called \"%s\"; skipped\n",
                    xbox_InputMap_ControlName(control), tok);
            (*unknown)++;
            continue;
        }
        for (j = 0; j < b->count[control]; j++)
            dup |= b->key[control][j] == vk;
        if (dup)
            continue;
        if (b->count[control] >= INPUT_BINDINGS_PER_CONTROL) {
            fprintf(stderr, "[KEYS] %s: more than %d keys; \"%s\" skipped\n",
                    xbox_InputMap_ControlName(control), INPUT_BINDINGS_PER_CONTROL, tok);
            (*unknown)++;
            continue;
        }
        b->key[control][b->count[control]++] = (uint8_t)vk;
    }
    if (named && !b->count[control] && *unknown > before) {
        memcpy(b->key[control], def->key[control], sizeof b->key[control]);
        b->count[control] = def->count[control];
        fprintf(stderr, "[KEYS] %s: none of its keys known; keeping the shipped keys\n",
                xbox_InputMap_ControlName(control));
    }
}

static int clamp_int(const char *key, const char *text, int lo, int hi)
{
    char *end;
    long v = strtol(text, &end, 10);
    if (end == text) {
        fprintf(stderr, "[KEYS] %s = \"%s\" is not a number; using %d\n", key, text, lo);
        return lo;
    }
    if (v < lo || v > hi) {
        long c = v < lo ? lo : hi;
        fprintf(stderr, "[KEYS] %s = %ld is outside %d..%d; using %ld\n", key, v, lo, hi, c);
        v = c;
    }
    return (int)v;
}

static void keys_text(const InputBindings *b, int control, char *out, size_t n)
{
    int i;
    size_t used = 0;
    out[0] = 0;
    if (!b->count[control]) {
        _snprintf(out, n, "none");
        out[n - 1] = 0;
        return;
    }
    for (i = 0; i < b->count[control] && used + 1 < n; i++) {
        const char *nm = key_name(b->key[control][i]);
        int w = _snprintf(out + used, n - used, "%s%s", i ? ", " : "", nm ? nm : "?");
        if (w < 0)
            break;
        used += (size_t)w;
    }
    out[n - 1] = 0;
}

void bw_keys_apply(void)
{
    const char *path = bw_settings_path();
    FILE *f;
    char line[512], sec[32] = "";
    InputBindings b, def;
    int in_keys = 0, have = 0, unknown = 0, c;
    int ramp = 0, strength = 100, dzl = 0, dzr = 0;
    static char log[2048];
    size_t used;

    /* wide: bw_settings_path() is UTF-8 since 0.2.5 item 18 (a user name like Zoë) */
    if (!path || !(f = _wfopen(bw_settings_path_w(), L"r")))
        return;                               /* no file: the defaults, silently */
    xbox_InputKeyboardBindings(&def);
    b = def;
    while (fgets(line, sizeof line, f)) {
        char *eq;
        trim(line);
        if (!line[0] || line[0] == ';' || line[0] == '#')
            continue;
        if (line[0] == '[') {
            char *e = strchr(line, ']');
            if (e)
                *e = 0;
            _snprintf(sec, sizeof sec, "%s", line + 1);
            sec[sizeof sec - 1] = 0;
            trim(sec);
            in_keys = !_stricmp(sec, "keys");
            have |= in_keys;
            continue;
        }
        if (!in_keys || !(eq = strchr(line, '=')))
            continue;
        *eq = 0;
        trim(line);
        trim(eq + 1);
        {
            char *v = eq + 1, *sc = strchr(v, ';');     /* a trailing comment */
            if (sc) {
                *sc = 0;
                trim(v);
            }
            if (!_stricmp(line, "ramp_ms"))
                ramp = clamp_int(line, v, 0, 2000);
            else if (!_stricmp(line, "strength_pct"))
                strength = clamp_int(line, v, 10, 100);
            else if (!_stricmp(line, "deadzone_left"))
                dzl = clamp_int(line, v, 0, 95);
            else if (!_stricmp(line, "deadzone_right"))
                dzr = clamp_int(line, v, 0, 95);
            else if ((c = xbox_InputMap_ControlFromName(line)) >= 0)
                parse_keys(&b, &def, c, v, &unknown);
            else {
                fprintf(stderr, "[KEYS] no control called \"%s\"; line skipped\n", line);
                unknown++;
            }
        }
    }
    fclose(f);
    if (!have)
        return;                               /* no [keys]: exactly as before */

    xbox_InputKeyboardConfigure(&b, ramp, strength);
    xbox_InputSetDeadzones(dzl, dzr);

    used = (size_t)_snprintf(log, sizeof log, "[KEYS] %s [keys]: ", path);
    {
        int changed = 0;
        for (c = 0; c < INPUT_CONTROL_COUNT && used + 64 < sizeof log; c++) {
            char t[128];
            if (b.count[c] == def.count[c]
                && !memcmp(b.key[c], def.key[c], b.count[c]))
                continue;
            keys_text(&b, c, t, sizeof t);
            used += (size_t)_snprintf(log + used, sizeof log - used, "%s%s = %s",
                                      changed++ ? "; " : "", xbox_InputMap_ControlName(c), t);
        }
        if (!changed)
            used += (size_t)_snprintf(log + used, sizeof log - used, "the default keys");
    }
    if (used < sizeof log)
        _snprintf(log + used, sizeof log - used,
                  "; steering ramp %d ms, strength %d%%; stick deadzones left %d%% right %d%%%s",
                  ramp, strength, dzl, dzr, unknown ? " (some names skipped, above)" : "");
    log[sizeof log - 1] = 0;
    fprintf(stderr, "%s\n", log);
}

int bw_keys_schema_main(void)
{
    if (!strstr(GetCommandLineA(), "--keys-schema"))
        return 0;
    bw_keys_print_schema(stdout);
    fflush(stdout);
    return 1;
}

void bw_keys_print_schema(FILE *out)
{
    InputBindings def;
    int c, vk, first = 1;
    size_t i;
    static const char *const aliases[] = { "mouse1", "mouse2", "mouse3", "return", "control", "esc" };

    xbox_InputKeyboardBindings(&def);
    fprintf(out, "{\"controls\": [");
    for (c = 0; c < INPUT_CONTROL_COUNT; c++) {
        int k;
        fprintf(out, "%s{\"name\": \"%s\", \"label\": \"%s\", \"keys\": [", c ? ", " : "",
                xbox_InputMap_ControlName(c), k_labels[c]);
        for (k = 0; k < def.count[c]; k++)
            fprintf(out, "%s\"%s\"", k ? ", " : "", key_name(def.key[c][k]));
        fprintf(out, "]}");
    }
    fprintf(out, "],\n \"keys\": [");
    for (vk = 1; vk < 256; vk++) {
        const char *nm = key_name(vk);
        if (!nm)
            continue;
        fprintf(out, "%s\"%s\"", first ? "" : ", ", nm);
        first = 0;
    }
    for (i = 0; i < sizeof aliases / sizeof aliases[0]; i++)
        if (xbox_InputMap_KeyFromName(aliases[i]) > 0)
            fprintf(out, ", \"%s\"", aliases[i]);
    fprintf(out, "],\n \"ramp_ms\": {\"default\": 0, \"min\": 0, \"max\": 2000},"
                 "\n \"strength_pct\": {\"default\": 100, \"min\": 10, \"max\": 100},"
                 "\n \"deadzone_left\": {\"default\": 0, \"min\": 0, \"max\": 95},"
                 "\n \"deadzone_right\": {\"default\": 0, \"min\": 0, \"max\": 95},"
                 "\n \"steering\": [\"left_stick_left\", \"left_stick_right\"]}\n");
}
