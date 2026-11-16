"""Fear Effect setup window: clone the port, drop in the asset pack and discs, then run install.bat / install.sh.

    python setup/helix_setup.py              open the window
    python setup/helix_setup.py --self-test  fixture run (no window, no real discs)

Stdlib only. Folder pickers use tkinter. 7-Zip is used for .7z packs when it is installed.
"""
from __future__ import annotations

import argparse, json, os, re, shutil, socket, subprocess, sys, threading, webbrowser, zipfile
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse

HERE = Path(__file__).resolve().parent
REPO_URL = "https://github.com/iva1nn/project-helix.git"
REPO_ZIP = "https://github.com/iva1nn/project-helix/archive/refs/heads/main.zip"
GAME_DIR_NAME = "FearEffect"
ASSETS_URL = "https://feremaster.us/downloads"
ARCHIVE_EXT = {".7z", ".zip", ".rar"}
DISC_EXT = {".cue", ".bin", ".img", ".iso"}
CUE_FILE = re.compile(r'FILE\s+"([^"]+)"|FILE\s+(\S+)', re.I)
# content tokens, Fear Effect 2 first so a mixed haystack cannot match FE1 first
SERIAL_BYTES = (
    (b"SLUS_012.66", "fe2"), (b"SLUS_012.75", "fe2"), (b"SLUS_012.76", "fe2"), (b"SLUS_012.77", "fe2"),
    (b"SLUS-01266", "fe2"), (b"SLUS-01275", "fe2"), (b"SLUS-01276", "fe2"), (b"SLUS-01277", "fe2"),
    (b"SLUS_009.20", "fe1"), (b"SLUS_010.56", "fe1"), (b"SLUS_010.57", "fe1"), (b"SLUS_010.58", "fe1"),
    (b"SLUS-00920", "fe1"), (b"SLUS-01056", "fe1"), (b"SLUS-01057", "fe1"), (b"SLUS-01058", "fe1"),
)
NAME_SERIALS = (
    (re.compile(r"slus[-_]?0?12(66|75|76|77)", re.I), "fe2"),
    (re.compile(r"fear\s*effect\s*2|retro\s*helix", re.I), "fe2"),
    (re.compile(r"slus[-_]?0?(0920|1056|1057|1058)", re.I), "fe1"),
    (re.compile(r"fear\s*effect", re.I), "fe1"),
)

WINDOWS = os.name == "nt"
_NOWIN = {}
if WINDOWS:
    _NOWIN = {"creationflags": getattr(subprocess, "CREATE_NO_WINDOW", 0x08000000)}
    try:
        _si = subprocess.STARTUPINFO()
        _si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        _si.wShowWindow = 0
        _NOWIN["startupinfo"] = _si
    except Exception:
        pass


def run_hidden(cmd, **kw):
    kw.update(_NOWIN)
    return subprocess.run(cmd, **kw)


def popen_hidden(cmd, **kw):
    kw.update(_NOWIN)
    return subprocess.Popen(cmd, **kw)


STATE = {
    "busy": False,
    "ok": False,
    "error": "",
    "log": [],
    "phase": "idle",
    "progress": 0.0,
    "status": "",
    "quit": False,
}
LOCK = threading.Lock()
BROWSER = None  # Popen of the Edge/Chrome --app window
CHROMIUM_PROFILE = "fe-setup-chromium"
PHASE_FRAC = {
    "idle": 0.0, "starting": 0.04, "clone": 0.10, "assets": 0.28,
    "discs": 0.42, "build": 0.50, "done": 1.0,
}
STEP_FRAC = {"1/5": 0.55, "2/5": 0.64, "3/5": 0.72, "4/5": 0.82, "5/5": 0.94}
PROGRESS_LINE = re.compile(r"^progress\s+(0\.\d+|1(?:\.0+)?)\s*(.*)$")


def port_root():
    """The port folder this setup lives in, or None if setup was copied out on its own."""
    p = HERE.parent
    if ((p / "install.bat").is_file() or (p / "install.sh").is_file()) and (
        p / "src" / "installer" / "install.py"
    ).is_file():
        return p
    return None


def in_tree():
    return port_root() is not None


def setup_version():
    for p in (HERE.parent / "src" / "VERSION.txt", HERE.parent / "VERSION.txt"):
        if p.is_file():
            return p.read_text(encoding="utf-8", errors="replace").strip().splitlines()[0]
    git = shutil.which("git")
    if git:
        r = run_hidden([git, "-C", str(HERE.parent), "rev-parse", "--short", "HEAD"],
                       capture_output=True, text=True)
        if r.returncode == 0 and r.stdout.strip():
            return r.stdout.strip()
    return "setup"


