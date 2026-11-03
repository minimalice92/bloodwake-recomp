/* bw_monitor.h - the monitor the game is on (0.2.5 item 34, contract 6).
 *
 * One answer for everything that measures "the screen": the widescreen shape
 * (bw_widescreen.c) and the high frame rate's refresh (bw_hifps_hooks.c).
 * A boot played through the host window (bw_host.c) is on the monitor of the
 * host's window (BW_PARENT_HWND); anything else is on the primary monitor,
 * where the toolkit's own window opens. */
#ifndef BW_MONITOR_H
#define BW_MONITOR_H

#include <windows.h>

/* The monitor of BW_PARENT_HWND's window; the primary if there is none. */
HMONITOR bw_game_monitor(void);

/* For log lines: "the game window's monitor" or "the primary monitor",
 * as bw_game_monitor() decided. */
const char *bw_game_monitor_source(void);

#endif
