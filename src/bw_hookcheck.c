/* bw_hookcheck.c - the hook check (item 62; bw_hookcheck.h, contract 7).
 *
 * One table (bw_hookcheck_table.h) of every place the port attaches to the
 * game by address, each with the FNV-1a 64 hash of the retail program's bytes
 * there. Hashed from the program file, so the IMAGE fixes (which patch guest
 * memory later) never change the answer, and --settings-schema gets the same
 * answer without starting the game.
 *
 * A row's features are fix IDs (turned off here), or named features that
 * aren't fixes: PORT and TEST (logged only), and ones whose own code asks
 * bw_hook_ok() and skips its work (CHEATS). tools/bw_hookcheck_table.py
 * refuses any other name, so a typo can't pass silently.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "bw_edition.h"
#include "bw_hookcheck.h"

typedef enum { HC_HOOK, HC_USE, HC_DATA } hc_kind_t;

typedef struct {
    hc_kind_t   kind;
    uint32_t    va, len;
    uint64_t    hash;
    const char *features;       /* comma list */
    const char *where;
} hc_row_t;

static const hc_row_t k_rows[] = {
#define BW_HOOKCHECK_ROW(kind, va, len, hash, features, where) { HC_##kind, va, len, hash, features, where },
#include "bw_hookcheck_table.h"
#undef BW_HOOKCHECK_ROW
};
#define HC_NROWS ((int)(sizeof k_rows / sizeof k_rows[0]))

static int s_state;                         /* 0 not run, 1 run, -1 no program to read */
static unsigned char s_row_bad[HC_NROWS];
static unsigned char s_fix_bad[BW_FIX_COUNT];
/* Named features (not fixes) with a row that differs. */
#define HC_NAMED 32
static struct { const char *s; size_t n; } s_named_bad[HC_NAMED];
static int s_nnamed;

static const char *k_unavailable = "off: this copy differs";

static uint64_t fnv1a64(const uint8_t *p, size_t n)
{
    uint64_t h = 0xCBF29CE484222325ull;
    while (n--) {
        h ^= *p++;
        h *= 0x100000001B3ull;
    }
    return h;
}

static uint32_t rd32(const uint8_t *b, size_t size, size_t off, int *ok)
{
    if (off + 4 > size) {
        *ok = 0;
        return 0;
    }
    return (uint32_t)b[off] | ((uint32_t)b[off + 1] << 8) | ((uint32_t)b[off + 2] << 16)
        | ((uint32_t)b[off + 3] << 24);
}

/* The file bytes of [va, va + len) when one section's raw data holds all of
 * them (the XBE header: base +0x104, section count +0x11C, headers +0x120;
 * each 0x38 bytes: +4 address, +0xC file offset, +0x10 file size). */
static const uint8_t *xbe_bytes(const uint8_t *b, size_t size, uint32_t va, uint32_t len)
{
    int ok = 1;
    uint32_t base, n, sh, i;
    if (size < 0x124 || memcmp(b, "XBEH", 4))
        return NULL;
    base = rd32(b, size, 0x104, &ok);
    n = rd32(b, size, 0x11C, &ok);
    sh = rd32(b, size, 0x120, &ok);
    if (!ok || sh < base || n > 256)
        return NULL;
    for (i = 0; i < n; i++) {
        size_t h = (size_t)(sh - base) + (size_t)i * 0x38u;
        uint32_t sva = rd32(b, size, h + 4, &ok), raw = rd32(b, size, h + 0xC, &ok);
        uint32_t rsz = rd32(b, size, h + 0x10, &ok);
        if (!ok)
            return NULL;
        if (va >= sva && (uint64_t)va + len <= (uint64_t)sva + rsz
            && (uint64_t)raw + rsz <= size)
            return b + raw + (va - sva);
    }
    return NULL;
}

/* BW_HOOKCHECK_TEST: is this row's expected hash to be falsified? */
static int test_falsified(uint32_t va)
{
    const char *e = getenv("BW_HOOKCHECK_TEST");
    while (e && *e) {
        char *end;
        unsigned long v = strtoul(e, &end, 16);
        if (end == e)
            break;
        if ((uint32_t)v == va)
            return 1;
        e = end;
        while (*e == ',' || *e == ' ')
            e++;
    }
    return 0;
}

static int fix_by_name(const char *s, size_t n)
{
    int i;
    for (i = 0; i < BW_FIX_COUNT; i++)
        if (strlen(g_bw_fixes[i].name) == n && !strncmp(g_bw_fixes[i].name, s, n))
            return i;
    return -1;
}

static int named_bad(const char *s, size_t n)
{
    int i;
    for (i = 0; i < s_nnamed; i++)
        if (s_named_bad[i].n == n && !strncmp(s_named_bad[i].s, s, n))
            return 1;
    return 0;
}

