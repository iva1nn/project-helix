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

The installed games copy `SDL2.dll` (zlib) next to the exe and `ffmpeg.exe` (GPL-3.0, run as a separate program)
into `data\`. Their sources are at the download sites above.
