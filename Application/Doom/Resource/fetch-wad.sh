#!/usr/bin/env bash
# fetch-wad.sh — Doom shareware IWAD (doom1.wad) を Resource/ へ取得する。
#
# バイナリ／データの追加のみ・ソース不改変の方針（TODO_Doom_Xorg_MethodA.md §5）。
# doom1.wad は id Software の DOOM シェアウェア配布物で自由再配布可。
# リポジトリにはコミットしない（.gitignore 済み）。初回のみネットワークが要る。
#
# 期待: DOOM shareware IWAD v1.9  size=4196020  md5=f0cefca49926d00903cf57551d901abe
set -euo pipefail

cd "$(dirname "$0")"
OUT=doom1.wad
WANT_MD5=f0cefca49926d00903cf57551d901abe
WANT_SIZE=4196020

if [ -f "$OUT" ] && [ "$(stat -c%s "$OUT")" = "$WANT_SIZE" ]; then
	echo "[fetch-wad] $OUT already present ($WANT_SIZE bytes)"
	exit 0
fi

ZIP_URL="https://www.doomworld.com/3ddownloads/ports/shareware_doom_iwad.zip"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

echo "[fetch-wad] GET $ZIP_URL"
curl -fsSL --retry 3 --retry-delay 2 -o "$tmp/sw.zip" "$ZIP_URL"

# unzip がホストに無くても取り出せるよう python の zipfile を使う
python3 - "$tmp/sw.zip" "$OUT" <<'PY'
import sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
name = next(n for n in z.namelist() if n.lower().endswith(".wad"))
open(sys.argv[2], "wb").write(z.read(name))
PY

got_size="$(stat -c%s "$OUT")"
got_md5="$(md5sum "$OUT" | cut -d' ' -f1)"
[ "$got_size" = "$WANT_SIZE" ] || { echo "[fetch-wad] size mismatch: $got_size != $WANT_SIZE" >&2; exit 1; }
[ "$got_md5" = "$WANT_MD5" ]   || { echo "[fetch-wad] md5 mismatch: $got_md5 != $WANT_MD5" >&2; exit 1; }
head -c4 "$OUT" | grep -q IWAD || { echo "[fetch-wad] not an IWAD" >&2; exit 1; }
echo "[fetch-wad] OK: $OUT ($got_size bytes, md5 $got_md5)"
