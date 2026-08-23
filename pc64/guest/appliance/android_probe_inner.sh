#!/bin/sh
# Runs INSIDE an alpine container.  Builds the smallest rootfs that can answer
# ONE question: does a Waydroid container start on the UnoDOS appliance
# kernel?  Not a browser, not a compositor, not a window - binder, LXC,
# cgroups, and the images mount.
set -e
R=/rootfs
REL=v3.20
MIRROR=http://dl-cdn.alpinelinux.org/alpine

apk add --no-cache e2fsprogs >/dev/null

mkdir -p $R
apk add --root $R --initdb --no-cache \
    -X $MIRROR/$REL/main -X $MIRROR/$REL/community --allow-untrusted \
    alpine-baselayout busybox musl musl-utils \
    waydroid lxc python3 py3-dbus py3-gobject3 dbus \
    e2fsprogs util-linux iproute2 nftables \
    ca-certificates ca-certificates-bundle \
    cage wlroots seatd mesa-dri-gallium \
    eudev udev-init-scripts >/dev/null

# THE ROOT FILESYSTEM IS READ-ONLY, so every mount point and every writable
# directory has to exist NOW.  The first run of this probe learned that the
# hard way: `mkdir -p /usr/share/waydroid-extra/images` at boot failed, the
# runtime image had nowhere to mount, waydroid found no preinstalled images,
# and it fell through to trying to DOWNLOAD 1 GB - which then failed on DNS
# and was reported as a network error.  Three layers of wrong answer from one
# missing directory.
mkdir -p $R/usr/share/waydroid-extra/images
mkdir -p $R/var/lib/dbus $R/var/lib/waydroid $R/run/dbus
# DNS, baked, because /etc is read-only at run time and because waydroid init
# CANNOT TOLERATE BEING OFFLINE: helpers/http.py's retrieve() catches only
# ValueError and HTTPError, so the URLError a DNS failure raises escapes and
# kills init - even when the images it would otherwise download are already
# preinstalled and it is one line away from deciding it does not need them.
echo "nameserver 10.0.2.3" > $R/etc/resolv.conf

cat > $R/sbin/uno-init <<'EOF'
#!/bin/sh
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
say() { echo "PROBE: $*" > /dev/ttyS0; }

mount -t proc  proc /proc 2>/dev/null
mount -t sysfs sys  /sys  2>/dev/null
mount -t devtmpfs dev /dev 2>/dev/null
mkdir -p /dev/pts /dev/shm
mount -t devpts devpts /dev/pts 2>/dev/null
mount -t tmpfs -o mode=1777,size=256m shm /dev/shm 2>/dev/null
mount -t tmpfs tmp /tmp 2>/dev/null
mount -t tmpfs run /run 2>/dev/null
mount -t tmpfs var /var 2>/dev/null
mkdir -p /var/log /var/tmp /var/lib /var/cache /var/lib/dbus /var/lib/waydroid /run/dbus /root

mkdir -p /sys/fs/cgroup
mount -t cgroup2 cgroup2 /sys/fs/cgroup 2>/dev/null && say "cgroup2 mounted" \
    || say "cgroup2 MOUNT FAILED"

( while :; do setsid sh -c 'exec sh </dev/ttyS0 >/dev/ttyS0 2>&1'; sleep 1; done ) &

say "kernel: $(uname -r)"

# ---- 0. the CPU, which decides which IMAGES waydroid will look for --------
# tools/helpers/arch.py: x86_64 is remapped to "x86" when /proc/cpuinfo does
# not advertise sse4_2.  A guest without it therefore goes looking for x86
# images that this appliance does not carry, and says so in a line that reads
# like a note rather than a failure.
if grep -q sse4_2 /proc/cpuinfo; then say "cpu has sse4_2 (waydroid will use x86_64)"
else say "cpu LACKS sse4_2 - waydroid will fall back to x86 and find nothing"; fi

# ---- 1. binder ------------------------------------------------------------
# With CONFIG_ANDROID_BINDERFS the legacy /dev/binder nodes DO NOT exist at
# boot: binderfs is mounted and nodes are allocated through it, which is what
# waydroid's own probeBinderDriver does.  So the thing to check is the
# FILESYSTEM, and /proc/filesystems separates its two columns with a TAB.
if grep -qw binder /proc/filesystems; then say "binderfs available"
else say "binderfs ABSENT - the kernel has no binder"; fi

# ---- 2. the runtime image -------------------------------------------------
if [ -b /dev/vdb ]; then
    mount -o ro /dev/vdb /usr/share/waydroid-extra/images \
        && say "android.img mounted: $(ls /usr/share/waydroid-extra/images | tr '\n' ' ')" \
        || say "android.img MOUNT FAILED"
else
    say "no /dev/vdb - no android runtime image"
fi

# ---- 3. network, because waydroid init still asks the OTA server ----------
ip link set lo up 2>/dev/null
for i in eth0 ens3; do ip link set $i up 2>/dev/null; done
udhcpc -i eth0 -n -q -t 3 -T 2 >/dev/null 2>&1 && say "dhcp ok: $(ip -4 -o addr show eth0 2>/dev/null | awk '{print $4}')" \
    || say "no dhcp (init will have to tolerate an offline OTA fetch)"

