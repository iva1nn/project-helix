"""The standalone installer (started by install.bat, which provides Python): checks the player's disc images and
the remastered assets, downloads the build tools, recompiles each game from the player's own discs, installs it
next to the data folder and creates the shortcuts. Every run is safe to repeat: finished steps are kept.

    install.bat [--game fe1|fe2] [--verify] [--rebuild] [--no-shortcuts] [--no-test]

    --verify        check every asset file's SHA-256 (slow: ~45 GB read), not just its size
    --rebuild       recompile from scratch (the recompiler's output and every object file)
    --no-shortcuts  do not create Desktop / Start menu shortcuts
    --no-test       skip the short test start of each built game

Repository layout (see README.md):
    discs/            the player's disc images (any sub-folder, any file names; sorted by content)
    data/             the remastered assets (the packs extract here), data/ffmpeg.exe, data/fonts
    src/              the port's source, the recompiler, this installer
    .toolchain/       downloaded tools (compiler, SDL2, ffmpeg, Python)
    Fear Effect/, Fear Effect 2/   the installed games (exe, settings, memory cards, save states)

Every failure the player can run into ends in "INSTALL STOPPED: <what happened and what to do>" (Fail), never in a
bare traceback; problems that do not block (a missing shortcut, a duplicate disc) are notes.
"""
import argparse, errno, hashlib, json, os, re, shutil, socket, ssl, subprocess, sys, tarfile, time, urllib.error
import urllib.request, zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
SRC = HERE.parent
ROOT = SRC.parent
TC = ROOT / ".toolchain"
DATA = ROOT / "data"
DISCS = ROOT / "discs"
LOG = ROOT / "install.log"
sys.path.insert(0, str(SRC / "tools" / "recomp"))
from disc import Disc  # noqa: E402

GAMES = json.loads((HERE / "games.json").read_text(encoding="utf-8"))
TOOLS = json.loads((HERE / "toolchain.json").read_text(encoding="utf-8"))
# the published asset packs (tools/export_standalone.py --pack): manifest and archive checksums, per game
PACKS = json.loads((HERE / "packs.json").read_text(encoding="utf-8")) if (HERE / "packs.json").exists() else {}
SECTOR = 2352
NEED_FREE_GB = 3.0  # toolchain (~0.9 GB unpacked + 0.2 GB archives), two builds, headroom

_log = None


def say(*a):
    msg = " ".join(str(x) for x in a)
    try:
        print(msg, flush=True)
    except UnicodeEncodeError:
        print(msg.encode("ascii", "replace").decode(), flush=True)
    if _log:
        try:
            _log.write(msg + "\n")
            _log.flush()
        except OSError:
            pass


def step(title):
    say("")
    say("== " + title)


class Fail(Exception):
    pass


def rel(p):
    try:
        return str(Path(p).resolve().relative_to(ROOT))
    except (ValueError, OSError):
        return str(p)


def free_gb(path=ROOT):
    try:
        return shutil.disk_usage(path).free / 1e9
    except OSError:
        return 1e9


def rmtree(p):
    """delete a folder; retried, since an antivirus scan or Explorer window can hold files for a moment"""
    def writable(fn, path, _):  # read-only files (unpacked archives, git objects) are deletable once made writable
        os.chmod(path, 0o666)
        fn(path)

    for i in range(5):
        try:
            if p.exists():
                shutil.rmtree(p, onerror=writable)
            return
        except OSError:
            time.sleep(1 + i)
    raise Fail("cannot delete %s: a program has files in it open (Explorer, an antivirus scan, the game). Close "
               "it and run install.bat again." % rel(p))


def pid_alive(pid):
    import ctypes
    h = ctypes.windll.kernel32.OpenProcess(0x1000, False, int(pid))
    if not h:
        return False
    code = ctypes.c_ulong()
    ctypes.windll.kernel32.GetExitCodeProcess(h, ctypes.byref(code))
    ctypes.windll.kernel32.CloseHandle(h)
    return code.value == 259


def ram_gb():
    """available physical memory (GB), None if unknown"""
    try:
        import ctypes

        class MS(ctypes.Structure):
            _fields_ = [("len", ctypes.c_ulong), ("load", ctypes.c_ulong), ("total", ctypes.c_ulonglong),
                        ("avail", ctypes.c_ulonglong), ("tp", ctypes.c_ulonglong), ("ap", ctypes.c_ulonglong),
                        ("tv", ctypes.c_ulonglong), ("av", ctypes.c_ulonglong), ("ext", ctypes.c_ulonglong)]
        m = MS()
        m.len = ctypes.sizeof(MS)
        if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m)):
            return m.avail / 2 ** 30
    except Exception:
        pass
    return None


