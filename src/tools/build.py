"""Build the native port: recompile the exe from the user's disc (if needed) and compile everything.

    python tools/build.py --disc "<path to disc 1 .cue>" [--regen] [-O2] [--config config/fe1.json]

Output: build/fe2port.exe (+ SDL2.dll). Uses w64devkit gcc (F:/tools/w64devkit) and SDL2 (F:/tools/sdl2).
The exe's icon and version information: icons/<game>port.rc (icons/<game>.ico, tools/make_icons.py), compiled with
the same toolchain's windres and linked in.
"""
import argparse, os, shlex, shutil, subprocess, sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WINDOWS = os.name == "nt"
# Linux / SteamOS (the standalone install.sh): FE_GCC is a compiler command ("zig cc" works), FE_SDL2_INC a folder
# holding SDL2/SDL.h, FE_LIBS the libraries linked by path (SteamOS's own libSDL2-2.0.so.0, libGL.so.1); no icon
GCC = os.environ.get("FE_GCC", "F:/tools/w64devkit/bin/gcc.exe" if WINDOWS else "cc")
CC = shlex.split(GCC) if not WINDOWS else [GCC]
WINDRES = os.environ.get("FE_WINDRES", str(Path(GCC).with_name("windres.exe")))
SDL = Path(os.environ.get("FE_SDL2", "F:/tools/sdl2/SDL2-2.30.9/x86_64-w64-mingw32"))
SDL_INC = Path(os.environ["FE_SDL2_INC"]) if os.environ.get("FE_SDL2_INC") else SDL / "include"

ap = argparse.ArgumentParser()
ap.add_argument("--disc", required=True, action="append",
                help="Disc 1 .cue (discs 2-4 are found next to it, or repeat --disc for each)")
ap.add_argument("--config", default=str(ROOT / "config/fe2.json"))
ap.add_argument("--regen", action="store_true")
ap.add_argument("--opt", default="-O2")
ap.add_argument("--watch", action="store_true", help="debug build with RAM write watchpoints (build_watch/)")
ap.add_argument("--debug", action="store_true", help="gdb build with symbols (build_dbg/)")
args = ap.parse_args()

import json
game = json.loads(Path(args.config).read_text()).get("name", "fe2")
sfx = "" if game == "fe2" else "_" + game  # FE1: gen_fe1/, build_fe1/ ...
gen = ROOT / ("gen" + sfx)
build = ROOT / (("build_watch" if args.watch else "build_dbg" if args.debug else "build") + sfx)
obj = build / "obj"
obj.mkdir(parents=True, exist_ok=True)
# the font atlas table (hashes and palettes of the games' font files): generated, never committed
if not (ROOT / "runtime" / "font_table.h").exists():
    if subprocess.run([sys.executable, str(ROOT / "tools" / "font_table.py")]).returncode:
        # its inputs (the remasters' unpacked fonts) are gone: the copy tools/make_data.py keeps
        sys.path.insert(0, str(ROOT / "tools"))
        from datadir import default_data
        saved = default_data() / "build" / "font_table.h"
        if not saved.exists():
            sys.exit("runtime/font_table.h: tools/font_table.py failed and %s is missing" % saved)
        shutil.copy2(saved, ROOT / "runtime" / "font_table.h")
        print("font table from", saved)
def disc_set(first):
    """Disc 1 .cue -> list of the set's .cue files: '(Disc 1)' -> '(Disc N)' in the same folder or in
    a sibling folder named discN (F:/fe_port/discs/<game> holds all four in one folder)."""
    first = Path(first)
    out = [first]
    for n in range(2, 5):
        name = first.name.replace("(Disc 1)", "(Disc %d)" % n)
        if name == first.name:
            break
        for cand in (first.parent / name, first.parent.parent / ("disc%d" % n) / name):
            if cand.exists():
                out.append(cand)
                break
        else:
            break
    return out


discs = args.disc if len(args.disc) > 1 else [str(p) for p in disc_set(args.disc[0])]
if args.regen or not (gen / "files.txt").exists():
    cmd = [sys.executable, str(ROOT / "tools/recomp/recomp.py"), args.config, "--out", str(gen)]
    for d in discs:
        cmd += ["--disc", d]
    print("discs:", ", ".join(Path(d).name for d in discs))
    subprocess.check_call(cmd)

