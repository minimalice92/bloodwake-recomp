/*
 * bw_settings.c - the settings file and its table (bw_settings.h). Agent D5.
 *
 * Each setting has a few options; each option says what it does in terms of
 * switches the port already reads:
 *   "fix:+ID" / "fix:-ID"   a change in the registry (src/bw_fixes.def), on or
 *                           off; collected into BW_FIX ahead of any BW_FIX the
 *                           environment has, so the environment's tokens win
 *   "env:NAME=VALUE"        an environment variable, set only if the
 *                           environment doesn't already have one
 * several separated by ';'. A setting the file doesn't mention does nothing,
 * so the game's own defaults stand. Fix tokens a setting would give an
 * edition that doesn't have that change (Original has none) are dropped, so
 * a file written for one edition can't stop another edition's boot.
 */
#include <windows.h>
#include <shlobj.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bw_edition.h"
#include "bw_settings.h"
#include "bw_hookcheck.h"

typedef struct {
    const char *value, *label, *effect;
} BwOpt;

typedef struct {
    const char *tab, *section, *key, *label, *tip, *dflt;
    const char *editions;       /* "all", or "remastered" (and every later edition) */
    BwOpt opt[8];
    const char *kind;           /* NULL: a list of choices; "check": a checkbox (on/off) */
} BwSetting;

#define ON_OFF(ID) { { "on", "On", "fix:+" #ID }, { "off", "Off", "fix:-" #ID } }

