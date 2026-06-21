#!/bin/sh
# UnoDOS / PinePhone (Allwinner A64) — FEL (USB) debug kit.
# =============================================================================
# The PinePhone has NO dedicated debug port: the only no-serial signal channel is
# the SoC's FEL mode (USB OTG BootROM protocol). When the BROM finds no valid boot
# medium it listens for commands on USB; `sunxi-tools` (sunxi-fel) drives it. This
# lets us bisect a "fully dark, won't boot" phone with nothing but a USB-C cable —
# no UART, no serial adapter.
#
# How to put the phone in FEL (button-free):  flash `fel-sdboot.sunxi` to the SPL
# slot of a microSD (this script's `felcard`), insert it, power on. The BROM runs
# that tiny SPL which immediately enters FEL. The screen STAYS DARK in FEL — that
# is expected; the phone is alive and listening on USB. Connect USB to this box.
#
# Then bisect, in order:
#   ./fel.sh probe        # is the SoC alive?  (rules out dead battery / dead board)
#   ./fel.sh uboot        # run OUR U-Boot from RAM -> does the PANEL light?
#   ./fel.sh payload      # (advanced) run U-Boot from RAM, which then loads the
#                         #            card's boot.scr+unodos.bin -> POST beacon
#
# Interpreting `uboot`:
#   * panel LIGHTS (U-Boot console/logo) -> U-Boot is GOOD; the fault is the
#     BROM->SD->SPL handoff (bad card write / wrong card / SD electrical), NOT our
#     firmware. Re-flash a fresh card (mksd.sh image) and retry a normal boot.
#   * panel STAYS DARK -> U-Boot itself can't bring up the DSI panel/PMIC/backlight;
#     focus on the U-Boot panel/regulator config, not the SD path.
#
# Run this ON THE LINUX BOX the phone's USB-C is plugged into (e.g. devbuntu), with
# sunxi-tools installed.  Artifacts come from ../pine-uboot (the mksd.sh build tree)
# unless overridden:  WORK=<dir>  FW=<u-boot-sunxi-with-spl.bin>
set -e
cd "$(dirname "$0")"
HERE=$(pwd)

WORK="${WORK:-$HOME/pine-uboot}"
FW="${FW:-$WORK/u-boot/u-boot-sunxi-with-spl.bin}"
[ -f "$FW" ] || FW="$HERE/build/u-boot-sunxi-with-spl.bin"
FELSDBOOT="${FELSDBOOT:-$WORK/fel-sdboot.sunxi}"

need() { command -v "$1" >/dev/null 2>&1 || { echo "missing '$1' (apt install sunxi-tools)"; exit 1; }; }

usb_hint() {
  echo "  (FEL device should appear in lsusb as '1f3a:efe8 Allwinner ... FEL mode')"
  lsusb 2>/dev/null | grep -i "1f3a:efe8" && echo "  -> FEL device present" \
    || echo "  -> no FEL device yet; insert the fel-sdboot card, power on, plug in USB"
}

cmd_probe() {
  need sunxi-fel
  echo "[probe] waiting for the SoC in FEL mode (Ctrl-C to stop)..."
  usb_hint
  i=0
  while [ $i -lt 60 ]; do
    if sunxi-fel version 2>/dev/null; then
      echo "[probe] *** SoC ALIVE in FEL — power + USB-OTG OK ***"
      return 0
    fi
    i=$((i+1)); sleep 1
  done
  echo "[probe] no response after 60s. Either not in FEL, dead battery, or USB/cable."
  return 1
}

cmd_uboot() {
  need sunxi-fel
  [ -f "$FW" ] || { echo "missing U-Boot: $FW (build it with ./mksd.sh fw)"; exit 1; }
  echo "[uboot] loading + running our U-Boot from RAM: $FW"
  echo "        WATCH THE PANEL. Lights => U-Boot good (SD-boot path is the fault)."
  echo "                         Dark   => U-Boot can't light the panel."
  sunxi-fel -v uboot "$FW"
}

