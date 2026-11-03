/*
 * Blood Wake carried patch of xboxrecomp v0.16.0 src/input/xinput_device.c
 * (MIT License, Copyright (c) 2026 sp00nz -- see ../xboxrecomp/LICENSE). Built
 * in place of the toolkit's copy by our CMakeLists.txt; xinput_device.c.diff
 * beside this file is the exact difference from the pinned original. Only the
 * Windows (XInput) backend changes; the additions are declared in
 * bw_input_ext.h. Release 0.2.5, lane C3 (docs/025/C3.md):
 *
 *  - Ports follow the connected controllers (item 73). Port n used to read
 *    XInput slot n, so a pad that Windows put in slot 2 or 3 (after a
 *    reconnect, or a Steam Input / DS4Windows virtual pad) gave player 1
 *    nothing. Now each connected controller takes the first free port, in
 *    slot order, and keeps it while it stays connected; a port whose
 *    controller goes away waits for the next one to connect. Rumble follows
 *    the same mapping. BW_PAD_SLOTS=fixed restores port n = slot n.
 *  - Rumble stops when the window loses the focus and when asked
 *    (xbox_InputRumbleAllOff: window close, boot start) (item 74).
 *  - Optional radial stick deadzones for real pads (item 76, settings.ini
 *    [keys] deadzone_left / deadzone_right); none unless set.
 *  - BW_TEST_XINPUT, a test switch that replaces the real controllers with
 *    stand-ins on chosen slots, connected and disconnected on a timeline.
 *
 * Suggested upstream as a toolkit change (the mapping and the rumble stop are
 * not specific to Blood Wake).
 */
/**
 * Xbox Input Compatibility Layer
 *
 * Translates the Xbox controller API to a host gamepad backend.
 * Handles the structural differences between the Xbox gamepad
 * (analog buttons as bytes, separate trigger channels) and the host
 * (digital face buttons, trigger axes).
 *
 *   _WIN32 -> Windows XInput
 *   POSIX  -> SDL2 GameController
 */

#include "xinput_xbox.h"
#include "keyboard.h"
#include "mkb_input.h"
#include <stdlib.h>
#include <string.h>

/* Merge the keyboard on top of a pad on port 0 rather than only standing in
 * for a missing one.
 *
 * The first version put the keyboard behind XInput's failure, on the
 * assumption that with nothing plugged in the call would fail. It does
 * not: under Wine XInputGetState returns ERROR_SUCCESS and a gamepad
 * with every button at rest, so the fallback was unreachable and
 * pressing a key did nothing at all.
 *
 * Merging is also the better rule. A real pad keeps working -- its
 * buttons are already in pState and the keyboard only adds to them --
 * and there is no special case left to get wrong. */
static void keyboard_merge(XBOX_INPUT_STATE *pState)
{
    XBOX_INPUT_STATE kb;
    int i;

    if (!xbox_InputKeyboardPoll(&kb))
        return;
    pState->Gamepad.wButtons |= kb.Gamepad.wButtons;
    for (i = 0; i < 8; i++)
        if (kb.Gamepad.bAnalogButtons[i] > pState->Gamepad.bAnalogButtons[i])
            pState->Gamepad.bAnalogButtons[i] = kb.Gamepad.bAnalogButtons[i];
    if (kb.Gamepad.sThumbLX) pState->Gamepad.sThumbLX = kb.Gamepad.sThumbLX;
    if (kb.Gamepad.sThumbLY) pState->Gamepad.sThumbLY = kb.Gamepad.sThumbLY;
    if (kb.Gamepad.sThumbRX) pState->Gamepad.sThumbRX = kb.Gamepad.sThumbRX;
    if (kb.Gamepad.sThumbRY) pState->Gamepad.sThumbRY = kb.Gamepad.sThumbRY;
    /* The input layer records edges, so an unchanged packet number is
     * read as the same state and the press never happens. */
    pState->dwPacketNumber = kb.dwPacketNumber;
}

/* ======================================================================== */
#if defined(_WIN32)
/* ====================  XInput backend  ================================== */
/* ======================================================================== */

#include <xinput.h>
#include <stdio.h>
#include "input_map.h"          /* xbox_InputMap_ApplyStick (Blood Wake) */
#include "bw_input_ext.h"       /* Blood Wake's additions */
#pragma comment(lib, "xinput.lib")

static BOOL  g_controller_connected[XBOX_MAX_CONTROLLERS] = { FALSE };
static DWORD g_last_packet[XBOX_MAX_CONTROLLERS] = { 0 };

