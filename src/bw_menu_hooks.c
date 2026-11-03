/* bw_menu_hooks.c - the front end's menus on a PC (release 0.2.5, lane C3,
 * docs/025/C3.md). D-011 wrappers: each is sub_X calling the game's own body
 * sub_X_gen; tools/run_pipeline.py hands this file to the generator
 * (bw_*_hooks.c), so the functions wrapped here are emitted as sub_X_gen.
 *
 * Item 78, "Game Demos" becomes "Quit Game" (MENU_QUIT, every edition after
 * Original, on by default; src/bw_fixes.def):
 *   - The label is text, not art (seen): the main menu's Element_9 in
 *     screens/FEInfo.pak is `text = MAINMENU_XDEMOS`, `font_pack =
 *     BAMBOO_TEXT`, and the bamboo textures MM_Bam1-4 carry no lettering;
 *     the language file of screen texts maps the key to the label.
 *     Screen_ReadElements (0x5DCxx) looks it up with Text_GetString
 *     (0xAA330) and hands the result to UIElem_SetText (seen, 0x5E531). The
 *     font (fonts/FRML436N, 224 glyphs 0x20-0xFF) draws every letter of the
 *     new label (seen: Q, u, i and t have real glyph boxes).
 *   - Text_GetString's wrapper writes the new label over the old one in the
 *     loaded string table, once, the first time the key is asked for: the new
 *     label is shorter, so it fits in place, and every later lookup gets it.
 *   - System_LaunchXDemos (0xAB0B0; its only caller is FrontEnd_Update's
 *     state 14, reached only from the main menu, so no mission is running and
 *     no save is being written; seen) closes the game instead, the way closing
 *     the window does: the host window's WM_CLOSE when there is one
 *     (BW_PARENT_HWND, bw_host.c), else the game window's own.
 *   - Original, or BW_FIX=-MENU_QUIT: both wrappers call the body and nothing
 *     else, so Game Demos is as shipped (the port shows Please wait and
 *     cold-boots back to the title: there is no XDemos program on a PC).
 *
 * The label is the game's own text in the game's own font, not new art
 * (D-016 forbids changing the art). Claims are labelled seen, inferred,
 * unknown.
 */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "bw_edition.h"

extern ptrdiff_t g_xbox_mem_offset;
extern __declspec(thread) uint32_t g_eax, g_esp;
extern void *bw_fb_window(void);                       /* carried fb_present.c */
extern void xbox_InputRumbleAllOff(const char *why);   /* carried xinput_device.c */
extern void bw_pad_pulse(int pad, int lx, int ly, unsigned digital, unsigned analog, unsigned ms);  /* carried usb_gamepad.c */

#define M_MEM32(va) (*(volatile uint32_t *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))
#define M_MEM16(va) (*(volatile uint16_t *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))
#define M_MEM8(va)  (*(volatile uint8_t  *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))
#define M_OK(va)    ((uint32_t)(va) >= 0x00010000u && (uint32_t)(va) < 0x08000000u)

/* ---- item 78: the label ---------------------------------------------------- */

static const char k_xdemos_key[] = "MAINMENU_XDEMOS";
static const char k_quit_label[] = "Quit Game";

static int m_key_is(uint32_t va, const char *s)
{
    size_t i;
    if (!M_OK(va))
        return 0;
    for (i = 0;; i++) {
        char c = (char)M_MEM8(va + (uint32_t)i);
        if (c != s[i])
            return 0;
        if (!c)
            return 1;
    }
}

/* -1 not decided yet, 0 nothing to do (Original, switched off, or done). */
static int s_label_todo = -1;

/* Text_GetString, 0x000AA330 (ecx = the string table, [esp+4] = the key;
 * returns the UTF-16 text in eax, or L"!NO_TEXT"). */
extern void sub_000AA330_gen(void);
void sub_000AA330(void)
{
    uint32_t key;
    if (!s_label_todo) {
        sub_000AA330_gen();
        return;
    }
    key = M_MEM32(g_esp + 4);
    sub_000AA330_gen();
    if (s_label_todo < 0)
        s_label_todo = bw_fix_enabled(BW_FIX_MENU_QUIT);
    if (s_label_todo && m_key_is(key, k_xdemos_key)) {
        uint32_t text = g_eax, n = 0, i;
        s_label_todo = 0;
        while (M_OK(text) && n < 64 && M_MEM16(text + 2 * n))
            n++;
        if (!M_OK(text) || n < sizeof k_quit_label - 1) {
            fprintf(stderr, "[MENU] Quit Game: the Game Demos label is %u characters at 0x%08X, "
                    "too short to hold the new one; left as shipped\n", (unsigned)n, (unsigned)text);
            return;
        }
        for (i = 0; i < sizeof k_quit_label; i++)          /* with its terminator */
            M_MEM16(text + 2 * i) = (uint16_t)(unsigned char)k_quit_label[i];
        fprintf(stderr, "[MENU] Quit Game: the main menu's Game Demos sign reads \"%s\" "
                "(MENU_QUIT)\n", k_quit_label);
    }
}

/* ---- item 78: the choice --------------------------------------------------- */

