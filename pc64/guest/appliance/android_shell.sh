#!/bin/bash
# Boot the Android appliance and leave a DRIVEABLE shell on its serial port,
# instead of a log file that can only be read afterwards.  The appliance's
# faults are all environment faults (see apps/android.app), and an
# environment fault takes seconds to test at a prompt and eight minutes to
# test through a rebuild.
#
#     ./android_shell.sh start [workdir]     # boot, leave it running
#     ./android_shell.sh run 'command'       # run it in the guest, print output
#     ./android_shell.sh shot name           # screendump -> <workdir>/shots
#     ./android_shell.sh log                 # tail the serial log
#     ./android_shell.sh stop
#
# ONE CONNECTION.  The serial chardev accepts a single client, so it is
# presented as a pty and a reader and a writer share that - which is also
# what makes `run` possible: the writer echoes a command, the reader picks
# the answer out of the same stream.
WORK=${2:-/work/unodos-android}
[ -d "$WORK" ] || WORK=/work/unodos-android
SER=/tmp/android-shell.ser
PTY=/tmp/android-shell.pty
MON=/tmp/android-shell.mon
OUT=/tmp/android-shell.out
SHOTS=$WORK/shots
MEM=${MEM:-4096}

case "$1" in
start)
    pkill -f "android-shell.ser" 2>/dev/null
    rm -f "$SER" "$MON" "$PTY" "$OUT"
    mkdir -p "$SHOTS"
    qemu-system-x86_64 -enable-kvm -cpu host -m "$MEM" -smp 2 -no-reboot -kernel "$WORK/bzImage" -append "console=ttyS0 root=/dev/vda ro init=/sbin/uno-init nokaslr" -drive file="$WORK/rootfs-android.img",format=raw,if=virtio,readonly=on -drive file="$WORK/android.img",format=raw,if=virtio,readonly=on -netdev user,id=n0 -device virtio-net,netdev=n0 -vga std -display none -serial unix:"$SER",server=on,wait=off -monitor unix:"$MON",server=on,wait=off >/dev/null 2>&1 &
    sleep 2
    setsid socat UNIX-CONNECT:"$SER" PTY,link="$PTY",raw,echo=0 >/dev/null 2>&1 &
    sleep 2
    setsid sh -c "cat $PTY > $OUT" >/dev/null 2>&1 &
    sleep 1
    echo "booted: pty $PTY, log $OUT, monitor $MON"
    ;;
run)
    # A MARKER ROUND THE ANSWER, because the serial stream carries kernel
    # messages and the appliance's own progress lines at the same time.
    M="__UNO_$$__"
    printf 'echo %s; %s; echo %s\n' "$M-B" "$2" "$M-E" > "$PTY"
    sleep "${3:-6}"
    awk -v b="$M-B" -v e="$M-E" '$0 ~ b {f=1; next} $0 ~ e {f=0} f' "$OUT" | tail -60
    ;;
click)
    # THE POINTER IS RELATIVE, so it is PINNED FIRST.  QEMU's i8042 aux port
    # is a PS/2 mouse and PS/2 mice have no coordinates - only deltas - so
    # "click at 640,604" means: drive it hard into the top-left corner where
    # the compositor clamps it, then move exactly that far.  A tablet would
    # make this one command, and would also be a device unovirt does not
    # give the guest, so the harness would be testing something the product
    # does not have.
    {   printf 'mouse_move -4000 -4000\n'
        printf 'mouse_move %s %s\n' "$2" "$3"
        printf 'mouse_button 1\n'
        printf 'mouse_button 0\n'
    } | socat - UNIX-CONNECT:"$MON" >/dev/null 2>&1
    echo "clicked $2,$3"
    ;;
key)
    # Through the emulated KEYBOARD, not through the guest's own tooling:
    # this is the path a person's keystroke takes, and the one that has to
    # work.  Names are QEMU's (ret, spc, kp_enter, a-z, 0-9).
    shift
    for k in "$@"; do
        printf 'sendkey %s\n' "$k" | socat - UNIX-CONNECT:"$MON" >/dev/null 2>&1
        sleep 0.15
    done
    echo "sent: $*"
    ;;
type)
    # One character at a time, because `sendkey` takes key NAMES.  Only the
    # characters a URL needs are mapped; anything else is skipped loudly
    # rather than silently dropped, since a URL that is quietly one character
    # short looks exactly like a browser that cannot resolve a name.
    s=$2
    i=0
    while [ $i -lt ${#s} ]; do
        c=$(printf '%s' "$s" | cut -c$((i + 1)))
        case "$c" in
        [a-z0-9]) K=$c ;;
        .) K=dot ;;
        /) K=slash ;;
        -) K=minus ;;
        :) K="shift-semicolon" ;;
        *) echo "type: no key name for '$c'" >&2; K="" ;;
        esac
        [ -n "$K" ] && printf 'sendkey %s\n' "$K" | socat - UNIX-CONNECT:"$MON" >/dev/null 2>&1
        sleep 0.12
        i=$((i + 1))
    done
    echo "typed: $s"
    ;;
shot)
    printf 'screendump %s\n' "$SHOTS/$2.ppm" | socat - UNIX-CONNECT:"$MON" >/dev/null 2>&1
    convert "$SHOTS/$2.ppm" "$SHOTS/$2.png" 2>/dev/null && rm -f "$SHOTS/$2.ppm"
    ls -la "$SHOTS/$2.png"
    ;;
log)
    tail -"${2:-40}" "$OUT"
    ;;
stop)
    pkill -f "android-shell.ser" 2>/dev/null
    pkill -f "$PTY" 2>/dev/null
    echo stopped
    ;;
*)
    echo "usage: $0 {start|run <cmd>|shot <name>|log [n]|stop}" >&2; exit 2 ;;
esac
