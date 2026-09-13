#!/bin/sh
# Read a RenderDoc capture without opening the interface.
#
#   ./scripts/inspect-capture.sh <capture.rdc> list
#       every change of render target through the frame, so the passes are
#       visible at a glance
#
#   ./scripts/inspect-capture.sh <capture.rdc> pixel <x> <y>
#       walks the draws and reports each one that changes the colour at that
#       point, given as a fraction of the frame - 0.5 0.5 is the middle. This
#       is how you find the draw responsible for what you can see.
#
#   ./scripts/inspect-capture.sh <capture.rdc> pixelxy <x> <y>
#       the same, but in the target's own pixels - use coordinates read off a
#       dumped render target, since those hold bands of the frame, not the
#       picture.
#
#   ./scripts/inspect-capture.sh <capture.rdc> draws [min-indices]
#       one line per draw - geometry size and the textures it binds. Use this
#       when probing a pixel will not do, which is whenever the guest is drawing
#       into its EDRAM surface rather than a picture-shaped target.
#
#   ./scripts/inspect-capture.sh <capture.rdc> draw <event>[,<event>...] [--shaders]
#       full state for those draws: targets, blending, bound textures and the
#       contents of every constant buffer, optionally with the translated
#       shaders disassembled
#
#   ./scripts/inspect-capture.sh <capture.rdc> textures <directory> [event]
#       every texture in the frame, as PNG. Render targets are recycled as the
#       frame goes on, so name an event to see them as they stood at that draw
#       rather than blank at the end.
#
# The replay API lives inside qrenderdoc, which is why this goes through it.
set -e
capture=$1
mode=$2
[ -n "$capture" ] && [ -n "$mode" ] || { sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }
[ -f "$capture" ] || { echo "no such capture: $capture" >&2; exit 1; }
command -v qrenderdoc >/dev/null || { echo "qrenderdoc is not installed" >&2; exit 1; }

here=$(cd "$(dirname "$0")" && pwd)
out=$(mktemp)

XE_CAPTURE=$(cd "$(dirname "$capture")" && pwd)/$(basename "$capture")
XE_MODE=$mode
XE_INSPECT_OUT=$out
export XE_CAPTURE XE_MODE XE_INSPECT_OUT

case "$mode" in
    pixelxy|pixel)
        XE_X=$3; XE_Y=$4
        [ -n "$XE_X" ] && [ -n "$XE_Y" ] || { echo "$mode needs x and y" >&2; exit 1; }
        export XE_X XE_Y ;;
    draw)
        XE_EVENTS=$3
        [ -n "$XE_EVENTS" ] || { echo "draw needs an event number" >&2; exit 1; }
        export XE_EVENTS
        case "$4" in --shaders) XE_SHADERS=1; export XE_SHADERS ;; esac ;;
    draws)
        [ -n "$3" ] && XE_MIN_INDICES=$3 && export XE_MIN_INDICES ;;
    textures)
        XE_TEXTURE_DIR=$3
        [ -n "$4" ] && XE_AT_EVENT=$4 && export XE_AT_EVENT
        [ -n "$XE_TEXTURE_DIR" ] || { echo "textures needs a directory" >&2; exit 1; }
        export XE_TEXTURE_DIR ;;
esac

qrenderdoc --python "$here/inspect_capture.py" >/dev/null 2>&1 || true
# qrenderdoc opens its window after running the script, so it has to be closed.
pkill -x qrenderdoc 2>/dev/null || true
cat "$out"
rm -f "$out"
