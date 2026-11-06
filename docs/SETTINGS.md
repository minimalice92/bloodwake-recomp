# Settings

Every choice in the launcher, tab by tab. The launcher saves them in `%LOCALAPPDATA%\BloodWake\settings.ini`; the game reads them at start-up. A Remastered setting does nothing in Original, which always plays exactly as shipped.

*Generated from the launcher's own settings table (`src/bw_settings.c`, the same table `bloodwake.exe --settings-schema` prints), so it matches the build. "Under the hood" is the switch each choice sets: `fix:+NAME` turns a registered change on, `fix:-NAME` off, `env:NAME=value` sets an environment variable (advanced; the launcher handles all of this).*

## Edition

### Edition

Original plays exactly as it shipped in 2001, bugs and all. Remastered is the same game with its bugs fixed and a modern picture, frame rate and sound, using the original art. Each edition keeps its own saved games.

Applies to: Original and Remastered. Default: **Original (as shipped)**.

| Choice | Under the hood |
|---|---|
| Original (as shipped) (default) | (no switch) |
| Remastered | `env:BW_EDITION=remastered` |

### Preset

Sets the quality settings on the Display and Graphics tabs in one go. Change any of them afterwards and it shows Custom.

Applies to: Remastered. Default: **Best Quality**.

| Choice | Under the hood |
|---|---|
| Best Quality (default) | (no switch) |
| Balanced and Original | (no switch) |
| Best Performance | (no switch) |
| Custom | (no switch) |

## Display

### Display mode

Play in a window or fill the screen. Alt+Enter or F11 switches while you play.

Applies to: Original and Remastered. Default: **Fullscreen (borderless)**.

| Choice | Under the hood |
|---|---|
| Window | (no switch) |
| Fullscreen (borderless) (default) | `env:RECOMP_FULLSCREEN=1` |

### Window size

How big the window opens. It never opens larger than your screen.

Applies to: Original and Remastered. Default: **Large (960 lines)**.

| Choice | Under the hood |
|---|---|
| Small (480 lines) | `env:RECOMP_WINDOW_SCALE=1` |
| Large (960 lines) (default) | (no switch) |
| Huge (1440 lines) | `env:RECOMP_WINDOW_SCALE=3` |

### Picture shape

Widescreen shows more of the world at the sides, with nothing stretched. Match my display uses your screen's shape; 4:3 is the console's.

Applies to: Remastered. Default: **Match my display**.

| Choice | Under the hood |
|---|---|
| Match my display (default) | `fix:+WIDESCREEN` |
| 16:9 | `fix:+WIDESCREEN;env:BW_ASPECT=16:9` |
| 21:9 (ultrawide) | `fix:+WIDESCREEN;env:BW_ASPECT=21:9` |
| 4:3 (as the console) | `fix:-WIDESCREEN` |

### HUD position

In widescreen, puts the radar and status panel in the screen's corners, or keeps the whole HUD in the middle as on the console.

Applies to: Remastered. Default: **Screen corners**.

| Choice | Under the hood |
|---|---|
| Screen corners (default) | `fix:+WS_HUD_EDGES` |
| Middle (as the console) | `fix:-WS_HUD_EDGES` |

### Rendering resolution

How sharp the game is drawn. Native matches your screen or window (sharpest); lower is easier on a slower graphics card.

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

The most frames a second the game draws; it plays at the same speed at any rate. Match my display follows your screen's refresh rate, but 120 or 144 is plenty and keeps the PC cooler and quieter.

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

Stops tearing on screens without G-Sync or FreeSync, at the cost of a little delay.

Applies to: Remastered. Default: **Off**.

| Choice | Under the hood |
|---|---|
| Off (default) | (no switch) |
| On | `env:BW_HIFPS_VSYNC=1` |

### High Priority Mode

Full speed and precise timing, even when the game's window is behind other windows or minimized. Turn it off to save power on a laptop running on battery.

Applies to: Original and Remastered. Default: **On**.

| Choice | Under the hood |
|---|---|
| On (default) | (no switch) |
| Off | `env:BW_PROC_QOS=0;env:BW_AUDIO_HIGHQOS=0` |

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

Keeps textures sharp at an angle, such as distant water and cliffs.

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

Soft shadows where things meet, such as under ledges and around boats. Not part of the original look.

Applies to: Remastered. Default: **Off**.

