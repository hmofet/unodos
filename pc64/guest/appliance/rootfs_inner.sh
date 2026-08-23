#!/bin/sh
# Runs INSIDE an alpine container (see build_rootfs.sh).  Populates /rootfs
# with Alpine, a display stack and ONE application, writes the appliance's own
# init, and packs the tree into an ext4 image with mke2fs -d - no loop devices,
# no host root.
#
# WHICH APPLICATION IS A PARAMETER, and everything in this file is the part
# that does not care which.  `apps/<name>.app` names the packages, the
# compositor and the client script; adding an appliance is adding one such
# file, not editing this one.  The contract is in `apps/README.md`.
set -e

REL=v3.20
MIRROR=http://dl-cdn.alpinelinux.org/alpine
R=/rootfs

APP=${UNO_APP:-chromium}
APPDEF=/scripts/apps/$APP.app
[ -f "$APPDEF" ] || { echo "uno-build: no such appliance: $APP ($APPDEF)" >&2; exit 1; }

# Defaults an app file may override.  Declared here so a typo in one of them
# reads as "the default was used" rather than as an empty command line.
APP_PKGS=""
APP_COMPOSITOR=cage
APP_CLIENT=/usr/share/uno/app.sh
APP_XCLIENT=""              # the X fallback session, if the app has one
APP_SELFTEST=""             # optional /usr/share/uno/... run once after boot
APP_ENV=""                  # one line appended verbatim to app.env
# The app-registry half.  Not consumed by anything yet - the foreign-package
# installer and its shim `.UNO` are docs/ANDROID-APPLIANCE-PLAN.md §3, and
# `unopkg` does not exist.  Declared here anyway, and written to a sidecar
# descriptor beside the image, because the alternative is that when the shim
# generator lands, an appliance's name, category and launch target get typed
# in a SECOND place and drift from the one that built it.
APP_ID=$APP
APP_TITLE=""
APP_CAT=""
APP_HOST=linux
APP_LAUNCH=""
app_write() { :; }
. "$APPDEF"
echo "uno-build: appliance '$APP' -> compositor $APP_COMPOSITOR, client $APP_CLIENT"

apk add --no-cache e2fsprogs >/dev/null

mkdir -p $R
# THE BASE, and only the base: everything every appliance needs whatever it
# runs.  An app's own packages come from its .app file, so a rootfs never
# carries a browser because the window manager happened to be listed next to
# one.
apk add --root $R --initdb --no-cache \
    -X $MIRROR/$REL/main -X $MIRROR/$REL/community \
    --allow-untrusted \
    alpine-baselayout busybox musl musl-utils \
    eudev udev-init-scripts \
    xorg-server xf86-input-libinput xf86-video-fbdev xinit xrandr xset \
    mesa-dri-gallium \
    font-dejavu \
    dbus dbus-x11 \
    ca-certificates ca-certificates-bundle \
    openbox xdotool seatd wlroots \
    wtype

# shellcheck disable=SC2086
[ -n "$APP_PKGS" ] && apk add --root $R --no-cache \
    -X $MIRROR/$REL/main -X $MIRROR/$REL/community \
    --allow-untrusted $APP_PKGS

# The input diagnostics.  Separate, and each allowed to be missing, because a
# name that has moved between Alpine releases must not take the whole rootfs
# down with it - and a build that dies here leaves the LAST image in place,
# which is far worse than an appliance with one tool short.
for p in libinput-tools evtest wev; do
    apk add --root $R --no-cache -X $MIRROR/$REL/main -X $MIRROR/$REL/community \
        --allow-untrusted "$p" >/dev/null 2>&1 \
        || echo "uno-build: optional package $p is not available" >&2
done

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