static void m_quit(void)
{
    static LONG once;
    const char *e = getenv("BW_PARENT_HWND");
    HWND host = e && *e ? (HWND)(uintptr_t)_strtoui64(e, NULL, 0) : NULL;
    HWND own = (HWND)bw_fb_window();
    if (InterlockedExchange(&once, 1))
        return;
    fprintf(stderr, "[MENU] Quit Game chosen on the main menu: closing the game (MENU_QUIT)\n");
    fflush(stderr);
    if (host && IsWindow(host))
        PostMessageA(host, WM_CLOSE, 0, 0);   /* the host closes its boot and itself */
    else if (own && IsWindow(own))
        PostMessageA(own, WM_CLOSE, 0, 0);    /* the window's own exit path */
    else {
        xbox_InputRumbleAllOff("Quit Game");
        fflush(stdout);
        TerminateProcess(GetCurrentProcess(), 0);
    }
}

/* System_LaunchXDemos, 0x000AB0B0 (ecx = the game object 0x1E5CF0, no stack
 * arguments, plain ret). FrontEnd_Update calls it every frame while the front
 * end is in state 14, until the process ends. */
extern void sub_000AB0B0_gen(void);
void sub_000AB0B0(void)
{
    if (!bw_fix_enabled(BW_FIX_MENU_QUIT)) {
        sub_000AB0B0_gen();
        return;
    }
    m_quit();
    g_esp += 4;                                /* the body's own `ret` */
}

/* ---- item 79: the mouse in the menus (MENU_MOUSE) --------------------------
 *
 * FrontEnd_UpdateMenuNav, 0x0005C060 (ecx = the front end 0x1C8180, plain
 * ret, returns al). FrontEnd_Update calls it once a front-end frame, after
 * FrontEnd_ReadPad has filled each port's record (front end + 0x18 * port:
 * +0xC/+0x10 the stick, +0x14 accept, +0x18 start, +0x1C action 6, +0x20
 * back, floats, a held button reading 0 after its first frame) and
 * FrontEnd_MergePadInputs has combined them (+0x6C/+0x70 the stick, +0x74
 * accept, +0x78 start, +0x80 back), and before the screen of the moment
 * updates (seen). It turns the merged stick into the nav code at +0x2A3C
 * (0x1CABBC): 1 up, 2 down, with a 0.2 s repeat (seen; MainMenu_UpdateActive
 * plays FESound_PlayUp and moves the selection up for 1, FESound_PlayDown
 * and down for 2, and takes accept or start above 0 as "next").
 *
 * So after its body, mouse input becomes the same input a pad gives, for
 * whichever front-end screen is up:
 *   - the wheel: one nav step a notch (up = 1, down = 2);
 *   - a left click: accept, a right click: back, for one frame, in the merged
 *     record and player 1's (screens that ask "which controller pressed A"
 *     read the port's);
 *   - on the main menu and the options menu, hovering over an item moves
 *     the game's own highlight there, one nav step a frame (with the game's
 *     own sound); a click on an item waits for the highlight to arrive;
 *     the items' places are the screens' elements in screens/FEInfo.pak
 *     (m_hover_screen);
 *   - a click on a screen's B "Back" prompt is back.
 * The window is the game's 640x480 picture fitted into the window's client
 * area (a menu boot is 4:3, pillarboxed in a wider window: bw_widescreen.h).
 * The cursor shows while the front end runs a screen and hides otherwise (in
 * play, movies, between boots); Original and BW_FIX=-MENU_MOUSE keep the
 * window's cursor as before and inject nothing. */
#define FE_VA          0x001C8180u
#define FE_NAV         (FE_VA + 0x2A3Cu)       /* 0x1CABBC */
#define FE_MAINMENU    (FE_VA + 0x2B7Cu)       /* the main menu object */
#define FE_ST_TITLE    2u
#define FE_ST_MAINMENU 3u
#define FE_OPTIONS     (FE_VA + 0x2B84u)       /* the Options object */
#define FE_ST_OPTIONS  10u                     /* Options_Update runs in states 10 and 13 */
#define FE_ST_OPTIONS2 13u
#define FE_STORY       (FE_VA + 0x2A04u)       /* the Story menu */
#define FE_ST_STORY    11u
#define FE_BATTLE      (FE_VA + 0x2B80u)       /* the Battle menu */
#define FE_ST_BATTLE   12u

extern int bw_fb_mouse(int *x, int *y, int *cw, int *ch, int *moves);  /* carried fb_present.c */
extern int bw_fb_mouse_wheel_take(void);
extern void bw_fb_mouse_clicks_take(int *left, int *right);
extern int xbox_FramebufferKeyDown(int vk);                            /* toolkit keyboard.c: the window key table */

static volatile LONG s_fe_tick;                /* GetTickCount of the last menu frame */
static volatile LONG s_fe_cursor;              /* that frame wants the cursor */

/* For the carried fb_present.c's WM_SETCURSOR: -1 leave the window's cursor
 * alone, 1 show the arrow, 0 hide it. */
int bw_menu_cursor_wanted(void)
{
    if (!bw_fix_enabled(BW_FIX_MENU_MOUSE))
        return -1;
    return s_fe_cursor && GetTickCount() - (DWORD)s_fe_tick < 300u;
}

static void m_set_float(uint32_t va, float f)
{
    memcpy((void *)((uintptr_t)va + g_xbox_mem_offset), &f, 4);
}

