![Blood Wake Recomp](docs/images/banner.svg)

**Blood Wake (Xbox, 2001), running natively on Windows.** The game's own program is translated into C and compiled on your PC, from your own disc: static recompilation, not emulation.

**100% complete.** Every function in the game recompiled to native code with none failing, and the whole game playable in both editions: all 28 story missions, Battle Mode and split screen for up to four players. Play it exactly as it shipped, or remastered: ultrawide, 4K and up to 290 frames a second, with the original art and sound.

**[Download the installer](https://github.com/minimalice92/bloodwake-recomp/releases)** · [Features and Showcase](#features-and-showcase) · [Getting started](#getting-started) · [How it was made](#how-it-was-made) · [The roadmap](#the-roadmap) · [Staying safe](#staying-safe)

![Remastered at 4K: a gunboat at full speed into a low sunset, its wake trailing, the HUD in the corners](docs/images/shot-hero.jpg)
*Full speed into the sunset. Remastered at 4K with every setting maxed: the original art, nothing redrawn or replaced.*

> [!TIP]
> **New in 0.3.1, native graphics:** all of the Xbox graphics chip's drawing now runs on your graphics card in both editions, within about 25 pixels in a million of 0.3.0's pictures, each a shade or a few lighter or darker, and the loading screen shows its picture and moving bar again. In Remastered, each frame is about 11 ms fresher when the graphics card finishes it, and the sound plays at the game's own level, with nothing squashed (about 4 dB quieter than 0.3.0: turn it up a notch). A new High Priority Mode asks Windows not to slow the game down to save power. Original still plays step for step as it shipped. [Read the release notes](https://github.com/minimalice92/bloodwake-recomp/releases/latest).

> [!NOTE]
> Unofficial, free and non-commercial. You need your own Blood Wake disc, and nothing from the game is in this repository. See [Staying safe](#staying-safe).

<img src="docs/images/stats.svg" width="600" alt="100%: recompiled, 0 functions failed, the whole game plays. 8,110 functions named. About 3 days from the first plan to the first release. 80+ AI agents. 0 crashes in 948 mission runs of soak tests. 18 bugs from 2001 fixed in Remastered.">

## Features and Showcase
One program, two editions, picked in the launcher.

### Original: the game as it shipped
- **Every mission and mode:** Story Mode's 28 missions, Battle Mode, and couch split screen for 2 to 4 players.
- **Exactly as shipped in 2001:** the 4:3 picture, 30 frames a second, its quirks and its bugs. Only problems the port itself would cause are fixed.
- **The console's own pace and sound:** the game runs on the console's 59.94 Hz clock, and the Xbox sound chip's own echo and 3D-sound programs run as on the console, now translated to native code as the game starts: the same output word for word, checked over 15.4 million frames side by side with the emulator.
- **Drawn by your graphics card:** every piece of the graphics chip's drawing runs natively, within about 25 pixels in a million of the processor's path (nearly all of them sky, by one shade of 256), and the loading screen shows its picture and moving bar, as on the console.
- **Its own saves**, kept apart from Remastered's.
- **Proven unchanged:** every merge is replayed against the previous build with the same random seed and compared step by step, for example 2,990 of 2,990 steps identical.

### Remastered: the same game at its best
The original art and sound, never replaced. Every change has its own switch.

**Looks**
- 16:9 and 21:9 widescreen: more of the world, nothing stretched, the HUD in the corners.
- Native resolution up to 4K, 8x anti-aliasing, 16x texture filtering, longer detail distance.
- Cleaner textures up close, no banding, sharp text. Ambient occlusion, bloom and an OLED care mode.
- **HDR done by the book:** on an HDR screen the picture is mapped from the screen's own data (its EDID), by the ITU's standards, so there are no washed-out greys and nothing to set up. Tick **Automatic HDR** and the launcher switches Windows HDR on while you play and back afterwards. A **Colour** option stretches the colours onto wide-gamut (P3) screens, if you like it vivid.

![Remastered at 21:9 ultrawide: a temple skyline across the full width, the HUD at the screen's edges](docs/images/shot-ultrawide.jpg)
*21:9 ultrawide: the whole temple skyline, more of the world at the sides, nothing stretched.*

![Dawn among tropical islands, with a one-line radio subtitle on screen](docs/images/shot-islands.jpg)
*Dawn in the islands, with the game's own subtitles drawn sharp at 4K.*

![Twilight: a temple under a rising moon, seen from the cockpit](docs/images/shot-temple.jpg)
*Moonrise over the temple.*

**Runs**
- Up to 290 fps at 3440×1440 and 229 at 4K, with the game still ticking 30 times a second: the same speed, far smoother.
- Four-player split screen at full detail: 113 fps.
- Every frame shown: 281 a second in Battle Mode on a 360 Hz variable-refresh screen, none dropped.
- Fresher frames: each frame is started just in time for the graphics card instead of waiting in a queue, so a press reaches the frame presented in 42 ms (47 in 0.3.0) and the graphics card finishes each frame about 11 ms sooner. It costs some frame rate, depending on the scene: in mission 1 on our test PC, about 5% fewer frames firing from a standstill (193 against 203 a second) and about 20% fewer steering through the course (172 against 220). The cost comes from the mode leaving the graphics card idle between frames: with it off, 0.3.1 draws as fast as 0.3.0. For the most frames instead of the freshest, set `BW_HIFPS_LATMODE=0` on the launcher's Advanced tab. The frame rates above were measured before this mode.

<img src="docs/images/fps.svg" width="600" alt="Frames per second, Remastered defaults at 3440×1440: Battle Mode 290, story mission 27 262, story mission 1 at 4K 229, 2-player split screen 214, story mission 1 205, story mission 3 175, 4-player split screen 113. The original runs at 30.">

![Battle Mode: rockets arc over limestone peaks](docs/images/shot-rockets.jpg)
*Rockets over the Tombstones: Battle Mode in widescreen.*

**Sounds**
- Cleaner pitch changes and the game's own surround mix on 5.1 and 7.1, at the game's own level: no boost, nothing squashed or ducked.
- Low latency: a shot reaches your ears 91 ms after you press fire (126 in 0.2.5), through Windows' own audio engine, so Discord and other apps keep their sound.
- Sound that survives: unplug the headphones, switch the output or let the monitor's HDMI audio drop out, and the sound comes back by itself.

**Moves like the Xbox at any frame rate**
- Spray, trails, bolts and effect scripts advance once per game step, bit for bit as the original does, and the reticle and markers glide between steps.

**Fixes**
- 18 bugs from 2001 fixed, each switchable: a mission that could never be won, sounds and radio calls that never played, AI rules that never matched, two textures that never shipped, a dark line on the distant night sea, and more.

![The final storm: lightning, rain, green energy arcs and an enemy ship lit by an explosion, with a radio subtitle](docs/images/shot-lightning.jpg)
*The final storm: lightning, rain and the green arcs of the legendary shield.*

Every setting: [docs/SETTINGS.md](docs/SETTINGS.md). Both editions in full: [docs/EDITIONS.md](docs/EDITIONS.md).

## Getting started
### What you need
- **Windows 10 or 11, 64-bit**, and a graphics card with DirectX 11.
- **Your own Blood Wake disc**, copied to your PC as a folder of its files, an Xbox ISO (XISO) or a full disc image. Setup recognises the North American retail release in [`fingerprints/retail.json`](fingerprints/retail.json). Another version, or a modified copy, can be built too, after a plain warning you have to accept.
- **A few GB of free space** for the build (the game's files alone are about 2 GB).
- An Xbox-style controller (XInput), or a keyboard and mouse: the keyboard works as controller 1 out of the box, and in Remastered the mouse works in every menu.

The build also uses free tools, which the installer fetches if they're missing, from their official sites only: Python 3.13 with the packages pinned in [`tools/requirements.txt`](tools/requirements.txt), Visual Studio 2022 Build Tools (version 17, C++ workload; 2026 isn't supported yet), CMake 3.20 or later, and the xboxrecomp toolkit at a pinned commit, checked against a pinned hash.

### Install
> **Windows may warn you:** the installer isn't code-signed, so SmartScreen can say "Windows protected your PC". Choose **More info**, then **Run anyway**. Each release lists the installer's SHA-256, so you can check you have the real file.

1. Download `BloodWakeRecomp-Setup-<version>.exe` from [Releases](https://github.com/minimalice92/bloodwake-recomp/releases) and run it. It installs for your Windows user only; only the step that installs Microsoft's build tools, if needed, asks for administrator rights.
2. Accept the disclaimer, confirming you own the game and copied it from your own disc.
3. Choose your copy of the disc. Setup tells you in plain words whether it recognises it. If it doesn't (another region, a modified copy or a damaged one), it explains what that means before you decide to go on.
4. Pick the install folder (by default `%LOCALAPPDATA%\Programs\Blood Wake Recomp`) and the shortcuts.
5. Let it build: about 15 minutes, with a progress bar, and you can cancel at any point. Its last step, "Preparing graphics", plays a few short scenes hidden and silent (about 2 minutes) so your first game doesn't stutter while its shaders compile.
6. Play from the Start menu or the desktop shortcut.

**Updating:** run the new installer over your current version, into the same folder. Your saves, settings and prepared graphics are kept.

![The installer's shortcuts page: a Start menu entry and a desktop shortcut, each with its own box](docs/images/ui-installer-shortcuts.jpg)
*The installer: a Start menu entry and a desktop shortcut; untick either.*

To uninstall, use Windows' **Apps** settings: it removes the build and its shortcuts, and keeps your saves and settings unless you choose otherwise.

**Or build by hand:**
```
git clone https://github.com/minimalice92/bloodwake-recomp.git
cd bloodwake-recomp
python -m pip install -r tools/requirements.txt
python tools/build_from_disc.py --disc "<your disc folder or image>" --install-dir "<where to install>"
```
Then start the launcher from the install folder. Every option, step and exit code: [docs/BUILDING.md](docs/BUILDING.md).

### Playing
![The launcher's Graphics tab: anti-aliasing, texture filtering, detail distance and the other picture settings, with a Play button](docs/images/ui-launcher-graphics.jpg)
*The launcher: pick an edition and your settings, then press Play.*

- The launcher has a tab each for Edition, Display, Graphics, Audio, Fixes, Controls, Cheats and Advanced. Hover over any setting for a short explanation.
- **Controls:** remap any key, or a mouse button, with a click and a press; set the keyboard's steering and the sticks' deadzones.
- **Cheats:** tick the ones you want under one switch.
- **If something goes wrong:** after two failed starts the launcher offers safe mode, and **Make a bug report** (Advanced tab) puts one zip on your desktop with the log and your settings, nothing from the game.

**Presets (Remastered).** The Edition tab has three presets that set the Display and Graphics tabs in one go. Best Quality is the default. You can still change any setting afterwards; the preset then shows Custom. Automatic HDR, Colour and OLED care are never changed by a preset.
- **Best Quality:** a modern game on max settings: every upgrade and extra on (ambient occlusion, bloom and gentle sharpening included), tuned so nothing overlaps.
- **Balanced and Original:** the original game true to form, looking its best on a modern screen, with none of the extras that change the look.
- **Best Performance:** for a slower PC: the cheapest settings that keep the essentials (widescreen, a high frame rate, light anti-aliasing), so it still feels like a modern game.
- The game opens borderless full screen; Alt+Enter or F11 switches to a window and back while you play, and Window stays a choice on the Display tab. In Remastered, the main menu's last entry reads Quit Game.
- Each edition keeps its own saves, under `%LOCALAPPDATA%\xboxrecomp\` (Remastered's in `editions\remastered`). The first time you play Remastered, it starts with a copy of your Original saves. Settings live in `%LOCALAPPDATA%\BloodWake\settings.ini`.

### Troubleshooting
- **"not a supported unmodified retail copy" (exit code 2):** the copy isn't the release in `fingerprints/retail.json`. A patched, trimmed or re-packed copy won't match: copy your disc again. To build it anyway, run Setup and accept its warning (or add `--allow-unverified` to `build_from_disc.py`); the build is then marked as an unverified copy.
- **A missing dependency (exit code 3):** the message names it. Install it, or run the installer, which fetches it.
- **The translation failed (exit code 4):** check the Python packages are exactly the pinned versions (`python -m pip install -r tools/requirements.txt`).
- **"generated code doesn't match" (exit code 5):** usually a different version of a Python package or of the toolkit. Reinstall the pinned versions and try again.
- **The build failed (exit code 6):** make sure Visual Studio 2022 (17.x) Build Tools with the C++ workload is installed.
- **Disk space or a file error (exit code 7):** free some space, or pick an install folder you can write to.
- **The game won't start after installing (Windows says it blocked an app):** Smart App Control is on. It blocks programs that aren't code-signed, and the game is built on your own PC, so it can't be. Turn it off in Windows Security > App & browser control > Smart App Control settings. It's a setting for the whole PC, so it's your call; Setup warns about it before it builds.
- **The game is slow:** in the launcher, pick the Best Performance preset, or lower the rendering resolution or the frame rate cap.
- **Crackling sound on a slow PC:** Audio tab, Sound processor: Simple (it skips the sound chip's echo and 3D effects).

Still stuck? Open an issue: say what you did, what you saw, and your Windows version and graphics card. Please don't attach game files, screenshots of the game's text, or your disc image.

## How it was made
### How it works
The Xbox runs a PC-style x86 processor, so its program can be translated instead of emulated. The [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) toolkit turns every function of the game into C, and Microsoft's compiler builds that into a Windows program. The toolkit's runtime stands in for the Xbox's system, memory, sound chip and controllers, and this port's Direct3D 11 back end turns the game's own graphics commands into work for your graphics card.

### Made with AI
The engineering was done by Claude, Anthropic's AI model: a lead session plus **80+ AI agents** working in batches (naming, file formats, stability, fidelity checks, the remaster, the installer). Each batch had a written brief and a written report, and the lead reviewed and re-verified it before merging. The maintainer set the direction, made the calls and played the game.

- **25 Oct 2026:** the plan in the morning; by night the whole program translated, booting to the menus and playing Battle Mode.
- **26 Oct:** sound, GPU graphics at full speed, every mission booting, split screen, 948 soak runs without a crash.
- **27 Oct:** all 8,110 functions named, and Remastered built.
- **Early 28 Oct:** the installer, this repository and release v0.1.0.
- **3 Nov:** 0.2.5, the stable base: a one-day run of ten lanes and more than 100 items, each checked by a release gate. Eight release candidates were tested and the bugs found on the way were fixed, every judgement call logged with a one-line undo.
- **5 Nov:** 0.3.0, the sound release: the sound chip's programs as native code, a new sound output, and the high frame rate made exact, through six release candidates and the same gate.
- **6 Nov:** 0.3.1, native graphics: the last of the graphics chip's work moved to the graphics card in both editions, the loading screen back, fresher frames in Remastered and the sound at the game's own level, through the same gate.

### Proof, not promises
- **Original stays exactly as shipped.** Every merge is replayed against the previous build with the same random seed and compared step by step. A build made the public way, from the disc and without the private names file, matches the workshop build for 3,867 of 3,867 steps in each edition.
- **The translation is deterministic.** Six runs gave six identical sets of generated C, and every player's build is checked against the published fingerprints of it.
- **The sound chip, translated word for word.** Its effects programs run as native code made as the game starts. 40 recordings of real play (835,360 runs) replay identically through both, and three hours of Original ran the old and the new side by side: 15.4 million frames, 0 differences.
- **The graphics, native and measured.** 26 recorded scenes replay pixel for pixel through the processor's old path, and with the native pieces on they differ from it on 22-25 pixels in a million, almost all by one shade of 256, each counted per scene and region.
- **The water, rewritten bit for bit.** The functions that build the sea's surface each frame are re-translated at build time into plain native code doing the same operations in the same order. 2,627,309 batches compared with the game's own code: 0 differences. Four-player split screen went from 66 to 113 fps.
- **Stable.** 0 crashes in 948 mission runs of soak tests; a later soak of every mission, both battles and Blood Ball: 0 crashes, 0 freezes. For 0.2.5: 3 hours of play per edition, both story campaigns to the end, 219 starts in a row, and sound kept through 68 forced audio-device failures, with no crash or freeze. For 0.3.0: 3 hours of play per edition at once, both story campaigns to the end, battles and split screen, 403 starts: 0 crashes, 0 freezes, 0 sound gaps. For 0.3.1, the same 3 hours: 412 starts, 0 crashes, 0 freezes, 0 sound gaps.
- **Played through.** An autopilot won 24 of the 28 story missions, the final one included, with no port problem in about 37 attempts. The other four need skills the autopilot doesn't have yet.
- **69 toolkit issues found and fixed here.** From 8- and 16-bit multiplies translated as 32-bit ones, to point sprites, fog and polygon offset never drawn, to a 60.00 Hz vertical blank where NTSC runs at 59.94. The fixes are carried in this project (`src/toolkit_patches/`, `tools/xbr_patches.py`), with the toolkit itself untouched at its pinned version. They will be offered to xboxrecomp with neutral repros containing nothing from any game; none has been submitted yet.
- **Every function named: 8,110.** Each name proposed with evidence from the code, and 6,149 re-checked independently (6,109 confirmed as they stood). Ten functions that deal with the console's security were left alone on purpose.
- **The file formats, decoded.** Models, boats and weapons, missions, effects, fonts, menus, sound and textures: specced, checked against every archive on the disc, then verified blind.
- **Bugs explained before they're fixed.** Each of the 18 fixes was traced in the code first. The hardest and strangest: a faint dark line on the distant sea at night, where folded waves got the sky reflection blended twice. Remastered now draws it once; Original keeps it, as the console very likely showed it.
- **Data, not guesswork.** The project works from captured data (logs, traces, frame dumps, step-by-step comparisons), never from memory or feel, and labels every finding seen, inferred or unknown.

## The Roadmap
### The Expanded Edition<!-- audit:allow expanded -->
Expanded is the planned third edition: a tribute to the developers that finishes what they started. It builds on what turned up when every archive was decoded and every function named. **None of it is in this repository or in any release yet.** <!-- audit:allow expanded -->

The plan, best value for the effort first (lift in agent time: S hours, M about a day, L days, XL weeks):

1. **Co-op battles** (S–M). *Found:* six two-player co-op battles were cut; their names, descriptions and team rules are on the disc, and the game's mode list is plain data. *Plan:* put the six back in Battle Mode with their teams and AI opponents. <!-- audit:allow expanded -->
2. **Co-op Story Mode** (M). *Found:* the launch data already holds up to four players, and split screen follows the player count. *Plan:* player 2 joins at boat select, two HUDs, respawns near your teammate, and a mission is lost only when both of you are down. Three or four players later. <!-- audit:allow expanded -->
3. **Co-op survival** (S–M). *Found:* a two-player survival game ends the moment player 1 sinks. *Plan:* an everyone's-down rule, team results, and two-player versions of the six survival modes. <!-- audit:allow expanded -->
4. **Blood Ball for everyone** (S–M). *Found:* the mode the team called their favourite sits behind a cheat. *Plan:* a normal menu entry plus team Blood Ball, two against two or both of you against the AI.
5. **Every boat in Battle Mode** (S–M). *Found:* two story-only hulls, the named story boats (they already play when a battle names them), a dozen enemy-only classes and one class used nowhere. *Plan:* boat-select slots, a HUD icon where one is missing, and a balance check for each.
6. **A developers' corner** (S). *Found:* a hidden level select that no code ever switches on, and debug switches nothing sets: free cameras, a mission clock on the HUD, switching boats. *Plan:* the harmless ones in a launcher tab, off by default.
7. **Secret objectives and earned unlocks** (S–M). *Found:* a cut secret-objective type, one mission still carrying one, and unused messages for unlocking a joke boat that today is only a cheat. *Plan:* finish the system, so the cheat boats can be earned.
8. **A leftovers gallery** (S–M). *Found:* an earlier front end that is never loaded (98 images found nowhere else), five unused font sets, early diary drafts, an epilogue page nothing opens, the artists' test effects, and a developer kit with a teapot in it. *Plan:* a gallery, a viewer arena for the effects, and the teapot hidden for you to find.
9. **Living AI** (M–L). *Found:* three of the AI's five attitudes have empty tables, so only two ever chase, ram or grab power-ups. *Plan:* fill them in for more varied opponents.
10. **Armour that counts** (M). *Found:* a cut armour system; only a test boat has armour values. *Plan:* finish it, with a balance pass.
11. **Unused sounds and effects** (M). *Found:* 51 effects nothing can start, plus unused collision and scrape sounds. *Plan:* wire each to the event it was named for, after a careful listen.
12. **Custom missions and boats** (L). *Found:* the missions are pure data in the decoded formats, built in the designers' own mission editor, and a capture objective that can never succeed. *Plan:* load your own missions and boats, with the capture objective finished.
13. **Squad tactics** (L). *Found:* a squad-matchup evaluator for the AI that is compiled in but never switched on. *Plan:* switch it on and tune it.
14. **Online, starting with a network Blood Ball** (XL). *Found:* the sequel's design called for networked multiplayer, and some of the developers once planned a network Blood Ball. *Plan:* delay-based lockstep, which a research pass for this project judged feasible. <!-- audit:allow expanded -->

Some things can't come back faithfully: a 29th mission (the E3 2001 show demo) whose list entry ships but whose mission file doesn't, 41 voice lines that were written but never recorded, and a second weather set for one mission whose files are missing.

## Staying safe
Fan ports live in a grey area. This one is built to be clean, to sit at the very safest end of it, and to be able to show it.

- **You must own the game.** The build checks your copy against published SHA-256 fingerprints before it does anything else. It recognises an unmodified retail copy made from your own disc. Any other copy is built only after a plain warning you have to accept (a copy from an unknown source could be tampered with), and it stays marked as unverified.
- **You build it yourself, on your own PC.** Your PC translates and compiles the game with Microsoft's own build tools; no game code or program is ever downloaded.
- **Nothing from the game is in this repository.** No game code, generated C or executables, and no art, sound, music, video, text or extracted files. The one exception is the handful of game screenshots on this page, each listed by its fingerprint.
- **Fingerprints, never code.** [`fingerprints/`](fingerprints/) holds SHA-256 hashes of the retail files and of the generated C, so your build proves it matches ours without the code ever being shared.
- **A fail-closed audit on every publish.** Every change arrives through a scripted sync that scans each file against the game itself (its program, every file on the disc, its text) and for generated C, personal data, outside links and anything to do with console security. One finding, or one input it can't read, and nothing is published. Each run plants samples of game bytes, text and code and must catch them all, and images pass only at their listed fingerprint, with no metadata.
- **The names stay private.** The file of 8,110 function names is kept out; the public build works without it and plays step for step the same.
- **A commit guard for every clone.** [`tools/git-hooks/pre-commit`](tools/git-hooks/pre-commit), switched on with one git command (see [CONTRIBUTING.md](CONTRIBUTING.md)), refuses game files, disc images, archives, executables and generated C.
- **Hands off the console's security.** No DRM, copy protection, licence checks, disc authentication, signing keys, save signatures or Xbox Live: none of it is touched, and none of it is needed. The toolkit reads the game's program directly and only provides the system calls the game makes, and the port writes its own saves. <!-- audit:allow off-limits -->
- **No help getting the game any other way.** No links to game images, and no help with ripping a disc or getting around any protection. <!-- audit:allow off-limits -->
- **Free, for good.** No sales, donations, ads or paid features.
- **Credit where it's due.** Blood Wake was made by Stormfront Studios and published by Microsoft Game Studios ([CREDITS.md](CREDITS.md)). If Microsoft ever re-releases it, this page will point there first. Buy it.

The full disclaimer is in [LEGAL.md](LEGAL.md); please read it before you build or play.

### Contact and takedown
The maintainer is [@minimalice92](https://github.com/minimalice92) on GitHub. Questions and bug reports: open an issue.

**Rights holders:** if you are Microsoft or act for the rights holders and want this project taken down, open an issue titled **Takedown**, or contact the maintainer through GitHub. It will be taken down at once, no questions asked.

## Credits and licence
Blood Wake is by **Stormfront Studios**, published by **Microsoft Game Studios**; all of its code, art, sound, music and writing is theirs. This port stands on [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) by sp00nz and on [xemu](https://xemu.app)'s work on the Xbox's sound chip. Full credits: [CREDITS.md](CREDITS.md).

Our own code is under the **MIT** licence ([LICENSE](LICENSE)); some parts are under the GPL or LGPL ([NOTICE.md](NOTICE.md), [LICENSES/](LICENSES/)). Please read [CONTRIBUTING.md](CONTRIBUTING.md) before you open an issue.
