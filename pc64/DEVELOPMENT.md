# UnoDOS / pc64 — development environment setup

How to recreate the full dev environment for the pc64 (x86-64 / UEFI) world on a
fresh machine, on **Linux** or **Windows**, and the build → test → deploy loop.

pc64 is a **UEFI application** (a PE32+ image) cross-compiled with mingw-w64 and
booted either in **QEMU + OVMF** or on real hardware from a FAT32 USB stick.
There is no EDK2 and no gnu-efi — the mingw target's native output *is* a valid
UEFI binary.

---

## 0. What you need (both OSes)

| Tool | Why | Package |
|------|-----|---------|
| `x86_64-w64-mingw32-gcc` | cross-compile the UEFI PE32+ image | `gcc-mingw-w64-x86-64` |
| `qemu-system-x86_64` | run/test the image | `qemu-system-x86` |
| OVMF firmware (`OVMF.fd`) | UEFI firmware for QEMU | `ovmf` |
| `python3` | build generators + the QEMU harness | `python3` |
| Python **Pillow** (`PIL`) | convert QEMU screenshots (PPM→PNG) | `python3-pil` |
| `git` | clone the repo | `git` |

Everything is done inside a Linux userland. On Windows that userland is **WSL2**
(the build toolchain is Linux-only); Windows is used only to *write the USB
stick* and to run the QEMU display if you prefer.

---

## 1. Get the code

The build reaches into sibling directories (`../unoui`, `../uno3d`,
`../unosound`) and runs generator scripts that read `../amiga` and `kernel/`, so
you must clone the **whole** monorepo, not just `pc64/`.

```sh
git clone https://github.com/hmofet/unodos.git
cd unodos
git checkout pc64-net-3d      # the active development branch
```

Everything below runs from the `pc64/` subdirectory unless noted.

---

## 2. Linux setup (native, or inside WSL — identical)

```sh
sudo apt update
sudo apt install -y gcc-mingw-w64-x86-64 qemu-system-x86 ovmf \
                    python3 python3-pil git
```

Confirm the toolchain:

```sh
x86_64-w64-mingw32-gcc --version      # the cross compiler
qemu-system-x86_64 --version
ls /usr/share/ovmf/OVMF.fd            # firmware (path used by the harness)
python3 -c 'import PIL; print("Pillow OK")'
```

> If `OVMF.fd` isn't at `/usr/share/ovmf/OVMF.fd`, find it with
> `dpkg -L ovmf | grep -i OVMF` — some distros split it into
> `OVMF_CODE.fd` + `OVMF_VARS.fd`. Point QEMU's `-bios` (or `-drive
> if=pflash`) at whatever your distro ships.

That's the entire Linux setup. Go to **§4 Build**.

---

## 3. Windows setup (via WSL2)

The compiler is Linux-only, so on Windows you build inside **WSL2** and write the
USB stick from **Windows PowerShell** (WSL can't easily do raw USB).

### 3a. Install WSL2 + Ubuntu (once)

In an **admin** PowerShell:

```powershell
wsl --install -d Ubuntu-24.04
```

Reboot if prompted, launch "Ubuntu" once to create your Linux user, then follow
**§2** *inside* the Ubuntu shell (`sudo apt install ...`). Clone the repo inside
the WSL filesystem (e.g. `~/unodos`) for speed, or under `/mnt/c/...` if you
want it visible to Windows tools — the build works either way, but native WSL
paths build noticeably faster.

### 3b. Building & testing

All build/QEMU commands run in the **WSL shell**, exactly as in §2/§4/§5.

### 3c. Writing the USB stick from Windows

The ESP is a small file tree, so "writing the stick" just means putting
`build/esp/EFI/BOOT/BOOTX64.EFI` (plus the sample/font files) onto a **FAT32**
USB stick under `\EFI\BOOT\`.

Easiest — File Explorer:
1. Format the USB stick as **FAT32**.
2. Copy the contents of `build/esp/` onto the stick so it ends up with
   `EFI\BOOT\BOOTX64.EFI` at the root.

Scripted (PowerShell, run **as Administrator** — it mounts the ESP partition and
copies the fresh `BOOTX64.EFI`): adapt `write-stick.ps1` from the scratchpad, or
just:

```powershell
# with the stick mounted as, say, E:
New-Item -ItemType Directory -Force E:\EFI\BOOT | Out-Null
Copy-Item \\wsl$\Ubuntu-24.04\home\<you>\unodos\pc64\build\esp\EFI\BOOT\BOOTX64.EFI E:\EFI\BOOT\ -Force
# (optional) the browser's demo assets + fonts:
Copy-Item \\wsl$\Ubuntu-24.04\home\<you>\unodos\pc64\build\esp\*.TTF E:\ -Force
Copy-Item \\wsl$\Ubuntu-24.04\home\<you>\unodos\pc64\build\esp\*.MD  E:\ -Force
Copy-Item \\wsl$\Ubuntu-24.04\home\<you>\unodos\pc64\build\esp\*.HTML E:\ -Force
```

> The `\\wsl$\<distro>\...` UNC path lets Windows read files inside WSL. If you
> cloned under `/mnt/c/...` instead, use the normal `C:\...` path.

---

## 4. Build

```sh
cd pc64
./build.sh                 # -> build/BOOTX64.EFI  and the ESP tree build/esp/
```

What it does: exports the 8×8 bitmap font to a C array, cross-compiles the
platform + unoui shell + drivers (net/TLS/3D) + the browser/JS/fonts, links the
PE32+ image, and stages `build/esp/EFI/BOOT/BOOTX64.EFI` alongside the browser's
demo files (`HELLO.MD`, `PAGE.HTML`) and the three fonts (`SANS/MONO/UBUNTU.TTF`).

Other targets:

```sh
./build.sh run             # build, then boot headless in QEMU+OVMF (VNC :0)
./build.sh legacy          # the older unodos.c core + 14 apps (reference)
```

Override the toolchain if your binaries are named differently:

```sh
CC=x86_64-w64-mingw32-gcc PY=python3 ./build.sh
```

---

## 5. Test in QEMU

### Interactive (see a window)

```sh
qemu-system-x86_64 -bios /usr/share/ovmf/OVMF.fd \
  -drive format=raw,file=fat:rw:build/esp \
  -m 256 -vga std -rtc base=localtime