/* A screen whose items the mouse can point at. The items' bands are the
 * screens' elements in screens/FEInfo.pak (seen), in the 640x480 screen; the
 * highlight is where the game keeps it (seen in each screen's code, named in
 * the comments). A picker row also has a value area: a click on its left or
 * right half (or holding the button there) is left or right, as is the wheel
 * over it; a click on a list item is accept. */
#define M_MAX_ITEMS 8
typedef struct {
    const char *name;                           /* for the trace */
    int n;
    double x0, x1;                              /* the items' left and right */
    double top[M_MAX_ITEMS + 1];                /* n + 1 edges, top to bottom */
    double vx0[M_MAX_ITEMS], vx1[M_MAX_ITEMS];  /* picker value area (0, 0: a list item) */
    int first;                                  /* the highlight's value for item 0 */
} m_layout_t;

typedef struct {
    const m_layout_t *lay;
    uint32_t sel_va;                            /* the highlight (a dword) */
    uint32_t list;                              /* a ScrollList: items are visible rows */
} m_hover_t;

/* The layouts (FEInfo.pak, seen; bands drawn between the rows' texts and the
 * highlight bamboo's y). */
static const m_layout_t L_MAIN = { "main menu", 4, 320, 640,
    { 108, 168, 223, 278, 340 }, { 0 }, { 0 }, 0 };
static const m_layout_t L_OPTIONS = { "options", 4, 60, 580,
    { 112, 164, 217, 270, 325 }, { 0 }, { 0 }, 0 };
static const m_layout_t L_GRAPHICS = { "graphics options", 2, 40, 600,
    { 124, 178, 232 }, { 330, 330 }, { 600, 590 }, 0 };
static const m_layout_t L_AUDIO = { "audio options", 4, 40, 600,
    { 108, 160, 212, 262, 312 }, { 330, 330, 330, 330 }, { 600, 600, 600, 590 }, 0 };
static const m_layout_t L_CONTROLLER = { "controller options", 2, 40, 600,
    { 86, 128, 176 }, { 266, 266 }, { 520, 520 }, 0 };
static const m_layout_t L_CREATELOAD = { "create or load", 2, 60, 600,
    { 108, 162, 222 }, { 0 }, { 0 }, 0 };
static const m_layout_t L_DIFFICULTY = { "difficulty", 3, 60, 600,
    { 176, 226, 276, 330 }, { 0 }, { 0 }, 0 };
/* Mission select: highlight +0x388 is the act row (0) and missions 1-6. */
static const m_layout_t L_MISSIONS = { "mission select", 7, 40, 600,
    { 92, 132, 170, 211, 252, 294, 335, 380 }, { 220 }, { 520 }, 0 };
static const m_layout_t L_PLAYERS = { "number of players", 4, 60, 600,
    { 110, 160, 205, 250, 300 }, { 0 }, { 0 }, 0 };
/* Game Select: a ScrollList that shows 4 of its rows at a time (seen:
 * ScrollList_Refresh); a fifth band would point past the window and scroll. */
static const m_layout_t L_GAMES = { "game select", 4, 60, 580,
    { 130, 178, 227, 275, 323 }, { 0 }, { 0 }, 0 };
/* Arena setup: arena, weather, kills, time, difficulty (the photo sits
 * between the second and third rows). */
static const m_layout_t L_ARENA = { "arena setup", 5, 20, 600,
    { 95, 140, 200, 238, 286, 333 }, { 270, 270, 265, 265, 265 }, { 520, 520, 455, 455, 455 }, 0 };

/* Arena setup's rows aren't evenly spaced: its bands, with the photo's band
 * (200-238) belonging to no row. */
static int m_arena_item(double gy)
{
    static const double t[6][2] = { { 95, 140 }, { 140, 200 }, { 238, 286 }, { 286, 333 }, { 333, 382 }, { 0, 0 } };
    int i;
    for (i = 0; i < 5; i++)
        if (gy >= t[i][0] && gy < t[i][1])
            return i;
    return -1;
}

static int m_popup_up(void)
{
    uint32_t p = M_MEM32(0x001CAB7Cu);          /* the front end's popup (FE + 0x29FC) */
    return M_OK(p) && M_MEM8(p + 4u) == 1u;     /* seen: MissionSelect_Update's test */
}

