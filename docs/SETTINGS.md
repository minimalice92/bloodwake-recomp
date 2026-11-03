# Settings

Every choice in the launcher, tab by tab. The launcher saves them in `%LOCALAPPDATA%\BloodWake\settings.ini`; the game reads them at start-up. A Remastered setting does nothing in Original, which always plays exactly as shipped.

*Generated from the launcher's own settings table (`src/bw_settings.c`, the same table `bloodwake.exe --settings-schema` prints), so it matches the build. "Under the hood" is the switch each choice sets: `fix:+NAME` turns a registered change on, `fix:-NAME` off, `env:NAME=value` sets an environment variable (advanced; the launcher handles all of this).*

## Edition

### Edition

Original is the game exactly as it shipped in 2001, bugs and all. Remastered fixes its bugs and plays it on a modern PC: widescreen, sharper picture, higher frame rate, better sound, with the original art. Each edition keeps its own saves.

Applies to: Original and Remastered. Default: **Original (as shipped)**.

| Choice | Under the hood |
|---|---|
| Original (as shipped) (default) | (no switch) |
| Remastered | `env:BW_EDITION=remastered` |

### Preset

Sets the Display and Graphics tabs in one go; you can still change any setting afterwards (the preset then shows Custom). A preset never changes HDR or OLED care: they depend on your screen.

Applies to: Remastered. Default: **Best Quality**.

| Choice | Under the hood |
|---|---|
| Best Quality (default) | (no switch) |
| Balanced and Original | (no switch) |
| Best Performance | (no switch) |
| Custom | (no switch) |

## Display

### Window

Play in a window, or fill the whole screen without a border (Alt+Enter or F11 switches while you play).

Applies to: Original and Remastered. Default: **Fullscreen (borderless)**.

| Choice | Under the hood |
|---|---|
| Window | (no switch) |
| Fullscreen (borderless) (default) | `env:RECOMP_FULLSCREEN=1` |

### Window size

How big the window opens, in steps of the console's 480-line picture. It shrinks itself to fit the screen.

Applies to: Original and Remastered. Default: **Large (960 lines)**.

| Choice | Under the hood |
|---|---|
| Small (480 lines) | `env:RECOMP_WINDOW_SCALE=1` |
| Large (960 lines) (default) | (no switch) |
| Huge (1440 lines) | `env:RECOMP_WINDOW_SCALE=3` |

### Picture shape

Widescreen shows more of the world at the sides, as a modern game does; nothing is stretched. Match my display picks your monitor's shape (21:9 on an ultrawide). 4:3 is the console's shape.

Applies to: Remastered. Default: **Match my display**.

| Choice | Under the hood |
|---|---|
| Match my display (default) | `fix:+WIDESCREEN` |
| 16:9 | `fix:+WIDESCREEN;env:BW_ASPECT=16:9` |
| 21:9 (ultrawide) | `fix:+WIDESCREEN;env:BW_ASPECT=21:9` |
| 4:3 (as the console) | `fix:-WIDESCREEN` |

### HUD position

In widescreen, put the radar and the status panel in the corners of the screen, or keep the whole HUD in the middle, where the console had it.

Applies to: Remastered. Default: **Screen corners**.

| Choice | Under the hood |
|---|---|
| Screen corners (default) | `fix:+WS_HUD_EDGES` |
| Middle (as the console) | `fix:-WS_HUD_EDGES` |

### Rendering resolution

How sharp the game is drawn. Native draws at your screen's or window's own resolution (sharpest); lower settings are kinder to a slower graphics card. 480p is the console's own resolution.

Applies to: Remastered. Default: **Native (match the screen)**.

| Choice | Under the hood |
|---|---|
| Native (match the screen) (default) | `fix:+VFX_NATIVE_RES` |
| 4K (2160p) | `fix:+VFX_NATIVE_RES;env:BW_VFX_SCALE=4.5` |
| 1440p | `fix:+VFX_NATIVE_RES;env:BW_VFX_SCALE=3` |
| 1080p | `fix:+VFX_NATIVE_RES;env:BW_VFX_SCALE=2.25` |
| 720p | `fix:+VFX_NATIVE_RES;env:BW_VFX_SCALE=1.5` |
| 480p (as the console) | `fix:-VFX_NATIVE_RES` |

### Frame rate cap

The most frames a second the game draws. The console ran missions at 30. Higher rates draw extra frames between the game's own steps, so it looks smoother while playing at the same speed. Match my display follows your screen's refresh rate, which on a very fast monitor means hundreds of frames a second: 120 or 144 is plenty and keeps the PC cooler and quieter.