# WHAT THE INPUT STACK ACTUALLY SAW, named rather than counted.  Interrupt
# counts say bytes arrived; this says which KEY each one became, at the layer
# every Wayland compositor reads from.  It answers the question a missing
# keystroke always raises - guest input layer, or everything above it - in
# one line per key.
#
# STRAIGHT AT THE TTY, NEVER THROUGH A PIPE.  The previous version was
# `od ... | sed ... > /dev/ttyS0`, and it printed nothing: od block-buffers
# when its stdout is a pipe, so a diagnostic that fires a dozen times a
# minute held its whole answer in a 4 KB buffer and flushed it never.  A
# character device is line-buffered; the pipe was the bug, not the tool.
#
# The keyboard node BY PATH, so this cannot drift onto the mouse when the
# enumeration order changes - and only the keyboard, because pointer motion
# would bury the line worth reading.
KBDNODE=/dev/input/by-path/platform-i8042-serio-0-event-kbd
if [ -x /usr/bin/libinput ] && [ -e "$KBDNODE" ]; then
    libinput debug-events --show-keycodes --device "$KBDNODE" \
        > /dev/ttyS0 2>&1 &
    echo "uno: libinput debug-events on $KBDNODE" > /dev/ttyS0
else
    echo "uno: no libinput debug-events (tool $([ -x /usr/bin/libinput ] && echo yes || echo MISSING))" > /dev/ttyS0
fi

( for d in /dev/input/event0 /dev/input/event1 /dev/input/event2; do
      [ -e "$d" ] || continue
      ( if dd if="$d" bs=24 count=1 >/dev/null 2>&1; then
            echo "uno: EVDEV EVENT on $d" > /dev/ttyS0
        else
            echo "uno: evdev read failed on $d" > /dev/ttyS0
        fi ) &
  done ) &

dbus-daemon --system 2>/dev/null

# A SESSION BUS, WHICH IS THE ONE A DESKTOP APPLICATION ACTUALLY WANTS.  The
# line above starts the SYSTEM bus, and for the browser appliance that was
# enough - so this gap sat here unnoticed until an app came along that uses
# the session bus for real.  GIMP registers `org.gimp.GIMP.UI` on it for
# single-instance behaviour; with no bus and no `dbus-launch` on the image it
# printed
#
#     Failed to execute child process "dbus-launch" (No such file or directory)
#     gui_dbus_name_lost: connection to the bus cannot be established.
#     gimp: fatal error: Segmentation fault
#
# and died on a loop under the hypervisor.  It survived the same missing bus
# under plain QEMU, which is the worst way for a fault to behave: the fast
# loop said the appliance was fine.
#
# `dbus-launch` is in dbus-x11, NOT in dbus - installing "dbus" and expecting
# the launcher is the whole mistake in one line.
#
# A MACHINE ID FIRST, on a read-only root.  dbus refuses to start without one,
# and /etc cannot be written - so /etc/machine-id is a symlink (made at build
# time, below) into the tmpfs this fills in now.
mkdir -p /var/lib/dbus
[ -s /var/lib/dbus/machine-id ] || dbus-uuidgen > /var/lib/dbus/machine-id 2>/dev/null
if [ -x /usr/bin/dbus-launch ]; then
    eval "$(dbus-launch --sh-syntax 2>/dev/null)" 2>/dev/null
    export DBUS_SESSION_BUS_ADDRESS
fi
# NAMED, NOT ASSUMED.  An empty address here is a client that will die minutes
# later for a reason that looks nothing like this line.
echo "uno: session bus ${DBUS_SESSION_BUS_ADDRESS:-MISSING} (machine-id $([ -s /var/lib/dbus/machine-id ] && echo ok || echo MISSING))" > /dev/ttyS0

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
# WHICH APPLIANCE THIS IS, read rather than compiled in.  app.env is written
# by the app's own .app file at build time and names the compositor, the
# client, the X fallback and the self-test.  A missing file is a build fault
# and is said out loud, because the fallbacks below would otherwise start a
# session with no client in it and look like a compositor that draws nothing.
UNO_APP=unknown
UNO_COMPOSITOR=cage
UNO_CLIENT=
UNO_XCLIENT=
UNO_SELFTEST=
if [ -f /usr/share/uno/app.env ]; then
    . /usr/share/uno/app.env
    echo "uno: appliance $UNO_APP ($UNO_COMPOSITOR -> $UNO_CLIENT)" > /dev/ttyS0
else
    echo "uno: NO /usr/share/uno/app.env - the rootfs was built wrong" > /dev/ttyS0
fi

