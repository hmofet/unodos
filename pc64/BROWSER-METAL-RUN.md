# Verifying the unoweb engine path on metal (ZimaBlade)

Two things about the browser's engine path cannot be verified in QEMU, and
this is how to close them on the always-on pc64 test box.

## Why this document exists

**QEMU delivers no pointer input to this guest.** Parking the mouse at a known
position and screenshotting shows no cursor, with or without
`-device qemu-xhci -device usb-tablet`. A click test there proves nothing in
either direction, so `uw_hit_test` -> `uw_link_at` -> navigate has never been
exercised end to end. Keyboard link navigation was added as the primary path
precisely because it *is* drivable; the pointer path is still unproven.

The good news: **the pointer does not need a human or a physical mouse.** URC
already carries an input-injection verb (`pc64/REMOTE.md`):

| verb | meaning |
|---|---|
| `key <scan> <uni> [ctrl]` | inject a keypress |
| `pointer <x> <y> <btn>` | inject a pointer event |

`pointer` lands in `uno_pc64_inject_pointer()` (`unoauto_remote.c`), which is
the same path a real mouse feeds, so a click injected this way exercises
exactly the code a user's click would.

## The box

| | |
|---|---|
| ZimaBlade | `192.168.2.118`, MAC `00:e0:4c:30:5b:d4`, r8169 gigabit |
| driven from | `devbuntu`, via `~/urc_bridge.py` |
| install path | network install (URC `install <disk>`), no USB stick needed |

Both were reachable when this was written (ping 3 ms; the bridge script is in
place).

**Check who else is on the bridge before you start.** As of 2026-07-27 the
running instance is `urc_bridge.py 5098`, its state lives in `~/urc/`
(`cmd.txt`, `session.log`), and its live link was to **192.168.2.254** - the
iwlwifi WiFi bring-up session, not this box. Driving verbs into that link, or
rebooting the box on the other end of it, would wreck another lane's work.

master's `urc_bridge.py` takes a per-box directory as `argv[2]` exactly so
boxes do not share state, so the correct move is a SECOND bridge:

```
python3 ~/urc_bridge.py 5099 ~/urc-zima
```

and the ZimaBlade dialing into that port. Confirm with `ss -tn | grep 5099`
that the peer is 192.168.2.118 before sending anything - a bridge with the
wrong box on it looks identical until the verbs land somewhere unexpected.

## The run

1. **Build the engine image.** The engine path is not the default, so it must
   be asked for explicitly:

   ```
   cd pc64 && BROWSER_ENGINE=uw ./build.sh
   ```

   That flag also pulls in unomedia's image half, which the default kernel
   does not carry.

2. **Install it over the network.** See `pc64/REMOTE.md` for the `install
   <disk>` verb and `unostorage`'s clone-over-link. The USB flasher is not
   needed (`CLAUDE.md`, 2026-07-23).

3. **Put a page with a link on the box.** Anything with an `<a href>` will do;
   the browser's built-in `Sample.html` already has one
   (`<a href='none'>links</a>`), which is convenient because following it
   fails with a DNS error - and that failure is unambiguous evidence the click
   resolved to the link rather than to the paragraph around it.

4. **Open the browser and the page** with `key` injections (Ctrl-Esc for the
   launcher, Down x13 to Browser, Enter; then Down to Sample.html, Enter).

5. **Screenshot, locate the link, click it.** Take a capture first so the
   link's on-screen position is known, then:

   ```
   pointer <x> <y> 1      # button down at the link
   pointer <x> <y> 0      # release
   ```

   Coordinates are in the framebuffer's own pixels. Note the box renders at
   its native resolution, so unlike the QEMU screendump (which is scaled)
   there is no conversion to do.

6. **Screenshot again.** Success is the address bar reading `none` and a
   "Couldn't load the page / DNS lookup failed" body - the same evidence the
   keyboard path produced in QEMU.

## What a failure would mean

If the click does nothing, the suspects in order are: the canvas not receiving
`UI_EV_MOUSE_DOWN` in document view; the event's coordinates being window
relative rather than screen relative (the handler subtracts `g_rect` and adds
`g_scroll`); or `uw_hit_test` attributing the point to the block rather than
the `<a>`. The last is covered by a host test (`hit-test`), so the first two
are likelier.

## Also worth doing while the box is up

- **Network image fetching** is not built, so a page's `<img>` only resolves
  from local files. Nothing to test yet; noted so the run is not mistaken for
  a full image check.
- The engine path is still opt-in. Nothing here flips the default.