Applies to: Remastered. Default: **Match my display**.

| Choice | Under the hood |
|---|---|
| Match my display (default) | `fix:+HIFPS` |
| 240 | `fix:+HIFPS;env:BW_HIFPS=240` |
| 144 | `fix:+HIFPS;env:BW_HIFPS=144` |
| 120 | `fix:+HIFPS;env:BW_HIFPS=120` |
| 60 | `fix:+HIFPS;env:BW_HIFPS=60` |
| 30 (as the console) | `fix:-HIFPS` |

### Vertical sync

Wait for the screen before showing each frame: no tearing on screens without variable refresh (G-Sync/FreeSync), a little more delay.

Applies to: Remastered. Default: **Off**.

| Choice | Under the hood |
|---|---|
| Off (default) | (no switch) |
| On | `env:BW_HIFPS_VSYNC=1` |

## Graphics

### Anti-aliasing

Smooths jagged edges. 4x is a good balance; 8x is smoothest and slowest.

Applies to: Remastered. Default: **4x**.

| Choice | Under the hood |
|---|---|
| Off | `fix:-VFX_MSAA` |
| 2x | `fix:+VFX_MSAA;env:BW_VFX_MSAA=2` |
| 4x (default) | `fix:+VFX_MSAA;env:BW_VFX_MSAA=4` |
| 8x | `fix:+VFX_MSAA;env:BW_VFX_MSAA=8` |

### Texture filtering

Keeps textures sharp when seen at an angle, like the water and the cliffs far away.

Applies to: Remastered. Default: **16x**.

| Choice | Under the hood |
|---|---|
| Off (as the console) | `fix:-VFX_ANISO` |
| 4x | `fix:+VFX_ANISO;env:BW_VFX_AF=4` |
| 8x | `fix:+VFX_ANISO;env:BW_VFX_AF=8` |
| 16x (default) | `fix:+VFX_ANISO;env:BW_VFX_AF=16` |

### Detail distance

How far away boats and buildings keep their full detail. Farther looks better and costs a little speed.

Applies to: Remastered. Default: **Far (2x)**.

| Choice | Under the hood |
|---|---|
| As the console | `fix:-VFX_DRAW_DISTANCE` |
| Far (2x) (default) | `fix:+VFX_DRAW_DISTANCE;env:BW_VFX_LOD=2` |
| Farther (3x) | `fix:+VFX_DRAW_DISTANCE;env:BW_VFX_LOD=3` |

### Ambient occlusion

Soft shadows where things meet (under ledges, around boats). Adds depth; off in Balanced and Original because the original didn't have it.

Applies to: Remastered. Default: **Off**.

| Choice | Under the hood |
|---|---|
| On | `fix:+VFX_AO;env:BW_VFX_AO=0.4` |
| Off (default) | `fix:-VFX_AO` |

### Bloom

A soft glow around the brightest highlights (sun, explosions). Off in Balanced and Original: it adds light the original didn't have.

Applies to: Remastered. Default: **Off**.

| Choice | Under the hood |
|---|---|
| On | `fix:+VFX_BLOOM;env:BW_VFX_BLOOM=0.15` |
| Off (default) | `fix:-VFX_BLOOM` |

### Sharpening

Crisper textures. Off in Balanced and Original: it changes the look a little.

Applies to: Remastered. Default: **Off**.

| Choice | Under the hood |
|---|---|
| On | `fix:+VFX_SHARPEN;env:BW_VFX_SHARPEN=0.15` |
| Off (default) | `fix:-VFX_SHARPEN` |

### Edge smoothing (FXAA)

An extra smoothing pass for edges anti-aliasing misses (foliage, wires). Softens text a little.

Applies to: Remastered. Default: **Off**.

| Choice | Under the hood |
|---|---|
| On | `fix:+VFX_FXAA` |
| Off (default) | `fix:-VFX_FXAA` |

### Split-screen detail

In 2-, 3- and 4-player split screen the console drew the sea coarser, and with 3 or 4 players it froze the boats' gunners, turrets and props. Balanced brings back the animations and gives every view the same sea as one player. Balanced gives 3 or 4 players the sea the game draws for 2, for slower PCs (with 4 players about 15% faster than Full).

Applies to: Remastered. Default: **Full**.

| Choice | Under the hood |
|---|---|
| As the console | `fix:-SPLIT_FULL_DETAIL` |
| Balanced (for slower PCs) | `fix:+SPLIT_FULL_DETAIL;env:BW_SPLIT_WATER=balanced` |
| Full (default) | `fix:+SPLIT_FULL_DETAIL;env:BW_SPLIT_WATER=full` |

