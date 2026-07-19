# Installing UnoDOS/pc64 to a local disk

UnoDOS boots from the USB stick the flasher writes, and can install itself to a
machine's internal disk from the running system: desktop icon **Install** (or
Start → Install).

Keyboard drive (works even where the pointer doesn't): **Up/Down** pick a
target, **I** installs, **R** rescans.

## The two target kinds

| Listing | What happens |
|---|---|
| `Volume "..."  ESP (has \EFI)  [keeps data]` | **Non-destructive.** Copies the running system into `\EFI\UNODOS\` on that FAT/ESP volume (BOOTX64.EFI + fonts + docs), adds a `UnoDOS` UEFI boot entry (NVRAM `Boot####` + `BootOrder`), and — only if the volume has no `\EFI\BOOT\BOOTX64.EFI` already — a removable-media fallback copy. Nothing is deleted; an existing Windows install keeps booting. |
| `Disk ...  [ERASES ALL]` | **Destructive.** Clones the boot USB's GPT + ESP onto that disk, relocates the backup GPT to the disk's real end (CRCs recomputed), and adds the boot entry. The Install button asks twice. |

"Boot UnoDOS by default" prepends the boot entry to `BootOrder`; unticked, it
appends (pick UnoDOS from the firmware's boot menu instead).

The boot USB itself is never listed (excluded by device-path match), and
non-512-byte-sector or too-small disks are listed but refused.

## Surface Laptop Go 1 (the hardware test case)

1. Flash the current stick from the share (`\\behemoth\unreplicated\unodos\pc64`,
   `UnoDosFlasher.exe`), Secure Boot off (as for Writer's Unlock).
2. Boot the stick (Vol-down + power → boot from USB).
3. Open **Install**. Expected list: the Windows ESP as
   `Volume "..." 260 MB ESP (has \EFI) [keeps data]`, Windows' main NTFS
   partition does NOT appear (not FAT), and the internal SSD as
   `Disk ... fixed [ERASES ALL]` — **do not** pick the disk unless you mean to
   wipe Windows.
4. Select the ESP volume, press **I**. Non-destructive: Windows stays.
5. Remove the stick, reboot → UnoDOS should come up (it is first in BootOrder).
   Booting Windows again: firmware boot menu, or from UnoDOS just re-run with
   "Boot UnoDOS by default" unticked... or use the Surface UEFI boot order UI.

To undo on any machine: delete `\EFI\UNODOS\` from the ESP and remove the
`UnoDOS` entry from the firmware boot menu (or `bcdedit /enum firmware` +
`bcdedit /delete {id}` from Windows).

## Headless verification

`python3 tools/install_test.py [disk|esp]` (WSL/Linux; `UNO_KVM=1` to use KVM)
boots the USB image + a scratch disk in QEMU/OVMF, drives the Install app by
keyboard, then reboots from the scratch disk alone and screenshots the desktop.
The `esp` phase additionally builds a foreign-content ESP first and verifies
offline (mtools) that the foreign files survived. Both phases pass on WSL
(TCG) and devbuntu (KVM) as of 2026-07-19.
