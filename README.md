# Fear Effect native port

Play **Fear Effect** and **Fear Effect 2: Retro Helix** (PlayStation, USA) natively on Windows, with no emulator.
You get 8x remastered backgrounds and cutscenes, remodelled characters, a redrawn interface, widescreen fill,
directional controls and save states.

This repository contains **no game code and no game data**. The installer recompiles the game on your PC from
**your own discs**. The remastered assets are a separate download.

## Install (six steps)

1. **Download this repository.** Use *Code → Download ZIP* and extract it, or `git clone` it, into a folder with a
   short, plain path, for example `C:\Games\FearEffect`. Avoid letters outside A–Z in the path.
2. **Download the remastered asset pack** for your game(s): `fe1` (Fear Effect) and/or `fe2` (Fear Effect 2).
   The files and their checksums are listed under [Asset packs](#asset-packs).
3. **Extract the asset pack into this folder,** for example with 7-Zip. The archive holds a `data` folder, so you
   end up with `C:\Games\FearEffect\data\fe2\...`. You can delete the archive afterwards.
4. **Copy your disc images into `discs\`.** Use `discs\fe1` for Fear Effect and `discs\fe2` for Fear Effect 2. You
   need all four discs. The file names don't matter: the installer recognises the discs by their content.
5. **Double-click `install.bat`.** It downloads the build tools once (about 200 MB), checks your discs and the
   assets, builds the game from your discs, tests it and creates shortcuts. The first run takes a few minutes.
6. **Play** from the *Fear Effect* / *Fear Effect 2 - Retro Helix* shortcut on the Desktop or in the Start menu.

### Supported discs

| Game | Version | Serials |
|---|---|---|
| Fear Effect | USA | SLUS-00920, SLUS-01056, SLUS-01057, SLUS-01058 |
| Fear Effect 2: Retro Helix | USA **v1.1** | SLUS-01266, SLUS-01275, SLUS-01276, SLUS-01277 |

Images must be **BIN/CUE** or raw `.bin` / `.img` (2352-byte sectors, as made by redump-style dumping tools).
A 2048-byte `.iso` won't work: it lacks the audio and movie sectors. Convert `.chd`, `.pbp` and `.ecm` to BIN/CUE
first. Other regions and FE2 v1.0 are not supported. The installer says so when it finds one.

## Asset packs

| Game | File | Size | SHA-256 |
|---|---|---:|---|
| Fear Effect | `fe1-assets-2026-11-13-f3c5ece.7z` | 15.2 GB | `BB56C00A78BC83F135424E9DD62437E93B94F784C36155CC6164A8C01DCF0E01` |
| Fear Effect 2: Retro Helix | `fe2-assets-2026-11-13-f3c5ece.7z` | 22.6 GB | `644C98DC35F86AB8CBC75B703B1E1211BFAFA808618E6F21E4BE62BFE4D441FB` |

You don't have to check these by hand: `install.bat` checks every pack it installs against the published one.
The repository holds the checksum of each pack's file list, and that list holds the checksum of every file in the
pack. A pack that was modified, re-uploaded with changes or extracted incompletely is reported before anything is
built. If the archive is still in this folder and not extracted yet, `install.bat` checks it against the checksum
above, which takes a minute. To check a download yourself, in PowerShell: `Get-FileHash .\fe2-assets-….7z`

## Updating

When a new version is announced:

1. **Update this repository:** `git pull`, or download the ZIP again and extract it over this folder, letting it
   overwrite files. Your discs, `data\` and the installed game folders are not in the ZIP, so they are kept.
2. **If the update comes with a new asset pack:** download it, then delete the game's old pack: the folder
   `data\fe1` or `data\fe2` and the file `data\manifests\fe1.json` or `data\manifests\fe2.json`. Extract the new pack
   into this folder. (Extracting over the old pack also works, but leaves unused old files behind.)
3. **Run `install.bat`.** It rebuilds whatever changed, usually in under a minute.

Your saves, save states, settings and disc images are never touched by an update. If you forget a step,
`install.bat` tells you which one: a pack older than the repository needs, or a repository older than the pack.

## After installing

- The installed games are in `Fear Effect\` and `Fear Effect 2\`. Your saves (`memcards\`), save states
  (`states\`) and settings (`fe1port.ini` / `fe2port.ini`) stay there. Re-running `install.bat`, for example
  after updating this repository, keeps them.
- Controls, keys and every option: `SETTINGS.txt` in each game folder. In the game, *Options → Controls* opens the
  port's controls menu. F12 toggles the remaster, F10 the screen layout, F11 fullscreen, F5 / F8 save and load a
  state.
- If you moved the folder, run `install.bat` again to fix the shortcuts.

## Installer options

```
install.bat --game fe2       only this game
install.bat --verify         also check every asset file's checksum (reads all ~45 GB)
install.bat --rebuild        recompile from scratch
install.bat --no-shortcuts   no Desktop / Start menu shortcuts
```

Everything the installer did is in `install.log`.

## If something goes wrong

The installer stops with **INSTALL STOPPED:** and says what to do. Fix it and run `install.bat` again: finished
steps (downloads, unpacked tools, builds) are kept, and an interrupted download continues where it stopped.

- **Windows protected your PC** (SmartScreen) when you start `install.bat`: choose *More info → Run anyway*.
  The file comes from the internet, so Windows asks once.
- **No internet on the gaming PC:** the message lists the files to download. Download them on another PC, put them
  into `.toolchain\downloads\`, then run `install.bat` again.
- **An antivirus removes or blocks the compiler or the game:** allow this folder in the antivirus, then run
  `install.bat` again.
- **"the image is incomplete" / "a different version":** dump or copy the disc again. Only the versions in the
  table above work.
- **"the asset pack is incomplete":** extract the whole archive again and let it overwrite.
- **"older than this installer needs" / "newer than this copy of the repository knows":** one half of an update
  is missing. See [Updating](#updating).
- **"differs from the published pack" / "is damaged or not the published file":** download the pack again from
  the official link.
- **The game is running:** close it before re-running the installer.

## What the installer downloads

Downloads are pinned to exact versions and checked by SHA-256. They are stored in `.toolchain\`:

| Tool | Why | From |
|---|---|---|
| Python 3.11.9 (embeddable) | runs the recompiler and the installer | python.org |
| w64devkit 2.11.0 (GCC) | compiles the recompiled game | github.com/skeeto/w64devkit |
| SDL2 2.30.9 | window, sound, controllers | github.com/libsdl-org/SDL |
| FFmpeg 9.0.1 (essentials) | decodes the remastered movies | github.com/GyanD/codexffmpeg |

## Licence

The port's own code (`src/`) is MIT licensed (`LICENSE`). Third-party parts and the downloaded tools:
`THIRD_PARTY.md`. Fear Effect and Fear Effect 2 are the property of their rights holders. This project ships none
of their code or data, and you need your own discs.
