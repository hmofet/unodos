#!/bin/bash
# Build the appliance initramfs: static busybox plus an /init that puts a
# shell on BOTH consoles - ttyS0 (the harness types here over the emulated
# UART) and tty1 (the Display window types here through the emulated i8042,
# and the reply paints into the guest framebuffer).
#
#     ./build_initrd.sh [workdir]        # default /work/unodos-guest
#
# Produces <workdir>/initrd.gz.  Staged as EFI\UNODOS\VM\INITRD.
#
# The cpio MUST carry /dev/console as a real character node: the kernel opens
# it before init runs, so an init that mknods it is too late and cannot even
# report the failure (pc64/UNOVIRT.md, A6c).
set -e
WORK=${1:-/work/unodos-guest}
ROOT="$WORK/initrd-root"

BB=$(command -v busybox || true)
[ -n "$BB" ] || BB=/bin/busybox
[ -x "$BB" ] || { echo "no busybox - apt install busybox-static" >&2; exit 1; }
file "$BB" | grep -q "statically linked" || {
    echo "$BB is not static - apt install busybox-static" >&2; exit 1; }

rm -rf "$ROOT"
mkdir -p "$ROOT"/{bin,sbin,dev,proc,sys,mnt,tmp,etc}
cp "$BB" "$ROOT/bin/busybox"
for a in sh ls cat echo mount umount mkdir mknod dmesg uname sleep ps \
         setsid cttyhack reboot poweroff clear head tail grep vi free df \
         switch_root ip ln; do
    ln -s busybox "$ROOT/bin/$a"
done

cat > "$ROOT/init" <<'EOF'
#!/bin/sh
export PATH=/bin:/sbin
mount -t proc proc /proc
mount -t sysfs sys /sys
mount -t devtmpfs dev /dev 2>/dev/null

# A disk with an appliance on it wins: mount it read-only and hand over.
# (The block device below unovirt can only read; every writable path in the
# appliance is a tmpfs its own init mounts.)
if [ -b /dev/vda ]; then
  mkdir -p /newroot
  if mount -o ro /dev/vda /newroot 2>/dev/null; then
    if [ -x /newroot/sbin/uno-init ]; then
      umount /proc /sys 2>/dev/null
      exec switch_root /newroot /sbin/uno-init
    fi
    umount /newroot 2>/dev/null
  fi
fi

# No appliance disk: the A8 shape - a shell on each console.
#
# NO setsid, NO cttyhack, and the sleep is load-bearing: busybox setsid can
# FORK, at which point the respawn loop stops waiting and spawns shells
# forever - and every one of them reads ttyS0, so a seeded command line is
# torn into fragments split across readers ("sh: linkth0: not found" was
# `busybox ip link set eth0 up` shared out among an army of shells).  One
# reader per tty, respawned only after the old one has really exited, a
# second apart so a crash-looping shell cannot eat the machine.

# The VT shell: what the Display window talks to.  tty1 only exists when
# the kernel found a framebuffer.
if [ -c /dev/tty1 ]; then
  (
    while :; do
      sh </dev/tty1 >/dev/tty1 2>&1
      sleep 1
    done
  ) &
fi

# The serial shell: what the harness (and the Appliances console view)
# talks to.  PID 1's own foreground job, exactly one, forever.
while :; do
  sh </dev/ttyS0 >/dev/ttyS0 2>&1
  sleep 1
done
EOF
chmod +x "$ROOT/init"
ln -s ../init "$ROOT/bin/init" 2>/dev/null || true

# /dev/console as a real node, inside the archive itself.
(cd "$ROOT" && find . | LC_ALL=C sort \
  | cpio -o -H newc --owner=0:0 2>/dev/null; ) > "$WORK/initrd.cpio"
# Append the console node with a device-table trick cpio can't do from a
# plain tree without root: use fakeroot if present, else mknod may work.
if command -v fakeroot >/dev/null; then
    fakeroot sh -c "mknod '$ROOT/dev/console' c 5 1; cd '$ROOT' && \
        find . | LC_ALL=C sort | cpio -o -H newc --owner=0:0" \
        > "$WORK/initrd.cpio" 2>/dev/null
else
    sudo mknod "$ROOT/dev/console" c 5 1 2>/dev/null || \
        mknod "$ROOT/dev/console" c 5 1
    (cd "$ROOT" && find . | LC_ALL=C sort | cpio -o -H newc --owner=0:0) \
        > "$WORK/initrd.cpio" 2>/dev/null
fi
gzip -9 -f "$WORK/initrd.cpio"
mv "$WORK/initrd.cpio.gz" "$WORK/initrd.gz"
ls -la "$WORK/initrd.gz"
echo "initrd: $WORK/initrd.gz"