/* Does the comma list name this feature (whole names only)? */
static int list_has(const char *list, const char *name)
{
    size_t n = strlen(name);
    while (list && *list) {
        const char *c = strchr(list, ',');
        size_t m = c ? (size_t)(c - list) : strlen(list);
        if (m == n && !strncmp(list, name, n))
            return 1;
        list = c ? c + 1 : NULL;
    }
    return 0;
}

/* Every feature of a differing row: a fix, or a named feature. */
static void mark_bad(const char *list)
{
    while (list && *list) {
        const char *c = strchr(list, ',');
        size_t n = c ? (size_t)(c - list) : strlen(list);
        int f = n ? fix_by_name(list, n) : -1;
        if (f >= 0)
            s_fix_bad[f] = 1;
        else if (n && !named_bad(list, n) && s_nnamed < HC_NAMED) {
            s_named_bad[s_nnamed].s = list;
            s_named_bad[s_nnamed].n = n;
            s_nnamed++;
        }
        list = c ? c + 1 : NULL;
    }
}

static void check_bytes(const uint8_t *b, size_t size)
{
    int i;
    memset(s_row_bad, 0, sizeof s_row_bad);
    memset(s_fix_bad, 0, sizeof s_fix_bad);
    s_nnamed = 0;
    for (i = 0; i < HC_NROWS; i++) {
        const hc_row_t *r = &k_rows[i];
        const uint8_t *p = xbe_bytes(b, size, r->va, r->len);
        uint64_t want = r->hash ^ (test_falsified(r->va) ? 1u : 0u);
        s_row_bad[i] = !p || fnv1a64(p, r->len) != want;
        if (s_row_bad[i])
            mark_bad(r->features);
    }
    s_state = 1;
}

/* Without a boot (--settings-schema): read the program file, quietly. */
static void run_from_file(void)
{
    char path[520];
    const char *dir = getenv("BW_GAME_DIR");
    FILE *f;
    uint8_t *b;
    long n;
    if (dir && *dir)
        _snprintf(path, sizeof path, "%s\\default.xbe", dir);
    else
        _snprintf(path, sizeof path, "game\\disc\\default.xbe");
    path[sizeof path - 1] = 0;
    s_state = -1;
    f = fopen(path, "rb");
    if (!f)
        return;
    if (fseek(f, 0, SEEK_END) || (n = ftell(f)) <= 0 || n > (64L << 20) || fseek(f, 0, SEEK_SET)) {
        fclose(f);
        return;
    }
    b = (uint8_t *)malloc((size_t)n);
    if (b && fread(b, 1, (size_t)n, f) == (size_t)n)
        check_bytes(b, (size_t)n);
    free(b);
    fclose(f);
}

static void ensure_run(void)
{
    if (!s_state)
        run_from_file();
}

/* ---- the boot's check ----------------------------------------------------- */

/* "0x..., 0x..." of the differing rows that name this feature. */
static void differing_at(const char *name, char *at, size_t len)
{
    size_t al = 0;
    int i;
    at[0] = 0;
    for (i = 0; i < HC_NROWS; i++)
        if (s_row_bad[i] && list_has(k_rows[i].features, name) && al + 16 < len)
            al += (size_t)_snprintf(at + al, len - al, "%s0x%08X", al ? ", " : "", k_rows[i].va);
}

static void add_off(char *off, size_t len, size_t *used, const char *name)
{
    if (*used + strlen(name) + 3 < len)
        *used += (size_t)_snprintf(off + *used, len - *used, "%s%s", *used ? ", " : "", name);
}

/* Item 95: the port's own wrappers stay attached on a copy whose code differs
 * (call C2-1), so such a copy may well crash. The player is told once, in
 * plain words, at the session's first boot: a cold start, with no launch data
 * from a previous boot (a later boot gets the host's BW_LAUNCH_DATA_FILE).
 * Never with BW_UNATTENDED (tests); the log says either way. The box is owned
 * by the play window (BW_PARENT_HWND) when there is one. */
