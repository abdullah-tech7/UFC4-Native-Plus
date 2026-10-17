#!/usr/bin/env bash
# UFC 3 native port (Linux release). Usage:
#   ./ufc3.sh /path/to/PS4/CUSA06534    first time: the base game folder (the -patch folder next
#                                       to it is merged automatically); remembered afterwards
#   ./ufc3.sh                           plays again with the remembered folder
# Settings: ~/.local/share/UFC3Native/ufc3.conf (created on the first start). Controls, the FPS
# counter and the overlay menu's options: bbport.ini in the same folder. Logs: logs/ there.
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
# No arguments opens the GUI; --play and the existing positional folder keep CLI use.
if [[ $# == 0 ]]; then exec python3 "$here/launcher/ufc3_launcher.py"; fi
if [[ ${1:-} == --play ]]; then shift; fi
data=${BB_DATA_DIR:-${XDG_DATA_HOME:-$HOME/.local/share}/UFC3Native}
mkdir -p "$data"
command -v python3 >/dev/null || { echo "UFC 3: python3 is needed (install it with your package manager)" >&2; exit 1; }
[[ -f $data/ufc3.conf ]] || cp "$here/ufc3.conf.example" "$data/ufc3.conf"
if [[ $# -gt 0 && -d $1 ]]; then
    game=$(cd -- "$1" && pwd); shift
    printf '%s\n' "$game" > "$data/game-folder"
elif [[ -f $data/game-folder ]]; then
    game=$(cat "$data/game-folder")
else
    sed -n '2,7p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 1
fi
[[ -f $game/eboot.bin ]] || { echo "UFC 3: no eboot.bin in $game" >&2; exit 1; }
game=$(python3 "$here/scripts/merge_update.py" "$game" | tail -1)
# ufc3.conf: KEY=value lines; a variable already set in the environment wins.
while IFS= read -r line || [[ -n $line ]]; do
    [[ $line =~ ^([A-Z][A-Z0-9_]*)=(.*)$ ]] || continue
    key=${BASH_REMATCH[1]} value=${BASH_REMATCH[2]}
    [[ -n $value && -z ${!key+x} ]] && export "$key=$value"
done < "$data/ufc3.conf"
export BB_PREBUILT=1 BB_DATA_DIR="$data" BB_GAME_DIR="$game" BB_SAVE_LOG=${BB_SAVE_LOG:-1}
export LD_LIBRARY_PATH="$here/bin/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$here/run.sh" "$@"
