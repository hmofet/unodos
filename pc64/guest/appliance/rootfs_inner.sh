#!/bin/sh
# Runs INSIDE an alpine container (see build_rootfs.sh).  Populates /rootfs
# with Alpine + Xorg + Chromium, writes the appliance's own init, and packs
# the tree into an ext4 image with mke2fs -d - no loop devices, no host root.
set -e

REL=v3.20
MIRROR=http://dl-cdn.alpinelinux.org/alpine
R=/rootfs

apk add --no-cache e2fsprogs >/dev/null

mkdir -p $R
apk add --root $R --initdb --no-cache \
    -X $MIRROR/$REL/main -X $MIRROR/$REL/community \
    --allow-untrusted \
    alpine-baselayout busybox musl musl-utils \
    eudev udev-init-scripts \
    xorg-server xf86-input-libinput xinit xrandr xset \
    mesa-dri-gallium \
    chromium \
    font-dejavu \
    dbus

# The appliance's init.  PID 1 after the initramfs switch_root.  The rootfs
# is mounted read-only off virtio-blk (the layer below can only read), so
# every writable path is a tmpfs.
cat > $R/sbin/uno-init <<'EOF'
#!/bin/sh
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
mount -t proc  proc /proc 2>/dev/null
mount -t sysfs sys  /sys  2>/dev/null
mount -t devtmpfs dev /dev 2>/dev/null
mkdir -p /dev/pts /dev/shm
mount -t devpts devpts /dev/pts 2>/dev/null
mount -t tmpfs -o mode=1777,size=256m shm /dev/shm 2>/dev/null
mount -t tmpfs tmp  /tmp  2>/dev/null
mount -t tmpfs run  /run  2>/dev/null
mount -t tmpfs var  /var  2>/dev/null
mkdir -p /var/log /var/tmp /var/lib /var/cache /run/dbus /root
mount -t tmpfs home /root 2>/dev/null

# Swap on zram: Chromium in a small carve wants somewhere to spill.
if [ -e /sys/class/zram-control ] || [ -e /sys/block/zram0 ]; then
    echo lz4 > /sys/block/zram0/comp_algorithm 2>/dev/null
    echo 512M > /sys/block/zram0/disksize 2>/dev/null
    mkswap /dev/zram0 >/dev/null 2>&1 && swapon /dev/zram0 2>/dev/null
fi

# A shell on the serial port, always: it is the appliance's back door and
# the harness's front door.
(
  while :; do
    setsid sh -c 'exec sh </dev/ttyS0 >/dev/ttyS0 2>&1'
    sleep 1
  done
) &

# Input devices need udev (libinput enumerates through it).
udevd -d 2>/dev/null
udevadm trigger --action=add 2>/dev/null
udevadm settle -t 10 2>/dev/null

dbus-daemon --system 2>/dev/null

export HOME=/root XDG_RUNTIME_DIR=/run
export XDG_CACHE_HOME=/tmp/cache XDG_CONFIG_HOME=/tmp/config
mkdir -p /tmp/cache /tmp/config

# The wire.  /etc is read-only, so /etc/resolv.conf is a symlink into tmpfs
# (made at build time) and udhcpc's script writes through it.  Backgrounded
# with a retry: the host bridge only carries frames once the appliance is
# running, and a lease that is not there yet is not a lease that never comes.
: > /tmp/resolv.conf
(
  n=0
  while [ $n -lt 30 ]; do
    ip link set eth0 up 2>/dev/null
    if udhcpc -i eth0 -t 4 -T 3 -n -q >/tmp/dhcp.log 2>&1; then
      echo "uno: lease $(ip -4 addr show eth0 | grep -o 'inet [0-9.]*')" \
          > /dev/ttyS0
      echo "uno: dns $(cat /tmp/resolv.conf | tr '\n' ' ')" > /dev/ttyS0
      break
    fi
    n=$((n + 1))
    # SAY SO WHEN IT FAILS.  A loop that only reports success is a loop that
    # reads identically to one that never ran, and the difference decides
    # whether to look at the guest or at the bridge.
    echo "uno: no lease yet (try $n)" > /dev/ttyS0
    sleep 2
  done
) &

# X plus Chromium, restarted if either dies.  Software rendering: the guest
# has simpledrm and no GPU, and Chromium's own rasteriser is the fast path.
#
# NOT --kiosk: the point of A8's input path is that this is a browser somebody
# can DRIVE - the address bar, ctrl-L, a link under the pointer.  A kiosk with
# no chrome proves rendering and nothing else.
URL=${UNO_URL:-https://example.com}
while :; do
    xinit /usr/bin/chromium \
        --no-sandbox --disable-gpu --disable-dev-shm-usage \
        --no-first-run --no-default-browser-check --disable-infobars \
        --password-store=basic --disable-sync \
        --window-position=0,0 --window-size=800,600 \
        --start-maximized "$URL" \
        -- /usr/bin/X :0 vt1 -nolisten tcp -quiet \
        >/tmp/x.log 2>&1
    sleep 2
done
EOF
chmod +x $R/sbin/uno-init

# DNS on a read-only root: udhcpc's script writes /etc/resolv.conf, so that
# path is a symlink into the tmpfs the appliance mounts over /tmp.  Without
# it every name lookup fails while the lease itself is perfectly fine.
ln -sf /tmp/resolv.conf $R/etc/resolv.conf

mkdir -p $R/usr/share/uno
cat > $R/usr/share/uno/welcome.html <<'EOF'
<!doctype html>
<meta charset="utf-8">
<title>UnoDOS appliance</title>
<style>
  body { background:#1a1a2e; color:#eee; font-family:sans-serif;
         display:flex; align-items:center; justify-content:center;
         height:100vh; margin:0; }
  .card { text-align:center; }
  h1 { font-size:2.2em; margin-bottom:.2em; }
  p  { color:#9ad; }
</style>
<div class="card">
  <h1>Chromium, inside UnoDOS</h1>
  <p>Blink is rendering this page in a Linux appliance under unovirt.</p>
  <p id=t></p>
  <script>
    document.getElementById('t').textContent =
      navigator.userAgent + ' - JS is live: 6*7=' + (6*7);
  </script>
</div>
EOF

# Pack.  Size: tree + 15% headroom, floor 256 MB.
KB=$(du -sk $R | cut -f1)
MB=$(( KB / 1024 * 115 / 100 + 64 ))
[ $MB -lt 256 ] && MB=256
rm -f /out/rootfs.img
mke2fs -q -t ext4 -d $R -L unoroot /out/rootfs.img ${MB}M
echo "rootfs.img: ${MB} MB ($(( KB / 1024 )) MB of files)"