static const BwSetting s_set[] = {
    /* ---- Edition ------------------------------------------------------- */
    { "Edition", "edition", "edition", "Edition",
      "Original is the game exactly as it shipped in 2001, bugs and all. Remastered fixes "
      "its bugs and plays it on a modern PC: widescreen, sharper picture, higher frame rate, "
      "better sound, with the original art. Each edition keeps its own saves.",
      "original", "all",
      { { "original", "Original (as shipped)", "" },
        { "remastered", "Remastered", "env:BW_EDITION=remastered" },
      } },
    /* The launcher's preset picker (D-021; the presets are s_preset, below).
     * The game doesn't act on it: the launcher writes every setting a preset
     * sets, so these choices switch nothing themselves. */
    { "Edition", "edition", "preset", "Preset",
      "Sets the Display and Graphics tabs in one go; you can still change any setting "
      "afterwards (the preset then shows Custom). A preset never changes HDR or OLED care: "
      "they depend on your screen.",
      "best", "remastered",
      { { "best", "Best Quality", "" },
        { "balanced", "Balanced and Original", "" },
        { "performance", "Best Performance", "" },
        { "custom", "Custom", "" } } },

    /* ---- Display --------------------------------------------------------- */
    /* Item 102 (the maintainer): Fullscreen (borderless) by default. A file that says
     * mode = window (0.2.4 wrote it for everyone) stays Window: no migration
     * (call 83). No settings file at all still means the game's own window.
     * Never in a preset. */
    { "Display", "display", "mode", "Window",
      "Play in a window, or fill the whole screen without a border (Alt+Enter or F11 "
      "switches while you play).",
      "fullscreen", "all",
      { { "window", "Window", "" },
        { "fullscreen", "Fullscreen (borderless)", "env:RECOMP_FULLSCREEN=1" } } },
    { "Display", "display", "window_size", "Window size",
      "How big the window opens, in steps of the console's 480-line picture. It shrinks "
      "itself to fit the screen.",
      "large", "all",
      { { "small", "Small (480 lines)", "env:RECOMP_WINDOW_SCALE=1" },
        { "large", "Large (960 lines)", "" },
        { "huge", "Huge (1440 lines)", "env:RECOMP_WINDOW_SCALE=3" } } },
    { "Display", "display", "aspect", "Picture shape",
      "Widescreen shows more of the world at the sides, as a modern game does; nothing is "
      "stretched. Match my display picks your monitor's shape (21:9 on an ultrawide). 4:3 "
      "is the console's shape.",
      "auto", "remastered",
      { { "auto", "Match my display", "fix:+WIDESCREEN" },
        { "16:9", "16:9", "fix:+WIDESCREEN;env:BW_ASPECT=16:9" },
        { "21:9", "21:9 (ultrawide)", "fix:+WIDESCREEN;env:BW_ASPECT=21:9" },
        { "4:3", "4:3 (as the console)", "fix:-WIDESCREEN" } } },
    { "Display", "display", "hud", "HUD position",
      "In widescreen, put the radar and the status panel in the corners of the screen, or "
      "keep the whole HUD in the middle, where the console had it.",
      "edges", "remastered",
      { { "edges", "Screen corners", "fix:+WS_HUD_EDGES" },
        { "centre", "Middle (as the console)", "fix:-WS_HUD_EDGES" } } },
    { "Display", "display", "resolution", "Rendering resolution",
      "How sharp the game is drawn. Native draws at your screen's or window's own "
      "resolution (sharpest); lower settings are kinder to a slower graphics card. 480p is "
      "the console's own resolution.",
      "native", "remastered",
      { { "native", "Native (match the screen)", "fix:+VFX_NATIVE_RES" },
        { "2160", "4K (2160p)", "fix:+VFX_NATIVE_RES;env:BW_VFX_SCALE=4.5" },
        { "1440", "1440p", "fix:+VFX_NATIVE_RES;env:BW_VFX_SCALE=3" },
        { "1080", "1080p", "fix:+VFX_NATIVE_RES;env:BW_VFX_SCALE=2.25" },
        { "720", "720p", "fix:+VFX_NATIVE_RES;env:BW_VFX_SCALE=1.5" },
        { "480", "480p (as the console)", "fix:-VFX_NATIVE_RES" } } },
    { "Display", "display", "framerate", "Frame rate cap",
      "The most frames a second the game draws. The console ran missions at 30. Higher "
      "rates draw extra frames between the game's own steps, so it looks smoother while "
      "playing at the same speed. Match my display follows your screen's refresh rate, "
      "which on a very fast monitor means hundreds of frames a second: 120 or 144 is "
      "plenty and keeps the PC cooler and quieter.",
      "display", "remastered",
      { { "display", "Match my display", "fix:+HIFPS" },
        { "240", "240", "fix:+HIFPS;env:BW_HIFPS=240" },
        { "144", "144", "fix:+HIFPS;env:BW_HIFPS=144" },
        { "120", "120", "fix:+HIFPS;env:BW_HIFPS=120" },
        { "60", "60", "fix:+HIFPS;env:BW_HIFPS=60" },
        { "30", "30 (as the console)", "fix:-HIFPS" } } },
    { "Display", "display", "vsync", "Vertical sync",
      "Wait for the screen before showing each frame: no tearing on screens without "
      "variable refresh (G-Sync/FreeSync), a little more delay.",
      "off", "remastered",
      { { "off", "Off", "" },
        { "on", "On", "env:BW_HIFPS_VSYNC=1" } } },

    /* ---- Graphics -------------------------------------------------------- */
    { "Graphics", "graphics", "antialiasing", "Anti-aliasing",
      "Smooths jagged edges. 4x is a good balance; 8x is smoothest and slowest.",
      "4", "remastered",
      { { "off", "Off", "fix:-VFX_MSAA" },
        { "2", "2x", "fix:+VFX_MSAA;env:BW_VFX_MSAA=2" },
        { "4", "4x", "fix:+VFX_MSAA;env:BW_VFX_MSAA=4" },
        { "8", "8x", "fix:+VFX_MSAA;env:BW_VFX_MSAA=8" } } },
    { "Graphics", "graphics", "texture_filtering", "Texture filtering",
      "Keeps textures sharp when seen at an angle, like the water and the cliffs far away.",
      "16", "remastered",
      { { "off", "Off (as the console)", "fix:-VFX_ANISO" },
        { "4", "4x", "fix:+VFX_ANISO;env:BW_VFX_AF=4" },
        { "8", "8x", "fix:+VFX_ANISO;env:BW_VFX_AF=8" },
        { "16", "16x", "fix:+VFX_ANISO;env:BW_VFX_AF=16" } } },
    { "Graphics", "graphics", "draw_distance", "Detail distance",
      "How far away boats and buildings keep their full detail. Farther looks better "
      "and costs a little speed.",
      "far", "remastered",
      { { "original", "As the console", "fix:-VFX_DRAW_DISTANCE" },
        { "far", "Far (2x)", "fix:+VFX_DRAW_DISTANCE;env:BW_VFX_LOD=2" },
        { "farther", "Farther (3x)", "fix:+VFX_DRAW_DISTANCE;env:BW_VFX_LOD=3" } } },
    { "Graphics", "graphics", "ambient_occlusion", "Ambient occlusion",
      "Soft shadows where things meet (under ledges, around boats). Adds depth; off in "
      "Balanced and Original because the original didn't have it.",
      "off", "remastered",              /* On's levels here and below: S1's (D-021) */
      { { "on", "On", "fix:+VFX_AO;env:BW_VFX_AO=0.4" }, { "off", "Off", "fix:-VFX_AO" } } },
    { "Graphics", "graphics", "bloom", "Bloom",
      "A soft glow around the brightest highlights (sun, explosions). Off in Balanced and "
      "Original: it adds light the original didn't have.",
      "off", "remastered",
      { { "on", "On", "fix:+VFX_BLOOM;env:BW_VFX_BLOOM=0.15" }, { "off", "Off", "fix:-VFX_BLOOM" } } },
    { "Graphics", "graphics", "sharpen", "Sharpening",
      "Crisper textures. Off in Balanced and Original: it changes the look a little.",
      "off", "remastered",
      { { "on", "On", "fix:+VFX_SHARPEN;env:BW_VFX_SHARPEN=0.15" }, { "off", "Off", "fix:-VFX_SHARPEN" } } },
    { "Graphics", "graphics", "fxaa", "Edge smoothing (FXAA)",
      "An extra smoothing pass for edges anti-aliasing misses (foliage, wires). Softens "
      "text a little.",
      "off", "remastered", ON_OFF(VFX_FXAA) },
    { "Graphics", "graphics", "split_detail", "Split-screen detail",
      "In 2-, 3- and 4-player split screen the console drew the sea coarser, and with 3 or 4 "
      "players it froze the boats' gunners, turrets and props. Balanced brings back the "
      "animations and gives every view the same sea as one player. Balanced gives 3 or 4 "
      "players the sea the game draws for 2, for slower PCs (with 4 players about 15% "
      "faster than Full).",
      "full", "remastered",
      { { "console", "As the console", "fix:-SPLIT_FULL_DETAIL" },
        { "balanced", "Balanced (for slower PCs)", "fix:+SPLIT_FULL_DETAIL;env:BW_SPLIT_WATER=balanced" },
        { "full", "Full", "fix:+SPLIT_FULL_DETAIL;env:BW_SPLIT_WATER=full" } } },
    { "Graphics", "graphics", "smooth_gradients", "Smooth gradients",
      "Draws with more colour precision: no banding in dark skies, fog and smoke, same look.",
      "on", "remastered", ON_OFF(VFX_PRECISION) },
    { "Graphics", "graphics", "sharp_text", "Sharp text",
      "Draws the game's own lettering crisply at high resolution.",
      "on", "remastered", ON_OFF(VFX_TEXT) },
    { "Graphics", "graphics", "smooth_hud_edges", "Smooth HUD edges",
      "Draws the edges of the HUD's shapes (the radar's compass ring, weapon icons) "
      "smooth and round at high resolution, instead of a beaded edge with dark specks.",
      "on", "remastered", ON_OFF(VFX_HUD_EDGES) },
    { "Graphics", "graphics", "texture_restoration", "Texture restoration",
      "Texture restoration: close-up textures redrawn more cleanly from the original art; "
      "nothing added. Boats, cliffs and buildings seen up close lose the blocky "
      "compression steps; from a distance they look exactly as before, and the sea, "
      "spray and the HUD are untouched. The menu photos and logo movies are cleaned the same way.",
      "on", "remastered", ON_OFF(VFX_TEX_RESTORE) },
    /* Item 100 (D-026, reversed to opt-in): one checkbox, Automatic HDR, off
     * by default. Checked: the game's HDR from the screen's own data
     * (BW_VFX_HDR=auto), and the launcher switches Windows HDR on for the
     * session when the screen supports HDR and reports its data, and back
     * afterwards (tools/launcher.pyw). Unchecked: no HDR highlights
     * (BW_VFX_HDR=off); the game's standards mapping on a screen already in
     * HDR mode always applies. 0.2.4's hdr = on reads as checked, off as
     * unchecked. BW_VFX_HDR=on (forced, HDR1000 defaults) stays an engine and
     * [advanced] switch only. Never in a preset (D-021). */
    { "Graphics", "graphics", "hdr", "Automatic HDR",
      "Plays in HDR on screens that support it: Blood Wake switches Windows HDR on while you "
      "play and back afterwards, and maps the picture from the screen's own data by the ITU's "
      "standards. Off by default.",
      "off", "remastered",
      { { "on", "On", "env:BW_VFX_HDR=auto" },
        { "off", "Off", "fix:-VFX_HDR;env:BW_VFX_HDR=off" } },
      "check" },
    /* Item 99 (the maintainer): the game's BT.709 colours read as P3-D65 on a screen
     * in HDR mode (bw_vfx_post.c). Never in a preset (it depends on the
     * screen, like HDR); Accurate is the default. */
    { "Graphics", "graphics", "gamut", "Colour",
      "Wide stretches the game's colours onto the P3 colour range of wide-gamut screens: more "
      "vivid reds, greens and blues; white and greys don't change. Only on a screen in HDR mode. "
      "Accurate shows the colours exactly as made.",
      "bt709", "remastered",
      { { "bt709", "Accurate (BT.709)", "env:BW_VFX_GAMUT=bt709" },
        { "p3stretch", "Wide (P3 stretch)", "env:BW_VFX_GAMUT=p3stretch" } } },
    { "Graphics", "graphics", "oled_care", "OLED care",
      "Slowly dims parts of the picture that don't change for a long time (the HUD, a "
      "paused menu), to spare an OLED screen.",
      "off", "remastered", ON_OFF(VFX_OLED_CARE) },

    /* ---- Audio ------------------------------------------------------------- */
    { "Audio", "audio", "sound_processor", "Sound processor",
      "Accurate runs the Xbox's own sound chip programs (echo, 3D sound) as the console "
      "did. Simple skips them, for a slow PC. Applies to every edition.",
      "accurate", "all",
      { { "accurate", "Accurate (as the console)", "" },
        { "simple", "Simple (faster)", "env:BW_APU_DSP=fold" } } },
    { "Audio", "audio", "resampling", "High-quality resampling",
      "Cleaner, brighter sound when the game changes a sound's pitch (engines, speed).",
      "on", "remastered", ON_OFF(AUDIO_SINC_RESAMPLE) },
    { "Audio", "audio", "louder", "Louder output",
      "The game mixes quietly; this lifts the whole mix a little, with a limiter so "
      "nothing clips. The balance between music, effects and voices stays the same.",
      "on", "remastered", ON_OFF(AUDIO_MAKEUP_GAIN) },
    { "Audio", "audio", "surround", "Surround sound (5.1)",
      "With 5.1 or 7.1 speakers (or a surround headset), the game's surround mix comes "
      "out of all of them. Stereo setups are not affected.",
      "on", "remastered", ON_OFF(AUDIO_SURROUND) },
    { "Audio", "audio", "low_latency", "Low latency",
      "Sound reacts a little sooner to what happens on screen.",
      "on", "remastered", ON_OFF(AUDIO_LOW_LATENCY) },


    /* ---- Fixes ----------------------------------------------------------------- */
    { "Fixes", "fixes", "mission10", "Mission 10 can always be won",
      "Sink the first strike force before it reaches Kerang and the second one waits off the "
      "map forever, so the mission can never end. On, the second force comes as scripted.",
      "on", "remastered", ON_OFF(M10_SECOND_STRIKE) },
    { "Fixes", "fixes", "ai_rules", "AI target rules",
      "Seventeen of the AI's target rules were written so they could never match, so in "
      "missions 9, 10, 14, 24, 26 and 27 some enemies ignored the targets the designers gave "
      "them (in mission 10 the strike force never went for Kerang's batteries). On, they work "
      "as written, and those fights play a little differently.",
      "on", "remastered", ON_OFF(AI_RULES_AND_AS_OR) },
    { "Fixes", "fixes", "autocannon_sound", "Autocannon hit sound",
      "One autocannon hit in three was silent: its third recording was never made. On, those "
      "hits play one of the other two.",
      "on", "remastered", ON_OFF(AUTOCANNON_HIT_SOUND) },
    { "Fixes", "fixes", "hull_sounds", "Hull impact sounds",
      "Hull smacks, splashes and plunges checked the wrong sound before playing, so some were "
      "dropped and some restarted over themselves.",
      "on", "remastered", ON_OFF(BOAT_IMPACT_SOUND_SELF) },
    { "Fixes", "fixes", "ai_mood", "AI mood swing",
      "The AI's slow 20-minute swing between cautious and aggressive was miscalculated into "
      "random noise several times a second. On, it swings slowly as written.",
      "on", "remastered", ON_OFF(AI_MOOD_WAVE_PERIOD) },
    { "Fixes", "fixes", "shield_hits", "Dreadnought shield-hit sound",
      "A sound the original never played because its name was misspelt.",
      "on", "remastered", ON_OFF(SOUND_SHIELDHITS_NAME) },
    { "Fixes", "fixes", "boat_splash", "Boat landing splash",
      "The splash sound for landing after a jump never played: every landing plunged. On, "
      "light landings splash and heavy ones (a second or more in the air, or nose first) "
      "plunge; little hops still slap the water as before.",
      "on", "remastered",
      { { "on", "On", "fix:+BOAT_SPLASH_RANGE;fix:+BOAT_LANDING_WEIGHT" },
        { "off", "Off", "fix:-BOAT_SPLASH_RANGE;fix:-BOAT_LANDING_WEIGHT" } } },
    { "Fixes", "fixes", "m17_crate", "Mission 17 health crate",
      "The health crate comes back every second, forever: the only endless crate in the "
      "story missions. On, it appears once like the mission's other crates (on by default: "
      "the maintainer's call).",
      "on", "remastered", ON_OFF(M17_CRATE_RESPAWN) },
    { "Fixes", "fixes", "m21_patrol", "Mission 21 patrol boat",
      "After the destroyer sinks, the patrol boat SeahawkE keeps coming back forever: the "
      "only one of 100 respawning squads that does. On, it comes back once like the others "
      "(on by default: the maintainer's call).",
      "on", "remastered", ON_OFF(M21_PATROL_RESPAWN) },
    { "Fixes", "fixes", "voice_lines", "Missing voice lines",
      "In missions 2, 7, 8, 10, 14 and 19 some radio calls (the out-of-bounds "
      "warnings, the friendly-fire calls, one taunt) showed their subtitle in silence: the "
      "mission's copy of the recording was left empty. On, they play the same recording the "
      "other missions use.",
      "on", "remastered", ON_OFF(VOICE_PLACEHOLDER_BORROW) },
    { "Fixes", "fixes", "m20_edge", "Mission 20 map edge",
      "In the south-east corner you can slip past the end of the out-of-bounds "
      "line and sail off the map. On, leaving there fails the mission like the rest of that "
      "edge (on by default: the maintainer's call; off leaves the corner open to explore).",
      "on", "remastered", ON_OFF(M20_EDGE_GAP) },
    { "Fixes", "fixes", "m23_textures", "Mission 23 missing textures",
      "Two textures in mission 23 were never shipped, so the grates in the hallway's "
      "ceiling show as plain black holes and the rods on the arena's banners are flat "
      "white. On, they use the closest match from the game's own textures: a riveted girder "
      "lattice for the grates, and pitted copper for the rods. (Advanced: "
      "BW_M23_GRATE and BW_M23_BRASS pick other textures by name.)",
      "on", "remastered", ON_OFF(M23_MISSING_TEXTURES) },
    { "Fixes", "fixes", "battle_water", "Battle Mode boat water sounds",
      "In Battle Mode the boats made no splash, slap or spray sounds on the water: those "
      "recordings were only packed with the story missions. On, battles use the same water "
      "sounds the story missions play.",
      "on", "remastered", ON_OFF(BATTLE_WATER_SOUNDS) },
    { "Fixes", "fixes", "m14_shield", "Mission 14 shield cutscene",
      "Grab the shield before blowing up the ammo depots and its cutscene played much later, "
      "showing the empty spot where the shield had been. On, it plays when you grab the shield.",
      "on", "remastered", ON_OFF(M14_SHIELD_CUTSCENE) },
    { "Fixes", "fixes", "m27_shield_hit", "Mission 27 \"You hit the Dragon!\"",
      "The radio call for turning the Dragon's Claw back into the Dragon with your shield was "
      "recorded but never played: the game listened for a weapon that nothing reports. On, "
      "the first shield blast that hits the Dragon plays it.",
      "on", "remastered", ON_OFF(M27_SHIELD_HIT_LINE) },
    { "Fixes", "fixes", "distant_wake", "Distant wake foam",
      "The game builds the far-away versions of the boat-wake foam from a mask it reads in "
      "the wrong order, so distant foam uses a scrambled pattern (almost certainly a slip; "
      "the sea's colour, waves and sky are not affected). Fixed reads it in the order the "
      "code intended; Smooth averages it instead. The difference is very small: try it in "
      "a stormy mission and decide.",
      "fixed", "remastered",
      { { "off", "As shipped", "fix:-OCEAN_MIPALPHA_FIX" },
        { "fixed", "Fixed", "fix:+OCEAN_MIPALPHA_FIX;env:BW_OCEAN_MIPALPHA_FIX=1" },
        { "smooth", "Smooth", "fix:+OCEAN_MIPALPHA_FIX;env:BW_OCEAN_MIPALPHA_FIX=2" } } },
    { "Fixes", "fixes", "water_line", "Distant-water line",
      "Where waves overlap each other in the distance, the game puts the sky's reflection on "
      "the water twice, which draws a thin dark line along the wave at night. On, the "
      "reflection goes on once; nothing else about the sea changes.",
      "on", "remastered",
      { { "on", "On", "fix:+OCEAN_REFLECT_SINGLE" },
        { "off", "As shipped", "fix:-OCEAN_REFLECT_SINGLE" } } },
    /* C3's items 78 and 79 (src/bw_menu_hooks.c). The arrow is written as its
     * UTF-8 bytes, so the source stays ASCII; js() turns it into →. */
    { "Fixes", "fixes", "menu_quit", "Game Demos \xe2\x86\x92 Quit Game",
      "The main menu's Game Demos sign starts the console's demo disc, which a PC can't run "
      "(it only goes back to the title). On, the sign reads Quit Game and closes the game, "
      "as closing the window does.",
      "on", "remastered", ON_OFF(MENU_QUIT) },
    { "Fixes", "fixes", "menu_mouse", "Mouse in the menus",
      "Use the mouse in the game's menus: in the main menu and the options menu, point at an "
      "item to highlight it and click to choose it; everywhere in the menus a right click goes "
      "back and the wheel moves up and down. The pointer hides while you play.",
      "on", "remastered", ON_OFF(MENU_MOUSE) },

    /* ---- Controls ---------------------------------------------------------------- */
    /* Call 89: On by default (mouse-and-keyboard play without a pad); a file
     * from before 0.2.5 with keyboard = off reads as On (see s_old_default).
     * The keyboard answers for port 0 only and is merged on top of a pad
     * there (toolkit_patches/input), so a connected controller keeps working.
     * Never in a preset. */
    { "Controls", "controls", "keyboard", "Keyboard as a controller",
      "Lets the keyboard act as controller 1 together with any pad: a connected controller "
      "keeps working as well. Keys only count while the game's window has the focus.",
      "on", "all",
      { { "off", "Off", "" },
        { "on", "On", "env:RECOMP_KEYBOARD=1" } } },
};
#define BW_NSET ((int)(sizeof s_set / sizeof s_set[0]))