def set_phase(name):
    labels = {
        "starting": "Starting install",
        "clone": "Cloning the port",
        "assets": "Setting up the asset pack",
        "discs": "Copying disc images",
        "build": "Building the game",
        "done": (
            "Ready. Press Play in RecompLauncher, or use the Desktop shortcut."
            if in_tree()
            else "Ready. Start the game from its Desktop shortcut."
        ),
    }
    with LOCK:
        STATE["phase"] = name
        if name in PHASE_FRAC:
            STATE["progress"] = max(float(STATE.get("progress") or 0), PHASE_FRAC[name])
        if name == "error":
            STATE["status"] = STATE.get("error") or "Install stopped"
        elif name in labels:
            STATE["status"] = labels[name]


def _out(line):
    try:
        if sys.stdout:
            print(line, flush=True)
    except OSError:
        pass


def hide_console():
    if not WINDOWS:
        return
    try:
        import ctypes
        hwnd = ctypes.windll.kernel32.GetConsoleWindow()
        if hwnd:
            ctypes.windll.user32.ShowWindow(hwnd, 0)
            ctypes.windll.kernel32.FreeConsole()
    except Exception:
        pass
    if sys.stdout is None:
        try:
            sys.stdout = open(os.devnull, "w")
            sys.stderr = open(os.devnull, "w")
        except OSError:
            pass


def log(msg):
    line = str(msg)
    _out(line)
    with LOCK:
        STATE["log"].append(line)
        if len(STATE["log"]) > 400:
            STATE["log"] = STATE["log"][-300:]
        m = PROGRESS_LINE.match(line)
        if m:
            STATE["progress"] = float(m.group(1))
            text = (m.group(2) or "").strip()
            if text:
                STATE["status"] = text
            return
        if line.startswith("== "):
            for key, frac in STEP_FRAC.items():
                if line.startswith("== " + key):
                    STATE["progress"] = max(float(STATE.get("progress") or 0), frac)
                    STATE["status"] = line[3:].strip()
                    return
        if line.startswith("INSTALL STOPPED"):
            STATE["status"] = line.split(":", 1)[-1].strip() or line


def os_info():
    if WINDOWS:
        return {
            "id": "windows",
            "label": "Windows",
            "script": "install.bat",
            "ok": True,
        }
    ident = ""
    try:
        ident = Path("/etc/os-release").read_text(encoding="utf-8", errors="replace").lower()
    except OSError:
        pass
    steam = "steamos" in ident or os.environ.get("SteamDeck") == "1" or Path("/usr/bin/steamos-session-select").exists()
    return {
        "id": "steamos" if steam else "linux",
        "label": "SteamOS" if steam else "Linux",
        "script": "install.sh",
        "ok": True,
    }


def recomp_library():
    appdata = os.environ.get("APPDATA") or os.environ.get("HOME") or ""
    lib_json = Path(appdata) / "com.recomplauncher.app" / "library.json"
    if lib_json.is_file():
        try:
            data = json.loads(lib_json.read_text(encoding="utf-8"))
            d = (data.get("settings") or {}).get("library_dir") or ""
            if d and Path(d).is_dir():
                return str(Path(d))
        except (OSError, ValueError):
            pass
    for cand in (Path("D:/RecompLib"), Path("C:/RecompLib"), Path.home() / "RecompLib"):
        if cand.is_dir():
            return str(cand)
    if Path("D:/").exists():
        return str(Path("D:/RecompLib"))
    return str(Path.home() / "RecompLib")


def default_game_folder():
    root = port_root()
    if root is not None:
        return str(root)
    return str(Path(recomp_library()) / GAME_DIR_NAME)


def find_7z():
    cands = [
        Path(r"C:\Program Files\7-Zip\7z.exe"),
        Path(r"C:\Program Files (x86)\7-Zip\7z.exe"),
    ]
    for p in cands:
        if p.is_file():
            return str(p)
    return shutil.which("7z") or shutil.which("7za")


def browse(kind):
    import tkinter as tk
    from tkinter import filedialog

    root = tk.Tk()
    root.withdraw()
    root.attributes("-topmost", True)
    title = {
        "library": "Recomp library (the folder games are installed into)",
        "game": "Game folder (the cloned repository will live here)",
        "assets": "Asset pack folder or archive",
        "discs": "Folder with your BIN/CUE disc images",
        "discs_fe1": "Fear Effect BIN/CUE (all 4 discs, USA)",
        "discs_fe2": "Fear Effect 2 BIN/CUE (all 4 discs, USA v1.1)",
    }.get(kind, "Choose a folder")
    path = filedialog.askdirectory(title=title)
    root.destroy()
    return path or ""


