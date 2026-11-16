#!/usr/bin/env bash
# Fear Effect setup window. ./setup.sh
cd "$(dirname "$(readlink -f "$0")")" || exit 1
if ! command -v python3 >/dev/null; then
  echo "python3 is needed to open the setup window." >&2
  exit 1
fi
for a in "$@"; do
  case "$a" in
    --self-test|--console) exec python3 -B setup/helix_setup.py "$@" ;;
  esac
done
# no terminal: the Chromium --app window is the UI
nohup python3 -B setup/helix_setup.py "$@" >/dev/null 2>&1 &
disown
exit 0
