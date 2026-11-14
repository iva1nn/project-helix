# Third-party components

## In this repository

| Component | Files | Licence |
|---|---|---|
| stb_image, stb_truetype (Sean Barrett) | `src/runtime/third_party/stb_*.h` | public domain / MIT (either), see each file's end |
| xxHash (Yann Collet) | `src/runtime/third_party/xxhash.h` | BSD 2-Clause, see the file's header |
| Anton, Barlow, Barlow Condensed, Oswald fonts | `src/fonts/*.ttf` | SIL Open Font License 1.1, `src/fonts/OFL.txt` |
| Special Elite font (Astigmatic) | `src/fonts/SpecialElite-Regular.ttf` | Apache License 2.0, https://www.apache.org/licenses/LICENSE-2.0 |

## Downloaded by the installer (not redistributed here)

| Component | Licence |
|---|---|
| Python 3.11.9 embeddable package | Python Software Foundation License |
| w64devkit 2.11.0 (GCC, binutils, mingw-w64) | w64devkit: public domain; GCC / binutils: GPL-3.0 with the GCC runtime exception; mingw-w64: ZPL / MIT and others (its `COPYING*` files) |
| SDL2 2.30.9 | zlib License |
| FFmpeg 9.0.1 essentials build (gyan.dev) | GPL-3.0, `.toolchain\ffmpeg\...\LICENSE` |
| Zig 0.16.0 (SteamOS / Linux only) | MIT; its bundled libc headers under their own licences |
| SDL2 2.30.9 source, headers only (SteamOS / Linux only) | zlib License |

On SteamOS / Linux the game uses the system's own SDL2, OpenGL (Mesa) and ffmpeg.

## Steam library artwork (SteamOS / Linux, downloaded by the installer)

Community artwork from [SteamGridDB](https://www.steamgriddb.com), downloaded from its servers at install time (not
part of this repository), with thanks to the artists:

| Game | Cover (portrait) | Wide cover | Hero banner | Logo |
|---|---|---|---|---|
| Fear Effect | Obdulio ([57962](https://www.steamgriddb.com/grid/57962)) | [692721](https://www.steamgriddb.com/grid/692721) | RealSayakaMaizono ([5764](https://www.steamgriddb.com/hero/5764)) | (in the banner) |
| Fear Effect 2: Retro Helix | Obdulio ([57971](https://www.steamgriddb.com/grid/57971)) | Wolfy ([27505](https://www.steamgriddb.com/grid/27505)) | RealSayakaMaizono ([5765](https://www.steamgriddb.com/hero/5765)) | RealSayakaMaizono ([6939](https://www.steamgriddb.com/logo/6939)) |

The installed games copy `SDL2.dll` (zlib) next to the exe and `ffmpeg.exe` (GPL-3.0, run as a separate program)
into `data\`. Their sources are at the download sites above.
