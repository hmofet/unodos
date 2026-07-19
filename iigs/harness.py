#!/usr/bin/env python3
"""UnoDOS/Apple IIGS ROM-free harness.

The house pattern (apple2/harness.py) on a 65C816: a Python firmware shim
plays the IIGS ROM so the kernel boots headlessly with no ROM image and no
emulator install. It drives cpu65816.CPU65816:

  * autoloads block 0 of the .po image to $00:0800 and enters at $0801 in
    6502 emulation mode with X = slot<<4 (slot 5, the 3.5" SmartPort),
  * serves the slot firmware's ProDOS block driver via a WDM-trap stub
    (boot.s reads kernel blocks 1..K through it; we also support WRITE so
    M2 can persist with --writeback),
  * intercepts the soft-switch page ($C0xx) - keyboard latch, NEWVIDEO,
    seeded mouse/vbl - and leaves the firmware pages otherwise as RAM,
  * renders the Super Hi-Res framebuffer (bank $E1) to a PNG: 200 rows x
    160 bytes, 4bpp, high-nibble-left, palette line 0 at $E1:9E00.

M0 usage:
    python iigs/harness.py build/unodos_iigs.po build/m0.png
M1 will add a wait/shot/key/mouse script runner over the same Harness.
"""
import sys
import struct
import zlib

from cpu65816 import CPU65816, C

BLOCK = 512
SLOT = 5                       # boot device slot (3.5" SmartPort)
FW_PAGE = 0xC000 + (SLOT << 8)  # $C500
DRV_OFF = 0x0A                 # driver entry offset within the firmware page
DRV_ENTRY = FW_PAGE + DRV_OFF  # $C50A

SHR_PIX = 0xE1_2000
SHR_SCB = 0xE1_9D00
SHR_PAL = 0xE1_9E00
ROWBYTES = 160
ROWS = 200
COLS = 320