/* The keyboard on port 0 (RECOMP_KEYBOARD=1) comes from keyboard.c, shared
 * with the SDL backend; the GDI window in fb_present.c records the keys. */

/* ---- Blood Wake: BW_TEST_XINPUT, stand-in controllers for tests -----------
 *
 * BW_TEST_XINPUT=<ms>:<+|-><slot>[!],...   e.g. "0:+2!" or
 * "0:+0,0:+1,6000:-0,9000:+2". Each event connects (+) or disconnects (-) a
 * stand-in pad on XInput slot 0-3, that many ms after the first input poll.
 * A stand-in reports nothing pressed; with '!' it presses START for 250 ms
 * every 2 s (enough to get past the title, which proves its port is read).
 * While the switch is set the real controllers are never read; rumble sent to
 * a stand-in is logged and dropped. Test runs only. */
#define BW_SIM_MAX 32
static struct { DWORD at; int slot, on, pulse; } s_sim[BW_SIM_MAX];
static int   s_nsim = -1;          /* -1 not read yet, 0 off */
static DWORD s_sim_t0;

static void sim_load(void)
{
    const char *e = getenv("BW_TEST_XINPUT");
    s_nsim = 0;
    if (!e || !*e)
        return;
    while (*e && s_nsim < BW_SIM_MAX) {
        char *end;
        unsigned long at = strtoul(e, &end, 10);
        if (end == e || *end != ':' || (end[1] != '+' && end[1] != '-')
            || end[2] < '0' || end[2] > '3') {
            fprintf(stderr, "[PAD] BW_TEST_XINPUT: can't read \"%.16s\"; entries are <ms>:+<slot> or <ms>:-<slot>\n", e);
            break;
        }
        s_sim[s_nsim].at = (DWORD)at;
        s_sim[s_nsim].on = end[1] == '+';
        s_sim[s_nsim].slot = end[2] - '0';
        e = end + 3;
        s_sim[s_nsim].pulse = *e == '!';
        if (*e == '!')
            e++;
        s_nsim++;
        while (*e == ',' || *e == ' ')
            e++;
    }
    s_sim_t0 = GetTickCount();
    if (!s_nsim)
        s_nsim = -2;                  /* set but unreadable: still no real pads */
    fprintf(stderr, "[PAD] test: %d stand-in controller event(s) from BW_TEST_XINPUT; real controllers not read\n",
            s_nsim > 0 ? s_nsim : 0);
}

static int sim_on(void)
{
    if (s_nsim == -1)
        sim_load();
    return s_nsim != 0;
}

/* A stand-in slot now: 0 disconnected, 1 connected, 2 connected and pulsing
 * START. The last event at or before now decides. */
static int sim_slot_state(int slot, DWORD *since)
{
    DWORD t = GetTickCount() - s_sim_t0;
    int i, st = 0;
    *since = 0;
    for (i = 0; i < s_nsim; i++)
        if (s_sim[i].slot == slot && s_sim[i].at <= t) {
            st = s_sim[i].on ? 1 + s_sim[i].pulse : 0;
            *since = s_sim[i].at;
        }
    return st;
}

static DWORD xi_get(DWORD slot, XINPUT_STATE *st)
{
    static DWORD packet[XBOX_MAX_CONTROLLERS];
    DWORD since;
    int s;
    if (!sim_on())
        return XInputGetState(slot, st);
    s = sim_slot_state((int)slot, &since);
    if (!s)
        return ERROR_DEVICE_NOT_CONNECTED;
    memset(st, 0, sizeof *st);
    st->dwPacketNumber = ++packet[slot];
    if (s == 2 && (GetTickCount() - s_sim_t0 - since) % 2000u < 250u)
        st->Gamepad.wButtons = XINPUT_GAMEPAD_START;
    return ERROR_SUCCESS;
}

static DWORD xi_set(DWORD slot, XINPUT_VIBRATION *v)
{
    if (!sim_on())
        return XInputSetState(slot, v);
    {
        /* Each change, so a test sees what reached the "pad" and when. */
        static int logged;
        static DWORD last[XBOX_MAX_CONTROLLERS];
        DWORD now = ((DWORD)v->wLeftMotorSpeed << 16) | v->wRightMotorSpeed;
        if (slot < XBOX_MAX_CONTROLLERS && (now != last[slot] || !logged) && logged < 200) {
            logged++;
            fprintf(stderr, "[PAD] test: rumble for stand-in slot %lu: left %u right %u (t=%lu ms)\n",
                    (unsigned long)slot, v->wLeftMotorSpeed, v->wRightMotorSpeed,
                    (unsigned long)(GetTickCount() - s_sim_t0));
        }
        if (slot < XBOX_MAX_CONTROLLERS)
            last[slot] = now;
    }
    return ERROR_SUCCESS;
}