| Choice | Under the hood |
|---|---|
| On | `fix:+VFX_AO;env:BW_VFX_AO=0.4` |
| Off (default) | `fix:-VFX_AO` |

### Bloom

A soft glow around the brightest highlights, such as the sun and explosions. Not part of the original look.

Applies to: Remastered. Default: **Off**.

| Choice | Under the hood |
|---|---|
| On | `fix:+VFX_BLOOM;env:BW_VFX_BLOOM=0.15` |
| Off (default) | `fix:-VFX_BLOOM` |

### Sharpening

Crisper textures. It changes the original look a little.

Applies to: Remastered. Default: **Off**.

| Choice | Under the hood |
|---|---|
| On | `fix:+VFX_SHARPEN;env:BW_VFX_SHARPEN=0.15` |
| Off (default) | `fix:-VFX_SHARPEN` |

### Edge smoothing (FXAA)

Smooths edges that anti-aliasing misses, such as foliage and wires. Softens text a little.

Applies to: Remastered. Default: **Off**.

| Choice | Under the hood |
|---|---|
| On | `fix:+VFX_FXAA` |
| Off (default) | `fix:-VFX_FXAA` |

### Split-screen detail

In split screen the console drew a coarser sea and, with 3 or 4 players, froze the boats' gunners, turrets and props. Full brings back the animations and the one-player sea in every view; Balanced gives 3 or 4 players the two-player sea, for slower PCs.

Applies to: Remastered. Default: **Full**.

| Choice | Under the hood |
|---|---|
| As the console | `fix:-SPLIT_FULL_DETAIL` |
| Balanced (for slower PCs) | `fix:+SPLIT_FULL_DETAIL;env:BW_SPLIT_WATER=balanced` |
| Full (default) | `fix:+SPLIT_FULL_DETAIL;env:BW_SPLIT_WATER=full` |

### Smooth gradients

Removes banding in dark skies, fog and smoke without changing the look.

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

Smooth, round edges on the HUD's shapes, such as the radar ring and weapon icons, instead of beaded edges with dark specks.

Applies to: Remastered. Default: **On**.

| Choice | Under the hood |
|---|---|
| On (default) | `fix:+VFX_HUD_EDGES` |
| Off | `fix:-VFX_HUD_EDGES` |

### Texture restoration

Cleans the blocky compression out of close-up textures (boats, cliffs, buildings, the menu photos), working only from the original art. Distant views, the sea and the HUD look as before.

Applies to: Remastered. Default: **On**.

| Choice | Under the hood |
|---|---|
| On (default) | `fix:+VFX_TEX_RESTORE` |
| Off | `fix:-VFX_TEX_RESTORE` |

### Automatic HDR

Plays in HDR on a screen that supports it. Windows HDR is switched on while you play and put back as it was afterwards.

Applies to: Remastered. Default: **Off**.

| Choice | Under the hood |
|---|---|
| On | `env:BW_VFX_HDR=auto` |
| Off (default) | `fix:-VFX_HDR;env:BW_VFX_HDR=off` |

### Colour

In HDR, Wide makes reds, greens and blues more vivid on a wide-colour screen; whites and greys stay the same. Accurate shows the colours exactly as made.

Applies to: Remastered. Default: **Accurate (BT.709)**.

| Choice | Under the hood |
|---|---|
| Accurate (BT.709) (default) | `env:BW_VFX_GAMUT=bt709` |
| Wide (P3 stretch) | `env:BW_VFX_GAMUT=p3stretch` |

### OLED care

Slowly dims parts of the picture that stay still for a long time, such as the HUD or a paused menu, to protect an OLED screen from burn-in.

Applies to: Remastered. Default: **Off**.

| Choice | Under the hood |
|---|---|
| On | `fix:+VFX_OLED_CARE` |
| Off (default) | `fix:-VFX_OLED_CARE` |

## Audio

### Sound processor

Accurate plays the console's own echo and 3D sound effects. Simple skips them, for a slow PC.

Applies to: Original and Remastered. Default: **Accurate (as the console)**.

| Choice | Under the hood |
|---|---|
| Accurate (as the console) (default) | (no switch) |
| Simple (faster) | `env:BW_APU_DSP=fold` |

### High-quality resampling

Cleaner sound when the game changes a sound's pitch, such as engines speeding up.

Applies to: Remastered. Default: **On**.

