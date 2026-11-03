# Legal notice and disclaimer

**Please read this before you build or play.**

## What this is
Blood Wake Recomp is an unofficial, non-commercial fan project for preservation and interoperability. It lets a person who owns Blood Wake (Xbox, 2001) run their own copy natively on a Windows PC.

It is **not affiliated with, endorsed by, sponsored by or connected to** Microsoft Corporation, Xbox Game Studios (formerly Microsoft Game Studios) or Stormfront Studios. "Blood Wake", "Xbox" and "Microsoft" are trademarks of their owners, used here only to say what this project works with.

## What this repository contains, and what it doesn't
- It contains **only this project's own work**: source code, build scripts, the launcher, the installer and documentation, plus third-party open-source parts under their own licences (see `NOTICE.md`).
- It contains **nothing from Blood Wake**: no game code, no code generated from it, no executables built from it, no art, models, textures, sound, music, video, text, saves or extracted files. The one exception is a few screenshots shown in the README (at most eight, taken by the project from its own build, each listed by its SHA-256 fingerprint).
- `fingerprints/` holds SHA-256 hashes only. A hash identifies a file without revealing anything about what's in it.

## Your responsibilities
- **You must own a legitimate retail copy of Blood Wake**, and make the copy you build from yourself, from your own disc. The build checks your copy against the published fingerprints. A copy it doesn't recognise (another version, or a modified or damaged copy) is built only after you accept a warning, and is marked as unverified: a copy from an unknown source could be tampered with, and Setup turns its program into one that runs on your PC.
- Use this only for yourself. **Don't share** the files your build produces: the unpacked game, the generated C, or the executable. They contain or are made from the game, which belongs to its rights holders.
- Follow the law where you live. Whether making a personal copy of a game you own is allowed differs from country to country; that's yours to check.

## What this project will never do
- It doesn't circumvent DRM, copy protection, licence checks or console security, and needs none of that: the toolkit reads the game's program directly and only provides the system calls the game makes. <!-- audit:allow off-limits -->
- It doesn't touch the Xbox's console security, signing keys, save signatures, disc authentication or online services, and it doesn't load or re-sign console saves. The port writes its own saves. <!-- audit:allow off-limits -->
- It won't link to game images or ROM sites, and won't help anyone rip discs or get around any protection. Requests for game files are closed without reply. <!-- audit:allow off-limits -->
- It is free: no sales, donations, ads or paid features.

## Takedown
If you are Microsoft or act for the rights holders and want this project taken down, open an issue titled **Takedown** on this repository, or contact the maintainer ([@minimalice92](https://github.com/minimalice92)) through GitHub. The repository will be taken down at once.

If Blood Wake is ever re-released by Microsoft, this project will point to the official release first.

## No warranty
This software is provided as is, without any warranty, as the licences in `LICENSE` and `LICENSES/` say. Building and running it is at your own risk.
