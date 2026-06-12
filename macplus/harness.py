#!/usr/bin/env python3
"""ROM-free Mac Plus test harness for the UnoDOS/MacPlus port.

Plays the part of the Mac Plus ROM around a Unicorn (QEMU) 68000 core:
  - Start Manager: sets low-mem globals (ScrnBase/MemTop), loads the boot
    blocks from the disk image and jumps to their entry point.
  - A-line traps: _Read ($A002) raw-sector reads against the disk image
    (the .Sony driver), _SysError ($A9C9) aborts the run.
  - VIA1: CA1 vblank tick, M0110A keyboard over the shift register
    (Instant poll protocol incl. the $79 keypad prefix), PB3 button,
    PB4/PB5 quadrature phase bits.
  - SCC: DCD ext/status interrupts for mouse X (ch A) / Y (ch B).
  - Interrupts: injected at slice boundaries as 6-byte 68000 frames;
    RTE surfaces as QEMU EXCP 0x100 and is popped here, symmetrically.
  - Framebuffer: 512x342x1 at ScrnBase, dumped as PNG.

Script on stdin (one command per line, # comments):
  wait N            run N vblank ticks
  shot NAME         screenshot to <artdir>/NAME.png
  moveto X Y        quadrature-step the mouse until the kernel agrees
  btn down|up       mouse button
  click X Y         moveto + press + release (with settle ticks)
  dblclick X Y      two clicks inside the double-click window
  key K             press+release (a-z, 0-9, enter, esc, space, tab,
                    backspace, up, down, left, right)
  quit              finish

Usage: harness.py <disk.dsk> <artdir> [--trace] < script
"""
import os, struct, sys, zlib
from unicorn import *
from unicorn.m68k_const import *

RAM_SIZE   = 0x100000               # 1MB Mac Plus
SCRN       = RAM_SIZE - 0x5900      # $0FA700, the real 1MB screen base
SCRW, SCRH, ROWB = 512, 342, 64
BOOT_AT    = 0x60000                # where "the ROM" puts the boot blocks
KERNBASE   = 0x20000

SLICE      = 2500                   # instructions per emulation slice
SLICES_PER_TICK = 6                 # -> one CA1 "vblank" per ~15k insns

VIA_BASE   = 0xEFE000
SCCR_PAGE  = 0x9FF000
SCCW_PAGE  = 0xBFF000

# scan codes (M0110A, pre-wire: raw byte = (scan<<1)|1, bit7 = release)
SCANS = {
    'a':0x00,'s':0x01,'d':0x02,'f':0x03,'h':0x04,'g':0x05,'z':0x06,'x':0x07,
    'c':0x08,'v':0x09,'b':0x0B,'q':0x0C,'w':0x0D,'e':0x0E,'r':0x0F,'t':0x10,
    'y':0x11,'u':0x20,'i':0x22,'o':0x1F,'p':0x23,'l':0x25,'j':0x26,'k':0x28,
    'n':0x2D,'m':0x2E,
    '1':0x12,'2':0x13,'3':0x14,'4':0x15,'5':0x17,'6':0x16,'7':0x1A,'8':0x1C,
    '9':0x19,'0':0x1D,'-':0x1B,'=':0x18,'[':0x21,']':0x1E,';':0x29,
    ',':0x2B,'.':0x2F,'/':0x2C,
    'enter':0x24,'space':0x31,'esc':0x32,'tab':0x30,'backspace':0x33,
}
ARROWS = {'up':0x0D,'down':0x08,'left':0x06,'right':0x02,    # keypad page
          'clr':0x07}                                        # Clr = F1/save ($50)


def png_gray(path, w, h, getpix):
    rows = b""
    for y in range(h):
        rows += b"\x00" + bytes(getpix(y))
    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c))
    hdr = struct.pack(">IIBBBBB", w, h, 8, 0, 0, 0, 0)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", hdr) +
                chunk(b"IDAT", zlib.compress(rows, 6)) + chunk(b"IEND", b""))