| Choice | Under the hood |
|---|---|
| On (default) | `fix:+AUDIO_SINC_RESAMPLE` |
| Off | `fix:-AUDIO_SINC_RESAMPLE` |

### Surround sound (5.1)

With 5.1 or 7.1 speakers or a surround headset, the game's surround mix uses all of them. Stereo is unaffected.

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

### Mission 10 second strike force

If you sink the first strike force before it reaches Kerang, the second one never arrives and the mission can't end. On, it arrives as scripted.

Applies to: Remastered. Default: **On**.

| Choice | Under the hood |
|---|---|
| On (default) | `fix:+M10_SECOND_STRIKE` |
| Off | `fix:-M10_SECOND_STRIKE` |

### AI target rules

Some enemies in missions 9, 10, 14, 24, 26 and 27 ignored the targets the designers gave them, because 17 of the AI's rules could never match. On, the rules work as written and those fights play a little differently.

Applies to: Remastered. Default: **On**.

| Choice | Under the hood |
|---|---|
| On (default) | `fix:+AI_RULES_AND_AS_OR` |
| Off | `fix:-AI_RULES_AND_AS_OR` |

### Autocannon hit sound

One autocannon hit in three was silent because its recording was never made. On, those hits play one of the other two.

Applies to: Remastered. Default: **On**.

| Choice | Under the hood |
|---|---|
| On (default) | `fix:+AUTOCANNON_HIT_SOUND` |
| Off | `fix:-AUTOCANNON_HIT_SOUND` |

### Hull impact sounds

Hull smacks, splashes and plunges checked the wrong sound before playing, so some were dropped and some restarted over themselves. On, each plays as intended.

Applies to: Remastered. Default: **On**.

| Choice | Under the hood |
|---|---|
| On (default) | `fix:+BOAT_IMPACT_SOUND_SELF` |
| Off | `fix:-BOAT_IMPACT_SOUND_SELF` |

### AI mood swing

The AI was meant to swing slowly between cautious and aggressive over 20 minutes, but a miscalculation made it flicker several times a second. On, it swings as written.

Applies to: Remastered. Default: **On**.

| Choice | Under the hood |
|---|---|
| On (default) | `fix:+AI_MOOD_WAVE_PERIOD` |
| Off | `fix:-AI_MOOD_WAVE_PERIOD` |

### Dreadnought shield-hit sound

On, plays a shield-hit sound the original never played because its name was misspelt.

Applies to: Remastered. Default: **On**.

| Choice | Under the hood |
|---|---|
| On (default) | `fix:+SOUND_SHIELDHITS_NAME` |
| Off | `fix:-SOUND_SHIELDHITS_NAME` |

### Boat landing splash

Every landing after a jump made the heavy plunge sound. On, light landings splash and only long or nose-first ones plunge; little hops still slap the water.

Applies to: Remastered. Default: **On**.

| Choice | Under the hood |
|---|---|
| On (default) | `fix:+BOAT_SPLASH_RANGE;fix:+BOAT_LANDING_WEIGHT` |
| Off | `fix:-BOAT_SPLASH_RANGE;fix:-BOAT_LANDING_WEIGHT` |

### Mission 17 health crate

The health crate comes back every second, forever, unlike every other crate in the story missions. On, it appears once.

Applies to: Remastered. Default: **On**.

| Choice | Under the hood |
|---|---|
| On (default) | `fix:+M17_CRATE_RESPAWN` |
| Off | `fix:-M17_CRATE_RESPAWN` |

### Mission 21 patrol boat

After the destroyer sinks, the patrol boat SeahawkE keeps coming back forever, unlike every other respawning squad. On, it comes back once.

Applies to: Remastered. Default: **On**.

| Choice | Under the hood |
|---|---|
| On (default) | `fix:+M21_PATROL_RESPAWN` |
| Off | `fix:-M21_PATROL_RESPAWN` |

### Missing voice lines

Some radio calls in missions 2, 7, 8, 10, 14 and 19 showed their subtitle in silence because the mission's recording was empty. On, they play the recording the other missions use.

Applies to: Remastered. Default: **On**.

| Choice | Under the hood |
|---|---|
| On (default) | `fix:+VOICE_PLACEHOLDER_BORROW` |
| Off | `fix:-VOICE_PLACEHOLDER_BORROW` |

### Mission 20 map edge

In the south-east corner you can slip past the out-of-bounds line and sail off the map. On, leaving there fails the mission as on the rest of that edge; Off keeps the corner open to explore.

