# The GIMP appliance: an application that puts up SEVERAL windows.
#
# Sourced by rootfs_inner.sh inside the build container with $R = the rootfs
# root.  See apps/README.md for the contract.
#
# WHY THIS APPLIANCE EXISTS.  Chromium proved the stack end to end, but it
# proved it for a program that wants one window, full-screen, forever - which
# is the one shape a kiosk compositor can serve.  GIMP 2.10 opens a toolbox, a
# dock and an image window, each independently placed, focused and raised.
# Everything that was implicit under cage becomes explicit here: focus follows
# a click rather than a client, the pointer has to land on a particular pixel
# of a particular window, and the compositor outlives the client instead of
# dying with it.
#
# A STACKING WINDOW MANAGER, NOT A KIOSK.  labwc is wlroots + the pixman
# software renderer, exactly like cage, so it inherits the one thing this
# guest is known to support: a compositor that takes simpledrm directly and
# reads evdev itself.  Xorg is NOT the answer here even though GIMP is an X
# client - it segfaults on this guest's framebuffer (see the 20-uno-fb.conf
# note in rootfs_inner.sh), and that is the whole reason the appliance moved
# to Wayland in the first place.
#
# GTK2 IS X11-ONLY, so this appliance needs XWayland and there is no way
# around it: GIMP 2.10 links libgtk-x11-2.0, which has no Wayland backend at
# all.  labwc starts XWayland itself; what this file has to do is make sure
# the client does not race it (see gimp.sh).  GIMP 3 would be native Wayland,
# and is not in Alpine 3.20.
APP_PKGS="gimp labwc xwayland"
APP_COMPOSITOR=labwc
APP_CLIENT=/usr/share/uno/gimp.sh
APP_XCLIENT=/usr/share/uno/gimp-x.sh
APP_SELFTEST=/usr/share/uno/gimp-selftest.sh

# How this appliance will appear in the app registry once the foreign-package
# shim exists (docs/ANDROID-APPLIANCE-PLAN.md §3.2 and §3.4: a `.deb` is the
# same shape as an `.apk`, and GIMP is named there as the natural first one).
# Declared now, consumed later: the build writes them to a sidecar descriptor
# beside the image, so the shim generator has somewhere to read them from and
# this file stays the single place an appliance is defined.
APP_ID=gimp
APP_TITLE="GIMP"
APP_CAT=graphics
APP_HOST=linux
APP_LAUNCH=/usr/bin/gimp