/* The hover screen up now, or 0. */
static int m_hover_screen(uint32_t state, m_hover_t *h)
{
    uint32_t o, s;
    memset(h, 0, sizeof *h);
    if (m_popup_up())
        return 0;
    if (state == FE_ST_MAINMENU) {
        o = M_MEM32(FE_MAINMENU);               /* MainMenu +0x80 */
        if (!M_OK(o)) return 0;
        h->lay = &L_MAIN; h->sel_va = o + 0x80u;
        return 1;
    }
    if (state == FE_ST_OPTIONS || state == FE_ST_OPTIONS2) {
        o = M_MEM32(FE_OPTIONS);                /* Options: +0 sub-screen, +8 menu, +0xC audio, +0x10 controller, +0x14 graphics */
        if (!M_OK(o)) return 0;
        switch (M_MEM32(o)) {
        case 0: s = M_MEM32(o + 0x8u);  h->lay = &L_OPTIONS;    h->sel_va = s + 0x84u;  break;  /* OptionsMenu_MoveHighlight */
        case 1: s = M_MEM32(o + 0x14u); h->lay = &L_GRAPHICS;   h->sel_va = s + 0xCCu;  break;  /* OptionsGraphics_HighlightRow */
        case 2: s = M_MEM32(o + 0xCu);  h->lay = &L_AUDIO;      h->sel_va = s + 0x100u; break;  /* OptionsAudio_MoveHighlight */
        case 3: s = M_MEM32(o + 0x10u); h->lay = &L_CONTROLLER; h->sel_va = s + 0x2C0u; break;  /* OptionsController_HighlightRow */
        default: return 0;
        }
        return M_OK(s);
    }
    if (state == FE_ST_STORY) {
        o = M_MEM32(FE_STORY);                  /* StoryMenu: +0 its state */
        if (!M_OK(o)) return 0;
        switch (M_MEM32(o)) {
        case 0: s = M_MEM32(o + 0x1D4u); h->lay = &L_CREATELOAD; h->sel_va = s + 0x5Cu;  break;  /* CreateLoad_ShowHighlight */
        case 4: s = M_MEM32(o + 0x1C0u); h->lay = &L_MISSIONS;   h->sel_va = s + 0x388u; break;  /* MissionSelect_MoveHighlight */
        case 5: s = M_MEM32(o + 0x1CCu); h->lay = &L_DIFFICULTY; h->sel_va = s + 0x5Cu;  break;  /* SelectDifficulty_MoveHighlight */
        default: return 0;
        }
        return M_OK(s);
    }
    if (state == FE_ST_BATTLE) {
        o = M_MEM32(FE_BATTLE);                 /* BattleMenu: +0 its state, +8 players, +0xC games, +0x10 arena */
        if (!M_OK(o)) return 0;
        switch (M_MEM32(o)) {
        case 0: s = M_MEM32(o + 0x8u);  h->lay = &L_PLAYERS; h->sel_va = s;           break;  /* PlayerSelect row +0 */
        case 1: s = M_MEM32(o + 0xCu);                                                       /* GameSelect: [+0x10] its ScrollList */
                if (!M_OK(s) || !M_OK(M_MEM32(s + 0x10u))) return 0;
                h->lay = &L_GAMES; h->list = M_MEM32(s + 0x10u);
                h->sel_va = h->list + 0x8Cu; break;                                           /* the list's index (ScrollList_HandleNav) */
        case 2: s = M_MEM32(o + 0x10u); h->lay = &L_ARENA;   h->sel_va = s + 0x864u;  break;  /* ArenaSetup_HighlightRow */
        default: return 0;
        }
        return M_OK(s);
    }
    return 0;
}

/* The item under the point (for a ScrollList, the entry: first visible +
 * row), or -1. */
static int m_hover_item(const m_hover_t *h, double gx, double gy)
{
    const m_layout_t *L = h->lay;
    int i = -1;
    if (gx < L->x0 || gx >= L->x1)
        return -1;
    if (L == &L_ARENA)
        i = m_arena_item(gy);
    else
        for (i = L->n - 1; i >= 0; i--)
            if (gy >= L->top[i] && gy < L->top[i + 1])
                break;
    if (i < 0)
        return -1;
    if (h->list) {
        int first = (int)M_MEM32(h->list + 0xFCu), count = (int)M_MEM32(h->list + 0x88u);
        i += first;
        if (i >= count)
            return -1;
    }
    return i + L->first;
}

/* -1 left, 1 right, 0 not on the item's value area (item is a highlight value). */
static int m_value_side(const m_hover_t *h, int item, double gx)
{
    const m_layout_t *L = h->lay;
    int i = item - L->first;
    double mid;
    if (h->list || i < 0 || i >= L->n || L->vx1[i] <= L->vx0[i])
        return 0;
    if (gx < L->vx0[i] || gx >= L->vx1[i])
        return 0;
    mid = (L->vx0[i] + L->vx1[i]) / 2.0;
    return gx < mid ? -1 : 1;
}

/* The screens' A and B prompts (the buttons at x 294 or 320 and 440, y 404,
 * the words to their right; seen in screens/FEInfo.pak). The title and the
 * main menu have none. */
static int m_on_back_prompt(uint32_t state, double gx, double gy)
{
    return state > FE_ST_MAINMENU && gx >= 432.0 && gx < 560.0 && gy >= 394.0 && gy < 442.0;
}

static int m_on_accept_prompt(uint32_t state, double gx, double gy)
{
    return state > FE_ST_MAINMENU && gx >= 288.0 && gx < 432.0 && gy >= 394.0 && gy < 442.0;
}

/* BW_MENU_MOUSE_TRACE=1 (tests): one line per mouse action and highlight
 * change. */
static int m_trace(void)
{
    static int on = -1;
    if (on < 0)
        on = getenv("BW_MENU_MOUSE_TRACE") != NULL;
    return on;
}

/* Accept and START together, merged and player 1's (the screens that take
 * START: the title takes only it; seen in the code that reads 0x1C81F8). */
static void m_press_accept_start(void)
{
    m_set_float(FE_VA + 0x74u, 1.0f);           /* merged accept */
    m_set_float(FE_VA + 0x78u, 1.0f);           /* merged start */
    m_set_float(FE_VA + 0x14u, 1.0f);           /* player 1's accept */
    m_set_float(FE_VA + 0x18u, 1.0f);           /* player 1's start */
}