# UNCONDITIONAL, because the last version waited for a file that did not
# always arrive and then reported nothing at all - a diagnostic that can
# block is a diagnostic that is silent exactly when it is needed.
#
# IRQ1 is the guest's own count of keyboard interrupts.  Paired with the
# emulated controller's count on the host side, the two together say which
# side of the wire a missing keystroke went missing on.
#
# SUM EVERY CPU COLUMN.  `print $2` is CPU0 alone, and on an SMP guest the
# i8042 interrupt lands wherever the affinity mask sends it - which was CPU1,
# so this line reported `irq1=0` through runs in which every keystroke
# arrived.  A counter that reads zero while the thing it counts is working is
# worse than no counter: it accuses the layer underneath it.
irqcount() {
  awk -v want="$1" '
    { n = $1; sub(/:$/, "", n); if (n != want) next;
      t = 0; for (i = 2; i <= NF; i++) if ($i ~ /^[0-9]+$/) t += $i; print t }
  ' /proc/interrupts
}
(
  n=0
  while [ $n -lt 40 ]; do
    n=$((n + 1))
    sleep 20
    echo "uno: irq1=$(irqcount 1) irq12=$(irqcount 12)" > /dev/ttyS0
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
  # What the session says WHILE it runs.  Reporting only on exit means a
  # renderer that dies inside a client that lives is never explained.
  while :; do
    sleep 90
    grep -iE "libinput|device|seat|keyboard|pointer" /tmp/x.log 2>/dev/null | tail -4 | sed 's/^/unoB| /' > /dev/ttyS0
  done
) &
#
# THE APP OWNS ITS OWN SELF-TEST, because the question it answers is
# app-shaped: for a browser it is "did a keystroke reach the omnibox", for an
# image editor it is "did a click land on the toolbox".  What is NOT
# app-shaped, and stays here, is the discipline the browser's version paid
# for: a self-test that cannot fail proves nothing.  xdotool speaks X and
# nothing else, so on the Wayland session it ran, found no display, failed
# every command and reported success anyway - which hid the fact that the
# appliance had moved off X.  An app's self-test therefore has to CHECK which
# display server is up (wtype for Wayland, xdotool for X) and say so out loud
# when neither can drive it, instead of typing into nowhere.
if [ -n "$UNO_SELFTEST" ] && [ -x "$UNO_SELFTEST" ]; then
  (
    export DISPLAY=:0 XDG_RUNTIME_DIR=/run WAYLAND_DISPLAY=wayland-0
    sleep 60
    "$UNO_SELFTEST" > /dev/ttyS0 2>&1
  ) &
else
  echo "uno: no selftest for this appliance" > /dev/ttyS0
fi

# seatd hands out the DRM and input devices a compositor needs.  Alpine's
# libseat is built WITHOUT the builtin backend, so without this daemon cage
# fails at once with "No backend matched name" and nothing is displayed.
if [ -x /usr/bin/seatd ] || [ -x /sbin/seatd ]; then
    (seatd -g video >/tmp/seatd.log 2>&1 &) 
    sleep 2
    echo "uno: seatd $([ -S /run/seatd.sock ] && echo up || echo MISSING)" > /dev/ttyS0