srcs = [gen / f for f in (gen / "files.txt").read_text().split()]
srcs += sorted((ROOT / "runtime").glob("*.c"))
cflags = ["-O1" if args.debug else args.opt, "-g" if args.debug else "-g0", "-fno-strict-aliasing", "-fwrapv", "-I", str(ROOT / "runtime"), "-I", str(gen),
          "-I", str(SDL_INC), "-Wall", "-Wno-unused-label", "-Wno-unused-variable", "-Wno-unused-but-set-variable",
          "-Wno-misleading-indentation", '-DFE_GAME="%s"' % game] + (["-DFE_WATCH"] if args.watch else []) + (
          [] if WINDOWS else ["-DFE_NO_FOPEN_WRAP"])
# the port build named in save states (runtime/state.c): the commit, + "+" when the working tree has changes
try:
    _rev = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "--short", "HEAD"], capture_output=True, text=True).stdout.strip()
    _dirty = subprocess.run(["git", "-C", str(ROOT), "status", "--porcelain", "--untracked-files=no"], capture_output=True,
                            text=True).stdout.strip()
    build_id = (_rev or "unknown") + ("+" if _dirty else "")
except OSError:
    build_id = "unknown"
cflags_state = ['-DFE_BUILD_ID="%s"' % build_id]

def cc(src):
    o = obj / (src.parent.name + "_" + src.stem + ".o")
    stamp = src.name == "state.c"  # carries the build id: compiled every time
    if not stamp and o.exists() and o.stat().st_mtime > src.stat().st_mtime and all(
            o.stat().st_mtime > h.stat().st_mtime for h in list((ROOT / "runtime").glob("*.h")) + [gen / "funcs.h"]):
        return o, None
    r = subprocess.run([*CC, *cflags, *(cflags_state if stamp else []), "-c", str(src), "-o", str(o)], capture_output=True,
                       text=True)
    return o, (r.stdout + r.stderr) if r.returncode else (r.stderr or None)

# FE_JOBS: parallel compiler processes (the standalone installer caps them by free memory)
with ThreadPoolExecutor(max_workers=max(1, int(os.environ.get("FE_JOBS", 0)) or os.cpu_count() or 1)) as ex:
    results = list(ex.map(cc, srcs))
failed = False
for (o, msg), src in zip(results, srcs):
    if msg:
        print("--", src.name)
        print(msg[:4000])
        if not o.exists() or "error" in msg:
            failed = True
if failed:
    sys.exit("compile failed")
if not WINDOWS:
    # Linux: the system's SDL2 and OpenGL (FE_LIBS: paths or -l names), no icon resource, no DLL to copy
    exe = build / (game + "port")
    libs = shlex.split(os.environ.get("FE_LIBS", "-lSDL2 -lGL"))
    subprocess.check_call([*CC, *[str(o) for o, _ in results], "-o", str(exe), *libs, "-lm", "-lpthread", "-ldl"])
    print("built", exe)
    sys.exit(0)
# the icon (resource 1) and version information; windres runs in icons/ so the .rc names the .ico plainly
rc = ROOT / "icons" / (game + "port.rc")
res = obj / (game + "port_res.o")
if not res.exists() or any(res.stat().st_mtime <= f.stat().st_mtime for f in (rc, rc.with_name(game + ".ico"))):
    subprocess.check_call([WINDRES, "-O", "coff", "-i", rc.name, "-o", str(res)], cwd=rc.parent)
exe = build / (game + "port.exe")
subprocess.check_call([GCC, *[str(o) for o, _ in results], str(res), "-o", str(exe), "-L", str(SDL / "lib"),
                       "-lmingw32", "-lSDL2main", "-lSDL2", "-lopengl32", "-mconsole",
                       "-Wl,--wrap=fopen"])  # runtime/filetrace.c (FE_TRACE_FILES)
shutil.copy(SDL / "bin" / "SDL2.dll", build / "SDL2.dll")
print("built", exe)
