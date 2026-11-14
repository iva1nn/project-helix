"""Linux / SteamOS: the games in the Steam library (install.py, step 5) as non-Steam games of every Steam account,
with SteamGridDB artwork (portrait, wide, hero, logo; steam_art.json) in the account's grid folder.

Made safe after 14 Nov 2026 (a first version's shortcuts.vdf, written while Steam was closed, was emptied by Steam on
its next start, taking the player's other non-Steam entries with it):
- the file is written exactly as Steam writes it (key names and order of a file Steam wrote: "appname" lower case,
  "sortas"), only after Steam has exited, with a timestamped backup of every account's file;
- after Steam starts again the installer waits and reads the file back: if any entry the player had, or one of ours,
  is missing, Steam is closed again, every backup is put back, and the player is told to use Steam's own
  "Add a Non-Steam Game" dialog instead (it lists the desktop launchers).
Not in Game Mode: there Steam is the session itself and cannot be closed and started again from a script."""
import os, shutil, struct, subprocess, sys, time, zlib
from pathlib import Path

HOME = Path.home()
CHECK_WAIT = 75  # seconds after Steam's start before the file is read back (Steam rewrote it after ~19 s on 14 Nov)


# ------------------------------------------------------------------ binary VDF (shortcuts.vdf)
def vdf_read(b, i=0):
    """-> (dict, next index); maps end with 0x08; types 0 map, 1 string, 2 int32"""
    out = {}
    while i < len(b):
        t = b[i]
        i += 1
        if t == 0x08:
            return out, i
        j = b.index(b"\x00", i)
        key = b[i:j].decode("utf-8", "replace")
        i = j + 1
        if t == 0x00:
            out[key], i = vdf_read(b, i)
        elif t == 0x01:
            j = b.index(b"\x00", i)
            out[key] = b[i:j].decode("utf-8", "replace")
            i = j + 1
        elif t == 0x02:
            out[key] = struct.unpack_from("<i", b, i)[0]
            i += 4
        else:
            raise ValueError("unknown VDF type %d" % t)
    return out, i


def vdf_write(d):
    out = bytearray()
    for k, v in d.items():
        kb = k.encode("utf-8") + b"\x00"
        if isinstance(v, dict):
            out += b"\x00" + kb + vdf_write(v) + b"\x08"
        elif isinstance(v, int):
            out += b"\x02" + kb + struct.pack("<i", v)
        else:
            out += b"\x01" + kb + str(v).encode("utf-8") + b"\x00"
    return bytes(out)


def shortcuts_file(entries):
    return b"\x00shortcuts\x00" + vdf_write({str(i): e for i, e in enumerate(entries)}) + b"\x08\x08"


def read_entries(f):
    if not f.exists():
        return []
    d, _ = vdf_read(f.read_bytes())
    return list(d.get("shortcuts", {}).values())


def _name(e):
    return e.get("appname", e.get("AppName", ""))


def _exe(e):
    return e.get("Exe", e.get("exe", ""))


def appid_unsigned(exe_field, name):
    """the id Steam gives a non-Steam game; its grid pictures are named after it"""
    return (zlib.crc32((exe_field + name).encode("utf-8")) & 0xFFFFFFFF) | 0x80000000


def steam_entry(exe, cue1, name, icon):
    """one shortcut, with the keys in the order Steam itself writes them"""
    exe_field = '"%s"' % exe
    appid = appid_unsigned(exe_field, name)
    entry = {
        "appid": struct.unpack("<i", struct.pack("<I", appid))[0], "appname": name, "Exe": exe_field,
        "StartDir": '"%s"' % exe.parent, "icon": str(icon) if icon else "", "ShortcutPath": "",
        "LaunchOptions": '"%s"' % cue1, "IsHidden": 0, "AllowDesktopConfig": 1, "AllowOverlay": 1, "OpenVR": 0,
        "Devkit": 0, "DevkitGameID": "", "DevkitOverrideAppID": 0, "LastPlayTime": 0, "FlatpakAppID": "",
        "sortas": "", "tags": {}}
    return entry, appid


# ------------------------------------------------------------------ accounts and the client
def steam_userdirs():
    roots = [HOME / ".local" / "share" / "Steam", HOME / ".steam" / "steam",
             HOME / ".var" / "app" / "com.valvesoftware.Steam" / ".local" / "share" / "Steam"]
    seen, out = set(), []
    for r in roots:
        try:
            r = r.resolve()
        except OSError:
            continue
        if r in seen or not (r / "userdata").is_dir():
            continue
        seen.add(r)
        for u in sorted((r / "userdata").iterdir()):
            if u.is_dir() and u.name.isdigit() and u.name != "0":
                out.append(u)
    return out


def _procs():
    names = set()
    for p in Path("/proc").iterdir():
        if p.name.isdigit():
            try:
                names.add((p / "comm").read_text().strip())
            except OSError:
                pass
    return names


def steam_running():
    return bool(_procs() & {"steam", "steamwebhelper"})


def game_mode():
    """SteamOS's Game Mode: gamescope runs and no desktop (plasmashell) does"""
    p = _procs()
    return bool(p & {"gamescope", "gamescope-wl"}) and "plasmashell" not in p