static int m_trace(void);
static void m_push_x(int side)
{
    m_set_float(FE_VA + 0x6Cu, side < 0 ? 0.0f : 65535.0f);   /* merged stick X: < 1000 / > 63500 */
    if (m_trace())
        fprintf(stderr, "[MENU] mouse trace: %s\n", side < 0 ? "left" : "right");
}

/* Boat select reads the current player's own pad, not the merged menu input
 * (seen: BoatSelect_ReadRowNav and Selector_Update on the player's port mask,
 * BoatSelect_FindAcceptingPort). So here the mouse presses that player's pad
 * for a moment (bw_pad_pulse, the carried usb_gamepad.c), the way the player
 * would: the stick up/down to change row, left/right to change the class or
 * boat, A and B. Rows (screens/FEInfo.pak, seen): the class row and the boat
 * row at y 320 and 362 (tiles from x 199, values centred at x 320, arrows at
 * x 215 and 415); the row is BoatSelect +0x15DD48 (0 class, 1 boat; seen in
 * BoatSelect_UpdateClassRow/UpdateBoatRow), the player +0x15D830, the
 * players' ports from 0x1C876F. */
#define FE_BOATSELECT  (FE_VA + 0x2A08u)
#define BS_ROW         0x15DD48u
#define BS_PLAYER      0x15D830u

static int m_boat_up(uint32_t state)
{
    uint32_t o;
    if (m_popup_up())
        return 0;
    if (state == 4u)
        return 1;
    if (state == FE_ST_STORY && M_OK(o = M_MEM32(FE_STORY)))
        return M_MEM32(o) == 8u;
    if (state == FE_ST_BATTLE && M_OK(o = M_MEM32(FE_BATTLE)))
        return M_MEM32(o) == 3u;
    return 0;
}

static void m_boat_frame(int inside, double gx, double gy, int moves, int l, int r, int *wheel_steps)
{
    static int last_moves = -1, target = -1;
    static DWORD next_ok;
    uint32_t bs = M_MEM32(FE_BOATSELECT);
    int row, hovered = -1, pad = 0, side = 0;
    DWORD now = GetTickCount();
    if (!M_OK(bs))
        return;
    row = (int)M_MEM32(bs + BS_ROW);
    {
        uint32_t player = M_MEM32(bs + BS_PLAYER);
        if (player < 4u && M_MEM8(0x001C876Fu + player) < 4u)
            pad = (int)M_MEM8(0x001C876Fu + player);
    }
    if (inside && gx >= 40.0 && gx < 460.0) {
        if (gy >= 300.0 && gy < 342.0) hovered = 0;
        else if (gy >= 342.0 && gy < 392.0) hovered = 1;
        if (hovered >= 0 && gx >= 200.0)
            side = gx < 320.0 ? -1 : 1;
    }
    if (m_trace() && (l || r || (hovered >= 0 && moves != last_moves)))
        fprintf(stderr, "[MENU] mouse trace: boat select, row %d, at %.0f,%.0f (row %d side %d) pad %d clicks %d/%d wheel %d\n",
                row, gx, gy, hovered, side, pad, l, r, *wheel_steps);
    if (m_trace()) {
        static uint32_t last_v;
        uint32_t v = (M_MEM32(bs + 0x15D398u) << 16) | (M_MEM32(bs + 0x15D498u) & 0xFFFFu);
        if (v != last_v)
            fprintf(stderr, "[MENU] mouse trace: boat select class %u boat %u\n", v >> 16, v & 0xFFFFu);
        last_v = v;
    }
    if (hovered >= 0 && moves != last_moves && hovered != row)
        target = hovered;
    last_moves = moves;
    if ((LONG)(now - next_ok) < 0)
        return;                                 /* the last press is still on the pad */
    if (inside && (r || (l && m_on_back_prompt(4u, gx, gy)))) {
        bw_pad_pulse(pad, 0, 0, 0, 0x02u, 120);                     /* B */
        next_ok = now + 300u;
        return;
    }
    if (l && inside && m_on_accept_prompt(4u, gx, gy)) {
        bw_pad_pulse(pad, 0, 0, 0, 0x01u, 120);                     /* A */
        next_ok = now + 300u;
        return;
    }
    if (target >= 0 && target != row) {
        /* Down from the class row to the boat row, up back (seen). */
        bw_pad_pulse(pad, 0, target > row ? -32767 : 32767, 0, 0, 120);
        next_ok = now + 300u;
        if (m_trace())
            fprintf(stderr, "[MENU] mouse trace: boat select %s\n", target > row ? "down" : "up");
        target = -1;
        return;
    }
    if (*wheel_steps && !(hovered >= 0 && hovered == row && side)) {
        /* The wheel off the value: the other row, as up/down would. */
        int down = *wheel_steps < 0;
        *wheel_steps += *wheel_steps > 0 ? -1 : 1;
        if ((down && row == 0) || (!down && row == 1)) {
            bw_pad_pulse(pad, 0, down ? -32767 : 32767, 0, 0, 120);
            next_ok = now + 300u;
        }
        return;
    }
    if (hovered >= 0 && hovered == row && (side && (l || *wheel_steps))) {
        int dir = l ? side : (*wheel_steps > 0 ? 1 : -1);
        if (!l)
            *wheel_steps += *wheel_steps > 0 ? -1 : 1;
        bw_pad_pulse(pad, dir * 32767, 0, 0, 0, 120);
        next_ok = now + 350u;
        if (m_trace())
            fprintf(stderr, "[MENU] mouse trace: boat select %s\n", dir < 0 ? "left" : "right");
    }
}