app_write() {

# ---- the canvas -------------------------------------------------------------
# SOMETHING TO OPEN, so the third window exists.  With no file argument GIMP
# shows a toolbox and a dock and an empty desktop, which is two windows and
# looks like a program that did not finish starting.  An image window is the
# one that makes the multi-window claim visible in a screenshot, and it is
# also the surface a host-driven click has to land on.
#
# A PPM WRITTEN WITH `tr`, not with a converter.  A build-time image needs no
# imagemagick (thirty megabytes for one file) and no shell loop over 900,000
# bytes: a P6 header plus that many 0xFF bytes IS a white canvas, and GIMP's
# PNM plug-in reads it.  White on purpose - the host harness proves the
# pointer by drawing on this canvas, and a stroke has to be visible.
printf 'P6\n640 480\n255\n' > $R/usr/share/uno/canvas.ppm
head -c 921600 /dev/zero | tr '\000' '\377' >> $R/usr/share/uno/canvas.ppm
[ "$(wc -c < $R/usr/share/uno/canvas.ppm)" -gt 921600 ] || {
    echo "uno-build: canvas.ppm is short - tr or head did not do what was asked" >&2
    exit 1; }

# ---- labwc's configuration --------------------------------------------------
# ON A READ-ONLY ROOT, so `-C` rather than $XDG_CONFIG_HOME: the appliance's
# config directory is part of the image, not something written into a tmpfs at
# boot where a failed write would silently give us labwc's defaults.
mkdir -p $R/usr/share/uno/labwc
cat > $R/usr/share/uno/labwc/rc.xml <<'RCEOF'
<?xml version="1.0"?>
<labwc_config>
  <!-- FLAT, AND AT SPEED ZERO.  This is the single most load-bearing line in
       the file and it is not about feel.  The host drives this appliance with
       a RELATIVE PS/2 mouse (unovdev_pc.c has no absolute pointer), so the
       harness positions the guest cursor by slamming it into a corner and
       then sending an exact delta.  libinput's default adaptive acceleration
       makes the distance travelled a function of how fast the deltas arrive,
       which under a hypervisor scheduling the guest a slice per frame is not
       a function of anything reproducible.  Flat, speed 0, is 1:1: N units in
       is N pixels across, every time. -->
  <libinput>
    <device category="default">
      <accelProfile>flat</accelProfile>
      <pointerSpeed>0</pointerSpeed>
      <naturalScroll>no</naturalScroll>
    </device>
  </libinput>
  <!-- Title bars are the proof, not decoration: they are what says these are
       three windows a window manager is managing, and they are what a
       host-side drag grabs to move one. -->
  <theme>
    <cornerRadius>0</cornerRadius>
  </theme>
  <!-- Focus on click, not on hover.  A pointer that merely PASSES over the
       toolbox on its way to the canvas must not take the keyboard with it -
       that is the PointerRoot mistake the X session made, one layer up. -->
  <focus>
    <followMouse>no</followMouse>
    <raiseOnFocus>no</raiseOnFocus>
  </focus>
</labwc_config>
RCEOF

# labwc reads this before anything else starts.  GTK2 has no Wayland backend,
# so naming the backend here is belt and braces rather than a fix - but it
# also stops any GTK3 helper GIMP spawns from picking Wayland and rendering
# into a surface the X-side self-test cannot see.
cat > $R/usr/share/uno/labwc/environment <<'ENVEOF'
GDK_BACKEND=x11
XDG_RUNTIME_DIR=/run
ENVEOF

# ---- the client -------------------------------------------------------------
cat > $R/usr/share/uno/gimp.sh <<'GIMPEOF'
#!/bin/sh
# EVERYTHING WORTH READING GOES TO THE SERIAL PORT AS WELL.  This script's
# stderr is the compositor's log INSIDE the guest, and that is precisely where
# a host driving this over a URC link cannot look.  Under the fast loop the
# distinction is invisible - the serial log carries the lot - and under the
# hypervisor it is the difference between watching GIMP start and watching a
# black rectangle for twelve minutes with no idea whether anything is running.
say() { echo "$*" >&2; echo "$*" > /dev/ttyS0 2>/dev/null; }
# labwc's startup command.  Unlike cage, labwc does NOT exit when this exits,
# so the restart loop here is about GIMP alone and the compositor is never
# torn down - which is the same conclusion browser.sh reached for the opposite
# reason (there the loop had to be inside cage to stop cage going down with
# its client; here it is inside labwc because labwc would carry on regardless
# and leave an empty desktop).
#
# WAIT FOR XWAYLAND, DO NOT ASSUME IT.  wlroots starts XWayland lazily, and
# labwc runs this startup command as soon as the compositor is up - which can
# be before the X socket exists.  A GTK2 program that finds no display does
# not wait: it prints "cannot open display" and exits, three times in a row,
# and the appliance shows an empty desktop that looks exactly like a
# compositor with nothing to draw.  So: find the display, then start.
i=0
while [ $i -lt 60 ]; do
    for d in 0 1; do
        [ -S "/tmp/.X11-unix/X$d" ] && { DISPLAY=":$d"; break 2; }
    done
    i=$((i + 1)); sleep 1
done
export DISPLAY=${DISPLAY:-:0}
say "uno-gimp: DISPLAY=$DISPLAY after ${i}s"

export HOME=/root
export XDG_CACHE_HOME=/tmp/cache XDG_CONFIG_HOME=/tmp/config
mkdir -p "$HOME" /tmp/cache /tmp/config

# THE FIRST-RUN COST, PAID AT BUILD TIME.  GIMP queries every plug-in it owns
# the first time it starts and writes the answer to pluginrc - about ninety
# binaries, each executed once.  On a desktop that is a few seconds; on a
# guest that gets a slice per frame it is the difference between an appliance
# that comes up and one that appears hung.  The build primes it; this copies
# the primed tree in before GIMP looks for one.
#
# INTO $XDG_CONFIG_HOME, NOT INTO $HOME/.config, and the difference is not
# cosmetic.  The appliance points XDG_CONFIG_HOME at a tmpfs, and GIMP obeys
# it - so a profile seeded under $HOME/.config is a profile GIMP never opens.
# It then built a fresh one, which cost the plug-in query the priming was
# there to avoid AND came up in single-window mode, which is the one thing
# this appliance exists to disprove.  Both symptoms, one wrong directory, and
# neither of them says "wrong directory" when you look at it.
GPROF=${XDG_CONFIG_HOME:-$HOME/.config}/GIMP
if [ -d /usr/share/uno/gimp-config ] && [ ! -d "$GPROF" ]; then
    mkdir -p "$(dirname "$GPROF")"
    cp -a /usr/share/uno/gimp-config "$GPROF"
fi
# SAID OUT LOUD, because the failure above was invisible.  This is the line
# that distinguishes "the primed profile is in use" from "GIMP is about to
# spend ten minutes rediscovering its own plug-ins".
say "uno-gimp: profile $GPROF pluginrc=$([ -f "$GPROF"/*/pluginrc ] && echo yes || echo NO) single-window=$(grep -ho 'single-window-mode [a-z]*' "$GPROF"/*/sessionrc 2>/dev/null | tail -1)"

IMG=${UNO_IMAGE:-/usr/share/uno/canvas.ppm}
[ -f "$IMG" ] || IMG=""

# THE ONE PROBE THAT SPLITS THE REMAINING QUESTION, run once before the GUI.
# GIMP dies three seconds into startup under unovirt and never under plain
# QEMU, and "startup" covers two entirely different stacks: babl and GEGL
# (pure computation, CPUID-dispatched SIMD, no display) and GTK/XWayland
# (a display server on a framebuffer with no GPU).  `gimp -i` runs the first
# and none of the second.
#
# If this crashes too, the display has nothing to do with it and the fault is
# in the CPU the hypervisor presents.  If it survives, the fault is above
# babl and the display path is where to look.  Either way the next change is
# aimed rather than guessed - and it costs one line and a few seconds.
gimp -i -b '(gimp-quit 0)' > /tmp/gimp-console.log 2>&1
say "uno-gimp: headless probe rc=$? (gimp -i, no display: babl+GEGL only)"
tail -6 /tmp/gimp-console.log | sed 's/^/uno-gimp[probe]| /' > /dev/ttyS0 2>/dev/null

n=0
while :; do
    n=$((n + 1))
    say "uno-gimp: launch $n ($IMG)"
    # THE LAYOUT IS COMPOSED PER LAUNCH, not once per boot: GIMP's toolbox and
    # docks die with the process that owned them, so a restart that did not
    # redo this would come back as a single image window and quietly undo the
    # thing the appliance is for.  It waits for a window itself, so starting
    # it before GIMP is not a race.
    /usr/share/uno/gimp-layout.sh &
    # --no-splash: the splash is a fourth window that covers the other three
    #   for the first minute of a guest this slow, and a screenshot taken
    #   during it shows one window where the point is that there are several.
    # --no-fonts: fontconfig's first scan is the other multi-minute stall, and
    #   nothing this appliance demonstrates needs a font list.  GIMP still
    #   draws its own UI text through GTK.
    # -s / --no-shm is NOT set: XWayland's shm path is how the pixels get
    #   from GIMP to the compositor at all, with no GPU anywhere.
    #
    # ITS OWN LOG FILE, AND THE TAIL GOES OUT ON ttyS0.  GIMP's stderr would
    # otherwise land in the compositor's log inside the guest, which is the
    # one place a host driving this over a URC link cannot open.  The obvious
    # way in - vmgr's Console view, typing `tail /tmp/x.log` at the appliance's
    # own shell - does not work in the build that matters: in a UNO_DEBUG
    # build the shell swallows F12 as its operator escape hatch
    # (pc64_uui.c, `if (scan == 0x16) { pc64_stress_stop(); ... continue; }`)
    # before vmgr ever sees it, so the view never leaves Display and the whole
    # command gets typed into the guest's keyboard instead.  The appliance
    # reporting its own failure needs no view to be switched and cannot be
    # intercepted, so that is what it does.
    gimp --no-splash --no-fonts $IMG > /tmp/gimp.log 2>&1
    RC=$?
    say "uno-gimp: gimp exited rc=$RC (launch $n)"
    # WITHOUT THE PER-KEY NOISE.  libinput debug-events runs from boot and
    # prints a line per keystroke to this same console; anything reported here
    # competes with it, so this stays short and says which launch it belongs to.
    grep -vE "^$" /tmp/gimp.log | tail -14 | sed "s/^/uno-gimp[$n]| /" > /dev/ttyS0 2>/dev/null
    sleep 3
done
GIMPEOF
chmod +x $R/usr/share/uno/gimp.sh

# The X fallback, for the same reason the browser has one: if the compositor
# will not start on some machine, an appliance that still runs is worth more
# than a correct diagnosis.  openbox because focus is not decoration (the
# comment in rootfs_inner.sh), and it is a stacking WM, which is what a
# multi-window app needs on either display server.
cat > $R/usr/share/uno/gimp-x.sh <<'GXEOF'
#!/bin/sh
openbox &
sleep 3
export HOME=/root XDG_CACHE_HOME=/tmp/cache XDG_CONFIG_HOME=/tmp/config
mkdir -p "$HOME" /tmp/cache /tmp/config
GPROF=${XDG_CONFIG_HOME:-$HOME/.config}/GIMP
if [ -d /usr/share/uno/gimp-config ] && [ ! -d "$GPROF" ]; then
    mkdir -p "$(dirname "$GPROF")"; cp -a /usr/share/uno/gimp-config "$GPROF"
fi
IMG=${UNO_IMAGE:-/usr/share/uno/canvas.ppm}
[ -f "$IMG" ] || IMG=""
exec gimp --no-splash --no-fonts $IMG
GXEOF
chmod +x $R/usr/share/uno/gimp-x.sh

# ---- the self-test ----------------------------------------------------------
# WHAT THIS APPLIANCE HAS TO PROVE IS COUNTABLE, which the browser's never
# was.  "Did a keystroke reach the omnibox" needed a screenshot and a human;
# "how many top-level windows does this client have, and where are they" is a
# number and three rectangles, printed on the serial console before anybody
# looks at a picture.  If that line says 3, multi-window works, whatever the
# screenshot turns out to look like.
#
# xdotool, NOT wtype, and that is not a coincidence: GIMP is an X client under
# XWayland, so the X tooling is the tooling that can see it.  A Wayland-native
# client on this compositor would need the opposite one, which is exactly the
# trap rootfs_inner.sh's comment records.
cat > $R/usr/share/uno/gimp-selftest.sh <<'STEOF'
#!/bin/sh
for d in 0 1; do [ -S "/tmp/.X11-unix/X$d" ] && DISPLAY=":$d"; done
export DISPLAY=${DISPLAY:-:0}
if ! xdotool getdisplaygeometry >/dev/null 2>&1; then
    echo "uno: selftest SKIPPED - no X display (XWayland never came up)"
    exit 0
fi
echo "uno: selftest display $DISPLAY geometry $(xdotool getdisplaygeometry | tr ' ' 'x')"

# --onlyvisible, BECAUSE GIMP KEEPS A 10x10 ONE.  A bare `search --class gimp`
# returns unmapped windows too, and GIMP owns a 10x10 helper titled "GNU Image
# Manipulation Program" that is never on screen.  Counting it made a
# single-window session report two top-levels - the right answer for the wrong
# reason, and it is the answer this whole appliance is judged on.
#
# WAIT FOR THE COUNT TO SETTLE, NOT FOR A NUMBER.  The previous version broke
# at >= 2 and so stopped the instant the image window appeared, before the
# toolbox and the docks had been mapped - it reported the count for a moment
# nobody cares about.  GIMP takes minutes to put its windows up on this guest;
# two consecutive samples that agree is what "it has finished" looks like from
# outside, and it needs no magic number to compare against.
i=0; N=0; PREV=-1
while [ $i -lt 40 ]; do
    W=$(xdotool search --onlyvisible --class -- gimp 2>/dev/null)
    N=$(echo "$W" | grep -c '[0-9]')
    [ "$N" -gt 0 ] && [ "$N" = "$PREV" ] && break
    PREV=$N
    i=$((i + 1)); sleep 15
done
echo "uno: gimp top-levels: $N (settled after $((i * 15))s)"
[ "$N" -ge 3 ] || echo "uno: FEWER THAN THREE - this is single-window mode, not a multi-window session"
for w in $W; do
    [ -n "$w" ] || continue
    NAME=$(xdotool getwindowname "$w" 2>/dev/null)
    GEOM=$(xdotool getwindowgeometry --shell "$w" 2>/dev/null \
           | tr '\n' ' ' | sed 's/[A-Z]*=//g')
    echo "uno: window $w '$NAME' $GEOM"
done

# AND THAT THEY ARE INDEPENDENT, which a count alone does not say: three
# windows one program never moves are indistinguishable from one window drawn
# three times.  Moving one and reading its position back is the difference.
TOP=$(echo "$W" | grep '[0-9]' | tail -1)
if [ -n "$TOP" ]; then
    xdotool windowmove "$TOP" 120 90 2>/dev/null
    sleep 2
    echo "uno: moved $TOP -> $(xdotool getwindowgeometry --shell "$TOP" 2>/dev/null | grep -E '^(X|Y)=' | tr '\n' ' ')"
fi
STEOF
chmod +x $R/usr/share/uno/gimp-selftest.sh

# ---- the layout: composed at runtime, not restored from config --------------
# THE LONG WAY ROUND, AND WHY IT IS THE SHORT ONE.  GIMP's multi-window layout
# lives in sessionrc, and three ways of getting a good one there all failed:
#
#   - A fresh profile comes up in SINGLE-window mode, which is the one thing
#     this appliance exists not to demonstrate.
#   - A hand-written `(single-window-mode no)` sessionrc reads to GIMP as "a
#     user who closed every dock": it gives an image window and nothing else.
#   - Letting GIMP author one and flipping the token does not work either,
#     because in single-window mode GIMP records the docks INSIDE the
#     `gimp-single-image-window` block.  Flip the mode and there is no
#     free-standing `session-info "toolbox"` or `"dock"` left to restore.
#
# Capturing a real one meant running GIMP under Xvfb in the build container,
# where it has no dbus machine-id, no iso-codes, no tag cache, and computes
# native window sizes like `1024 x 43721` until X refuses them with BadValue.
# It is not stable enough to drive, and a build step that flakes is worse than
# no build step.
#
# So the appliance opens its own windows, at runtime, through GIMP's own
# accelerators - which is exactly what worked first time on the real guest,
# under a real compositor: Ctrl+B is Windows -> Toolbox, Ctrl+L is Windows ->
# Dockable Dialogs -> Layers.  Composing the layout beats restoring it: it
# needs no captured blob in the tree, it cannot go stale against a GIMP
# upgrade the way a sessionrc format can, and it reads the SCREEN SIZE first,
# so the same appliance lays itself out on unovirt's 800x600 framebuffer and
# on the fast loop's 1280x800 without either being hard-coded.
cat > $R/usr/share/uno/gimp-layout.sh <<'LAYEOF'
#!/bin/sh
# Run in the background by gimp.sh, once, after GIMP has a window.
for d in 0 1; do [ -S "/tmp/.X11-unix/X$d" ] && DISPLAY=":$d"; done
export DISPLAY=${DISPLAY:-:0}

# THE IMAGE WINDOW IS THE ONE WITH THE MENU BAR, and the accelerators only
# exist there.  Its title ends in "GIMP"; the toolbox's is exactly
# "GNU Image Manipulation Program", so matching on the suffix cannot pick the
# wrong one.  Waiting for it rather than sleeping a fixed time: GIMP's startup
# on this guest is measured in minutes and varies with the slice it gets.
# HALF AN HOUR, NOT FIVE MINUTES.  This waited 60 x 5s, which is generous
# under the fast loop - the window arrives in five seconds there - and far
# too short under unovirt, where the guest gets a slice per frame and GIMP
# took four and a half minutes to map the same window.  The wait expired at
# almost exactly the moment the window appeared, so the appliance ran GIMP
# correctly and never laid it out, and said so only on stderr, which is
# inside the guest.  A timeout copied from the fast loop is a timeout
# measured on the wrong machine.
i=0; W=""
while [ $i -lt 180 ]; do
    W=$(xdotool search --onlyvisible --name ' GIMP$' 2>/dev/null | head -1)
    [ -n "$W" ] && break
    i=$((i + 1)); sleep 10
done
if [ -z "$W" ]; then
    echo "uno-layout: NO GIMP image window after $((i * 10))s - not laying out" > /dev/ttyS0 2>/dev/null
    echo "uno-layout: no GIMP image window after $((i * 10))s" >&2
    exit 1
fi
echo "uno-layout: image window $W after $((i * 10))s" > /dev/ttyS0 2>/dev/null
echo "uno-layout: image window $W after $((i * 10))s" >&2

xdotool windowactivate "$W" 2>/dev/null; sleep 2
xdotool key --window "$W" --clearmodifiers ctrl+b; sleep 6    # Toolbox
xdotool key --window "$W" --clearmodifiers ctrl+l; sleep 6    # Layers

# LAID OUT FOR THE SCREEN THAT IS ACTUALLY THERE.  `getdisplaygeometry` prints
# "W H" on ONE line, space separated - not two lines, which is what the first
# version assumed.  `head -1` then returned "1280 800", the numeric test
# rejected it, and both fallbacks fired: the appliance laid itself out for
# 800x600 on a 1280x800 screen and used the left two thirds of it.  It looked
# entirely correct, because 800x600 is the size it runs at under unovirt - a
# wrong answer that happens to match the common case is the hardest kind to
# see.  The fallbacks stay, for an XWayland that answers oddly.
set -- $(xdotool getdisplaygeometry 2>/dev/null)
SW=$1; SH=$2
case "$SW" in ''|*[!0-9]*) SW=800 ;; esac
case "$SH" in ''|*[!0-9]*) SH=600 ;; esac
TW=$((SW / 6)); [ $TW -lt 110 ] && TW=110
DW=$((SW / 4)); [ $DW -lt 180 ] && DW=180
CW=$((SW - TW - DW - 16))
# ROOM FOR THE TITLE BARS.  `xdotool windowmove` positions the CLIENT, and a
# reparenting window manager hangs its decoration above that - so y=0 puts
# every title bar off the top of the screen.  The layout still worked and the
# screenshot still showed three windows, but it showed them looking like three
# panes of one, which is the exact reading this appliance exists to rule out.
# The frame is the evidence; leave it somewhere visible.
TB=28
CH=$((SH - TB - 12))

