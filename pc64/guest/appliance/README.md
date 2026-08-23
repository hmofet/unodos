# The appliance payload

The Linux guest the pc64 hypervisor boots: kernel, initramfs, and (for the
browser appliance) an Alpine + wlroots + Chromium rootfs.  Everything in this
directory is OURS (build scripts and configs); everything it BUILDS is GPL
or MIT third-party and therefore never enters the repo - the artefacts are
staged onto the ESP as files (`EFI\UNODOS\VM\*`), which is mere aggregation
(UNOVIRT-PLAN §6).

Build on quill (or any Linux box with kernel build deps + docker):

    ./build_kernel.sh   /work/unodos-guest     # -> bzImage   (~15 min)
    ./build_initrd.sh   /work/unodos-guest     # -> initrd.gz (seconds)
    ./build_rootfs.sh   /work/unodos-guest     # -> rootfs.img (~5 min, docker)

Then stage into a pc64 debug build (the payload paths are what
`tools/vm_stage.py` looks for):

    cp /work/unodos-guest/bzImage   build/bzImage
    cp /work/unodos-guest/initrd.gz build/initrd.gz
    cp /work/unodos-guest/rootfs.img build/rootfs.img      # optional
    UNO_DISK_MIB=1400 python3 tools/vm_stage.py            # 1400 with rootfs

Verification:

    python3 tools/hv_remote.py <kvm-box>  --time=380       # serial: shell answers
    python3 tools/vm_display_urc.py /tmp/unodos-uefi.img 600 4 example.net

## The fast loop, and why it exists

A hypervisor run is about twenty-five minutes.  The SAME appliance boots
under plain QEMU with a VGA in about two, and every late bug in this
directory was found there:

    qemu-system-x86_64 -enable-kvm -m 2048 -smp 2         -kernel bzImage -initrd initrd.gz         -drive file=rootfs.img,format=raw,if=virtio,readonly=on         -append "console=ttyS0 rdinit=/init nokaslr"         -netdev user,id=n0 -device virtio-net,netdev=n0         -vga std -display none         -serial unix:/tmp/lab.ser,server=on,wait=off         -monitor unix:/tmp/lab.mon,server=on,wait=off

`socat UNIX-CONNECT:/tmp/lab.ser PTY,link=/tmp/lab.pty,raw,echo=0` gives a
live shell on the appliance's ttyS0 (ONE connection - the chardev accepts
one, so present it as a pty and let a reader and a writer share it), and
`screendump` over the monitor socket photographs it.

The kernel carries `DRM_BOCHS` and `DRM_VIRTIO_GPU` purely so this loop has
a display; UnoDOS offers the guest no PCI at all and never uses them.

**QEMU's keyboard is not the one under test.**  To exercise the exact byte
stream `unovdev_pc.c` sends, use the appliance's own injector, which goes in
through i8042 controller command 0xD2 ("write keyboard output buffer") and
so reaches atkbd as if the keyboard had sent it:

    /usr/share/uno/inject_scancodes.sh 29 38 166 157     # ctrl+L, set 1

`libinput debug-events` runs from boot and prints every key the guest's
input stack saw to ttyS0, so the two ends of the wire can be compared
directly.

What each piece is:

- **bzImage** - 6.12 LTS, `unodos-guest.config` merged over defconfig.  NO
  modules: a driver that exists only as a module is a device that silently
  does not work in an appliance.  The config matches the machine model in
  `unovdev.c`/`unovdev_pc.c` exactly; if a device is added there, its driver
  goes here in the same change.
- **initrd.gz** - static busybox.  With no disk: a shell on ttyS0 (the
  harness) and tty1 (the Display window).  With `/dev/vda` carrying
  `/sbin/uno-init`: switch_root into the appliance.
- **rootfs.img** - Alpine + a wlroots kiosk (cage, pixman renderer) +
  Chromium, read-only ext4, every writable path a tmpfs, zram swap.  Its
  init is `/sbin/uno-init` (written by `rootfs_inner.sh`), which also keeps
  a shell on ttyS0.  Xorg is a fallback and segfaults on this guest's
  framebuffer; cage is the real display server.  Chromium is restarted
  inside the compositor rather than around it, because cage exits when its
  client does.
