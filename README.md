# Fear Effect — native PC port

**Fear Effect** and **Fear Effect 2: Retro Helix**, running natively on Windows with no emulator, remastered.

🎬 **See the remaster in action: [feremaster.us](https://feremaster.us/)**

- 8x remastered backgrounds and cutscenes
- Remodelled characters, redrawn interface and fonts
- Widescreen fill, directional (camera-relative) controls, gamepad support
- Save states, faster loading, automatic disc changes

> This repository contains **no game code and no game data**. The installer builds the game on your PC from
> **your own discs**. The remastered assets are a separate download.

---

## What you need

- **Windows 10 or 11** (64-bit) and a graphics card with OpenGL 3.3
- **Your own disc images:** all four discs of the game, USA version (see [Supported discs](#supported-discs))
- **The asset pack** for your game (see [Asset packs](#asset-packs))
- **Free disk space:** about 21 GB for Fear Effect, 33 GB for Fear Effect 2, plus room for the archive while you
  extract it
- **Internet once,** for the installer to download its build tools (about 200 MB)

## Install

**1. Download this repository.** Use *Code → Download ZIP* and extract it, or `git clone` it. Use a short path
with plain letters, such as `C:\Games\FearEffect`.

**2. Add your discs and the asset pack** so the folder looks like this:

```
C:\Games\FearEffect\
├── install.bat         ← you run this in step 3
├── discs\
│   ├── fe1\            ← your Fear Effect disc images (all 4)
│   └── fe2\            ← your Fear Effect 2 disc images (all 4)
├── data\               ← the asset pack ends up here (data\fe1\, data\fe2\)
└── src\                (the source code: nothing to do here)
```

- **Discs:** file names don't matter, because the installer recognises each disc by its content.
- **Asset pack:** extract the archive, for example with [7-Zip](https://www.7-zip.org/), into the
  `C:\Games\FearEffect` folder itself. The archive already contains the `data` folder. You can delete the archive
  afterwards.

**3. Double-click `install.bat`.** It checks your discs and the assets, downloads the build tools, builds the game
from your discs, tests it and creates shortcuts. The first run takes a few minutes.

**4. Play** from the *Fear Effect* or *Fear Effect 2 - Retro Helix* shortcut on your Desktop or in the Start menu.

> If Windows shows *"Windows protected your PC"* when you start `install.bat`, click **More info → Run anyway**.

---

## Supported discs

| Game | Version | Disc serials |
|---|---|---|
| Fear Effect | USA | SLUS-00920, SLUS-01056, SLUS-01057, SLUS-01058 |
| Fear Effect 2: Retro Helix | USA **v1.1** | SLUS-01266, SLUS-01275, SLUS-01276, SLUS-01277 |

- **Works:** BIN/CUE, or raw `.bin` / `.img` files (2352-byte sectors), as made by redump-style dumping tools.
- **Doesn't work:** a 2048-byte `.iso`, because it lacks the audio and movie data. Convert `.chd`, `.pbp` and
  `.ecm` images to BIN/CUE first.
- **Not supported:** other regions and Fear Effect 2 v1.0. The installer tells you if it finds one.

## Asset packs

| Game | Download | Size | SHA-256 |
|---|---|---:|---|
| Fear Effect | [`fe1-assets-2026-11-13-f3c5ece.7z`](https://drive.google.com/file/d/1k_6z9l4Rae3eXTpGvgdYS8Tf48yebKoV/view?usp=sharing) | 15.2 GB | `BB56C00A78BC83F135424E9DD62437E93B94F784C36155CC6164A8C01DCF0E01` |
| Fear Effect 2: Retro Helix | [`fe2-assets-2026-11-13-f3c5ece.7z`](https://drive.google.com/file/d/1zA3IIyikUYEVd7MYbpphT8mURIhHn021/view?usp=sharing) | 22.6 GB | `644C98DC35F86AB8CBC75B703B1E1211BFAFA808618E6F21E4BE62BFE4D441FB` |

Google Drive warns that it can't scan files this large for viruses: choose **Download anyway**.

You don't need to check these by hand: `install.bat` checks every pack against the published one, file by file.
It also checks an archive you haven't extracted yet. To check a download yourself, run this in PowerShell:
`Get-FileHash .\fe2-assets-….7z`

---

## Playing

The installed games are in the `Fear Effect\` and `Fear Effect 2\` folders. Your saves, save states and settings
live there too.

| Key | Gamepad | Action |
|---|---|---|
| F12 | Select / touchpad | Remaster on / off |
| F10 | R3 | Screen layout (fill, HUD over the picture, original 4:3) |
| F7 | L3 | Directional / original tank controls |
| F11 or Alt+Enter | | Fullscreen / window |
| F5 / F8 | | Save / load state (Page Up / Page Down picks the slot) |

Inside the game, *Options → Controls* opens the controls menu, where every key and button can be changed. Every
other setting is described in `SETTINGS.txt` in each game folder.

## Updating

1. **Update this repository:** `git pull`, or download the ZIP again and extract it over this folder.
2. **If the update has a new asset pack:**
   1. Delete the old one: the `data\fe1` or `data\fe2` folder and its file `data\manifests\fe1.json` or
      `fe2.json`.
   2. Extract the new pack as in [Install](#install).
3. **Run `install.bat`.** It rebuilds only what changed, usually in under a minute.

Your saves, save states, settings and disc images are kept. If you skip a step, `install.bat` tells you which one.

## If something goes wrong

When there's a problem, `install.bat` stops with **INSTALL STOPPED:** and says what to do. Fix it, then run it
again. Finished steps are kept, and an interrupted download continues where it stopped. The full record is in
`install.log`.

| The installer says | What to do |
|---|---|
| a download failed / no internet | Check the connection and run it again. On a PC without internet: download the listed files on another PC, put them into `.toolchain\downloads\`, then run it again. |
| the image is incomplete / a different version | Copy or dump the disc again. Only the [supported versions](#supported-discs) work. |
| the asset pack is incomplete | Extract the whole archive again and let it overwrite. |
| older than this installer needs | Your asset pack is out of date: follow [Updating](#updating), step 2. |
| newer than this copy of the repository | Your repository is out of date: follow [Updating](#updating), step 1. |
| differs from the published pack / not the published file | Download the pack again from its link under [Asset packs](#asset-packs). |
| an antivirus is blocking something | Allow this folder in your antivirus, then run it again. |
| the game is probably running | Close the game, then run it again. |

<details>
<summary><b>Installer options</b></summary>

```
install.bat --game fe2       install only this game (fe1 or fe2)
install.bat --verify         also check every asset file's checksum (reads all of the asset files)
install.bat --rebuild        rebuild the game from scratch
install.bat --no-shortcuts   don't create Desktop / Start menu shortcuts
```

</details>

<details>
<summary><b>What the installer downloads</b></summary>

Exact versions, checked by SHA-256, kept in `.toolchain\`:

| Tool | Why | From |
|---|---|---|
| Python 3.11.9 (embeddable) | runs the installer and the recompiler | python.org |
| w64devkit 2.11.0 (GCC) | compiles the game from your discs | github.com/skeeto/w64devkit |
| SDL2 2.30.9 | window, sound, controllers | github.com/libsdl-org/SDL |
| FFmpeg 9.0.1 (essentials) | plays the remastered movies | github.com/GyanD/codexffmpeg |

</details>

---

## Licence

- **This project's code** (`src/`, the installer) is MIT licensed: see `LICENSE`.
- **Third-party parts and the downloaded tools:** see `THIRD_PARTY.md`.
- **The games:** Fear Effect and Fear Effect 2: Retro Helix belong to their rights holders. This project contains
  none of their code or data; you need your own discs.