T=$(xdotool search --onlyvisible --name '^Toolbox$' | head -1)
L=$(xdotool search --onlyvisible --name '^Layers$'  | head -1)
[ -n "$T" ] && { xdotool windowmove "$T" 0 $TB;             xdotool windowsize "$T" $TW $CH; }
[ -n "$L" ] && { xdotool windowmove "$L" $((SW - DW)) $TB;  xdotool windowsize "$L" $DW $CH; }
xdotool windowmove "$W" $((TW + 8)) $TB
xdotool windowsize "$W" $CW $CH
sleep 3

# THE COUNT, ON THE SERIAL PORT, EVERY BOOT.  Not a diagnostic for a bad day -
# it is the appliance's own statement of the thing it exists to show, and it
# has to land somewhere the HOST can read.
#
# ttyS0 AS WELL AS stderr, and that distinction cost a hypervisor run's worth
# of blindness.  stderr here is the compositor's log INSIDE the guest, which
# is exactly the place a host driving this over a URC link cannot reach: under
# the fast loop the two look identical because the serial log carries
# everything, and under unovirt only ttyS0 comes back.  A line that reports
# the headline result to a file nobody outside the guest can open is a line
# that does not report it.
N=$(xdotool search --onlyvisible --class -- gimp 2>/dev/null | grep -c '[0-9]')
MSG="uno-layout: ${SW}x${SH}, toolbox=${T:-MISSING} layers=${L:-MISSING} canvas=$W, $N top-levels"
echo "$MSG" >&2
echo "$MSG" > /dev/ttyS0 2>/dev/null
LAYEOF
chmod +x $R/usr/share/uno/gimp-layout.sh