fi

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
# WHAT THIS COMPOSITOR ACCEPTS, ASKED RATHER THAN ASSUMED.  `cage -D` was
# added as a debug flag and cage 0.1.5 has no such flag: it printed its usage,
# exited 1, and the appliance fell through to the X fallback on every single
# boot since - silently, because the fallback works under QEMU.  Every run
# after that was diagnosed as a Wayland problem while Wayland was never
# running.  So the flags are checked against the binary's own help before use,
# and that check is the FIRST thing a new appliance inherits: cage and labwc
# take their client on completely different command lines, and guessing which
# is a session that starts with no client in it.
COMP_BIN=$(command -v "$UNO_COMPOSITOR" 2>/dev/null)
COMP_ARGS=""
COMP_MODE=""
if [ -n "$COMP_BIN" ]; then
    COMP_HELP=$("$COMP_BIN" -h 2>&1; "$COMP_BIN" --help 2>&1)
    case "$UNO_COMPOSITOR" in
    cage)
        # -s: allow VT switching, which is also what makes libseat treat this
        # session as one that can OWN the seat rather than share it.  Without
        # it libinput keeps its devices PAUSED and the client cannot be typed
        # at.  cage takes its client after `--`.
        case "$COMP_HELP" in *" -s"*) COMP_ARGS="-s" ;; esac
        COMP_MODE=dashdash
        ;;
    labwc)
        # labwc is a STACKING window manager, not a kiosk: it outlives its
        # clients, so the client cannot be its argv tail the way cage's is.
        # `-s <command>` is its startup command, and `-C <dir>` points the
        # config at a read-only path - both checked, because a labwc that
        # rejects one of them exits before drawing anything.
        case "$COMP_HELP" in *"-s"*|*"--startup"*) COMP_MODE=startup ;; esac
        case "$COMP_HELP" in *"-C"*|*"--config-dir"*)
            COMP_ARGS="-C /usr/share/uno/labwc" ;; esac
        ;;
    esac
    echo "uno: $UNO_COMPOSITOR $("$COMP_BIN" -v 2>&1 | tr -d '\n') args '$COMP_ARGS' mode '$COMP_MODE'" > /dev/ttyS0
    [ -n "$COMP_MODE" ] || echo "uno: $UNO_COMPOSITOR takes NEITHER form of client argument - the session will be empty" > /dev/ttyS0
else
    echo "uno: compositor '$UNO_COMPOSITOR' is not installed" > /dev/ttyS0
fi

WFAIL=0
while :; do
    if [ -n "$COMP_BIN" ] && [ -n "$COMP_MODE" ] && [ ! -f /tmp/wayland-failed ]; then
        # ON A VT, AND HOLDING IT.  Without logind, libseat's seatd backend
        # ties input to the seat's ACTIVE virtual terminal: a compositor
        # started from a serial-console shell gets DRM master (so it draws
        # perfectly) while libinput keeps its devices PAUSED - which is a
        # client that renders and cannot be typed at, exactly what the
        # first Wayland run produced.
        chvt 1 2>/dev/null
        T0=$(cut -d. -f1 /proc/uptime)
        if [ "$COMP_MODE" = dashdash ]; then
            "$COMP_BIN" $COMP_ARGS -- "$UNO_CLIENT" < /dev/tty1 > /tmp/x.log 2>&1
        else
            "$COMP_BIN" $COMP_ARGS -s "$UNO_CLIENT" < /dev/tty1 > /tmp/x.log 2>&1
        fi
        RC=$?
        T1=$(cut -d. -f1 /proc/uptime)
        # GUEST SECONDS, and say so.  /proc/uptime counts the time this guest
        # was SCHEDULED, which under the hypervisor is a small fraction of the
        # wall clock - a session that ran for four minutes of anybody's life
        # reports seven.  Useful as a relative number, useless as a duration,
        # and a fallback rule written against it fires on healthy sessions.
        echo "uno: wayland session ended rc=$RC after ${T1}-${T0}=$((T1 - T0)) guest-s ----" > /dev/ttyS0
        # WITHOUT THE NOISE, or the tail is all noise.  Crashpad prints
        # `sched_getscheduler: Function not implemented` once per thread while
        # it CAPTURES a dump - thirty identical lines that arrive after the
        # thing worth reading and push it off the end.  The one line that
        # named the crash was the thirty-first.
        grep -vE "sched_getscheduler|scaling_(cur|max)_freq" /tmp/x.log \
            | tail -25 | sed 's/^/uno| /' > /dev/ttyS0
        # FALL BACK ON REPETITION, NOT ON DURATION.  A compositor that refuses
        # its own arguments fails instantly and identically every time; a
        # browser that crashes once deserves to be restarted, which is the
        # whole point of this loop.  Counting consecutive failures tells those
        # apart without needing a clock the guest does not have.
        if [ $RC -ne 0 ]; then
            WFAIL=$((WFAIL + 1))
            if [ $WFAIL -ge 3 ]; then
                : > /tmp/wayland-failed
                echo "uno: $UNO_COMPOSITOR failed $WFAIL times running - falling back to X" > /dev/ttyS0
            fi
        else
            WFAIL=0
        fi
    fi
    if [ -f /tmp/wayland-failed ] || [ -z "$COMP_BIN" ] || [ -z "$COMP_MODE" ]; then
        # THE FALLBACK IS OPTIONAL NOW, and saying so beats spinning.  Xorg
        # segfaults on this guest's framebuffer, so an appliance whose app has
        # no X session is better off reporting that than restarting an empty
        # xinit every two seconds and filling the console with it.
        if [ -n "$UNO_XCLIENT" ] && [ -x "$UNO_XCLIENT" ]; then
            xinit "$UNO_XCLIENT" \
              -- /usr/bin/X :0 vt1 -nolisten tcp -quiet > /tmp/x.log 2>&1
            echo "uno: X session ended ----" > /dev/ttyS0
            tail -10 /tmp/x.log | sed 's/^/uno| /' > /dev/ttyS0
        else
            echo "uno: no session left to run ($UNO_APP has no X fallback)" > /dev/ttyS0
            sleep 30
        fi
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