class Harness:
    def __init__(self, image_path, writeback=None):
        self.image = bytearray(open(image_path, "rb").read())
        self.writeback = writeback
        self.newvideo = 0x00
        self.kbd = 0x00            # $C000 keyboard data (bit7 = key ready)
        # ADB mouse model: a FIFO of signed (dx,dy) delta bytes + a button bit.
        self.mouse_fifo = bytearray()
        self.mouse_btn = 0
        self.cmd_x = 160          # harness-tracked commanded cursor position
        self.cmd_y = 100
        self.vbl = 0              # $C019 toggles each read
        self.frame = 0            # WDM #$02 frame count
        self._frame_flag = False
        # Ensoniq DOC model (control path only; no audio synthesis)
        self.doc_regs = bytearray(256)
        self.doc_ram = bytearray(65536)
        self.doc_addr = 0
        self.doc_ctl = 0
        self.doc_writes = []      # log of (register, value) writes
        self.cpu = CPU65816(read_hook=self._read, write_hook=self._write,
                            wdm_hook=self._wdm)
        self._install_firmware()

    # ---------------------------------------------------------- firmware
    def _install_firmware(self):
        cpu = self.cpu
        # block 0 -> $00:0800
        cpu.mem[0x0800:0x0800 + BLOCK] = self.image[0:BLOCK]
        assert cpu.mem[0x0800] == 0x01, "boot block missing $01 signature"
        # slot firmware: $CnFF = driver offset; stub = WDM #$01 ; RTS
        cpu.mem[FW_PAGE + 0xFF] = DRV_OFF
        cpu.mem[DRV_ENTRY + 0] = 0x42     # WDM
        cpu.mem[DRV_ENTRY + 1] = 0x01     # signature -> block-driver trap
        cpu.mem[DRV_ENTRY + 2] = 0x60     # RTS
        # enter the boot stage exactly as ProDOS firmware does
        cpu.reset(pc=0x0801, pbr=0)
        cpu.e = 1
        cpu.x = SLOT << 4                 # X = $50
        cpu.y = 0
        cpu.sp = 0x01FF

    # --------------------------------------------------- memory I/O hooks
    @staticmethod
    def _is_io(addr):
        bank = addr >> 16
        off = addr & 0xFFFF
        return bank in (0x00, 0xE0) and 0xC000 <= off <= 0xC0FF

    def _read(self, addr):
        if not self._is_io(addr):
            return None                   # plain RAM (incl. the $Cn00 stub page)
        off = addr & 0xFFFF
        if off == 0xC000:                 # keyboard data
            return self.kbd
        if off == 0xC010:                 # keyboard strobe clear
            self.kbd &= 0x7F
            return 0x00
        if off == 0xC019:                 # VBL status (bit7) - toggle per read
            self.vbl ^= 0x80
            return self.vbl
        if off == 0xC024:                 # ADB mouse delta FIFO (signed byte)
            if self.mouse_fifo:
                return self.mouse_fifo.pop(0)
            return 0x00
        if off == 0xC027:                 # KMSTATUS: bit7=movement, bit0=button
            return (0x80 if self.mouse_fifo else 0x00) | (self.mouse_btn & 1)
        if off == 0xC029:                 # NEWVIDEO read-back
            return self.newvideo
        return 0x00                       # other soft switches read 0

    def _write(self, addr, val):
        if not self._is_io(addr):
            return False                  # let the core write RAM
        off = addr & 0xFFFF
        if off == 0xC029:
            self.newvideo = val
        elif off == 0xC03E:                # DOC address low
            self.doc_addr = (self.doc_addr & 0xFF00) | val
        elif off == 0xC03F:                # DOC address high
            self.doc_addr = (self.doc_addr & 0x00FF) | (val << 8)
        elif off == 0xC03C:                # DOC control (access mode)
            self.doc_ctl = val
        elif off == 0xC03D:                # DOC data
            if self.doc_ctl & 0x20:        # RAM access
                self.doc_ram[self.doc_addr & 0xFFFF] = val
            else:                          # register access
                self.doc_regs[self.doc_addr & 0xFF] = val
                self.doc_writes.append((self.doc_addr & 0xFF, val))
            if self.doc_ctl & 0x40:        # autoincrement
                self.doc_addr = (self.doc_addr + 1) & 0xFFFF
        # all soft-switch writes are swallowed (state we don't model is a no-op)
        return True

    # ------------------------------------------------------- ProDOS driver
    def _wdm(self, cpu, imm):
        if imm == 0x02:                    # main-loop frame marker
            self.frame += 1
            self._frame_flag = True
            return
        if imm != 0x01:
            return
        m = cpu.mem
        cmd = m[0x42]
        unit = m[0x43]
        buf = m[0x44] | (m[0x45] << 8)
        blk = m[0x46] | (m[0x47] << 8)
        drive2 = unit & 0x80
        ok = True
        if cmd == 1:                       # READ block -> buffer (bank 0)
            src = blk * BLOCK
            if src + BLOCK <= len(self.image):
                m[buf:buf + BLOCK] = self.image[src:src + BLOCK]
            else:
                ok = False
        elif cmd == 2:                     # WRITE buffer -> block (M2)
            dst = blk * BLOCK
            if dst + BLOCK <= len(self.image):
                self.image[dst:dst + BLOCK] = m[buf:buf + BLOCK]
            else:
                ok = False
        elif cmd == 0:                     # STATUS - report ready
            ok = True
        else:
            ok = False
        # ProDOS driver result: carry clear + A=0 on success, else carry set.
        if ok:
            cpu.p &= ~C
            cpu.a = (cpu.a & 0xFF00)
        else:
            cpu.p |= C
            cpu.a = (cpu.a & 0xFF00) | 0x27   # I/O error

    # --------------------------------------------------------------- run
    def run(self, max_steps=4_000_000):
        """Run to a STP halt (M0-style splash)."""
        n = self.cpu.run(max_steps=max_steps)
        if self.writeback:
            open(self.writeback, "wb").write(self.image)
        return n

    def boot(self, max_steps=4_000_000):
        """Run until the first main-loop frame marker (desktop is up)."""
        self._run_to_frame(max_steps)

    def _run_to_frame(self, max_steps=4_000_000):
        self._frame_flag = False
        n = 0
        cpu = self.cpu
        while not self._frame_flag and not cpu.halted and n < max_steps:
            cpu.step()
            n += 1
        return n

    def frames(self, count=1):
        """Advance `count` full main-loop iterations."""
        for _ in range(count):
            self._run_to_frame()      # consume the pending marker
            self._run_to_frame()      # run one full body to the next marker

    # ------------------------------------------------------- input scripting
    def key(self, ascii_code):
        """Inject one keypress and let the kernel consume + process it."""
        self.kbd = (ascii_code & 0x7F) | 0x80
        self.frames(1)

    def _push_delta(self, dx, dy):
        for v in (dx, dy):
            self.mouse_fifo.append(v & 0xFF)

    def move_to(self, x, y):
        """Walk the cursor to (x,y) via signed-byte deltas, then settle."""
        x = max(0, min(319, x))
        y = max(0, min(199, y))
        while self.cmd_x != x or self.cmd_y != y:
            dx = max(-120, min(120, x - self.cmd_x))
            dy = max(-120, min(120, y - self.cmd_y))
            self.cmd_x += dx
            self.cmd_y += dy
            self._push_delta(dx, dy)
        self.frames(1)

    def click(self, x=None, y=None):
        if x is not None:
            self.move_to(x, y)
        self.mouse_btn = 1
        self.frames(1)
        self.mouse_btn = 0
        self.frames(1)

    def drag(self, x0, y0, x1, y1, step=8):
        """Press at (x0,y0), step-move to (x1,y1) with the button held (so the
        window follows one cell-step per frame), then release. Exercises the
        drag damage-rect."""
        self.move_to(x0, y0)
        self.mouse_btn = 1
        self.frames(1)
        while self.cmd_x != x1 or self.cmd_y != y1:
            dx = max(-step, min(step, x1 - self.cmd_x))
            dy = max(-step, min(step, y1 - self.cmd_y))
            self.cmd_x += dx
            self.cmd_y += dy
            self._push_delta(dx, dy)
            self.frames(1)
        self.frames(2)
        self.mouse_btn = 0
        self.frames(1)

    def run_script(self, path):
        """Run a wait/shot/key/move/click text script (parity with apple2).

        Relative `shot` paths resolve against the current working directory.
        """
        for raw in open(path):
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            parts = line.split()
            op = parts[0]
            if op == "boot":
                self.boot()
            elif op in ("wait", "frames"):
                self.frames(int(parts[1]))
            elif op == "key":
                self.key(int(parts[1], 0))
            elif op == "move":
                self.move_to(int(parts[1]), int(parts[2]))
            elif op == "click":
                if len(parts) >= 3:
                    self.click(int(parts[1]), int(parts[2]))
                else:
                    self.click()
            elif op == "drag":
                self.drag(int(parts[1]), int(parts[2]),
                          int(parts[3]), int(parts[4]))
            elif op == "shot":
                self.render_png(parts[1])
            else:
                raise ValueError(f"unknown script op: {op}")
        if self.writeback:
            open(self.writeback, "wb").write(self.image)

    # ------------------------------------------------------- SHR -> PNG
    def render_png(self, out_path, scale=2):
        m = self.cpu.mem
        if not (self.newvideo & 0x80):
            sys.stderr.write("warning: SHR (NEWVIDEO bit7) was never enabled\n")
        # palette line 0: 16 colours, $0RGB little-endian
        pal = []
        for i in range(16):
            w = m[SHR_PAL + i * 2] | (m[SHR_PAL + i * 2 + 1] << 8)
            r = (w >> 8) & 0x0F
            g = (w >> 4) & 0x0F
            b = w & 0x0F
            pal.append((r * 17, g * 17, b * 17))
        # 320x200 -> RGB rows (palette line 0 for every scanline at M0)
        rows = []
        for y in range(ROWS):
            base = SHR_PIX + y * ROWBYTES
            row = bytearray()
            for bx in range(ROWBYTES):
                byte = m[base + bx]
                for nib in ((byte >> 4) & 0x0F, byte & 0x0F):
                    row += bytes(pal[nib])
            rows.append(bytes(row))
        _write_png(out_path, rows, COLS, ROWS, scale)
        print(f"wrote {out_path} ({COLS*scale}x{ROWS*scale}, "
              f"{self.cpu.cycles} instrs)")