/* ---- Remastered's presets (D-021) ------------------------------------------
 * Picking one in the launcher sets every setting in BW_PRESET_SETS: to the
 * choice the preset names, or else to that setting's own default above (which
 * is today's Remastered, so Balanced and Original names nothing). The game
 * never reads a preset; a run without the launcher's file plays as before.
 * Never in a preset: HDR and OLED care (they depend on the screen), the
 * window, the HUD position and vsync (taste, not speed). */
#define BW_PRESET_SETS "display.aspect display.resolution display.framerate " \
    "graphics.antialiasing graphics.texture_filtering graphics.draw_distance " \
    "graphics.ambient_occlusion graphics.bloom graphics.sharpen graphics.fxaa " \
    "graphics.split_detail graphics.smooth_gradients graphics.sharp_text " \
    "graphics.smooth_hud_edges graphics.texture_restoration"
/* Best Quality's choices: S1's (docs/day1/S1.md, D-021), this one line; the
 * levels of AO, bloom and sharpening are S1's too, in those settings' On. */
#define BW_PRESET_BEST "graphics.antialiasing=8 graphics.texture_filtering=16 graphics.draw_distance=farther graphics.ambient_occlusion=on graphics.bloom=on graphics.sharpen=on graphics.fxaa=off"

static const struct { const char *value, *text, *choices; } s_preset[] = {
    { "best", "A modern game on max settings: every upgrade and extra on, ambient occlusion, "
              "bloom and gentle sharpening included, tuned so nothing overlaps.",
      BW_PRESET_BEST },
    { "balanced", "The original game true to form, looking its best on a modern screen: none "
                  "of the extras that change the look.",
      "" },
    { "performance", "For a slower PC: the cheapest settings that keep the essentials "
                     "(widescreen, a high frame rate, light anti-aliasing), so it still feels "
                     "like a modern game.",
      "display.framerate=60 graphics.antialiasing=2 graphics.texture_filtering=4 "
      "graphics.draw_distance=original graphics.split_detail=balanced "
      "graphics.smooth_gradients=off graphics.texture_restoration=off" },
};
#define BW_NPRESET ((int)(sizeof s_preset / sizeof s_preset[0]))

