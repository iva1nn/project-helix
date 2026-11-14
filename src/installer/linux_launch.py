"""Linux / SteamOS launchers for the installed games (install.py, step 5): desktop launchers in
~/.local/share/applications (the app menu; Steam's "Add a Non-Steam Game" dialog lists them) and on the Desktop.
The Steam library step is linux_steam.py."""
import struct, subprocess
from pathlib import Path

HOME = Path.home()


def ico_png(ico, dst):
    """the largest PNG-compressed image of an .ico (the pack's exe icon), written to dst; False if there is none"""
    try:
        b = Path(ico).read_bytes()
        n = struct.unpack_from("<H", b, 4)[0]
        best = None
        for i in range(n):
            w, h, _, _, _, _, size, off = struct.unpack_from("<BBBBHHII", b, 6 + 16 * i)
            data = b[off:off + size]
            if data[:8] == b"\x89PNG\r\n\x1a\n" and (best is None or (w or 256) > best[0]):
                best = (w or 256, data)
        if not best:
            return False
        Path(dst).write_bytes(best[1])
        return True
    except (OSError, struct.error):
        return False


def _q(s):
    """a .desktop Exec argument: double-quoted, with " ` $ \\ escaped"""
    return '"%s"' % "".join("\\" + c if c in '"`$\\' else c for c in str(s))


def desktop_dir():
    try:
        d = subprocess.run(["xdg-user-dir", "DESKTOP"], capture_output=True, text=True).stdout.strip()
        if d and Path(d).is_dir() and Path(d) != HOME:
            return Path(d)
    except OSError:
        pass
    d = HOME / "Desktop"
    return d if d.is_dir() else None


def launchers(g, exe, cue1, ico, say):
    icon = exe.parent / "icon.png"
    has_icon = ico_png(ico, icon)
    text = "\n".join([
        "[Desktop Entry]", "Type=Application", "Name=%s" % g["shortcut"],
        "Comment=Native PC port, recompiled from your own discs",
        "Exec=%s %s" % (_q(exe), _q(cue1)), "Path=%s" % exe.parent,
        "Icon=%s" % (icon if has_icon else "applications-games"), "Terminal=false", "Categories=Game;", ""])
    slug = "fe-native-" + ("fe2" if "2" in g["folder"] else "fe1")
    targets = [HOME / ".local" / "share" / "applications" / (slug + ".desktop")]
    d = desktop_dir()
    if d:
        targets.append(d / (g["shortcut"] + ".desktop"))
    for t in targets:
        try:
            t.parent.mkdir(parents=True, exist_ok=True)
            t.write_text(text, encoding="utf-8")
            t.chmod(0o755)
            say("  launcher:", t)
        except OSError as e:
            say("  note: could not write the launcher %s (%s)" % (t, e))