# ---- priming GIMP's plug-in cache at build time ------------------------------
# ONE THING, THE ONE THAT WORKS HEADLESS.  GIMP executes every plug-in it owns
# once on first run - about ninety binaries - and writes the answer to
# pluginrc.  On a desktop that is seconds; on a guest scheduled a slice per
# frame it is the difference between an appliance that comes up and one that
# appears hung.  `gimp -i` needs no display and does exactly this.
#
# The mode token goes in the same file, because it is the one piece of layout
# state that a hand-written line CAN carry correctly: it is a flag, not a
# window list.
#
# BEST EFFORT, AND LOUD WHEN IT FAILS.  It needs a chroot and /proc; a
# container may refuse them, and that is survivable - the appliance pays the
# first-run cost on its first boot instead.  Doing it silently is not.
#
# EVERY TEARDOWN IS GUARDED.  The first version ended `umount $R/dev
# 2>/dev/null` with nothing after it: the unmount failed, and under `set -e`
# an unguarded command that returns non-zero IS the end of the build - which
# showed up as a build that stopped 32 seconds in, right after the package
# list, with no message of any kind.  `2>/dev/null` hides the reason; it does
# not stop the exit.  That is twice in this file that a "harmless" statement
# in a `set -e` script has ended it in silence.
mkdir -p $R/proc $R/dev $R/sys
MOK=1
mount -t proc none $R/proc 2>/dev/null || MOK=0
echo "uno-build: priming mount $([ $MOK = 1 ] && echo ok || echo REFUSED by the container)"
if [ $MOK = 1 ]; then
    PRIMED=1
    chroot $R /bin/sh -c '
        export HOME=/tmp/prime
        mkdir -p $HOME/.config/GIMP/2.10
        echo "(single-window-mode no)" > $HOME/.config/GIMP/2.10/sessionrc
        timeout 1200 gimp -i -d -f --no-fonts -b "(gimp-quit 0)" >/dev/null 2>&1
        [ -d $HOME/.config/GIMP ] && cp -a $HOME/.config/GIMP /usr/share/uno/gimp-config
        rm -rf $HOME
    ' || PRIMED=0
    echo "uno-build: priming chroot returned $PRIMED"
    umount $R/proc 2>/dev/null || true