/* ---- the settings the shaders depend on (item 21) ----------------------------
 * A choice here changes the text of the game's pixel shaders (seen: the
 * combiner key's vfx bits, src/gpu/bw_d3d11.c "VFX (D9)" and "T1", set from
 * bw_vfx.c), so after a change the graphics warm-up (tools/bw_warmup.py) has
 * new shaders to prepare. All are Remastered's: Original's shaders never
 * change. `classes` groups the choices that make the same shaders, as
 * "value=class ...": anti-aliasing matters only on or off (alpha to
 * coverage); its level is in no shader. Empty: every choice is its own. */
static const struct { const char *id, *classes; } s_shader_set[] = {
    { "graphics.antialiasing", "off=off 2=msaa 4=msaa 8=msaa" },
    { "graphics.sharp_text", "" },
    { "graphics.smooth_hud_edges", "" },
    { "graphics.texture_restoration", "" },
};
#define BW_NSHADER ((int)(sizeof s_shader_set / sizeof s_shader_set[0]))

/* ---- the file ------------------------------------------------------------- */

/* Wide all the way (item 18): a user name like "Zoë", or one outside the
 * ANSI code page, reaches the file intact. The environment is read from the
 * process block itself, which keeps every character the launcher passed. */
const wchar_t *bw_settings_path_w(void)
{
    static wchar_t path[MAX_PATH];
    wchar_t e[MAX_PATH], base[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"BW_SETTINGS", e, MAX_PATH);
    if (n >= MAX_PATH)
        return NULL;                             /* too long for a path */
    if (n) {
        if (!_wcsicmp(e, L"none") || !wcscmp(e, L"0") || !_wcsicmp(e, L"off"))
            return NULL;
        _snwprintf(path, MAX_PATH, L"%ls", e);
    } else if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, base)))
        _snwprintf(path, MAX_PATH, L"%ls\\BloodWake\\settings.ini", base);
    else
        return NULL;
    path[MAX_PATH - 1] = 0;
    return path;
}

