#!/bin/sh
# cosmo64/readlog.sh -- read the UnoDOS debug log back off the Cosmo.
#
# The loop:
#   1. boot UnoDOS from the LK menu (p38) and do whatever you are debugging;
#   2. reboot and let it come up in trixie (the default);
#   3. ./readlog.sh
#
# UnoDOS writes its log (log.c) into the ramoops CONSOLE zone at 0x5449F000, in
# ramoops' own format. So on the next boot the kernel finds it exactly as it
# would find its own pre-panic console, saves it, and exposes it under
# /sys/fs/pstore -- which is why this needs no /dev/mem (that kernel has none),
# no kernel module and no photographs of the panel.
#
# One catch: systemd-pstore.service fires the moment /sys/fs/pstore is
# non-empty and MOVES the files into /var/lib/systemd/pstore/<id>/. Whichever
# won the race, the log is in one of the two places, so look in both -- newest
# first.
#
# The zone survives the reset but not a Linux boot: the kernel zaps it after
# saving, and then writes its own console there. So each UnoDOS run is readable
# exactly once, from the first trixie boot after it. Read it before rebooting
# again.
#
#   ./readlog.sh            print the newest saved log
#   ./readlog.sh -a         print every saved log, oldest first
#   ./readlog.sh -c         print it, then delete it (frees the pstore slot)
#   DEV=root@1.2.3.4 ./readlog.sh    another address for the device
set -e

DEV="${DEV:-root@192.168.2.56}"
MODE=one
CLEAR=
for a in "$@"; do
    case "$a" in
    -a) MODE=all ;;
    -c) CLEAR=1 ;;
    *)  echo "usage: readlog.sh [-a] [-c]" >&2; exit 1 ;;
    esac
done

ssh "$DEV" "MODE=$MODE CLEAR=$CLEAR sh -s" <<'REMOTE'
set -e
live=$(ls -1 /sys/fs/pstore/console-ramoops-* 2>/dev/null || true)
saved=$(ls -1 /var/lib/systemd/pstore/*/console-ramoops-* 2>/dev/null || true)
all=$(printf '%s\n%s\n' "$live" "$saved" | grep -v '^$' || true)

if [ -z "$all" ]; then
    echo "readlog: nothing in pstore." >&2
    echo "  Either UnoDOS has not run since the last trixie boot, or this IS" >&2
    echo "  the second boot since and the log was already consumed." >&2
    exit 2
fi

# newest last, so "tail -1" is the newest and "-a" reads oldest first
ordered=$(ls -1tr $all 2>/dev/null)
[ "$MODE" = all ] || ordered=$(echo "$ordered" | tail -1)

for f in $ordered; do
    echo "########## $f"
    if head -c 4096 "$f" | grep -q '=== UnoDOS cosmo64 ==='; then
        :
    else
        echo "## note: no UnoDOS banner in this one -- it is a KERNEL console" >&2
        echo "##       log, not a UnoDOS one." >&2
    fi
    cat "$f"
    echo
done

if [ -n "$CLEAR" ]; then
    for f in $ordered; do rm -f "$f"; done
    echo "## cleared"
fi
REMOTE
