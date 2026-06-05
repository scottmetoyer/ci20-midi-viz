#!/bin/sh
# runviz.sh — run the visualizer with /dev/fb0 freed from the text console.
#
# PowerVR's FLIP WSEGL needs exclusive use of the framebuffer, but the Linux
# framebuffer console (fbcon) normally owns /dev/fb0 — so viz fails at
# eglInitialize with EGL_BAD_ALLOC (0x3003). This unbinds fbcon, runs viz, and
# rebinds the console automatically on exit (even on Ctrl-C).
#
# Usage:  sudo ./runviz.sh shaders/reactive.frag
#         sudo ./runviz.sh shaders/whatever.frag 0 3
cd "$(dirname "$0")"

# find the framebuffer console (vtcon* whose name is "frame buffer device")
FBCON=""
for v in /sys/class/vtconsole/vtcon*; do
    if grep -qi "frame buffer" "$v/name" 2>/dev/null; then FBCON="$v/bind"; fi
done

if [ -z "$FBCON" ] || [ ! -w "$FBCON" ]; then
    echo "Need root to free the framebuffer console."
    echo "Run:  sudo ./runviz.sh $*"
    exit 1
fi

rebind() { echo 1 > "$FBCON" 2>/dev/null; }
trap rebind EXIT INT TERM

echo 0 > "$FBCON"      # release /dev/fb0 from the text console
./viz "$@"            # rebind() runs on exit via the trap
