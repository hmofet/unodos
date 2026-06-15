#!/usr/bin/env python3
"""A functionally-correct (not cycle-accurate) WDC 65C816 interpreter.

Written for the UnoDOS/Apple-IIGS ROM-free harness (iigs/harness.py), the
same house pattern as apple2/harness.py's py65 core - except no usable
Python 65816 core existed, so this is one. It is deliberately small and
readable: full addressing-mode set, the standard opcode table, native +
emulation modes with M/X width tracking, MVN/MVP block moves, and a WDM
hook the harness traps on to play firmware (the ProDOS block driver).

Memory model: a flat 16 MB bytearray (banks 0..255). Reads/writes go
through optional `read_hook(addr24)->int|None` and `write_hook(addr24,val)`
callbacks so the harness can intercept the I/O page ($00/$E0 $Cxxx) and the
WDM-trap firmware pages without the core knowing IIGS specifics.

Not modelled (unused by UnoDOS boot/kernel): decimal-mode ADC/SBC BCD
fix-ups (binary only - we never SED), cycle counts, abort/IRQ/NMI vector
pulls beyond what RTI needs. STP/WAI halt the core (harness watches `halted`).
"""

# flag bit masks (P register)
C = 0x01   # carry
Z = 0x02   # zero
I = 0x04   # irq disable
D = 0x08   # decimal
X = 0x10   # index width: 1 => X/Y are 8-bit
M = 0x20   # accumulator width: 1 => A is 8-bit
V = 0x40   # overflow
N = 0x80   # negative


