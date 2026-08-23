# The Android appliance: ONE Android app, full-screen, with nothing around it.
#
# Sourced by rootfs_inner.sh inside the build container with $R = the rootfs
# root.  See apps/README.md for the contract, and
# docs/ANDROID-APPLIANCE-PLAN.md for what this is a phase of (P1).
#
# WHAT IS DIFFERENT ABOUT THIS ONE.  Every other appliance's client is a
# program that draws.  This one's client is a CONTAINER MANAGER: cage runs
# `waydroid session start`, which brings up an LXC container holding a whole
# Android, and the surfaces that reach the compositor are Android activities
# rendered by SwiftShader inside it.  So the client script is longer than the
# others put together, and almost all of it is environment rather than
# command line - see §3.5 of the plan for the seven faults that shaped it,
# every one of which presents as a black window.
#
# cage, not labwc, and not because Firefox is a browser: with
# `persist.waydroid.multi_windows=true` Waydroid gives each Android activity
# its OWN xdg_toplevel and there is no launcher surface at all, so a kiosk
# compositor showing exactly one toplevel is the whole "no Android on screen"
# requirement, enforced by the compositor rather than by policy.
# nftables IS NOT OPTIONAL, and which firewall tool is installed decides
# whether Android gets an address at all.  waydroid's container manager runs
# data/scripts/waydroid-net.sh, which picks its backend by what it can find:
# with `nft` present it writes a native nftables ruleset, and without it
# falls back to iptables.  Alpine's iptables is iptables-nft, a translation
# layer that needs CONFIG_NFT_COMPAT - which this kernel does not have - so
# that path fails on the first rule:
#
#     Extension udp revision 0 not supported, missing kernel module?
#     iptables v1.8.10 (nf_tables): RULE_APPEND failed (No such file or
#         directory): rule in chain POSTROUTING
#     Failed to setup waydroid-net.
#
# and the script runs under `set -e` with a cleanup trap, so ONE failed rule
# tears the whole network down.  The nftables path asks the kernel for
# exactly what it already has (inet filter chains, an ip nat postrouting
# chain, masquerade), and it does not use the CHECKSUM target the iptables
# path needs for DHCP - which is the other symbol this kernel lacks.  One
# package instead of two kernel symbols and a rebuild.
APP_PKGS="waydroid cage nftables"
APP_COMPOSITOR=cage
APP_CLIENT=/usr/share/uno/android.sh
# NO X FALLBACK, deliberately.  Xorg segfaults on this guest's framebuffer,
# and Waydroid's session client is Wayland-only - there is nothing for an X
# session to run.  An appliance with no fallback says so at boot (uno-init
# prints "no session left to run") instead of restarting an empty xinit.
APP_XCLIENT=
APP_SELFTEST=/usr/share/uno/selftest.sh

# The app-registry half.  This appliance IS Firefox as far as the shell is
# concerned; `host: android` is what tells unopkg's shim which runtime to ask.
APP_ID=firefox
APP_TITLE=Firefox
APP_CAT=internet
APP_HOST=android
APP_LAUNCH=org.mozilla.firefox

# WHICH PACKAGE, as one variable, because everything below is Android-shaped
# rather than Firefox-shaped: point these two at another APK and the appliance
# runs that instead.  UNO_APK is the file the build baked in; UNO_APP_PKG is
# what to launch once it is installed.
APP_ENV='export UNO_APK=${UNO_APK:-/usr/share/uno/app.apk}
export UNO_APP_PKG=${UNO_APP_PKG:-org.mozilla.firefox}'