const char *bw_settings_path(void)
{
    static char path[MAX_PATH * 3];
    const wchar_t *w = bw_settings_path_w();
    if (!w || !WideCharToMultiByte(CP_UTF8, 0, w, -1, path, (int)sizeof path, NULL, NULL))
        return NULL;
    return path;
}

#define BW_MAXKV 160
typedef struct { char sec[32], key[64], val[256]; } BwKv;

static void trim(char *s)
{
    char *e;
    while (*s && isspace((unsigned char)*s))
        memmove(s, s + 1, strlen(s));
    e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1]))
        *--e = 0;
}

static int read_file(const wchar_t *path, BwKv *kv, int max)
{
    FILE *f = _wfopen(path, L"r");
    char line[512], sec[32] = "";
    int n = 0;
    if (!f)
        return -1;
    while (fgets(line, sizeof line, f) && n < max) {
        char *eq;
        trim(line);
        if (!line[0] || line[0] == ';' || line[0] == '#')
            continue;
        if (line[0] == '[') {
            char *c = strchr(line, ']');
            if (c) {
                *c = 0;
                _snprintf(sec, sizeof sec, "%s", line + 1);
                sec[sizeof sec - 1] = 0;
                _strlwr(sec);
            }
            continue;
        }
        if (!(eq = strchr(line, '=')))
            continue;
        *eq = 0;
        _snprintf(kv[n].sec, sizeof kv[n].sec, "%s", sec);
        _snprintf(kv[n].key, sizeof kv[n].key, "%s", line);
        _snprintf(kv[n].val, sizeof kv[n].val, "%s", eq + 1);
        kv[n].sec[sizeof kv[n].sec - 1] = kv[n].key[sizeof kv[n].key - 1] = 0;
        kv[n].val[sizeof kv[n].val - 1] = 0;
        trim(kv[n].key);
        trim(kv[n].val);
        n++;
    }
    fclose(f);
    return n;
}

