# The Chromium appliance: a browser, full-screen, driveable.
#
# Sourced by rootfs_inner.sh inside the build container with $R = the rootfs
# root.  See apps/README.md for the contract.
#
# cage, because this app wants exactly one window and wants it focused
# forever.  A kiosk compositor is the right shape for a browser and the wrong
# shape for anything that puts up a second window - see apps/gimp.app.
APP_PKGS="chromium cage"
APP_COMPOSITOR=cage
APP_CLIENT=/usr/share/uno/browser.sh
APP_XCLIENT=/usr/share/uno/session.sh
APP_SELFTEST=/usr/share/uno/selftest.sh

# THE STARTUP PAGE MUST NOT LOOK LIKE THE DESTINATION.  It was example.org,
# and the host harness types example.net - two pages whose bodies are the SAME
# four lines of text, so the screenshot that is supposed to prove a host-typed
# navigation showed a page identical to the one already there, and only the
# address bar told them apart.  The appliance's own page is unmistakable,
# needs no network, and so cannot cache a startup failure either (the M3
# trap).
APP_ENV='export UNO_URL_ENV=${UNO_URL:-file:///usr/share/uno/welcome.html}'

app_write() {
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

# A COMMENT INSIDE A LINE CONTINUATION ENDS THE COMMAND.  The flag list
# above used to carry an explanatory comment between two backslashed lines:
# the backslash joins the lines, the `#` then starts a comment that runs to
# the real newline, and the command ENDS there.  Every flag after it became
# its own "not found" line, so this session's Chromium ran without the DNS
# workaround and without a URL at all - and none of that appears in a
# screenshot of a browser sitting on its new-tab page.  Comments go above the
# command now, never inside it.
#
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

# `--disable-features=...AsyncDns,DnsOverHttps` is load-bearing: Chromium's
# own async resolver and Secure DNS ignore the resolv.conf that nslookup and
# wget both use happily, and answer ERR_NAME_NOT_RESOLVED for names the rest
# of the guest resolves fine.  Sending it back to getaddrinfo makes the
# browser agree with its own operating system.
exec chromium \
    --no-sandbox --disable-gpu --disable-dev-shm-usage \
    --no-first-run --no-default-browser-check --disable-infobars \
    --password-store=basic --disable-sync \
    --disable-background-networking --disable-component-update \
    --disable-domain-reliability --disable-breakpad \
    --disable-client-side-phishing-detection --no-pings \
    --safebrowsing-disable-auto-update --metrics-recording-only \
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
# SAY WHY IT DIED.  Chromium in the carve ends sessions, and without its own
# log the only trace is crashpad's process reader complaining about
# `sched_getscheduler` - a warning printed while CAPTURING a dump, not the
# reason for one.  With logging on, the line under it reads
# `Assertion failed: v > 0 (double-conversion/fast-dtoa.cc: FastDtoa: 641)`:
# Alpine builds Chromium against the SYSTEM double-conversion, which keeps its
# assertions, so a value upstream's bundled copy would format and forget
# aborts the browser here.
#
# RESTARTED INSIDE THE COMPOSITOR, NOT AROUND IT.  cage exits when its client
# does, so an `exec` here turned one Chromium abort into a whole session
# restart - seatd, a VT switch, a compositor and a browser cold-start, minutes
# of wall time on this guest, during which the appliance shows a black screen
# and anything typed at it is lost.  Looping in here keeps cage's client alive,
# so the compositor never goes down and only the browser comes back.
# --metrics-recording-only is gone with it: it turns metrics recording ON
# (without upload), which is work nobody asked for on a quarter-core guest.
n=0
while :; do
    n=$((n + 1))
    echo "uno-browser: launch $n" >&2
    chromium     --enable-logging=stderr --ozone-platform=wayland --no-sandbox --disable-gpu --disable-dev-shm-usage --no-first-run --no-default-browser-check --disable-infobars --password-store=basic --disable-sync --disable-background-networking --disable-component-update --disable-domain-reliability --disable-breakpad --disable-client-side-phishing-detection --no-pings --safebrowsing-disable-auto-update --disable-features=OptimizationHints,MediaRouter,AsyncDns,DnsOverHttps --renderer-process-limit=1 --process-per-site --disable-hang-monitor --disable-session-crashed-bubble --js-flags=--max-old-space-size=192 --start-maximized "$UNO_URL_ENV"
    echo "uno-browser: chromium exited rc=$? (launch $n)" >&2
    sleep 2
done
BROWSEREOF
chmod +x $R/usr/share/uno/browser.sh
# NO OMNIBOX SUGGESTIONS, BY POLICY.  Every letter typed into the address bar
# sends a suggest query to Google and repaints a ten-row dropdown over the
# page - the only thing this browser does that is triggered BY TYPING, and the
# thing the FastDtoa abort (see browser.sh) lands in run after run, always on
# the first address typed and never while the browser sits idle.  Chromium's
# managed-policy directory is the supported way to switch it off; a
# command-line flag for it does not exist.  The harness types a literal URL,
# so nothing of value is lost.
mkdir -p $R/etc/chromium/policies/managed
cat > $R/etc/chromium/policies/managed/uno-appliance.json <<'POLEOF'
{
  "SearchSuggestEnabled": false,
  "DefaultSearchProviderEnabled": false,
  "MetricsReportingEnabled": false,
  "BackgroundModeEnabled": false,
  "SafeBrowsingProtectionLevel": 0,
  "PromotionalTabsEnabled": false
}
POLEOF

mkdir -p $R/usr/share/uno
cat > $R/usr/share/uno/welcome.html <<'EOF'
<!doctype html>
<meta charset="utf-8">
<title>UnoDOS appliance</title>
<style>
  /* LIGHT ON PURPOSE.  The harness decides whether the guest is showing a
     browser or a console by how much of its surface is near-black, and a
     dark splash page is a console as far as that test is concerned. */
  body { background:#f4f6fb; color:#1a1a2e; font-family:sans-serif;
         display:flex; align-items:center; justify-content:center;
         height:100vh; margin:0; }
  .card { text-align:center; }
  h1 { font-size:2.2em; margin-bottom:.2em; }
  p  { color:#456; }
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

# THE SELF-TEST THAT SPLITS THE REMAINING QUESTION.  Runs have shown a browser
# that renders and will not take a keystroke, with the emulated keyboard
# provably emitting and the guest provably draining it.  So: type the same
# navigation from INSIDE the guest.  If the page changes, the display server
# and Chromium are fine and the gap is between libinput and the emulated
# device; if it does not, the gap is above them both.  Either way the next
# change is aimed rather than guessed.
#
# ONE TOOL PER DISPLAY SERVER.  xdotool speaks X and nothing else, so on the
# Wayland session it ran, found no display, failed every command and reported
# success anyway - a self-test that cannot fail proves nothing, and it hid the
# fact that the appliance had moved off X.
cat > $R/usr/share/uno/selftest.sh <<'STEOF'
#!/bin/sh
# THREE DIFFERENT PAGES, and they have to stay different: the browser opens on
# the appliance's own welcome.html, this self-test drives it to example.com,
# and the HOST harness types example.net.  For a while the self-test and the
# host both used example.net, and before that the startup page was example.org
# - whose body is the SAME four lines as example.net's.  Either way the
# screenshot that is supposed to prove the host drove the browser proved
# nothing at all.
if [ -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ] && [ -x /usr/bin/wtype ]; then
    echo "uno: selftest via wtype (wayland)"
    if wtype -M ctrl l -m ctrl 2>/tmp/wtype.err; then
        sleep 1
        wtype -d 120 "example.com" 2>>/tmp/wtype.err
        wtype -k Return 2>>/tmp/wtype.err
        echo "uno: selftest typed example.com from inside"
    else
        echo "uno: selftest FAILED: $(head -c 120 /tmp/wtype.err | tr '\n' ' ')"
    fi
elif xdotool getdisplaygeometry >/dev/null 2>&1; then
    W=$(xdotool search --class -- chromium 2>/dev/null | tail -1)
    echo "uno: selftest via xdotool (X), window=$W"
    [ -n "$W" ] && xdotool windowactivate --sync "$W" 2>/dev/null
    xdotool key --clearmodifiers ctrl+l 2>/dev/null
    sleep 1
    xdotool type --delay 120 "example.com" 2>/dev/null
    xdotool key Return 2>/dev/null
    echo "uno: selftest typed example.com from inside"
else
    echo "uno: selftest SKIPPED - no X display and no wayland socket"
fi
STEOF
chmod +x $R/usr/share/uno/selftest.sh
}