/* ---- Blood Wake: which controller each port reads (0.2.5 item 73) ---------
 *
 * s_port_slot[port] is the XInput slot that port reads, or -1 while it has
 * none. A connected controller with no port takes the first free one, in slot
 * order; it keeps it while it stays connected. Checked once a second (a read
 * of an empty slot is slow, so they aren't polled every report) and at once
 * when a port's own controller stops answering. */
static SRWLOCK s_map_lock = SRWLOCK_INIT;
static int     s_port_slot[XBOX_MAX_CONTROLLERS] = { -1, -1, -1, -1 };
static int     s_slot_up[XBOX_MAX_CONTROLLERS];
static DWORD   s_last_scan;
static int     s_scanned, s_fixed = -1;

static int map_fixed(void)
{
    if (s_fixed < 0) {
        const char *e = getenv("BW_PAD_SLOTS");
        s_fixed = e && !_stricmp(e, "fixed");
        if (s_fixed)
            fprintf(stderr, "[PAD] BW_PAD_SLOTS=fixed: player n reads Windows controller slot n (the old mapping)\n");
    }
    return s_fixed;
}

/* Under s_map_lock. */
static void map_rescan(void)
{
    int s, p;
    for (s = 0; s < XBOX_MAX_CONTROLLERS; s++) {
        XINPUT_STATE st;
        s_slot_up[s] = xi_get((DWORD)s, &st) == ERROR_SUCCESS;
    }
    s_last_scan = GetTickCount();
    s_scanned = 1;
    for (p = 0; p < XBOX_MAX_CONTROLLERS; p++) {
        if (s_port_slot[p] >= 0 && !s_slot_up[s_port_slot[p]]) {
            fprintf(stderr, "[PAD] player %d: the controller in Windows slot %d went away; "
                    "player %d waits for the next controller to connect\n",
                    p + 1, s_port_slot[p], p + 1);
            s_port_slot[p] = -1;
        }
    }
    for (s = 0; s < XBOX_MAX_CONTROLLERS; s++) {
        if (!s_slot_up[s])
            continue;
        for (p = 0; p < XBOX_MAX_CONTROLLERS && s_port_slot[p] != s; p++)
            ;
        if (p < XBOX_MAX_CONTROLLERS)
            continue;                         /* already has its port */
        for (p = 0; p < XBOX_MAX_CONTROLLERS && s_port_slot[p] >= 0; p++)
            ;
        if (p == XBOX_MAX_CONTROLLERS)
            break;                            /* every port taken */
        s_port_slot[p] = s;
        fprintf(stderr, "[PAD] player %d: the controller in Windows slot %d\n", p + 1, s);
    }
}

/* The slot a port reads now, or -1. */
static int map_slot(DWORD port)
{
    int slot;
    if (map_fixed())
        return (int)port;
    AcquireSRWLockExclusive(&s_map_lock);
    if (!s_scanned || GetTickCount() - s_last_scan >= 1000u)
        map_rescan();
    slot = s_port_slot[port];
    ReleaseSRWLockExclusive(&s_map_lock);
    return slot;
}

/* The port's controller stopped answering: free the port now, so the next
 * controller to connect takes it at the next check. */
static void map_lost(DWORD port, int slot)
{
    if (map_fixed() || slot < 0)
        return;
    AcquireSRWLockExclusive(&s_map_lock);
    if (s_port_slot[port] == slot) {
        fprintf(stderr, "[PAD] player %lu: the controller in Windows slot %d stopped answering; "
                "player %lu waits for the next controller to connect\n",
                (unsigned long)port + 1, slot, (unsigned long)port + 1);
        s_port_slot[port] = -1;
        s_slot_up[slot] = 0;
    }
    ReleaseSRWLockExclusive(&s_map_lock);
}

int xbox_InputConnectedCount(void)
{
    int s, n = 0;
    AcquireSRWLockExclusive(&s_map_lock);
    map_rescan();
    for (s = 0; s < XBOX_MAX_CONTROLLERS; s++)
        n += s_slot_up[s];
    ReleaseSRWLockExclusive(&s_map_lock);
    return n;
}

