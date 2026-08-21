# The appliance payload

The Linux guest the pc64 hypervisor boots: kernel, initramfs, and (for the
browser appliance) an Alpine+Xorg+Chromium rootfs.  Everything in this
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
    UNO_DISK_MIB=1200 python3 tools/vm_stage.py            # 1200 with rootfs

Verification:

    python3 tools/hv_remote.py <kvm-box>  --time=380       # serial: shell answers
    python3 tools/vm_display_urc.py /tmp/unodos-uefi.img   # display: on the kvm box

What each piece is:

- **bzImage** - 6.12 LTS, `unodos-guest.config` merged over defconfig.  NO
  modules: a driver that exists only as a module is a device that silently
  does not work in an appliance.  The config matches the machine model in
  `unovdev.c`/`unovdev_pc.c` exactly; if a device is added there, its driver
  goes here in the same change.
- **initrd.gz** - static busybox.  With no disk: a shell on ttyS0 (the
  harness) and tty1 (the Display window).  With `/dev/vda` carrying
  `/sbin/uno-init`: switch_root into the appliance.
- **rootfs.img** - Alpine + Xorg + Chromium in kiosk mode, read-only ext4,
  every writable path a tmpfs, zram swap.  Its init is `/sbin/uno-init`
  (written by `rootfs_inner.sh`), which also keeps a shell on ttyS0.
