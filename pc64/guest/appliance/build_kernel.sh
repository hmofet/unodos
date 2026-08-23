#!/bin/bash
# Build the appliance guest kernel on quill (or any Linux box with the usual
# kernel build deps).  The kernel is GPL and therefore NEVER enters the repo
# (UNOVIRT-PLAN §6): this script is ours, its OUTPUT is a build artefact that
# gets staged as EFI\UNODOS\VM\BZIMAGE by tools/vm_stage.py.
#
#     ./build_kernel.sh [workdir]        # default /work/unodos-guest
#
# Produces <workdir>/bzImage.
set -e
WORK=${1:-/work/unodos-guest}
VER=6.12.10
HERE="$(cd "$(dirname "$0")" && pwd)"

mkdir -p "$WORK"
cd "$WORK"

if [ ! -d "linux-$VER" ]; then
    [ -f "linux-$VER.tar.xz" ] || \
        curl -fLO "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-$VER.tar.xz"
    tar xf "linux-$VER.tar.xz"
fi

cd "linux-$VER"
mkdir -p "$WORK/kbuild"
make O="$WORK/kbuild" defconfig
# merge_config -m only merges; olddefconfig then resolves dependencies.  The
# fragment carries "# CONFIG_MODULES is not set", which is how a fragment
# DISABLES an option - deleting the line would silently keep modules on.
./scripts/kconfig/merge_config.sh -m -O "$WORK/kbuild" \
    "$WORK/kbuild/.config" "$HERE/unodos-guest.config"
make O="$WORK/kbuild" olddefconfig

# The merge is only as good as its report: verify the load-bearing options
# actually survived dependency resolution rather than trusting the tool.
for opt in VIRTIO_MMIO_CMDLINE_DEVICES VIRTIO_BLK SERIO_I8042 KEYBOARD_ATKBD \
           MOUSE_PS2 FB_EFI FRAMEBUFFER_CONSOLE INPUT_EVDEV EXT4_FS; do
    grep -q "^CONFIG_$opt=y" "$WORK/kbuild/.config" || {
        echo "MISSING: CONFIG_$opt did not make it into .config" >&2; exit 1; }
done
grep -q "^CONFIG_MODULES=y" "$WORK/kbuild/.config" && {
    echo "CONFIG_MODULES survived - the fragment failed to disable it" >&2; exit 1; }

# The android set is verified separately because it fails differently: these
# are options whose NAMES have moved between kernel versions (CONFIG_ANDROID,
# the menu, was deleted in 5.19; CONFIG_ASHMEM in 5.18), so a fragment line
# that no longer matches anything merges silently and produces a kernel that
# boots fine and cannot start a container.  Checking the .config is the only
# place that distinguishes "asked for" from "got".
for opt in ANDROID_BINDER_IPC ANDROID_BINDERFS NAMESPACES NET_NS PID_NS            CGROUPS CGROUP_DEVICE CGROUP_FREEZER VETH BRIDGE NF_NAT PSI            SND_VIRTIO; do
    grep -q "^CONFIG_$opt=y" "$WORK/kbuild/.config" || {
        echo "MISSING: CONFIG_$opt (android set) did not make it into .config" >&2
        exit 1; }
done
grep -q '^CONFIG_ANDROID_BINDER_DEVICES="binder,hwbinder,vndbinder"'      "$WORK/kbuild/.config" || {
    echo "MISSING: binder device nodes - the container will find no /dev/binder" >&2
    exit 1; }

make O="$WORK/kbuild" -j"$(nproc)" bzImage
cp "$WORK/kbuild/arch/x86/boot/bzImage" "$WORK/bzImage"
ls -la "$WORK/bzImage"
echo "kernel: $WORK/bzImage"
