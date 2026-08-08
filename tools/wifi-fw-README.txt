UnoDOS Wi-Fi firmware helper
============================

UnoDOS drives Intel wireless adapters, but Intel's firmware is not ours to
redistribute, so published images ship without it. This tool puts it on your
own stick.

You only need this on REAL HARDWARE with an Intel Wi-Fi card. In a virtual
machine the emulated network card works out of the box, and wired Ethernet
works everywhere without it.

Write unodos-pc64-hybrid.img to a USB stick first, then:

  Windows   double-click uno-wifi-fw.cmd
  macOS     double-click uno-wifi-fw.command
  Linux     python3 uno-wifi-fw.py

It auto-detects the card and the stick. To be explicit:

  python3 uno-wifi-fw.py --list-cards
  python3 uno-wifi-fw.py --card ax201 --dest E:\
  python3 uno-wifi-fw.py --card ax210 --dest /Volumes/UNODOS
  python3 uno-wifi-fw.py --source local    (copy from this machine's /lib/firmware)

It fetches the blob from Debian's non-free-firmware pool, or copies it from a
Linux machine you already have, and writes it under the exact name the driver
opens. Python 3 standard library only. Nothing to install.