/* Create Game and Load Game (Story menu states 1 and 2): a storage picker
 * above a list of save slots (screens/FEInfo.pak, seen: the storage row at
 * y 101, its arrows at x 326 and 577; the slot rows' tiles from y 190, 41 px
 * apart, x 55-300). The screen's row is its +0 (0 storage, 1 the list; seen
 * in CreateGame_UpdateActive), the list [+0x38C184] (a ScrollList: index
 * +0x8C, count +0x88, first visible +0xFC). Up from the list's top goes to
 * the storage row, down from the storage row to the list (seen). */
static uint32_t m_slots_screen(uint32_t state)
{
    uint32_t o, st, s;
    if (state != FE_ST_STORY || m_popup_up() || !M_OK(o = M_MEM32(FE_STORY)))
        return 0;
    st = M_MEM32(o);
    if (st != 1u && st != 2u)
        return 0;
    s = M_MEM32(o + (st == 1u ? 0x1D8u : 0x1DCu));
    return M_OK(s) && M_OK(M_MEM32(s + 0x38C184u)) ? s : 0;
}

static void m_slots_frame(uint32_t cg, int inside, double gx, double gy, int moves, int l, int *wheel_steps)
{
    static int last_moves = -1, target = -2, click_pending, lr_pending, steps_left;
    static DWORD last_step, click_at;
    uint32_t list = M_MEM32(cg + 0x38C184u);
    int row = (int)M_MEM32(cg), idx = (int)M_MEM32(list + 0x8Cu);
    int count = (int)M_MEM32(list + 0x88u), first = (int)M_MEM32(list + 0xFCu);
    int hovered = -2, side = 0;                  /* -1 the storage row, 0.. a slot entry */
    DWORD now = GetTickCount();

    if (inside && gx >= 40.0 && gx < 600.0 && gy >= 86.0 && gy < 132.0) {
        hovered = -1;
        if (gx >= 320.0)
            side = gx < 465.0 ? -1 : 1;
    } else if (inside && gx >= 50.0 && gx < 300.0 && gy >= 184.0 && gy < 184.0 + 41.0 * 4) {   /* 4 rows shown */
        int r = (int)((gy - 184.0) / 41.0);
        if (first + r < count)
            hovered = first + r;
    }
    if (m_trace() && (l || (hovered > -2 && moves != last_moves)))
        fprintf(stderr, "[MENU] mouse trace: save slots, row %d index %d of %d, at %.0f,%.0f (item %d side %d) clicks %d\n",
                row, idx, count, gx, gy, hovered, side, l);
    if (M_MEM32(FE_NAV) != 0)
        target = -2;
    else if (hovered > -2 && moves != last_moves) {
        target = hovered;
        steps_left = 2 * count + 4;
    }
    last_moves = moves;

    if (l && hovered == -1 && side) {
        lr_pending = side;
        target = -1;
        steps_left = 2 * count + 4;
        click_at = now;
    } else if (l && hovered >= 0) {
        click_pending = 1;
        target = hovered;
        click_at = now;
    }
    if (*wheel_steps && hovered == -1 && row == 0 && side) {
        m_push_x(*wheel_steps > 0 ? 1 : -1);
        *wheel_steps += *wheel_steps > 0 ? -1 : 1;
    } else if (*wheel_steps && M_MEM32(FE_NAV) == 0 && now - last_step >= 50u) {
        M_MEM32(FE_NAV) = *wheel_steps > 0 ? 1u : 2u;
        *wheel_steps += *wheel_steps > 0 ? -1 : 1;
        last_step = now;
        target = -2;
    } else if (target > -2 && M_MEM32(FE_NAV) == 0 && now - last_step >= 50u && steps_left > 0) {
        int up = -1;
        if (target == -1)
            up = row == 1 ? 1 : -1;              /* up the list until the storage row */
        else if (row == 0)
            up = 0;                              /* down into the list */
        else if (idx != target)
            up = target < idx;
        if (up >= 0) {
            M_MEM32(FE_NAV) = up ? 1u : 2u;
            last_step = now;
            steps_left--;
        } else if (!click_pending && !lr_pending)
            target = -2;                         /* there */
    }
    if (lr_pending) {
        if (row == 0) {
            m_push_x(lr_pending);
            lr_pending = 0;
        } else if (now - click_at > 1500u)
            lr_pending = 0;
    }
    if (click_pending) {
        if ((row == 1 && idx == target) || now - click_at > 1500u) {
            if (row == 1 && idx == target) {
                m_set_float(FE_VA + 0x74u, 1.0f);
                m_set_float(FE_VA + 0x14u, 1.0f);
            }
            click_pending = 0;
            target = -2;
        }
    }
}