/* ---- Blood Wake: rumble stops (0.2.5 item 74) ------------------------------
 * What each port last asked for, so the focus coming back can put it back. */
static volatile LONG s_unfocused, s_held_back;
static XBOX_VIBRATION s_last_vib[XBOX_MAX_CONTROLLERS];

void xbox_InputRumbleAllOff(const char *why)
{
    DWORD s;
    XINPUT_VIBRATION off = { 0, 0 };
    for (s = 0; s < XBOX_MAX_CONTROLLERS; s++)
        xi_set(s, &off);
    fprintf(stderr, "[PAD] rumble off on every controller (%s)\n", why ? why : "asked");
    fflush(stderr);                  /* the window-close path ends the process next */
}

void xbox_InputSetFocus(int focused)
{
    DWORD p;
    if (!focused) {
        if (InterlockedExchange(&s_unfocused, 1) == 0)
            xbox_InputRumbleAllOff("the game window lost the focus");
        return;
    }
    if (InterlockedExchange(&s_unfocused, 0) == 0)
        return;
    fprintf(stderr, "[PAD] the game window has the focus again: rumble back on "
            "(%ld rumble changes were held back while it hadn't)\n",
            (long)InterlockedExchange(&s_held_back, 0));
    for (p = 0; p < XBOX_MAX_CONTROLLERS; p++) {
        int slot = map_slot(p);
        if (slot >= 0 && (s_last_vib[p].wLeftMotorSpeed || s_last_vib[p].wRightMotorSpeed)) {
            XINPUT_VIBRATION v;
            v.wLeftMotorSpeed = s_last_vib[p].wLeftMotorSpeed;
            v.wRightMotorSpeed = s_last_vib[p].wRightMotorSpeed;
            xi_set((DWORD)slot, &v);
        }
    }
}

/* ---- Blood Wake: stick deadzones (0.2.5 item 76) --------------------------- */
static volatile LONG s_dz_left, s_dz_right;

void xbox_InputSetDeadzones(int left_pct, int right_pct)
{
    if (left_pct < 0) left_pct = 0;
    if (left_pct > 95) left_pct = 95;
    if (right_pct < 0) right_pct = 0;
    if (right_pct > 95) right_pct = 95;
    InterlockedExchange(&s_dz_left, left_pct);
    InterlockedExchange(&s_dz_right, right_pct);
}

void xbox_InputInit(void)
{
    for (DWORD i = 0; i < XBOX_MAX_CONTROLLERS; i++) {
        XINPUT_STATE state;
        DWORD result = xi_get(i, &state);
        g_controller_connected[i] = (result == ERROR_SUCCESS);
    }
}

DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    XINPUT_STATE xi_state = { 0 };
    DWORD result;
    int slot;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pState)
        return ERROR_DEVICE_NOT_CONNECTED;

    /* Blood Wake: the port's controller, not slot dwPort (item 73). */
    slot = map_slot(dwPort);
    result = slot >= 0 ? xi_get((DWORD)slot, &xi_state) : ERROR_DEVICE_NOT_CONNECTED;
    if (result != ERROR_SUCCESS && slot >= 0)
        map_lost(dwPort, slot);
    if (result != ERROR_SUCCESS) {
        g_controller_connected[dwPort] = FALSE;
        if (dwPort == 0) {
            /* Keyboard and mouse stand in for an absent pad, so the game is
             * playable with no controller plugged in. */
            int kb;
            memset(pState, 0, sizeof *pState);
            kb = xbox_InputKeyboardPoll(pState);
            if (mkb_merge(dwPort, pState))
                g_controller_connected[dwPort] = TRUE;
            if (kb || mkb_enabled())
                return ERROR_SUCCESS;
        }
        return result;
    }

    g_controller_connected[dwPort] = TRUE;
    g_last_packet[dwPort] = xi_state.dwPacketNumber;

    memset(pState, 0, sizeof(XBOX_INPUT_STATE));
    pState->dwPacketNumber = xi_state.dwPacketNumber;
    pState->Gamepad.wButtons = xi_state.Gamepad.wButtons & 0x00FF;

    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_A] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_A) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_B] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_B) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_X] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_X) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_Y) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] = xi_state.Gamepad.bLeftTrigger;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] = xi_state.Gamepad.bRightTrigger;

    pState->Gamepad.sThumbLX = xi_state.Gamepad.sThumbLX;
    pState->Gamepad.sThumbLY = xi_state.Gamepad.sThumbLY;
    pState->Gamepad.sThumbRX = xi_state.Gamepad.sThumbRX;
    pState->Gamepad.sThumbRY = xi_state.Gamepad.sThumbRY;

    /* Blood Wake (item 76): the player's deadzones, on the pad's own sticks
     * only (before the keyboard is merged). None set: untouched, as always. */
    {
        int dzl = (int)s_dz_left, dzr = (int)s_dz_right;
        if (dzl > 0)
            xbox_InputMap_ApplyStick(pState->Gamepad.sThumbLX, pState->Gamepad.sThumbLY, dzl,
                                     &pState->Gamepad.sThumbLX, &pState->Gamepad.sThumbLY);
        if (dzr > 0)
            xbox_InputMap_ApplyStick(pState->Gamepad.sThumbRX, pState->Gamepad.sThumbRY, dzr,
                                     &pState->Gamepad.sThumbRX, &pState->Gamepad.sThumbRY);
    }

    if (dwPort == 0)
        keyboard_merge(pState);

    /* Mouse+keyboard merges on top of a real pad too: the mouse owns the
     * aim stick while it is moving, WASD owns movement while held. */
    if (dwPort == 0)
        mkb_merge(dwPort, pState);

    return ERROR_SUCCESS;
}

DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration)
{
    XINPUT_VIBRATION xi_vib;
    int slot;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pVibration)
        return ERROR_DEVICE_NOT_CONNECTED;

    /* Blood Wake: to the port's controller (item 73), and held back while
     * the window doesn't have the focus (item 74). */
    s_last_vib[dwPort] = *pVibration;
    slot = map_slot(dwPort);
    if (slot < 0)
        return ERROR_DEVICE_NOT_CONNECTED;
    if (s_unfocused) {
        InterlockedIncrement(&s_held_back);
        return ERROR_SUCCESS;
    }
    xi_vib.wLeftMotorSpeed = pVibration->wLeftMotorSpeed;
    xi_vib.wRightMotorSpeed = pVibration->wRightMotorSpeed;
    return xi_set((DWORD)slot, &xi_vib);
}

BOOL xbox_InputIsConnected(DWORD dwPort)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS) return FALSE;
    return g_controller_connected[dwPort];
}

DWORD xbox_InputGetCapabilities(DWORD dwPort, DWORD dwFlags, XBOX_INPUT_CAPABILITIES *pCaps)
{
    XINPUT_CAPABILITIES xi_caps;
    DWORD result;
    int slot;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pCaps)
        return ERROR_DEVICE_NOT_CONNECTED;

    /* Blood Wake: the port's controller (item 73). */
    slot = map_slot(dwPort);
    if (slot < 0)
        return ERROR_DEVICE_NOT_CONNECTED;
    if (sim_on()) {
        XINPUT_STATE st;
        memset(pCaps, 0, sizeof(XBOX_INPUT_CAPABILITIES));
        pCaps->Type = XINPUT_DEVTYPE_GAMEPAD;
        pCaps->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
        return xi_get((DWORD)slot, &st);
    }
    result = XInputGetCapabilities((DWORD)slot, dwFlags, &xi_caps);
    if (result != ERROR_SUCCESS) return result;

    memset(pCaps, 0, sizeof(XBOX_INPUT_CAPABILITIES));
    pCaps->Type = xi_caps.Type;
    pCaps->SubType = xi_caps.SubType;
    pCaps->Flags = xi_caps.Flags;
    return ERROR_SUCCESS;
}

/* ======================================================================== */
#else /* !_WIN32 */
/* ====================  SDL2 GameController backend  ===================== */
/* ======================================================================== */

#include <SDL.h>

static SDL_GameController *g_pads[XBOX_MAX_CONTROLLERS];
static BOOL  g_controller_connected[XBOX_MAX_CONTROLLERS];
static DWORD g_packet[XBOX_MAX_CONTROLLERS];

/* Set while the framebuffer window's main thread pumps SDL events
 * (src/video/fb_present_sdl.c). The pump already updates the controllers,
 * and SDL_GameControllerUpdate pumps events itself, which on macOS must only
 * happen on the main thread; so then this side only reads them. */
static volatile int g_main_pump;

void xbox_InputSetMainPump(int on)
{
    g_main_pump = on;
}

/* Whether any controller is open: SDL must stay up for it at shutdown. */
int xbox_InputAnyOpen(void)
{
    for (int i = 0; i < XBOX_MAX_CONTROLLERS; i++)
        if (g_pads[i])
            return 1;
    return 0;
}