static const char *find_kv(const BwKv *kv, int n, const char *sec, const char *key)
{
    int i;
    for (i = n - 1; i >= 0; i--)                 /* the last one wins */
        if (!_stricmp(kv[i].sec, sec) && !_stricmp(kv[i].key, key))
            return kv[i].val;
    return NULL;
}

/* The launcher writes "[launcher] format = 2" from 0.2.5 on; a file without
 * it is from 0.2.4 or before (format 1). (Item 100 reversed call 67: 0.2.4's
 * hdr = off stays off, on stays on.) */
#define BW_SETTINGS_FORMAT 2
static int file_format(const BwKv *kv, int n)
{
    const char *v = find_kv(kv, n, "launcher", "format");
    return v && atoi(v) > 0 ? atoi(v) : 1;
}

/* 0.2.4's launcher wrote every setting, so its defaults are in every file it
 * saved; where 0.2.5 changed a default, a file from before 0.2.5 (format 1)
 * that still holds the old default reads as the new one. A choice made in
 * 0.2.5's launcher (format 2) stays; no file at all changes nothing.
 * tools/launcher.pyw shows the file by the same table (OLD_DEFAULTS).
 *   call 85 (the maintainer): mode = window, held back then by the
 *     keyboard-and-mouse support and the missing Quit Game -> Fullscreen;
 *   call 89: keyboard = off -> On (mouse-and-keyboard play without a pad). */
static const struct { const char *section, *key, *was, *now; } s_old_default[] = {
    { "display", "mode", "window", "fullscreen" },
    { "controls", "keyboard", "off", "on" },
};

static const char *before_025(const BwSetting *st, const char *v, const BwKv *kv, int n)
{
    int i;
    for (i = 0; i < (int)(sizeof s_old_default / sizeof s_old_default[0]); i++)
        if (!strcmp(st->section, s_old_default[i].section) && !strcmp(st->key, s_old_default[i].key)
            && !_stricmp(v, s_old_default[i].was))
            return file_format(kv, n) < BW_SETTINGS_FORMAT ? s_old_default[i].now : NULL;
    return NULL;
}

static int env_set(const char *name)
{
    const char *v = getenv(name);
    return v && *v;
}

/* ---- applying it -------------------------------------------------------------- */

static int fix_ok_for(const char *id, bw_edition_t ed)
{
    int i;
    if (ed == BW_ED_ORIGINAL)
        return 0;
    for (i = 0; i < BW_FIX_COUNT; i++)
        if (!_stricmp(g_bw_fixes[i].name, id))
            return (g_bw_fixes[i].editions & BW_ED_BIT(ed)) != 0;
    return 0;
}

/* An [advanced] line's name is a switch: capitals, digits and '_', and not
 * one the file sets another way. */
static int adv_name_ok(const char *key)
{
    const char *p;
    if (!key[0])
        return 0;
    for (p = key; *p; p++)
        if (!(isupper((unsigned char)*p) || isdigit((unsigned char)*p) || *p == '_'))
            return 0;
    return strcmp(key, "BW_SETTINGS") && strcmp(key, "BW_EDITION") && strcmp(key, "BW_FIX");
}

/* The [advanced] line that sets switch `name` (the last one, as everywhere
 * in the file), or -1. */
static int adv_line(const BwKv *kv, int n, const char *name)
{
    int i;
    for (i = n - 1; i >= 0; i--)
        if (!_stricmp(kv[i].sec, "advanced") && adv_name_ok(kv[i].key) && !_stricmp(kv[i].key, name))
            return i;
    return -1;
}