# THE FAST LOOP'S KEYBOARD, kept with the appliance because it is the thing
# that made a twenty-five-minute question a two-minute one.  i8042 controller
# command 0xD2 is "write keyboard output buffer": the byte after it is handed
# to the guest's own atkbd exactly as if the keyboard had sent it, IRQ1 and
# all.  So from inside the guest, under plain QEMU, this reproduces the byte
# stream `unovdev_pc.c`'s k8_tap queues under the hypervisor - without the
# hypervisor.
#
#   inject_scancodes.sh 29 38 166 157    # ctrl+L, set-1 make/break, decimal
#   KDELAY=0 inject_scancodes.sh 30 158  # no gap between bytes
#
# The default five-millisecond gap is NOT padding.  Each byte is two outb
# writes from a task on one vCPU while the guest's i8042 ISR reads port 0x60
# on the other, and back to back the two race: a burst of four loses its
# middle two, so ctrl+L arrives as Ctrl down, Ctrl up, and the L vanishes -
# which looks exactly like the bug this was written to chase.  That race is
# the TOOL's, not the emulated controller's; UnoDOS fills its queue while the
# guest is not running.  This reproduces the byte VALUES faithfully and the
# delivery atomicity only with a gap.
#
# Bytes come from a 0x00..0xFF table rather than a printf escape, because
# `dd skip=` needs no escaping and every layer between here and an author
# eats backslashes.
mkdir -p $R/usr/share/uno
: > $R/usr/share/uno/bytes.bin
i=0; while [ $i -lt 256 ]; do
    printf "\\$(printf '%03o' $i)" >> $R/usr/share/uno/bytes.bin
    i=$((i + 1))
done
# CHECKED, because this loop is exactly the sort that produces a plausible
# wrong answer: a printf whose escape handling or locale differs writes a
# multi-byte sequence for everything over 0x7F and the table comes out four
# thousand bytes long, silently indexing the wrong scancode for every key
# above 127 - which is every BREAK code there is.
[ "$(wc -c < $R/usr/share/uno/bytes.bin)" = "256" ] || {
    echo "uno-build: bytes.bin is $(wc -c < $R/usr/share/uno/bytes.bin) bytes, not 256" >&2
    exit 1; }
cat > $R/usr/share/uno/inject_scancodes.sh <<'INJEOF'
#!/bin/sh
B=/usr/share/uno/bytes.bin
D=${KDELAY:-5}
for b in "$@"; do
  dd if=$B of=/dev/port bs=1 skip=210 seek=100 count=1 conv=notrunc 2>/dev/null
  dd if=$B of=/dev/port bs=1 skip="$b" seek=96 count=1 conv=notrunc 2>/dev/null
  if [ "$D" -gt 0 ]; then usleep $((D * 1000)); fi
done
exit 0
INJEOF
chmod +x $R/usr/share/uno/inject_scancodes.sh

# DNS on a read-only root.  /etc/resolv.conf is a symlink into the tmpfs the
# appliance mounts over /tmp - but that alone is NOT enough, and the first
# bridged run proved it: the guest took a perfectly good lease (10.0.2.16)
# and still had no DNS, because the stock Alpine udhcpc script writes its
# resolv.conf through a TEMP FILE beside the target, and /etc is read-only.
# So udhcpc gets a script of ours that writes the tmpfs path directly.
ln -sf /tmp/resolv.conf $R/etc/resolv.conf

