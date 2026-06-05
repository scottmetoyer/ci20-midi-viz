#!/bin/sh
# Build the CI20 GLES2 tools ON THE BOARD (gcc is on the CI20; these are mipsel binaries).
set -e
cd "$(dirname "$0")"
CFLAGS="-O2 -std=gnu99 -I./include"
gcc $CFLAGS viz.c      -lEGL -lGLESv2 -lm -lrt -lpthread -o viz
gcc $CFLAGS host.c     -lEGL -lGLESv2 -lm -lrt           -o glrun
gcc $CFLAGS midiread.c                                    -o midiread
echo "built: viz, glrun, midiread"
echo
echo "run:  ./viz shaders/reactive.frag        # MIDI-reactive visualizer (Ctrl-C to stop)"
echo "      ./glrun shaders/plasma.frag        # plain shader runner (no MIDI)"
echo "      ./midiread                         # print incoming MIDI events (debug)"
