// ============================================================================
// UnoDOS / PinePhone — bare-metal MIPI-DSI panel bring-up (Path A).
// ============================================================================
// THE key finding (see PINEPHONE-BRINGUP.md §3/§6): nothing in the A64 PinePhone
// boot chain lights the DSI panel — not mainline U-Boot, not Megi's fork, not
// Tow-Boot. Unlike the Raspberry Pi (where VideoCore firmware brings up HDMI before
// the kernel), here the payload MUST do the full panel bring-up itself, or the
// screen stays dark. This file is that bring-up, distilled register-for-register
// from Apache NuttX's PinePhone port (lupyuen) + the Linux sun50i-a64 drivers:
//
//   {tcon0,dsi,dphy,de}_clk  CCU PLLs/gates, brought up per-block in p-boot order
//                 (PLL_VIDEO0/PLL_MIPI before TCON0; DSI bus before host; MIPI clk
//                  before D-PHY; PLL_DE/DE clock last, before the mixer)
//   rsb_init      Reduced Serial Bus driver (to reach the AXP803 PMIC)
//   pmic_init     AXP803 rails — DLDO2 = MIPI-DSI power, DLDO1/GPIO0LDO
//   dsi_host_init MIPI-DSI host controller (0x01CA0000)
//   dphy_init     MIPI D-PHY analog + LDOs (0x01CA1000)
//   st7703_init   ~20 DCS commands to the Sitronix ST7703 (XBD599 720x1440 panel),
//                 streamed from the precomputed dsi_init_seq blob (see mkdata.py)
//   dsi_start     switch the DSI bus to high-speed video mode
//   tcon0_init    TCON0 timing controller (720x1440, DSI/8080 triggered mode)
//   backlight_on  PWM (PL10) + enable GPIO (PH10)
//
// The DE2 mixer / UI overlay (the only part the port previously did) runs AFTER all
// of the above, in fb_init (kernel.s). None of this can be exercised by the Unicorn
// harness (it has no panel/DSI/clock model — it sinks the MMIO and satisfies the
// status polls). Real verification is on hardware; build with --defsym PANELDBG=1
// to blink the PD18 status LED a stage count between each block so a freeze localises
// blind (no serial). All status polls are bounded so a stuck peripheral can't hang.
// ============================================================================

// ---- block base addresses (A64) -------------------------------------------
.equ SRAM_CTRL1, 0x01C00004      // SRAM C1 -> DE routing
.equ CCU,        0x01C20000      // clock control unit
.equ TCON0,      0x01C0C000      // timing controller 0
.equ DSI,        0x01CA0000      // MIPI-DSI host
.equ DPHY,       0x01CA1000      // MIPI D-PHY
.equ RSB,        0x01F03400      // reduced serial bus (to the PMIC)
.equ R_PWM,      0x01F03800      // backlight PWM
.equ R_PIO,      0x01F02C00      // port L GPIO (PWM/RSB pins)
.equ R_PRCM,     0x01F01400      // PRCM (RSB clock gate/reset)
.equ PIO,        0x01C20800      // main GPIO (ports B-H); reset/backlight pins
// RSB registers
.equ RSB_CTRL,   RSB+0x00
.equ RSB_CCR,    RSB+0x04
.equ RSB_STAT,   RSB+0x0C
.equ RSB_AR,     RSB+0x10
.equ RSB_DATA,   RSB+0x1C
.equ RSB_DMCR,   RSB+0x28
.equ RSB_CMD,    RSB+0x2C
.equ RSB_DAR,    RSB+0x30
.equ AXP_RT,     0x2D            // AXP803 runtime address on the RSB

// ============================================================================
// tiny MMIO helpers
// ============================================================================
// apply_pokes: x0 -> table of {.word addr, .word val} pairs, terminated by addr==0.
apply_pokes:
ap_l:
    ldr   w1, [x0], #4
    cbz   w1, ap_done
    ldr   w2, [x0], #4
    str   w2, [x1]
    dsb   sy
    b     ap_l
ap_done:
    ret

// mmio_setbits / mmio_clrbits: x0 = addr, w1 = mask
mmio_setbits:
    ldr   w2, [x0]
    orr   w2, w2, w1
    str   w2, [x0]
    dsb   sy
    ret
mmio_clrbits:
    ldr   w2, [x0]
    bic   w2, w2, w1
    str   w2, [x0]
    dsb   sy
    ret

// delay_ms: w0 = milliseconds, paced off the 24 MHz generic timer. Leaf.
delay_ms:
    mov   x2, #24000
    mul   x2, x2, x0
    mrs   x1, cntpct_el0
    add   x1, x1, x2
dms1:
    mrs   x3, cntpct_el0
    cmp   x3, x1
    b.lo  dms1
    ret

// ============================================================================
// PD18 status LED (green; GPIO114). Shared with the LEDTEST path in kernel.s and
// used by the staged PANELDBG beacon below. Polarity-agnostic blink.
// ============================================================================
// The PinePhone status LED is RGB: red=PD19, green=PD18, blue=PD20 (all bank D,
// active-high). led_init configures all three as outputs; led_rgb drives a colour.
led_init:
    ldr   x0, =PD_CFG2                     // PD16-23 mode; PD18/19/20 = [11:8]/[15:12]/[19:16]
    ldr   w1, [x0]
    ldr   w2, =0x000FFF00                  // clear PD18/19/20 mode fields
    bic   w1, w1, w2
    ldr   w2, =0x00011100                  // 0b001 (output) for each
    orr   w1, w1, w2
    str   w1, [x0]
    dsb   sy
    ret
// led_rgb: w0 = colour mask, bit0=red(PD19) bit1=green(PD18) bit2=blue(PD20). Leaf.
led_rgb:
    ldr   x1, =PD_DAT
    ldr   w2, [x1]
    ldr   w3, =0x001C0000                  // clear PD18/19/20 data bits
    bic   w2, w2, w3
    tst   w0, #1
    b.eq  lrg_g
    orr   w2, w2, #0x80000                 // red  PD19
lrg_g:
    tst   w0, #2
    b.eq  lrg_b
    orr   w2, w2, #0x40000                 // green PD18
lrg_b:
    tst   w0, #4
    b.eq  lrg_w
    orr   w2, w2, #0x100000                // blue PD20
lrg_w:
    str   w2, [x1]
    dsb   sy
    ret
led_set:                                  // w0 != 0 -> green on/off (LEDTEST path)
    ldr   x1, =PD_DAT
    ldr   w2, [x1]
    mov   w3, #0x40000                    // 1 << 18
    cbz   w0, ls_off
    orr   w2, w2, w3
    b     ls_w
ls_off:
    bic   w2, w2, w3
ls_w:
    str   w2, [x1]
    ret
led_halfsec:
    mov   w0, #400
    b     delay_ms

.ifdef PANELDBG
// Debug exception vectors: any CPU exception lands here (VBAR set in _start) and we
// fast-flutter the LED forever instead of letting U-Boot reset (which would reboot-
// loop and hide where panel_init faulted). The flutter rate (~CPU-loop, not the timer)
// is deliberately faster than the stage beacon so a crash is unmistakable.
// Each of the 16 vector slots stamps its index into w9 then branches to the common
// handler, so the UART dump can report WHICH vector fired (slot 0-3 = cur-EL SP0,
// 4-7 = cur-EL SPx [sync=4, IRQ=5, FIQ=6, SError=7], 8-11 = lower-EL aarch64). That
// distinction is the whole game: a *synchronous* abort (slot 4) = a bad load/store
// address (read FAR); an *SError* (slot 7) = an async external abort from a peripheral
// (e.g. the DSI host erroring on a FIFO write into a wedged engine).
.macro VEC n
    .balign 0x80
    mov   w9, #\n
    b     fault_loop
.endm
.balign 0x800
dbg_vectors:
    VEC 0
    VEC 1
    VEC 2
    VEC 3
    VEC 4
    VEC 5
    VEC 6
    VEC 7
    VEC 8
    VEC 9
    VEC 10
    VEC 11
    VEC 12
    VEC 13
    VEC 14
    VEC 15
fault_loop:
    bl    led_init
    mov   w0, #7                          // solid WHITE while we dump
    bl    led_rgb
    ldr   x0, =s_fault                    // "\r\n[FAULT]\r\n"
    bl    uart_puts
    ldr   x0, =s_vec                      // which vector slot fired
    mov   w1, w9
    bl    print_reg
    mrs   x10, CurrentEL                  // bits[3:2] = current EL
    lsr   w10, w10, #2
    and   w10, w10, #3
    ldr   x0, =s_el
    mov   w1, w10
    bl    print_reg
    cmp   w10, #2                         // read the syndrome regs of the right EL
    b.lo  flt_el1                         // (mrs esr_el2 in EL1 would itself trap)
    mrs   x11, esr_el2
    mrs   x12, far_el2
    mrs   x13, elr_el2
    b     flt_pr
flt_el1:
    mrs   x11, esr_el1
    mrs   x12, far_el1
    mrs   x13, elr_el1
