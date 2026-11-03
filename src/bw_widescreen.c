/*
 * bw_widescreen.c - Remastered widescreen: the picture's shape, decided in
 * one place (bw_widescreen.h). Nothing here touches guest memory; the hooks
 * (bw_remaster_hooks.c) and the back end (gpu/bw_d3d11.c) read the result.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bw_edition.h"
#include "bw_widescreen.h"
#include "bw_monitor.h"        /* 0.2.5 item 34: the game's monitor, not always the primary */

#define WS_ASPECT_43   (4.0f / 3.0f)
#define WS_ASPECT_MAX  2.39f          /* 21:9 monitors (2560x1080 2.37, 3440x1440 2.39) */

static float s_aspect = WS_ASPECT_43;
static float s_k = 1.0f;

float bw_ws_aspect(void) { return s_aspect; }
float bw_ws_k(void)      { return s_k; }

/* "16:9", "64:27", "1.7778", "display". 0 for "display" or nonsense. */
static float parse_aspect(const char *e)
{
    float a = 0.0f, w, h;
    if (!e || !*e || !_stricmp(e, "display"))
        return 0.0f;
    if (sscanf(e, "%f:%f", &w, &h) == 2 && w > 0.0f && h > 0.0f)
        a = w / h;
    else if (sscanf(e, "%f", &w) == 1)
        a = w;
    if (!_stricmp(e, "21:9"))
        a = 64.0f / 27.0f;           /* the marketing name for 2.37 */
    return a;
}

/* The shape of the monitor the game is on (item 34: it used to be the
 * primary's always, so a game on a 21:9 second screen got the 16:9 shape). */
static float display_aspect(int *dw, int *dh)
{
    MONITORINFO mi;
    mi.cbSize = sizeof mi;
    *dw = *dh = 0;
    if (GetMonitorInfoA(bw_game_monitor(), &mi)) {
        *dw = mi.rcMonitor.right - mi.rcMonitor.left;
        *dh = mi.rcMonitor.bottom - mi.rcMonitor.top;
    }
    return (*dw > 0 && *dh > 0) ? (float)*dw / (float)*dh : 16.0f / 9.0f;
}

void bw_ws_init(int gameplay_boot, int log)
{
    const char *e = getenv("BW_ASPECT");
    float want = parse_aspect(e), a;
    int dw = 0, dh = 0;

    s_aspect = WS_ASPECT_43;
    s_k = 1.0f;
    if (!bw_fix_enabled(BW_FIX_WIDESCREEN)) {
        if (log && bw_edition() != BW_ED_ORIGINAL)
            fprintf(stderr, "[WS] off: 4:3 (BW_FIX=-WIDESCREEN)\n");
        return;                       /* Original: nothing, as shipped */
    }
    {
        const char *g = getenv("BW_GPU");
        if (!g || strcmp(g, "d3d11")) {
            if (log)
                fprintf(stderr, "[WS] off: widescreen needs the GPU renderer (BW_GPU=d3d11)\n");
            return;
        }
    }
    a = want > 0.0f ? want : display_aspect(&dw, &dh);
    if (a < WS_ASPECT_43)
        a = WS_ASPECT_43;             /* a portrait or square screen: 4:3, letterboxed */
    if (a > WS_ASPECT_MAX)
        a = WS_ASPECT_MAX;            /* wider: pillarboxed to 21:9 */
    s_aspect = a;
    s_k = gameplay_boot ? a / WS_ASPECT_43 : 1.0f;
    if (s_k < 1.001f)
        s_k = 1.0f;
    if (log) {
        if (want > 0.0f)
            fprintf(stderr, "[WS] on: picture %.4f (BW_ASPECT=%s)", a, e);
        else
            fprintf(stderr, "[WS] on: picture %.4f (display %dx%d, %s)", a, dw, dh,
                    bw_game_monitor_source());
        fprintf(stderr, ", k %.4f this boot (%s)\n", s_k,
                gameplay_boot ? "gameplay: Hor+, 2D pillarboxed"
                              : "title and menus: 4:3, pillarboxed in the wide window");
        fflush(stderr);
    }
}
