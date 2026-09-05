#!/bin/sh
# cosmo64/readlog.sh -- read the UnoDOS debug log back off the Cosmo.
#
# The loop:
#   1. boot UNODOS from the LK menu (p38) and do whatever you are debugging;
#   2. reboot and let it come up in trixie (the default);
#   3. ./readlog.sh
#
# THE LOG LIVES ON THE eMMC. UnoDOS writes it into the unused tail of its own
# boot partition -- p38 is 32 MiB, the boot image is 512 KiB, so the window
# sits 2 MiB in and is 128 KiB long. Block 0 of the window is a header
# ("(UNOLOG)", byte count, the tail's ring offset, and the boot-preamble
# length); the text follows. On a long session that text is the frozen boot
# preamble, a "[older log wrapped away]" marker, then the recent tail, so the
# boot story is always there no matter how long UnoDOS ran.
#
# It used to live in DRAM, in the kernel's ramoops console zone, which would
# have needed no eMMC driver at all. That never reached pstore on this device
# (tested 2026-09-01), and the cause is still open. The first explanation --
# the preloader wiping DRAM -- was DISPROVEN by the survey in log.c: 82 ramoops
# signatures were still standing when UnoDOS took over, and the zone map came
# back as mainline's exact layout, so both the memory and the address were
# right. The likeliest remaining explanation is the way out: leaving UnoDOS by
# holding the power button is a cold power-off, while the LK-menu route in is a
# warm reboot. Untested. The ramoops path is still tried below (-r), because it
# costs one `cat` and it is the only log that exists before storage comes up.
#
#   ./readlog.sh            print the log
#   ./readlog.sh -r         also try the DRAM/pstore path
#   DEV=root@1.2.3.4 ./readlog.sh    another address for the device
set -e

# The Cosmo's address is a DHCP lease and it moves (it has been .56 and .121).
# Probe the ones it has held rather than making the caller notice, and confirm
# it really is the Cosmo before doing anything with it -- .121 has also been
# galaxy's wired address, and this script's siblings write to /dev/mmcblk0.
find_dev() {
    # addresses it has actually held, newest first
    for a in 192.168.2.65 192.168.2.121 192.168.2.56; do
        if [ "$(ssh -o BatchMode=yes -o ConnectTimeout=4 "root@$a" hostname \
                2>/dev/null)" = cosmocom ]; then
            echo "root@$a"
            return 0
        fi
    done
    # it has moved again: sweep the LAN and ask every live host its name. The
    # lease follows whichever machine currently holds the shared USB adapter,
    # so the address is not worth remembering -- the hostname is.
    echo "readlog: not at a known address, sweeping the LAN..." >&2
    for i in $(seq 1 254); do
        ping -n 1 -w 300 "192.168.2.$i" >/dev/null 2>&1 &
    done
    wait 2>/dev/null
    found=
    for ip in $(arp -a | grep -oE "192\.168\.2\.[0-9]+" | sort -u); do
        [ "$ip" = 192.168.2.255 ] && continue
        if [ "$(ssh -o BatchMode=yes -o ConnectTimeout=3 \
                -o StrictHostKeyChecking=no "root@$ip" hostname \
                2>/dev/null)" = cosmocom ]; then
            found="root@$ip"
            break
        fi
    done
    [ -n "$found" ] || return 1
    echo "$found"
}

if [ -z "$DEV" ]; then
    DEV=$(find_dev) || {
        echo "readlog: no Cosmo found (tried .65/.121/.56 and a LAN sweep)." >&2
        echo "  It is offline while sitting in UnoDOS. Set DEV=root@<ip> to override." >&2
        exit 3
    }
    echo "readlog: using $DEV" >&2
fi
ALSO_RAM=
for a in "$@"; do
    case "$a" in
    -r) ALSO_RAM=1 ;;
    *)  echo "usage: readlog.sh [-r]" >&2; exit 1 ;;
    esac
done

ssh "$DEV" "ALSO_RAM=$ALSO_RAM sh -s" <<'REMOTE'
set -e

# The window is at a fixed offset inside p38, and p38 has its own device node,
# so no GPT lookup is needed here: skip=4096 blocks is LOG_OFF_SECTORS.
WIN=4096

HDRBLK=$(dd if=/dev/mmcblk0p38 bs=512 skip=$WIN count=1 2>/dev/null | od -An -tx1 -N16 | tr -d ' \n')
MAGIC=$(printf '%s' "$HDRBLK" | cut -c1-16)

# "(UNOLOG)" as it lands on disk: 28 55 4e 4f 4c 4f 47 29
if [ "$MAGIC" = "28554e4f4c4f4729" ]; then
    LEN=$(dd if=/dev/mmcblk0p38 bs=512 skip=$WIN count=1 2>/dev/null | od -An -tu4 -j8 -N4 | tr -d ' \n')
    PRE=$(dd if=/dev/mmcblk0p38 bs=512 skip=$WIN count=1 2>/dev/null | od -An -tu4 -j16 -N4 | tr -d ' \n')
    note=
    # PRE > 0 means the session outran the window: what follows is the boot
    # preamble, then a "[older log wrapped away]" marker, then the recent tail.
    # The boot story is present either way -- that is the point of the preamble.
    [ "${PRE:-0}" -gt 0 ] && note=" (wrapped: boot preamble + recent tail, middle dropped)"
    echo "########## eMMC log: p38 + 2 MiB, $LEN bytes$note"
    dd if=/dev/mmcblk0p38 bs=512 skip=$((WIN + 1)) count=255 2>/dev/null \
        | head -c "$LEN" | tr -d '\000'
    echo
else
    echo "readlog: no UnoDOS log in p38's window (header reads $MAGIC)." >&2
    echo "  Either UnoDOS has not run since this slot was last written, or it" >&2
    echo "  died before the eMMC driver came up -- check the panel for a red" >&2
    echo "  block (a fault) or a stalled beacon colour." >&2
fi

if [ -n "$ALSO_RAM" ]; then
    echo
    echo "########## DRAM/pstore path (expected to be empty on this device)"
    f=$(ls -1tr /sys/fs/pstore/console-ramoops-* \
                /var/lib/systemd/pstore/*/console-ramoops-* 2>/dev/null | tail -1)
    if [ -n "$f" ]; then
        echo "## $f"
        cat "$f"
    else
        echo "## nothing in /sys/fs/pstore or /var/lib/systemd/pstore"
    fi
fi
REMOTE