def close_steam():
    try:
        subprocess.run(["steam", "-shutdown"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=30)
    except (OSError, subprocess.TimeoutExpired):
        return False
    for _ in range(90):
        if not steam_running():
            time.sleep(3)  # Steam writes its files while it exits
            return not steam_running()
        time.sleep(1)
    return False


def start_steam():
    if not (os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY")):
        return False
    try:
        subprocess.Popen(["steam"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL,
                         start_new_session=True)
        return True
    except OSError:
        return False


# ------------------------------------------------------------------ the library step
def steam_add(games, say, fetch_art, assume_yes=False):
    """games: [(game info, exe, cue1, icon path or None, {kind: art entry})]; fetch_art(entry) -> local file"""
    users = steam_userdirs()
    if not users:
        say("  Steam: no Steam account on this system; the games have desktop launchers only")
        return
    if game_mode():
        say("  Steam: skipped in Game Mode (Steam cannot be closed here). Switch to Desktop Mode and run install.sh "
            "again, or add the games with Steam's 'Add a Non-Steam Game'.")
        return
    # the artwork first: a download problem must not leave Steam closed
    arts = []
    for g, exe, cue1, icon, art in games:
        files = {}
        for kind, entry in art.items():
            try:
                files[kind] = fetch_art(entry)
            except Exception as e:  # artwork is optional: the game is added without it
                say("  note: %s %s artwork not downloaded (%s)" % (g["shortcut"], kind, e))
        arts.append(files)
    was_running = steam_running()
    if was_running:
        go = assume_yes
        if not go and sys.stdin and sys.stdin.isatty():
            try:
                go = input("  Steam must be closed for a moment to add the games to your library. Close Steam now? "
                           "[Y/n] ").strip().lower() in ("", "y", "yes")
            except EOFError:
                go = False
        if not go:
            say("  Steam: skipped. Run install.sh again to add the games, or use Steam's 'Add a Non-Steam Game'.")
            return
        say("  closing Steam ...")
        if not close_steam():
            say("  Steam: it did not close; skipped. Close Steam and run install.sh again to add the games.")
            return
    stamp = time.strftime("%Y%m%d-%H%M%S")
    plans = []  # (file, backup or None, entries written)
    names = {"portrait": "%dp", "wide": "%d", "hero": "%d_hero", "logo": "%d_logo"}
    for u in users:
        f = u / "config" / "shortcuts.vdf"
        try:
            entries = read_entries(f)
            bak = None
            if f.exists():
                bak = f.with_name("shortcuts.vdf.fe-native-%s" % stamp)
                shutil.copy2(f, bak)
            for (g, exe, cue1, icon, art), files in zip(games, arts):
                entry, appid = steam_entry(exe, cue1, g["shortcut"], icon)
                entries = [e for e in entries if _exe(e) != entry["Exe"] and _name(e) != g["shortcut"]]
                entries.append(entry)
                grid = u / "config" / "grid"
                grid.mkdir(parents=True, exist_ok=True)
                for kind, src in files.items():
                    shutil.copy2(src, grid / ((names[kind] % appid) + Path(src).suffix))
            tmp = f.with_name("shortcuts.vdf.fe_tmp")
            f.parent.mkdir(parents=True, exist_ok=True)
            tmp.write_bytes(shortcuts_file(entries))
            if f.exists():
                shutil.copymode(f, tmp)
            os.replace(tmp, f)
            plans.append((f, bak, entries))
            say("  Steam account %s: %s added (backup: %s)" % (
                u.name, " and ".join(g["shortcut"] for g, *_ in games), bak.name if bak else "none needed"))
        except (OSError, ValueError) as e:
            say("  note: Steam account %s not changed (%s)" % (u.name, e))
    if not start_steam():
        say("  Start Steam: the games are in your library (Non-Steam).")
        return
    say("  Steam started; checking that it keeps the library (about a minute) ...")
    time.sleep(CHECK_WAIT)
    lost = []
    for f, bak, written in plans:
        now = read_entries(f) if f.exists() else []
        have = {(_name(e), _exe(e)) for e in now}
        missing = [_name(e) for e in written if (_name(e), _exe(e)) not in have]
        if missing:
            lost.append((f, bak, missing))
    if not lost:
        say("  Steam kept the library. In Game Mode: Library > Non-Steam.")
        return
    for f, _, missing in lost:
        say("  Steam dropped entries of account %s: %s" % (f.parent.parent.name, ", ".join(missing)))
    say("  putting your Steam library back as it was ...")
    if close_steam():
        for f, bak, _ in lost:
            if bak:
                shutil.copy2(bak, f)
            elif f.exists():
                f.unlink()
        start_steam()
        say("  Steam library restored. Add the games with Steam's 'Add a Non-Steam Game' (Desktop Mode): both are "
            "listed there.")
    else:
        say("  Steam did not close. Restore your library: with Steam closed, copy %s over shortcuts.vdf."
            % ", ".join(str(b) for _, b, _ in lost if b))
