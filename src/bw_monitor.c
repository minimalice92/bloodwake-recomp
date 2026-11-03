/* bw_monitor.c - the monitor the game is on (0.2.5 item 34). See bw_monitor.h. */
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include "bw_monitor.h"

static int s_from_window;

HMONITOR bw_game_monitor(void)
{
    const char *e = getenv("BW_PARENT_HWND");
    POINT origin = { 0, 0 };
    if (e && *e) {
        HWND h = (HWND)(uintptr_t)_strtoui64(e, NULL, 0);
        if (h && IsWindow(h)) {
            HMONITOR m = MonitorFromWindow(h, MONITOR_DEFAULTTONULL);
            if (m) {
                s_from_window = 1;
                return m;
            }
        }
    }
    s_from_window = 0;
    return MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
}

const char *bw_game_monitor_source(void)
{
    return s_from_window ? "the game window's monitor" : "the primary monitor";
}