def _write_png(path, rgb_rows, w, h, scale=1):
    """Minimal truecolor PNG writer (stdlib zlib only, no PIL)."""
    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    sw, sh = w * scale, h * scale
    raw = bytearray()
    for y in range(h):
        src = rgb_rows[y]
        line = bytearray()
        for x in range(w):
            line += src[x * 3:x * 3 + 3] * scale
        for _ in range(scale):
            raw.append(0)               # filter type 0
            raw += line
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", sw, sh, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    open(path, "wb").write(png)


def main():
    if len(sys.argv) < 3:
        print("usage: harness.py <image.po> <out.png|script.script> "
              "[--writeback out.po] [--frames N]")
        return 1
    image, out = sys.argv[1], sys.argv[2]
    wb = None
    if "--writeback" in sys.argv:
        wb = sys.argv[sys.argv.index("--writeback") + 1]
    h = Harness(image, writeback=wb)

    if out.endswith(".script"):           # M1+ script: boot, drive, shot(s)
        h.boot()
        h.run_script(out)
        return 0
    if "--frames" in sys.argv:            # M1+ boot-and-shot
        n = int(sys.argv[sys.argv.index("--frames") + 1])
        h.boot()
        h.frames(n)
        h.render_png(out)
        return 0
    # M0 default: run the splash to its STP halt
    h.run()
    if not h.cpu.halted:
        sys.stderr.write("warning: CPU did not halt (no STP reached)\n")
    h.render_png(out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
