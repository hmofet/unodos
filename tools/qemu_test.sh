#!/bin/bash
# UnoDOS headless QEMU test driver.
# Usage: qemu_test.sh <image> <artifact-dir> <instance-id>
# Starts QEMU headless with a monitor socket, then reads commands from stdin:
#   wait <seconds>          - sleep
#   key <qemu-keyname>      - send a key (e.g. ret, down, a, esc, shift-a)
#   keys <k1> <k2> ...      - send several keys with 0.3s gaps
#   type <text>             - type a string (letters/digits only)
#   mousemove <dx> <dy>     - relative mouse move
#   click [btn]             - press+release mouse button (default 1=left)
#   dblclick                - double click left button
#   shot <name>             - screendump to <artifact-dir>/<name>.png
#   quit                    - shut down QEMU
set -u
IMG="$1"; ART="$2"; ID="${3:-0}"
SOCK="/tmp/unodos-mon-$ID.sock"
mkdir -p "$ART"
rm -f "$SOCK"
qemu-system-i386 -M isapc -m 640K -snapshot \
  -drive "file=$IMG,format=raw,if=floppy" -boot a \
  -display none -monitor "unix:$SOCK,server,nowait" \
  -rtc base=localtime &
QPID=$!
trap 'kill $QPID 2>/dev/null' EXIT
sleep 1

mon() { echo "$1" | socat - "unix-connect:$SOCK" >/dev/null 2>&1; }

while read -r cmd rest; do
  case "$cmd" in
    wait) sleep "$rest" ;;
    key) mon "sendkey $rest"; sleep 0.3 ;;
    keys) for k in $rest; do mon "sendkey $k"; sleep 0.3; done ;;
    type) for (( i=0; i<${#rest}; i++ )); do c="${rest:$i:1}"; [ "$c" = " " ] && c=spc; mon "sendkey $c"; sleep 0.2; done ;;
    mousemove) mon "mouse_move $rest"; sleep 0.2 ;;
    click) b="${rest:-1}"; mon "mouse_button $b"; sleep 0.15; mon "mouse_button 0"; sleep 0.3 ;;
    btn) mon "mouse_button ${rest:-0}"; sleep 0.3 ;;
    dblclick) mon "mouse_button 1"; sleep 0.1; mon "mouse_button 0"; sleep 0.15; mon "mouse_button 1"; sleep 0.1; mon "mouse_button 0"; sleep 0.3 ;;
    shot) mon "screendump $ART/$rest.ppm"; sleep 0.5; pnmtopng "$ART/$rest.ppm" > "$ART/$rest.png" 2>/dev/null && rm -f "$ART/$rest.ppm" ;;
    quit) break ;;
  esac
done
mon "quit"
sleep 1
kill $QPID 2>/dev/null
exit 0