app_write() {
# THE MOUNT POINT IS MADE AT BUILD TIME, and this is the single most
# expensive line in the file to omit.  The root is read-only at run time, so
# `mkdir` at boot fails; the runtime image then has nowhere to mount; Waydroid
# then finds no preinstalled images; it then tries to DOWNLOAD a gigabyte;
# and it reports a NETWORK error.  Three layers of wrong answer from one
# missing directory, and the plan's §3.5 table has it first for that reason.
#
# THIS PATH AND NO OTHER.  Waydroid's own defaults list
# /usr/share/waydroid-extra/images as a preinstalled-images path
# (tools/config/__init__.py).  Finding system.img and vendor.img there is
# also what makes `waydroid init` TOLERATE a failed OTA fetch instead of
# treating it as fatal.
mkdir -p $R/usr/share/waydroid-extra/images
mkdir -p $R/var/lib/waydroid $R/run/waydroid-lxc

# THE PACKAGE IS BAKED IN, and it comes from the build directory rather than
# from the tree.  Firefox for Android is MPL-2.0 and redistributable, but
# UNODOS carries build scripts and not binaries (§6 of the plan), so the APK
# is a file the operator drops next to the images:
#
#     cp firefox-x86_64.apk /work/unodos-guest/firefox.apk
#     UNO_APP=android ./build_rootfs.sh /work/unodos-guest
#
# It is 138 MB of the image and it is worth it: the alternative is fetching
# it at every boot, on a guest with a quarter of a core, into a tmpfs that
# forgets it.  P4 replaces this with PKG_PUT over the channel, which streams
# the user's OWN package off the ESP and makes this line go away.
#
# FAIL THE BUILD, do not warn.  An appliance whose APK is missing boots an
# Android with nothing in it, which on a framebuffer is indistinguishable
# from every other failure in this directory: a black screen.
if [ -f /out/firefox.apk ]; then
    cp /out/firefox.apk $R/usr/share/uno/app.apk
    echo "uno-build: baked $(du -m /out/firefox.apk | cut -f1) MB APK"
else
    echo "uno-build: android needs an APK at /out/firefox.apk (the workdir)" >&2
    echo "uno-build:   cp firefox-x86_64.apk <workdir>/firefox.apk, then rebuild" >&2
    exit 1
fi

cat > $R/usr/share/uno/android.sh <<'ANDEOF'
#!/bin/sh
# cage's client.  Brings up Waydroid, installs the app if Android does not
# have it, launches it, and then STAYS IN THE FOREGROUND holding the session
# open - because cage exits when its client exits, and a cage that exits
# takes the whole display down with it.
say() { echo "uno-android: $*" > /dev/ttyS0; }

# ---- 0. what this guest is, since three things below depend on it ---------
# waydroid's tools/helpers/arch.py remaps x86_64 to "x86" when /proc/cpuinfo
# does not advertise sse4_2, and then looks for x86 images this appliance does
# not carry.  It says so in an INFO line that reads like a note.  UnoDOS
# passes CPUID through, and QEMU needs -cpu host; either way, when this line
# says LACKS, nothing below can work and the reason is one word.
grep -q sse4_2 /proc/cpuinfo \
    && say "cpu has sse4_2 (waydroid will use the x86_64 images)" \
    || say "cpu LACKS sse4_2 - waydroid will look for x86 images and find none"
grep -qw binder /proc/filesystems \
    && say "binderfs available" \
    || say "binderfs ABSENT - this kernel cannot run Android"

# ---- 1. cgroup2, which the common init does not mount --------------------
# LXC needs it and Android's libprocessgroup needs it writable.  It is here
# rather than in uno-init because no other appliance runs a container.
if ! grep -q ' /sys/fs/cgroup ' /proc/mounts; then
    mkdir -p /sys/fs/cgroup
    mount -t cgroup2 cgroup2 /sys/fs/cgroup 2>/dev/null \
        && say "cgroup2 mounted" || say "cgroup2 MOUNT FAILED - lxc will not start"
fi

# ---- 2. the runtime image ------------------------------------------------
# Not a fixed device: the appliance gets its rootfs on vda and the Android
# image on whatever slot the host gave it, and under plain QEMU that is vdb
# while under unovirt it depends on how many blk slots the machine model has.
# Probing costs nothing and turns "no images" into a named device.
if ! grep -q '/usr/share/waydroid-extra/images' /proc/mounts; then
    for d in /dev/vdb /dev/vdc /dev/vdd; do
        [ -b "$d" ] || continue
        if mount -o ro "$d" /usr/share/waydroid-extra/images 2>/dev/null; then
            say "runtime image: $d -> $(ls /usr/share/waydroid-extra/images | tr '\n' ' ')"
            break
        fi
    done
fi
grep -q '/usr/share/waydroid-extra/images' /proc/mounts \
    || say "NO RUNTIME IMAGE - no android.img on any virtio-blk slot"

# ---- 3. the NAT the container's network is built on ----------------------
# waydroid's container manager runs data/scripts/waydroid-net.sh, which makes
# the waydroid0 bridge, hands out 192.168.240.0/24 with dnsmasq and NATs it
# behind this guest's own lease.  Forwarding is the one thing it does not
# turn on for itself, and without it Android gets an address and reaches
# nothing - which inside the app looks like a broken site rather than a
# broken guest.
echo 1 > /proc/sys/net/ipv4/ip_forward 2>/dev/null
say "ip_forward=$(cat /proc/sys/net/ipv4/ip_forward 2>/dev/null)"

# ---- 4. HOME, XDG_RUNTIME_DIR, and the two buses -------------------------
# uno-init exports HOME=/root (a tmpfs) and starts both buses, so this only
# checks them - but it checks them OUT LOUD, because the session manager
# writes $HOME/.local/share/waydroid and an unset HOME makes that `//.local`
# on a read-only root, reported as "[Errno 30] Read-only file system:
# '//.local'": a doubled slash that looks like a Waydroid bug and is a
# missing variable.
mkdir -p "$HOME/.local/share" 2>/dev/null
say "home=$HOME xdg=$XDG_RUNTIME_DIR wayland=$WAYLAND_DISPLAY"
say "session bus ${DBUS_SESSION_BUS_ADDRESS:-MISSING}"
mkdir -p /run/waydroid-lxc /var/lib/waydroid /run/dbus /run/lock /var/lib/dbus
# /var/run -> /run, because the tmpfs over /var buried alpine-baselayout's
# symlink and dbus binds its system socket under /var/run.  uno-init restores
# it too; this survives an older common half.
[ -e /var/run ] || ln -s /run /var/run 2>/dev/null
# THE SYSTEM BUS IS THE ONE THIS APPLIANCE LIVES ON, and it was not running.
# uno-init starts it, but it starts it BEFORE it writes the machine-id that
# dbus refuses to start without - an ordering that no other appliance can
# see, because Chromium and GIMP both want the SESSION bus and that one is
# started afterwards and works.  The whole symptom was one line from the
# session manager:
#
#     [21:16:54] WayDroid container is not listening
#
# repeated eighty-five times, because the container service registers on the
# system bus and the session looks for it there.  Nothing in that sentence
# mentions dbus.
#
# The ordering is fixed in uno-init as well.  This stays because it is
# cheap, because it makes the appliance survive an older common half, and
# because "start the bus you need" is not a thing to inherit silently.
if [ ! -S /run/dbus/system_bus_socket ] && [ ! -S /var/run/dbus/system_bus_socket ]; then
    [ -s /var/lib/dbus/machine-id ] || dbus-uuidgen > /var/lib/dbus/machine-id 2>/dev/null
    ( dbus-daemon --system --nofork > /tmp/dbus-system.log 2>&1 & )
    sleep 3
fi
if [ -S /run/dbus/system_bus_socket ] || [ -S /var/run/dbus/system_bus_socket ]; then
    say "system bus up"
else
    say "system bus MISSING: $(head -3 /tmp/dbus-system.log 2>/dev/null | tr '\n' ' ')"
fi

# ---- 5. waydroid init, once ----------------------------------------------
# `waydroid init` CANNOT TOLERATE BEING OFFLINE even when it needs nothing
# from the network: helpers/http.py's retrieve() catches ValueError and
# HTTPError, so the URLError a DNS failure raises escapes and kills it.
# uno-init brings the lease up in the FOREGROUND before starting any
# compositor, so by the time this runs DNS is either working or hopeless.
if [ ! -f /var/lib/waydroid/waydroid.cfg ]; then
    say "--- waydroid init ---"
    waydroid init -f > /tmp/wd-init.log 2>&1 || say "init returned $?"
    tail -6 /tmp/wd-init.log > /dev/ttyS0
fi
[ -f /var/lib/waydroid/waydroid.cfg ] || say "NO waydroid.cfg - init did not complete"

# ---- 6. the properties, and they must be set BEFORE the container starts --
# MULTI-WINDOW IS WHAT MAKES THIS AN APPLIANCE RATHER THAN AN EMULATOR.  In
# single-window mode Waydroid presents Android's whole display as one surface
# - launcher, status bar and all - which is exactly the look the plan
# rejects.  In multi-window mode each activity is its own xdg_toplevel with
# its own title, there is no launcher surface, and cage shows the one window
# that exists.  It is read when the container starts, so setting it with
# `waydroid prop set` afterwards is a session too late.
#
# DENSITY, because Android assumes a phone.  At the default ~440 dpi a
# 1024x768 window shows a giant phone layout with four widgets in it; 120
# gives Firefox a desktop-shaped page.
#
# SWIFTSHADER BY NAME.  There is no GPU and no render node here, so the EGL
# and gralloc implementations have to be the software ones - and being
# explicit means a wrong guess fails at boot with a named property instead of
# inside a renderer.
PROP=/var/lib/waydroid/waydroid_base.prop
if [ -f "$PROP" ]; then
    grep -q multi_windows "$PROP" || cat >> "$PROP" <<'PROPEOF'
persist.waydroid.multi_windows=true
ro.hardware.gralloc=default
ro.hardware.egl=swiftshader
ro.sf.lcd_density=120
PROPEOF
    say "props: $(grep -c . $PROP) lines, multi_windows=$(grep -c multi_windows $PROP)"
else
    say "no waydroid_base.prop - the session will run single-window"
fi

# ---- 7. the container service --------------------------------------------
# A system service, started here rather than by an init system because there
# is no init system.  It owns the LXC container and the network; the SESSION
# below is a different thing on a different bus, and the container stays
# STOPPED until a session attaches to a Wayland display - which is why a
# probe with no compositor looks like total failure while everything under it
# works.
if ! pgrep -f "waydroid container start" >/dev/null 2>&1; then
    ( waydroid container start > /tmp/wd-container.log 2>&1 ) &
    say "container service started"
fi

# ---- 8. install and launch, once the session is up -----------------------
# In the background, because the session start below has to be in the
# foreground and this has to wait for it.  Everything here is idempotent, so
# a session that restarts gets its app back without any state to reconcile.
(
    i=0
    while [ $i -lt 60 ]; do
        sleep 5; i=$((i + 1))
        S=$(waydroid status 2>/dev/null | tr '\n' ' ')
        case "$S" in *"Session:"*RUNNING*) break ;; esac
        [ $((i % 4)) -eq 0 ] && say "waiting for the session [$i]: $S"
    done
    case "$S" in
    *RUNNING*) say "session RUNNING after ${i}0s" ;;
    *) say "session NEVER came up: $S"
       tail -12 /tmp/wd-session.log > /dev/ttyS0 2>/dev/null; exit 0 ;;
    esac

    # The Android IP, which is the half of "is there network" that the guest's
    # own lease does not answer.  dnsmasq leases it over the veth; a container
    # with no address here has NAT without DHCP, and inside the app that reads
    # as one broken website rather than as a broken guest.
    j=0
    while [ $j -lt 12 ]; do
        IP=$(waydroid status 2>/dev/null | awk -F'\t' '/IP address/{print $2}')
        case "$IP" in 192.168.240.*) say "android has $IP"; break ;; esac
        sleep 5; j=$((j + 1))
    done
    case "$IP" in 192.168.240.*) ;; *) say "android has NO address ($IP) - dnsmasq or the veth" ;; esac

    if waydroid app list 2>/dev/null | grep -q "$UNO_APP_PKG"; then
        say "$UNO_APP_PKG is already installed"
    elif [ -f "$UNO_APK" ]; then
        say "installing $(du -m "$UNO_APK" | cut -f1) MB, this takes a while under swiftshader"
        waydroid app install "$UNO_APK" > /tmp/wd-install.log 2>&1 \
            && say "installed" || { say "install FAILED"; tail -6 /tmp/wd-install.log > /dev/ttyS0; }
    else
        say "NO APK at $UNO_APK - the rootfs was built without one"
    fi

    # LAUNCHED FULL-SCREEN, and this is the difference between an appliance
    # and a phone emulator in a corner of the display.  `waydroid app launch`
    # starts the activity in FREEFORM mode, because multi_windows puts the
    # whole Android display in freeform - and Android's default freeform
    # window is phone-shaped: 309x549 in the middle of a 1280x800 output,
    # with the rest of the screen showing Android's wallpaper.
    #
    # `--windowingMode 1` is fullscreen, and it only takes effect when the
    # TASK IS CREATED.  Passing it to an activity whose task already exists
    # is silently ignored - the task keeps the mode it was born with, which
    # is how the first attempt at this looked like a flag that does nothing.
    # So the mode is chosen on the first launch, and a task that somehow
    # ends up freeform anyway is maximised through its own caption button
    # below rather than relaunched.
    #
    # The activity is RESOLVED rather than assumed: `waydroid app launch`
    # knows how to find a package's launcher activity and `am start -n`
    # needs it spelled out, so ask the package manager, which is the only
    # thing that actually knows.
    # `waydroid app launch`, AND NOTHING ELSE STARTS AN APP HERE.  The
    # obvious alternative - `waydroid shell -- am start -n pkg/activity` -
    # runs, returns 0, prints NOTHING, and starts nothing; so does `cmd
    # activity start-activity -W`, which is supposed to print a status line
    # whatever happens.  In the same shell `cmd package resolve-activity` and
    # `pm list packages` answer perfectly, so it is not the shell and not
    # binder in general: `waydroid shell` attaches to the container as root
    # in a context the activity manager will not take a start from, while
    # `app launch` goes through waydroid's own platform service over gbinder.
    #
    # This cost a whole boot to find because a silent no-op is the one
    # failure a launcher loop cannot tell from a slow app.
    #
    # It ALSO means this must run with DBUS_SESSION_BUS_ADDRESS set, which
    # it does here (uno-init exports it) and does not in a serial shell -
    # where the same command answers "Unable to autolaunch a dbus-daemon
    # without a $DISPLAY for X11" and tries to start a SECOND session.
    waydroid app launch "$UNO_APP_PKG" > /tmp/wd-launch.log 2>&1

    k=0
    while [ $k -lt 10 ]; do
        sleep 20; k=$((k + 1))
        B=$(waydroid shell -- dumpsys activity activities 2>/dev/null \
            | grep -m1 -A1 "A=[0-9]*:$UNO_APP_PKG" | tr '\n' ' ')
        case "$B" in
        *mode=fullscreen*) say "$UNO_APP_PKG has a full-screen window"; break ;;
        *mode=freeform*)
            # MAXIMISE THROUGH THE CAPTION BUTTON, because `am task resize`
            # returns success and does nothing here.  Android draws a
            # freeform caption bar on the task, and its maximise button is
            # the second from the right, one caption height in - so the tap
            # is computed from the task's OWN bounds rather than from a
            # screen coordinate that changes with the output size.
            R=$(printf '%s' "$B" | grep -oE 'mBounds=Rect\([0-9]+, [0-9]+ - [0-9]+, [0-9]+\)' | head -1)
            X=$(printf '%s' "$R" | grep -oE '[0-9]+' | sed -n 3p)
            Y=$(printf '%s' "$R" | grep -oE '[0-9]+' | sed -n 2p)
            if [ -n "$X" ] && [ -n "$Y" ]; then
                say "freeform at $R - maximising"
                waydroid shell -- input tap $((X - 48)) $((Y + 15)) >/dev/null 2>&1
            fi
            ;;
        *)  # NO TASK AT ALL, so launch again.  The first launch of a
            # just-installed package can land while the package manager is
            # still optimising it, and comes back having done nothing.
            say "no window for $UNO_APP_PKG yet [$k]: $(head -c 80 /tmp/wd-launch.log | tr '\n' ' ')"
            waydroid app launch "$UNO_APP_PKG" > /tmp/wd-launch.log 2>&1 ;;
        esac
    done
    say "MEM $(awk '/MemAvailable/{printf "avail=%dM", $2/1024}' /proc/meminfo) $(lxc-info -P /var/lib/waydroid/lxc -n waydroid 2>/dev/null | awk '/Memory use/{print "container=" $3 $4}')"
) &