```

`fat:rw:build/esp` exposes the ESP tree as a virtual FAT drive — no disk image
to build. Add networking for the browser / Network app:

```sh
  -device e1000,netdev=n0 -netdev user,id=n0 -cpu max
```

> **Note:** OVMF ships **no pointer driver**, so QEMU has no mouse — the desktop
> is **keyboard-only** in emulation (Ctrl-Esc = Start menu, arrows + Enter,
> Tab/Ctrl-Tab, F2). Anything mouse-driven (Paint drawing, window-resize grip,
> the calendar day-click) can only be exercised on real hardware.

### Headless + screenshots (the harness)

`harness.py` drives QEMU over QMP (`send-key` + `screendump`) and writes PNGs to
`shots/`:

```sh
python3 harness.py boot        # boots, screenshots the desktop
python3 nettest.py             # net + TLS self-test vs QEMU SLIRP (legacy build)
```

To script your own boot + keypresses + screenshot, connect to QEMU's QMP socket
(`-qmp tcp:127.0.0.1:4444,server,nowait`), send `qmp_capabilities`, then
`send-key` / `screendump` commands. A fresh-varstore OVMF boot takes **~40–75 s**
before the desktop appears — wait that long before the first screenshot.

**Testing the browser's network fetch without a host web server:** QEMU SLIRP's
`guestfwd` runs a per-connection host command (no listening socket needed):

```sh
  -netdev user,id=n0,guestfwd=tcp:10.0.2.100:80-cmd:"python3 /path/http_oneshot.py"
```

then browse to `http://10.0.2.100/` in the guest. (`http_oneshot.py` just reads
the request and writes a fixed HTTP/1.0 response.)

---

## 6. Deploy to real hardware

1. `./build.sh`
2. Format a USB stick **FAT32**.
3. Copy `build/esp/*` onto it (result: `EFI/BOOT/BOOTX64.EFI` at the root).
   - Linux: `mount` the stick, `cp -r build/esp/* /media/<you>/USB/`.
   - Windows: see **§3c**.
4. Boot the target PC from USB with **Secure Boot disabled**.

Validated on a Lenovo ThinkPad X1 Carbon Gen 8; any 64-bit UEFI PC is in scope.
See [`METAL-CHECKLIST.md`](METAL-CHECKLIST.md) for what to verify on hardware
(the things QEMU can't test — mouse, real RTC, PC-speaker audio, power control).

---

## 7. The inner loop

```sh
# edit code, then:
./build.sh && python3 harness.py boot   # rebuild + screenshot
# or interactive:
./build.sh && qemu-system-x86_64 -bios /usr/share/ovmf/OVMF.fd \
  -drive format=raw,file=fat:rw:build/esp -m 256 -vga std -rtc base=localtime
```

Commit as you go; the pc64 world lives on `master` of
`github.com/hmofet/unodos` (the pre-pc64 snapshot is preserved on the `classic` branch).

---

## 8. Gotchas

- **Clone the whole repo.** `pc64/build.sh` reads `../unoui`, `../uno3d`,
  `../unosound`, `../amiga`, and `kernel/`. A bare `pc64/` checkout won't build.
- **`long` is 32-bit** under mingw (LLP64). Use `unsigned long long` /
  `uintptr_t` for anything that must be 64-bit (DMA addresses, pointers cast to
  ints) — a truncated `long` was the original e1000 DMA bug.
- **Freestanding.** No host libc; the build is `-ffreestanding -nostdinc`.
  Use the port's own `include/*.h`, `pc64_libc.c`, `pc64_math.c`.
- **OVMF has no mouse.** Mouse-driven features are metal-only to verify.
- **Kill stale QEMU** between headless runs (`pkill -9 qemu-system-x86`); a
  leftover instance holding the QMP port makes the next run look like a hang.
- **QMP sockets** must live on a native FS, not a `/mnt/c` drvfs path — put any
  unix-socket under `/tmp` (or just use the `tcp:` QMP form).
