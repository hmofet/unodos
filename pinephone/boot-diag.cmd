echo "==================================================="
echo " UnoDOS 3 / PinePhone : SELF-DIAGNOSING SD BOOT"
echo " Reading this => OUR U-Boot ran from the microSD."
echo "==================================================="
echo "devtype=${devtype} devnum=${devnum} bootpart=${distro_bootpart}"
setenv loadaddr 0x40080000
sleep 5
echo "[1] fatls mmc 0:1  (SD FAT root contents):"
fatls mmc 0:1
sleep 5
echo "[2] fatload unodos.bin -> ${loadaddr} ..."
if fatload mmc 0:1 ${loadaddr} unodos.bin; then
  echo "    OK: loaded ${filesize} bytes."
  echo "    Launching in 5s -- watch for RED then GREEN then BLUE (payload POST)."
  sleep 5
  dcache flush
  dcache off
  icache off
  go ${loadaddr}
  echo "    !! go returned unexpectedly (payload exited)"
else
  echo "    !! FAILED to fatload unodos.bin from the SD FAT partition"
fi
echo "==================================================="
echo " HALT -- deliberately NOT booting eMMC/postmarketOS."
echo " Note the LAST [stage] shown above and report it."
echo " Power-cycle to retry."
echo "==================================================="
while true; do sleep 10; done