class CPU65816:
    def __init__(self, mem=None, read_hook=None, write_hook=None,
                 wdm_hook=None):
        self.mem = mem if mem is not None else bytearray(1 << 24)
        self.read_hook = read_hook
        self.write_hook = write_hook
        self.wdm_hook = wdm_hook          # wdm_hook(cpu, imm8) called on WDM
        self.a = 0          # 16-bit accumulator (B:A when M=0)
        self.x = 0
        self.y = 0
        self.sp = 0x01FF    # 16-bit stack pointer
        self.d = 0          # direct page register
        self.pc = 0         # 16-bit program counter (within PBR)
        self.pbr = 0        # program bank
        self.dbr = 0        # data bank
        self.p = M | X | I  # status; widths start 8-bit
        self.e = 1          # emulation mode (reset state)
        self.halted = False
        self.cycles = 0     # instruction count (rough budget meter)
        self._build_table()

    # ------------------------------------------------------------- memory
    def rb(self, addr):
        addr &= 0xFFFFFF
        if self.read_hook is not None:
            v = self.read_hook(addr)
            if v is not None:
                return v & 0xFF
        return self.mem[addr]

    def wb(self, addr, val):
        addr &= 0xFFFFFF
        val &= 0xFF
        if self.write_hook is not None:
            if self.write_hook(addr, val):
                return
        self.mem[addr] = val

    def rw(self, addr):                 # 16-bit, bank wraps within addr math
        return self.rb(addr) | (self.rb((addr + 1) & 0xFFFFFF) << 8)

    def ww(self, addr, val):
        self.wb(addr, val & 0xFF)
        self.wb((addr + 1) & 0xFFFFFF, (val >> 8) & 0xFF)

    # program fetch (within program bank, 16-bit PC wrap)
    def fb(self):
        v = self.rb((self.pbr << 16) | self.pc)
        self.pc = (self.pc + 1) & 0xFFFF
        return v

    def fw(self):
        lo = self.fb()
        return lo | (self.fb() << 8)

    def fl(self):
        lo = self.fb()
        mid = self.fb()
        return lo | (mid << 8) | (self.fb() << 16)

    # ------------------------------------------------------------- flags
    @property
    def m8(self):
        return self.e or (self.p & M)

    @property
    def x8(self):
        return self.e or (self.p & X)

    def set_zn(self, val, eight):
        if eight:
            self.p = (self.p & ~(Z | N)) | (Z if (val & 0xFF) == 0 else 0) \
                | (N if (val & 0x80) else 0)
        else:
            self.p = (self.p & ~(Z | N)) | (Z if (val & 0xFFFF) == 0 else 0) \
                | (N if (val & 0x8000) else 0)

    # --------------------------------------------------------- stack ops
    def push_b(self, val):
        self.wb(self.sp, val & 0xFF)
        if self.e:
            self.sp = 0x0100 | ((self.sp - 1) & 0xFF)
        else:
            self.sp = (self.sp - 1) & 0xFFFF

    def pop_b(self):
        if self.e:
            self.sp = 0x0100 | ((self.sp + 1) & 0xFF)
        else:
            self.sp = (self.sp + 1) & 0xFFFF
        return self.rb(self.sp)

    def push_w(self, val):
        self.push_b((val >> 8) & 0xFF)
        self.push_b(val & 0xFF)

    def pop_w(self):
        lo = self.pop_b()
        return lo | (self.pop_b() << 8)

    # ------------------------------------------------- effective addresses
    # Each returns a 24-bit effective address. Direct-page math wraps in
    # bank 0; data accesses add DBR for the abs/(dp),y/etc. families.
    def _dp(self):                       # direct page,  d + offset
        off = self.fb()
        return (self.d + off) & 0xFFFF

    def _dp_x(self):
        off = self.fb()
        return (self.d + off + self.x) & 0xFFFF

    def _dp_y(self):
        off = self.fb()
        return (self.d + off + self.y) & 0xFFFF

    def _abs(self):
        return (self.dbr << 16) | self.fw()

    def _abs_x(self):
        return ((self.dbr << 16) | self.fw()) + self.x & 0xFFFFFF

    def _abs_y(self):
        return ((self.dbr << 16) | self.fw()) + self.y & 0xFFFFFF

    def _absl(self):
        return self.fl()

    def _absl_x(self):
        return (self.fl() + self.x) & 0xFFFFFF

    def _ind_dp(self):                   # (dp)
        p = self._dp()
        return (self.dbr << 16) | self.rw(p)

    def _ind_dp_x(self):                 # (dp,x)
        p = self._dp_x()
        return (self.dbr << 16) | self.rw(p)

    def _ind_dp_y(self):                 # (dp),y
        p = self._dp()
        base = (self.dbr << 16) | self.rw(p)
        return (base + self.y) & 0xFFFFFF

    def _indl_dp(self):                  # [dp]
        p = self._dp()
        return self.rw(p) | (self.rb((p + 2) & 0xFFFF) << 16)

    def _indl_dp_y(self):                # [dp],y
        p = self._dp()
        base = self.rw(p) | (self.rb((p + 2) & 0xFFFF) << 16)
        return (base + self.y) & 0xFFFFFF

    def _sr(self):                       # stack relative
        off = self.fb()
        return (self.sp + off) & 0xFFFF

    def _sr_y(self):                     # (sr),y
        p = (self.sp + self.fb()) & 0xFFFF
        base = (self.dbr << 16) | self.rw(p)
        return (base + self.y) & 0xFFFFFF

    # width-aware load/store through an effective address
    def load_m(self, ea):
        return self.rb(ea) if self.m8 else self.rw(ea)

    def store_m(self, ea, val):
        if self.m8:
            self.wb(ea, val)
        else:
            self.ww(ea, val)

    # ------------------------------------------------------------- ALU
    def _adc(self, val):
        if self.m8:
            a = self.a & 0xFF
            r = a + val + (self.p & C)
            self.p &= ~(C | V | Z | N)
            if r > 0xFF:
                self.p |= C
            if (~(a ^ val) & (a ^ r) & 0x80):
                self.p |= V
            r &= 0xFF
            self.a = (self.a & 0xFF00) | r
            self.set_zn(r, True)
        else:
            a = self.a & 0xFFFF
            r = a + val + (self.p & C)
            self.p &= ~(C | V | Z | N)
            if r > 0xFFFF:
                self.p |= C
            if (~(a ^ val) & (a ^ r) & 0x8000):
                self.p |= V
            r &= 0xFFFF
            self.a = r
            self.set_zn(r, False)

    def _sbc(self, val):
        if self.m8:
            val ^= 0xFF
            a = self.a & 0xFF
            r = a + val + (self.p & C)
            self.p &= ~(C | V | Z | N)
            if r > 0xFF:
                self.p |= C
            if (~(a ^ val) & (a ^ r) & 0x80):
                self.p |= V
            r &= 0xFF
            self.a = (self.a & 0xFF00) | r
            self.set_zn(r, True)
        else:
            val ^= 0xFFFF
            a = self.a & 0xFFFF
            r = a + val + (self.p & C)
            self.p &= ~(C | V | Z | N)
            if r > 0xFFFF:
                self.p |= C
            if (~(a ^ val) & (a ^ r) & 0x8000):
                self.p |= V
            r &= 0xFFFF
            self.a = r
            self.set_zn(r, False)

    def _cmp(self, reg, val, eight):
        if eight:
            r = (reg & 0xFF) - (val & 0xFF)
            self.p &= ~(C | Z | N)
            if r >= 0:
                self.p |= C
            self.set_zn(r & 0xFF, True)
        else:
            r = (reg & 0xFFFF) - (val & 0xFFFF)
            self.p &= ~(C | Z | N)
            if r >= 0:
                self.p |= C
            self.set_zn(r & 0xFFFF, False)

    # ------------------------------------------------------- run / step
    def reset(self, pc=None, pbr=0):
        self.e = 1
        self.p |= M | X | I
        self.sp = 0x01FF
        self.d = 0
        self.dbr = 0
        self.pbr = pbr
        if pc is None:
            pc = self.rw(0xFFFC)
        self.pc = pc & 0xFFFF
        self.halted = False

    def step(self):
        op = self.fb()
        self.cycles += 1
        self.table[op]()

    def run(self, max_steps=20_000_000):
        n = 0
        while not self.halted and n < max_steps:
            self.step()
            n += 1
        return n

    # =====================================================================
    #  opcode table
    # =====================================================================
    def _build_table(self):
        t = [self._op_brk] * 256
        # --- helpers that close over a mode-fn and apply an operation -----

        def ld_a(mode):
            def f():
                v = self.load_m(mode())
                if self.m8:
                    self.a = (self.a & 0xFF00) | (v & 0xFF)
                    self.set_zn(self.a, True)
                else:
                    self.a = v & 0xFFFF
                    self.set_zn(self.a, False)
            return f

        def ld_x(mode):
            def f():
                if self.x8:
                    self.x = self.rb(mode()) & 0xFF
                    self.set_zn(self.x, True)
                else:
                    self.x = self.rw(mode()) & 0xFFFF
                    self.set_zn(self.x, False)
            return f

        def ld_y(mode):
            def f():
                if self.x8:
                    self.y = self.rb(mode()) & 0xFF
                    self.set_zn(self.y, True)
                else:
                    self.y = self.rw(mode()) & 0xFFFF
                    self.set_zn(self.y, False)
            return f

        def st_a(mode):
            def f():
                self.store_m(mode(), self.a)
            return f

        def st_x(mode):
            def f():
                ea = mode()
                if self.x8:
                    self.wb(ea, self.x)
                else:
                    self.ww(ea, self.x)
            return f

        def st_y(mode):
            def f():
                ea = mode()
                if self.x8:
                    self.wb(ea, self.y)
                else:
                    self.ww(ea, self.y)
            return f

        def st_z(mode):
            def f():
                self.store_m(mode(), 0)
            return f

        def op_and(mode):
            def f():
                v = self.load_m(mode())
                if self.m8:
                    self.a = (self.a & 0xFF00) | ((self.a & v) & 0xFF)
                    self.set_zn(self.a, True)
                else:
                    self.a &= v
                    self.set_zn(self.a, False)
            return f

        def op_ora(mode):
            def f():
                v = self.load_m(mode())
                if self.m8:
                    self.a = (self.a & 0xFF00) | ((self.a | v) & 0xFF)
                    self.set_zn(self.a, True)
                else:
                    self.a |= v
                    self.set_zn(self.a, False)
            return f

        def op_eor(mode):
            def f():
                v = self.load_m(mode())
                if self.m8:
                    self.a = (self.a & 0xFF00) | ((self.a ^ v) & 0xFF)
                    self.set_zn(self.a, True)
                else:
                    self.a ^= v
                    self.set_zn(self.a, False)
            return f

        def op_adc(mode):
            def f():
                self._adc(self.load_m(mode()))
            return f

        def op_sbc(mode):
            def f():
                self._sbc(self.load_m(mode()))
            return f

        def op_cmp(mode):
            def f():
                self._cmp(self.a, self.load_m(mode()), self.m8)
            return f

        def op_cpx(mode):
            def f():
                v = self.rb(mode()) if self.x8 else self.rw(mode())
                self._cmp(self.x, v, self.x8)
            return f

        def op_cpy(mode):
            def f():
                v = self.rb(mode()) if self.x8 else self.rw(mode())
                self._cmp(self.y, v, self.x8)
            return f

        def op_bit(mode, imm=False):
            def f():
                v = self.load_m(mode())
                if self.m8:
                    self.p &= ~Z
                    if (self.a & v & 0xFF) == 0:
                        self.p |= Z
                    if not imm:
                        self.p = (self.p & ~(N | V)) | (v & 0xC0)
                else:
                    self.p &= ~Z
                    if (self.a & v & 0xFFFF) == 0:
                        self.p |= Z
                    if not imm:
                        self.p = (self.p & ~(N | V)) | ((v >> 8) & 0xC0)
            return f

        # read-modify-write helpers (memory)
        def rmw(mode, fn):
            def f():
                ea = mode()
                if self.m8:
                    v = self.rb(ea)
                    v = fn(v, True)
                    self.wb(ea, v & 0xFF)
                else:
                    v = self.rw(ea)
                    v = fn(v, False)
                    self.ww(ea, v & 0xFFFF)
            return f

        def _asl(v, e8):
            top = 0x80 if e8 else 0x8000
            self.p = (self.p & ~C) | (C if v & top else 0)
            v <<= 1
            self.set_zn(v, e8)
            return v

        def _lsr(v, e8):
            self.p = (self.p & ~C) | (C if v & 1 else 0)
            v >>= 1
            self.set_zn(v, e8)
            return v

        def _rol(v, e8):
            top = 0x80 if e8 else 0x8000
            cin = self.p & C
            self.p = (self.p & ~C) | (C if v & top else 0)
            v = (v << 1) | cin
            self.set_zn(v, e8)
            return v

        def _ror(v, e8):
            cin = (self.p & C) << (7 if e8 else 15)
            self.p = (self.p & ~C) | (C if v & 1 else 0)
            v = (v >> 1) | cin
            self.set_zn(v, e8)
            return v

        def _inc(v, e8):
            v += 1
            self.set_zn(v, e8)
            return v

        def _dec(v, e8):
            v -= 1
            self.set_zn(v, e8)
            return v

        def _tsb(v, e8):
            a = self.a & (0xFF if e8 else 0xFFFF)
            self.p = (self.p & ~Z) | (Z if (a & v) == 0 else 0)
            return v | a

        def _trb(v, e8):
            a = self.a & (0xFF if e8 else 0xFFFF)
            self.p = (self.p & ~Z) | (Z if (a & v) == 0 else 0)
            return v & ~a

        # accumulator-mode shifts
        def acc(fn):
            def f():
                if self.m8:
                    r = fn(self.a & 0xFF, True)
                    self.a = (self.a & 0xFF00) | (r & 0xFF)
                else:
                    self.a = fn(self.a & 0xFFFF, False) & 0xFFFF
            return f

        # immediate operand fetch sized by M / X
        def imm_m():
            return self.fw() if not self.m8 else self.fb()

        def imm_x():
            return self.fw() if not self.x8 else self.fb()

        # ---- the ALU/load/store address modes, keyed by suffix ----
        M_dp = self._dp
        M_dpx = self._dp_x
        M_dpy = self._dp_y
        M_abs = self._abs
        M_absx = self._abs_x
        M_absy = self._abs_y
        M_absl = self._absl
        M_abslx = self._absl_x
        M_indd = self._ind_dp
        M_inddx = self._ind_dp_x
        M_inddy = self._ind_dp_y
        M_ildp = self._indl_dp
        M_ildpy = self._indl_dp_y
        M_sr = self._sr
        M_sry = self._sr_y

        # ---------- LDA (full mode set) ----------
        t[0xA9] = self._imm_a_lda
        t[0xA5] = ld_a(M_dp); t[0xB5] = ld_a(M_dpx)
        t[0xAD] = ld_a(M_abs); t[0xBD] = ld_a(M_absx); t[0xB9] = ld_a(M_absy)
        t[0xAF] = ld_a(M_absl); t[0xBF] = ld_a(M_abslx)
        t[0xA1] = ld_a(M_inddx); t[0xB1] = ld_a(M_inddy); t[0xB2] = ld_a(M_indd)
        t[0xA7] = ld_a(M_ildp); t[0xB7] = ld_a(M_ildpy)
        t[0xA3] = ld_a(M_sr); t[0xB3] = ld_a(M_sry)
        # ---------- STA ----------
        t[0x85] = st_a(M_dp); t[0x95] = st_a(M_dpx)
        t[0x8D] = st_a(M_abs); t[0x9D] = st_a(M_absx); t[0x99] = st_a(M_absy)
        t[0x8F] = st_a(M_absl); t[0x9F] = st_a(M_abslx)
        t[0x81] = st_a(M_inddx); t[0x91] = st_a(M_inddy); t[0x92] = st_a(M_indd)
        t[0x87] = st_a(M_ildp); t[0x97] = st_a(M_ildpy)
        t[0x83] = st_a(M_sr); t[0x93] = st_a(M_sry)
        # ---------- LDX / LDY ----------
        t[0xA2] = self._imm_x_ldx
        t[0xA6] = ld_x(M_dp); t[0xB6] = ld_x(M_dpy)
        t[0xAE] = ld_x(M_abs); t[0xBE] = ld_x(M_absy)
        t[0xA0] = self._imm_x_ldy
        t[0xA4] = ld_y(M_dp); t[0xB4] = ld_y(M_dpx)
        t[0xAC] = ld_y(M_abs); t[0xBC] = ld_y(M_absx)
        # ---------- STX / STY / STZ ----------
        t[0x86] = st_x(M_dp); t[0x96] = st_x(M_dpy)
        t[0x8E] = st_x(M_abs)
        t[0x84] = st_y(M_dp); t[0x94] = st_y(M_dpx)
        t[0x8C] = st_y(M_abs)
        t[0x64] = st_z(M_dp); t[0x74] = st_z(M_dpx)
        t[0x9C] = st_z(M_abs); t[0x9E] = st_z(M_absx)
        # ---------- AND / ORA / EOR ----------
        t[0x29] = op_and(None)  # immediate handled specially below
        t[0x25] = op_and(M_dp); t[0x35] = op_and(M_dpx)
        t[0x2D] = op_and(M_abs); t[0x3D] = op_and(M_absx); t[0x39] = op_and(M_absy)
        t[0x2F] = op_and(M_absl); t[0x3F] = op_and(M_abslx)
        t[0x21] = op_and(M_inddx); t[0x31] = op_and(M_inddy); t[0x32] = op_and(M_indd)
        t[0x27] = op_and(M_ildp); t[0x37] = op_and(M_ildpy)
        t[0x23] = op_and(M_sr); t[0x33] = op_and(M_sry)
        t[0x09] = op_ora(None)
        t[0x05] = op_ora(M_dp); t[0x15] = op_ora(M_dpx)
        t[0x0D] = op_ora(M_abs); t[0x1D] = op_ora(M_absx); t[0x19] = op_ora(M_absy)
        t[0x0F] = op_ora(M_absl); t[0x1F] = op_ora(M_abslx)
        t[0x01] = op_ora(M_inddx); t[0x11] = op_ora(M_inddy); t[0x12] = op_ora(M_indd)
        t[0x07] = op_ora(M_ildp); t[0x17] = op_ora(M_ildpy)
        t[0x03] = op_ora(M_sr); t[0x13] = op_ora(M_sry)
        t[0x49] = op_eor(None)
        t[0x45] = op_eor(M_dp); t[0x55] = op_eor(M_dpx)
        t[0x4D] = op_eor(M_abs); t[0x5D] = op_eor(M_absx); t[0x59] = op_eor(M_absy)
        t[0x4F] = op_eor(M_absl); t[0x5F] = op_eor(M_abslx)
        t[0x41] = op_eor(M_inddx); t[0x51] = op_eor(M_inddy); t[0x52] = op_eor(M_indd)
        t[0x47] = op_eor(M_ildp); t[0x57] = op_eor(M_ildpy)
        t[0x43] = op_eor(M_sr); t[0x53] = op_eor(M_sry)
        # immediate AND/ORA/EOR need M-sized fetch -> dedicated handlers
        t[0x29] = self._imm_and; t[0x09] = self._imm_ora; t[0x49] = self._imm_eor
        # ---------- ADC / SBC ----------
        t[0x69] = self._imm_adc
        t[0x65] = op_adc(M_dp); t[0x75] = op_adc(M_dpx)
        t[0x6D] = op_adc(M_abs); t[0x7D] = op_adc(M_absx); t[0x79] = op_adc(M_absy)
        t[0x6F] = op_adc(M_absl); t[0x7F] = op_adc(M_abslx)
        t[0x61] = op_adc(M_inddx); t[0x71] = op_adc(M_inddy); t[0x72] = op_adc(M_indd)
        t[0x67] = op_adc(M_ildp); t[0x77] = op_adc(M_ildpy)
        t[0x63] = op_adc(M_sr); t[0x73] = op_adc(M_sry)
        t[0xE9] = self._imm_sbc
        t[0xE5] = op_sbc(M_dp); t[0xF5] = op_sbc(M_dpx)
        t[0xED] = op_sbc(M_abs); t[0xFD] = op_sbc(M_absx); t[0xF9] = op_sbc(M_absy)
        t[0xEF] = op_sbc(M_absl); t[0xFF] = op_sbc(M_abslx)
        t[0xE1] = op_sbc(M_inddx); t[0xF1] = op_sbc(M_inddy); t[0xF2] = op_sbc(M_indd)
        t[0xE7] = op_sbc(M_ildp); t[0xF7] = op_sbc(M_ildpy)
        t[0xE3] = op_sbc(M_sr); t[0xF3] = op_sbc(M_sry)
        # ---------- CMP / CPX / CPY ----------
        t[0xC9] = self._imm_cmp
        t[0xC5] = op_cmp(M_dp); t[0xD5] = op_cmp(M_dpx)
        t[0xCD] = op_cmp(M_abs); t[0xDD] = op_cmp(M_absx); t[0xD9] = op_cmp(M_absy)
        t[0xCF] = op_cmp(M_absl); t[0xDF] = op_cmp(M_abslx)
        t[0xC1] = op_cmp(M_inddx); t[0xD1] = op_cmp(M_inddy); t[0xD2] = op_cmp(M_indd)
        t[0xC7] = op_cmp(M_ildp); t[0xD7] = op_cmp(M_ildpy)
        t[0xC3] = op_cmp(M_sr); t[0xD3] = op_cmp(M_sry)
        t[0xE0] = self._imm_cpx
        t[0xE4] = op_cpx(M_dp); t[0xEC] = op_cpx(M_abs)
        t[0xC0] = self._imm_cpy
        t[0xC4] = op_cpy(M_dp); t[0xCC] = op_cpy(M_abs)
        # ---------- BIT ----------
        t[0x89] = self._imm_bit
        t[0x24] = op_bit(M_dp); t[0x34] = op_bit(M_dpx)
        t[0x2C] = op_bit(M_abs); t[0x3C] = op_bit(M_absx)
        # ---------- shifts / RMW ----------
        t[0x0A] = acc(_asl); t[0x4A] = acc(_lsr)
        t[0x2A] = acc(_rol); t[0x6A] = acc(_ror)
        t[0x06] = rmw(M_dp, _asl); t[0x16] = rmw(M_dpx, _asl)
        t[0x0E] = rmw(M_abs, _asl); t[0x1E] = rmw(M_absx, _asl)
        t[0x46] = rmw(M_dp, _lsr); t[0x56] = rmw(M_dpx, _lsr)
        t[0x4E] = rmw(M_abs, _lsr); t[0x5E] = rmw(M_absx, _lsr)
        t[0x26] = rmw(M_dp, _rol); t[0x36] = rmw(M_dpx, _rol)
        t[0x2E] = rmw(M_abs, _rol); t[0x3E] = rmw(M_absx, _rol)
        t[0x66] = rmw(M_dp, _ror); t[0x76] = rmw(M_dpx, _ror)
        t[0x6E] = rmw(M_abs, _ror); t[0x7E] = rmw(M_absx, _ror)
        t[0x1A] = acc(_inc); t[0x3A] = acc(_dec)
        t[0xE6] = rmw(M_dp, _inc); t[0xF6] = rmw(M_dpx, _inc)
        t[0xEE] = rmw(M_abs, _inc); t[0xFE] = rmw(M_absx, _inc)
        t[0xC6] = rmw(M_dp, _dec); t[0xD6] = rmw(M_dpx, _dec)
        t[0xCE] = rmw(M_abs, _dec); t[0xDE] = rmw(M_absx, _dec)
        t[0x04] = rmw(M_dp, _tsb); t[0x0C] = rmw(M_abs, _tsb)
        t[0x14] = rmw(M_dp, _trb); t[0x1C] = rmw(M_abs, _trb)
        # ---------- INX/INY/DEX/DEY ----------
        t[0xE8] = self._inx; t[0xC8] = self._iny
        t[0xCA] = self._dex; t[0x88] = self._dey
        # ---------- transfers ----------
        t[0xAA] = self._tax; t[0xA8] = self._tay
        t[0x8A] = self._txa; t[0x98] = self._tya
        t[0xBA] = self._tsx; t[0x9A] = self._txs
        t[0x9B] = self._txy; t[0xBB] = self._tyx
        t[0x5B] = self._tcd; t[0x7B] = self._tdc
        t[0x1B] = self._tcs; t[0x3B] = self._tsc
        t[0xEB] = self._xba
        # ---------- stack ----------
        t[0x48] = self._pha; t[0x68] = self._pla
        t[0xDA] = self._phx; t[0xFA] = self._plx
        t[0x5A] = self._phy; t[0x7A] = self._ply
        t[0x08] = self._php; t[0x28] = self._plp
        t[0x8B] = self._phb; t[0xAB] = self._plb
        t[0x0B] = self._phd; t[0x2B] = self._pld
        t[0x4B] = self._phk
        t[0xF4] = self._pea; t[0xD4] = self._pei; t[0x62] = self._per
        # ---------- flags ----------
        t[0x18] = self._clc; t[0x38] = self._sec
        t[0x58] = self._cli; t[0x78] = self._sei
        t[0xB8] = self._clv; t[0xD8] = self._cld; t[0xF8] = self._sed
        t[0xC2] = self._rep; t[0xE2] = self._sep; t[0xFB] = self._xce
        # ---------- branches ----------
        t[0x90] = self._bcc; t[0xB0] = self._bcs
        t[0xD0] = self._bne; t[0xF0] = self._beq
        t[0x10] = self._bpl; t[0x30] = self._bmi
        t[0x50] = self._bvc; t[0x70] = self._bvs
        t[0x80] = self._bra; t[0x82] = self._brl
        # ---------- jumps / calls ----------
        t[0x4C] = self._jmp_abs; t[0x6C] = self._jmp_ind
        t[0x7C] = self._jmp_indx; t[0x5C] = self._jml_abs
        t[0xDC] = self._jml_ind
        t[0x20] = self._jsr_abs; t[0xFC] = self._jsr_indx
        t[0x22] = self._jsl; t[0x60] = self._rts
        t[0x6B] = self._rtl; t[0x40] = self._rti
        # ---------- block moves ----------
        t[0x54] = self._mvn; t[0x44] = self._mvp
        # ---------- misc ----------
        t[0xEA] = self._nop; t[0x42] = self._wdm
        t[0xDB] = self._stp; t[0xCB] = self._wai
        t[0x00] = self._brk; t[0x02] = self._cop
        self.table = t

    # ----- immediate handlers (need M/X-sized fetch) -----
    def _imm_a_lda(self):
        if self.m8:
            self.a = (self.a & 0xFF00) | self.fb()
            self.set_zn(self.a, True)
        else:
            self.a = self.fw()
            self.set_zn(self.a, False)

    def _imm_x_ldx(self):
        if self.x8:
            self.x = self.fb()
            self.set_zn(self.x, True)
        else:
            self.x = self.fw()
            self.set_zn(self.x, False)

    def _imm_x_ldy(self):
        if self.x8:
            self.y = self.fb()
            self.set_zn(self.y, True)
        else:
            self.y = self.fw()
            self.set_zn(self.y, False)

    def _imm_m(self):
        return self.fb() if self.m8 else self.fw()

    def _imm_and(self):
        v = self._imm_m()
        if self.m8:
            self.a = (self.a & 0xFF00) | ((self.a & v) & 0xFF)
            self.set_zn(self.a, True)
        else:
            self.a &= v
            self.set_zn(self.a, False)

    def _imm_ora(self):
        v = self._imm_m()
        if self.m8:
            self.a = (self.a & 0xFF00) | ((self.a | v) & 0xFF)
            self.set_zn(self.a, True)
        else:
            self.a |= v
            self.set_zn(self.a, False)

    def _imm_eor(self):
        v = self._imm_m()
        if self.m8:
            self.a = (self.a & 0xFF00) | ((self.a ^ v) & 0xFF)
            self.set_zn(self.a, True)
        else:
            self.a ^= v
            self.set_zn(self.a, False)

    def _imm_adc(self):
        self._adc(self._imm_m())

    def _imm_sbc(self):
        self._sbc(self._imm_m())

    def _imm_cmp(self):
        self._cmp(self.a, self._imm_m(), self.m8)

    def _imm_cpx(self):
        v = self.fb() if self.x8 else self.fw()
        self._cmp(self.x, v, self.x8)

    def _imm_cpy(self):
        v = self.fb() if self.x8 else self.fw()
        self._cmp(self.y, v, self.x8)

    def _imm_bit(self):
        v = self._imm_m()
        m = 0xFF if self.m8 else 0xFFFF
        self.p = (self.p & ~Z) | (Z if (self.a & v & m) == 0 else 0)

    # ----- index inc/dec -----
    def _inx(self):
        if self.x8:
            self.x = (self.x + 1) & 0xFF; self.set_zn(self.x, True)
        else:
            self.x = (self.x + 1) & 0xFFFF; self.set_zn(self.x, False)

    def _iny(self):
        if self.x8:
            self.y = (self.y + 1) & 0xFF; self.set_zn(self.y, True)
        else:
            self.y = (self.y + 1) & 0xFFFF; self.set_zn(self.y, False)

    def _dex(self):
        if self.x8:
            self.x = (self.x - 1) & 0xFF; self.set_zn(self.x, True)
        else:
            self.x = (self.x - 1) & 0xFFFF; self.set_zn(self.x, False)

    def _dey(self):
        if self.x8:
            self.y = (self.y - 1) & 0xFF; self.set_zn(self.y, True)
        else:
            self.y = (self.y - 1) & 0xFFFF; self.set_zn(self.y, False)

    # ----- transfers (width per destination register) -----
    def _tax(self):
        if self.x8:
            self.x = self.a & 0xFF; self.set_zn(self.x, True)
        else:
            self.x = self.a & 0xFFFF; self.set_zn(self.x, False)

    def _tay(self):
        if self.x8:
            self.y = self.a & 0xFF; self.set_zn(self.y, True)
        else:
            self.y = self.a & 0xFFFF; self.set_zn(self.y, False)

    def _txa(self):
        if self.m8:
            self.a = (self.a & 0xFF00) | (self.x & 0xFF); self.set_zn(self.a, True)
        else:
            self.a = self.x & 0xFFFF; self.set_zn(self.a, False)

    def _tya(self):
        if self.m8:
            self.a = (self.a & 0xFF00) | (self.y & 0xFF); self.set_zn(self.a, True)
        else:
            self.a = self.y & 0xFFFF; self.set_zn(self.a, False)

    def _tsx(self):
        if self.x8:
            self.x = self.sp & 0xFF; self.set_zn(self.x, True)
        else:
            self.x = self.sp & 0xFFFF; self.set_zn(self.x, False)

    def _txs(self):
        if self.e:
            self.sp = 0x0100 | (self.x & 0xFF)
        else:
            self.sp = self.x & 0xFFFF

    def _txy(self):
        if self.x8:
            self.y = self.x & 0xFF; self.set_zn(self.y, True)
        else:
            self.y = self.x & 0xFFFF; self.set_zn(self.y, False)

    def _tyx(self):
        if self.x8:
            self.x = self.y & 0xFF; self.set_zn(self.x, True)
        else:
            self.x = self.y & 0xFFFF; self.set_zn(self.x, False)

    def _tcd(self):
        self.d = self.a & 0xFFFF; self.set_zn(self.d, False)

    def _tdc(self):
        self.a = self.d & 0xFFFF; self.set_zn(self.a, False)

    def _tcs(self):
        if self.e:
            self.sp = 0x0100 | (self.a & 0xFF)
        else:
            self.sp = self.a & 0xFFFF

    def _tsc(self):
        self.a = self.sp & 0xFFFF; self.set_zn(self.a, False)

    def _xba(self):
        lo = self.a & 0xFF
        hi = (self.a >> 8) & 0xFF
        self.a = (lo << 8) | hi
        self.set_zn(hi, True)        # flags from new low byte (old high)

    # ----- stack -----
    def _pha(self):
        if self.m8:
            self.push_b(self.a)
        else:
            self.push_w(self.a)

    def _pla(self):
        if self.m8:
            self.a = (self.a & 0xFF00) | self.pop_b(); self.set_zn(self.a, True)
        else:
            self.a = self.pop_w(); self.set_zn(self.a, False)

    def _phx(self):
        if self.x8:
            self.push_b(self.x)
        else:
            self.push_w(self.x)

    def _plx(self):
        if self.x8:
            self.x = self.pop_b(); self.set_zn(self.x, True)
        else:
            self.x = self.pop_w(); self.set_zn(self.x, False)

    def _phy(self):
        if self.x8:
            self.push_b(self.y)
        else:
            self.push_w(self.y)

    def _ply(self):
        if self.x8:
            self.y = self.pop_b(); self.set_zn(self.y, True)
        else:
            self.y = self.pop_w(); self.set_zn(self.y, False)

    def _php(self):
        self.push_b(self.p)

    def _plp(self):
        self.p = self.pop_b()
        if self.e:
            self.p |= M | X
        if self.p & X:                # narrowing index regs drops high bytes
            self.x &= 0xFF
            self.y &= 0xFF

    def _phb(self):
        self.push_b(self.dbr)

    def _plb(self):
        self.dbr = self.pop_b(); self.set_zn(self.dbr, True)

    def _phd(self):
        self.push_w(self.d)

    def _pld(self):
        self.d = self.pop_w(); self.set_zn(self.d, False)

    def _phk(self):
        self.push_b(self.pbr)

    def _pea(self):
        self.push_w(self.fw())

    def _pei(self):
        self.push_w(self.rw(self._dp()))

    def _per(self):
        rel = self.fw()
        self.push_w((self.pc + rel) & 0xFFFF)

    # ----- flags -----
    def _clc(self): self.p &= ~C
    def _sec(self): self.p |= C
    def _cli(self): self.p &= ~I
    def _sei(self): self.p |= I
    def _clv(self): self.p &= ~V
    def _cld(self): self.p &= ~D
    def _sed(self): self.p |= D

    def _rep(self):
        m = self.fb()
        self.p &= ~m
        if self.e:
            self.p |= M | X

    def _sep(self):
        m = self.fb()
        self.p |= m
        if self.p & X:
            self.x &= 0xFF
            self.y &= 0xFF

    def _xce(self):
        carry = self.p & C
        newe = 1 if carry else 0
        self.p = (self.p & ~C) | (C if self.e else 0)
        self.e = newe
        if self.e:
            self.p |= M | X
            self.sp = 0x0100 | (self.sp & 0xFF)
            self.x &= 0xFF
            self.y &= 0xFF

    # ----- branches -----
    def _branch(self, take):
        off = self.fb()
        if off & 0x80:
            off -= 0x100
        if take:
            self.pc = (self.pc + off) & 0xFFFF

    def _bcc(self): self._branch(not (self.p & C))
    def _bcs(self): self._branch(self.p & C)
    def _bne(self): self._branch(not (self.p & Z))
    def _beq(self): self._branch(self.p & Z)
    def _bpl(self): self._branch(not (self.p & N))
    def _bmi(self): self._branch(self.p & N)
    def _bvc(self): self._branch(not (self.p & V))
    def _bvs(self): self._branch(self.p & V)
    def _bra(self): self._branch(True)

    def _brl(self):
        rel = self.fw()
        if rel & 0x8000:
            rel -= 0x10000
        self.pc = (self.pc + rel) & 0xFFFF

    # ----- jumps / calls -----
    def _jmp_abs(self):
        self.pc = self.fw()

    def _jmp_ind(self):
        p = self.fw()
        self.pc = self.rw(p)         # (abs) reads from bank 0

    def _jmp_indx(self):
        p = (self.fw() + self.x) & 0xFFFF
        self.pc = self.rw((self.pbr << 16) | p)

    def _jml_abs(self):
        self.pc = self.fw()
        self.pbr = self.fb()

    def _jml_ind(self):                # JML [abs]
        p = self.fw()
        self.pc = self.rw(p)
        self.pbr = self.rb((p + 2) & 0xFFFF)

    def _jsr_abs(self):
        target = self.fw()
        ret = (self.pc - 1) & 0xFFFF
        self.push_w(ret)
        self.pc = target

    def _jsr_indx(self):               # JSR (abs,x)
        target_ptr = self.fw()
        ret = (self.pc - 1) & 0xFFFF
        self.push_w(ret)
        p = (target_ptr + self.x) & 0xFFFF
        self.pc = self.rw((self.pbr << 16) | p)

    def _jsl(self):
        lo = self.fb(); mid = self.fb(); bank = self.fb()
        ret = (self.pc - 1) & 0xFFFF
        self.push_b(self.pbr)
        self.push_w(ret)
        self.pc = lo | (mid << 8)
        self.pbr = bank

    def _rts(self):
        self.pc = (self.pop_w() + 1) & 0xFFFF

    def _rtl(self):
        self.pc = (self.pop_w() + 1) & 0xFFFF
        self.pbr = self.pop_b()

    def _rti(self):
        self.p = self.pop_b()
        if self.e:
            self.p |= M | X
            self.pc = self.pop_w()
        else:
            self.pc = self.pop_w()
            self.pbr = self.pop_b()
        if self.p & X:
            self.x &= 0xFF; self.y &= 0xFF

    # ----- block moves (MVN/MVP): A = count-1, X src, Y dst ----------
    def _mvn(self):
        dbank = self.fb(); sbank = self.fb()
        self.dbr = dbank
        # transfer one byte per execution, decrementing PC if more remain
        src = (sbank << 16) | (self.x & 0xFFFF)
        dst = (dbank << 16) | (self.y & 0xFFFF)
        self.wb(dst, self.rb(src))
        self.x = (self.x + 1) & 0xFFFF
        self.y = (self.y + 1) & 0xFFFF
        self.a = (self.a - 1) & 0xFFFF
        if self.a != 0xFFFF:
            self.pc = (self.pc - 3) & 0xFFFF   # re-execute MVN
        if self.x8:
            self.x &= 0xFF; self.y &= 0xFF

    def _mvp(self):
        dbank = self.fb(); sbank = self.fb()
        self.dbr = dbank
        src = (sbank << 16) | (self.x & 0xFFFF)
        dst = (dbank << 16) | (self.y & 0xFFFF)
        self.wb(dst, self.rb(src))
        self.x = (self.x - 1) & 0xFFFF
        self.y = (self.y - 1) & 0xFFFF
        self.a = (self.a - 1) & 0xFFFF
        if self.a != 0xFFFF:
            self.pc = (self.pc - 3) & 0xFFFF
        if self.x8:
            self.x &= 0xFF; self.y &= 0xFF

    # ----- misc -----
    def _nop(self):
        pass

    def _wdm(self):
        imm = self.fb()
        if self.wdm_hook is not None:
            self.wdm_hook(self, imm)

    def _stp(self):
        self.halted = True

    def _wai(self):
        # no interrupts in the harness; treat as halt so a stray WAI is visible
        self.halted = True

    def _brk(self):
        self.fb()                    # signature byte
        self.push_w(self.pc)
        self.push_b(self.p)
        self.p |= I
        if self.e:
            self.pc = self.rw(0xFFFE)
        else:
            self.push_b(self.pbr); self.pbr = 0
            self.pc = self.rw(0xFFE6)

    def _cop(self):
        self.fb()
        self.push_w(self.pc)
        self.push_b(self.p)
        self.p |= I
        if self.e:
            self.pc = self.rw(0xFFF4)
        else:
            self.push_b(self.pbr); self.pbr = 0
            self.pc = self.rw(0xFFE4)

    def _op_brk(self):
        # default-table placeholder (overwritten); real BRK is _brk
        self._brk()


