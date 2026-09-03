#!/bin/sh
# cosmo64/flashp38.sh -- write the built boot image to the Cosmo's p38 slot.
#
#   ./build.sh shell && ./flashp38.sh
#   ./flashp38.sh build/pc64arm-boot.img        # an explicit image
#   DEV=root@1.2.3.4 ./flashp38.sh              # skip the search
#
# WHY THIS EXISTS RATHER THAN A REMEMBERED dd LINE. The Cosmo's address is a
# DHCP lease and it moves -- it has been .56, .65, .121 and .254 -- and .121
# has ALSO been galaxy's wired address, because the USB Ethernet adapter
# carries its lease between machines. A `dd` to /dev/mmcblk0 aimed at the
# wrong host is unrecoverable. So the target is confirmed by HOSTNAME and
# serial number before anything is written, never by address.
#
# The device must be in TRIXIE (or any Linux on it), not UnoDOS: this writes
# over ssh as root. p38 is UnoDOS's own 32 MiB boot slot; the image is well
# under that, and its unused tail is where the debug log lives (msdc.c), which
# is why this writes only the image's own length and never the whole slot.
#
# NEVER point this at lk, lk2 or preloader. Those are the only partitions on
# this device with no recovery path.
set -e

IMG="${1:-build/pc64arm-boot.img}"
[ -f "$IMG" ] || { echo "flashp38: no image at $IMG (run ./build.sh shell)" >&2; exit 1; }

SERIAL=VON7AUT8IR9DFMAU
HOST=cosmocom

confirm() {          # $1 = user@addr; echoes it back only if it IS the Cosmo
    h=$(ssh -o BatchMode=yes -o ConnectTimeout=4 -o StrictHostKeyChecking=no \
            "$1" hostname 2>/dev/null) || return 1
    [ "$h" = "$HOST" ] || return 1
    echo "$1"
}

find_dev() {
    for a in 192.168.2.65 192.168.2.254 192.168.2.121 192.168.2.56; do
        confirm "root@$a" && return 0
    done
    echo "flashp38: not at a known address, sweeping the LAN..." >&2
    for i in $(seq 1 254); do
        ping -n 1 -w 300 "192.168.2.$i" >/dev/null 2>&1 &
    done
    wait 2>/dev/null
    for ip in $(arp -a | grep -oE "192\.168\.2\.[0-9]+" | sort -u); do
        [ "$ip" = 192.168.2.255 ] && continue
        confirm "root@$ip" && return 0
    done
    return 1
}

if [ -n "$DEV" ]; then
    # An address given by hand gets the SAME identity check as a discovered
    # one: a typo is exactly the case this script exists to catch.
    confirm "$DEV" >/dev/null || {
        echo "flashp38: $DEV is not a machine calling itself $HOST -- REFUSING" >&2
        exit 1; }
else
    DEV="$(find_dev)" || true
fi
[ -n "$DEV" ] || { echo "flashp38: the Cosmo is not reachable -- is it booted into Trixie?" >&2; exit 1; }

# Belt and braces: the hostname matched, now match the serial the bootloader
# reports. Two independent facts about the same machine.
S=$(ssh -o BatchMode=yes "$DEV" 'tr " " "\n" < /proc/cmdline | sed -n "s/^androidboot.serialno=//p" | head -1' | tr -d '\r')
if [ "$S" != "$SERIAL" ]; then
    echo "flashp38: $DEV says hostname $HOST but serial '$S', wanted '$SERIAL' -- REFUSING" >&2
    exit 1
fi

N=$(wc -c < "$IMG" | tr -d ' ')
LOCAL=$(sha256sum "$IMG" | cut -d' ' -f1)
echo "flashp38: $IMG ($N bytes, sha256 $LOCAL)"
echo "flashp38: target $DEV (hostname $HOST, serial $SERIAL) -> /dev/mmcblk0p38"

scp -q "$IMG" "$DEV:/root/pc64arm-boot.img"
REMOTE=$(ssh -o BatchMode=yes "$DEV" "set -e
    dd if=/root/pc64arm-boot.img of=/dev/mmcblk0p38 bs=1M conv=fsync status=none
    dd if=/dev/mmcblk0p38 bs=1M count=32 status=none | head -c $N | sha256sum | cut -d' ' -f1")
REMOTE=$(echo "$REMOTE" | tr -d '\r')

if [ "$REMOTE" != "$LOCAL" ]; then
    echo "flashp38: READBACK MISMATCH -- wrote $LOCAL, p38 reads $REMOTE" >&2
    exit 1
fi
echo "flashp38: readback verified. Reboot and pick UNODOS from the LK menu."
