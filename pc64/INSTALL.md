# Installing UnoDOS/pc64 to a local disk

UnoDOS boots from the USB stick the flasher writes, and can install itself to a
machine's internal disk from the running system: desktop icon **Install** (or
Start → Install).

Keyboard drive (works even where the pointer doesn't): **Up/Down** pick a
target, **I** installs, **R** rescans.

## The two target kinds

| Listing | What happens |
|---|---|
| `Volume "..."  ESP (has \EFI)  [keeps data]` | **Non-destructive.** Copies the running system into `\EFI\UNODOS\` on that FAT/ESP volume (BOOTX64.EFI + fonts + docs + the `APPS\*.UNO` app modules), adds a `UnoDOS` UEFI boot entry (NVRAM `Boot####` + `BootOrder`), and — only if the volume has no `\EFI\BOOT\BOOTX64.EFI` already — a removable-media fallback copy. Nothing is deleted; an existing Windows install keeps booting. |
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
keyboard, then reboots from the scratch disk alone. Needs
`build/unodos-uefi.img` (`python3 tools/mkuefi.py 256`); it now says so plainly
instead of failing on a QMP socket that never opened.

**Both phases assert** (they did not always - see below):
- **offline (mtools):** whole-disk => `\EFI\BOOT\BOOTX64.EFI` + `APPS\*.UNO` on
  the target; ESP => the same under `\EFI\UNODOS\`, plus the foreign marker intact.
  The partition extent is read from the target's GPT, since after a whole-disk
  clone the ESP is the *source* stick's size.
- **from-disk boot:** the frame after booting the disk alone must actually be a
  desktop. Measured as the fraction of non-black pixels (a booted desktop covers
  the frame at ~100%; the UEFI shell is a black screen with a few lines of text at
  ~1%), which survives a theme change in a way brightness alone would not.

The scratch disk is sized from the USB image + 64 MiB, because `install_disk()`
refuses a target smaller than the source's used extent + 33 sectors.

> **Why the asserts.** Until 2026-07-28 the `disk` phase returned success
> unconditionally and checked nothing, so it stayed green through three stacked
> breakages: the scratch disk was a hardcoded 256 MiB that the app listed as
> `[too small]` and refused; the Start-menu index was hardcoded, so after the
> Network app was dropped the test opened **Music** instead of Install; and the
> whole-disk confirm gate had grown a type-`ERASE` box that two bare `i` presses
> no longer satisfy. The committed screenshots show all of it — nobody was
> looking, because the exit code said pass. Menu positions now come from the
> shell's own `kAppNames[]`, and the confirm sequence matches
> `tools/install_confirm_test.py`, which is the spec for that gate.

## Installing from a detached system (2026-07-29)

Everything above describes the firmware path: Simple File System for the
file-level install, Block IO for the whole-disk clone, runtime `SetVariable`
for the `Boot####` entry. A machine that has detached (ExitBootServices, see
`pc64/USB.md` and `docs/DETACH-COMPLETION-PLAN.md`) has none of those, and the
installer used to simply refuse.

It no longer does. When `uno_pc64_detached()`, `uno_inst_scan()` /
`uno_inst_install()` enumerate and write through the native stack instead -
`blkdev` for disks, `unostorage_prepare_esp()` to author the GPT + ESP +
FAT32, `uno_fs_copytree()` to clone the tree, `uno_fat_sync()` to persist. Same
two target kinds, same UI, same safety rules (the boot disk is never a target;
whole-disk installs still need the typed `ERASE` confirmation).

**The one difference: no NVRAM boot entry.** Runtime `SetVariable` is declined
after ExitBootServices (`uno_pc64_set_bootnext`), which the URC `install`
verb already lives with. So a detached install boots by the removable-media
fallback path `\EFI\BOOT\BOOTX64.EFI`:

- **Whole-disk install**, fine. The disk has nothing else on it, and firmware
  falls back to the removable path.
- **ESP install alongside another OS**, the other OS's boot entry keeps
  winning. Pick UnoDOS from the firmware boot menu, or install while attached
  (a stick boot stays attached by default today).
