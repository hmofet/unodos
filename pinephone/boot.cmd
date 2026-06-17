echo "=== UnoDOS 3 / PinePhone (Allwinner A64) ==="
setenv loadaddr 0x40080000
if fatload ${devtype} ${devnum}:${distro_bootpart} ${loadaddr} unodos.bin; then
  echo "Loaded unodos.bin -> disabling caches and launching at ${loadaddr}"
  dcache flush
  dcache off
  icache off
  go ${loadaddr}
fi
if fatload mmc 0:1 ${loadaddr} unodos.bin; then
  echo "Loaded via mmc0:1 -> disabling caches and launching"
  dcache flush
  dcache off
  icache off
  go ${loadaddr}
fi
echo "!! unodos.bin not found"