# ---- 9. the session, in the foreground, restarted in place ---------------
# INSIDE THE LOOP, NOT AROUND IT.  cage exits when its client does, so an
# `exec` here would turn one session fault into a whole compositor restart -
# seatd, a VT switch and an Android cold boot, minutes of wall time on this
# guest, with a black screen throughout.  Looping here keeps cage's client
# alive so the display never goes down.
n=0
while :; do
    n=$((n + 1))
    say "session start (attempt $n) on $WAYLAND_DISPLAY"
    waydroid session start > /tmp/wd-session.log 2>&1
    say "session exited rc=$? (attempt $n)"
    tail -8 /tmp/wd-session.log > /dev/ttyS0 2>/dev/null
    # TEN SECONDS, not one.  A session that cannot reach the container
    # service fails instantly, so a tight loop turns one fault into a
    # hundred identical lines and buries the boot that produced it.
    sleep 10
done
ANDEOF
chmod +x $R/usr/share/uno/android.sh

cat > $R/usr/share/uno/selftest.sh <<'STEOF'
#!/bin/sh
# What this appliance has to prove, asked from inside the guest 60 s in, and
# again as the run goes on: the container is up, Android has an address, the
# app has a window, and a keystroke that enters the COMPOSITOR reaches the
# app.  Every one of them is a different layer, and a black screen is what
# all four look like from the host.
#
# A SELF-TEST THAT CANNOT FAIL PROVES NOTHING.  The browser appliance's
# version ran xdotool against a Wayland session, where it failed every
# command and reported success - so this one names the tool it used and says
# so out loud when it has none.
say() { echo "uno-selftest: $*"; }