else
    echo "uno-build: cannot mount /proc in this container - GIMP profile not primed" >&2
fi

# CHECKED SEPARATELY, because they fail separately and one "primed ok" cannot
# tell them apart.  Both are fatal: a missing pluginrc is an appliance that
# looks hung, and a sessionrc still in single-window mode is an appliance that
# looks like a success while demonstrating nothing.
GCFG=$R/usr/share/uno/gimp-config
S=$GCFG/2.10/sessionrc
if [ -f "$S" ]; then
    HAVE_PLUG=$([ -f $GCFG/2.10/pluginrc ] && echo yes || echo NO)
    HAVE_SWM=$(grep -c 'single-window-mode no' "$S" || true)
    echo "uno-build: GIMP profile $(du -sk $GCFG | cut -f1) KB, pluginrc=$HAVE_PLUG, single-window-mode-no=$HAVE_SWM"
    [ "$HAVE_PLUG" = yes ] || { echo "uno-build: no pluginrc - first boot will re-query every plug-in" >&2; exit 1; }
    [ "$HAVE_SWM" -ge 1 ] || { echo "uno-build: sessionrc does not say single-window-mode no" >&2; exit 1; }
else
    echo "uno-build: GIMP priming produced no profile (rc=$PRIMED)" >&2
    exit 1
fi
}