def git_clone(dest: Path):
    dest.parent.mkdir(parents=True, exist_ok=True)
    if (dest / "install.bat").is_file() or (dest / "install.sh").is_file():
        log("Repository already in %s" % dest)
        root = port_root()
        try:
            same = root is not None and dest.resolve() == root.resolve()
        except OSError:
            same = False
        if same:
            return
        git = shutil.which("git")
        if git and (dest / ".git").exists():
            r = run_hidden([git, "-C", str(dest), "pull", "--ff-only"], capture_output=True, text=True)
            if r.returncode:
                log("git pull skipped: %s" % ((r.stderr or r.stdout).strip().splitlines() or ["failed"])[-1])
            else:
                log("Updated from GitHub")
        return
    git = shutil.which("git")
    if git:
        log("Cloning %s" % REPO_URL)
        r = run_hidden([git, "clone", "--depth", "1", REPO_URL, str(dest)], capture_output=True, text=True)
        if r.returncode == 0:
            log("Cloned into %s" % dest)
            return
        log("git clone failed: %s" % ((r.stderr or r.stdout).strip().splitlines() or ["error"])[-1])
    log("Downloading the repository zip")
    import urllib.request

    zip_path = dest.parent / (GAME_DIR_NAME + ".zip")
    urllib.request.urlretrieve(REPO_ZIP, zip_path)
    tmp = dest.parent / (GAME_DIR_NAME + ".unpack")
    if tmp.exists():
        shutil.rmtree(tmp)
    with zipfile.ZipFile(zip_path) as z:
        z.extractall(tmp)
    zip_path.unlink(missing_ok=True)
    inner = next(tmp.iterdir())
    if dest.exists():
        shutil.rmtree(dest)
    inner.rename(dest)
    shutil.rmtree(tmp, ignore_errors=True)
    log("Unpacked repository into %s" % dest)


def overlay_local_src(dest: Path):
    """Copy installer/runtime fixes from this setup tree into the cloned game folder (after git pull)."""
    standalone = HERE.parent
    pairs = []
    inst = standalone / "src" / "installer" / "install.py"
    if inst.is_file():
        pairs.append((inst, dest / "src" / "installer" / "install.py"))
    cdrom = standalone / "src" / "runtime" / "cdrom.c"
    if not cdrom.is_file():
        cdrom = standalone.parent.parent / "runtime" / "cdrom.c"
    if cdrom.is_file():
        pairs.append((cdrom, dest / "src" / "runtime" / "cdrom.c"))
    n = 0
    for src, dst in pairs:
        try:
            if src.resolve() == dst.resolve():
                continue
        except OSError:
            pass
        if not src.is_file() or not dst.parent.exists():
            continue
        shutil.copy2(src, dst)
        n += 1
        log("  local fix: %s" % dst.relative_to(dest))
    if n:
        log("Applied %d local installer/runtime fix(es)" % n)


def is_junction(path: Path):
    if not path.exists():
        return False
    try:
        if path.is_symlink():
            return True
    except OSError:
        pass
    if not WINDOWS:
        return False
    import ctypes
    attrs = ctypes.windll.kernel32.GetFileAttributesW(str(path))
    return attrs != 0xFFFFFFFF and bool(attrs & 0x400)  # FILE_ATTRIBUTE_REPARSE_POINT


def remove_link(path: Path):
    """Remove a junction or symlink without touching its target."""
    if path.is_symlink():
        path.unlink()
        return
    path.rmdir()


def stale_pack_link(path: Path):
    """A leftover junction to a catalogue wrapper (…\\fe1 with a nested data\\), not the pack's fe1 tree."""
    if not path.exists() or not is_junction(path):
        return False
    return (path / "data").is_dir() or not looks_like_game_assets(path)


def clear_stale_pack_links(dest: Path):
    data_dst = dest / "data"
    if not data_dst.is_dir():
        return
    for name in ("fe1", "fe2"):
        p = data_dst / name
        if stale_pack_link(p):
            log("  removing leftover data\\%s link (it pointed at a catalogue folder, not the pack)" % name)
            remove_link(p)


def placeholder_disc_dir(folder: Path):
    if not folder.exists():
        return True
    for p in folder.iterdir():
        if p.suffix.lower() in DISC_EXT or p.is_dir():
            return False
        if p.name.lower() not in ("readme.txt", "readme.md", ".gitkeep"):
            return False
    return True


def link_or_copy(src: Path, dst: Path):
    if dst.exists():
        try:
            empty = dst.is_dir() and not any(dst.iterdir())
        except OSError:
            empty = False
        if empty:
            dst.rmdir()
        else:
            return "exists"
    dst.parent.mkdir(parents=True, exist_ok=True)
    if src.is_dir() and WINDOWS:
        r = run_hidden(["cmd", "/c", "mklink", "/J", str(dst), str(src)], capture_output=True, text=True)
        if r.returncode == 0:
            return "link"
    elif src.is_dir():
        try:
            os.symlink(src, dst, target_is_directory=True)
            return "link"
        except OSError:
            pass
    if src.is_dir():
        shutil.copytree(src, dst)
    else:
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)
    return "copy"


def looks_like_game_assets(gdir: Path):
    """The pack's data\\fe1 or data\\fe2 tree (models, movies, …), not a catalogue wrapper."""
    if not gdir.is_dir():
        return False
    return any((gdir / name).is_dir() for name in ("models", "movies", "textures", "coll", "icons"))


