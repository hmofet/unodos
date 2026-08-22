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
    xorg-server xf86-input-libinput xf86-video-fbdev xinit xrandr xset \
    mesa-dri-gallium \
    chromium \
    font-dejavu \
    dbus \
    ca-certificates ca-certificates-bundle \
    openbox xdotool cage seatd wlroots

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

# Swap on zram: Chromium in a small carve wants somewhere to spill, and it
# spills a LOT - a renderer that runs out dies as "Aw, Snap! Error code: 8",
# which looks like a browser bug and is a memory ceiling.  Sized against the
# carve rather than fixed.
if [ -e /sys/class/zram-control ] || [ -e /sys/block/zram0 ]; then
    echo lz4 > /sys/block/zram0/comp_algorithm 2>/dev/null
    echo 1024M > /sys/block/zram0/disksize 2>/dev/null
    mkswap /dev/zram0 >/dev/null 2>&1 && swapon /dev/zram0 2>/dev/null
    echo 100 > /proc/sys/vm/swappiness 2>/dev/null
fi

# A shell on the serial port, always: it is the appliance's back door and
# the harness's front door.
(
  while :; do
    setsid sh -c 'exec sh </dev/ttyS0 >/dev/ttyS0 2>&1'
    sleep 1
  done
) &

# INPUT DEVICES NEED UDEV, and this used to fail in silence.  Xorg's
# libinput driver enumerates through udev, so a udevd that is not running
# means an X server with no keyboard and no mouse - which looks exactly like
# an emulated i8042 that does not work, and sent a whole run chasing the
# wrong layer.  `2>/dev/null` on the daemon that matters is how that hid.
if [ -x /sbin/udevd ]; then UDEVD=/sbin/udevd
elif [ -x /lib/udev/udevd ]; then UDEVD=/lib/udev/udevd
else UDEVD=$(command -v udevd 2>/dev/null); fi
if [ -n "$UDEVD" ] && $UDEVD --daemon; then
    echo "uno: udevd up ($UDEVD)" > /dev/ttyS0
else
    echo "uno: udevd MISSING - X will have no input" > /dev/ttyS0
fi
udevadm trigger --action=add 2>/dev/null
udevadm settle -t 10 2>/dev/null
# What the kernel actually created, which is the other half of the question.
echo "uno: input nodes: $(ls /dev/input 2>/dev/null | tr '\n' ' ')" > /dev/ttyS0

# DOES THE KERNEL'S INPUT LAYER EVER PRODUCE AN EVENT?  Interrupt counts
# cannot answer this: a driver's own probe generates dozens of them, so
# irq1=85 says nothing about whether a single keystroke arrived.  One
# blocking read of the evdev node does say it, in one word, for each device.
# Armed early so they are already waiting when the harness types.
( for d in /dev/input/event0 /dev/input/event1 /dev/input/event2; do
      [ -e "$d" ] || continue
      ( if dd if="$d" bs=24 count=1 >/dev/null 2>&1; then
            echo "uno: EVDEV EVENT on $d" > /dev/ttyS0
        else
            echo "uno: evdev read failed on $d" > /dev/ttyS0
        fi ) &
  done ) &

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
#
# TWO SEPARATE CLAIMS, because they fail for different reasons and a single
# "it did not work" cannot tell them apart: whether packets reach the
# internet at all (an IP with no name in it), and whether names resolve.
UNO_SITE=${UNO_SITE:-unodos.arinbakht.com}
# 1) AN IP WITH NO NAME AND NO TLS IN IT.  `wget http://1.1.1.1` is the wrong
# probe and cost a round trip: it redirects to HTTPS, so its failure was a
# certificate error that read as a dead network while TCP was working
# perfectly.  A bare TCP connect claims exactly one thing.
n=0
while [ $n -lt 20 ]; do
  if nc -w 4 1.1.1.1 443 </dev/null >/dev/null 2>&1; then
    echo "uno: ip-ok (tcp to 1.1.1.1:443 through the bridge)" > /dev/ttyS0
    break
  fi
  n=$((n + 1)); echo "uno: ip failed ($n)" > /dev/ttyS0; sleep 3
