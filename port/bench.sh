#!/bin/sh
# Rendering benchmark: boot, pass the save prompt, sit on the animated title screen.
# usage: port/bench.sh <exe> <log>
PSP2I_FPS_LOG=1 timeout 300 "$1" --root GameData --eboot EBOOT.BIN --headless --seconds 60 \
  --press circle@500 --press circle@800 > "$2" 2>&1
grep "frame rate\|host time" "$2"
grep "^fps:" "$2" | awk '{printf "%s ", $2} END {print ""}'
