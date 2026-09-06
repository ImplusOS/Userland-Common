#!/bin/bash
#
# Drive the compositor with a real GTK3 client on the build host.
#
#   ./run.sh [client-binary] [seconds]
#
# Builds Compositor.c + Wayland.c natively against HostShim.c and runs the
# staged Debian gtk3-demo against it over a real AF_UNIX socket, with
# libwayland-client doing the marshalling -- so a protocol mistake shows up
# here instead of costing a QEMU boot. Every presented frame is written to
# run/frameNNN.ppm; ppm2png.py turns one into an image.
#
# Scripted input: put lines in an input file and pass it as $WLC_INPUT.
#   "<frame> m <dx> <dy> <buttons>"   pointer delta + button mask
#   "<frame> k <scancode> <down> <mods>"   set-1 scancode, e.g. 57424 = Down
#
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
APP="$HERE/.."
REPO="$(cd "$APP/../../.." && pwd)"
STAGE="$REPO/Build/x86_64/LinuxRuntime/stage"
OUT="$HERE/.work"
CLIENT="${1:-$STAGE/usr/bin/gtk3-demo}"
SECS="${2:-8}"

if [ ! -x "$STAGE/lib64/ld-linux-x86-64.so.2" ]; then
    echo "no staged Linux runtime at $STAGE -- run 'make linux_runtime_stage' first" >&2
    exit 1
fi

mkdir -p "$OUT/run"
gcc -g -O1 -o "$OUT/wlc_host" -I"$APP" -I"$REPO/Userland/API/Source" \
    -D_start=wlc_main ${WLC_TRACE:+-DWLC_PROTOCOL_TRACE} \
    "$APP/Wayland.c" "$APP/Compositor.c" "$HERE/HostShim.c" || exit 1

pkill -x wlc_host 2>/dev/null
sleep 0.3
rm -f "$OUT/run"/frame*.ppm
WLC_TEST_DIR="$OUT/run" WLC_DUMP_EACH=1 "$OUT/wlc_host" >"$OUT/compositor.log" 2>&1 &
COMP=$!
sleep 1

# Point every runtime-data lookup at the staged tree, not the build host's own
# GTK installation -- otherwise the harness silently tests the host's fonts and
# image loaders and passes while the image is missing them.
XDG_RUNTIME_DIR="$OUT/run" WAYLAND_DISPLAY=wayland-0 GDK_BACKEND=wayland \
GSETTINGS_BACKEND=memory GSETTINGS_SCHEMA_DIR="$STAGE/usr/share/glib-2.0/schemas" \
FONTCONFIG_FILE="$STAGE/etc/fonts/fonts.conf" HOME="$OUT" \
GDK_PIXBUF_MODULE_FILE="$STAGE/usr/lib/x86_64-linux-gnu/gdk-pixbuf-2.0/2.10.0/loaders.cache" \
GDK_PIXBUF_MODULEDIR="$STAGE/usr/lib/x86_64-linux-gnu/gdk-pixbuf-2.0/2.10.0/loaders" \
timeout "$SECS" "$STAGE/lib64/ld-linux-x86-64.so.2" \
    --library-path "$STAGE/usr/lib/x86_64-linux-gnu" "$CLIENT" \
    >"$OUT/client.log" 2>&1
echo "client exit=$? (124 = still running when the timer expired, which is the good case)"

sleep 0.5
kill -TERM $COMP 2>/dev/null
wait $COMP 2>/dev/null

echo "--- client output ---"; cat "$OUT/client.log"
echo "--- compositor ---";    grep -v '^\[wl\] req' "$OUT/compositor.log"
echo "--- frames in $OUT/run ---"; ls "$OUT/run"/frame*.ppm 2>/dev/null | tail -1
