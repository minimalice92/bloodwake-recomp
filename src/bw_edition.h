#ifndef BW_EDITION_H
#define BW_EDITION_H

/* The edition switch (D-007, D-015): Original or Remastered, chosen
 * at launch. Design: docs/remaster/editions.md (agent R5); wired in by D5.
 *
 * One process is one boot of the game, and the game reboots itself between
 * the title, menus and missions. The edition therefore travels in the
 * environment, BW_EDITION, which every boot inherits from its launcher
 * (tools/play.py and its host window src/bw_host.c, play.py --windows,
 * tools/bw_run.py). Nothing about it is stored in guest memory or in the
 * game's launch data.
 *
 *   BW_EDITION = original | remastered   (unset or empty: original;
 *                anything else: the boot refuses to start)
 *   BW_FIX     = comma list of all, none, +ID, -ID, ID (not Original;
 *                Original ignores it and says so)
 *
 * Original is the zero state: g_bw_fix_on[] is all zeros and stays so, so
 * every bw_fix_enabled() is 0 even if bw_edition_init() was never called.
 * Nothing in this file writes guest memory in Original, and Original's save
 * folder is the toolkit's default, exactly as before the switch. */

#include <stddef.h>
#include <stdint.h>


typedef enum {
    BW_ED_ORIGINAL   = 0,
    BW_ED_REMASTERED = 1,
    BW_ED_COUNT
} bw_edition_t;

/* Registry lines name the editions they apply to: BW_ED_RX is every edition
 * after Original. */
#define BW_ED_BIT(e) (1u << (e))
#define BW_ED_R      BW_ED_BIT(BW_ED_REMASTERED)
#define BW_ED_RX     BW_ED_R

typedef enum {
    BW_FIXKIND_IMAGE,     /* XBE image bytes, patched at boot */
    BW_FIXKIND_HOOK,      /* a D-011 wrapper tests bw_fix_enabled() */
    BW_FIXKIND_PAK,       /* loaded .pak bytes, patched after parsing (later) */
    BW_FIXKIND_RUNTIME    /* a parameter our runtime reads (back end, hooks) */
} bw_fix_kind_t;

typedef enum {
#define BW_FIX(id, editions, dflt, kind, logref, desc) BW_FIX_##id,
#include "bw_fixes.def"
#undef BW_FIX
    BW_FIX_COUNT
} bw_fix_id_t;

typedef struct {
    const char   *name;       /* the ID without BW_FIX_ */
    unsigned      editions;   /* BW_ED_BIT() bits */
    int           dflt;       /* on by default in those editions */
    bw_fix_kind_t kind;
    const char   *logref;     /* REMASTER_LOG / spec reference */
    const char   *desc;
} bw_fix_info_t;

extern const bw_fix_info_t g_bw_fixes[];
/* 1 = on. Written once by bw_edition_init(), before any guest thread runs,
 * and by bw_edition_apply_image_fixes() only to turn off a fix it refused;
 * read-only after that, so no locking. */
extern unsigned char g_bw_fix_on[];

/* Read BW_EDITION and BW_FIX. 0 = fine; -1 = refuse to start, with the
 * reason in err (a buffer of errlen > 0 bytes; main.c hands it to bw_fail).
 * Logs one "[ED]" line. On -1 every fix is off. */
int bw_edition_init(char *err, size_t errlen);

bw_edition_t bw_edition(void);
/* Remastered includes Original's fixes-to-be; each later edition includes
 * the one before (lead, 2026-10-27). So "at least Remastered" is any edition
 * after Original. */
static __inline int bw_edition_at_least(bw_edition_t e)
{
    return (int)bw_edition() >= (int)e;
}
static __inline int bw_is_original(void)   { return bw_edition() == BW_ED_ORIGINAL; }
static __inline int bw_is_remastered(void) { return bw_edition() == BW_ED_REMASTERED; }
const char  *bw_edition_name(bw_edition_t e);       /* "original", ... */
const char  *bw_edition_title(bw_edition_t e);      /* "Original", ... */
/* For the launcher (bw_host.c): parse a name. NULL or "" gives Original.
 * Returns 0, or -1 for an unknown name. */
int bw_edition_parse(const char *s, bw_edition_t *out);

/* The hot test, for hooks: one byte load. */
static __inline int bw_fix_enabled(bw_fix_id_t id)
{
    return g_bw_fix_on[id];
}
/* The same by name ("BOAT_SPLASH_RANGE"), for code that isn't hot. An unknown
 * name is 0. */
int bw_fix_enabled_name(const char *name);

/* Apply the IMAGE fixes that are on, after xbox_MemoryLayoutInit and before
 * the entry point (main.c). Each patch first checks every byte it expects;
 * a fix with any mismatch is refused whole and turned off. With the XBE
 * buffer it also counts the .rdata/.data bytes that differ from the file
 * before and after, and logs "[ED] image check". Does nothing in Original
 * except that check. */
void bw_edition_apply_image_fixes(const void *xbe, size_t xbe_size);

/* Saves, before xbox_path_init (main.c). Returns the save root to hand to
 * xbox_path_init: NULL in Original (the toolkit's default,
 * %LOCALAPPDATA%\xboxrecomp, exactly as before the switch), or
 * BW_EDITION_SAVE_ROOT itself when that is set (test runs: Original's
 * partition images and Z: unpack cache then stay out of the player's
 * folder; docs/audio/iso.md). For the other
 * editions: <base>\editions\<name>, base = BW_EDITION_SAVE_ROOT or
 * %LOCALAPPDATA%\xboxrecomp. If RECOMP_SAVE_DIR is set (test runs), it is
 * moved to <RECOMP_SAVE_DIR>\editions\<name> the same way. On the first boot
 * of an edition whose folder has no saves yet, Original's UDATA and TDATA are
 * copied in (one way; Original's files are only read). */
const char *bw_edition_prepare_saves(void);

/* Counters for the exit summary: hooks call this when a fix changed
 * something; probes (test hooks) register a summary printer. */
void bw_fix_note_hit(bw_fix_id_t id);
void bw_edition_add_summary(void (*fn)(void));
/* "[ED] summary" lines; call where the run ends (bw_dump_evidence, and the
 * relaunch hook sub_000DB8B9, since ExitProcess skips atexit). */
void bw_edition_summary(void);

#endif