### Smooth gradients

Draws with more colour precision: no banding in dark skies, fog and smoke, same look.

Applies to: Remastered. Default: **On**.

| Choice | Under the hood |
|---|---|
| On (default) | `fix:+VFX_PRECISION` |
| Off | `fix:-VFX_PRECISION` |

### Sharp text

Draws the game's own lettering crisply at high resolution.

Applies to: Remastered. Default: **On**.

| Choice | Under the hood |
|---|---|
| On (default) | `fix:+VFX_TEXT` |
| Off | `fix:-VFX_TEXT` |

### Smooth HUD edges

Draws the edges of the HUD's shapes (the radar's compass ring, weapon icons) smooth and round at high resolution, instead of a beaded edge with dark specks.

Applies to: Remastered. Default: **On**.

| Choice | Under the hood |
|---|---|
| On (default) | `fix:+VFX_HUD_EDGES` |
| Off | `fix:-VFX_HUD_EDGES` |

### Texture restoration

Texture restoration: close-up textures redrawn more cleanly from the original art; nothing added. Boats, cliffs and buildings seen up close lose the blocky compression steps; from a distance they look exactly as before, and the sea, spray and the HUD are untouched. The menu photos and logo movies are cleaned the same way.

Applies to: Remastered. Default: **On**.

| Choice | Under the hood |
|---|---|
| On (default) | `fix:+VFX_TEX_RESTORE` |
| Off | `fix:-VFX_TEX_RESTORE` |

### Automatic HDR

Plays in HDR on screens that support it: Blood Wake switches Windows HDR on while you play and back afterwards, and maps the picture from the screen's own data by the ITU's standards. Off by default.

Applies to: Remastered. Default: **Off**.

| Choice | Under the hood |
|---|---|
| On | `env:BW_VFX_HDR=auto` |
| Off (default) | `fix:-VFX_HDR;env:BW_VFX_HDR=off` |

### Colour

Wide stretches the game's colours onto the P3 colour range of wide-gamut screens: more vivid reds, greens and blues; white and greys don't change. Only on a screen in HDR mode. Accurate shows the colours exactly as made.

Applies to: Remastered. Default: **Accurate (BT.709)**.

| Choice | Under the hood |
|---|---|
| Accurate (BT.709) (default) | `env:BW_VFX_GAMUT=bt709` |
| Wide (P3 stretch) | `env:BW_VFX_GAMUT=p3stretch` |

### OLED care

Slowly dims parts of the picture that don't change for a long time (the HUD, a paused menu), to spare an OLED screen.

Applies to: Remastered. Default: **Off**.

| Choice | Under the hood |
|---|---|
| On | `fix:+VFX_OLED_CARE` |
| Off (default) | `fix:-VFX_OLED_CARE` |

## Audio

### Sound processor

Accurate runs the Xbox's own sound chip programs (echo, 3D sound) as the console did. Simple skips them, for a slow PC. Applies to every edition.

Applies to: Original and Remastered. Default: **Accurate (as the console)**.

| Choice | Under the hood |
|---|---|
| Accurate (as the console) (default) | (no switch) |
| Simple (faster) | `env:BW_APU_DSP=fold` |

### High-quality resampling

Cleaner, brighter sound when the game changes a sound's pitch (engines, speed).

Applies to: Remastered. Default: **On**.

| Choice | Under the hood |
|---|---|
| On (default) | `fix:+AUDIO_SINC_RESAMPLE` |
| Off | `fix:-AUDIO_SINC_RESAMPLE` |

### Louder output

The game mixes quietly; this lifts the whole mix a little, with a limiter so nothing clips. The balance between music, effects and voices stays the same.

Applies to: Remastered. Default: **On**.

| Choice | Under the hood |
|---|---|
| On (default) | `fix:+AUDIO_MAKEUP_GAIN` |
| Off | `fix:-AUDIO_MAKEUP_GAIN` |

### Surround sound (5.1)

With 5.1 or 7.1 speakers (or a surround headset), the game's surround mix comes out of all of them. Stereo setups are not affected.

Applies to: Remastered. Default: **On**.

| Choice | Under the hood |
|---|---|
| On (default) | `fix:+AUDIO_SURROUND` |
| Off | `fix:-AUDIO_SURROUND` |

### Low latency

Sound reacts a little sooner to what happens on screen.

Applies to: Remastered. Default: **On**.