done
# 2) A name.
n=0
while [ $n -lt 20 ]; do
  A=$(nslookup "$UNO_SITE" 2>/dev/null | grep -A2 "^Name:" | grep -o \
      '[0-9]\+\.[0-9]\+\.[0-9]\+\.[0-9]\+' | head -1)
  if [ -n "$A" ]; then echo "uno: dns-ok $UNO_SITE -> $A" > /dev/ttyS0; break; fi
  n=$((n + 1)); echo "uno: dns failed ($n)" > /dev/ttyS0; sleep 3
done
# 3) The whole stack, including TLS against real roots - which is the same
# work Chromium is about to do, done by a different program, so a browser
# showing a page is confirmed rather than interpreted.
n=0
while [ $n -lt 15 ]; do
  T=$(wget -q -T 15 -O - "https://$UNO_SITE/" 2>/tmp/wget.err \
      | tr -d '\n' | grep -o '<title>[^<]*' | head -c 48)
  if [ -n "$T" ]; then echo "uno: https-ok $T" > /dev/ttyS0; break; fi
  n=$((n + 1))
  echo "uno: https failed ($n) $(head -c 60 /tmp/wget.err | tr '\n' ' ')" > /dev/ttyS0
  sleep 4
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
#
# A WINDOW MANAGER, because focus is not decoration.  With no WM, X gives the
# keyboard to whatever the pointer happens to be over (PointerRoot), so a
# browser can be rendering perfectly and still receive nothing typed at it -
# which is what a whole run looked like, with the keyboard, the input nodes
# and udev all provably fine.  openbox takes the window, focuses it, and the
# keys land.
export UNO_URL_ENV=${UNO_URL:-http://example.org/}

# UNCONDITIONAL, because the last version waited for a file that did not
# always arrive and then reported nothing at all - a diagnostic that can
# block is a diagnostic that is silent exactly when it is needed.
#
# IRQ1 is the guest's own count of keyboard interrupts.  Paired with the
# emulated controller's count on the host side, the two together say which
# side of the wire a missing keystroke went missing on.
(
  n=0
  while [ $n -lt 40 ]; do
    n=$((n + 1))
    sleep 20
    echo "uno: irq1=$(awk '/^ *1:/ {print $2}' /proc/interrupts) irq12=$(awk '/^ *12:/ {print $2}' /proc/interrupts) ptr=$(DISPLAY=:0 xdotool getmouselocation 2>/dev/null | tr ' ' ',')" \
        > /dev/ttyS0
  done
) &

# THE SELF-TEST THAT SPLITS THE REMAINING QUESTION.  Five runs have shown a
# browser that renders and will not take a keystroke, with the emulated
# keyboard provably emitting and the guest provably draining it.  So: type
# the same navigation from INSIDE the guest with xdotool.  If the page
# changes, X and Chromium are fine and the gap is between libinput and my
# emulated device; if it does not, the gap is above them both.  Either way
# the next change is aimed rather than guessed.
(
  # What the browser says WHILE it runs.  Reporting only on exit means a
  # renderer that dies inside a browser that lives is never explained.
  while :; do
    sleep 90
    grep -iE "libinput|device|seat|keyboard|pointer" /tmp/x.log 2>/dev/null | tail -4 | sed 's/^/unoB| /' > /dev/ttyS0
  done
) &
(
  export DISPLAY=:0
  sleep 60
  W=$(xdotool search --class -- chromium 2>/dev/null | tail -1)
  echo "uno: selftest window=$W" > /dev/ttyS0
  [ -n "$W" ] && xdotool windowactivate --sync "$W" 2>/dev/null
  xdotool key --clearmodifiers ctrl+l 2>/dev/null
  sleep 1
  # A DIFFERENT SITE from the one the host harness types later, so the two
  # navigations cannot be mistaken for each other in a screenshot.
  xdotool type --delay 120 "example.net" 2>/dev/null
  xdotool key Return 2>/dev/null
  echo "uno: selftest typed example.net from inside" > /dev/ttyS0
) &

# seatd hands out the DRM and input devices a compositor needs.  Alpine's
# libseat is built WITHOUT the builtin backend, so without this daemon cage
# fails at once with "No backend matched name" and nothing is displayed.
if [ -x /usr/bin/seatd ] || [ -x /sbin/seatd ]; then
    (seatd -g video >/tmp/seatd.log 2>&1 &) 
    sleep 2
    echo "uno: seatd $([ -S /run/seatd.sock ] && echo up || echo MISSING)" > /dev/ttyS0
fi

URL=$UNO_URL_ENV
export XDG_RUNTIME_DIR=/run
export LIBSEAT_BACKEND=seatd
export WLR_RENDERER=pixman
# NO WLR_BACKENDS.  Naming "drm" does not mean "the DRM one as well as the
# usual" - it means ONLY that one, and wlroots' libinput backend is what
# reads the keyboard and mouse.  Setting it produced a compositor that drew
# perfectly and could not be typed at, which is indistinguishable from a
# broken emulated keyboard and was chased as one.  Autocreate picks DRM plus
# libinput, which is what a compositor on real hardware wants.

# NO PIPELINE AROUND EITHER SESSION.  busybox grep has no --line-buffered:
# the filter exited at once, tee took SIGPIPE, and the session died with it -
# which is why a browser that had been running perfectly vanished and left a
# bare console behind.  Logs are files; their tails are reported when a
# session ends, which is the moment worth reading them.
while :; do
    if [ -x /usr/bin/cage ]; then
        # ON A VT, AND HOLDING IT.  Without logind, libseat's seatd backend
        # ties input to the seat's ACTIVE virtual terminal: a compositor
        # started from a serial-console shell gets DRM master (so it draws
        # perfectly) while libinput keeps its devices PAUSED - which is a
        # browser that renders and cannot be typed at, exactly what the
        # first Wayland run produced.
        chvt 1 2>/dev/null
        # -d so the compositor says which input devices libinput gave it.
        # Keys provably reach this guest's evdev and provably do not reach
        # the browser, and the only layer left between them is this one.
        # -D is debug; -d means "no client-side decorations" and cost a run.
        # -s allows VT switching, which is also what makes libseat treat this
        # session as one that can OWN the seat rather than share it.
        cage -D -s -- /usr/share/uno/browser.sh < /dev/tty1 > /tmp/x.log 2>&1
        RC=$?
        echo "uno: wayland session ended rc=$RC ----" > /dev/ttyS0
        tail -10 /tmp/x.log | sed 's/^/uno| /' > /dev/ttyS0
        # If the compositor cannot start at all, fall back to X rather than
        # spinning: a guest with no display is worse than one with a fragile
        # display.
        if [ $RC -ne 0 ] && [ ! -f /tmp/wayland-failed ]; then
            : > /tmp/wayland-failed
            echo "uno: falling back to X" > /dev/ttyS0
        fi
    fi
    if [ -f /tmp/wayland-failed ] || [ ! -x /usr/bin/cage ]; then
        xinit /usr/share/uno/session.sh \
          -- /usr/bin/X :0 vt1 -nolisten tcp -quiet > /tmp/x.log 2>&1
        echo "uno: X session ended ----" > /dev/ttyS0
        tail -10 /tmp/x.log | sed 's/^/uno| /' > /dev/ttyS0
    fi
    sleep 2
done
EOF
chmod +x $R/sbin/uno-init

# XORG ON A DUMB FRAMEBUFFER MUST NOT TRY TO ACCELERATE.  UnoDOS gives the
# guest a plain linear framebuffer through simpledrm - no GPU, no render
# node, no GL - and modesetting's default glamor acceleration NULL-derefs on
# exactly that: "(EE) Segmentation fault at address 0x0", minutes into a
# session that had been rendering perfectly.  It never happened under plain
# QEMU because a VGA gives a real DRM driver to work with, which is why this
# took a hypervisor run to see.  ShadowFB keeps the drawing off the slow
# framebuffer until it has to land there.
mkdir -p $R/etc/X11/xorg.conf.d
cat > $R/etc/X11/xorg.conf.d/20-uno-fb.conf <<'XCONFEOF'
Section "Device"
    Identifier  "uno-fb"
    Driver      "modesetting"
    Option      "AccelMethod"  "none"
    Option      "ShadowFB"     "true"
EndSection
XCONFEOF

mkdir -p $R/usr/share/uno
cat > $R/usr/share/uno/session.sh <<'SESSEOF'
#!/bin/sh
# WAYLAND FIRST, X ONLY AS A FALLBACK.  Xorg segfaults on this guest's
# framebuffer - simpledrm, no GPU, no render node - and did so with
# modesetting, with acceleration off, and (differently) with fbdev.  A
# wlroots compositor with the pixman software renderer is what modern
# systems actually run on a dumb framebuffer, and it needs no X server, no
# window manager and no libinput-inside-X: cage takes the DRM device
# directly, reads evdev itself, and gives its single client the whole
# screen, focused, forever.
#
# LIBSEAT_BACKEND=builtin is what lets it take DRM master as root without
# logind or a seatd daemon.
openbox &
sleep 3

# A FRESH PROFILE EVERY LAUNCH.  Chromium remembers that it did not shut
# down cleanly and opens a "Restore pages?" bubble - and that bubble takes
# the keyboard focus, so the first thing typed at the browser goes into a
# dialog instead of the address bar.  --disable-session-crashed-bubble did
# not suppress it in this build; deleting the profile removes the belief
# that produces it.
rm -rf /tmp/config/chromium /tmp/cache/chromium 2>/dev/null

# AND THE BROWSER WINDOW KEEPS THE FOCUS.  A popup that appears later still
# takes it, so this re-asserts it for the first couple of minutes rather
# than once.
(
  for i in $(seq 1 24); do
    sleep 5
    W=$(xdotool search --class -- chromium 2>/dev/null | tail -1)
    [ -n "$W" ] && xdotool windowactivate "$W" 2>/dev/null
  done
) &

exec chromium \
    --no-sandbox --disable-gpu --disable-dev-shm-usage \
    --no-first-run --no-default-browser-check --disable-infobars \
    --password-store=basic --disable-sync \
    --disable-background-networking --disable-component-update \
    --disable-domain-reliability --disable-breakpad \
    --disable-client-side-phishing-detection --no-pings \
    --safebrowsing-disable-auto-update --metrics-recording-only \
    # Chromium's own async resolver (and Secure DNS) ignore the
    # resolv.conf that nslookup and wget both use happily, and answer
    # ERR_NAME_NOT_RESOLVED for names the rest of the guest resolves
    # fine.  Sending it back to getaddrinfo makes the browser agree
    # with its own operating system.
    --disable-features=OptimizationHints,MediaRouter,AsyncDns,DnsOverHttps \
    --renderer-process-limit=1 --process-per-site \
    --disable-hang-monitor --disable-session-crashed-bubble \
    --js-flags=--max-old-space-size=192 \
    --window-position=0,0 --window-size=800,600 \
    --start-maximized "$UNO_URL_ENV"
SESSEOF
chmod +x $R/usr/share/uno/session.sh

# The Wayland client cage runs: Chromium on Ozone, with the same flags the X
# session uses.  No window manager and no focus problem - cage gives its one
# client the whole output, focused, for as long as it lives.
cat > $R/usr/share/uno/browser.sh <<'BROWSEREOF'
#!/bin/sh
rm -rf /tmp/config/chromium /tmp/cache/chromium 2>/dev/null
exec chromium     --ozone-platform=wayland     --no-sandbox --disable-gpu --disable-dev-shm-usage     --no-first-run --no-default-browser-check --disable-infobars     --password-store=basic --disable-sync     --disable-background-networking --disable-component-update     --disable-domain-reliability --disable-breakpad     --disable-client-side-phishing-detection --no-pings     --safebrowsing-disable-auto-update --metrics-recording-only     --disable-features=OptimizationHints,MediaRouter,AsyncDns,DnsOverHttps     --renderer-process-limit=1 --process-per-site     --disable-hang-monitor --disable-session-crashed-bubble     --js-flags=--max-old-space-size=192     --start-maximized "$UNO_URL_ENV"
BROWSEREOF
chmod +x $R/usr/share/uno/browser.sh

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
    # A PUBLIC RESOLVER FIRST, then whatever DHCP offered.  The offered one
    # here is the emulator's own proxy, which forwards to the host's resolver
    # - and that resolver is somebody else's LAN box, which on this rig
    # answers for some names and not others (example.com resolved nowhere
    # while cloudflare.com resolved fine).  A guest that inherits a broken
    # resolver looks exactly like a guest with no network at all.
    # ONE RESOLVER, NOT A LIST WITH A GOOD ONE AT THE TOP.  musl queries
    # every nameserver in PARALLEL and takes the first reply, so a broken
    # LAN resolver that answers NXDOMAIN quickly beats a correct public one
    # that answers properly a few milliseconds later - which is why names
    # resolved intermittently and differently per run.  Order is not
    # precedence here; presence is.
    : > /tmp/resolv.conf
    echo "nameserver 1.1.1.1" >> /tmp/resolv.conf
    echo "nameserver 8.8.8.8" >> /tmp/resolv.conf
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