void bw_settings_apply(void)
{
    static BwKv kv[BW_MAXKV];
    static char fix[2048], log[2048], kept[512], over[512];
    const char *path = bw_settings_path(), *ev;
    bw_edition_t ed = BW_ED_ORIGINAL;
    int n, i, k, choices = 0, envs = 0;

    if (!path) {
        fprintf(stderr, "[SET] no settings file (BW_SETTINGS=none): switches from the environment only\n");
        return;
    }
    /* A boot under the host window inherits the host's environment, which
     * already holds the file's choices: applying them again would only
     * repeat the BW_FIX tokens. */
    ev = getenv("BW_SETTINGS_APPLIED");
    if (ev && *ev) {
        fprintf(stderr, "[SET] settings %s: applied by the launching process (inherited)\n", ev);
        return;
    }
    n = read_file(bw_settings_path_w(), kv, BW_MAXKV);
    if (n < 0) {
        fprintf(stderr, "[SET] no settings file (%s): the game's defaults\n", path);
        return;
    }
    fix[0] = log[0] = kept[0] = over[0] = 0;

    /* The edition first: it decides which changes a token may name. */
    ev = getenv("BW_EDITION");
    if (!(ev && *ev)) {
        const char *v = find_kv(kv, n, "edition", "edition");
        if (v && bw_edition_parse(v, &ed) == 0 && ed != BW_ED_ORIGINAL)
            _putenv_s("BW_EDITION", bw_edition_name(ed));
    } else if (bw_edition_parse(ev, &ed) != 0)
        ed = BW_ED_ORIGINAL;                     /* main.c refuses it later */

    for (i = 0; i < BW_NSET; i++) {
        const BwSetting *st = &s_set[i];
        const char *v = find_kv(kv, n, st->section, st->key);
        const BwOpt *o = NULL;
        char eff[256], *tok, *ctx = NULL;
        if (!v || (!strcmp(st->key, "edition") && !strcmp(st->section, "edition")))
            continue;
        /* A setting for a later edition does nothing in an edition that
         * doesn't have it: Original takes only the "all" ones (window, keys,
         * the sound processor). */
        if ((!strcmp(st->editions, "remastered") && ed == BW_ED_ORIGINAL)
            )
            continue;
        {
            const char *now = before_025(st, v, kv, n);
            if (now) {
                fprintf(stderr, "[SET] %s.%s = %s in a settings file from before 0.2.5 (no "
                        "[launcher] format line): read as %s, 0.2.5's default\n",
                        st->section, st->key, v, now);
                v = now;
            }
        }
        for (k = 0; k < 8 && st->opt[k].value; k++)
            if (!_stricmp(st->opt[k].value, v))
                o = &st->opt[k];
        if (!o) {
            fprintf(stderr, "[SET] %s.%s = %s: not one of its choices; ignored\n",
                    st->section, st->key, v);
            continue;
        }
        choices++;
        _snprintf(eff, sizeof eff, "%s", o->effect);
        eff[sizeof eff - 1] = 0;
        for (tok = strtok_s(eff, ";", &ctx); tok; tok = strtok_s(NULL, ";", &ctx)) {
            if (!strncmp(tok, "fix:", 4)) {
                if (fix_ok_for(tok + 5, ed)) {
                    strncat(fix, fix[0] ? "," : "", sizeof fix - strlen(fix) - 1);
                    strncat(fix, tok + 4, sizeof fix - strlen(fix) - 1);
                }
            } else if (!strncmp(tok, "env:", 4)) {
                char name[64], *val = strchr(tok + 4, '=');
                if (!val)
                    continue;
                _snprintf(name, sizeof name, "%.*s", (int)(val - (tok + 4)), tok + 4);
                name[sizeof name - 1] = 0;
                if (env_set(name)) {
                    strncat(kept, " ", sizeof kept - strlen(kept) - 1);
                    strncat(kept, name, sizeof kept - strlen(kept) - 1);
                } else if (adv_line(kv, n, name) >= 0) {
                    /* Item 19: an [advanced] line for the same switch wins
                     * over the table's choice (it is set below). */
                    strncat(over, " ", sizeof over - strlen(over) - 1);
                    strncat(over, name, sizeof over - strlen(over) - 1);
                } else {
                    _putenv_s(name, val + 1);
                    envs++;
                    strncat(log, " ", sizeof log - strlen(log) - 1);
                    strncat(log, tok + 4, sizeof log - strlen(log) - 1);
                }
            }
        }
    }
    /* [advanced]: raw switches, NAME = value. The environment still wins;
     * over the table's choices, they win (item 19). */
    for (i = 0; i < n; i++) {
        if (_stricmp(kv[i].sec, "advanced"))
            continue;
        if (!adv_name_ok(kv[i].key)) {
            fprintf(stderr, "[SET] advanced.%s: not a switch name; ignored\n", kv[i].key);
            continue;
        }
        if (adv_line(kv, n, kv[i].key) != i)
            continue;                            /* a later line for it wins */
        if (env_set(kv[i].key)) {
            strncat(kept, " ", sizeof kept - strlen(kept) - 1);
            strncat(kept, kv[i].key, sizeof kept - strlen(kept) - 1);
        } else {
            _putenv_s(kv[i].key, kv[i].val);
            envs++;
            strncat(log, " ", sizeof log - strlen(log) - 1);
            strncat(log, kv[i].key, sizeof log - strlen(log) - 1);
            strncat(log, "=", sizeof log - strlen(log) - 1);
            strncat(log, kv[i].val, sizeof log - strlen(log) - 1);
        }
    }
    /* The file's changes go first; the environment's BW_FIX, if any, after
     * them, so its tokens win (BW_FIX is read left to right). */
    if (fix[0]) {
        static char merged[4096];
        const char *old = getenv("BW_FIX");
        _snprintf(merged, sizeof merged, "%s%s%s", fix, old && *old ? "," : "", old && *old ? old : "");
        merged[sizeof merged - 1] = 0;
        _putenv_s("BW_FIX", merged);
    }
    fprintf(stderr, "[SET] settings %s: %d choices for %s; changes %s; switches set%s%s%s%s%s%s\n",
            path, choices, bw_edition_title(ed), fix[0] ? fix : "(none)",
            envs ? "" : " (none)", log, kept[0] ? "; kept from the environment:" : "", kept,
            over[0] ? "; [advanced] wins over the table for:" : "", over);
    fflush(stderr);
    _putenv_s("BW_SETTINGS_APPLIED", path);
}

/* ---- the schema, for the launcher ------------------------------------------------ */

/* A JSON string of s (UTF-8: the table's text, and bw_settings_path()), in
 * plain ASCII: anything else is a \u escape (UTF-16, so a character beyond
 * U+FFFF is a surrogate pair), so the launcher reads the same characters
 * whatever code page it decodes with (item 18). */
static void js(FILE *f, const char *s)
{
    wchar_t buf[1024], *w = buf;
    int n = s && *s ? MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0) : 0, i;
    if (n > (int)(sizeof buf / sizeof buf[0]))
        w = (wchar_t *)malloc(n * sizeof *w);
    if (n > 0 && (!w || !MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n)))
        n = 0;
    fputc('"', f);
    for (i = 0; i < n - 1; i++) {
        if (w[i] == '"' || w[i] == '\\')
            fprintf(f, "\\%c", (char)w[i]);
        else if (w[i] < 0x20 || w[i] >= 0x7f)
            fprintf(f, "\\u%04x", (unsigned)w[i]);
        else
            fputc((char)w[i], f);
    }
    fputc('"', f);
    if (w && w != buf)
        free(w);
}

/* Where id starts a word of a space-separated list (the word ending there or
 * at '='), or NULL. */
static const char *find_word(const char *list, const char *id)
{
    size_t len = strlen(id);
    const char *s;
    for (s = strstr(list, id); s; s = strstr(s + 1, id))
        if ((s == list || s[-1] == ' ') && (!s[len] || s[len] == ' ' || s[len] == '='))
            return s;
    return NULL;
}

/* Each preset with the choice it gives every preset setting; a slip in the
 * table is reported on stderr and left out, so the launcher never sees it. */