| Choice | Under the hood |
|---|---|
| On (default) | `fix:+AUDIO_LOW_LATENCY` |
| Off | `fix:-AUDIO_LOW_LATENCY` |

## Fixes

### Mission 10 can always be won

Sink the first strike force before it reaches Kerang and the second one waits off the map forever, so the mission can never end. On, the second force comes as scripted.

Applies to: Remastered. Default: **On**.

| Choice | Under the hood |
|---|---|
| On (default) | `fix:+M10_SECOND_STRIKE` |
| Off | `fix:-M10_SECOND_STRIKE` |

### AI target rules

Seventeen of the AI's target rules were written so they could never match, so in missions 9, 10, 14, 24, 26 and 27 some enemies ignored the targets the designers gave them (in mission 10 the strike force never went for Kerang's batteries). On, they work as written, and those fights play a little differently.

Applies to: Remastered. Default: **On**.

| Choice | Under the hood |
|---|---|
| On (default) | `fix:+AI_RULES_AND_AS_OR` |
| Off | `fix:-AI_RULES_AND_AS_OR` |

### Autocannon hit sound

One autocannon hit in three was silent: its third recording was never made. On, those hits play one of the other two.

Applies to: Remastered. Default: **On**.

| Choice | Under the hood |
|---|---|
| On (default) | `fix:+AUTOCANNON_HIT_SOUND` |
| Off | `fix:-AUTOCANNON_HIT_SOUND` |

### Hull impact sounds

Hull smacks, splashes and plunges checked the wrong sound before playing, so some were dropped and some restarted over themselves.

Applies to: Remastered. Default: **On**.

| Choice | Under the hood |
|---|---|
| On (default) | `fix:+BOAT_IMPACT_SOUND_SELF` |
| Off | `fix:-BOAT_IMPACT_SOUND_SELF` |

### AI mood swing

The AI's slow 20-minute swing between cautious and aggressive was miscalculated into random noise several times a second. On, it swings slowly as written.

Applies to: Remastered. Default: **On**.

| Choice | Under the hood |
|---|---|
| On (default) | `fix:+AI_MOOD_WAVE_PERIOD` |
| Off | `fix:-AI_MOOD_WAVE_PERIOD` |

### Dreadnought shield-hit sound

A sound the original never played because its name was misspelt.

Applies to: Remastered. Default: **On**.

| Choice | Under the hood |
|---|---|
| On (default) | `fix:+SOUND_SHIELDHITS_NAME` |
| Off | `fix:-SOUND_SHIELDHITS_NAME` |

### Boat landing splash

The splash sound for landing after a jump never played: every landing plunged. On, light landings splash and heavy ones (a second or more in the air, or nose first) plunge; little hops still slap the water as before.

Applies to: Remastered. Default: **On**.

| Choice | Under the hood |
|---|---|
| On (default) | `fix:+BOAT_SPLASH_RANGE;fix:+BOAT_LANDING_WEIGHT` |
| Off | `fix:-BOAT_SPLASH_RANGE;fix:-BOAT_LANDING_WEIGHT` |

### Mission 17 health crate

The health crate comes back every second, forever: the only endless crate in the story missions. On, it appears once like the mission's other crates (on by default: the maintainer's call).

Applies to: Remastered. Default: **On**.

| Choice | Under the hood |
|---|---|
| On (default) | `fix:+M17_CRATE_RESPAWN` |
| Off | `fix:-M17_CRATE_RESPAWN` |

### Mission 21 patrol boat