def looks_like_data_tree(folder: Path):
    """A folder whose children are fe1/, fe2/, manifests/, build/ as the installer expects under data\\."""
    if not folder.is_dir():
        return False
    man = folder / "manifests"
    if man.is_dir() and any(man.glob("*.json")):
        return True
    return any(looks_like_game_assets(folder / g) for g in ("fe1", "fe2"))


def pack_data_roots(folder: Path):
    """One or more data-tree roots. A catalogue (fe1/data + fe2/data) yields both; a single extracted pack yields one."""
    folder = folder.resolve()
    found, seen = [], set()

    def add(p: Path):
        key = str(p).lower() if WINDOWS else str(p)
        if key in seen:
            return
        seen.add(key)
        found.append(p)

    catalogue = False
    for g in ("fe1", "fe2"):
        for c in (folder / g / "data", folder / g):
            if looks_like_data_tree(c):
                add(c)
                catalogue = True
                break
    if catalogue:
        return found
    for c in (folder, folder / "data"):
        if looks_like_data_tree(c):
            add(c)
            return found
    return found


def extract_assets(src: Path, dest: Path):
    """Put the pack's data\\ into the game folder. Archives are extracted; an already-unpacked pack is copied."""
    clear_stale_pack_links(dest)
    if src.is_file() and src.suffix.lower() in ARCHIVE_EXT:
        archives = [src]
        root = src.parent
        roots = []
    elif src.is_dir():
        roots = pack_data_roots(src)
        archives = []
        root = src
        if not roots:
            archives = [p for p in src.iterdir() if p.is_file() and p.suffix.lower() in ARCHIVE_EXT]
    else:
        raise RuntimeError("that asset path is not a folder or an archive")
    if roots:
        data_dst = dest / "data"
        data_dst.mkdir(exist_ok=True)
        for unpacked in roots:
            log("Using extracted pack at %s" % unpacked)
            for name in ("fe1", "fe2", "build", "manifests"):
                s = unpacked / name
                d = data_dst / name
                if not s.exists():
                    continue
                if name == "manifests" and s.is_dir():
                    d.mkdir(exist_ok=True)
                    for f in sorted(s.glob("*.json")):
                        out = d / f.name
                        if out.exists():
                            continue
                        shutil.copy2(f, out)
                        log("  copy data\\manifests\\%s" % f.name)
                    continue
                if name == "build" and s.is_dir():
                    d.mkdir(exist_ok=True)
                    for f in sorted(p for p in s.rglob("*") if p.is_file()):
                        out = d / f.relative_to(s)
                        if out.exists():
                            continue
                        out.parent.mkdir(parents=True, exist_ok=True)
                        shutil.copy2(f, out)
                        log("  copy data\\build\\%s" % f.relative_to(s).as_posix().replace("/", "\\"))
                    continue
                how = link_or_copy(s, d)
                log("  %s data\\%s" % (how, name))
        return
    if not archives:
        raise RuntimeError("no .7z / .zip pack in %s. Download it from %s" % (root, ASSETS_URL))
    for arc in archives:
        log("Extracting %s (this can take a while)" % arc.name)
        if arc.suffix.lower() == ".zip":
            with zipfile.ZipFile(arc) as z:
                z.extractall(dest)
            continue
        z7 = find_7z()
        if not z7:
            raise RuntimeError("7-Zip is needed to extract %s. Install it from https://www.7-zip.org/" % arc.name)
        r = run_hidden([z7, "x", str(arc), "-o" + str(dest), "-y", "-bsp1"], capture_output=True, text=True)
        if r.returncode:
            raise RuntimeError("could not extract %s: %s" % (arc.name, (r.stderr or r.stdout)[-300:]))
        log("  extracted %s" % arc.name)


def sniff_bytes(path: Path):
    try:
        with open(path, "rb") as f:
            blob = f.read(2 << 20)
    except OSError:
        return None
    for token, game in SERIAL_BYTES:
        if token in blob:
            return game
    return None


def cue_bins(cue: Path):
    try:
        text = cue.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return []
    out = []
    for m in CUE_FILE.finditer(text):
        name = (m.group(1) or m.group(2) or "").strip()
        if not name:
            continue
        p = Path(name) if Path(name).is_absolute() else cue.parent / name
        if not p.exists():
            alt = [q for q in cue.parent.iterdir() if q.name.lower() == Path(name).name.lower()]
            if alt:
                p = alt[0]
        out.append(p)
    return out


def sniff_disc(path: Path, hint=None):
    """Which game a disc file belongs to. Content and serials beat the row the user picked."""
    name = path.name
    joined = str(path)
    for rx, game in NAME_SERIALS:
        if rx.search(name) or rx.search(joined):
            return game
    ext = path.suffix.lower()
    if ext in (".bin", ".img", ".iso"):
        game = sniff_bytes(path)
        if game:
            return game
    if ext == ".cue":
        for b in cue_bins(path):
            if b.suffix.lower() in (".bin", ".img", ".iso"):
                game = sniff_bytes(b)
                if game:
                    return game
            for rx, game in NAME_SERIALS:
                if rx.search(b.name):
                    return game
    for part in reversed(path.parts):
        low = part.lower()
        if low in ("fe1", "fe2"):
            return low
    return hint if hint in ("fe1", "fe2") else None


