"""Loaded by site.py when this folder is on PYTHONPATH (installer child processes).

Console-subsystem tools (git, gcc, cmd, powershell) otherwise open extra windows
because the setup GUI has no console for them to inherit. CREATE_NO_WINDOW keeps
stdout on pipes so the launcher log still receives every line.
"""
import subprocess
import sys

if sys.platform != "win32":
    pass
else:
    _CF = getattr(subprocess, "CREATE_NO_WINDOW", 0x08000000)
    _OrigPopen = subprocess.Popen

    def _hide(kwargs):
        kwargs = dict(kwargs)
        kwargs["creationflags"] = kwargs.get("creationflags", 0) | _CF
        try:
            si = kwargs.get("startupinfo") or subprocess.STARTUPINFO()
            si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
            si.wShowWindow = 0
            kwargs["startupinfo"] = si
        except Exception:
            pass
        return kwargs

    class Popen(_OrigPopen):
        def __init__(self, *args, **kwargs):
            super().__init__(*args, **_hide(kwargs))

    def _wrap(orig):
        def wrapped(*args, **kwargs):
            return orig(*args, **_hide(kwargs))
        return wrapped

    subprocess.Popen = Popen
    subprocess.run = _wrap(subprocess.run)
    subprocess.call = _wrap(subprocess.call)
    subprocess.check_call = _wrap(subprocess.check_call)
    subprocess.check_output = _wrap(subprocess.check_output)
