# Editions

One executable, two editions. You pick one in the launcher (Edition tab), or with `BW_EDITION=original|remastered` for advanced use. Each edition keeps its own saves (under `%LOCALAPPDATA%\xboxrecomp\`; Remastered's in `editions\remastered`); the first time you start Remastered, it begins with a copy of your Original saves.

## Original
The game exactly as it shipped in 2001, its quirks and bugs included, at the console's 4:3 picture and 30 frames a second, in a window or borderless fullscreen.

The only things fixed are problems the port itself would otherwise cause (they're fixed in both editions). Original is the reference: every change Remastered makes is tested against it with a fixed random seed, step by step, to prove Original stays exactly as shipped.

## Remastered
The same game, with the **original art and sound**, looking, running and sounding its best on a modern PC. Nothing is replaced or redrawn: no new textures, models, sounds or music. Every change has its own switch (see [SETTINGS.md](SETTINGS.md)), so you can turn any of it off.

### Display
- **Widescreen**: 16:9, or 21:9 on an ultrawide, showing more of the world at the sides. Nothing is stretched. 4:3 stays available.
- **HUD in the corners** in widescreen (or in the middle, where the console had it).
- **Native resolution**: drawn at your screen's own resolution, up to 4K, or a fixed 720p, 1080p or 1440p for slower graphics cards.
- **Higher frame rate**: 60, 120, 144 or 240 frames a second, or your display's refresh rate. The game still runs its own 30 steps a second, so it plays at the same speed; the extra frames are drawn in between, which makes it look smoother. Vertical sync optional; variable refresh (G-Sync/FreeSync) works.
- **Low-latency frames** (on by default): each frame is started just in time, so the picture answers your controls sooner and more evenly, at the cost of some frame rate. Untick it for the most frames a second.

### Graphics
- **Anti-aliasing** (2x, 4x, 8x) and **texture filtering** (up to 16x), which keeps the water and distant cliffs sharp.
- **Longer detail distance**: boats and buildings keep their full detail further away.
- **Smooth gradients**: more colour precision, so dark skies, fog and smoke don't band.
- **Sharp text** and **smooth HUD edges** at high resolution.
- **Texture restoration**: close-up textures redrawn more cleanly from the original art, without the blocky compression steps. Nothing is added; from a distance they look exactly as before.
- **Split-screen detail**: in 2-4 player split screen the console drew a coarser sea and, with 3 or 4 players, froze the boats' gunners and turrets. Full and Balanced bring the detail back.
- **Optional extras, off by default** because the original didn't have them: ambient occlusion, bloom, sharpening, an extra edge-smoothing pass (FXAA), **HDR** on an HDR screen, and **OLED care**, which slowly dims parts of the picture that don't change for a long time.

### Sound
- **Higher-quality resampling**: cleaner, brighter sound when the game changes a sound's pitch (engines, speed).
- **Surround**: the game's own surround mix on 5.1 or 7.1 speakers or a surround headset.
- **Low latency**: sound reacts a little sooner.

The Xbox sound chip's own effects (echo and 3D sound) run as on the console in **both** editions (Audio tab, Sound processor: Accurate).

### The original game's own bugs, fixed
Each fix is on by default in Remastered and has its own switch in the launcher's Fixes tab. Original keeps every one of them as shipped.
- A story mission that could never be finished if you sank the first strike force too early.
- AI target rules that could never match, so some enemies ignored the targets the designers gave them.
- Sounds that never played: one autocannon hit in three, a shield-hit sound with a misspelt name, the splash for a light landing, a mission's recorded radio call, and the water sounds in Battle Mode.
- Sound checks that dropped or restarted hull impact sounds.
- The AI's slow swing between cautious and aggressive, miscalculated into random noise.
- Radio calls in six missions that showed their subtitle in silence.
- An endless health crate and an endlessly respawning patrol boat, each the only one of its kind in the story missions.
- A gap at one mission's map edge, through which you could sail off the map.
- Two textures that were never shipped, shown as black holes and flat white (replaced by the closest match from the game's own textures).
- A shield cutscene that played at the wrong time.
- A saved Headphones sound mode that only took effect after you picked it again in the options.
- The far-away boat-wake foam, built from a mask read in the wrong order.

The full list, with what each one changes, is in [SETTINGS.md](SETTINGS.md) under Fixes.

## In both editions
- **High Priority Mode** (Display tab, on by default): the game keeps its full speed and precise timing even when its window is behind other windows or minimized. Turn it off to save power on a laptop running on battery; the sound's own safeguards against gaps stay on either way.
- **The sound at the game's own level**: the mix goes out exactly as the game makes it, nothing boosted, squashed or ducked.

## Planned
A third edition, **Expanded**, will add the developers' cut and half-finished content, completed. It isn't in this repository yet. <!-- audit:allow expanded -->
