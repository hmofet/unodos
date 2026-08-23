#!/bin/bash
# The Android fast loop: does a Waydroid container BOOT ANDROID on the UnoDOS
# appliance kernel?  Seven minutes, plain QEMU, no hypervisor, no compositor
# on a screen, nothing to look at - a serial log that either says
# `Session: RUNNING` with zygote in the process list, or says which layer
# stopped it.
#
#     ./build_kernel.sh  /work/unodos-guest     # once
#     ./build_android.sh /work/unodos-guest     # once, ~10 min, 2.7 GB
#     ./android_probe.sh /work/unodos-guest     # as often as you like
#
# WHY THIS EXISTS SEPARATELY FROM THE APPLIANCE.  The appliance answers "can a
# person use an Android app in a UnoDOS window", which needs a compositor, a
# framebuffer, input, and a client - four more things that can be broken.
# This answers only "does the runtime run", and every one of the seven
# environment faults found while writing it (see below) would have presented,
# through the appliance, as a black window.
#
# WHAT IT USES THAT THE REAL APPLIANCE WILL NOT.  wlroots' HEADLESS backend.
# Waydroid starts Android from a SESSION, and a session needs a compositor to
# put surfaces on - so with no display at all the container sits at STOPPED
# with everything else working perfectly.  The headless backend gives a real
# Wayland socket and no display hardware, which separates "Android boots" from
# "a framebuffer works".
set -e
WORK=${1:-/work/unodos-guest}
HERE="$(cd "$(dirname "$0")" && pwd)"
LOG=${LOG:-$WORK/android-probe.log}
MEM=${MEM:-3072}
TIMEOUT=${TIMEOUT:-700}

[ -f "$WORK/bzImage" ]    || { echo "no $WORK/bzImage - run build_kernel.sh" >&2; exit 2; }
[ -f "$WORK/android.img" ] || { echo "no $WORK/android.img - run build_android.sh" >&2; exit 2; }

echo "android_probe: building the probe rootfs"
docker run --rm -v "$WORK":/out -v "$HERE":/scripts:ro \
    alpine:3.20 sh /scripts/android_probe_inner.sh >/dev/null

echo "android_probe: booting (up to ${TIMEOUT}s, log: $LOG)"
rm -f "$LOG"
# -cpu host IS NOT A PERFORMANCE CHOICE.  waydroid's tools/helpers/arch.py
# remaps x86_64 to "x86" when /proc/cpuinfo does not advertise sse4_2, and
# then looks for x86 images this appliance does not carry.  QEMU's default
# CPU model has no sse4_2, so without this the probe fails by looking for the
# wrong architecture - and says so in a line that reads like a note.
timeout "$TIMEOUT" qemu-system-x86_64 -enable-kvm -cpu host -m "$MEM" -smp 2 \
    -no-reboot \
    -kernel "$WORK/bzImage" \
    -append "console=ttyS0 root=/dev/vda ro init=/sbin/uno-init nokaslr" \
    -drive file="$WORK/rootfs-android-probe.img",format=raw,if=virtio,readonly=on \
    -drive file="$WORK/android.img",format=raw,if=virtio,readonly=on \
    -netdev user,id=n0 -device virtio-net,netdev=n0 \
    -display none -serial file:"$LOG" >/dev/null 2>&1 || true

echo
echo "===================== what the probe found ====================="
grep -E "^PROBE:" "$LOG" | grep -vE "status\[([2-9]|[0-9][0-9])\]" || true
echo "---------------------------------------------------------------"
if grep -q "Container:.*RUNNING" "$LOG" 2>/dev/null \
   && grep -q "zygote" "$LOG" 2>/dev/null; then
    echo "PASS: Android booted - container RUNNING, zygote and surfaceflinger up"
    grep -E "Memory use|CPU use" "$LOG" | head -2
    exit 0
fi
echo "FAIL: Android did not reach a running container.  The PROBE lines above"
echo "      name the layer; $LOG has the whole boot."
exit 1