def disc_files(folder: Path):
    return [p for p in folder.rglob("*") if p.is_file() and p.suffix.lower() in DISC_EXT]


def peek_folder(path):
    """How many discs in a folder look like FE1 / FE2 (unique cue, or raw image if no cue)."""
    out = {"fe1": 0, "fe2": 0, "unknown": 0}
    folder = Path(path) if path else None
    if not folder or not folder.is_dir():
        return out
    seen = set()
    for p in disc_files(folder):
        if p.suffix.lower() in (".bin", ".img") and p.with_suffix(".cue").is_file():
            continue
        game = sniff_disc(p)
        g = game if game in ("fe1", "fe2") else "unknown"
        disc_n = re.search(r"\(disc\s*(\d+)\)", p.name, re.I)
        key = (g, disc_n.group(1) if disc_n else p.stem.lower())
        if key in seen:
            continue
        seen.add(key)
        out[g] += 1
    return out


def _copy_one(src: Path, dst: Path):
    dst.parent.mkdir(parents=True, exist_ok=True)
    if dst.exists():
        return False
    shutil.copy2(src, dst)
    return True


def copy_discs(dest: Path, fe1="", fe2=""):
    """Copy or link picked folders into discs\\fe1 and discs\\fe2. The installer identifies by content."""
    sources = []  # (folder, hint)
    a, b = (fe1 or "").strip(), (fe2 or "").strip()
    if a and b:
        pa, pb = Path(a), Path(b)
        try:
            same = pa.resolve() == pb.resolve()
        except OSError:
            same = os.path.normcase(str(pa)) == os.path.normcase(str(pb))
        if same:
            sources.append((pa, None))
        else:
            sources.append((pa, "fe1"))
            sources.append((pb, "fe2"))
    elif a:
        sources.append((Path(a), "fe1"))
    elif b:
        sources.append((Path(b), "fe2"))
    else:
        raise RuntimeError("choose a folder of BIN/CUE images for Fear Effect, Fear Effect 2, or both")

    seen, items = set(), []
    for src, hint in sources:
        if not src.is_dir():
            raise RuntimeError("choose the folder that holds your .cue / .bin files (%s)" % src)
        files = disc_files(src)
        if not files:
            raise RuntimeError("no .cue / .bin / .img files in %s" % src)
        for p in files:
            try:
                key = str(p.resolve()).lower() if WINDOWS else str(p.resolve())
            except OSError:
                key = str(p)
            if key in seen:
                continue
            seen.add(key)
            game = sniff_disc(p, hint=hint) or hint or "unknown"
            items.append((p, game, hint))

    by_game = {"fe1": [], "fe2": [], "unknown": []}
    reroute = []
    for p, game, hint in items:
        by_game.setdefault(game, []).append(p)
        if hint in ("fe1", "fe2") and game in ("fe1", "fe2") and game != hint:
            reroute.append((p.name, hint, game))
    titles = {"fe1": "Fear Effect", "fe2": "Fear Effect 2"}
    if reroute:
        log("  note: some images did not match the row they were picked in (the files win):")
        for name, hint, game in reroute[:8]:
            log("    %s: %s row → %s" % (name, titles[hint], titles[game]))

    for game in ("fe1", "fe2"):
        files = by_game.get(game) or []
        if not files:
            continue
        dst = dest / "discs" / game
        if is_junction(dst):
            log("  removing disc junction discs\\%s (cues must live on the game drive)" % game)
            remove_link(dst)
        dst.mkdir(parents=True, exist_ok=True)
        n = 0
        for p in files:
            if p.suffix.lower() in (".bin", ".img") and p.with_suffix(".cue").is_file():
                continue
            if p.suffix.lower() == ".cue":
                bins = cue_bins(p)
                it = iter(bins)
                try:
                    text = p.read_text(encoding="utf-8", errors="replace")
                except OSError:
                    text = p.read_bytes().decode("latin-1", "replace")

                def repl(m, _it=it):
                    b = next(_it, None)
                    return ('FILE "%s"' % str(Path(b).resolve())) if b is not None else m.group(0)

                if CUE_FILE.search(text):
                    text = CUE_FILE.sub(repl, text)
                out = dst / p.name
                out.write_text(text, encoding="utf-8")
                n += 1
                log("  cue %s -> discs\\%s (image stays at %s)" % (p.name, game, p.parent))
            elif p.suffix.lower() in (".bin", ".img"):
                cue = dst / (p.stem + ".cue")
                if not cue.exists():
                    cue.write_text(
                        'FILE "%s" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n' % p.resolve(),
                        encoding="ascii",
                    )
                    n += 1
                    log("  cue for %s -> discs\\%s" % (p.name, game))
        if n:
            log("Wrote %d cue files into discs\\%s" % (n, game))
        else:
            log("Disc cues were already in discs\\%s" % game)

    unknown = by_game.get("unknown") or []
    if unknown:
        disc_dst = dest / "discs"
        disc_dst.mkdir(exist_ok=True)
        n = 0
        for p in unknown:
            if _copy_one(p, disc_dst / p.name):
                n += 1
                log("  disc %s -> discs\\ (unrecognized, the installer will identify it)" % p.name)
        if n:
            log("Copied %d unrecognized disc files into discs\\" % n)