Applies to: Remastered. Default: **On**.

| Choice | Under the hood |
|---|---|
| On (default) | `fix:+M20_EDGE_GAP` |
| Off | `fix:-M20_EDGE_GAP` |

### Mission 23 missing textures

Two textures in mission 23 were never shipped, so the hallway's ceiling grates show as black holes and the arena banners' rods as flat white. On, the closest textures from the game stand in: a riveted girder lattice and pitted copper.

Applies to: Remastered. Default: **On**.

| Choice | Under the hood |
|---|---|
| On (default) | `fix:+M23_MISSING_TEXTURES` |
| Off | `fix:-M23_MISSING_TEXTURES` |

### Battle Mode boat water sounds

In Battle Mode the boats made no splash, slap or spray sounds, because those recordings shipped only with the story missions. On, battles use them too.

Applies to: Remastered. Default: **On**.

| Choice | Under the hood |
|---|---|
| On (default) | `fix:+BATTLE_WATER_SOUNDS` |
| Off | `fix:-BATTLE_WATER_SOUNDS` |

### Headphones mode at start-up

A saved Headphones sound mode only reached the sound chip after you picked it again in the game's options. On, it applies from the start. On stereo you won't hear a difference.

Applies to: Remastered. Default: **On**.

| Choice | Under the hood |
|---|---|
| On (default) | `fix:+HEADPHONES_AT_BOOT` |
| Off | `fix:-HEADPHONES_AT_BOOT` |

### Mission 14 shield cutscene

If you grabbed the shield before blowing up the ammo depots, its cutscene played much later and showed the empty spot where it had been. On, it plays when you grab it.

Applies to: Remastered. Default: **On**.

| Choice | Under the hood |
|---|---|
| On (default) | `fix:+M14_SHIELD_CUTSCENE` |
| Off | `fix:-M14_SHIELD_CUTSCENE` |

### Mission 27 "You hit the Dragon!"

The radio call for hitting the Dragon with your shield's blast was recorded but never played. On, your first shield blast that hits it plays the call.

Applies to: Remastered. Default: **On**.

| Choice | Under the hood |
|---|---|
| On (default) | `fix:+M27_SHIELD_HIT_LINE` |
| Off | `fix:-M27_SHIELD_HIT_LINE` |

### Distant wake foam

Distant boat-wake foam used a scrambled pattern because the game read its mask in the wrong order. Fixed reads it in the intended order; Smooth averages it. The difference is small: try it in a stormy mission.

Applies to: Remastered. Default: **Fixed**.

| Choice | Under the hood |
|---|---|
| As shipped | `fix:-OCEAN_MIPALPHA_FIX` |
| Fixed (default) | `fix:+OCEAN_MIPALPHA_FIX;env:BW_OCEAN_MIPALPHA_FIX=1` |
| Smooth | `fix:+OCEAN_MIPALPHA_FIX;env:BW_OCEAN_MIPALPHA_FIX=2` |

### Distant-water line

Overlapping distant waves got the sky's reflection twice, which draws a thin dark line along them at night. On, the reflection is drawn once.

Applies to: Remastered. Default: **On**.

| Choice | Under the hood |
|---|---|
| On (default) | `fix:+OCEAN_REFLECT_SINGLE` |
| As shipped | `fix:-OCEAN_REFLECT_SINGLE` |

### Game Demos → Quit Game

On a PC the main menu's Game Demos sign only goes back to the title. On, it reads Quit Game and closes the game.

Applies to: Remastered. Default: **On**.

| Choice | Under the hood |
|---|---|
| On (default) | `fix:+MENU_QUIT` |
| Off | `fix:-MENU_QUIT` |

### Mouse in the menus

Point and click in the main and options menus; anywhere in the menus, a right click goes back and the wheel moves up and down. The pointer hides while you play.

Applies to: Remastered. Default: **On**.

| Choice | Under the hood |
|---|---|
| On (default) | `fix:+MENU_MOUSE` |
| Off | `fix:-MENU_MOUSE` |

## Controls

### Keyboard as a controller

The keyboard acts as controller 1, alongside any connected controller. Keys only work while the game's window is active.

Applies to: Original and Remastered. Default: **On**.

| Choice | Under the hood |
|---|---|
| Off | (no switch) |
| On (default) | `env:RECOMP_KEYBOARD=1` |