class Mac:
    def __init__(self, disk_path, artdir, trace=False):
        self.disk = bytearray(open(disk_path, "rb").read())
        self.artdir = artdir
        self.trace = trace
        os.makedirs(artdir, exist_ok=True)

        mu = self.mu = Uc(UC_ARCH_M68K, UC_MODE_BIG_ENDIAN)
        mu.ctl_set_cpu_model(UC_CPU_M68K_M68000)
        mu.mem_map(0, RAM_SIZE)
        mu.mem_map(SCCR_PAGE, 0x1000)
        mu.mem_map(SCCW_PAGE, 0x1000)
        mu.mem_map(VIA_BASE, 0x2000)

        # --- device state
        self.via_ifr = 0
        self.via_ier = 0
        self.via_acr = 0
        self.via_sr = 0x7B
        self.btn = False            # PB3 active low
        self.x2 = 0                 # PB4
        self.y2 = 0                 # PB5
        self.dcd_x = 0              # SCC ch A DCD
        self.dcd_y = 0              # SCC ch B DCD
        self.scc_ptr = {0: 0, 1: 0} # 0 = A, 1 = B (write reg pointer)
        self.kb_queue = []          # response bytes (already wire-encoded)
        self.kb_events = []         # (delay_slices, kind, ...) SR events
        self.kb_attn = False        # data line held low (mode-6 zero)
        self.kb_cmd = None          # command awaiting end-of-command
        self.kb_stash = None        # $79-prefix second byte (Instant)
        self.pend1 = False          # level-1 (VIA) interrupt pending
        self.pend2 = False          # level-2 (SCC) interrupt pending
        self.ctx_stack = []         # saved contexts of injected interrupts
        self.rte_flag = False       # an injected ISR just finished
        self.fail = None
        self.slices = 0

        mu.hook_add(UC_HOOK_INTR, self._intr)
        mu.hook_add(UC_HOOK_MEM_READ, self._via_r, begin=VIA_BASE,
                    end=VIA_BASE + 0x2000 - 1)
        mu.hook_add(UC_HOOK_MEM_WRITE, self._via_w, begin=VIA_BASE,
                    end=VIA_BASE + 0x2000 - 1)
        mu.hook_add(UC_HOOK_MEM_READ, self._scc_r, begin=SCCR_PAGE,
                    end=SCCR_PAGE + 0xFFF)
        mu.hook_add(UC_HOOK_MEM_WRITE, self._scc_w, begin=SCCW_PAGE,
                    end=SCCW_PAGE + 0xFFF)

        # --- Start Manager
        mu.mem_write(0x824, struct.pack(">I", SCRN))      # ScrnBase
        mu.mem_write(0x108, struct.pack(">I", RAM_SIZE))  # MemTop
        mu.mem_write(0x2AE, struct.pack(">I", 0x60400))   # ROMBase (fake)
        mu.mem_write(0x60408, struct.pack(">H", 0x0075))  # ROM version: Plus
        boot = bytes(self.disk[:1024])
        assert boot[0:2] == b"\x4c\x4b", "no LK signature on disk"
        mu.mem_write(BOOT_AT, boot)
        mu.reg_write(UC_M68K_REG_A7, BOOT_AT - 0x1000)
        mu.reg_write(UC_M68K_REG_SR, 0x2700)
        self.pc = BOOT_AT + 2       # the bbEntry bra.w
        self.vars = None            # discovered from the UDM1 header

    # ------------------------------------------------------------ devices
    def _via_reg(self, addr):
        return (addr - (VIA_BASE | 0x1FE)) >> 9

    def _via_r(self, uc, access, addr, size, value, user):
        r = self._via_reg(addr)
        v = 0
        if r == 0:                  # ORB
            v = (0 if self.btn else 8) | (self.x2 << 4) | (self.y2 << 5)
        elif r == 10:               # SR
            v = self.via_sr
            self.via_ifr &= ~0x04
        elif r == 11:
            v = self.via_acr
        elif r == 13:
            v = self.via_ifr | (0x80 if (self.via_ifr & self.via_ier & 0x7F) else 0)
        elif r == 14:
            v = self.via_ier | 0x80
        uc.mem_write(addr, bytes([v & 0xFF]))

    def _via_w(self, uc, access, addr, size, value, user):
        r = self._via_reg(addr)
        v = value & 0xFF
        if r == 10:                 # SR write (faithful M0110 contract,
            self.via_ifr &= ~0x04   # mirrors Mini vMac KBRDEMDV/VIAEMDEV)
            mode = (self.via_acr >> 2) & 7
            if mode == 6 and v == 0:
                self.kb_attn = True             # data line pulled low
            elif mode == 7 and self.kb_attn:
                self.kb_attn = False
                self.kb_cmd = v                 # keyboard clocks it in
                self.kb_events.append([2, "sent"])
        elif r == 11:
            old = self.via_acr
            self.via_acr = v
            # out->in switch floats the data line high: the keyboard
            # treats that as end-of-command and shifts the response in
            if (old & 0x1C) != 0x0C and (v & 0x1C) == 0x0C \
                    and self.kb_cmd is not None:
                self.kb_events.append([2, "resp", self.kb_cmd])
                self.kb_cmd = None
        elif r == 13:
            self.via_ifr &= ~(v & 0x7F)
        elif r == 14:
            if v & 0x80:
                self.via_ier |= v & 0x7F
            else:
                self.via_ier &= ~(v & 0x7F)

    def _scc_ch(self, addr):
        # +0/+4 = channel B ctl/data, +2/+6 = channel A (read base $9FFFF8)
        return 0 if (addr & 2) else 1   # 0 = A, 1 = B

    def _scc_r(self, uc, access, addr, size, value, user):
        ch = self._scc_ch(addr)
        ptr = self.scc_ptr[ch]
        self.scc_ptr[ch] = 0
        v = 0
        if ptr == 0:                # RR0: DCD in bit 3, TX empty bit 2
            dcd = self.dcd_x if ch == 0 else self.dcd_y
            v = (dcd << 3) | 0x04
        uc.mem_write(addr, bytes([v]))

    def _scc_w(self, uc, access, addr, size, value, user):
        ch = self._scc_ch(addr)
        v = value & 0xFF
        if self.scc_ptr[ch] == 0:
            reg = v & 7
            if v & 8:
                reg += 8
            cmd = (v >> 3) & 7
            if cmd in (2, 7):       # reset ext/status, reset highest IUS
                return
            if reg:
                self.scc_ptr[ch] = reg
        else:
            self.scc_ptr[ch] = 0    # value write: nothing we model

    # ------------------------------------------------------------ traps
    def _intr(self, uc, intno, user):
        pc = uc.reg_read(UC_M68K_REG_PC)
        if intno == 0x100:          # RTE: end of an injected ISR
            # Restore the saved context in-hook, then force the PC
            # redirect with an explicit reg_write (context_restore alone
            # does not retarget the running translation block; a PC write
            # does). Keeping the slice alive matters: stopping the slice
            # at every RTE starved the main thread to ~1/3 throughput
            # once the keyboard moved to two interrupts per tick.
            if self.ctx_stack:
                uc.context_restore(self.ctx_stack.pop())
                uc.reg_write(UC_M68K_REG_PC, uc.reg_read(UC_M68K_REG_PC))
                return
            self.fail = f"RTE with no injected interrupt @ {pc:#x}"
            uc.emu_stop()
            return
        if intno == 10:             # A-line
            op = struct.unpack(">H", uc.mem_read(pc, 2))[0]
            if op == 0xA002:        # _Read on the .Sony driver
                pb = uc.reg_read(UC_M68K_REG_A0)
                blk = uc.mem_read(pb, 50)
                refnum = struct.unpack(">h", blk[24:26])[0]
                buf, count = struct.unpack(">II", blk[32:40])
                off = struct.unpack(">I", blk[46:50])[0]
                assert refnum == -5, f"_Read on refNum {refnum}"
                data = bytes(self.disk[off:off + count])
                uc.mem_write(buf, data)
                uc.mem_write(pb + 16, b"\x00\x00")          # ioResult
                uc.mem_write(pb + 40, struct.pack(">I", len(data)))
                uc.reg_write(UC_M68K_REG_D0, 0)
                uc.reg_write(UC_M68K_REG_PC, pc + 2)
                if self.trace:
                    print(f"[rom] _Read {count} bytes @{off} -> {buf:#x}")
                return
            if op == 0xA003:        # _Write on the .Sony driver
                pb = uc.reg_read(UC_M68K_REG_A0)
                blk = uc.mem_read(pb, 50)
                refnum = struct.unpack(">h", blk[24:26])[0]
                buf, count = struct.unpack(">II", blk[32:40])
                off = struct.unpack(">I", blk[46:50])[0]
                assert refnum == -5, f"_Write on refNum {refnum}"
                self.disk[off:off + count] = uc.mem_read(buf, count)
                uc.mem_write(pb + 16, b"\x00\x00")          # ioResult
                uc.mem_write(pb + 40, struct.pack(">I", count))
                uc.reg_write(UC_M68K_REG_D0, 0)
                uc.reg_write(UC_M68K_REG_PC, pc + 2)
                if self.trace:
                    print(f"[rom] _Write {count} bytes {buf:#x} -> @{off}")
                return
            if op == 0xA9C9:        # _SysError
                self.fail = f"SysError d0={uc.reg_read(UC_M68K_REG_D0):#x}"
                uc.emu_stop()
                return
            self.fail = f"unknown A-trap {op:#06x} @ {pc:#x}"
            uc.emu_stop()
            return
        self.fail = f"CPU exception intno={intno} @ {pc:#x}"
        uc.emu_stop()

    # ------------------------------------------------------------ irqs
    def _inject(self, level, vector):
        # Interrupt entry: snapshot the FULL CPU context (lazy flags and
        # all), then run the ISR; its RTE restores the snapshot. The mask
        # bits of SR are real (not lazy), so gating on them is safe.
        mu = self.mu
        # Flag-safe boundary: never interrupt right before a conditional
        # consumer (Bcc/DBcc/Scc). QEMU's lazy CC state does not reliably
        # survive a stop/inject/restore cycle landing exactly between a
        # tst and its branch (found as a 64KB wild fill); stepping the
        # conditional first lets QEMU evolve the flags internally.
        # KNOWN RESIDUAL (~1/1000 injections): a branch evaluated right at
        # an injection/restore boundary can still misread the lazy CCR -
        # widening the pre-step to flag-producing instructions does NOT
        # change the rate (measured), so the fault is in the context
        # restore, not the boundary choice. Manifests as one-pixel clamp
        # spikes in Paint drags and the rare 2-char gap in drawn labels.
        # Harness-only; Mini vMac / real hardware are unaffected.
        for _ in range(8):
            op = int.from_bytes(mu.mem_read(self.pc, 2), "big")
            hi = op >> 12
            if hi == 6 or (hi == 5 and (op & 0xC0) == 0xC0):
                mu.emu_start(self.pc, 0xFFFFFFFF, count=1)
                self.pc = mu.reg_read(UC_M68K_REG_PC)
            else:
                break
        sr = mu.reg_read(UC_M68K_REG_SR)
        if ((sr >> 8) & 7) >= level:
            return False
        self.ctx_stack.append(mu.context_save())
        mu.reg_write(UC_M68K_REG_SR, (sr & ~0x0700) | 0x2000 | (level << 8))
        handler = struct.unpack(">I", mu.mem_read(vector, 4))[0]
        self.pc = handler
        return True

    # ------------------------------------------------------------ runtime
    def run_slice(self):
        if self.fail:
            raise RuntimeError(self.fail)
        # keyboard shift-register events mature between slices
        for ev in self.kb_events:
            ev[0] -= 1
        due = [e for e in self.kb_events if e[0] <= 0]
        self.kb_events = [e for e in self.kb_events if e[0] > 0]
        for ev in due:
            if ev[1] == "resp":
                cmd = ev[2]
                if cmd == 0x14:         # Instant: stashed prefix byte only
                    b = self.kb_stash if self.kb_stash is not None else 0x7B
                    self.kb_stash = None
                elif cmd == 0x10:       # Inquiry: next key event
                    if self.kb_queue:
                        b = self.kb_queue.pop(0)
                        if b == 0x79 and self.kb_queue:
                            self.kb_stash = self.kb_queue.pop(0)
                    else:
                        b = 0x7B
                else:
                    b = 0
                self.via_sr = b
            self.via_ifr |= 0x04
        # raise pending device interrupts (priority: SCC level 2 first)
        if self.via_ifr & self.via_ier & 0x7F:
            self.pend1 = True
        if self.pend2 and self._inject(2, 0x68):
            self.pend2 = False
        elif self.pend1 and self._inject(1, 0x64):
            self.pend1 = False
        try:
            self.mu.emu_start(self.pc, 0xFFFFFFFF, count=SLICE)
        except UcError as e:
            if not self.fail:
                self.fail = f"{e} @ {self.mu.reg_read(UC_M68K_REG_PC):#x}"
        if self.rte_flag:           # injected ISR returned: restore the
            self.rte_flag = False   # interrupted context (flags intact;
            self.mu.context_restore(self.ctx_stack.pop())  # memory kept)
        self.pc = self.mu.reg_read(UC_M68K_REG_PC)
        if self.fail:
            raise RuntimeError(self.fail)
        self.slices += 1
        if self.slices % SLICES_PER_TICK == 0:
            self.via_ifr |= 0x02    # CA1 vblank
        self._find_vars()

    def tick(self, n=1):
        target = self.slices + n * SLICES_PER_TICK
        while self.slices < target:
            self.run_slice()

    def _find_vars(self):
        if self.vars is None:
            hdr = self.mu.mem_read(KERNBASE + 4, 8)
            if hdr[:4] == b"UDM1":
                self.vars = struct.unpack(">I", hdr[4:])[0]

    def read_w(self, addr):
        return struct.unpack(">H", self.mu.mem_read(addr, 2))[0]

    @property
    def mouse(self):
        assert self.vars, "kernel not up yet"
        return (self.read_w(self.vars + 28), self.read_w(self.vars + 30))

    # ------------------------------------------------------------ input
    def kb_send(self, *wire):
        self.kb_queue.extend(wire)

    def key(self, name):
        if name in ARROWS:
            s = ARROWS[name]
            self.kb_send(0x79, (s << 1) | 1, 0x79, (s << 1) | 1 | 0x80)
            polls = 4
        else:
            s = SCANS[name]
            self.kb_send((s << 1) | 1, (s << 1) | 1 | 0x80)
            polls = 2
        self.tick(polls * 2 + 4)    # one byte per Instant poll (per tick)

    def mouse_step(self, axis, d):
        # one quadrature step: toggle X1/Y1, set the phase bit so the
        # kernel's (DCD xor phase)==0 -> +1 rule moves the right way
        if axis == 0:
            self.dcd_x ^= 1
            self.x2 = self.dcd_x if d > 0 else self.dcd_x ^ 1
        else:
            self.dcd_y ^= 1
            self.y2 = self.dcd_y if d > 0 else self.dcd_y ^ 1
        self.pend2 = True
        self.run_slice()

    def moveto(self, tx, ty):
        for _ in range(4000):
            x, y = self.mouse
            if (x, y) == (tx, ty):
                return
            if x != tx:
                self.mouse_step(0, 1 if tx > x else -1)
            if y != ty:
                self.mouse_step(1, 1 if ty > y else -1)
        raise RuntimeError(f"moveto({tx},{ty}) stuck at {self.mouse}")

    def button(self, down):
        self.btn = down
        self.tick(2)

    def click(self, x, y):
        self.moveto(x, y)
        self.tick(1)
        self.button(True)
        self.button(False)
        self.tick(2)

    # ------------------------------------------------------------ output
    def shot(self, name):
        fb = self.mu.mem_read(SCRN, SCRH * ROWB)
        def row(y):
            out = bytearray()
            base = y * ROWB
            for b in fb[base:base + ROWB]:
                for bit in range(7, -1, -1):
                    out.append(0 if (b >> bit) & 1 else 255)
            return out
        path = os.path.join(self.artdir, name + ".png")
        png_gray(path, SCRW, SCRH, row)
        print(f"[shot] {path}")


def main():
    args = [a for a in sys.argv[1:] if a != "--trace"]
    trace = "--trace" in sys.argv
    disk, artdir = args[0], args[1]
    mac = Mac(disk, artdir, trace)
    for line in sys.stdin:
        line = line.split("#")[0].strip()
        if not line:
            continue
        cmd, *rest = line.split()
        if cmd == "wait":
            mac.tick(int(rest[0]))
        elif cmd == "shot":
            mac.shot(rest[0])
        elif cmd == "moveto":
            mac.moveto(int(rest[0]), int(rest[1]))
        elif cmd == "btn":
            mac.button(rest[0] == "down")
        elif cmd == "click":
            mac.click(int(rest[0]), int(rest[1]))
        elif cmd == "dblclick":
            mac.click(int(rest[0]), int(rest[1]))
            mac.click(int(rest[0]), int(rest[1]))
        elif cmd == "key":
            mac.key(rest[0])
        elif cmd == "quit":
            break
        else:
            raise SystemExit(f"unknown command: {cmd}")
    print("[harness] done")


if __name__ == "__main__":
    main()