def run_installer(dest: Path):
    env = dict(os.environ)
    env["FE_NO_PAUSE"] = "1"
    env["FE_IN_TERMINAL"] = "1"  # install.sh: do not open konsole/gnome-terminal
    hide = HERE / "_hidewin"
    if hide.is_dir():
        env["PYTHONPATH"] = str(hide) + os.pathsep + env.get("PYTHONPATH", "")
    py = dest / "src" / "installer" / "install.py"
    label = "installer"
    if py.is_file():
        cmd = [sys.executable, "-B", str(py)]
        log("Running the installer (builds every complete game, no extra windows)")
    else:
        info = os_info()
        label = info["script"]
        script = dest / info["script"]
        if not script.is_file():
            raise RuntimeError("%s is missing in %s" % (label, dest))
        log("Running %s (builds every game that has a complete disc set)" % label)
        if WINDOWS:
            cmd = ["cmd", "/c", str(script)]
        else:
            os.chmod(script, os.stat(script).st_mode | 0o111)
            cmd = ["bash", str(script)]
    proc = popen_hidden(
        cmd, cwd=str(dest), env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, errors="replace",
    )
    assert proc.stdout is not None
    for line in proc.stdout:
        t = line.rstrip()
        if t:
            log(t)
    rc = proc.wait()
    if rc:
        raise RuntimeError("%s stopped with code %d (see the log)" % (label, rc))
    log("Installer finished. Start the game from its Desktop shortcut.")


def install(game_folder: str, assets: str, discs_fe1: str, discs_fe2: str):
    with LOCK:
        if STATE["busy"]:
            raise RuntimeError("an install is already running")
        STATE.update(busy=True, ok=False, error="", phase="starting", progress=0.04,
                     status="Starting install")
        STATE["log"] = []
    dest = Path(game_folder)
    try:
        set_phase("clone")
        log("== Game folder: %s" % dest)
        git_clone(dest)
        overlay_local_src(dest)
        set_phase("assets")
        log("== Asset pack")
        extract_assets(Path(assets), dest)
        set_phase("discs")
        log("== Disc images")
        if discs_fe1:
            log("  Fear Effect folder: %s" % discs_fe1)
        if discs_fe2:
            log("  Fear Effect 2 folder: %s" % discs_fe2)
        copy_discs(dest, fe1=discs_fe1, fe2=discs_fe2)
        set_phase("build")
        log("== Building the game")
        run_installer(dest)
        set_phase("done")
        with LOCK:
            STATE["ok"] = True
            STATE["busy"] = False
        log("Done.")
    except Exception as e:
        with LOCK:
            STATE["error"] = str(e)
            STATE["busy"] = False
            STATE["phase"] = "error"
            STATE["status"] = str(e)
        log("INSTALL STOPPED: %s" % e)
        raise


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        return

    def _json(self, code, obj):
        raw = json.dumps(obj).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(raw)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(raw)

    def _file(self, path: Path, mime):
        data = path.read_bytes()
        self.send_response(200)
        self.send_header("Content-Type", mime)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        p = urlparse(self.path).path
        if p in ("/", "/index.html"):
            return self._file(HERE / "index.html", "text/html; charset=utf-8")
        if p == "/style.css":
            return self._file(HERE / "style.css", "text/css; charset=utf-8")
        if p == "/app.js":
            return self._file(HERE / "app.js", "application/javascript; charset=utf-8")
        if p == "/api/state":
            info = os_info()
            with LOCK:
                st = dict(STATE)
                st["log"] = list(STATE["log"][-80:])
            st.update(os=info, library=recomp_library(), game_folder=default_game_folder(),
                      in_tree=in_tree(), assets_url=ASSETS_URL, seven_zip=bool(find_7z()),
                      git=bool(shutil.which("git")), version=setup_version())
            return self._json(200, st)
        if p == "/api/progress":
            with LOCK:
                st = dict(STATE)
                st["log"] = list(STATE["log"][-80:])
            return self._json(200, st)
        self.send_error(404)

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        try:
            body = json.loads(self.rfile.read(n).decode("utf-8") or "{}") if n else {}
        except ValueError:
            return self._json(400, {"error": "bad json"})
        p = urlparse(self.path).path
        if p == "/api/browse":
            kind = body.get("kind") or "game"
            path = browse(kind)
            peek = peek_folder(path) if path and str(kind).startswith("discs") else None
            return self._json(200, {"path": path, "peek": peek})
        if p == "/api/peek":
            path = (body.get("path") or "").strip()
            return self._json(200, {"peek": peek_folder(path)})
        if p == "/api/open":
            webbrowser.open(body.get("url") or ASSETS_URL)
            return self._json(200, {"ok": True})
        if p == "/api/quit":
            STATE["quit"] = True
            threading.Timer(0.15, _quit_process).start()
            return self._json(200, {"ok": True})
        if p == "/api/install":
            game_folder = (body.get("game_folder") or "").strip()
            assets = (body.get("assets") or "").strip()
            discs_fe1 = (body.get("discs_fe1") or "").strip()
            discs_fe2 = (body.get("discs_fe2") or "").strip()
            # older UI sent one folder + a game toggle
            if not discs_fe1 and not discs_fe2 and (body.get("discs") or "").strip():
                if body.get("game") == "fe2":
                    discs_fe2 = body.get("discs").strip()
                else:
                    discs_fe1 = body.get("discs").strip()
            if not game_folder or not assets or not (discs_fe1 or discs_fe2):
                return self._json(400, {"error": "choose the game folder, the asset pack and at least one disc folder"})
            t = threading.Thread(target=_install_thread, args=(game_folder, assets, discs_fe1, discs_fe2), daemon=True)
            t.start()
            return self._json(200, {"ok": True})
        self.send_error(404)


