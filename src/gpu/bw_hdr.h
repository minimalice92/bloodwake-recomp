/* bw_hdr.h - the screen in HDR mode, for every edition (0.2.5 item 97, D-026).
 *
 * When Windows has the game's screen in HDR mode (its desktop is BT.2100 PQ),
 * the game presents through its own scRGB swap chain (linear, BT.709
 * primaries, 1.0 = 80 cd/m2; Windows makes the BT.709 to BT.2020 step,
 * BT.2087, when it composes for the screen) instead of an 8-bit one, so
 * Windows' own composition of SDR content (piecewise sRGB, the "SDR content
 * brightness" slider) never touches the picture. The game's SDR picture is
 * shown by ITU-R BT.2408's display-referred (direct) mapping: the BT.1886
 * EOTF with the screen's black, scaled so 100% sits at BT.2408's reference
 * (Graphics) white for the screen's peak (Table 4). Remastered's visual suite
 * adds its highlight mapping on top (bw_vfx_post.c). On a screen not in HDR
 * mode nothing here changes the picture: the swap chain is 8-bit as before.
 *
 * The screen's data, in this order, each value checked for plausibility:
 *   1. its own EDID (the registry copy Windows keeps; CTA-861.3 HDR static
 *      metadata block for the luminances, the base block for the colours);
 *   2. for a value the EDID lacks, the HDR calibration profile Windows has
 *      for the screen (its MHC2 tag);
 *   3. the standards' defaults (BT.2408's 1000 cd/m2 reference display; a
 *      zero black, BT.1886's pure 2.4 power).
 * DXGI's own figures (IDXGIOutput6::GetDesc1) are logged beside them; DXGI's
 * colour space is the "is the screen in HDR mode" test. Read-only throughout.
 * The formulas and constants follow docs/remaster/hdr_standards.md.
 *
 * A watcher thread follows the window to other screens and Windows HDR being
 * switched (2 s poll of DisplayConfig's advanced-colour state, woken at once
 * by a monitor or size change); the device thread never waits on Windows'
 * display configuration after the swap chain is made. */
#ifndef BW_HDR_H
#define BW_HDR_H

#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>

typedef struct {
    int hdr;                    /* the desktop: 1 BT.2100 PQ (HDR mode), 0 SDR, -1 unknown, -2 screen not found */
    float peak, black, maxfall; /* cd/m2, as used (after the checks and fallbacks) */
    const char *peak_src, *black_src, *maxfall_src;
    float prim[8];              /* R, G, B, white CIE 1931 xy, as used */
    const char *prim_src;
    UINT bits;                  /* DXGI: bits per colour of the desktop output */
    WCHAR name[32];             /* GDI name, \\.\DISPLAYn */
} BwHdrScreen;

/* At the swap chain's creation (device thread): look at the window's screen
 * now; DXGI_FORMAT_R16G16B16A16_FLOAT when it is in HDR mode, else `sdr`. */
DXGI_FORMAT bw_hdr_swap_format(HWND hwnd, DXGI_FORMAT sdr);
/* Right after: an FP16 swap chain gets the scRGB colour space; if that is
 * refused it is made 8-bit (no back buffer is held yet). Starts or re-aims
 * the watcher. Returns 1 when presenting scRGB to a screen in HDR mode. */
int bw_hdr_swap_created(IDXGISwapChain1 *swap);
/* Each present (device thread): follow the window (w x h: its client size)
 * and take the watcher's newest result. */
void bw_hdr_follow(UINT w, UINT h);
/* swap_ensure: 1 once when the screen entered or left HDR mode and the swap
 * chain should be made again (logged). */
int bw_hdr_remake(void);
/* The swap chain is scRGB and the screen is in HDR mode now. */
int bw_hdr_scrgb(void);
const BwHdrScreen *bw_hdr_screen(void);

/* Reference (Graphics) white, cd/m2: BT.2408 Table 4 for the screen's peak;
 * BW_VFX_HDR_PAPER=<cd/m2> overrides. */
float bw_hdr_paper(const char **from);
/* The peak highlights may use: BW_VFX_HDR_PEAK, else the screen's. */
float bw_hdr_peak(const char **from);
/* The direct mapping's constants for bwh_decode (HLSL below): x 2.4 (BT.1886)
 * or 2.2 (BW_VFX_HDR_EOTF=2.2: 0.2.4's power), y a, z b (relative to Lw =
 * paper white, Lb = the screen's black when black != 0), w paper/80 (scRGB). */
void bw_hdr_direct_consts(float k[4], int with_black);
/* The BT.2390 EETF (BT.2408 Annex 5) to the screen's black and peak, for a
 * PQ signal normalised to [0, 10000] cd/m2 (the mastering levels aren't
 * known): x minLum, y maxLum, z KS, w 1. */
void bw_hdr_eetf_consts(float k[4]);
/* One line: what the mapping uses and why (logged by the callers when it
 * changes). */
void bw_hdr_log_mapping(const char *who);
/* Before any swap chain (the first render targets): look at the window's
 * screen (or the primary) now; 1 when it is in HDR mode. */
int bw_hdr_window_hdr(HWND hwnd);
/* The screen's black as used (cd/m2). */
float bw_hdr_black(const char **from);
/* The EDID or a calibration profile gave the screen's peak. */
int bw_hdr_have_data(void);
/* Remastered with HDR forced on: with no data, use the HDR1000 defaults. */
void bw_hdr_set_forced(int on);
/* bloodwake.exe --screen-info[=<x>,<y>]: one JSON object on stdout (the
 * screen under the point, else the primary); returns the exit code. */
int bw_hdr_screen_info(const char *arg);

/* HLSL helpers shared by the plain present and the visual suite. */
extern const char bw_hdr_hlsl[];

#endif