flt_pr:
    ldr   x0, =s_esr                      // ESR: EC[31:26] = exception class
    mov   w1, w11
    bl    print_reg
    ldr   x0, =s_far                      // FAR: faulting address (data abort)
    mov   w1, w12
    bl    print_reg
    ldr   x0, =s_elr                      // ELR: instruction that faulted
    mov   w1, w13
    bl    print_reg
fl_on:
    mov   w0, #7                          // WHITE (all channels) — fast flutter = fault
    bl    led_rgb
    mov   w3, #0x4000000
fl_d1:
    subs  w3, w3, #1
    b.ne  fl_d1
    mov   w0, #0
    bl    led_rgb
    mov   w3, #0x4000000
fl_d2:
    subs  w3, w3, #1
    b.ne  fl_d2
    b     fl_on
.endif

// ---- UART0 serial debug (PANELDBG) -----------------------------------------
// U-Boot leaves UART0 (0x01C28000, 16550) configured at 115200 8N1, so we just poll
// LSR.THRE (bit5) and write the byte to THR. Lets us print real diagnostics over the
// headphone-jack serial console instead of guessing.
.ifdef PANELDBG
uart_putc:                                // w0 = char
    ldr   x1, =0x01C28014                  // LSR
1:  ldr   w2, [x1]
    tst   w2, #0x20                         // THRE: holding reg empty?
    b.eq  1b
    ldr   x1, =0x01C28000                  // THR
    str   w0, [x1]
    ret
uart_puts:                                // x0 = asciz ptr
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    mov   x19, x0
2:  ldrb  w0, [x19], #1
    cbz   w0, 3f
    bl    uart_putc
    b     2b
3:  ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret
uart_hex:                                 // w0 = value -> 8 hex digits
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    mov   w19, w0
    mov   w20, #28
4:  lsr   w0, w19, w20
    and   w0, w0, #0xF
    cmp   w0, #10
    add   w1, w0, #'0'
    add   w2, w0, #('a' - 10)
    csel  w0, w1, w2, lo
    bl    uart_putc
    subs  w20, w20, #4
    b.ge  4b
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret
// print_reg: x0 = label asciz, w1 = value -> "label=hhhhhhhh\r\n"
print_reg:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    mov   w19, w1
    bl    uart_puts
    mov   w0, #'='
    bl    uart_putc
    mov   w0, w19
    bl    uart_hex
    mov   w0, #0x0D
    bl    uart_putc
    mov   w0, #0x0A
    bl    uart_putc
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret
s_panel: .asciz "\r\n[unodos] panel_init\r\n"
s_sctlr: .asciz "SCTLR"
s_rxctl: .asciz "RXCTL"
s_rxdat: .asciz "RXDAT"
s_plde:  .asciz "PLL_DE"
s_plv0:  .asciz "PLL_VIDEO0"
s_plmi:  .asciz "PLL_MIPI"
s_tcon:  .asciz "TCON0_GCTL"
s_gint:  .asciz "TCON0_GINT0"
s_glbst: .asciz "DE2_GLB_STATUS"
s_dsi0:  .asciz "DSI_BASIC_CTL0"
s_de2:   .asciz "DE2_GLB_CTL"
s_ovl:   .asciz "DE2_OVL_TOPADD"
s_done:  .asciz "[unodos] mainloop\r\n"
s_fphdr:   .asciz "\r\n[frameperiod] cntpct ticks over 60 TCON0 frames:\r\n"
s_fptot:   .asciz "FRAME_TICKS_60"
s_fpfrm:   .asciz "FRAME_TICKS_1"
s_fpstall: .asciz "FRAME_PERIOD=STALL (GINT0 never latched)\r\n"
s_ccufull: .asciz "\r\n[ccu-full] complete CCU sweep 0x01C20000-0x01C202FF:\r\n"
s_pbref: .asciz "\r\n[pboot-ref] live DE2/TCON0/DSI/DPHY/CCU as left by p-boot:\r\n"
s_pmic:  .asciz "[pmic] ok\r\n"
s_dsih:  .asciz "[dsi_host] ok\r\n"
s_dphy:  .asciz "[dphy] ok\r\n"
s_st70:  .asciz "\r\n[st7703] ok\r\n"
s_ystg:  .asciz "[ystage] ok\r\n"
s_rsthi: .asciz "[reset_hi] ok\r\n"
s_stb:   .asciz "[st7703] begin: "
s_fault: .asciz "\r\n[FAULT]\r\n"
s_vec:   .asciz "VEC"
s_el:    .asciz "EL"
s_esr:   .asciz "ESR"
s_far:   .asciz "FAR"
s_elr:   .asciz "ELR"
s_bctl0: .asciz "  BCTL0"
s_cmdct: .asciz "  CMDCTL"
.align 2
.endif