/* Open up to XBOX_MAX_CONTROLLERS attached game controllers. */
static void open_controllers(void)
{
    int slot = 0;
    for (int i = 0; i < SDL_NumJoysticks() && slot < XBOX_MAX_CONTROLLERS; i++) {
        if (!SDL_IsGameController(i))
            continue;
        if (!g_pads[slot]) {
            g_pads[slot] = SDL_GameControllerOpen(i);
            g_controller_connected[slot] = (g_pads[slot] != NULL);
        }
        slot++;
    }
}

void xbox_InputInit(void)
{
    /* Keep the default SIGINT/SIGTERM: SDL would turn them into an SDL_QUIT
     * that nothing reads without a window, and Ctrl-C would stop working. */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (!SDL_WasInit(SDL_INIT_GAMECONTROLLER))
        SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
    open_controllers();
}

DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pState)
        return ERROR_DEVICE_NOT_CONNECTED;

    SDL_GameController *c = g_pads[dwPort];
    if (!c || !SDL_GameControllerGetAttached(c)) {
        g_controller_connected[dwPort] = FALSE;
        if (dwPort == 0 && xbox_InputKeyboardPoll(pState))
            return ERROR_SUCCESS;
        return ERROR_DEVICE_NOT_CONNECTED;
    }

    if (!g_main_pump)
        SDL_GameControllerUpdate();
    g_controller_connected[dwPort] = TRUE;

    memset(pState, 0, sizeof(XBOX_INPUT_STATE));
    pState->dwPacketNumber = ++g_packet[dwPort];

    WORD btn = 0;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_UP))    btn |= XBOX_GAMEPAD_DPAD_UP;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_DOWN))  btn |= XBOX_GAMEPAD_DPAD_DOWN;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_LEFT))  btn |= XBOX_GAMEPAD_DPAD_LEFT;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) btn |= XBOX_GAMEPAD_DPAD_RIGHT;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_START))      btn |= XBOX_GAMEPAD_START;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_BACK))       btn |= XBOX_GAMEPAD_BACK;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_LEFTSTICK))  btn |= XBOX_GAMEPAD_LEFT_THUMB;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSTICK)) btn |= XBOX_GAMEPAD_RIGHT_THUMB;
    pState->Gamepad.wButtons = btn;

    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_A] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_A) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_B] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_B) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_X] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_X) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_Y) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) ? 255 : 0;

    /* SDL trigger axes are 0..32767 -> Xbox analog button 0..255 */
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] =
        (BYTE)(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) >> 7);
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] =
        (BYTE)(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >> 7);

    /* SDL Y axis points down; the Xbox Y axis points up -- invert.
     * Use (-1 - v) so v = -32768 does not overflow SHORT. */
    pState->Gamepad.sThumbLX = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
    pState->Gamepad.sThumbLY =
        (SHORT)(-1 - SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY));
    pState->Gamepad.sThumbRX = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTX);
    pState->Gamepad.sThumbRY =
        (SHORT)(-1 - SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTY));

    if (dwPort == 0)
        keyboard_merge(pState);
    return ERROR_SUCCESS;
}

DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pVibration)
        return ERROR_DEVICE_NOT_CONNECTED;

    SDL_GameController *c = g_pads[dwPort];
    if (!c) return ERROR_DEVICE_NOT_CONNECTED;

    /* SDL rumble needs a duration; refresh for ~1s on each call (the game
     * polls vibration continuously). */
    SDL_GameControllerRumble(c, pVibration->wLeftMotorSpeed,
                             pVibration->wRightMotorSpeed, 1000);
    return ERROR_SUCCESS;
}

BOOL xbox_InputIsConnected(DWORD dwPort)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS) return FALSE;
    return g_controller_connected[dwPort];
}

DWORD xbox_InputGetCapabilities(DWORD dwPort, DWORD dwFlags, XBOX_INPUT_CAPABILITIES *pCaps)
{
    (void)dwFlags;
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pCaps)
        return ERROR_DEVICE_NOT_CONNECTED;
    if (!g_pads[dwPort])
        return ERROR_DEVICE_NOT_CONNECTED;

    memset(pCaps, 0, sizeof(XBOX_INPUT_CAPABILITIES));
    pCaps->Type    = 1;   /* XINPUT_DEVTYPE_GAMEPAD */
    pCaps->SubType = 1;   /* XINPUT_DEVSUBTYPE_GAMEPAD */
    pCaps->Flags   = 0;
    return ERROR_SUCCESS;
}

#endif /* _WIN32 */
