#!/usr/bin/env bash
# Started by RecompLauncher after install.sh --prepare. A normal install uses the desktop launcher.
here="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)" || exit 1
game=fe2
args=()
while [ $# -gt 0 ]; do
  case "$1" in
    --game)
      game="${2:-}"
      shift 2
      ;;
    --game=*)
      game="${1#--game=}"
      shift
      ;;
    *)
      args+=("$1")
      shift
      ;;
  esac
done

case "$game" in
  fe1) dir="$here/Fear Effect" ;;
  fe2) dir="$here/Fear Effect 2" ;;
  *) echo "Use --game fe1 or --game fe2." >&2; exit 3 ;;
esac

exe="$dir/${game}port"
if [ ! -f "$exe" ]; then exe="$dir/${game}port.exe"; fi
if [ ! -f "$exe" ]; then
  echo "This game has not been built yet. Set it up from its page in RecompLauncher." >&2
  exit 3
fi

cue=""
fallback=""
if [ -d "$here/discs/$game" ]; then
  while IFS= read -r f; do
    [ -n "$fallback" ] || fallback=$f
    if [ -z "$cue" ] && head -n 1 "$f" | grep -q "REM written by the installer"; then
      cue=$f
    fi
  done < <(find "$here/discs/$game" -maxdepth 1 -name '*(Disc 1)*.cue' | sort)
fi
[ -n "$cue" ] || cue=$fallback

cd "$dir" || exit 1
if [ -n "$cue" ]; then
  exec "$exe" "$cue" "${args[@]}"
else
  exec "$exe" "${args[@]}"
fi