static void m_mouse_frame(void)
{
    static int last_moves = -1, target = -1, click_pending, wheel_steps, last_sel = -1;
    static int lr_pending, lr_item = -1, steps_left, held_side;
    static const m_layout_t *last_lay, *last_lay_traced;
    static int last_wheel_traced;
    static int step_from = -999;
    static int click_item = -1;
    static uint32_t last_state;
    static DWORD click_at, last_step, last_lr, press_at;
    uint32_t state = M_MEM32(FE_VA);
    m_hover_t h;
    int x, y, cw, ch, moves, inside, l, r, hovered = -1, hover_screen, sel = -1, side = 0, picker = 0;
    double gx = -1.0, gy = -1.0;
    DWORD now = GetTickCount();

    /* 0 intro movies, 1 prologue, 14 Game Demos, 15 leaving: no cursor. */
    InterlockedExchange(&s_fe_cursor, state >= 2u && state <= 13u);
    InterlockedExchange(&s_fe_tick, (LONG)now);
    wheel_steps += bw_fb_mouse_wheel_take();
    bw_fb_mouse_clicks_take(&l, &r);
    if (state < 2u && l) {
        /* The intro movies and the prologue (seen: IntroMovies_HandleInput,
         * PrologueScreen_UpdateActive read accept and start): a click skips,
         * as A or START does. The cursor stays hidden over a movie. */
        m_press_accept_start();
    }
    if (state < 2u || state > 13u) {
        click_pending = lr_pending = 0;
        wheel_steps = 0;
        target = -1;
        last_state = state;
        return;
    }
    if (wheel_steps > 8) wheel_steps = 8;
    if (wheel_steps < -8) wheel_steps = -8;
    inside = bw_fb_mouse(&x, &y, &cw, &ch, &moves);
    if (inside) {
        double s = (double)cw / 640.0 < (double)ch / 480.0 ? (double)cw / 640.0 : (double)ch / 480.0;
        gx = (x - (cw - 640.0 * s) / 2.0) / s;
        gy = (y - (ch - 480.0 * s) / 2.0) / s;
        if (gx < 0.0 || gx >= 640.0 || gy < 0.0 || gy >= 480.0)
            inside = 0;
    }

    {
        uint32_t cg = m_slots_screen(state);
        if (cg) {
            if (l && inside && m_on_back_prompt(state, gx, gy))
                r = 1;                           /* handled below as back */
            else {
                m_slots_frame(cg, inside, gx, gy, moves, l, &wheel_steps);
                l = 0;
            }
            if (r && inside) {
                m_set_float(FE_VA + 0x80u, 1.0f);
                m_set_float(FE_VA + 0x20u, 1.0f);
            }
            target = -1;
            click_pending = lr_pending = 0;
            return;
        }
    }
    if (m_boat_up(state)) {
        m_boat_frame(inside, gx, gy, moves, l, r, &wheel_steps);
        target = -1;
        click_pending = lr_pending = 0;
        return;
    }
    hover_screen = m_hover_screen(state, &h);
    if (!hover_screen || h.lay != last_lay || state != last_state) {
        target = -1;                            /* a new screen: start again */
        lr_pending = 0;
    }
    last_state = state;
    last_lay = hover_screen ? h.lay : NULL;
    if (hover_screen) {
        int i;
        sel = (int)M_MEM32(h.sel_va);
        for (i = 0; i < h.lay->n; i++)
            picker |= h.lay->vx1[i] > h.lay->vx0[i];
        if (inside) {
            hovered = m_hover_item(&h, gx, gy);
            if (hovered >= 0)
                side = m_value_side(&h, hovered, gx);
        }
    }
    if (m_trace()) {
        if ((hover_screen ? h.lay : NULL) != last_lay_traced) {
            fprintf(stderr, "[MENU] mouse trace: screen %u is %s", (unsigned)state, hover_screen ? h.lay->name : "not a hover screen");
            if (hover_screen && h.list)
                fprintf(stderr, " (list of %u, first %u)", (unsigned)M_MEM32(h.list + 0x88u), (unsigned)M_MEM32(h.list + 0xFCu));
            fputc(10, stderr);
            last_lay_traced = hover_screen ? h.lay : NULL;
        }
        if (sel != last_sel)
            fprintf(stderr, "[MENU] mouse trace: screen %u (%s), highlight on item %d\n", (unsigned)state,
                    hover_screen ? h.lay->name : "-", sel);
        if (l || r || wheel_steps != last_wheel_traced || (hovered >= 0 && moves != last_moves))
            fprintf(stderr, "[MENU] mouse trace: screen %u at %.0f,%.0f (item %d side %d) clicks %d/%d wheel %d\n",
                    (unsigned)state, gx, gy, hovered, side, l, r, wheel_steps);
    }
    last_sel = sel;
    last_wheel_traced = wheel_steps;
    if (m_trace() && hover_screen && h.lay == &L_ARENA) {
        static uint32_t last_v;
        uint32_t v = 0, a = h.sel_va - 0x864u;
        int k;
        for (k = 0; k < 5; k++)                 /* the five selectors' items (+0x870.., current +0x80) */
            v = v * 16u + (M_MEM32(a + 0x870u + 0x100u * (uint32_t)k + 0x80u) & 15u);
        if (v != last_v)
            fprintf(stderr, "[MENU] mouse trace: arena setup values %05X\n", (unsigned)v);
        last_v = v;
    }

    if (M_MEM32(FE_NAV) != 0)
        target = -1;                            /* the pad or keys moved it: theirs */
    else if (hovered >= 0 && moves != last_moves && hovered != sel
             && !(h.lay == &L_MISSIONS && hovered == 0)) {
        target = hovered;                       /* the mouse moved onto another item */
        steps_left = 2 * (hover_screen ? h.lay->n + 2 : 8) + (h.list ? (int)M_MEM32(h.list + 0x88u) : 0);
    }
    last_moves = moves;

    /* The wheel: over a picker's value area left/right, else up/down. */
    if (wheel_steps && hovered >= 0 && side && (hovered == sel || (h.lay == &L_MISSIONS && hovered == 0))) {
        if (now - last_lr >= 200u) {
            m_push_x(wheel_steps > 0 ? 1 : -1);
            wheel_steps += wheel_steps > 0 ? -1 : 1;
            last_lr = now;
        }
    } else if (M_MEM32(FE_NAV) == 0 && wheel_steps && now - last_step >= 50u) {
        M_MEM32(FE_NAV) = wheel_steps > 0 ? 1u : 2u;     /* a notch a step */
        wheel_steps += wheel_steps > 0 ? -1 : 1;
        last_step = now;
        target = -1;
    } else if (M_MEM32(FE_NAV) == 0 && target >= 0 && hover_screen) {
        /* One step at a time: the next only once the last one shows (some
         * screens move their highlight with a short slide), or after 0.4 s. */
        if (sel == target || steps_left <= 0)
            target = -1;                        /* there, or it can't be reached (a locked row) */
        else if (now - last_step >= 50u && (sel != step_from || now - last_step >= 400u)) {
            M_MEM32(FE_NAV) = target < sel ? 1u : 2u;
            last_step = now;
            step_from = sel;
            steps_left--;
        }
    }

    /* Clicks. Right click, or the Back prompt: back. The accept prompt:
     * accept. On a picker's value area: left/right (held: again and again,
     * at the game's own pace). On a list item: accept, once the highlight is
     * there. Off the hover screens: accept anywhere on the picture. */
    if (inside && (r || (l && m_on_back_prompt(state, gx, gy)))) {
        m_set_float(FE_VA + 0x80u, 1.0f);       /* merged back */
        m_set_float(FE_VA + 0x20u, 1.0f);       /* player 1's back */
        if (m_popup_up())
            bw_pad_pulse(0, 0, 0, 0, 0x02u, 120);
        l = 0;
        click_pending = lr_pending = 0;
    }
    if (l && inside && m_on_accept_prompt(state, gx, gy)) {
        m_set_float(FE_VA + 0x74u, 1.0f);
        m_set_float(FE_VA + 0x14u, 1.0f);
        l = 0;
    }
    if (l && inside && hovered == 0 && side && h.lay == &L_MISSIONS) {
        /* Mission select's act selector reads the stick every frame, with no
         * highlight of its own (MissionSelect_UpdateActSelector): straight in. */
        m_push_x(side);
        press_at = now;
        held_side = side;
        l = 0;
    } else if (l && inside && hovered >= 0 && side) {
        lr_pending = side;                      /* after the highlight is on that row */
        lr_item = hovered;
        target = hovered != sel ? hovered : -1;
        steps_left = 16;
        click_at = press_at = now;
        held_side = side;
        l = 0;
    } else if (l && inside && (!hover_screen || (hovered >= 0 && !picker) || (hovered >= 0 && !side && h.lay == &L_MISSIONS))) {
        click_pending = 1;
        click_at = now;
        click_item = hovered;
        if (hovered >= 0 && hovered != sel) {
            target = hovered;
            steps_left = 2 * (h.lay ? h.lay->n + 2 : 8) + (h.list ? (int)M_MEM32(h.list + 0x88u) : 0);
        }
    }
    if (lr_pending) {
        if (sel == lr_item) {
            m_push_x(lr_pending);
            last_lr = now;
            lr_pending = 0;
        } else if (now - click_at > 1000u)
            lr_pending = 0;
    }
    /* Held on the same half of the highlighted row's value: repeat after 0.35 s. */
    if (held_side && xbox_FramebufferKeyDown(0x01) && hovered >= 0 && side == held_side
        && (hovered == sel || (h.lay == &L_MISSIONS && hovered == 0))
        && now - press_at > 350u)
        m_push_x(held_side);
    else if (!xbox_FramebufferKeyDown(0x01))
        held_side = 0;
    if (click_pending) {
        /* Accept once the highlight is on the clicked item; a click on an
         * item it can't reach (a locked mission) is dropped after 1 s, never
         * taken as a click on the item that is highlighted. */
        int ready = !(hover_screen && click_item >= 0 && sel != click_item);
        if (!ready && now - click_at > 1000u)
            click_pending = 0;
        if (ready && click_pending) {
            m_set_float(FE_VA + 0x74u, 1.0f);   /* merged accept */
            m_set_float(FE_VA + 0x14u, 1.0f);   /* player 1's accept */
            if (state == FE_ST_TITLE)
                m_press_accept_start();         /* the title takes START only */
            if (m_popup_up())                   /* "press A on your controller": the pad itself */
                bw_pad_pulse(0, 0, 0, 0, 0x01u, 120);
            click_pending = 0;
        }
    }
}

extern void sub_0005C060_gen(void);
void sub_0005C060(void)
{
    sub_0005C060_gen();
    if (bw_fix_enabled(BW_FIX_MENU_MOUSE))
        m_mouse_frame();
}