def _install_thread(game_folder, assets, discs_fe1, discs_fe2):
    try:
        install(game_folder, assets, discs_fe1, discs_fe2)
    except Exception:
        pass


def pick_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


WINDOW_W, WINDOW_H = 720, 440


def chromium_profile():
    return Path(os.environ.get("TEMP") or os.environ.get("TMP") or "/tmp") / CHROMIUM_PROFILE


def close_window():
    """Edge/Chrome --app windows ignore window.close() (not opened by script). Kill our process tree."""
    global BROWSER
    proc = BROWSER
    BROWSER = None
    if proc is not None and proc.poll() is None:
        try:
            if WINDOWS:
                run_hidden(["taskkill", "/PID", str(proc.pid), "/T", "/F"], capture_output=True)
            else:
                proc.terminate()
        except OSError:
            pass
    if WINDOWS:
        # hand-off child (msedge often exits the stub and keeps a process with our profile)
        ps = (
            "Get-CimInstance Win32_Process | Where-Object { "
            "($_.Name -eq 'msedge.exe' -or $_.Name -eq 'chrome.exe') -and "
            "$_.CommandLine -like '*" + CHROMIUM_PROFILE + "*' "
            "} | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }"
        )
        try:
            run_hidden(
                ["powershell", "-NoProfile", "-NonInteractive", "-Command", ps],
                capture_output=True, timeout=8,
            )
        except (OSError, subprocess.TimeoutExpired):
            pass


def _quit_process():
    close_window()
    os._exit(0)