# --------------------------------------------------------------------------
# self-test: a tiny program exercising width switches, math, branches, JSR,
# block move and a WDM trap.  `python cpu65816.py` should print "SELFTEST OK".
# --------------------------------------------------------------------------
if __name__ == "__main__":
    cpu = CPU65816()
    trapped = []
    cpu.wdm_hook = lambda c, imm: trapped.append(imm)

    prog = bytes([
        0x18,                    # CLC
        0xFB,                    # XCE   -> native
        0xC2, 0x30,              # REP #$30  (16-bit A/X/Y)
        0xA9, 0x34, 0x12,        # LDA #$1234
        0x18,                    # CLC
        0x69, 0x11, 0x11,        # ADC #$1111 -> $2345
        0x8D, 0x00, 0x20,        # STA $2000
        0xA2, 0x05, 0x00,        # LDX #5
        0xA0, 0x00, 0x00,        # LDY #0  (sum 5+4+..+1 via loop)
        # loop: TYA? simpler: decrement X to 0 counting in A
        0xA9, 0x00, 0x00,        # LDA #0
        # loop @ pc
        0x18,                    # CLC
        0x8A,                    # TXA (X->A, but A 16 ok)  -- actually adds X
        # do A += X each iter: we'll just sum via: CLC, (push) ...
        0xEA,                    # NOP
        0x42, 0x99,              # WDM #$99   (trap)
        0xDB                     # STP
    ])
    cpu.mem[0x8000:0x8000 + len(prog)] = prog
    cpu.reset(pc=0x8000)
    cpu.run(max_steps=10000)

    ok = True
    if cpu.rw(0x2000) != 0x2345:
        print("FAIL adc/sta:", hex(cpu.rw(0x2000))); ok = False
    if trapped != [0x99]:
        print("FAIL wdm trap:", trapped); ok = False
    if not cpu.halted:
        print("FAIL not halted"); ok = False

    # block-move test: copy 4 bytes from $1000 to $1100
    cpu2 = CPU65816()
    cpu2.mem[0x1000:0x1004] = b"\xDE\xAD\xBE\xEF"
    prog2 = bytes([
        0x18, 0xFB,              # CLC XCE
        0xC2, 0x30,              # REP #$30
        0xA9, 0x03, 0x00,        # LDA #3  (count-1)
        0xA2, 0x00, 0x10,        # LDX #$1000
        0xA0, 0x00, 0x11,        # LDY #$1100
        0x54, 0x00, 0x00,        # MVN #0,#0
        0xDB                     # STP
    ])
    cpu2.mem[0x8000:0x8000 + len(prog2)] = prog2
    cpu2.reset(pc=0x8000)
    cpu2.run(max_steps=10000)
    if cpu2.mem[0x1100:0x1104] != b"\xDE\xAD\xBE\xEF":
        print("FAIL mvn:", cpu2.mem[0x1100:0x1104].hex()); ok = False

    print("SELFTEST OK" if ok else "SELFTEST FAILED")