# ---- 4. dbus, which the container manager registers on --------------------
dbus-uuidgen --ensure=/etc/machine-id 2>/dev/null
dbus-uuidgen --ensure 2>/dev/null
mkdir -p /run/dbus /var/run/dbus
( dbus-daemon --system --nofork > /tmp/dbus.log 2>&1 & )
sleep 2
if [ -S /run/dbus/system_bus_socket ] || [ -S /var/run/dbus/system_bus_socket ]; then
    say "dbus up"
else
    say "dbus FAILED:"; head -5 /tmp/dbus.log > /dev/ttyS0
fi

# ---- 5. waydroid ----------------------------------------------------------
say "waydroid version: $(waydroid --version 2>&1 | head -1)"
say "--- waydroid init ---"
waydroid init -f > /tmp/init.log 2>&1 || say "init returned non-zero"
tail -25 /tmp/init.log > /dev/ttyS0
say "--- cfg ---"
cat /var/lib/waydroid/waydroid.cfg > /dev/ttyS0 2>/dev/null || say "no waydroid.cfg"
say "rootfs dir: $(ls /var/lib/waydroid/rootfs 2>/dev/null | tr '\n' ' ')"

say "--- container service ---"
( waydroid container start > /tmp/container.log 2>&1 ) &
sleep 5

# A WAYLAND DISPLAY, HEADLESS.  Waydroid starts the Android container from a
# SESSION, and a session needs a compositor to put surfaces on - which is why
# the container sat at STOPPED with everything else working.  wlroots'
# headless backend gives a real Wayland socket with no display hardware at
# all, so this proves Android boots without also having to prove a
# framebuffer.  LIBSEAT_BACKEND=noop because there is no logind or seatd here
# and a headless compositor has no devices to be granted anyway.
# A WRITABLE HOME.  The session manager writes $HOME/.local/share/waydroid,
# and with HOME unset that is `//.local` on a read-only root - which reports
# as "[Errno 30] Read-only file system: '//.local'", a path with a doubled
# slash that looks like a bug in waydroid and is a missing variable here.
export HOME=/tmp/home
mkdir -p $HOME/.local/share
export XDG_RUNTIME_DIR=/run/user/0
mkdir -p $XDG_RUNTIME_DIR && chmod 700 $XDG_RUNTIME_DIR
export WLR_BACKENDS=headless LIBSEAT_BACKEND=noop WLR_RENDERER=pixman
( cage -- sleep 100000 > /tmp/cage.log 2>&1 ) &
sleep 6
SOCK=$(ls $XDG_RUNTIME_DIR 2>/dev/null | grep -m1 '^wayland-')
if [ -n "$SOCK" ]; then say "compositor up, WAYLAND_DISPLAY=$SOCK"
else say "compositor FAILED:"; head -6 /tmp/cage.log > /dev/ttyS0; fi
export WAYLAND_DISPLAY=${SOCK:-wayland-0}

# A SESSION BUS, which is a different bus from the system one.  waydroid
# session start talks to BOTH: the container service on the system bus, and
# its own session manager on the user bus.  With no DBUS_SESSION_BUS_ADDRESS
# it tries to autolaunch one, which on a machine with no X gives the
# memorable "Unable to autolaunch a dbus-daemon without a $DISPLAY for X11" -
# an X11 error message, on a Wayland-only appliance, for a missing env var.
DBUS_SESSION_BUS_ADDRESS=$(dbus-daemon --session --print-address --fork 2>/dev/null)
export DBUS_SESSION_BUS_ADDRESS
[ -n "$DBUS_SESSION_BUS_ADDRESS" ] && say "session bus: $DBUS_SESSION_BUS_ADDRESS" \
    || say "session bus FAILED"

say "--- session start ---"
( waydroid session start > /tmp/session.log 2>&1 ) &
i=0
while [ $i -lt 30 ]; do
    sleep 5
    i=$((i+1))
    S=$(waydroid status 2>/dev/null | tr '\n' ' ')
    say "status[$i]: $S"
    case "$S" in *RUNNING*) break;; esac
done
say "--- session log ---"
tail -15 /tmp/session.log > /dev/ttyS0 2>/dev/null
say "--- container log ---"
tail -40 /tmp/container.log > /dev/ttyS0 2>/dev/null
say "--- android init processes (the proof it really booted) ---"
ps aux 2>/dev/null | grep -iE "zygote|servicemanager|surfaceflinger|hwservice" \
    | grep -v grep | head -10 > /dev/ttyS0
say "--- lxc ---"
lxc-info -P /var/lib/waydroid/lxc -n waydroid 2>&1 | head -8 > /dev/ttyS0
say "--- what android says about itself ---"
waydroid prop get ro.product.model 2>&1 | head -2 > /dev/ttyS0
waydroid prop get ro.build.version.release 2>&1 | head -2 > /dev/ttyS0
say "--- installed packages (a real android package manager answering) ---"
waydroid app list 2>&1 | head -12 > /dev/ttyS0
say "PROBE-DONE"
exec sh
EOF
chmod +x $R/sbin/uno-init
ln -sf /sbin/uno-init $R/init

SZ=$(du -sm $R | cut -f1)
SZ=$((SZ * 130 / 100 + 64))
rm -f /out/rootfs-android-probe.img
truncate -s ${SZ}M /out/rootfs-android-probe.img
mke2fs -q -t ext4 -L uno-probe -d $R /out/rootfs-android-probe.img
ls -la /out/rootfs-android-probe.img