// led_report: x0 = register addr, w1 = bitmask. Reads the register on REAL hardware
// and reports the bit as a long solid flash: GREEN = bit set, RED = bit clear. Used to
// read back status the Unicorn harness can't model (e.g. PLL lock). PANELDBG only.
led_report:
.ifdef PANELDBG
    stp   x29, x30, [sp, #-16]!
    ldr   w2, [x0]
    ands  w2, w2, w1
    mov   w0, #1                          // RED = clear
    b.eq  lrp_show
    mov   w0, #2                          // GREEN = set
lrp_show:
    bl    led_rgb
    mov   w0, #1100
    bl    delay_ms
    mov   w0, #0
    bl    led_rgb
    mov   w0, #500
    bl    delay_ms
    ldp   x29, x30, [sp], #16
.endif
    ret

// led_stage: w0 = COLOUR mask for the block about to run (PANELDBG builds only).
// Flash off briefly, then hold the colour solid (~0.8 s) so it's visible even if the
// block is instant; the block then runs with this colour showing, so whatever colour
// is on when it freezes / flutters identifies the failing block.
led_stage:
.ifdef PANELDBG
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    mov   w19, w0
    mov   w0, #0                          // off (mark transition)
    bl    led_rgb
    mov   w0, #300
    bl    delay_ms
    mov   w0, w19                         // set the block's colour
    bl    led_rgb
    mov   w0, #800                        // hold so it's clearly visible
    bl    delay_ms
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
.endif
    ret

// ============================================================================
// MASTER bring-up sequence
// ============================================================================
panel_init:
    stp   x29, x30, [sp, #-16]!
.ifdef PANELDBG
    // Distinct "payload started" marker: the SPL leaves the LED solid GREEN; we turn
    // it fully OFF and hold ~1.2 s so the start of OUR colour beacon is unambiguous.
    // Each block then lights a distinct colour (held while it runs); the colour on
    // screen when it freezes / flutters identifies the failing block:
    // Distinct colours (no cyan; white reserved for the fault flutter):
    //   BLUE=clocks  RED=PMIC  GREEN=DSI/D-PHY  YELLOW=ST7703  MAGENTA=dsi_start/done
    //   (fast WHITE flutter = a CPU fault in that block)
    bl    led_init
    mov   w0, #0
    bl    led_rgb
    mov   w0, #1200
    bl    delay_ms
    ldr   x0, =s_panel
    bl    uart_puts
    mrs   x1, CurrentEL                   // report cache/MMU state: SCTLR bit2=C(D$),
    lsr   x1, x1, #2                       // bit12=I(I$), bit0=M(MMU). If C is SET the FB
    and   x1, x1, #3                       // writes may be cached -> DE2 reads stale DRAM
    cmp   x1, #2                           // -> dark screen = coherency, not the DSI path.
    b.lt  pi_sctl_el1
    mrs   x1, sctlr_el2
    b     pi_sctl_pr
pi_sctl_el1:
    mrs   x1, sctlr_el1
pi_sctl_pr:
    ldr   x0, =s_sctlr
    bl    print_reg
.endif
    // ===== p-boot display_init() order — see clk_init note above =====
    // tcon0_init FIRST (timing master), with its clocks (PLL_VIDEO0/PLL_MIPI/TCON0 src)
    // brought up immediately before, exactly as p-boot's tcon0_init does internally.
    mov   w0, #4                          // BLUE: clocks + TCON0
    bl    led_stage
    bl    tcon0_clk_init                  // PLL_VIDEO0, PLL_MIPI, TCON0 source clock
    bl    tcon0_init                      // TCON0 timing registers
.ifdef PANELDBG
    ldr   x0, =s_plv0                     // PLL readbacks (bit28 = lock) — now AFTER their setup
    ldr   x1, =CCU+0x10
    ldr   w1, [x1]
    bl    print_reg
    ldr   x0, =s_plmi
    ldr   x1, =CCU+0x40
    ldr   w1, [x1]
    bl    print_reg
    ldr   x0, =s_tcon
    ldr   x1, =TCON0+0x00
    ldr   w1, [x1]
    bl    print_reg                       // TCON0 GCTL (bit31 = enabled)
.endif
    // dsi_init: gate the DSI bus clock, THEN the DSI host register table (p-boot order:
    // dsi_run_init_seq(dsi_init_seq) runs before display_board_init / dphy).
    mov   w0, #2                          // GREEN: DSI host
    bl    led_stage
    bl    dsi_bus_clk_on                  // MIPI-DSI bus gate/reset (just before host regs)
    bl    dsi_host_init                   // MIPI-DSI controller register table
.ifdef PANELDBG
    ldr   x0, =s_dsih
    bl    uart_puts
.endif
    // display_board_init: assert panel reset LOW, power the PMIC rails, settle 15ms —
    // in p-boot this comes AFTER the DSI host regs and BEFORE dphy_enable.
    mov   w0, #1                          // RED: PMIC + reset
    bl    led_stage
    bl    panel_reset_low                 // ST7703 reset asserted (PD23 low)
    bl    rsb_init                        // reach the PMIC
    bl    pmic_init                       // DLDO1/GPIO0LDO 3.3V, DLDO2 1.8V = MIPI power
    mov   w0, #15                         // p-boot udelay(15000) power-settle
    bl    delay_ms
.ifdef PANELDBG
    ldr   x0, =s_pmic
    bl    uart_puts
.endif
    // dphy_enable: its first act is CCU_MIPI_DSI_CLK, then the D-PHY registers/analog.
    bl    dphy_clk_on                     // DSI D-PHY clock (p-boot dphy_enable's first write)
    bl    dphy_init                       // MIPI D-PHY + analog LDO sequence
.ifdef PANELDBG
    ldr   x0, =s_dphy
    bl    uart_puts
.endif
    // deassert panel reset, 15ms, then the ST7703 DCS init stream.
    mov   w0, #3                          // YELLOW: ST7703 panel init
    bl    led_stage
.ifdef PANELDBG
    ldr   x0, =s_ystg
    bl    uart_puts
.endif
    bl    panel_reset_high                // release reset
    mov   w0, #15
    bl    delay_ms
.ifdef PANELDBG
    ldr   x0, =s_rsthi
    bl    uart_puts
.endif
    bl    st7703_init                     // DCS init stream (LP), SLPOUT/120ms/DISPON
.ifdef PANELDBG
    ldr   x0, =s_st70
    bl    uart_puts
    bl    dcs_read_dbg                     // probe: is the panel alive / answering DSI?
                                          // (LP read of power-mode, BEFORE the HS switch)
.endif
    // HS video start, THEN de2_init's clocks (p-boot brings PLL_DE/DE clock up LAST,
    // after sun6i_dsi_start(HSD), right before the mixer). fb_init does the mixer config.
    mov   w0, #5                          // MAGENTA: DSI HS start + DE clocks + backlight
    bl    led_stage
    bl    dsi_start                       // bus -> high-speed video mode
.ifdef PANELDBG
    ldr   x0, =s_dsi0
    ldr   x1, =DSI+0x10
    ldr   w1, [x1]
    bl    print_reg                       // DSI BASIC_CTL0 after HS start
.endif
    bl    de_clk_init                     // PLL_DE, DE clock + bus gate/reset (p-boot de2_init head)
.ifdef PANELDBG
    ldr   x0, =s_plde
    ldr   x1, =CCU+0x48
    ldr   w1, [x1]
    bl    print_reg                       // PLL_DE (bit28 = lock) — now after its setup
.endif
    bl    backlight_on                    // PWM + enable GPIO
    ldp   x29, x30, [sp], #16
    ret

// ============================================================================
// panel_dsi_only — BISECTION variant (build.sh pbootdsi). Re-inits ONLY the DSI link
// (DSI host + D-PHY + panel reset/DCS + dsi_start) on top of p-boot's already-running
// clocks AND TCON0 — i.e. it does NOT touch the PLLs/TCON0/DE clocks. Used under
// PBOOT+NATIVEPANEL+DSIONLY to split the panel_init suspect set: if THIS breaks p-boot's
// working display (black) the bug is in our DSI-host/D-PHY/dsi_start (the leading HS-
// transmit theory — LP works, HS is separate analog); if the launcher survives, our DSI
// link bring-up is fine and the bug is in our clocks/TCON0. p-boot already powered the
// PMIC rails, so PMIC is skipped (no RSB re-init). Mirrors panel_init's DSI portion.
panel_dsi_only:
    stp   x29, x30, [sp, #-16]!
.ifdef PANELDBG
    bl    led_init
    ldr   x0, =s_panel
    bl    uart_puts
.endif
    mov   w0, #2                          // GREEN: DSI host
    bl    led_stage
    bl    dsi_bus_clk_on                  // DSI bus gate/reset (idempotent on p-boot's)
    bl    dsi_host_init                   // re-init the DSI host registers
.ifdef PANELDBG
    ldr   x0, =s_dsih
    bl    uart_puts
.endif
    mov   w0, #1                          // RED: panel reset (PMIC already up from p-boot)
    bl    led_stage
    bl    panel_reset_low
    mov   w0, #15
    bl    delay_ms
    bl    dphy_clk_on                     // DSI D-PHY clock
    bl    dphy_init                       // D-PHY + analog LDO sequence
.ifdef PANELDBG
    ldr   x0, =s_dphy
    bl    uart_puts
.endif
    mov   w0, #3                          // YELLOW: ST7703
    bl    led_stage
    bl    panel_reset_high
    mov   w0, #15
    bl    delay_ms
    bl    st7703_init                     // re-send the panel DCS init stream
.ifdef PANELDBG
    ldr   x0, =s_st70
    bl    uart_puts
    bl    dcs_read_dbg                     // panel still answering? (LP read)
.endif
    mov   w0, #5                          // MAGENTA: DSI HS start
    bl    led_stage
    bl    dsi_start                       // bus -> high-speed video mode
.ifdef PANELDBG
    ldr   x0, =s_dsi0
    ldr   x1, =DSI+0x10
    ldr   w1, [x1]
    bl    print_reg
.endif
    ldp   x29, x30, [sp], #16
    ret

// ============================================================================
// panel_dsistart_only — FINEST bisection (build.sh pbootdsistart). Re-runs ONLY our
// dsi_start (the HS handoff: JUMP_SEL HSC -> clear LANE_CEN -> 1ms -> JUMP_SEL HSD) on
// top of p-boot's FULLY-working DSI link — no panel reset, no DSI-host re-init, no D-PHY
// re-power, so it's the lowest-artifact perturbation possible. Splits the DSI-link
// suspect set: black -> our dsi_start HS handoff is the bug; launcher survives -> it's
// dsi_host_init or dphy_init (build a dphy-only test next). NOTE: no dcs_read here — that
// would flip JUMP_SEL to LPRX and itself disturb the HS state we're testing.
panel_dsistart_only:
    stp   x29, x30, [sp, #-16]!
.ifdef PANELDBG
    bl    led_init
    ldr   x0, =s_panel
    bl    uart_puts
.endif
    mov   w0, #5                          // MAGENTA: DSI HS start
    bl    led_stage
    bl    dsi_start                       // re-issue HSC -> LANE_CEN -> HSD on p-boot's link
.ifdef PANELDBG
    ldr   x0, =s_dsi0
    ldr   x1, =DSI+0x10
    ldr   w1, [x1]
    bl    print_reg
.endif
    ldp   x29, x30, [sp], #16
    ret

// ============================================================================
// panel_dsihost_only — splits the last two suspects (build.sh pbootdsihost). Re-writes
// our DSI HOST register table (dsi_host_init) + re-triggers HS (dsi_start) on top of
// p-boot's live pipe, leaving p-boot's D-PHY analog UNTOUCHED. dsi_start is already
// proven safe to re-run (pbootdsistart = launcher), so the only new variable here is our
// dsi_host_init. Low-artifact (config-register rewrite, not an analog re-power). Outcome:
//   black     -> our dsi_host_init is the bug.
//   launcher  -> dsi_host_init is fine => dphy_init is the bug BY ELIMINATION (which also
//                sidesteps the D-PHY re-power-glitch artifact a direct dphy-only test has).
panel_dsihost_only:
    stp   x29, x30, [sp, #-16]!
.ifdef PANELDBG
    bl    led_init
    ldr   x0, =s_panel
    bl    uart_puts
.endif
    mov   w0, #2                          // GREEN: DSI host
    bl    led_stage
    bl    dsi_bus_clk_on                  // DSI bus gate/reset (idempotent on p-boot's)
    bl    dsi_host_init                   // rewrite our DSI host register table
.ifdef PANELDBG
    ldr   x0, =s_dsih
    bl    uart_puts
.endif
    mov   w0, #5                          // MAGENTA: DSI HS start
    bl    led_stage
    bl    dsi_start                       // re-trigger HS on p-boot's D-PHY
.ifdef PANELDBG
    ldr   x0, =s_dsi0
    ldr   x1, =DSI+0x10
    ldr   w1, [x1]
    bl    print_reg
.endif
    ldp   x29, x30, [sp], #16
    ret

// ============================================================================
// panel_nodphy_only — AIRTIGHT confirmation that dphy_init is the bug (build.sh
// pbootnodphy). Runs the ENTIRE DSI link EXCEPT dphy_clk_on/dphy_init — dsi_host_init +
// panel reset + st7703 (re-DCS) + dsi_start — keeping p-boot's known-good D-PHY analog.
// It's the exact complement of pbootdsi (which was BLACK): the ONLY thing removed is our
// dphy. launcher -> dphy_init is DEFINITIVELY the bug; black -> the breakage is in our
// panel reset / st7703 re-init path, not dphy (would redirect the hunt).
panel_nodphy_only:
    stp   x29, x30, [sp, #-16]!
.ifdef PANELDBG
    bl    led_init
    ldr   x0, =s_panel
    bl    uart_puts
.endif
    mov   w0, #2                          // GREEN: DSI host
    bl    led_stage
    bl    dsi_bus_clk_on
    bl    dsi_host_init
.ifdef PANELDBG
    ldr   x0, =s_dsih
    bl    uart_puts
.endif
    mov   w0, #1                          // RED: panel reset (NO dphy — keep p-boot's)
    bl    led_stage
    bl    panel_reset_low
    mov   w0, #15
    bl    delay_ms
    bl    panel_reset_high
    mov   w0, #15
    bl    delay_ms
    mov   w0, #3                          // YELLOW: ST7703 re-DCS
    bl    led_stage
    bl    st7703_init
.ifdef PANELDBG
    ldr   x0, =s_st70
    bl    uart_puts
    bl    dcs_read_dbg
.endif
    mov   w0, #5                          // MAGENTA: DSI HS start
    bl    led_stage
    bl    dsi_start
.ifdef PANELDBG
    ldr   x0, =s_dsi0
    ldr   x1, =DSI+0x10
    ldr   w1, [x1]
    bl    print_reg
.endif
    ldp   x29, x30, [sp], #16
    ret

// ============================================================================
// panel_st7703_only — splits RESET-artifact vs st7703-transmission (build.sh pbootst7703).
// Re-sends our st7703 DCS init on top of p-boot's ALREADY-LIT panel WITHOUT a panel reset
// (the only artifact-prone step). Our st7703 DATA is proven byte-identical to p-boot
// (cmp_st7703.py: all 20 match), so this isolates the TRANSMISSION/re-DCS act. Outcome:
//   launcher -> st7703 is fine; the pbootnodphy black was the panel-RESET perturbation
//               artifact -> the real cold bug is reset-timing or back in clocks/TCON0/dphy.
//   black    -> our dcs_send transmission corrupts the panel even with correct data.
panel_st7703_only:
    stp   x29, x30, [sp, #-16]!
.ifdef PANELDBG
    bl    led_init
    ldr   x0, =s_panel
    bl    uart_puts
.endif
    mov   w0, #2                          // GREEN: DSI host
    bl    led_stage
    bl    dsi_bus_clk_on
    bl    dsi_host_init
    mov   w0, #3                          // YELLOW: ST7703 re-DCS (NO panel reset)
    bl    led_stage
    bl    st7703_init
.ifdef PANELDBG
    ldr   x0, =s_st70
    bl    uart_puts
    bl    dcs_read_dbg
.endif
    mov   w0, #5                          // MAGENTA: DSI HS start
    bl    led_stage
    bl    dsi_start
.ifdef PANELDBG
    ldr   x0, =s_dsi0
    ldr   x1, =DSI+0x10
    ldr   w1, [x1]
    bl    print_reg
.endif
    ldp   x29, x30, [sp], #16
    ret

// ============================================================================
// panel_dphy_only — the LAST untested block (build.sh pbootdphy). Re-runs ONLY our
// dphy_clk_on + dphy_init + dsi_start on top of p-boot's clocks/TCON0/DSI-host/panel —
// no panel reset, no dsi_host rewrite. Every prior working test used p-boot's D-PHY; this
// is the first to drive OUR D-PHY analog bring-up. launcher -> our dphy is fine => the cold
// bug is in our clocks or TCON0 (a rate issue registers can't show); black -> our dphy_init
// is the bug (modulo a re-power glitch from reconfiguring an already-powered D-PHY: our
// dphy_tbl writes ANA2=0x2, briefly clearing CK_CPU/P2S_CPU, then rebuilds).
panel_dphy_only:
    stp   x29, x30, [sp, #-16]!
.ifdef PANELDBG
    bl    led_init
    ldr   x0, =s_panel
    bl    uart_puts
.endif
    mov   w0, #2                          // GREEN: D-PHY (re-driven by us)
    bl    led_stage
    bl    dphy_clk_on
    bl    dphy_init
.ifdef PANELDBG
    ldr   x0, =s_dphy
    bl    uart_puts
.endif
    mov   w0, #5                          // MAGENTA: DSI HS start
    bl    led_stage
    bl    dsi_start
.ifdef PANELDBG
    ldr   x0, =s_dsi0
    ldr   x1, =DSI+0x10
    ldr   w1, [x1]
    bl    print_reg
.endif
    ldp   x29, x30, [sp], #16
    ret

// ============================================================================
// CCU clocks / PLLs (base 0x01C20000) — DISTRIBUTED in p-boot order
// ============================================================================
// 2026-06-22: after EXHAUSTIVE register-snapshot parity with working p-boot (every
// display-path register byte-identical; PINEPHONE-BRINGUP §8) the panel is still
// backlit-black. The one thing a snapshot cannot capture is the RUNTIME bring-up
// ORDER. We previously front-loaded EVERY clock in one clk_init (plus an assert→
// deassert reset-cycle p-boot never does). p-boot instead brings each block's clock
// up *immediately before* using that block, distributed across display_init:
//   tcon0_init   -> PLL_VIDEO0, PLL_MIPI, TCON0 source clock, LCD0 gate/reset
//   dsi_init     -> DSI bus gate/reset  (just before the DSI host regs)
//   dphy_enable  -> CCU_MIPI_DSI_CLK    (its first act)
//   de2_init     -> PLL_DE, DE clock, DE gate/reset  (LAST, before the mixer)
// These helpers reproduce that exact ordering. Register VALUES are unchanged from the
// snapshot-verified set; only WHEN each clock comes up (and deassert-only resets,
// matching p-boot — the assert-cycle is dropped) differs. panel_init calls them at the
// p-boot-equivalent points. (If this still doesn't light it, we've faithfully tested
// "exact p-boot order" and the next wedge is a DE2-output-vs-DSI-HS-transmit splitter.)

// tcon0_clk_init: p-boot tcon0_init head — PLL_VIDEO0, PLL_MIPI (LDO→settle→full),
// TCON0 source = MIPI_PLL, LCD0 bus gate + reset-off. Run BEFORE tcon0_init's regs.
tcon0_clk_init:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =CCU+0x10                   // PLL_VIDEO0 = 297 MHz
    ldr   w1, =0x81006207
    str   w1, [x0]
    dsb   sy
    mov   w0, #1
    bl    delay_ms
    ldr   x0, =CCU+0x40                   // PLL_MIPI: LDO1/LDO2 enable first
    ldr   w1, =0x00C00000
    str   w1, [x0]
    dsb   sy
    mov   w0, #1                          // p-boot udelay(100); 1ms is amply longer
    bl    delay_ms
    ldr   x0, =CCU+0x40                   // PLL_MIPI: full enable (K=2 N=8 M=11 -> 432 MHz)
    ldr   w1, =0x80C0071A
    str   w1, [x0]
    dsb   sy
    mov   w0, #1
    bl    delay_ms
    ldr   x0, =CCU+0x118                  // TCON0 clock: source = MIPI_PLL, gate on
    ldr   w1, =0x80000000
    str   w1, [x0]
    dsb   sy
    ldr   x0, =CCU+0x64                   // LCD0/TCON0 bus gate (bit 3)
    mov   w1, #0x8
    bl    mmio_setbits
    ldr   x0, =CCU+0x2C4                  // de-assert TCON0 reset (bit 3) — deassert-only
    mov   w1, #0x8
    bl    mmio_setbits
    ldp   x29, x30, [sp], #16
    ret

// dsi_bus_clk_on: p-boot dsi_init head — gate the MIPI-DSI bus clock + reset-off, run
// immediately before the DSI host register table.
dsi_bus_clk_on:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =CCU+0x60                   // MIPI-DSI bus gate (bit 1)
    mov   w1, #0x2
    bl    mmio_setbits
    ldr   x0, =CCU+0x2C0                  // de-assert MIPI-DSI reset (bit 1)
    mov   w1, #0x2
    bl    mmio_setbits
    ldp   x29, x30, [sp], #16
    ret

// dphy_clk_on: p-boot dphy_enable's first act — the DSI D-PHY clock (150 MHz, src
// periph0, M=4). Run immediately before dphy_init.
dphy_clk_on:
    ldr   x0, =CCU+0x168
    ldr   w1, =0x00008203
    str   w1, [x0]
    dsb   sy
    ret

// de_clk_init: p-boot de2_init head — SRAM-C -> DE, PLL_DE, DE special clock + bus
// gate/reset. Run LAST (after the DSI is in HS video mode), right before fb_init's
// mixer config — exactly as p-boot does de2_init after sun6i_dsi_start(HSD).
de_clk_init:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =SRAM_CTRL1                 // map SRAM C1 -> Display Engine: clear ONLY bit24
    mov   w1, #0x01000000                 // (RMW, as p-boot/NuttX — zeroing the whole reg
    bl    mmio_clrbits                     // could disturb other SRAM mappings).
    ldr   x0, =CCU+0x48                   // PLL_DE, poll lock (bit 28)
    ldr   w1, =0x81001701
    str   w1, [x0]
    dsb   sy
    mov   w2, #0x100000
cdk_lock:
    ldr   w1, [x0]
    tst   w1, #0x10000000
    b.ne  cdk_locked
    subs  w2, w2, #1
    b.ne  cdk_lock
cdk_locked:
    ldr   x0, =CCU+0x104                  // DE clock: source = PLL_DE, gate on, div=0 (/1).
    ldr   w1, [x0]                         // Clear divider[3:0] too (a U-Boot-left divider
    ldr   w2, =0x8300000F                 // would starve TCON0 below the pixel rate).
    bic   w1, w1, w2
    ldr   w2, =0x81000000
    orr   w1, w1, w2
    str   w1, [x0]
    dsb   sy
    ldr   x0, =CCU+0x2C4                  // de-assert DE bus reset (bit 12) — deassert-only
    mov   w1, #0x1000
    bl    mmio_setbits
    ldr   x0, =CCU+0x64                   // open DE bus gate (bit 12)
    mov   w1, #0x1000
    bl    mmio_setbits
    ldp   x29, x30, [sp], #16
    ret

// ============================================================================
// Reduced Serial Bus (RSB) driver + AXP803 PMIC rails
// ============================================================================
// rsb_wait: poll RSB_CTRL START_TRANS (bit7) clear, bounded. Leaf.
rsb_wait:
    ldr   x0, =RSB_CTRL
    mov   w2, #0x100000
rsw1:
    ldr   w1, [x0]
    tst   w1, #0x80
    b.eq  rsw_done
    subs  w2, w2, #1
    b.ne  rsw1
rsw_done:
    ret

rsb_init:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =R_PRCM+0x28                // PRCM apb0 gate: PIO(0) | RSB(3)
    mov   w1, #0x9
    bl    mmio_setbits
    ldr   x0, =R_PRCM+0xB0                // PRCM apb0 reset: same bits
    mov   w1, #0x9
    bl    mmio_setbits
    // R_PIO PL0/PL1 -> function 2 (R_RSB)
    ldr   x0, =R_PIO+0x00
    ldr   w1, [x0]
    mov   w2, #0xFF
    bic   w1, w1, w2
    mov   w2, #0x22
    orr   w1, w1, w2
    str   w1, [x0]
    dsb   sy
    ldr   x0, =RSB_CTRL                   // soft reset
    mov   w1, #1
    str   w1, [x0]
    ldr   x0, =RSB_CCR                    // 3 MHz bus clock
    ldr   w1, =0x00000103
    str   w1, [x0]
    ldr   x0, =RSB_DMCR                   // device-mode start
    ldr   w1, =0x807C3E00
    str   w1, [x0]
    dsb   sy
    mov   w2, #0x100000
rsi_dm:
    ldr   w1, [x0]
    tst   w1, #0x80000000
    b.eq  rsi_dmok
    subs  w2, w2, #1
    b.ne  rsi_dm
rsi_dmok:
    // assign the AXP803 its runtime address 0x2D
    ldr   x0, =RSB_DAR
    ldr   w1, =0x002D03A3                 // (0x2D<<16) | hw-addr 0x3A3
    str   w1, [x0]
    ldr   x0, =RSB_CMD
    mov   w1, #0xE8                       // SET_RTSADDR
    str   w1, [x0]
    ldr   x0, =RSB_CTRL
    mov   w1, #0x80                       // START_TRANS
    str   w1, [x0]
    dsb   sy
    bl    rsb_wait
    ldp   x29, x30, [sp], #16
    ret

// rsb_wr8: w0 = PMIC reg, w1 = value (write to runtime addr 0x2D).
rsb_wr8:
    stp   x29, x30, [sp, #-16]!
    mov   w3, w0
    mov   w4, w1
    ldr   x0, =RSB_CMD
    mov   w1, #0x4E                       // BYTE_WRITE
    str   w1, [x0]
    ldr   x0, =RSB_DAR
    movz  w1, #0x2D, lsl #16
    str   w1, [x0]
    ldr   x0, =RSB_AR
    str   w3, [x0]
    ldr   x0, =RSB_DATA
    str   w4, [x0]
    ldr   x0, =RSB_CTRL
    mov   w1, #0x80
    str   w1, [x0]
    dsb   sy
    bl    rsb_wait
    ldp   x29, x30, [sp], #16
    ret

// rsb_rd8: w0 = PMIC reg -> w0 = value.
rsb_rd8:
    stp   x29, x30, [sp, #-16]!
    mov   w3, w0
    ldr   x0, =RSB_CMD
    mov   w1, #0x8B                       // BYTE_READ
    str   w1, [x0]
    ldr   x0, =RSB_DAR
    movz  w1, #0x2D, lsl #16
    str   w1, [x0]
    ldr   x0, =RSB_AR
    str   w3, [x0]
    ldr   x0, =RSB_CTRL
    mov   w1, #0x80
    str   w1, [x0]
    dsb   sy
    bl    rsb_wait
    ldr   x0, =RSB_DATA
    ldr   w0, [x0]
    and   w0, w0, #0xFF
    ldp   x29, x30, [sp], #16
    ret

// rsb_rmw: w0 = PMIC reg, w1 = OR-mask (read-modify-write on the PMIC).
rsb_rmw:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    mov   w19, w0
    mov   w20, w1
    bl    rsb_rd8                         // w0 -> current value
    orr   w1, w0, w20
    mov   w0, w19
    bl    rsb_wr8
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

pmic_init:
    stp   x29, x30, [sp, #-16]!
    mov   w0, #0x15                        // DLDO1 = 3.3 V
    mov   w1, #0x1A
    bl    rsb_wr8
    mov   w0, #0x12                        // power on DLDO1 (bit 3)
    mov   w1, #0x08
    bl    rsb_rmw
    mov   w0, #0x91                        // GPIO0LDO = 3.3 V (touch)
    mov   w1, #0x1A
    bl    rsb_wr8
    mov   w0, #0x90                        // GPIO0 = LDO mode
    mov   w1, #0x03
    bl    rsb_wr8
    mov   w0, #0x16                        // DLDO2 = 1.8 V  (MIPI-DSI power)
    mov   w1, #0x0B
    bl    rsb_wr8
    mov   w0, #0x12                        // power on DLDO2 (bit 4)
    mov   w1, #0x10
    bl    rsb_rmw
    ldp   x29, x30, [sp], #16
    ret

// ============================================================================
// MIPI-DSI host controller (base 0x01CA0000) + DCS transmit
// ============================================================================
dsi_host_init:
    ldr   x0, =dsi_host_tbl
    b     apply_pokes                      // tail-call (returns to panel_init)

dsi_start:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =DSI+0x48                    // start HS clock
    ldr   w1, =0x00000F02
    str   w1, [x0]
    dsb   sy
    ldr   x0, =DSI+0x10                    // set INSTRU_EN (kick HSC)
    mov   w1, #1
    bl    mmio_setbits
    ldr   x0, =DSI+0x20                    // clear LP11 INST_FUNC LANE_CEN (bit4) so the
    mov   w1, #0x10                        // clock lane enters CONTINUOUS high-speed mode --
    bl    mmio_clrbits                     // NuttX does this BEFORE the settle delay, not
    mov   w0, #1                           // after: the continuous HS clock must stabilise
    bl    delay_ms                         // before HS data starts or the panel can't lock.
    ldr   x0, =DSI+0x48                    // start HS data
    ldr   w1, =0x63F07006
    str   w1, [x0]
    dsb   sy
    ldr   x0, =DSI+0x10
    mov   w1, #1
    bl    mmio_setbits
    ldp   x29, x30, [sp], #16
    ret

.ifdef PANELDBG
// dcs_read_dbg: probe whether the PANEL IS ALIVE / receiving DSI. Sends a DCS READ of
// get_power_mode (0x0A) in low-power escape mode and prints the raw DSI CMD_CTL + the
// RX FIFO word. Interpreting the serial output:
//   RXCTL bit25 (RX_FLAG) SET + RXDAT non-zero  -> panel ANSWERED: DSI link + init OK,
//        and bits[15:8] of RXDAT = the power-mode byte (~0x9C = awake + display-on).
//        => the dark screen is downstream (DE2/TCON0 video scanout), NOT the panel.
//   RXCTL bit25 clear + RXDAT 0 (or bit26 RX_OVERFLOW) -> panel SILENT: the DCS never
//        reached it => D-PHY / lane / LP-signalling problem (the analog layer).
// MUST run in LP/command mode (before dsi_start switches the bus to HS video).
dcs_read_dbg:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =DSI+0x200                    // clear RX_OVERFLOW|RX_FLAG|TX_FLAG
    ldr   w1, =0x06000200
    str   w1, [x0]
    dsb   sy
    ldr   x0, =DSI+0x300                    // read-request header: DI=0x06(DCS read),
    ldr   w1, =0x3F000A06                   // d0=0x0A(power mode), d1=0, ECC=0x3F
    str   w1, [x0]
    dsb   sy
    ldr   x0, =DSI+0x200                    // CMD_CTL length = 4-1 = 3 (RMW low byte)
    ldr   w1, [x0]
    mov   w2, #0xFF
    bic   w1, w1, w2
    orr   w1, w1, #3
    str   w1, [x0]
    dsb   sy
    ldr   x0, =DSI+0x48                     // LPRX instruction chain:
    ldr   w1, =0x100700F4                   // LP11->LPDT->DLY->TBA->END
    str   w1, [x0]
    dsb   sy
    ldr   x0, =DSI+0x10                     // kick: toggle INSTRU_EN
    mov   w1, #1
    bl    mmio_clrbits
    ldr   x0, =DSI+0x10
    mov   w1, #1
    bl    mmio_setbits
    ldr   x0, =DSI+0x10                     // wait for the instruction to finish (bounded)
    mov   w3, #0x100000
drd_poll:
    ldr   w4, [x0]
    tst   w4, #1
    b.eq  drd_done
    subs  w3, w3, #1
    b.ne  drd_poll
drd_done:
    ldr   x0, =DSI+0x200                    // CMD_CTL: bit25=RX_FLAG, bit26=RX_OVERFLOW
    ldr   w1, [x0]
    ldr   x0, =s_rxctl
    bl    print_reg
    ldr   x0, =DSI+0x240                    // CMD_RX0: the panel's response word
    ldr   w1, [x0]
    ldr   x0, =s_rxdat
    bl    print_reg
    ldp   x29, x30, [sp], #16
    ret

// dump_regs: x0 -> table of .word MMIO addresses (terminated by 0). Prints "addr=value"
// for each over UART. We verified our register WRITES match p-boot/NuttX byte-for-byte,
// but NOT that they STUCK on real silicon — a write to an under-clocked/gated block is
// silently dropped and reads back wrong (we already hit this once: DE2 GLB_CTL read 0
// until the DE internal gates were enabled). This dumps the actual on-HW readbacks of the
// whole DE2/TCON0/DSI/CCU set so any value != what-we-wrote pinpoints a dropped write.
dump_regs:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    mov   x19, x0
dmr_l:
    ldr   w20, [x19], #4                    // next address (0 terminates)
    cbz   w20, dmr_done
    mov   w0, w20                            // print the address
    bl    uart_hex
    mov   w0, #'='
    bl    uart_putc
    ldr   w1, [x20]                          // read the register
    mov   w0, w1
    bl    uart_hex
    mov   w0, #0x0D
    bl    uart_putc
    mov   w0, #0x0A
    bl    uart_putc
    b     dmr_l
dmr_done:
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// dump_range: x0 = base MMIO addr, w1 = WORD count. Prints "addr=value" for each word.
// Used to sweep a whole contiguous CONFIG block so we catch even registers we don't know to
// name (reserved/undocumented offsets) — the caller must keep the range clear of any FIFO /
// data / RX port that has read side effects.
dump_range:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    mov   x19, x0
    mov   w20, w1
drng_l:
    cbz   w20, drng_done
    mov   w0, w19                            // address (low 32 bits; all < 0x02000000)
    bl    uart_hex
    mov   w0, #'='
    bl    uart_putc
    ldr   w1, [x19]
    mov   w0, w1
    bl    uart_hex
    mov   w0, #0x0D
    bl    uart_putc
    mov   w0, #0x0A
    bl    uart_putc
    add   x19, x19, #4
    sub   w20, w20, #1
    b     drng_l
drng_done:
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// dump_all: the COMPLETE display-register snapshot. Sweeps each block's full config range
// (so an unsampled sub-register can't hide a divergence) then dumps the curated TCON0 + CCU
// lists (which contain CPU-IF data ports we must NOT blind-sweep). All ranges stop before any
// FIFO/data port: DSI ends at 0xEC (0x200 CMD_CTL / 0x240 RX / 0x300 TX excluded); the DE2,
// D-PHY and DE-top blocks have no read-side-effect registers in the swept ranges.
dump_all:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =0x01000000                    // DE top: gates/reset/mux
    mov   w1, #4
    bl    dump_range
    ldr   x0, =0x01100000                    // DE2 MIXER0 global (GLB_CTL..0x1C)
    mov   w1, #8
    bl    dump_range
    ldr   x0, =0x01101000                    // DE2 blender (0x1000..0x10A0, all pipes)
    mov   w1, #41
    bl    dump_range
    ldr   x0, =0x01103000                    // DE2 UI overlay ch1 (0x3000..0x3040)
    mov   w1, #17
    bl    dump_range
    ldr   x0, =0x01CA0000                    // MIPI-DSI host, FULL config range 0x00..0xEC
    mov   w1, #60
    bl    dump_range
    ldr   x0, =0x01CA1000                    // MIPI D-PHY, full 0x00..0x60
    mov   w1, #25
    bl    dump_range
    ldr   x0, =dump_tbl                       // TCON0 (CPU-IF-data-port-safe) + CCU, curated
    bl    dump_regs
    bl    dump_ccu_full                        // NEW STRATEGY #2: the COMPLETE CCU space
    ldp   x29, x30, [sp], #16
    ret

// ============================================================================
// frame-period measurement (PINEPHONE-BRINGUP §8 NEW STRATEGY #1) — the decisive
// rate test. cntpct_el0 is a fixed 24 MHz reference independent of the display
// clock tree. Count cntpct ticks across N TCON0 frames (GINT0 bit11 = frame-done /
// vblank latch) to get the ACTUAL frame period. Run in native (our TCON0) AND in
// PBOOT (p-boot's TCON0); if the periods differ, a clock divider/PLL is at the wrong
// RATE despite matching register bits (chase PLL_VIDEO0->PLL_MIPI->TCON0 DCLK). If
// identical, the bug is NOT a clock rate -> redirect to TCON0 video/CPU-IF logic or
// a DSI<->TCON0 sync detail. Non-disruptive: pure reads of GINT0 + the timer.
// ----------------------------------------------------------------------------
// wait_frame_edge: spin until TCON0 GINT0 bit11 latches, then clear it. Bounded
// ~50 ms by cntpct so a stalled TCON0 can't hang the diagnostic. Returns w0=1 (edge
// seen) / w0=0 (timeout). No calls (leaf).
wait_frame_edge:
    mrs   x2, cntpct_el0
    ldr   x3, =1200000                        // 50 ms @ 24 MHz (>> one 60 Hz frame)
    add   x2, x2, x3                           // deadline
    ldr   x1, =TCON0+0x04
wfe_spin:
    ldr   w0, [x1]
    tst   w0, #(1<<11)
    b.ne  wfe_edge
    mrs   x4, cntpct_el0
    cmp   x4, x2
    b.lo  wfe_spin
    mov   w0, #0                              // timeout: TCON0 not latching frames
    ret
wfe_edge:
    str   wzr, [x1]                           // clear latch so the next set is a fresh edge
    dsb   sy
    mov   w0, #1
    ret

.equ MFP_FRAMES, 60
// measure_frame_period: time MFP_FRAMES TCON0 frames against cntpct; print total
// ticks and ticks/frame (the offline native-vs-pboot comparison). "STALL" if GINT0
// never latches (would mean TCON0 isn't scanning — but we've confirmed it does).
measure_frame_period:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    ldr   x0, =s_fphdr
    bl    uart_puts
    bl    wait_frame_edge                      // align to a frame boundary first
    cbz   w0, mfp_stall
    mrs   x19, cntpct_el0                      // start tick
    mov   w20, #MFP_FRAMES
mfp_l:
    bl    wait_frame_edge
    cbz   w0, mfp_stall
    subs  w20, w20, #1
    b.ne  mfp_l
    mrs   x0, cntpct_el0
    sub   x19, x0, x19                         // total ticks across MFP_FRAMES frames
    ldr   x0, =s_fptot
    mov   w1, w19
    bl    print_reg
    mov   x0, x19
    mov   x1, #MFP_FRAMES
    udiv  x20, x0, x1                          // ticks per single frame
    ldr   x0, =s_fpfrm
    mov   w1, w20
    bl    print_reg
    b     mfp_done
mfp_stall:
    ldr   x0, =s_fpstall
    bl    uart_puts
mfp_done:
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// dump_ccu_full (NEW STRATEGY #2): sweep the COMPLETE CCU register space
// 0x01C20000-0x01C202FF (every PLL / gate / divider / mux), not just the curated
// dump_tbl subset. Diff p-boot (pbootdbg pboot-ref dump) vs native (paneldbg mainloop
// dump) to catch a clock register we never compared. CCU regs have no read side
// effects, so a blind sweep is safe.
dump_ccu_full:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =s_ccufull
    bl    uart_puts
    ldr   x0, =0x01C20000
    mov   w1, #192                            // 0x300 bytes / 4 words
    bl    dump_range
    ldp   x29, x30, [sp], #16
    ret

.align 2
// Curated TCON0 + CCU table for dump_all (dump_range sweeps DSI/DPHY/DE2/DE-top contiguously;
// these two blocks contain CPU-IF data ports / a sparse clock map we must NOT blind-sweep).
// dump_regs prints "addr=value"; the offline diff aligns by address.
dump_tbl:
    // --- TCON0 (0x01C0C000): timing master. Curated to skip the CPU-IF read-data ports
    //     (0x64/0x68/0x6C), which would trigger a CPU-IF read transaction. ---
    .word 0x01C0C000   // GCTL  (bit31 enable)
    .word 0x01C0C004   // GINT0 (frame/vblank status)
    .word 0x01C0C008   // GINT1
    .word 0x01C0C040   // CTL   (bit31 enable | IF select)
    .word 0x01C0C044   // DCLK  (clock divider)
    .word 0x01C0C048   // BASIC0 (active size)
    .word 0x01C0C060   // CPU_IF (8080 mode / TRI)
    .word 0x01C0C088   // IO_POL
    .word 0x01C0C08C   // IO_TRI (tristate)
    .word 0x01C0C0F4   // TCON1 IO_TRI (parked)
    .word 0x01C0C0F8   // ECC_FIFO
    .word 0x01C0C160   // TRI0
    .word 0x01C0C164   // TRI1 (high half = live block counter)
    .word 0x01C0C168   // TRI2
    .word 0x01C0C1F0   // SAFE_PERIOD
    // --- CCU clock tree (0x01C20000): PLLs, the DE divider, bus gates (REG0 has the DSI gate
    //     at bit1) + resets. The prime "environmental" suspects. ---
    .word 0x01C20010   // PLL_VIDEO0_CTRL
    .word 0x01C20028   // PLL_PERIPH0_CTRL
    .word 0x01C20040   // PLL_MIPI_CTRL   (the DSI/panel pixel clock source)
    .word 0x01C20048   // PLL_DE_CTRL
    .word 0x01C20060   // BUS_CLK_GATE0   (MIPI-DSI = bit1)
    .word 0x01C20064   // BUS_CLK_GATE1   (TCON0=bit3 / DE=bit12)
    .word 0x01C20104   // DE_CLK_REG      (source sel [26:24] + divider [3:0])
    .word 0x01C20118   // TCON0_CLK_REG
    .word 0x01C2015C   // MBUS_CLK_REG
    .word 0x01C20168   // DSI_DPHY_CLK_REG
    .word 0x01C202C0   // BUS_SOFT_RST0   (MIPI-DSI = bit1)
    .word 0x01C202C4   // BUS_SOFT_RST1   (TCON0=bit3 / DE=bit12)
    .word 0x01C202C8   // BUS_SOFT_RST2
    .word 0
.endif

// st7703_init: walk the precomputed dsi_init_seq blob, sending each DCS packet.
st7703_init:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
.ifdef PANELDBG
    ldr   x0, =s_stb                       // "[st7703] begin: " then a dot per command
    bl    uart_puts
.endif
    ldr   x19, =dsi_init_seq
sti_l:
    // Read len/delay as BYTE pairs, not ldrh: a DCS packet can be an odd number of
    // bytes (e.g. SETMIPI = 35 B), so after `x19 += 4 + len` the pointer lands on an
    // odd address. A halfword load (ldrh) on an odd address alignment-faults with
    // SCTLR.A enabled (this was the cmd-2 crash: ESR EC=0x25, DFSC=0x21). Byte reads
    // are alignment-agnostic, so the walker tolerates any packet length.
    ldrb  w0, [x19]                        // packet length (lo)
    ldrb  w1, [x19, #1]                    // packet length (hi)
    orr   w0, w0, w1, lsl #8
    cbz   w0, sti_done
    ldrb  w20, [x19, #2]                   // delay-ms after this packet (lo)
    ldrb  w1, [x19, #3]                    // delay-ms (hi)
    orr   w20, w20, w1, lsl #8
    mov   w2, w0                           // length (saved before any debug prints clobber w0)
    add   x1, x19, #4                      // -> packet bytes
.ifdef PANELDBG
    mov   w0, #'.'                          // one dot per DCS command attempted
    bl    uart_putc
.endif
    bl    dcs_send
    ldrb  w0, [x19]                        // advance: x19 += 4 + len (re-read, byte pair)
    ldrb  w1, [x19, #1]
    orr   w0, w0, w1, lsl #8
    add   x19, x19, #4
    add   x19, x19, w0, uxtw
    cbz   w20, sti_l
    mov   w0, w20
    bl    delay_ms
    b     sti_l
sti_done:
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// dcs_send: x1 -> packet bytes, w2 = length. Spill into the DSI TX FIFO, trigger,
// poll for completion (INSTRU_EN self-clears). Bounded poll. Clobbers x0,w3-w9.
dcs_send:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =DSI+0x200                   // clear RX_OVERFLOW(26)|RX_FLAG(25)|TX_FLAG(9)
    ldr   w3, =0x06000200                  // (was missing RX_FLAG bit25 -> 2nd DCS faulted)
    str   w3, [x0]
    dsb   sy
    ldr   x4, =DSI+0x300                    // TX FIFO window
    mov   w5, #0                            // byte index
dcw_word:
    cmp   w5, w2
    b.hs  dcw_spilled
    mov   w6, #0                            // word accumulator
    mov   w7, #0                            // bit shift
    mov   w8, #0                            // bytes packed
dcw_byte:
    cmp   w5, w2
    b.hs  dcw_emit
    ldrb  w9, [x1, w5, uxtw]
    lsl   w9, w9, w7
    orr   w6, w6, w9
    add   w7, w7, #8
    add   w5, w5, #1
    add   w8, w8, #1
    cmp   w8, #4
    b.lo  dcw_byte
dcw_emit:
    str   w6, [x4], #4
    dsb   sy
    b     dcw_word
dcw_spilled:
    ldr   x0, =DSI+0x200                    // TX length = len-1 in bits[7:0] (RMW)
    ldr   w3, [x0]
    mov   w6, #0xFF
    bic   w3, w3, w6
    sub   w6, w2, #1
    and   w6, w6, #0xFF
    orr   w3, w3, w6
    str   w3, [x0]
    dsb   sy
    ldr   x0, =DSI+0x48                     // sequencer: LPDT -> END
    ldr   w3, =0x000F0004
    str   w3, [x0]
    dsb   sy
    ldr   x0, =DSI+0x10                     // kick: toggle INSTRU_EN
    mov   w1, #1
    bl    mmio_clrbits
    ldr   x0, =DSI+0x10
    mov   w1, #1
    bl    mmio_setbits
    ldr   x0, =DSI+0x10                     // poll INSTRU_EN clear (bounded)
    mov   w3, #0x100000
dcw_poll:
    ldr   w4, [x0]
    tst   w4, #1
    b.eq  dcw_done
    subs  w3, w3, #1
    b.ne  dcw_poll
dcw_done:
.ifdef PANELDBG
    ldr   x0, =DSI+0x10                     // BASIC_CTL0 after this cmd: bit0 set = the LP
    ldr   w1, [x0]                          // transmit instruction NEVER completed (engine
    ldr   x0, =s_bctl0                      // wedged) -> the very next cmd's FIFO write faults
    bl    print_reg
    ldr   x0, =DSI+0x200                    // CMD_CTL: RX_OVERFLOW(26)/RX_FLAG(25)/TX_FLAG(9)
    ldr   w1, [x0]                          // status of the transfer just attempted
    ldr   x0, =s_cmdct
    bl    print_reg
.endif
    ldp   x29, x30, [sp], #16
    ret

// ============================================================================
// MIPI D-PHY (base 0x01CA1000)
// ============================================================================
dphy_init:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =dphy_tbl
    bl    apply_pokes                       // TX-control + timing + initial analog
    mov   w0, #1
    bl    delay_ms
    ldr   x0, =DPHY+0x58                     // ANA3: enable LDOR/LDOC/LDOD
    ldr   w1, =0x03040000
    str   w1, [x0]
    dsb   sy
    mov   w0, #1
    bl    delay_ms
    ldr   x0, =DPHY+0x58                     // ANA3: ENABLEVTTC
    ldr   w1, =0xF8000000
    bl    mmio_setbits
    mov   w0, #1
    bl    delay_ms
    ldr   x0, =DPHY+0x58                     // ANA3: ENABLEDIV
    ldr   w1, =0x04000000
    bl    mmio_setbits
    mov   w0, #1
    bl    delay_ms
    ldr   x0, =DPHY+0x54                     // ANA2: ENABLECKCPU
    mov   w1, #0x10
    bl    mmio_setbits
    mov   w0, #1
    bl    delay_ms
    ldr   x0, =DPHY+0x50                     // ANA1: VTTMODE
    ldr   w1, =0x80000000
    bl    mmio_setbits
    ldr   x0, =DPHY+0x54                     // ANA2: ENABLEP2SCPU
    ldr   w1, =0x0F000000
    bl    mmio_setbits
    ldp   x29, x30, [sp], #16
    ret

// ============================================================================
// TCON0 timing controller (base 0x01C0C000)
// ============================================================================
tcon0_init:
    ldr   x0, =tcon0_tbl
    b     apply_pokes                        // tail-call

// ============================================================================
// panel reset (PD23, active-low) + backlight (PWM PL10 + enable PH10)
// ============================================================================
// Leaf routines (inline the RMW rather than bl mmio_* — they must not clobber LR).
panel_reset_low:
    ldr   x0, =PIO+0x74                       // PD CFG2: PD23 field [30:28] = output
    ldr   w1, [x0]
    ldr   w2, =0x70000000
    bic   w1, w1, w2
    ldr   w2, =0x10000000
    orr   w1, w1, w2
    str   w1, [x0]
    dsb   sy
    ldr   x0, =PIO+0x7C                        // PD DAT: clear PD23 (assert reset)
    ldr   w1, [x0]
    ldr   w2, =0x00800000
    bic   w1, w1, w2
    str   w1, [x0]
    dsb   sy
    ret
panel_reset_high:
    ldr   x0, =PIO+0x7C                        // PD DAT: set PD23 (release reset)
    ldr   w1, [x0]
    ldr   w2, =0x00800000
    orr   w1, w1, w2
    str   w1, [x0]
    dsb   sy
    ret

backlight_on:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =R_PIO+0x04                      // PL10 -> PWM function (R_PIO CFG1 [11:8]=2)
    ldr   w1, [x0]
    mov   w2, #0x700
    bic   w1, w1, w2
    mov   w2, #0x200
    orr   w1, w1, w2
    str   w1, [x0]
    dsb   sy
    ldr   x0, =R_PWM+0x00                       // gate PWM off while configuring
    mov   w1, #0x40
    bl    mmio_clrbits
    ldr   x0, =R_PWM+0x04                       // period 1199 cyc, ~90% duty
    ldr   w1, =0x04AF0437
    str   w1, [x0]
    dsb   sy
    ldr   x0, =R_PWM+0x00                       // enable: gating|EN|prescale 0xF
    mov   w1, #0x5F
    str   w1, [x0]
    dsb   sy
    ldr   x0, =PIO+0x100                        // PH CFG1 (port H @ 0x01C20900): PH10 field
    ldr   w1, [x0]                              // [11:8] = output. (Was PIO+0xA0 = PE_DAT and
    mov   w2, #0xF00                            // PIO+0xCC = PF region -> PH10, the AP3127
    bic   w1, w1, w2                            // backlight-enable GPIO, was never driven, so
    mov   w2, #0x100                            // the backlight stayed OFF = screen fully dark.)
    orr   w1, w1, w2
    str   w1, [x0]
    dsb   sy
    ldr   x0, =PIO+0x10C                        // PH DAT: drive PH10 high (backlight on)
    mov   w1, #0x400
    bl    mmio_setbits
    ldp   x29, x30, [sp], #16
    ret

// ============================================================================
// register tables (addr, val pairs; addr==0 terminates)
// ============================================================================
.align 2
dsi_host_tbl:
    .word DSI+0x00, 0x00000001          // DSI_CTL: enable
    .word DSI+0x10, 0x00030000          // BASIC_CTL0: CRC_En|ECC_En
    .word DSI+0x60, 0x0000000A          // TRANS_START
    .word DSI+0x78, 0x00000000          // TRANS_ZERO
    .word DSI+0x20, 0x0000001F          // INST_FUNC0  LP11 (4 lanes)
    .word DSI+0x24, 0x10000001          // INST_FUNC1  TBA
    .word DSI+0x28, 0x20000010          // INST_FUNC2  HSC
    .word DSI+0x2C, 0x2000000F          // INST_FUNC3  HSD
    .word DSI+0x30, 0x30100001          // INST_FUNC4  LPDT
    .word DSI+0x34, 0x40000010          // INST_FUNC5  HSCEXIT
    .word DSI+0x38, 0x0000000F          // INST_FUNC6  NOP
    .word DSI+0x3C, 0x5000001F          // INST_FUNC7  DLY
    .word DSI+0x4C, 0x00560001          // INST_JUMP_CFG
    .word DSI+0x2F8, 0x000000FF         // DEBUG_DATA
    .word DSI+0x14, 0x00005BC7          // BASIC_CTL1: video mode, start delay 1468
    .word DSI+0x7C, 0x10000007          // TCON_DRQ
    .word DSI+0x40, 0x30000002          // INST_LOOP_SEL
    .word DSI+0x44, 0x00310031          // INST_LOOP_NUM0
    .word DSI+0x54, 0x00310031          // INST_LOOP_NUM1
    .word DSI+0x90, 0x1308703E          // PIXEL_PH: DT 0x3E, WC=2160 (720*3)
    .word DSI+0x98, 0x0000FFFF          // PIXEL_PF0
    .word DSI+0x9C, 0xFFFFFFFF          // PIXEL_PF1
    .word DSI+0x80, 0x00010008          // PIXEL_CTL0: RGB888
    .word DSI+0x0C, 0x00000000          // BASIC_CTL
    .word DSI+0xB0, 0x12000021          // SYNC_HSS
    .word DSI+0xB4, 0x01000031          // SYNC_HSE
    .word DSI+0xB8, 0x07000001          // SYNC_VSS
    .word DSI+0xBC, 0x14000011          // SYNC_VSE
    .word DSI+0x18, 0x0011000A          // BASIC_SIZE0: VSA=10, VBP=17
    .word DSI+0x1C, 0x05CD05A0          // BASIC_SIZE1: VACT=1440, VT=1485
    .word DSI+0xC0, 0x09004A19          // H-blank HSA0
    .word DSI+0xC4, 0x50B40000          // HSA1
    .word DSI+0xC8, 0x35005419          // HBP0
    .word DSI+0xCC, 0x757A0000          // HBP1
    .word DSI+0xD0, 0x09004A19          // HFP0
    .word DSI+0xD4, 0x50B40000          // HFP1
    .word DSI+0xE0, 0x0C091A19          // HBLK0
    .word DSI+0xE4, 0x72BD0000          // HBLK1
    .word DSI+0xE8, 0x1A000019          // V-blank VBLK0
    .word DSI+0xEC, 0xFFFF0000          // VBLK1
    .word 0, 0

.align 2
dphy_tbl:
    .word DPHY+0x04, 0x10000000         // TX_CTL
    .word DPHY+0x10, 0x0A06000E         // TX_TIME0
    .word DPHY+0x14, 0x0A033207         // TX_TIME1
    .word DPHY+0x18, 0x0000001E         // TX_TIME2
    .word DPHY+0x1C, 0x00000000         // TX_TIME3
    .word DPHY+0x20, 0x00000303         // TX_TIME4
    .word DPHY+0x00, 0x00000031         // GCTL: enable
    .word DPHY+0x4C, 0x9F007F00         // ANA0
    .word DPHY+0x50, 0x17000000         // ANA1
    .word DPHY+0x5C, 0x01F01555         // ANA4
    .word DPHY+0x54, 0x00000002         // ANA2
    .word 0, 0

.align 2
tcon0_tbl:
    .word TCON0+0x00, 0x00000000        // GCTL: disable
    .word TCON0+0x04, 0x00000000        // GINT0
    .word TCON0+0x08, 0x00000000        // GINT1
    .word TCON0+0x8C, 0xFFFFFFFF        // IO_TRI: park TCON0 outputs
    .word TCON0+0xF4, 0xFFFFFFFF        // TCON1 IO_TRI: park (unused)
    .word TCON0+0x44, 0x80000006        // DCLK: enable, MIPI-PLL / 6
    .word TCON0+0x40, 0x81000000        // CTL: enable, IF=8080(DSI), src=DE0
    .word TCON0+0x48, 0x02CF059F        // BASIC0: X=719, Y=1439
    .word TCON0+0xF8, 0x00000008        // ECC_FIFO
    .word TCON0+0x60, 0x10010005        // CPU_IF: 24-bit DSI, flush, trigger
    .word TCON0+0x160, 0x002F02CF       // TRI0: block space 47, size 719
    .word TCON0+0x164, 0x0000059F       // TRI1: block num 1439
    .word TCON0+0x168, 0x1BC2000A       // TRI2: start delay 7106
    .word TCON0+0x1F0, 0x0BB80003       // SAFE_PERIOD: FIFO_NUM(3000=0xBB8)<<16 | MODE(3).
                                        // Was 0x00BB8003 (0xBB8 a nibble too low) -> the
                                        // frame-transfer trigger never fired -> panel is
                                        // display-ON (DCS read = 0x1C) but gets no video.
    .word TCON0+0x8C, 0xE0000000        // IO_TRI: drive outputs
    .word TCON0+0x00, 0x80000000        // GCTL: enable
    .word 0, 0
