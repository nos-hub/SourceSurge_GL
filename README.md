# SourceSurge_GL

**SourceSurge** is a free, open-source **GoldSrc** movement mod built on the Half-Life 1 SDK ([halflife-updated](https://github.com/twhl-community/halflife-updated)) and designed to run on a custom **[Xash3D FWGS](https://github.com/FWGS/xash3d-fwgs)**.

Fast movement, style scoring, wave survival, and custom weapons — still in active development. Builds may be unstable.

> **Not affiliated with Valve.**  

---

## Features (current)

- Custom movement tech (bhop, surf-related fixes, edgebug assist, jump stats, style points)
- Melee with charge-based knockback (CS knife)
- Grappling hook
- Sniper (AWP-style) with scope overlay and pierce shots
- Wave mode
- Custom HUD modules (speedometer, style/points, charges, edgebug popup, jump stats, HUD editor)
- Discord Rich Presence (optional)

More systems are planned as the foundation stabilizes.

---

## Running the mod
**needs windows - not linux compatible**

1. download the latest release from the [Discord](https://discord.gg/PUsJJPeJQH)
2. extract the zip file named "xash3d-fwgs-win32-i386"
3. launch "Source Surge GL.exe" (note : if you want the console open the game with the .bat file server you can find in the discord)
if any issues ask in the discord directly.

---

## Building

See [BUILDING.md](BUILDING.md).

Typical Windows flow:

1. Open `projects/vs2019/projects.sln` in **Visual Studio 2019+** (v142 toolset).
2. Build **hldll** (server → `hl.dll`) and **hl_cdll** (client → `client.dll`), **Release | Win32**.
3. Copy DLLs into your mod’s `dlls/` and `cl_dlls/` folders (post-build events may already target your local path).

---

## Credits & licenses

### Valve
- **Half-Life 1** and the **Half-Life 1 SDK** © Valve Corporation.
- SDK use is subject to the [Half-Life 1 SDK License](LICENSE) included in this repository.
- You may develop and distribute a **free** modified game using the SDK; commercial use requires contacting Valve.
- **Half-Life game assets are not included** and must come from a legitimate copy you own.

### Xash3D FWGS
- Engine: [FWGS / xash3d-fwgs](https://github.com/FWGS/xash3d-fwgs)  
- Licensed under the terms of that project (primarily GPL). If you distribute a modified engine binary, comply with its license (including source offer where required).

### Half-Life Updated
- Base SDK improvements: [twhl-community/halflife-updated](https://github.com/twhl-community/halflife-updated) and contributors.

### SourceSurge
- Mod design, movement systems, HUD, weapons, and gameplay code: **nos** / SourceSurge contributors.
- Community: [Discord](https://discord.gg/PUsJJPeJQH)

### Third-party content
- Some models/sounds (e.g. AWP, knife) may come from community packs.

---

## Legal summary

- **Free and open source** mod for non-commercial use under the HL1 SDK terms + engine licenses.
- **Do not** present this as an official Valve or Steam product.

---

## Status

Active development. Expect crashes, incomplete systems, and frequent changes.

---

## Links

- Discord: https://discord.gg/PUsJJPeJQH  
- Xash3D FWGS: https://github.com/FWGS/xash3d-fwgs  
- Half-Life Updated: https://github.com/twhl-community/halflife-updated  
