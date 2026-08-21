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

# THE WIRE COMES UP BEFORE THE BROWSER DOES, and that ordering is the whole
# difference between a browser and a screenshot of an error page: Chromium
# navigates once at startup and CACHES the failure, so a guest whose lease
# arrives thirty seconds later sits on "This site can't be reached" forever
# while its network works perfectly.  That is exactly what the first bridged
# run produced.  So this loop is in the FOREGROUND.
: > /tmp/resolv.conf
n=0
while [ $n -lt 40 ]; do
  ip link set eth0 up 2>/dev/null
  if udhcpc -i eth0 -t 4 -T 3 -n -q -s /usr/share/uno/dhcp.script \
            >/tmp/dhcp.log 2>&1; then
    echo "uno: lease $(ip -4 addr show eth0 | grep -o 'inet [0-9.]*')" > /dev/ttyS0
    echo "uno: route $(ip route | grep -o 'default.*' | head -1)" > /dev/ttyS0
    echo "uno: dns $(tr '\n' ' ' < /tmp/resolv.conf)" > /dev/ttyS0
    break
  fi
  n=$((n + 1))
  # SAY SO WHEN IT FAILS.  A loop that only reports success is a loop that
  # reads identically to one that never ran, and the difference decides
  # whether to look at the guest or at the bridge.
  echo "uno: no lease yet (try $n)" > /dev/ttyS0
  sleep 2
done

# THE PROOF LINE, and the gate the browser waits on.  A screenshot of a
# browser can be a cached page or an error page misread at a glance; a title
# fetched by a different program entirely says the guest reached the
# internet through the bridge, in one line, before Chromium is asked to.
n=0
while [ $n -lt 20 ]; do
  T=$(wget -q -T 10 -O - http://example.com 2>/tmp/wget.err \
      | tr -d '\n' | grep -o '<title>[^<]*' | head -c 48)
  if [ -n "$T" ]; then echo "uno: fetch OK $T" > /dev/ttyS0; break; fi
  n=$((n + 1))
  echo "uno: fetch failed ($n) $(head -c 50 /tmp/wget.err | tr '\n' ' ')" \
      > /dev/ttyS0
  sleep 3
done

# X plus Chromium, restarted if either dies.  Software rendering: the guest
# has simpledrm and no GPU, and Chromium's own rasteriser is the fast path.
#
# NOT --kiosk: the point of A8's input path is that this is a browser somebody
# can DRIVE - the address bar, ctrl-L, a link under the pointer.  A kiosk with
# no chrome proves rendering and nothing else.
#
# THE BACKGROUND DOWNLOADS HAVE TO GO.  Chromium's component updater, variation
# seed and safe-browsing lists are tens of megabytes, and a quarter-core guest
# behind a bridged link spends its whole first minutes on them instead of on
# the page somebody asked for - 20,213 frames arrived on the first bridged run
# and none of them were the website.  Every flag below turns off traffic
# nobody asked for; none of them touch rendering.
URL=${UNO_URL:-http://example.com}
while :; do
    xinit /usr/bin/chromium \
        --no-sandbox --disable-gpu --disable-dev-shm-usage \
        --no-first-run --no-default-browser-check --disable-infobars \
        --password-store=basic --disable-sync \
        --disable-background-networking --disable-component-update \
        --disable-domain-reliability --disable-breakpad \
        --disable-client-side-phishing-detection --no-pings \
        --safebrowsing-disable-auto-update --metrics-recording-only \
        --disable-features=OptimizationHints,MediaRouter \
        --window-position=0,0 --window-size=800,600 \
        --start-maximized "$URL" \
        -- /usr/bin/X :0 vt1 -nolisten tcp -quiet \
        >/tmp/x.log 2>&1
    sleep 2
done
EOF
chmod +x $R/sbin/uno-init

# DNS on a read-only root.  /etc/resolv.conf is a symlink into the tmpfs the
# appliance mounts over /tmp - but that alone is NOT enough, and the first
# bridged run proved it: the guest took a perfectly good lease (10.0.2.16)
# and still had no DNS, because the stock Alpine udhcpc script writes its
# resolv.conf through a TEMP FILE beside the target, and /etc is read-only.
# So udhcpc gets a script of ours that writes the tmpfs path directly.
ln -sf /tmp/resolv.conf $R/etc/resolv.conf
mkdir -p $R/usr/share/uno
cat > $R/usr/share/uno/dhcp.script <<'DHCPEOF'
#!/bin/sh
# busybox udhcpc hands its result in the environment: $ip $mask $router $dns.
case "$1" in
  bound|renew)
    ip addr flush dev "$interface" 2>/dev/null
    ip addr add "$ip/${mask:-24}" dev "$interface"
    ip link set "$interface" up
    for r in $router; do
        ip route add default via "$r" dev "$interface" 2>/dev/null && break
    done
    : > /tmp/resolv.conf
    for s in $dns; do echo "nameserver $s" >> /tmp/resolv.conf; done
    ;;
esac
exit 0
DHCPEOF
chmod +x $R/usr/share/uno/dhcp.script

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