# ---------------------------------------------------------------------------------------------- toolchain
def sha256_file(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        while True:
            b = f.read(1 << 22)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def why(e):
    """a download error in plain words"""
    if isinstance(e, urllib.error.HTTPError):
        return "the site answered with error %d (%s)" % (e.code, e.reason)
    r = getattr(e, "reason", e)
    if isinstance(r, ssl.SSLError) or isinstance(e, ssl.SSLError):
        return ("the site's certificate could not be checked: a proxy or antivirus that inspects HTTPS, or a "
                "wrong date and time on this PC")
    if isinstance(r, socket.gaierror):
        return "no internet connection (the site's name could not be looked up)"
    if isinstance(r, (socket.timeout, TimeoutError)) or isinstance(e, (socket.timeout, TimeoutError)):
        return "the site did not answer in time"
    if isinstance(r, (ConnectionError, OSError)):
        return "the connection failed (%s)" % (getattr(r, "strerror", None) or r)
    return str(e)


def fetch(url, part, size):
    """download url into part, continuing a previous partial download (HTTP Range)"""
    have = part.stat().st_size if part.exists() else 0
    if have > size:
        part.unlink()
        have = 0
    hdr = {"User-Agent": "fe-standalone-installer"}
    if have:
        hdr["Range"] = "bytes=%d-" % have
    req = urllib.request.Request(url, headers=hdr)
    with urllib.request.urlopen(req, timeout=60) as r:
        if have and r.status != 206:  # the server ignored the range: start over
            have = 0
        with open(part, "ab" if have else "wb") as f:
            got, last = have, have
            while True:
                b = r.read(1 << 20)
                if not b:
                    break
                f.write(b)
                got += len(b)
                if got - last > size / 10:
                    last = got
                    say("    %3d %%" % (100 * got // max(size, 1)))
    if part.stat().st_size != size:
        raise ConnectionError("the download stopped early (%d of %d bytes)" % (part.stat().st_size, size))


def download(name, t):
    dl = TC / "downloads"
    dl.mkdir(parents=True, exist_ok=True)
    urls = [t["url"]] + t.get("mirrors", [])
    dst = dl / t["url"].rsplit("/", 1)[1]
    if dst.exists():
        if dst.stat().st_size == t["size"] and sha256_file(dst) == t["sha256"]:
            return dst
        dst.unlink()  # damaged or another version: fetch it again
    if free_gb(dl) < t["size"] / 1e9 * 3 + 0.5:
        raise Fail("not enough free disk space for %s (%.1f GB free)" % (name, free_gb(dl)))
    part = dst.with_name(dst.name + ".part")
    errors = []
    for url in urls:
        for attempt in range(1, 4):
            try:
                say("  downloading %s (%.0f MB) from %s%s" % (name, t["size"] / 1e6, url.split("/")[2],
                                                               " (attempt %d)" % attempt if attempt > 1 else ""))
                fetch(url, part, t["size"])
                if sha256_file(part) == t["sha256"]:
                    os.replace(part, dst)
                    return dst
                part.unlink()
                errors.append("%s: the file's checksum is wrong" % url.split("/")[2])
                break  # a wrong file at this address: retrying it will not help
            except Fail:
                raise
            except Exception as e:  # network errors of every kind: retried, then the next address
                w = why(e)
                say("    failed: " + w)
                errors.append("%s: %s" % (url.split("/")[2], w))
                if isinstance(e, urllib.error.HTTPError) and e.code in (403, 404, 410):
                    break
                time.sleep(3 * attempt)
    lines = ["%s could not be downloaded:" % name] + ["      " + e for e in errors[-3:]] + [
        "  Check the internet connection and run install.bat again (finished downloads are kept).",
        "  Without internet on this PC: download these files on another PC and put them into",
        "      " + str(dl) + "\\",
        "  then run install.bat again:"]
    for n, tt in TOOLS.items():
        if n.startswith("_"):
            continue
        f = dl / tt["url"].rsplit("/", 1)[1]
        if not (f.exists() and f.stat().st_size == tt["size"]):
            lines.append("      " + tt["url"])
    raise Fail("\n".join(lines))


def ensure_tool(name):
    t = TOOLS[name]
    home = TC / name
    check = home / t["check"]
    stamp = home / ".installed"
    if check.exists() and stamp.exists() and stamp.read_text().strip() == t["sha256"]:
        return home / t.get("root", "")
    arc = download(name, t)
    say("  unpacking", name)
    rmtree(home)
    tmp = TC / (name + ".tmp")
    rmtree(tmp)
    tmp.mkdir(parents=True)
    kind = t["kind"]
    try:
        if kind == "zip":
            with zipfile.ZipFile(arc) as z:
                members = [m for m in z.namelist() if not t.get("only") or any(m.endswith(o) for o in t["only"])]
                z.extractall(tmp, members)
        elif kind == "tar.gz":
            with tarfile.open(arc, "r:gz") as z:
                if hasattr(tarfile, "data_filter"):
                    z.extractall(tmp, filter="data")
                else:
                    z.extractall(tmp)
        elif kind == "7z-sfx":
            # the official self-extracting 7-Zip archive: -o<dir> -y extracts without asking
            r = subprocess.run([str(arc), "-o" + str(tmp), "-y"], capture_output=True, text=True, errors="replace",
                               timeout=900)
            if r.returncode:
                raise OSError((r.stdout + r.stderr).strip()[-300:] or "exit code %d" % r.returncode)
    except (zipfile.BadZipFile, tarfile.TarError, EOFError) as e:
        arc.unlink()
        rmtree(tmp)
        raise Fail("%s: the downloaded archive is damaged (%s). It was deleted: run install.bat again." % (name, e))
    except (OSError, subprocess.TimeoutExpired) as e:
        rmtree(tmp)
        if getattr(e, "errno", None) == errno.ENOSPC:
            raise Fail("the disk is full: free a few GB and run install.bat again.")
        raise Fail("%s could not be unpacked (%s). An antivirus program may be blocking it: allow the folder %s "
                   "and run install.bat again." % (name, e, TC))
    if not (tmp / t["check"]).exists():
        rmtree(tmp)
        raise Fail("%s: %s is missing after unpacking. An antivirus program may have removed it: allow the folder "
                   "%s and run install.bat again." % (name, t["check"], TC))
    os.replace(tmp, home)
    stamp.write_text(t["sha256"])
    return home / t.get("root", "")


def check_compiler(w64):
    try:
        r = subprocess.run([str(w64 / "bin" / "gcc.exe"), "--version"], capture_output=True, text=True, timeout=120)
        ok = r.returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        ok = False
    if not ok:
        (TC / "w64devkit" / ".installed").unlink(missing_ok=True)  # unpacked again next run
        raise Fail("the compiler does not run. An antivirus program may be blocking it: allow the folder %s and "
                   "run install.bat again." % TC)


# ---------------------------------------------------------------------------------------------- assets
def gather_misplaced_packs():
    """A pack extracted into a sub-folder (e.g. <repo>\\fe2_assets\\data\\...) or into data\\ itself
    (data\\data\\...): move its data tree into place. Same drive, so moves are instant."""
    moved = []
    for man in list(ROOT.glob("*/data/manifests/*.json")) + list(ROOT.glob("*/*/data/manifests/*.json")):
        top = man.parent.parent  # .../data
        first = top.relative_to(ROOT).parts[0]
        if top == DATA or first in (".toolchain", "src", ".git"):
            continue
        try:
            for p in sorted(top.rglob("*")):
                if p.is_file():
                    dst = DATA / p.relative_to(top)
                    dst.parent.mkdir(parents=True, exist_ok=True)
                    if dst.exists():
                        dst.unlink()
                    os.replace(p, dst)
        except OSError as e:
            raise Fail("could not move the asset pack from %s into data\\ (%s). Close programs that use these files "
                       "(Explorer windows, the archive program) and run install.bat again." % (rel(top), e))
        for d in sorted((d for d in top.rglob("*") if d.is_dir()), key=lambda d: -len(d.parts)):
            try:
                d.rmdir()
            except OSError:
                pass
        for d in (top, top.parent):
            if d != ROOT and d != DATA:
                try:
                    d.rmdir()
                except OSError:
                    pass
        moved.append(rel(top))
    if moved:
        say("  moved asset packs into data\\ from:", ", ".join(moved))


def unextracted_archives():
    out = []
    for d in (ROOT, DATA):
        if d.is_dir():
            for p in d.iterdir():
                if p.is_file() and re.search(r"\.(7z|zip|rar|tar|gz|zst|00\d)$", p.name, re.I):
                    out.append(p)
    return out


def get_it(game):
    """' Download: <link>' for the game's current pack, if the repository has one"""
    pins = PACKS.get(game, [])
    url = pins[-1].get("url") if pins else None
    return ("\n      Download: %s" % url) if url else ""


def check_archive(game, arc):
    """a pack archive still in the folder, checked against the published one before the player extracts it"""
    pins = PACKS.get(game, [])
    size = arc.stat().st_size
    pin = next((e for e in pins if e["archive_size"] == size), None)
    if not pin:
        if any(e["archive"].lower() == arc.name.lower() for e in pins):
            return "%s is incomplete (%.2f GB, the published file has %.2f GB): download it again.%s" % (
                rel(arc), size / 2 ** 30, pins[-1]["archive_size"] / 2 ** 30, get_it(game))
        return None  # not a pack of this game
    say("  checking %s against the published pack (%.0f GB) ..." % (rel(arc), size / 1e9))
    if sha256_file(arc) != pin["archive_sha256"]:
        return "%s is damaged or not the published file (its checksum differs): download it again.%s" % (
            rel(arc), get_it(game))
    return ("%s is the published pack and is fine, but not extracted yet: extract it into this folder (its data "
            "folder merges with data\\), then run install.bat again." % rel(arc))


def pin_check(game, man_path, man):
    """the extracted pack must be a published one (packs.json): its manifest's SHA-256 is pinned in this repository,
    and the manifest holds every file's SHA-256 (the chain the build inputs are checked along)"""
    pins = PACKS.get(game, [])
    if not pins:
        return None  # a repository without published packs (development): sizes and hashes only
    h = sha256_file(man_path)
    if any(e["manifest_sha256"] == h for e in pins):
        return None
    have, want = str(man.get("version", "?")), max(e["version"] for e in pins)
    known = ", ".join(e["version"] for e in pins)
    if have[:10] < want[:10]:
        return ("this pack (version %s) is older than this installer needs (%s): download the current pack and "
                "replace the old one with it (README.md, section Updating).%s" % (have, known, get_it(game)))
    if have[:10] > want[:10]:
        return ("this pack (version %s) is newer than this copy of the repository knows (%s): update the "
                "repository (git pull, or download the ZIP again) and run install.bat again." % (have, known))
    return ("this pack's file list differs from the published pack %s: the pack was modified, or comes from "
            "somewhere else. Download the pack again from the official link and extract it over this one.%s" % (
                known, get_it(game)))


def check_assets(game, verify):
    man_path = DATA / "manifests" / (game + ".json")
    arcs = [a for a in unextracted_archives() if a.suffix.lower() == ".7z" or re.search(r"\.7z\.00\d$", a.name, re.I)]
    if not man_path.exists():
        for a in arcs:
            msg = check_archive(game, a)
            if msg:
                return msg
        hint = ""
        if arcs:
            hint = (" The archive %s is still packed: extract it into this folder (its contents go here, the "
                    "archive's data folder merges with data\\)." % ", ".join(rel(a) for a in arcs))
        elif (DATA / game).is_dir():
            hint = " data\\%s exists but data\\manifests\\%s.json does not: extract the whole pack again." % (game, game)
        return "no asset pack found." + hint + get_it(game)
    try:
        man = json.loads(man_path.read_text(encoding="utf-8"))
        files = man["files"]
    except (ValueError, KeyError, OSError):
        return "data\\manifests\\%s.json is damaged: extract the whole pack again." % game
    prob = pin_check(game, man_path, man)
    if prob:
        return prob
    missing, wrong = [], []
    total = sum(e["size"] for e in files.values())
    done = 0
    t0 = time.time()
    for rel_, e in files.items():
        p = DATA / rel_
        # compiled into the game (font_table.h) or linked into it (the icon): always checked byte for byte
        build_input = rel_.startswith("build/") or rel_.startswith(game + "/icons/")
        try:
            if not p.is_file():
                missing.append(rel_)
                continue
            if p.stat().st_size != e["size"]:
                wrong.append(rel_)
                continue
            if build_input and e.get("sha256") and sha256_file(p) != e["sha256"]:
                wrong.append(rel_)
                continue
            if verify and e.get("sha256"):
                if sha256_file(p) != e["sha256"]:
                    wrong.append(rel_)
                done += e["size"]
                if time.time() - t0 > 15:
                    t0 = time.time()
                    say("    checked %d %%" % (100 * done // max(total, 1)))
        except OSError:
            wrong.append(rel_)
    say("  %s pack version %s: %d files, %.1f GB" % (game, man.get("version", "?"), len(files), total / 1e9))
    if missing or wrong:
        lines = ["the asset pack is incomplete: %d files missing, %d damaged or unreadable. Extract the whole "
                 "archive again (and let it overwrite):" % (len(missing), len(wrong))]
        lines += ["      missing " + m for m in missing[:6]] + ["      damaged " + w for w in wrong[:6]]
        if len(missing) + len(wrong) > 12:
            lines.append("      ...")
        return "\n".join(lines)
    return None


# ---------------------------------------------------------------------------------------------- discs
def raw_kind(path):
    """'raw' (2352-byte sectors, what the games need), 'iso' (2048-byte), or None"""
    try:
        with open(path, "rb") as f:
            f.seek(16 * SECTOR)
            s = f.read(SECTOR)
            if s[:12] == b"\x00" + b"\xff" * 10 + b"\x00" and s[24 + 1:24 + 6] == b"CD001":
                return "raw"
            f.seek(16 * 2048)
            if f.read(6)[1:6] == b"CD001":
                return "iso"
    except OSError:
        pass
    return None


def read_text_any(p):
    b = p.read_bytes()
    for enc in ("utf-8-sig", "mbcs", "latin-1"):
        try:
            return b.decode(enc)
        except (UnicodeDecodeError, LookupError):
            pass
    return b.decode("latin-1")


CUE_FILE = re.compile(r'^\s*FILE\s+(?:"([^"]+)"|(\S+))', re.I | re.M)


def cue_files(cue):
    """the files a .cue names (quoted or not), as paths; [] if unreadable"""
    try:
        text = read_text_any(cue)
    except OSError:
        return []
    out = []
    for m in CUE_FILE.finditer(text):
        name = (m.group(1) or m.group(2)).strip()
        p = Path(name) if Path(name).is_absolute() else cue.parent / name
        if not p.exists():  # names in another letter case, or a path from the dumper's PC: look next to the .cue
            alt = [q for q in cue.parent.iterdir() if q.name.lower() == Path(name).name.lower()]
            if alt:
                p = alt[0]
        out.append(p)
    return out


def identify(path, image):
    """-> (game, disc number, problem or None); image = the data track's file"""
    cut = "the image is incomplete (shorter than its own file table): copy or dump it again"
    try:
        d = Disc(image)
        fs = d.files()
        # the image must hold every file of the disc (a cut-off copy or download would fail later in the game)
        end = max(lba + (size + 2047) // 2048 for lba, size in fs.values())
        if Path(image).stat().st_size // SECTOR < end:
            return None, None, cut
        cnf = d.file("/SYSTEM.CNF").decode("ascii", "replace")
    except PermissionError:
        return None, None, "cannot be read: another program has it open (an emulator, a mounting tool). Close it."
    except Exception:
        return None, None, "not a PlayStation disc image, or a damaged one"
    m = re.search(r"BOOT\s*=\s*cdrom:\\*([^;\s]+)", cnf, re.I)
    if not m:
        return None, None, "not a PlayStation game disc"
    boot = "/" + m.group(1).upper().replace("\\", "/")
    serial = boot.strip("/").replace("_", "-").replace(".", "")
    try:
        for game, g in GAMES.items():
            for n, want in g["discs"].items():
                if want["serial"] != serial:
                    continue
                for f, sha in want["files"].items():
                    if f not in fs or hashlib.sha1(d.file(f)).hexdigest() != sha:
                        return game, int(n), ("a different version of %s disc %s (%s): the port needs %s" % (
                            g["title"], n, serial, g["version"]))
                return game, int(n), None
    except Exception:
        return None, None, "a damaged disc image (its files cannot be read)"
    known = ", ".join("%s (%s)" % (g["title"], g["version"]) for g in GAMES.values())
    return None, None, "a disc of another game or version (%s); supported: %s" % (serial, known)


MARK = "REM written by the installer (install.bat) from "


def is_ours(cue):
    try:
        with open(cue, "rb") as f:
            return f.read(len(MARK)) == MARK.encode()
    except OSError:
        return False


def scan_discs():
    """-> {game: {n: (image as given, data-track file)}}, [problems], [notes]"""
    found, problems, notes = {}, [], []
    DISCS.mkdir(exist_ok=True)
    files = []
    for p in DISCS.rglob("*"):
        try:
            if p.is_file():
                files.append(p)
        except OSError:
            pass
    cues = [p for p in files if p.suffix.lower() == ".cue" and not is_ours(p)]
    used = set()
    images = []  # (path as the player gave it, the data track's file)
    for c in cues:
        bins = cue_files(c)
        if not bins:
            notes.append("%s: an empty or unreadable .cue file, skipped" % rel(c))
            continue
        lost = [b for b in bins if not b.is_file()]
        if lost:
            notes.append("%s names %s, which is not there (the .bin files are checked on their own)" % (
                rel(c), ", ".join(b.name for b in lost)))
            continue
        used.update(b.resolve() for b in bins)
        images.append((c, bins[0]))
    for p in files:
        ext = p.suffix.lower()
        try:
            if ext in (".bin", ".img", ".iso") and p.resolve() not in used:
                k = raw_kind(p)
                if k == "raw":
                    images.append((p, p))
                elif k == "iso":
                    problems.append("%s: a 2048-byte .iso image. It lacks the games' audio and movie sectors: make a "
                                    "BIN/CUE (raw, 2352-byte sectors) image of the disc instead" % rel(p))
                elif ext != ".bin" or p.stat().st_size > 50 << 20:
                    problems.append("%s: not a disc image this installer can read (or a damaged one)" % rel(p))
            elif ext in (".chd", ".pbp", ".ecm", ".cso", ".mds", ".mdf", ".nrg", ".cdi", ".ccd", ".toc"):
                if ext == ".ccd" and p.with_suffix(".img").exists():
                    continue  # CloneCD: its .img is the raw image, read above
                problems.append("%s: %s images are not supported: convert it to BIN/CUE first" % (rel(p), ext))
            elif ext in (".zip", ".7z", ".rar", ".gz"):
                problems.append("%s: an archive: extract the disc images from it first" % rel(p))
        except OSError as e:
            problems.append("%s: cannot be read (%s)" % (rel(p), e.strerror or e))
    for given, image in images:
        game, n, prob = identify(given, image)
        if prob:
            problems.append("%s: %s" % (rel(given), prob))
            continue
        if n in found.setdefault(game, {}):
            notes.append("%s disc %d found twice, using %s" % (GAMES[game]["title"], n, rel(found[game][n][0])))
            continue
        found[game][n] = (given, image)
    return found, problems, notes


def stray_images():
    """disc images put somewhere else in the folder (not under discs\\)"""
    out = []
    for p in list(ROOT.glob("*")) + list(ROOT.glob("*/*")):
        try:
            if p.is_file() and p.suffix.lower() in (".cue", ".bin", ".img", ".iso", ".chd") and \
                    p.resolve().parts[:len(DISCS.parts)] != DISCS.parts:
                out.append(p)
        except OSError:
            pass
    return out


def write_cues(game, images):
    """discs/<game>/<Title> (Disc N).cue for each disc, pointing at the player's own files (relative paths): the
    game finds discs 2-4 by that name. The player's own files are never changed."""
    g = GAMES[game]
    out = DISCS / game
    out.mkdir(parents=True, exist_ok=True)
    # one name pattern for all discs (the game finds disc N by replacing "(Disc 1)"); a file of the player's that
    # happens to have that name is never overwritten: then all get the "[installer]" suffix
    pattern = "%s (Disc %%d).cue" % g["cue_name"]
    if any((out / (pattern % n)).exists() and not is_ours(out / (pattern % n)) for n in images):
        pattern = "%s [installer] (Disc %%d).cue" % g["cue_name"]
    cues = {}
    for n, (given, image) in sorted(images.items()):
        dst = out / (pattern % n)
        if given.suffix.lower() == ".cue":
            text = read_text_any(given)
            bins = iter(cue_files(given))
            # every FILE line rewritten with the found file, quoted, relative to the new .cue (the game reads the
            # first FILE "..." line and joins it to the .cue's folder)
            text = CUE_FILE.sub(lambda m: 'FILE "%s"' % os.path.relpath(next(bins).resolve(), out), text)
        else:
            text = 'FILE "%s" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n' % os.path.relpath(
                image.resolve(), out)
        dst.write_text(MARK + rel(given) + "\n" + text, encoding="utf-8")
        cues[n] = dst
    return cues


# ---------------------------------------------------------------------------------------------- build
def tree_hash(paths):
    h = hashlib.sha1()
    for p in sorted(paths):
        h.update(p.name.encode())
        h.update(p.read_bytes())
    return h.hexdigest()


def build(game, cues, rebuild, w64, sdl):
    g = GAMES[game]
    sfx = "" if game == "fe2" else "_" + game
    gen = SRC / ("gen" + sfx)
    bld = SRC / ("build" + sfx)
    # what the recompiler's output depends on (an update of this repository may change it) and what the objects
    # depend on besides the sources (the compiler)
    recomp_id = tree_hash(list((SRC / "tools" / "recomp").glob("*.py")) + [SRC / "config" / (game + ".json")])
    tools_id = TOOLS["w64devkit"]["sha256"] + TOOLS["sdl2"]["sha256"]
    rstamp, tstamp = gen / ".recomp_id", bld / ".toolchain_id"
    if not rebuild and bld.exists() and (not tstamp.exists() or tstamp.read_text() != tools_id):
        rebuild_objs = True
    else:
        rebuild_objs = rebuild
    if rebuild:
        rmtree(gen)
    if rebuild_objs:
        rmtree(bld)
    # build inputs from the asset pack: the font atlas table and the exe icon
    for src, dst in ((DATA / "build" / "font_table.h", SRC / "runtime" / "font_table.h"),
                     (DATA / game / "icons" / (game + ".ico"), SRC / "icons" / (game + ".ico"))):
        if not src.exists():
            raise Fail("%s is missing from the asset pack: extract the whole pack again." % rel(src))
        shutil.copy2(src, dst)
    info = gen / "build_info.json"
    try:
        regen = json.loads(info.read_text()).get("exe_sha1") != g["exe_sha1"] or rstamp.read_text() != recomp_id
    except (OSError, ValueError):
        regen = True
    if free_gb() < 1.0:
        raise Fail("not enough free disk space to build (%.1f GB free, 1 GB needed)" % free_gb())
    env = dict(os.environ)
    env["PATH"] = str(w64 / "bin") + os.pathsep + env.get("PATH", "")
    env["FE_GCC"] = str(w64 / "bin" / "gcc.exe")
    env["FE_WINDRES"] = str(w64 / "bin" / "windres.exe")
    env["FE_SDL2"] = str(sdl / TOOLS["sdl2"]["prefix"])
    # parallel compiler processes: one per core, at most one per ~1 GB of free memory (a big generated file at -O2)
    ram = ram_gb()
    jobs = max(1, min(os.cpu_count() or 1, int(ram) if ram else 4))
    env["FE_JOBS"] = str(jobs)
    cmd = [sys.executable, "-B", str(SRC / "tools" / "build.py"), "--config", str(SRC / "config" / (game + ".json"))]
    for n in sorted(cues):
        cmd += ["--disc", str(cues[n])]
    if regen:
        cmd.append("--regen")
    say("  recompiling %s from your discs and compiling (%d parallel jobs; a few minutes the first time) ..." % (
        g["title"], jobs))
    t0 = time.time()
    try:
        p = subprocess.Popen(cmd, cwd=str(SRC), env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                             errors="replace")
    except OSError as e:
        raise Fail("the build could not start (%s)" % e)
    tail = []
    for line in p.stdout:
        if _log:
            _log.write("    | " + line)
        tail = (tail + [line.rstrip()])[-30:]
    if p.wait():
        text = "\n".join(tail)
        hint = ""
        if re.search(r"out of memory|cannot allocate|virtual memory exhausted|MemoryError", text, re.I):
            hint = "The PC ran out of memory: close other programs and run install.bat again."
        elif re.search(r"No space left|disk full", text, re.I):
            hint = "The disk is full: free a few GB and run install.bat again."
        elif re.search(r"Permission denied|Access is denied", text, re.I):
            hint = ("A file was locked: close the game if it is running; an antivirus program may also block the "
                    "compiler (allow the folder %s)." % ROOT)
        raise Fail("building %s failed. %s\n  The last lines of the build (all of it is in install.log):\n      %s"
                   % (g["title"], hint, "\n      ".join(tail[-12:])))
    exe = bld / (game + "port.exe")
    if not exe.exists():
        raise Fail("building %s: %s is missing (an antivirus program may have removed it: allow the folder %s)"
                   % (g["title"], rel(exe), ROOT))
    gen.mkdir(exist_ok=True)
    rstamp.write_text(recomp_id)
    tstamp.write_text(tools_id)
    say("  built in %.0f s" % (time.time() - t0))
    return bld


def install_game(game, bld, ffmpeg):
    g = GAMES[game]
    home = ROOT / g["folder"]
    try:
        home.mkdir(exist_ok=True)
        for f in (game + "port.exe", "SDL2.dll"):
            shutil.copy2(bld / f, home / f)
        (home / "memcards").mkdir(exist_ok=True)
        (home / "states").mkdir(exist_ok=True)
        shutil.copy2(SRC / "SETTINGS.md", home / "SETTINGS.txt")
        # shared by both games: the movie decoder and the fonts (the game looks for both in the data folder)
        if not (DATA / "ffmpeg.exe").exists() or (DATA / "ffmpeg.exe").stat().st_size != ffmpeg.stat().st_size:
            shutil.copy2(ffmpeg, DATA / "ffmpeg.exe")
        (DATA / "fonts").mkdir(exist_ok=True)
        for f in (SRC / "fonts").iterdir():
            if f.is_file():
                shutil.copy2(f, DATA / "fonts" / f.name)
    except PermissionError:
        raise Fail("cannot replace the files in %s\\: %s is probably running. Close the game and run install.bat "
                   "again." % (g["folder"], g["title"]))
    return home / (game + "port.exe")


def test_start(game, exe, cue1):
    """a short start without a window: boots the game for ~20 s of game time"""
    logf = TC / ("test_%s.log" % game)
    try:
        r = subprocess.run([str(exe), str(cue1), "--headless", "--fast", "--frames", "1200", "--mute",
                            "--log", str(logf)], cwd=str(exe.parent), capture_output=True, text=True, timeout=300,
                           errors="replace")
    except subprocess.TimeoutExpired:
        return "the test start did not finish in 5 minutes"
    except OSError as e:
        return "the game could not be started (%s). An antivirus program may be blocking it: allow the folder %s" % (
            e, ROOT)
    if r.returncode:
        tail = (r.stdout + r.stderr).strip().splitlines()[-6:]
        try:
            tail += logf.read_text(errors="replace").strip().splitlines()[-6:]
        except OSError:
            pass
        return "the test start ended with code %d:\n      %s" % (r.returncode, "\n      ".join(tail))
    return None


def shortcuts(game, exe, cue1):
    """Desktop + Start menu shortcuts; a failure here is a note (the game is installed and runs without them)"""
    g = GAMES[game]
    ps = r"""
$ErrorActionPreference = 'Stop'
$s = New-Object -ComObject WScript.Shell
$dirs = @([Environment]::GetFolderPath('Desktop'), (Join-Path ([Environment]::GetFolderPath('Programs')) $env:FE_MENU))
New-Item -ItemType Directory -Force -Path $dirs[1] | Out-Null
foreach ($d in $dirs) {
  if (-not $d -or -not (Test-Path -LiteralPath $d)) { continue }
  $l = $s.CreateShortcut((Join-Path $d ($env:FE_NAME + '.lnk')))
  $l.TargetPath = $env:FE_EXE
  $l.Arguments = '"' + $env:FE_CUE + '"'
  $l.WorkingDirectory = Split-Path $env:FE_EXE
  $l.IconLocation = $env:FE_EXE + ',0'
  $l.Description = $env:FE_NAME
  $l.Save()
  Write-Output (Join-Path $d ($env:FE_NAME + '.lnk'))
}
"""
    env = dict(os.environ, FE_EXE=str(exe), FE_CUE=str(cue1), FE_NAME=g["shortcut"], FE_MENU="Fear Effect (native port)")
    try:
        r = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", ps], env=env,
                           capture_output=True, text=True, errors="replace", timeout=120)
        made = [x.strip() for x in r.stdout.split("\n") if x.strip()]
        err = (r.stderr or "").strip() if r.returncode else ""
    except (OSError, subprocess.TimeoutExpired) as e:
        made, err = [], str(e)
    for m in made:
        say("  shortcut:", m)
    if err or not made:
        say("  note: the shortcuts could not be created (%s)." % (err.splitlines()[-1] if err else "no folder"))
        say('  Start the game with %s\\%s, or make a shortcut to it yourself.' % (g["folder"], exe.name))


# ---------------------------------------------------------------------------------------------- main
def take_lock():
    TC.mkdir(exist_ok=True)
    lock = TC / "install.lock"
    try:
        pid = int(lock.read_text().strip())
        if pid != os.getpid() and pid_alive(pid):
            raise Fail("install.bat is already running in another window. Wait for it to finish (or close it), then "
                       "run install.bat again if needed.")
    except (OSError, ValueError):
        pass
    lock.write_text(str(os.getpid()))
    return lock


def main():
    global _log
    ap = argparse.ArgumentParser(prog="install.bat")
    ap.add_argument("--game", choices=sorted(GAMES))
    ap.add_argument("--verify", action="store_true")
    ap.add_argument("--rebuild", action="store_true")
    ap.add_argument("--no-shortcuts", action="store_true")
    ap.add_argument("--no-test", action="store_true")
    args = ap.parse_args()
    try:
        _log = open(LOG, "a", encoding="utf-8")
        _log.write("\n\n######## %s  %s\n" % (time.strftime("%Y-%m-%d %H:%M:%S"), " ".join(sys.argv[1:])))
    except OSError:
        _log = None  # the log is a convenience; install.bat has checked that the folder is writable
    ver = (SRC / "VERSION.txt").read_text().strip() if (SRC / "VERSION.txt").exists() else "?"
    say("Fear Effect native port: installer (version %s)" % ver)
    say("folder:", ROOT)
    try:
        str(ROOT).encode("ascii")
    except UnicodeEncodeError:
        raise Fail("the folder's path has letters outside A-Z, which the compiler and the game cannot open. Move the "
                   "whole folder to a plain path such as C:\\Games\\FearEffect and run install.bat again.")
    if len(str(ROOT)) > 150:
        raise Fail("the folder's path is too long (%d characters): move the folder closer to the drive's root, "
                   "e.g. C:\\Games\\FearEffect" % len(str(ROOT)))
    if "onedrive" in str(ROOT).lower():
        say("  note: this folder is inside OneDrive. If OneDrive keeps files online-only, the game stutters or "
            "cannot read them: choose 'Always keep on this device' for the folder, or move it out of OneDrive.")
    lock = take_lock()
    try:
        return run(args)
    finally:
        try:
            lock.unlink()
        except OSError:
            pass


def run(args):
    step("1/5 Your disc images (discs\\)")
    found, problems, notes = scan_discs()
    for n in notes:
        say("  note:", n)
    for p in problems:
        say("  problem:", p)
    ready = {}
    for game, g in GAMES.items():
        if args.game and game != args.game:
            continue
        have = found.get(game, {})
        if not have:
            say("  %s: no discs found" % g["title"])
            continue
        missing = [n for n in range(1, len(g["discs"]) + 1) if n not in have]
        for n in sorted(have):
            say("  %s disc %d: %s" % (g["title"], n, rel(have[n][0])))
        if missing:
            say("  %s: disc %s missing (all %d discs are needed)" % (g["title"], ", ".join(map(str, missing)),
                                                                       len(g["discs"])))
            continue
        ready[game] = have
    if not ready:
        stray = stray_images()
        hint = ""
        if stray:
            hint = ("\n  Disc images found outside discs\\: %s. Move them into discs\\fe1 or discs\\fe2." %
                    ", ".join(rel(s) for s in stray[:4]))
        raise Fail("no complete set of supported discs in discs\\. Copy your own disc images (BIN/CUE, all four "
                   "discs) into discs\\fe1 (Fear Effect) or discs\\fe2 (Fear Effect 2: Retro Helix) and run "
                   "install.bat again." + hint)

    step("2/5 Remastered assets (data\\)")
    gather_misplaced_packs()
    for game in list(ready):
        prob = check_assets(game, args.verify)
        if prob:
            say("  %s: %s" % (GAMES[game]["title"], prob))
            say("  %s is skipped until its asset pack is in order (see the line above)" % GAMES[game]["title"])
            del ready[game]
    if not ready:
        raise Fail("no game has both its discs and its asset pack (see above).")

    step("3/5 Build tools (.toolchain\\, downloaded once)")
    if not all((TC / n / ".installed").exists() for n in ("w64devkit", "sdl2", "ffmpeg")) and free_gb() < NEED_FREE_GB:
        raise Fail("not enough free disk space: %.1f GB free on this drive, %.0f GB needed for the build tools and "
                   "the build." % (free_gb(), NEED_FREE_GB))
    w64 = ensure_tool("w64devkit")
    sdl = ensure_tool("sdl2")
    ff = ensure_tool("ffmpeg") / TOOLS["ffmpeg"]["check"]
    check_compiler(w64)
    say("  compiler, SDL2 and ffmpeg ready")

    installed, failed = {}, {}
    step("4/5 Building the games from your discs")
    for game, have in ready.items():
        cues = write_cues(game, have)
        bld = build(game, cues, args.rebuild, w64, sdl)
        exe = install_game(game, bld, ff)
        say("  installed:", rel(exe))
        if not args.no_test:
            prob = test_start(game, exe, cues[1])
            if prob:
                say("  %s was built but its test start failed: %s" % (GAMES[game]["title"], prob))
                failed[game] = prob
                continue
            say("  test start: ok")
        installed[game] = (exe, cues[1])

    step("5/5 Shortcuts")
    for game, (exe, cue1) in installed.items():
        if args.no_shortcuts:
            say("  skipped (--no-shortcuts). Start the game with:", '"%s" "%s"' % (exe, cue1))
        else:
            shortcuts(game, exe, cue1)
    if failed:
        raise Fail("%s does not start (see above). Please report it with install.log." %
                   ", ".join(GAMES[g]["title"] for g in failed))
    say("")
    say("Done: %s. Start the game from its shortcut on the Desktop or in the Start menu." %
        ", ".join(GAMES[g]["title"] for g in installed))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Fail as e:
        say("")
        say("INSTALL STOPPED: " + str(e))
        say("(the full log is in install.log)")
        sys.exit(1)
    except KeyboardInterrupt:
        say("")
        say("INSTALL STOPPED: interrupted. Run install.bat again to continue; finished steps are kept.")
        sys.exit(1)
    except OSError as e:
        say("")
        if e.errno == errno.ENOSPC:
            say("INSTALL STOPPED: the disk is full. Free a few GB and run install.bat again.")
        elif isinstance(e, PermissionError):
            say("INSTALL STOPPED: access denied to %s. Close programs that use it (the game, Explorer windows); an "
                "antivirus program may also block it." % (e.filename or "a file"))
        else:
            say("INSTALL STOPPED: %s (%s)" % (e.strerror or e, e.filename or ""))
        say("(the full log is in install.log)")
        sys.exit(1)
    except Exception:
        import traceback
        say("")
        say("INSTALL STOPPED by an unexpected error:\n" + traceback.format_exc())
        say("(please report it with install.log)")
        sys.exit(2)