# run: the FAST bare-payload dev loop now that the payload lights the panel itself
# (Path A). FEL can't run a DRAM payload cold (DRAM is dark in the BROM), so we use
# the SPL to bring DRAM up, then load + execute unodos.bin straight into RAM — NO SD
# write per iteration. `sunxi-fel spl <fw>` runs the SPL (it inits DRAM and returns
# to FEL via the eGON thunk); then we write the payload to the arm64 load address and
# jump. The panel bring-up (clocks/PMIC/DSI/D-PHY/ST7703/TCON0/backlight) runs in the
# payload, so the screen should light WITHOUT U-Boot.  Usage: ./fel.sh run [payload]
cmd_run() {
  need sunxi-fel
  PAYLOAD="${2:-$HERE/build/unodos.bin}"
  [ -f "$PAYLOAD" ] || PAYLOAD="$HERE/build/unodos_paneldbg.bin"   # fall back to the LED-staged build
  [ -f "$PAYLOAD" ] || { echo "no payload: build with ./build.sh (or ./build.sh paneldbg)"; exit 1; }
  [ -f "$FW" ] || { echo "missing SPL/U-Boot $FW (build with ./mksd.sh fw) — needed to init DRAM"; exit 1; }
  echo "[run] SPL DRAM init: $FW"
  sunxi-fel spl "$FW"
  echo "[run] load $PAYLOAD @ 0x40080000 and execute"
  sunxi-fel write 0x40080000 "$PAYLOAD"
  sunxi-fel exec 0x40080000
  echo "[run] executing. WATCH THE PANEL (and the green PD18 LED for a paneldbg build)."
  echo "      paneldbg blink codes: 1=clocks 2=PMIC 3=reset/DSI/DPHY 4=panel-reset-hi"
  echo "                            5=ST7703-init 6=TCON0/HS/backlight done."
}

cmd_payload() {
  # Run our U-Boot from RAM; it then runs distro_bootcmd, which scans the inserted
  # card and finds boot.scr + unodos.bin (present even on a felcard — fel-sdboot only
  # overwrites the SPL slot, the FAT partition is intact). End result: the POST
  # beacon (build a POST payload first: ./build.sh post, then put unodos_post.bin on
  # the card as unodos.bin, or use the normal payload).
  cmd_uboot
}

# Build a "FEL trigger" SD image: a normal UnoDOS card (FAT: boot.scr + unodos.bin)
# but with fel-sdboot.sunxi in the SPL slot instead of U-Boot, so the BROM drops to
# FEL while leaving the payload on the card for the `payload` test.
cmd_felcard() {
  IMG="$HERE/build/pinephone-felcard.img"
  BASE="$HERE/build/pinephone-unodos.img"
  # Fall back to the mksd.sh build tree (e.g. on devbuntu where the repo isn't checked out).
  [ -f "$BASE" ] || { BASE="$WORK/pine.img"; IMG="$WORK/pine-felcard.img"; }
  [ -f "$FELSDBOOT" ] || { echo "missing $FELSDBOOT — fetch fel-sdboot.sunxi from sunxi-tools"; exit 1; }
  [ -f "$BASE" ] || { echo "missing base image $BASE — run ./mksd.sh image first"; exit 1; }
  cp "$BASE" "$IMG"
  # fel-sdboot occupies the same sector-16 SPL slot; overwrite just that, keep FAT.
  dd if="$FELSDBOOT" of="$IMG" bs=1024 seek=8 conv=notrunc,fsync status=none
  echo "[felcard] $IMG  (boot -> FEL; FAT payload intact for ./fel.sh payload)"
  echo "          write it like a normal card:  ./mksd.sh write /dev/sdX   (point IMG at it)"
}

case "${1:-}" in
  probe)   cmd_probe ;;
  run)     cmd_run "$@" ;;
  uboot)   cmd_uboot ;;
  payload) cmd_payload ;;
  felcard) cmd_felcard ;;
  *) echo "usage: ./fel.sh {probe|run|uboot|payload|felcard}"; echo
     echo "  probe   - is the SoC alive in FEL? (sunxi-fel version)"
     echo "  run     - SPL inits DRAM -> load unodos.bin into RAM -> exec (panel self-lit)"
     echo "            ./fel.sh run [payload.bin]   (the fast no-SD-write dev loop)"
     echo "  uboot   - run our U-Boot from RAM; watch if the panel lights"
     echo "  payload - U-Boot from RAM -> card's boot.scr -> unodos.bin (POST beacon)"
     echo "  felcard - build a fel-sdboot trigger card image (keeps the FAT payload)" ;;
esac
