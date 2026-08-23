#!/bin/bash
# The Android appliance's fast loop: boot apps/android.app under plain QEMU,
# photograph it, and print the lines that say which layer got as far as it
# did.  A hypervisor run is twenty-five minutes; this is about five, and
# every layer below the hypervisor is identical.
#
#     ./build_kernel.sh  /work/unodos-android          # once
#     ./build_android.sh /work/unodos-android          # once, ~10 min
#     cp firefox-x86_64.apk /work/unodos-android/firefox.apk
#     UNO_APP=android ./build_rootfs.sh /work/unodos-android
#     ./android_loop.sh /work/unodos-android [seconds]
#
# Shots land in <workdir>/shots/ and the serial log in <workdir>/loop.log.
#
# -cpu host IS NOT A PERFORMANCE CHOICE.  waydroid's tools/helpers/arch.py
# remaps x86_64 to "x86" when /proc/cpuinfo does not advertise sse4_2, and
# then looks for x86 images this appliance does not carry.  QEMU's default
# CPU model has no sse4_2, so without this the whole run fails by looking for
# the wrong architecture, and says so in a line that reads like a note.
set -e
WORK=${1:-/work/unodos-android}
RUN=${2:-420}
MEM=${MEM:-4096}
LOG=$WORK/loop.log
MON=/tmp/android-loop.mon
SHOTS=$WORK/shots

for f in bzImage rootfs-android.img android.img; do
    [ -f "$WORK/$f" ] || { echo "android_loop: no $WORK/$f" >&2; exit 2; }
done
mkdir -p "$SHOTS"
rm -f "$LOG" "$MON" "$SHOTS"/*.ppm "$SHOTS"/*.png 2>/dev/null || true

echo "android_loop: booting for ${RUN}s (${MEM} MB), log $LOG"
qemu-system-x86_64 -enable-kvm -cpu host -m "$MEM" -smp 2 -no-reboot -kernel "$WORK/bzImage" -append "console=ttyS0 root=/dev/vda ro init=/sbin/uno-init nokaslr" -drive file="$WORK/rootfs-android.img",format=raw,if=virtio,readonly=on -drive file="$WORK/android.img",format=raw,if=virtio,readonly=on -netdev user,id=n0 -device virtio-net,netdev=n0 -vga std -display none -serial file:"$LOG" -monitor unix:"$MON",server=on,wait=off &
QPID=$!
trap 'kill $QPID 2>/dev/null || true' EXIT

# PHOTOGRAPH IT ON A SCHEDULE, because the interesting moments are minutes
# apart and each one looks like the last from the outside: an empty
# compositor, an Android that is booting, and an app that has not drawn yet
# are all black.  A shot every 45 s makes the sequence readable afterwards.
shot() {
    printf 'screendump %s\n' "$SHOTS/$1.ppm" | socat - UNIX-CONNECT:$MON >/dev/null 2>&1 || return 0
    [ -s "$SHOTS/$1.ppm" ] || return 0
    if command -v convert >/dev/null 2>&1; then
        convert "$SHOTS/$1.ppm" "$SHOTS/$1.png" 2>/dev/null && rm -f "$SHOTS/$1.ppm"
    fi
}

t=0
while [ $t -lt "$RUN" ]; do
    sleep 45; t=$((t + 45))
    kill -0 $QPID 2>/dev/null || { echo "android_loop: qemu exited at ${t}s"; break; }
    shot "t$(printf %03d $t)"
    echo "--- ${t}s ---"
    grep -E "uno-android:|uno-selftest:" "$LOG" 2>/dev/null | tail -3 || true
done

kill $QPID 2>/dev/null || true
sleep 1
echo
echo "===================== what the appliance said ====================="
grep -E "^uno: (appliance|lease|dns-ok|https-ok|ip-ok|cage|labwc|input nodes)|uno-android:|uno-selftest:" "$LOG" | tail -45 || true
echo "-------------------------------------------------------------------"
ls -la "$SHOTS" | tail -12
echo "full log: $LOG"
