/*
 * bw_padwatch.c - when the game sees each controller (lane au-iso, 2026-11-04;
 * a test aid in every edition, inert unless BW_PADWATCH is set).
 *
 * Why: an Original 2-player boot straight into a battle can open on the
 * game's own controller-missing pause (docs/audio/iso.md). The port
 * plugs its pads in one at a time (carried ohci.c: each one 50 passes after
 * the previous is configured), and the game pauses when a player's pad isn't
 * there when the battle starts. The OHCI arrival lines say when each pad is
 * presented; this says when the game has it and when it pauses, on the same
 * clock (h=, the host performance counter in ms, as BW_STEPLOG prints it).
 *
 * A host thread samples, about once a millisecond, and prints a line whenever
 * one changes:
 *   - the game's connected flags, Devices (0x1C7468) bytes +0..+3, which
 *     Devices_IsPadConnected reads, and its pad handles +0x3F8 + 4*port;
 *   - the Game object's state (0x1D5C08 +0x10; GameFlow_Pause sets 1).
 * Read-only: nothing here writes guest memory. At most 200 lines.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

extern ptrdiff_t g_xbox_mem_offset;

#define PW_DEVICES   0x001C7468u
#define PW_PADS      0x3F8u
#define PW_GAME      0x001D5C08u
#define PW_STATE     0x10u
#define PW_U8(va)    (*(volatile uint8_t  *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))
#define PW_U32(va)   (*(volatile uint32_t *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))

static DWORD WINAPI padwatch_thread(LPVOID unused)
{
    LARGE_INTEGER f, c;
    uint8_t conn[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    uint32_t pads[4] = {1, 1, 1, 1}, padc[4] = {9, 9, 9, 9}, state = 0xFFFFFFFFu;
    int lines = 0, i;

    (void)unused;
    QueryPerformanceFrequency(&f);
    while (!g_xbox_mem_offset)
        Sleep(1);
    while (lines < 200) {
        uint8_t nc[4];
        uint32_t np[4], npc[4], ns;
        int changed = 0;
        Sleep(1);
        for (i = 0; i < 4; i++) {
            nc[i] = PW_U8(PW_DEVICES + (uint32_t)i);
            np[i] = PW_U32(PW_DEVICES + PW_PADS + 4u * (uint32_t)i);
            /* the pad object's +4, which Players_AllPadsConnected reads */
            npc[i] = np[i] >= 0x10000u && np[i] < 0x08000000u ? PW_U8(np[i] + 4u) : 9u;
            changed |= nc[i] != conn[i] || np[i] != pads[i] || npc[i] != padc[i];
        }
        ns = PW_U32(PW_GAME + PW_STATE);
        changed |= ns != state;
        if (!changed)
            continue;
        QueryPerformanceCounter(&c);
        fprintf(stderr, "[PADWATCH] h=%.1f connected %u %u %u %u pads %08X %08X %08X %08X "
                "game state %u pad+4 %u %u %u %u\n",
                (double)c.QuadPart * 1000.0 / (double)f.QuadPart,
                nc[0], nc[1], nc[2], nc[3], np[0], np[1], np[2], np[3], ns,
                npc[0], npc[1], npc[2], npc[3]);
        fflush(stderr);
        for (i = 0; i < 4; i++) {
            conn[i] = nc[i];
            pads[i] = np[i];
            padc[i] = npc[i];
        }
        state = ns;
        lines++;
    }
    return 0;
}

void bw_padwatch_init(void)
{
    const char *e = getenv("BW_PADWATCH");
    HANDLE h;
    if (!e || !*e || *e == '0')
        return;
    h = CreateThread(NULL, 0, padwatch_thread, NULL, 0, NULL);
    if (h)
        CloseHandle(h);
    fprintf(stderr, "[PADWATCH] on (BW_PADWATCH): the game's connected pads and its state, "
            "on the host clock\n");
}

/* RECOMP_PAD_CLOCK=game (lane au-orig, a test aid; the carried usb_gamepad.c
 * asks only when that is set): the game's own clock for the pad script, the
 * weapons clock 0x1CDDC0 (ms; 33 a game step from the mission's first step,
 * the key BW_STEPLOG and BW_STEPLOG_OBJ print), so a scripted press reaches
 * the same game step in every run. Read-only. */
long bw_pad_game_clock_ms(void)
{
    return (long)PW_U32(0x001CDDC0u);
}
