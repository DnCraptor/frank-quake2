#!/bin/bash
# frank-quake2 — one frame from the HDMI capture card ("USB Video").
#   tools/grab.sh [OUT.png]
OUT=${1:-build/grab.png}
ffmpeg -hide_banner -loglevel error -y -f avfoundation -framerate 30 -video_size 640x480 \
    -i "USB Video:none" -frames:v 1 -update 1 "$OUT" && echo "$OUT"