static void print_presets(FILE *f)
{
    int p, i, k;
    fprintf(f, "  \"presets\": [\n");
    for (p = 0; p < BW_NPRESET; p++) {
        char buf[1024], *id, *ctx = NULL;
        int n = 0;
        _snprintf(buf, sizeof buf, "%s", s_preset[p].choices);
        buf[sizeof buf - 1] = 0;
        for (id = strtok_s(buf, " ", &ctx); id; id = strtok_s(NULL, " ", &ctx)) {
            char *eq = strchr(id, '=');
            if (eq)
                *eq = 0;
            if (!eq || !find_word(BW_PRESET_SETS, id))
                fprintf(stderr, "[SET] preset %s: %s is not a preset setting; ignored\n",
                        s_preset[p].value, id);
        }
        fprintf(f, "    {\"value\": ");
        js(f, s_preset[p].value);
        fprintf(f, ", \"text\": ");
        js(f, s_preset[p].text);
        fprintf(f, ",\n     \"settings\": {");
        _snprintf(buf, sizeof buf, "%s", BW_PRESET_SETS);
        buf[sizeof buf - 1] = 0;
        ctx = NULL;
        for (id = strtok_s(buf, " ", &ctx); id; id = strtok_s(NULL, " ", &ctx)) {
            const BwSetting *st = NULL;
            const char *w = find_word(s_preset[p].choices, id);
            char name[96], val[64];
            for (i = 0; i < BW_NSET && !st; i++) {
                _snprintf(name, sizeof name, "%s.%s", s_set[i].section, s_set[i].key);
                name[sizeof name - 1] = 0;
                if (!strcmp(name, id))
                    st = &s_set[i];
            }
            if (w && w[strlen(id)] == '=') {
                w += strlen(id) + 1;
                _snprintf(val, sizeof val, "%.*s", (int)strcspn(w, " "), w);
            } else
                _snprintf(val, sizeof val, "%s", st ? st->dflt : "");
            val[sizeof val - 1] = 0;
            for (k = 0; st && k < 8 && st->opt[k].value && strcmp(st->opt[k].value, val); k++)
                ;
            if (!st || k == 8 || !st->opt[k].value) {
                fprintf(stderr, "[SET] preset %s: %s=%s is not a setting and one of its choices; "
                        "left out\n", s_preset[p].value, id, val);
                continue;
            }
            fprintf(f, "%s", n++ ? ", " : "");
            js(f, id);
            fprintf(f, ": ");
            js(f, val);
        }
        fprintf(f, "}}%s\n", p + 1 < BW_NPRESET ? "," : "");
    }
    fprintf(f, "  ]\n");
}

void bw_settings_print_schema(FILE *f)
{
    int i, k;
    const char *path = bw_settings_path();
    fprintf(f, "{\n  \"file\": ");
    js(f, path ? path : "");
    /* C3's --keys-schema is in this exe (main.c, bw_keys.c): the launcher asks
     * for it only when the schema says so (an exe without it would boot). */
    fprintf(f, ",\n  \"keys_schema\": true");
    /* And B3's --screen-info (D-026): the launcher's screen line and Auto-HDR. */
    fprintf(f, ",\n  \"screen_info\": true");
    fprintf(f, ",\n  \"tabs\": [\"Edition\", \"Display\", \"Graphics\", \"Audio\", "
               "\"Fixes\", \"Controls\"],\n  \"advanced_section\": \"advanced\",\n  \"settings\": [\n");
    for (i = 0; i < BW_NSET; i++) {
        const BwSetting *st = &s_set[i];
        fprintf(f, "    {\"tab\": ");
        js(f, st->tab);
        fprintf(f, ", \"section\": ");
        js(f, st->section);
        fprintf(f, ", \"key\": ");
        js(f, st->key);
        fprintf(f, ", \"label\": ");
        js(f, st->label);
        fprintf(f, ", \"default\": ");
        js(f, st->dflt);
        if (st->kind) {                             /* item 100: "check" draws a checkbox */
            fprintf(f, ", \"kind\": ");
            js(f, st->kind);
        }
        fprintf(f, ", \"editions\": ");
        js(f, st->editions);
        {   /* item 62, contract 7: an option's fix whose code differs on this copy */
            const char *un = NULL;
            for (k = 0; k < 8 && st->opt[k].value && !un; k++)
                un = bw_hook_unavailable(st->opt[k].effect);
            if (un) {
                fprintf(f, ", \"unavailable\": ");
                js(f, un);
            }
        }
        fprintf(f, ",\n     \"tip\": ");
        js(f, st->tip);
        fprintf(f, ",\n     \"options\": [");
        for (k = 0; k < 8 && st->opt[k].value; k++) {
            fprintf(f, "%s{\"value\": ", k ? ", " : "");
            js(f, st->opt[k].value);
            fprintf(f, ", \"label\": ");
            js(f, st->opt[k].label);
            fprintf(f, "}");
        }
        fprintf(f, "]}%s\n", i + 1 < BW_NSET ? "," : "");
    }
    fprintf(f, "  ],\n  \"shader_settings\": [\n");
    for (i = 0; i < BW_NSHADER; i++) {
        char buf[256], *tok, *ctx = NULL;
        int c = 0;
        fprintf(f, "    {\"id\": ");
        js(f, s_shader_set[i].id);
        fprintf(f, ", \"classes\": {");
        _snprintf(buf, sizeof buf, "%s", s_shader_set[i].classes);
        buf[sizeof buf - 1] = 0;
        for (tok = strtok_s(buf, " ", &ctx); tok; tok = strtok_s(NULL, " ", &ctx)) {
            char *eq = strchr(tok, '=');
            if (!eq)
                continue;
            *eq = 0;
            fprintf(f, "%s", c++ ? ", " : "");
            js(f, tok);
            fprintf(f, ": ");
            js(f, eq + 1);
        }
        fprintf(f, "}}%s\n", i + 1 < BW_NSHADER ? "," : "");
    }
    fprintf(f, "  ],\n");
    print_presets(f);
    fprintf(f, "}\n");
}
