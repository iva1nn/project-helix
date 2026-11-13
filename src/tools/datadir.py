"""The port's data folder for the tools (NOTES.md "Self-contained data folder"): <repo>/data, and from a
git worktree the main checkout's data/ (one ~45 GB copy for all worktrees)."""
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def main_checkout():
    try:
        common = subprocess.run(["git", "rev-parse", "--path-format=absolute", "--git-common-dir"], cwd=ROOT,
                                capture_output=True, text=True, check=True).stdout.strip()
        return Path(common).parent
    except (OSError, subprocess.CalledProcessError):
        return ROOT


def redraw_root():
    """The remaster's redraw toolchain (HYPIR, Qwen, FastBlend, SAM/OWLv2, Hunyuan3D: venvs, weights, scripts, plates),
    moved from F:\\fe2_remaster on 5 Nov 2026 (NOTES.md "Redraw toolchain"): <repo>/third_party/fe2_remaster, from a
    worktree the main checkout's (git-ignored, ~75 GB, one copy). FE_REDRAW_ROOT overrides."""
    import os
    if os.environ.get("FE_REDRAW_ROOT"):
        return Path(os.environ["FE_REDRAW_ROOT"])
    own = ROOT / "third_party" / "fe2_remaster"
    return own if own.is_dir() else main_checkout() / "third_party" / "fe2_remaster"


def default_data():
    own = ROOT / "data"
    return own if own.is_dir() else main_checkout() / "data"
