#!/bin/bash
#
# Fetch the shell icon set from external icon libraries and pre-render it to
# anti-aliased white RGBA PNGs (the shell tints them at runtime).
#
#   glyphs  : @material-design-icons  (Apache-2.0)  -- "round" style
#   cursor  : @mdi/svg  (Apache-2.0)  -- cursor-default
#
# Output: Resource/Icons/md/<name>.png  (referenced by WM_Icons.c / ImUI.c /
# com.ImplusOS.loginui). Re-run to refresh.
#
set -euo pipefail
cd "$(dirname "$0")/.."

OUT=Resource/Icons/md
TMP="$(mktemp -d)"
SIZE=80
CURSOR_SIZE=48
MDI_VER=0.14.13
mkdir -p "$OUT"

# name -> material-design-icons round svg
GLYPHS="
menu apps search wifi wifi_off volume_up volume_off keyboard notifications
settings power_settings_new restart_alt folder folder_open description person
close remove crop_square filter_none expand_less terminal edit
arrow_back arrow_forward arrow_upward home refresh note_add file_open save
delete content_copy content_cut content_paste check storage
"

for name in $GLYPHS; do
    url="https://cdn.jsdelivr.net/npm/@material-design-icons/svg@${MDI_VER}/round/${name}.svg"
    if curl -fsS "$url" -o "$TMP/$name.svg"; then
        python3 Tools/svg2png.py "$TMP/$name.svg" "$OUT/$name.png" "$SIZE"
        echo "  $name.png"
    else
        echo "  !! missing $name" >&2
    fi
done

curl -fsS "https://cdn.jsdelivr.net/npm/@mdi/svg/svg/cursor-default.svg" -o "$TMP/cursor.svg"
python3 Tools/svg2png.py "$TMP/cursor.svg" "$OUT/cursor.png" "$CURSOR_SIZE"
echo "  cursor.png"

rm -rf "$TMP"
echo "done -> $OUT"
