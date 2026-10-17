#!/usr/bin/env bash
# UFC 3: screenshot of the game window only (grim cropped to its Hyprland client geometry).
out=${1:?usage: ufc3-shot.sh out.png}
pid=$(pgrep -x sacredfire | head -1)
geo=$(hyprctl clients -j | jq -r --argjson p "$pid" '.[] | select(.pid == $p) | "\(.at[0]),\(.at[1]) \(.size[0])x\(.size[1])"' | head -1)
[[ -n $geo ]] || { echo "game window not found" >&2; exit 1; }
grim -g "$geo" "$out"