for i in 1 2 3 4 5 6 7 8; do
    S=$(waydroid status 2>/dev/null | tr '\n\t' '  ')
    say "[$i] $S"

    # THE WINDOW, with its MODE, because "there is a window" was true for
    # every run that showed a phone-sized rectangle in the middle of a black
    # screen.  freeform means it did not maximise; fullscreen is the answer.
    B=$(waydroid shell -- dumpsys activity activities 2>/dev/null \
        | grep -m1 -A1 "A=[0-9]*:$UNO_APP_PKG" | tr '\n' ' ')
    say "[$i] window: $(printf '%s' "$B" | grep -oE 'mode=[a-z]+|mBounds=Rect\([^)]*\)' | tr '\n' ' ')"

    # THE NETWORK, from ANDROID'S SIDE OF THE VETH, and in the two places
    # that fail independently.  An address is not connectivity: a run that
    # showed `IP address: 192.168.240.112` in waydroid status had no default
    # route, no DNS server, and a browser reporting NS_ERROR_OFFLINE.
    #
    # THE ROUTE IS IN TABLE 1003, NOT IN main.  Android puts each network's
    # routes in its own table and marks app traffic to select it, so `ip
    # route` from a root shell shows an on-link prefix and nothing else even
    # when the network is perfectly up - which reads as a broken network and
    # is not one.
    say "[$i] android route: $(waydroid shell -- ip route show table 1003 2>/dev/null | grep -m1 default || echo NONE)"

    # VALIDATED IS THE PROOF, and it is Android's own rather than ours.
    # ConnectivityService only sets that capability after its NetworkMonitor
    # has resolved a name and fetched a URL over this network - so one word
    # in one line says DNS, NAT, forwarding and the upstream route all work,
    # from inside the container, without a browser being involved or a page
    # being interpreted.  Its absence is equally specific: a network that is
    # up and cannot reach anything.
    C=$(waydroid shell -- dumpsys connectivity 2>/dev/null | grep -m1 -oE 'Capabilities: [A-Z_&]+')
    case "$C" in
    *VALIDATED*) say "[$i] android network: VALIDATED (it reached the internet)" ;;
    *)           say "[$i] android network: NOT validated - $C" ;;
    esac

    W=$(waydroid shell -- dumpsys window visible-apps 2>/dev/null | grep -c "$UNO_APP_PKG")

    # THE INPUT QUESTION, and it is the one this appliance exists to answer.
    # wtype talks to the COMPOSITOR, so a keystroke it sends travels
    # compositor -> waydroid session -> Android input -> the app: every layer
    # above the emulated i8042.  If this types and the host's own keyboard
    # does not, the gap is the device; if neither does, it is above them both.
    if [ "$i" = 3 ] && [ -x /usr/bin/wtype ] && [ "${W:-0}" -gt 0 ]; then
        if wtype -d 120 "unodos.arinbakht.com" 2>/tmp/wtype.err && wtype -k Return 2>>/tmp/wtype.err; then
            say "typed a URL into the app through the compositor"
        else
            say "wtype FAILED: $(head -c 120 /tmp/wtype.err | tr '\n' ' ')"
        fi
    fi
    sleep 45
done
STEOF
chmod +x $R/usr/share/uno/selftest.sh
}
