#!/usr/bin/env bash
# Fear Effect native port: the installer for SteamOS (Steam Deck) and other x86_64 Linux systems.
# Put your disc images into discs/ and extract the asset pack(s) into this folder, then run this file
# (double-click it in the file manager, or ./install.sh in a terminal). Options: ./install.sh --help
cd "$(dirname "$(readlink -f "$0")")" || exit 1

# started from a file manager (no terminal): open one, so the progress and the questions can be seen
if [ ! -t 0 ] && [ -z "$FE_IN_TERMINAL" ] && { [ -n "$DISPLAY" ] || [ -n "$WAYLAND_DISPLAY" ]; }; then
  export FE_IN_TERMINAL=1
  if command -v konsole >/dev/null; then exec konsole -e "$0" "$@"; fi
  if command -v gnome-terminal >/dev/null; then exec gnome-terminal -- "$0" "$@"; fi
  if command -v xterm >/dev/null; then exec xterm -e "$0" "$@"; fi
fi

rc=1
if [ "$(uname -m)" != "x86_64" ]; then
  echo "INSTALL STOPPED: this port needs an x86_64 system (Steam Deck or a PC)."
elif ! command -v python3 >/dev/null; then
  echo "INSTALL STOPPED: python3 is missing. SteamOS has it; on other Linux systems install the python3 package."
else
  python3 -B src/installer/install.py "$@"
  rc=$?
fi
if [ -n "$FE_IN_TERMINAL" ] && [ -z "$FE_NO_PAUSE" ]; then
  echo
  read -r -p "Press Enter to close this window. " _
fi
exit $rc