# THE MACHINE ID, for the same reason and by the same trick.  dbus will not
# start a bus without one, it lives at /etc/machine-id, and /etc is read-only -
# so the file is a symlink into the tmpfs uno-init mounts over /var, and
# uno-init fills it in with dbus-uuidgen at boot.  A per-boot identity is the
# honest answer for an appliance whose whole filesystem is thrown away anyway.
mkdir -p $R/var/lib/dbus
ln -sf /var/lib/dbus/machine-id $R/etc/machine-id
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

# ---- the app's own half ----------------------------------------------------
# Last, so an app file can overwrite anything the common half wrote.
mkdir -p $R/usr/share/uno
app_write

# WHAT uno-init READS AT BOOT.  Written here rather than in the app file so a
# .app that forgets one of these still produces a bootable appliance with a
# named fault, and so the file's shape is one thing rather than five.
cat > $R/usr/share/uno/app.env <<ENVEOF
UNO_APP=$APP
UNO_COMPOSITOR=$APP_COMPOSITOR
UNO_CLIENT=$APP_CLIENT
UNO_XCLIENT=$APP_XCLIENT
UNO_SELFTEST=$APP_SELFTEST
$APP_ENV
ENVEOF

# CHECKED, because every one of these is a file uno-init will try to EXEC, and
# a missing one presents as a compositor that starts and shows nothing - which
# is the most expensive failure in this directory to diagnose.  A build that
# dies here leaves the last image in place, which beats shipping a rootfs whose
# session is empty.
for f in $APP_CLIENT $APP_XCLIENT $APP_SELFTEST; do
    [ -x "$R$f" ] || { echo "uno-build: $APP names $f but the rootfs has no executable there" >&2; exit 1; }
done
# IN THE ROOTFS, NOT IN THE CONTAINER.  The first version of this check ran
# `command -v $APP_COMPOSITOR` "advisory only" - and under `set -e` a bare
# command that returns 127 IS the exit status of the script.  The build died
# at 127 with its last line reading "primed GIMP profile", which is a success
# message, so the failure looked like it came from the step after the one that
# actually failed.  There is no advisory statement in a `set -e` script.
[ -x "$R/usr/bin/$APP_COMPOSITOR" ] || [ -x "$R/usr/sbin/$APP_COMPOSITOR" ] || {
    echo "uno-build: $APP wants compositor '$APP_COMPOSITOR' and no package installed one" >&2; exit 1; }

# WHAT THE HOST WILL NEED TO MAKE THIS AN ICON.  A sidecar next to the image,
# in the `.UNO` descriptor's own key names (`uno_appdesc.h`), so the shim
# generator of ANDROID-APPLIANCE-PLAN §3.2 reads rather than re-derives them.
# `kind`/`host`/`launch` are the plan's additive keys; today's descriptor
# reader ignores keys it does not know, which is what makes writing them now
# free.  `icon:` stays an emblem name until the app-registry lane lands
# `icon: @` (inline QOI) - a request, not something to invent here.
cat > /out/rootfs-$APP.desc <<DESCEOF
id:      $APP_ID
name:    ${APP_TITLE:-$APP}
short:   ${APP_TITLE:-$APP}
icon:    generic
cat:     ${APP_CAT:-utility}
kind:    foreign
host:    $APP_HOST
launch:  $APP_LAUNCH
DESCEOF
cp /out/rootfs-$APP.desc $R/usr/share/uno/app.desc
echo "uno-build: descriptor -> rootfs-$APP.desc"

# Pack.  Size: tree + 15% headroom, floor 256 MB.
KB=$(du -sk $R | cut -f1)
MB=$(( KB / 1024 * 115 / 100 + 64 ))
[ $MB -lt 256 ] && MB=256
rm -f /out/rootfs.img
mke2fs -q -t ext4 -d $R -L unoroot /out/rootfs.img ${MB}M
echo "rootfs.img: $APP, ${MB} MB ($(( KB / 1024 )) MB of files)"