def open_window(url):
    # A separate profile is required: if Edge/Chrome is already running, --app is handed
    # to that process and --window-size is ignored (it restores the last app window).
    global BROWSER
    profile = chromium_profile()
    try:
        profile.mkdir(parents=True, exist_ok=True)
    except OSError:
        profile = Path(os.environ.get("TEMP") or os.environ.get("TMP") or "/tmp")
    flags = [
        "--user-data-dir=" + str(profile),
        "--no-first-run",
        "--no-default-browser-check",
        "--window-size=%d,%d" % (WINDOW_W, WINDOW_H),
        "--window-position=80,60",
        "--app=" + url,
    ]
    cands = []
    if WINDOWS:
        local = os.environ.get("LOCALAPPDATA", "")
        cands = [
            [str(Path(local) / "Microsoft/Edge/Application/msedge.exe")] + flags,
            [r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"] + flags,
            [r"C:\Program Files\Google\Chrome\Application\chrome.exe"] + flags,
        ]
    else:
        cands = [["chromium"] + flags, ["google-chrome"] + flags, ["microsoft-edge"] + flags]
    for cmd in cands:
        if cmd and (Path(cmd[0]).is_file() or shutil.which(cmd[0])):
            try:
                BROWSER = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                return
            except OSError:
                pass
    webbrowser.open(url)


def _write_fake_disc(folder: Path, name: str, serial: bytes):
    folder.mkdir(parents=True, exist_ok=True)
    cue = folder / (name + ".cue")
    bin_name = name + ".bin"
    cue.write_text('FILE "%s" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n' % bin_name, encoding="ascii")
    (folder / bin_name).write_bytes(b"\x00" * 128 + b"BOOT = cdrom:\\" + serial + b";1\n" + b"\x00" * 64)


def self_test(tmp: Path):
    """A tiny fake pack + discs + installer, no GitHub and no real game files."""
    lib = tmp / "RecompLib"
    game = lib / GAME_DIR_NAME
    game.mkdir(parents=True)
    (game / "install.bat").write_text("@echo off\necho fake install\nexit /b 0\n", encoding="ascii")
    (game / "install.sh").write_text("#!/bin/sh\necho fake install\nexit 0\n", encoding="ascii")

    pack = tmp / "pack"
    (pack / "data" / "fe2").mkdir(parents=True)
    (pack / "data" / "fe2" / "models").mkdir()
    (pack / "data" / "manifests").mkdir(parents=True)
    (pack / "data" / "build").mkdir(parents=True)
    (pack / "data" / "fe2" / "ok.txt").write_text("pack", encoding="utf-8")
    (pack / "data" / "fe2" / "models" / "ok.txt").write_text("pack", encoding="utf-8")
    (pack / "data" / "manifests" / "fe2.json").write_text("{}", encoding="utf-8")
    (pack / "data" / "build" / "font_table.h").write_text("/* test */\n", encoding="utf-8")
    z = tmp / "fe2-assets-test.zip"
    with zipfile.ZipFile(z, "w") as a:
        a.writestr("data/fe2/fromzip.txt", "zip")
    extract_assets(pack / "data", game)
    assert (game / "data" / "fe2" / "ok.txt").is_file()
    assert (game / "data" / "build" / "font_table.h").is_file()
    extract_assets(z, game)
    assert (game / "data" / "fe2" / "fromzip.txt").is_file()

    # catalogue root like F:\\fe_assets (fe1\\data and fe2\\data), not a single extracted data\\
    cat = tmp / "catalogue"
    for g in ("fe1", "fe2"):
        (cat / g / "data" / g / "models").mkdir(parents=True)
        (cat / g / "data" / g / "models" / "ok.txt").write_text(g, encoding="utf-8")
        (cat / g / "data" / "manifests").mkdir(parents=True)
        (cat / g / "data" / "manifests" / (g + ".json")).write_text("{}", encoding="utf-8")
        (cat / g / "data" / "build").mkdir(parents=True)
        (cat / g / "data" / "build" / "font_table.h").write_text("/* %s */\n" % g, encoding="utf-8")
    from_cat = tmp / "fromcat"
    from_cat.mkdir()
    extract_assets(cat, from_cat)
    assert (from_cat / "data" / "fe1" / "models" / "ok.txt").read_text(encoding="utf-8") == "fe1"
    assert (from_cat / "data" / "fe2" / "models" / "ok.txt").read_text(encoding="utf-8") == "fe2"
    assert (from_cat / "data" / "manifests" / "fe1.json").is_file()
    assert (from_cat / "data" / "manifests" / "fe2.json").is_file()
    assert not (from_cat / "data" / "fe1" / "data").exists(), "catalogue wrapper was linked as the pack"

    # FE2 images picked in the Fear Effect row must still land in discs\\fe2
    picked_fe1 = tmp / "picked_as_fe1"
    _write_fake_disc(picked_fe1, "whatever (Disc 1)", b"SLUS_012.66")
    copy_discs(game, fe1=str(picked_fe1), fe2="")
    assert (game / "discs" / "fe2" / "whatever (Disc 1).cue").is_file()
    assert not list((game / "discs" / "fe1").glob("*.cue") if (game / "discs" / "fe1").exists() else [])

    mixed = tmp / "mixed"
    _write_fake_disc(mixed / "fe1", "Fear Effect (Disc 1)", b"SLUS_009.20")
    _write_fake_disc(mixed / "fe2", "Fear Effect 2 (Disc 1)", b"SLUS_012.66")
    both = tmp / "both"
    both.mkdir()
    copy_discs(both, fe1=str(mixed), fe2="")
    assert (both / "discs" / "fe1" / "Fear Effect (Disc 1).cue").is_file()
    assert (both / "discs" / "fe2" / "Fear Effect 2 (Disc 1).cue").is_file()

    peek = peek_folder(mixed)
    assert peek["fe1"] == 1 and peek["fe2"] == 1, peek
    peek2 = peek_folder(picked_fe1)
    assert peek2["fe2"] == 1 and peek2["fe1"] == 0, peek2

    run_installer(game)
    print("self-test ok", game)
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--tmp", type=Path, default=None)
    ap.add_argument("--port", type=int, default=0)
    ap.add_argument("--no-browser", action="store_true")
    ap.add_argument("--console", action="store_true", help="keep the helper console visible")
    args = ap.parse_args()
    if args.self_test:
        tmp = args.tmp or Path(os.environ.get("TEMP") or "/tmp") / ("fe-setup-test-%d" % os.getpid())
        if tmp.exists():
            shutil.rmtree(tmp)
        tmp.mkdir(parents=True)
        try:
            return self_test(tmp)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
    if not args.console and not args.no_browser:
        hide_console()
    port = args.port or pick_port()
    httpd = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    url = "http://127.0.0.1:%d/" % port
    _out("Fear Effect setup: %s" % url)
    if not args.no_browser:
        open_window(url)
    httpd.timeout = 0.5
    try:
        while not STATE["quit"]:
            httpd.handle_request()
    except KeyboardInterrupt:
        STATE["quit"] = True
    close_window()
    return 0


if __name__ == "__main__":
    sys.exit(main() or 0)