After the destroyer sinks, the patrol boat SeahawkE keeps coming back forever: the only one of 100 respawning squads that does. On, it comes back once like the others (on by default: the maintainer's call).

Applies to: Remastered. Default: **On**.

| Choice | Under the hood |
|---|---|
| On (default) | `fix:+M21_PATROL_RESPAWN` |
| Off | `fix:-M21_PATROL_RESPAWN` |

### Missing voice lines

In missions 2, 7, 8, 10, 14 and 19 some radio calls (the out-of-bounds warnings, the friendly-fire calls, one taunt) showed their subtitle in silence: the mission's copy of the recording was left empty. On, they play the same recording the other missions use.

Applies to: Remastered. Default: **On**.

| Choice | Under the hood |
|---|---|
| On (default) | `fix:+VOICE_PLACEHOLDER_BORROW` |
| Off | `fix:-VOICE_PLACEHOLDER_BORROW` |

### Mission 20 map edge

In the south-east corner you can slip past the end of the out-of-bounds line and sail off the map. On, leaving there fails the mission like the rest of that edge (on by default: the maintainer's call; off leaves the corner open to explore).

Applies to: Remastered. Default: **On**.

| Choice | Under the hood |
|---|---|
| On (default) | `fix:+M20_EDGE_GAP` |
| Off | `fix:-M20_EDGE_GAP` |

### Mission 23 missing textures

Two textures in mission 23 were never shipped, so the grates in the hallway's ceiling show as plain black holes and the rods on the arena's banners are flat white. On, they use the closest match from the game's own textures: a riveted girder lattice for the grates, and pitted copper for the rods. (Advanced: BW_M23_GRATE and BW_M23_BRASS pick other textures by name.)

Applies to: Remastered. Default: **On**.

| Choice | Under the hood |
|---|---|
| On (default) | `fix:+M23_MISSING_TEXTURES` |
| Off | `fix:-M23_MISSING_TEXTURES` |

### Battle Mode boat water sounds

In Battle Mode the boats made no splash, slap or spray sounds on the water: those recordings were only packed with the story missions. On, battles use the same water sounds the story missions play.

Applies to: Remastered. Default: **On**.

| Choice | Under the hood |
|---|---|
| On (default) | `fix:+BATTLE_WATER_SOUNDS` |
| Off | `fix:-BATTLE_WATER_SOUNDS` |

### Mission 14 shield cutscene

Grab the shield before blowing up the ammo depots and its cutscene played much later, showing the empty spot where the shield had been. On, it plays when you grab the shield.

Applies to: Remastered. Default: **On**.

| Choice | Under the hood |
|---|---|
| On (default) | `fix:+M14_SHIELD_CUTSCENE` |
| Off | `fix:-M14_SHIELD_CUTSCENE` |

### Mission 27 "You hit the Dragon!"

The radio call for turning the Dragon's Claw back into the Dragon with your shield was recorded but never played: the game listened for a weapon that nothing reports. On, the first shield blast that hits the Dragon plays it.

Applies to: Remastered. Default: **On**.

| Choice | Under the hood |
|---|---|
| On (default) | `fix:+M27_SHIELD_HIT_LINE` |
| Off | `fix:-M27_SHIELD_HIT_LINE` |

### Distant wake foam

The game builds the far-away versions of the boat-wake foam from a mask it reads in the wrong order, so distant foam uses a scrambled pattern (almost certainly a slip; the sea's colour, waves and sky are not affected). Fixed reads it in the order the code intended; Smooth averages it instead. The difference is very small: try it in a stormy mission and decide.

Applies to: Remastered. Default: **Fixed**.

| Choice | Under the hood |
|---|---|
| As shipped | `fix:-OCEAN_MIPALPHA_FIX` |
| Fixed (default) | `fix:+OCEAN_MIPALPHA_FIX;env:BW_OCEAN_MIPALPHA_FIX=1` |
| Smooth | `fix:+OCEAN_MIPALPHA_FIX;env:BW_OCEAN_MIPALPHA_FIX=2` |

### Distant-water line

Where waves overlap each other in the distance, the game puts the sky's reflection on the water twice, which draws a thin dark line along the wave at night. On, the reflection goes on once; nothing else about the sea changes.

Applies to: Remastered. Default: **On**.

| Choice | Under the hood |
|---|---|
| On (default) | `fix:+OCEAN_REFLECT_SINGLE` |
| As shipped | `fix:-OCEAN_REFLECT_SINGLE` |

### Game Demos → Quit Game

The main menu's Game Demos sign starts the console's demo disc, which a PC can't run (it only goes back to the title). On, the sign reads Quit Game and closes the game, as closing the window does.

Applies to: Remastered. Default: **On**.

| Choice | Under the hood |
|---|---|
| On (default) | `fix:+MENU_QUIT` |
| Off | `fix:-MENU_QUIT` |

### Mouse in the menus

Use the mouse in the game's menus: in the main menu and the options menu, point at an item to highlight it and click to choose it; everywhere in the menus a right click goes back and the wheel moves up and down. The pointer hides while you play.

Applies to: Remastered. Default: **On**.

| Choice | Under the hood |
|---|---|
| On (default) | `fix:+MENU_MOUSE` |
| Off | `fix:-MENU_MOUSE` |

## Controls

### Keyboard as a controller

Lets the keyboard act as controller 1 together with any pad: a connected controller keeps working as well. Keys only count while the game's window has the focus.

Applies to: Original and Remastered. Default: **On**.

| Choice | Under the hood |
|---|---|
| Off | (no switch) |
| On (default) | `env:RECOMP_KEYBOARD=1` |
