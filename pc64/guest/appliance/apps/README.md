# Adding an appliance

One file. `apps/<name>.app` is a POSIX shell fragment **sourced by
`rootfs_inner.sh` inside the build container**, with `$R` set to the rootfs
root. It declares what the appliance runs; everything else - the kernel, the
init, the network bring-up, the input diagnostics, the packing - is common and
belongs to nobody.

    UNO_APP=gimp ./build_rootfs.sh /work/unodos-guest

## What a `.app` file sets

| Variable | Means | Default |
|---|---|---|
| `APP_PKGS` | extra apk packages, on top of the base | none |
| `APP_COMPOSITOR` | `cage` (one client, full-screen, forever) or `labwc` (stacking WM, outlives its clients) | `cage` |
| `APP_CLIENT` | what the compositor runs; absolute path inside the rootfs | `/usr/share/uno/app.sh` |
| `APP_XCLIENT` | the Xorg fallback session, or empty for none | none |
| `APP_SELFTEST` | run once, 60 s after boot, output to `ttyS0` | none |
| `APP_ENV` | one line appended verbatim to `app.env`, sourced by `uno-init` | none |
| `app_write()` | writes those scripts, and any config, into `$R` | no-op |

`rootfs_inner.sh` writes `/usr/share/uno/app.env` from the first six and
**refuses to pack an image** whose `APP_CLIENT` / `APP_XCLIENT` /
`APP_SELFTEST` are not executable files, or whose compositor no package
installed. A rootfs whose session is empty is the most expensive failure in
this directory to diagnose, and it is a spelling mistake away at all times.

## Which compositor

`cage` gives its single client the whole output, focused, for as long as it
lives, and **exits when that client does** - so a client restart loop has to
run *inside* it. That is the right shape for a browser and the wrong shape for
anything with a second window.

`labwc` is a stacking window manager on the same wlroots + pixman software
renderer, so it runs on this guest's dumb framebuffer for the same reasons
cage does. It **outlives its clients**, takes its client on `-s` rather than
after `--`, and gives every window a title bar, a focus and a stacking order.
Multi-window applications need it.

Neither one wants Xorg. Xorg segfaults on this guest's framebuffer (simpledrm,
no GPU, no render node); the X fallback exists for machines where the
compositor will not start at all, not as a preference.

## Two things the whole directory has learnt

**Check flags against the binary, never against your memory of it.** `cage -D`
does not exist in cage 0.1.5: it printed its usage, exited 1, and the
appliance fell through to the X fallback on every boot for a week - silently,
because the fallback works under plain QEMU. `uno-init` now asks each
compositor for its own help before deciding how to hand it a client.

**A self-test that cannot fail proves nothing.** `xdotool` speaks X and
nothing else; run against a Wayland session it fails every command and reports
success. Match the tool to the display server the client is actually on -
which for an XWayland client (GTK2, so GIMP) is the X tooling, on the Wayland
compositor.

## The appliances that exist

| Name | Runs | Compositor | Proves |
|---|---|---|---|
| `chromium` | Chromium on Ozone/Wayland | `cage` | rendering, network, and a host-typed address navigating a real browser (M5) |
| `gimp` | GIMP 2.10 (GTK2) under XWayland | `labwc` | several windows, independently placed and focused, driven by a host-side pointer |