static void tell_player(int nport)
{
    const char *ld = getenv("BW_LAUNCH_DATA_FILE"), *ph = getenv("BW_PARENT_HWND");
    wchar_t msg[512];
    HWND owner = NULL;
    if (getenv("BW_UNATTENDED")) {
        fprintf(stderr, "[HOOKCHECK] the player would be told that this copy differs in %d place%s "
                "the port needs (not shown: BW_UNATTENDED)\n", nport, nport == 1 ? "" : "s");
        return;
    }
    if (ld && *ld) {
        fprintf(stderr, "[HOOKCHECK] not a first boot (it has launch data): the player was told at "
                "the session's first boot\n");
        return;
    }
    if (ph && *ph)
        owner = (HWND)(uintptr_t)_strtoui64(ph, NULL, 0);
    _snwprintf(msg, sizeof msg / sizeof msg[0],
               L"This copy's program differs from the retail Blood Wake in %d place%s the port "
               L"needs, so it may not run correctly.\n\n"
               L"You chose to build it anyway. If it crashes, a bug report with play.log helps.",
               nport, nport == 1 ? L"" : L"s");
    msg[sizeof msg / sizeof msg[0] - 1] = 0;
    fprintf(stderr, "[HOOKCHECK] told the player: this copy differs in %d place%s the port needs\n",
            nport, nport == 1 ? "" : "s");
    fflush(stderr);
    MessageBoxW(owner, msg, L"Blood Wake Recomp", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
}

void bw_hookcheck_run(const void *xbe, size_t xbe_size)
{
    int i, f, bad = 0, nport;
    char off[512], at[160];
    size_t used = 0;

    check_bytes((const uint8_t *)xbe, xbe_size);
    for (i = 0; i < HC_NROWS; i++)
        bad += s_row_bad[i];
    if (getenv("BW_HOOKCHECK_TEST") && *getenv("BW_HOOKCHECK_TEST"))
        fprintf(stderr, "[HOOKCHECK] test switch BW_HOOKCHECK_TEST=%s: those rows count as "
                "differing\n", getenv("BW_HOOKCHECK_TEST"));

    /* One line per fix turned off, naming the addresses that differ. */
    off[0] = 0;
    for (f = 0; f < BW_FIX_COUNT; f++) {
        if (!s_fix_bad[f])
            continue;
        differing_at(g_bw_fixes[f].name, at, sizeof at);
        fprintf(stderr, "[HOOKCHECK] %s off: this copy's program differs from the retail one "
                "at %s%s\n", g_bw_fixes[f].name, at,
                g_bw_fix_on[f] ? "" : " (it was off already)");
        g_bw_fix_on[f] = 0;
        add_off(off, sizeof off, &used, g_bw_fixes[f].name);
    }
    /* Named features: their own code asks bw_hook_ok() and skips its work. */
    for (f = 0; f < s_nnamed; f++) {
        char name[64];
        _snprintf(name, sizeof name, "%.*s", (int)s_named_bad[f].n, s_named_bad[f].s);
        name[sizeof name - 1] = 0;
        if (!strcmp(name, "PORT") || !strcmp(name, "TEST"))
            continue;
        differing_at(name, at, sizeof at);
        fprintf(stderr, "[HOOKCHECK] %s off: this copy's program differs from the retail one "
                "at %s\n", name, at);
        add_off(off, sizeof off, &used, name);
    }
    for (i = 0, nport = 0; i < HC_NROWS; i++)
        if (s_row_bad[i] && list_has(k_rows[i].features, "PORT")) {
            nport++;
            fprintf(stderr, "[HOOKCHECK] warning: the port's own hook at 0x%08X (%s) differs from "
                    "the retail program; it stays attached, since every edition needs it, so this "
                    "copy may not run correctly\n", k_rows[i].va, k_rows[i].where);
        }
    if (nport)
        tell_player(nport);
    for (i = 0; i < HC_NROWS; i++)
        if (s_row_bad[i] && list_has(k_rows[i].features, "TEST"))
            fprintf(stderr, "[HOOKCHECK] the test aid at 0x%08X (%s) differs from the retail "
                    "program (test switches only)\n", k_rows[i].va, k_rows[i].where);
    if (!bad)
        fprintf(stderr, "[HOOKCHECK] %d of %d match: this is the retail program's code at every "
                "place the port attaches\n", HC_NROWS, HC_NROWS);
    else
        fprintf(stderr, "[HOOKCHECK] %d of %d match; off for this copy: %s%s\n", HC_NROWS - bad,
                HC_NROWS, used ? off : "nothing",
                named_bad("PORT", 4) ? "; the port's own hooks differ too" : "");
    fflush(stderr);
}

/* ---- queries ---------------------------------------------------------------- */

int bw_hook_ok(const char *feature)
{
    int f;
    if (!feature)
        return 1;
    ensure_run();
    if (s_state != 1)
        return 1;
    f = fix_by_name(feature, strlen(feature));
    if (f >= 0)
        return !s_fix_bad[f];
    return !named_bad(feature, strlen(feature));
}

const char *bw_hook_unavailable(const char *id)
{
    const char *p;
    if (!id || !*id)
        return NULL;
    if (!strstr(id, "fix:"))
        return bw_hook_ok(id) ? NULL : k_unavailable;
    for (p = strstr(id, "fix:+"); p; p = strstr(p, "fix:+")) {
        size_t n;
        int f;
        p += 5;
        n = strcspn(p, ";, ");
        f = fix_by_name(p, n);
        ensure_run();
        if (f >= 0 && s_state == 1 && s_fix_bad[f])
            return k_unavailable;
    }
    return NULL;
}
