// ============================================================================
// usb.inc.s — DWC2 (Synopsys DesignWare OTG) USB host + HID boot keyboard
// ----------------------------------------------------------------------------
// Pi 3 (BCM2837) wiring: the SoC's single DWC2 port feeds the onboard SMSC
// LAN9514 USB hub, so a keyboard is ALWAYS one hub-hop downstream and (being a
// low/full-speed device behind a high-speed hub) needs SPLIT transactions.
//
// Bring-up flow:
//   usb_init  -> core soft-reset, force host mode, DMA, power+reset root port
//   usb_enum  -> enumerate the LAN9514 hub (addr 1, direct), power+reset its
//                ports, find the keyboard, enumerate it (addr 2, via split),
//                select HID boot protocol
//   usb_poll  -> once per frame: interrupt-IN poll of the keyboard's endpoint,
//                decode the 8-byte boot report -> pad bits (same bits as UART)
//
// Everything is DMA-mode on host channel 0 (we run one transfer at a time, so a
// single channel is enough). All MMIO polls have timeouts so a missing/odd
// device can never hang the desktop; usb_ok gates usb_poll and the UART input
// path stays available as a fallback. The harness models the DWC2 register file
// + the hub + a single HID keyboard, so this whole state machine is exercised
// headlessly; split-retry/NYET timing is the metal-only part.
// ============================================================================

// ---- serial debug log over the PL011 UART0 TX (GPIO14) ---------------------
// Boot-time diagnostics for real-HW USB bring-up. The harness's FR model never
// sets TXFF, so these never block there. Open a 115200 8N1 terminal on the USB-
// TTL adapter to read them.
// uart_tx_init: make sure the PL011 is enabled for TX/RX (don't rely on the
// firmware). Preserves the firmware's baud (IBRD/FBRD/LCRH untouched). Leaf.
uart_tx_init:
    ldr   x0, =UART0_CR
    ldr   w1, [x0]
    orr   w1, w1, #0x1                     // UARTEN
    orr   w1, w1, #0x100                   // TXE
    orr   w1, w1, #0x200                   // RXE
    str   w1, [x0]
    ret
dbg_putc:                                 // w0 = byte to transmit. Leaf.
    ldr   x1, =UART0_FR
    mov   w3, #0x10000                     // timeout: a stuck/off UART must NOT hang boot
dpc_w:
    ldr   w2, [x1]
    tst   w2, #0x20                        // TXFF: transmit FIFO full?
    b.eq  dpc_tx
    subs  w3, w3, #1
    b.ne  dpc_w
    ret                                    // gave up (TX not draining) — drop the byte
dpc_tx:
    ldr   x1, =UART0_DR
    str   w0, [x1]
    ret
dbg_puts:                                 // x0 = NUL-terminated string.
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    mov   x19, x0
dps_l:
    ldrb  w0, [x19], #1
    cbz   w0, dps_d
    bl    dbg_putc
    b     dps_l
dps_d:
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret
dbg_hex32:                                // w0 = value -> 8 hex digits.
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    mov   w19, w0
    mov   w20, #28
dh_l:
    lsr   w0, w19, w20
    and   w0, w0, #0xF
    cmp   w0, #10
    b.lo  dh_dig
    add   w0, w0, #7                       // 'A'..'F'
dh_dig:
    add   w0, w0, #'0'
    bl    dbg_putc
    subs  w20, w20, #4
    b.ge  dh_l
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// LOG msg            -> print a string literal label
// LOGV msg           -> print label, then the value previously stashed in w9 as
//                       hex, then CRLF (w9 survives dbg_* which only touch x0-x2,
//                       x19-x20).
.macro LOG msg
    ldr   x0, =\msg
    bl    dbg_puts
.endm
.macro LOGV msg
    ldr   x0, =\msg
    bl    dbg_puts
    mov   w0, w9
    bl    dbg_hex32
    ldr   x0, =d_crlf
    bl    dbg_puts
.endm

// usb_delay: busy-wait w0 microseconds off the 1MHz system timer (harness fast-
// forwards the timer, so this is instant there; real HW genuinely waits). Leaf.
usb_delay:
    ldr   x3, =SYS_TIMER_CLO
    ldr   w1, [x3]
    add   w1, w1, w0
ud1:
    ldr   w2, [x3]
    cmp   w2, w1
    b.lo  ud1
    ret

// usb_setup8: build the 8-byte SETUP packet at usb_setup.
//  w0=bmRequestType w1=bRequest w2=wValue w3=wIndex w4=wLength. Leaf.
usb_setup8:
    ldr   x5, =usb_setup
    strb  w0, [x5, #0]
    strb  w1, [x5, #1]
    strb  w2, [x5, #2]
    lsr   w6, w2, #8
    strb  w6, [x5, #3]
    strb  w3, [x5, #4]
    lsr   w6, w3, #8
    strb  w6, [x5, #5]
    strb  w4, [x5, #6]
    lsr   w6, w4, #8
    strb  w6, [x5, #7]
    ret

// uc_fire: (re)enable host channel 0 with the prebuilt HCCHAR value in w19 and
// poll HCINT0 until the "done" bit in w20 sets (ACK for a start-split, XFRC for a
// complete/non-split). Returns w0 = 0 done | 1 STALL/hard-error/timeout |
// 2 NAK/NYET ; w1 = the final HCINT0 value (so the caller can tell NAK vs NYET).
// Leaf (no calls), so usb_chan can call it repeatedly for the split handshake.
uc_fire:
    ldr   x2, =USB_HC0INT
    movn  w3, #0
    str   w3, [x2]                         // clear all int flags
    ldr   x3, =USB_HC0CHAR
    str   w19, [x3]                        // (re)enable the channel
    ldr   w4, =0x200000                    // timeout iterations
uf_poll:
    ldr   w1, [x2]
    tst   w1, w20                          // the phase's "done" bit (ACK or XFRC)
    b.ne  uf_ok
    tst   w1, #HCINT_STALL
    b.ne  uf_err
    tst   w1, #HCINT_NAK
    b.ne  uf_nak
    tst   w1, #HCINT_NYET
    b.ne  uf_nak
    tst   w1, #HCINT_ERRS
    b.ne  uf_err
    subs  w4, w4, #1
    b.ne  uf_poll
uf_err:
    mov   w0, #1
    ret
uf_nak:
    mov   w0, #2
    ret
uf_ok:
    mov   w0, #0
    ret

// usb_chan: run ONE transaction on host channel 0 (DMA), polling to completion.
//  w0=epnum  w1=dir(1=IN)  w2=eptype(0 ctrl,3 intr)  w3=pid  w4=buf(phys) w5=len
//  device context read from cur_addr/cur_mps/cur_lspd/cur_split (+kbd_hub/port).
//  When cur_split is set (device behind the LAN9514 hub) the transaction is a
//  two-phase split: START-SPLIT (await ACK) then COMPLETE-SPLIT (await XFRC,
//  retrying while the hub answers NYET). Returns w0 = 0 ok | 1 error | 2 NAK.
usb_chan:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    stp   x21, x22, [sp, #-16]!
    mov   w21, w0                          // epnum
    mov   w9,  w1                          // dir (1=IN)
    mov   w10, w2                          // eptype
    mov   w11, w3                          // pid
    mov   w12, w4                          // buf phys
    mov   w22, w5                          // len
    // ---- HCTSIZ0 = (pid<<29) | (pktcnt<<19) | xfersize
    ldr   x0, =cur_mps
    ldr   w15, [x0]                        // mps
    cbnz  w22, uc_pc
    mov   w2, #1                           // len 0 -> 1 packet
    b     uc_tsz
uc_pc:
    add   w2, w22, w15
    sub   w2, w2, #1
    udiv  w2, w2, w15                      // pktcnt = ceil(len/mps)
uc_tsz:
    lsl   w2, w2, #19
    orr   w2, w2, w22                      // | xfersize
    lsl   w3, w11, #29
    orr   w2, w2, w3                       // | pid
    ldr   x0, =USB_HC0TSIZ
    str   w2, [x0]
    // ---- HCDMA0 = buf | BUS_ALIAS
    ldr   w0, =BUS_ALIAS
    orr   w1, w12, w0
    ldr   x0, =USB_HC0DMA
    str   w1, [x0]
    // ---- build HCCHAR0 (with CHENA) into w19, reused for every fire
    mov   w1, w15                          // mps (bits10:0)
    lsl   w2, w21, #11                     // epnum<<11
    orr   w1, w1, w2
    cbz   w9, uc_dirout
    ldr   w2, =HCC_EPDIR_IN
    orr   w1, w1, w2
uc_dirout:
    ldr   x0, =cur_lspd
    ldr   w2, [x0]
    cbz   w2, uc_nols
    ldr   w2, =HCC_LSPD
    orr   w1, w1, w2
uc_nols:
    lsl   w2, w10, #18                     // eptype<<18
    orr   w1, w1, w2
    mov   w2, #0x100000                    // MC/EC = 1 (bit20)
    orr   w1, w1, w2
    ldr   x0, =cur_addr
    ldr   w2, [x0]
    lsl   w2, w2, #22                      // devaddr<<22
    orr   w1, w1, w2
    ldr   w2, =HCC_CHENA
    orr   w1, w1, w2
    mov   w19, w1                          // w19 = HCCHAR | CHENA
    // ---- split vs direct
    ldr   x0, =cur_split
    ldr   w0, [x0]
    cbz   w0, uc_direct
    // Build the START-SPLIT register value once into w11 (w11/w12 are free here:
    // pid/buf were already consumed into HCTSIZ/HCDMA above). XactErr on a split
    // is the spec's retry signal, so wrap the whole SS+CS in a retry loop.
    ldr   x0, =kbd_hub
    ldr   w1, [x0]
    lsl   w1, w1, #7                       // HUBADDR (bits13:7)
    ldr   x0, =kbd_port
    ldr   w2, [x0]
    orr   w1, w1, w2                       // | PRTADDR (bits6:0)
    mov   w2, #0xC000                      // XACTPOS = 3 (all)
    orr   w1, w1, w2
    ldr   w2, =HCSPLT_ENA
    orr   w11, w1, w2                      // w11 = START-SPLIT value
    mov   w12, #40                         // whole-transaction retry budget (NAK/XactErr)
uc_try:
    ldr   x0, =USB_HC0SPLT
    str   w11, [x0]                        // START-SPLIT (COMPLSPLT clear)
    mov   w20, #HCINT_ACK
    bl    uc_fire
    ldr   x2, =usb_ssint                   // stash SS HCINT for serial debug
    str   w1, [x2]
    cbz   w0, uc_ss_ok                     // ACK -> do the complete-split
    // SS failed (XactErr etc): retry the whole transaction after a microframe
    subs  w12, w12, #1
    b.eq  uc_fail
    mov   w0, #200
    bl    usb_delay
    b     uc_try
uc_ss_ok:
    ldr   x0, =USB_HC0SPLT
    ldr   w1, =HCSPLT_COMPL
    orr   w1, w11, w1
    str   w1, [x0]                         // COMPLETE-SPLIT
    mov   w20, #HCINT_XFRC
    mov   w13, #32                         // NYET retry budget within this attempt
uc_cs:
    bl    uc_fire
    ldr   x2, =usb_csint                   // stash CS HCINT for serial debug
    str   w1, [x2]
    cbz   w0, uc_ok
    cmp   w0, #2                           // NAK/NYET?
    b.ne  uc_cs_err
    tst   w1, #HCINT_NYET                  // NYET -> complete-split not ready, re-issue CS
    b.ne  uc_cs_nyet
    // plain NAK: the device wasn't ready. For a CONTROL transfer (eptype 0) the
    // spec says retry the whole split transaction; for an interrupt poll a NAK
    // just means "no key this frame".
    cmp   w10, #0
    b.ne  uc_nak                           // interrupt etc -> no data
    b     uc_cs_err                        // control -> retry whole transaction
uc_cs_nyet:
    subs  w13, w13, #1
    b.ne  uc_cs
uc_cs_err:
    // CS errored / NYET-exhausted / control-NAK: retry the whole split transaction
    subs  w12, w12, #1
    b.eq  uc_fail
    mov   w0, #200
    bl    usb_delay
    b     uc_try
uc_direct:
    ldr   x0, =USB_HC0SPLT
    str   wzr, [x0]                        // no split
    mov   w20, #HCINT_XFRC
    bl    uc_fire
    cbz   w0, uc_ok
    cmp   w0, #2
    b.eq  uc_nak
    b     uc_fail
uc_ok:
    mov   w0, #0
    b     uc_done
uc_nak:
    mov   w0, #2
    b     uc_done
uc_fail:
    mov   w0, #1
uc_done:
    ldp   x21, x22, [sp], #16
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// usb_control: full control transfer. SETUP packet must already be in usb_setup.
//  w0 = wLength (data bytes)   w1 = data dir (1 = IN, 0 = OUT/none)
//  device context in cur_*. Data buffer is usb_buf. Returns w0 = 0 ok / 1 err.
usb_control:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    mov   w19, w0                          // wLength
    mov   w20, w1                          // data dir
    // SETUP stage: ep0 OUT, PID SETUP, 8 bytes
    mov   w0, #1
    ldr   x1, =usb_stage
    str   w0, [x1]
    mov   w0, #0
    mov   w1, #0
    mov   w2, #0
    mov   w3, #PID_SETUP
    ldr   w4, =usb_setup
    mov   w5, #8
    bl    usb_chan
    mov   w9, w0                           // save SETUP rc
    ldr   x0, =usb_ssint                   // capture the SETUP stage's split HCINTs
    ldr   w1, [x0]
    ldr   x0, =usb_su_ss
    str   w1, [x0]
    ldr   x0, =usb_csint
    ldr   w1, [x0]
    ldr   x0, =usb_su_cs
    str   w1, [x0]
    cbnz  w9, uctl_err
    // DATA stage (if any): ep0, dir=w20, PID DATA1, wLength bytes
    cbz   w19, uctl_status
    mov   w0, #2
    ldr   x1, =usb_stage
    str   w0, [x1]
    mov   w0, #0
    mov   w1, w20
    mov   w2, #0
    mov   w3, #PID_DATA1
    ldr   w4, =usb_buf
    mov   w5, w19
    bl    usb_chan
    cbnz  w0, uctl_err
uctl_status:
    mov   w0, #3
    ldr   x1, =usb_stage
    str   w0, [x1]
    // STATUS stage: opposite direction of data (IN data -> OUT status, else IN),
    // zero length, PID DATA1.
    mov   w0, #0
    mov   w2, #0
    mov   w3, #PID_DATA1
    ldr   w4, =usb_buf
    mov   w5, #0
    cmp   w20, #1
    cset  w1, ne                           // data IN -> status OUT(0); else IN(1)
    bl    usb_chan
    b     uctl_done
uctl_err:
    mov   w0, #1
    b     uctl_ret
uctl_done:
    mov   w0, #0
uctl_ret:
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// set_ctx: w0=addr w1=mps w2=lspd w3=split -> cur_* context. Leaf.
set_ctx:
    ldr   x4, =cur_addr
    str   w0, [x4]
    ldr   x4, =cur_mps
    str   w1, [x4]
    ldr   x4, =cur_lspd
    str   w2, [x4]
    ldr   x4, =cur_split
    str   w3, [x4]
    ret

// set_stat: w0 = on-screen USB bring-up progress code -> usb_stat. Leaf.
//   1 power-on  2 core-reset  3 host/FIFO ready  4 root-port reset
//   5 root device connected (hub seen)  6 hub configured  7 downstream port found
//   8 keyboard addressed  9 HID endpoint parsed  10 keyboard ready (usb_ok)
set_stat:
    ldr   x1, =usb_stat
    str   w0, [x1]
    ret

// usb_power_on: turn the USB HCD on via the VideoCore mailbox "set power state"
// tag (device 3). REQUIRED on a Pi — the firmware leaves the controller powered
// down, so without this every DWC2 register write is a no-op and no port (hence
// no attached device) ever gets power. Reuses MBOX_BUF (fb_init is long done).
usb_power_on:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =MBOX_BUF
    mov   w1, #32
    str   w1, [x0, #0]                    // total size
    str   wzr, [x0, #4]                   // request
    ldr   w1, =0x28001                    // set power state
    str   w1, [x0, #8]
    mov   w1, #8
    str   w1, [x0, #12]                   // value buffer size
    str   wzr, [x0, #16]                  // request code
    mov   w1, #3
    str   w1, [x0, #20]                   // device id = USB HCD
    mov   w1, #3
    str   w1, [x0, #24]                   // state = POWER_ON | WAIT
    str   wzr, [x0, #28]                  // end tag
    ldr   x0, =MBOX_BUF
    orr   x0, x0, #MBOX_CH_PROP
    ldr   x2, =MBOX_STATUS
    ldr   w3, =0x400000
upw_w:
    ldr   w1, [x2]
    tst   w1, #MBOX_FULL
    b.eq  upw_wd
    subs  w3, w3, #1
    b.ne  upw_w
    b     upw_done                         // mailbox stuck full — bail (no hang)
upw_wd:
    ldr   x2, =MBOX_WRITE
    str   w0, [x2]
    ldr   x2, =MBOX_STATUS
    ldr   w3, =0x400000
upw_r:
    ldr   w1, [x2]
    tst   w1, #MBOX_EMPTY
    b.eq  upw_rd
    subs  w3, w3, #1
    b.ne  upw_r
    b     upw_done
upw_rd:
    ldr   x2, =MBOX_READ
    ldr   w1, [x2]
upw_done:
    ldp   x29, x30, [sp], #16
    ret

// usb_init: power on the controller, bring the DWC2 core up in host mode, size +
// flush the FIFOs, then power + reset the root port. Returns w0 = 1 if a device
// is present on the root port, else 0. Updates usb_stat as it advances.
usb_init:
    stp   x29, x30, [sp, #-16]!
    LOG   d_init
    // 0) power the USB controller on (mailbox) — the metal prerequisite
    bl    usb_power_on
    mov   w0, #1
    bl    set_stat
    LOG   d_pwron
    mov   w0, #10000
    bl    usb_delay
    // ungate the core's clocks
    ldr   x0, =USB_PCGCTL
    str   wzr, [x0]
    mov   w0, #1000
    bl    usb_delay
    // wait for AHB master idle, then core soft reset
    ldr   x0, =USB_GRSTCTL
    ldr   w3, =0x100000
ui_idle:
    ldr   w1, [x0]
    tst   w1, #RST_AHBIDLE
    b.ne  ui_idled
    subs  w3, w3, #1
    b.ne  ui_idle
ui_idled:
    mov   w1, #RST_CSFTRST
    str   w1, [x0]
    ldr   w3, =0x100000
ui_rst:
    ldr   w1, [x0]
    tst   w1, #RST_CSFTRST
    b.eq  ui_rstd
    subs  w3, w3, #1
    b.ne  ui_rst
ui_rstd:
    mov   w0, #10000
    bl    usb_delay
    mov   w0, #2
    bl    set_stat
    // GUSBCFG: force host mode, clear HNP/SRP (we are host-only)
    ldr   x0, =USB_GUSBCFG
    ldr   w1, [x0]
    ldr   w2, =GUSBCFG_FORCEHOST
    orr   w1, w1, w2
    ldr   w2, =GUSBCFG_HNPCAP
    bic   w1, w1, w2
    ldr   w2, =GUSBCFG_SRPCAP
    bic   w1, w1, w2
    str   w1, [x0]
    ldr   w0, =25000
    bl    usb_delay
    // GAHBCFG: enable DMA (bit5) + global interrupt mask bit (bit0)
    ldr   x0, =USB_GAHBCFG
    mov   w1, #0x21
    str   w1, [x0]
    // FIFO sizing (Pi DWC2 has 4080 words): RX 1024 / NPTX @1024 d1024 / PTX @2048 d1024
    ldr   x0, =USB_GRXFSIZ
    mov   w1, #1024
    str   w1, [x0]
    ldr   x0, =USB_GNPTXFSIZ
    movz  w1, #1024
    movk  w1, #1024, lsl #16
    str   w1, [x0]
    ldr   x0, =USB_HPTXFSIZ
    movz  w1, #2048
    movk  w1, #1024, lsl #16
    str   w1, [x0]
    // flush all TX FIFOs
    ldr   x0, =USB_GRSTCTL
    ldr   w1, =(RST_TXFFLSH | RST_TXFNUM_ALL)
    str   w1, [x0]
    ldr   w3, =0x100000
ui_txf:
    ldr   w1, [x0]
    tst   w1, #RST_TXFFLSH
    b.eq  ui_txfd
    subs  w3, w3, #1
    b.ne  ui_txf
ui_txfd:
    // flush the RX FIFO
    mov   w1, #RST_RXFFLSH
    str   w1, [x0]
    ldr   w3, =0x100000
ui_rxf:
    ldr   w1, [x0]
    tst   w1, #RST_RXFFLSH
    b.eq  ui_rxfd
    subs  w3, w3, #1
    b.ne  ui_rxf
ui_rxfd:
    // HCFG: FSLSPCLKSEL = 1 (48 MHz FS/LS PHY clock)
    ldr   x0, =USB_HCFG
    ldr   w1, [x0]
    orr   w1, w1, #1
    str   w1, [x0]
    mov   w0, #3
    bl    set_stat
    LOG   d_reset
    // power the root port (preserve non-W1C, set PRTPWR)
    bl    hprt_read_clean
    mov   w1, w0
    ldr   w2, =HPRT_PWR
    orr   w1, w1, w2
    ldr   x0, =USB_HPRT
    str   w1, [x0]
    ldr   w0, =50000
    bl    usb_delay
    // reset the root port (PRTRST high ~60ms, then low)
    bl    hprt_read_clean
    mov   w1, w0
    ldr   w2, =HPRT_RST
    orr   w1, w1, w2
    ldr   x0, =USB_HPRT
    str   w1, [x0]
    ldr   w0, =60000
    bl    usb_delay
    bl    hprt_read_clean
    ldr   w2, =HPRT_RST
    bic   w1, w0, w2
    ldr   x0, =USB_HPRT
    str   w1, [x0]
    ldr   w0, =50000
    bl    usb_delay
    mov   w0, #4
    bl    set_stat
    // log the post-reset HPRT (connect / speed / enable bits) then test connect
    ldr   x0, =USB_HPRT
    ldr   w9, [x0]
    LOGV  d_hprt
    and   w0, w9, #HPRT_CONNSTS
    cbz   w0, ui_noconn
    LOG   d_conn
    mov   w0, #5
    bl    set_stat
    mov   w0, #1
    b     ui_done
ui_noconn:
    LOG   d_noconn
    mov   w0, #0
ui_done:
    ldp   x29, x30, [sp], #16
    ret

// hprt_read_clean: read HPRT into w0 with the write-1-to-clear status bits masked
// off, so a value written back won't accidentally clear connect/enable changes
// or (critically) DISABLE the port by writing 1 to PRTENA. Leaf.
hprt_read_clean:
    ldr   x0, =USB_HPRT
    ldr   w0, [x0]
    ldr   w1, =HPRT_WC_BITS
    bic   w0, w0, w1
    ret

// usb_enum: enumerate the hub, then the keyboard behind it. Sets usb_ok on
// success. Returns w0 = 1 ok / 0 fail.
usb_enum:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    LOG   d_enumhub
    // ---------- the hub (root device): addr 0, direct (no split) ----------
    mov   w0, #0
    mov   w1, #8
    mov   w2, #0
    mov   w3, #0
    bl    set_ctx
    // GET_DESCRIPTOR(DEVICE, 8) -> bMaxPacketSize0
    mov   w0, #0x80
    mov   w1, #REQ_GET_DESC
    movz  w2, #0x0100
    mov   w3, #0
    mov   w4, #8
    bl    usb_setup8
    mov   w0, #8
    mov   w1, #1
    bl    usb_control
    mov   w9, w0
    LOGV  d_hubrc
    cbnz  w9, ue_fail
    ldr   x0, =usb_buf
    ldrb  w1, [x0, #7]                     // bMaxPacketSize0
    ldr   x0, =cur_mps
    str   w1, [x0]
    // SET_ADDRESS(1)
    mov   w0, #0
    mov   w1, #REQ_SET_ADDR
    mov   w2, #1
    mov   w3, #0
    mov   w4, #0
    bl    usb_setup8
    mov   w0, #0
    mov   w1, #0
    bl    usb_control
    cbnz  w0, ue_fail
    ldr   x0, =cur_addr
    mov   w1, #1
    str   w1, [x0]
    // SET_CONFIGURATION(1)
    mov   w0, #0
    mov   w1, #REQ_SET_CONFIG
    mov   w2, #1
    mov   w3, #0
    mov   w4, #0
    bl    usb_setup8
    mov   w0, #0
    mov   w1, #0
    bl    usb_control
    cbnz  w0, ue_fail
    mov   w0, #6                           // hub configured
    bl    set_stat
    // GET hub descriptor (class, type 0x29) -> bNbrPorts
    mov   w0, #0xA0
    mov   w1, #REQ_GET_DESC
    movz  w2, #0x2900
    mov   w3, #0
    mov   w4, #8
    bl    usb_setup8
    mov   w0, #8
    mov   w1, #1
    bl    usb_control
    cbnz  w0, ue_fail
    ldr   x0, =usb_buf
    ldrb  w19, [x0, #2]                    // w19 = bNbrPorts
    mov   w9, w19
    LOGV  d_nports
    cbz   w19, ue_fail
    // power every port: SET_FEATURE(PORT_POWER=8)
    mov   w20, #1                          // port index (1-based)
ue_pwr:
    mov   w0, #0x23
    mov   w1, #REQ_SET_FEAT
    mov   w2, #8                           // PORT_POWER
    mov   w3, w20
    mov   w4, #0
    bl    usb_setup8
    mov   w0, #0
    mov   w1, #0
    bl    usb_control
    add   w20, w20, #1
    cmp   w20, w19
    b.le  ue_pwr
    ldr   w0, =100000                      // power-good settle
    bl    usb_delay
    // Scan ports: reset each CONNECTED port and pick the first NON-high-speed
    // device. The LAN9514's onboard Ethernet is a HIGH-speed device on an
    // internal port (typically port 1) — it must be skipped, not mistaken for
    // the keyboard. A USB keyboard is low/full speed and needs split transactions.
    mov   w20, #1
ue_scan:
    mov   w0, #0xA3
    mov   w1, #REQ_GET_STATUS
    mov   w2, #0
    mov   w3, w20
    mov   w4, #4
    bl    usb_setup8
    mov   w0, #4
    mov   w1, #1
    bl    usb_control
    cbnz  w0, ue_next
    ldr   x0, =usb_buf
    ldrh  w9, [x0, #0]                     // wPortStatus
    LOGV  d_pstat
    tst   w9, #1                           // PORT_CONNECTION?
    b.eq  ue_next
    // connected -> reset it so the speed bits become valid
    mov   w0, #0x23
    mov   w1, #REQ_SET_FEAT
    mov   w2, #4                           // PORT_RESET
    mov   w3, w20
    mov   w4, #0
    bl    usb_setup8
    mov   w0, #0
    mov   w1, #0
    bl    usb_control
    ldr   w0, =60000
    bl    usb_delay
    mov   w0, #0xA3
    mov   w1, #REQ_GET_STATUS
    mov   w2, #0
    mov   w3, w20
    mov   w4, #4
    bl    usb_setup8
    mov   w0, #4
    mov   w1, #1
    bl    usb_control
    ldr   x0, =usb_buf
    ldrh  w9, [x0, #0]                     // wPortStatus after reset (speed valid)
    LOGV  d_pspeed
    tst   w9, #0x400                       // PORT_HIGH_SPEED (bit10)?
    b.ne  ue_next                          // high-speed -> Ethernet/hub, skip it
    // low/full-speed device -> our keyboard
    lsr   w2, w9, #9
    and   w2, w2, #1                       // PORT_LOW_SPEED (bit9)
    ldr   x0, =kbd_lspd
    str   w2, [x0]
    ldr   x0, =kbd_hub
    mov   w1, #1
    str   w1, [x0]
    ldr   x0, =kbd_port
    str   w20, [x0]
    b     ue_found
ue_next:
    add   w20, w20, #1
    cmp   w20, w19
    b.le  ue_scan
    b     ue_fail                          // no low/full-speed device on any port
ue_found:
    mov   w0, #7                           // low/full-speed device (keyboard) found
    bl    set_stat
    mov   w9, w20
    LOGV  d_found
    LOG   d_enumkbd
    // ---------- the keyboard: addr 0, behind the hub (split) ----------
    mov   w0, #0
    mov   w1, #8
    ldr   x2, =kbd_lspd
    ldr   w2, [x2]
    mov   w3, #1                           // split
    bl    set_ctx
    // SET_ADDRESS(2) FIRST — a no-data control transfer. This both rules out an
    // addr-0 conflict with the reset Ethernet and tells us (via the stage log)
    // whether SETUP+STATUS reach the device at all before we try a data stage.
    mov   w0, #0
    mov   w1, #REQ_SET_ADDR
    mov   w2, #2
    mov   w3, #0
    mov   w4, #0
    bl    usb_setup8
    mov   w0, #0
    mov   w1, #0
    bl    usb_control
    ldr   x1, =usb_kbdrc
    str   w0, [x1]
    mov   w9, w0
    LOGV  d_setaddr
    ldr   x0, =usb_stage
    ldr   w9, [x0]
    LOGV  d_stage
    ldr   x0, =usb_ssint
    ldr   w9, [x0]
    LOGV  d_ssint
    ldr   x0, =usb_csint
    ldr   w9, [x0]
    LOGV  d_csint
    ldr   x0, =usb_kbdrc
    ldr   w9, [x0]
    cbnz  w9, ue_fail
    ldr   x0, =cur_addr                    // device now at address 2
    mov   w1, #2
    str   w1, [x0]
    ldr   x0, =kbd_addr
    str   w1, [x0]
    mov   w0, #5000                        // USB set-address recovery (>2 ms)
    bl    usb_delay
    mov   w0, #8                           // keyboard addressed
    bl    set_stat
    // GET_DESCRIPTOR(DEVICE, 8) at addr 2 -> bMaxPacketSize0
    mov   w0, #0x80
    mov   w1, #REQ_GET_DESC
    movz  w2, #0x0100
    mov   w3, #0
    mov   w4, #8
    bl    usb_setup8
    mov   w0, #8
    mov   w1, #1
    bl    usb_control
    ldr   x1, =usb_kbdrc
    str   w0, [x1]
    mov   w9, w0
    LOGV  d_kbdrc
    ldr   x0, =usb_stage
    ldr   w9, [x0]
    LOGV  d_stage
    ldr   x0, =usb_su_ss
    ldr   w9, [x0]
    LOGV  d_suss
    ldr   x0, =usb_su_cs
    ldr   w9, [x0]
    LOGV  d_sucs
    ldr   x0, =usb_ssint
    ldr   w9, [x0]
    LOGV  d_ssint
    ldr   x0, =usb_csint
    ldr   w9, [x0]
    LOGV  d_csint
    ldr   x0, =usb_kbdrc
    ldr   w9, [x0]
    cbnz  w9, ue_fail
    ldr   x0, =usb_buf
    ldrb  w1, [x0, #7]                     // bMaxPacketSize0
    cmp   w1, #8                           // clamp to a sane minimum (never 0)
    b.hs  ue_mpsok
    mov   w1, #8
ue_mpsok:
    ldr   x0, =cur_mps
    str   w1, [x0]
    // DIAGNOSTIC (toward mouse support): attempt the 64-byte config-descriptor
    // read so we can see how a multi-packet split-IN behaves on real silicon.
    // Logged only — the boot endpoint below is used regardless, so the working
    // keyboard cannot regress.
    mov   w0, #0x80
    mov   w1, #REQ_GET_DESC
    movz  w2, #0x0200
    mov   w3, #0
    mov   w4, #64
    bl    usb_setup8
    mov   w0, #64
    mov   w1, #1
    bl    usb_control
    mov   w9, w0
    LOGV  d_cfgrc
    ldr   x0, =usb_ssint
    ldr   w9, [x0]
    LOGV  d_ssint
    ldr   x0, =usb_csint
    ldr   w9, [x0]
    LOGV  d_csint
    ldr   x0, =usb_buf                     // first 4 bytes received (config header)
    ldr   w9, [x0]
    LOGV  d_cfgdat
    // HID BOOT keyboards expose an interrupt-IN endpoint at EP1 with 8-byte
    // reports. Reading the full config descriptor to discover it would need a
    // multi-packet split IN, which isn't reliable yet (it crashed/aborted
    // enumeration), so assume the boot-standard endpoint and skip the config
    // read. Every remaining transfer is then single-packet (SET_* have no data
    // stage; the boot report is one 8-byte packet).
    ldr   x0, =kbd_ep
    mov   w1, #1
    str   w1, [x0]
    ldr   x0, =kbd_mps
    mov   w1, #8
    str   w1, [x0]
    mov   w0, #9                           // (boot) HID endpoint assumed
    bl    set_stat
    ldr   x0, =kbd_ep
    ldr   w9, [x0]
    LOGV  d_kbdep
    ldr   x0, =kbd_mps
    ldr   w9, [x0]
    LOGV  d_kbdmps
    // SET_CONFIGURATION(1)
    mov   w0, #0
    mov   w1, #REQ_SET_CONFIG
    mov   w2, #1
    mov   w3, #0
    mov   w4, #0
    bl    usb_setup8
    mov   w0, #0
    mov   w1, #0
    bl    usb_control
    cbnz  w0, ue_fail
    // SET_PROTOCOL(boot=0): HID class, interface 0, bRequest 0x0B
    mov   w0, #0x21
    mov   w1, #0x0B
    mov   w2, #0
    mov   w3, #0
    mov   w4, #0
    bl    usb_setup8
    mov   w0, #0
    mov   w1, #0
    bl    usb_control
    // SET_IDLE(0): HID class, bRequest 0x0A (ignore failure)
    mov   w0, #0x21
    mov   w1, #0x0A
    mov   w2, #0
    mov   w3, #0
    mov   w4, #0
    bl    usb_setup8
    mov   w0, #0
    mov   w1, #0
    bl    usb_control
    // done
    ldr   x0, =usb_tgl
    str   wzr, [x0]
    ldr   x0, =usb_ok
    mov   w1, #1
    str   w1, [x0]
    mov   w0, #10                          // keyboard ready
    bl    set_stat
    LOG   d_ready
    mov   w0, #1
    b     ue_ret
ue_fail:
    LOG   d_fail
    ldr   x0, =usb_ok
    str   wzr, [x0]
    mov   w0, #0
ue_ret:
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// parse_config: walk the config descriptor in usb_buf, find the first IN
// endpoint, store kbd_ep (epnum) + kbd_mps (max packet). w0 = 1 found / 0 not.
// Leaf.
parse_config:
    ldr   x0, =usb_buf
    ldrh  w1, [x0, #2]                     // wTotalLength
    and   w1, w1, #0xFFFF
    cmp   w1, #64
    csel  w1, w1, w1, lo                   // (cap below)
    mov   w2, #64
    cmp   w1, w2
    csel  w1, w1, w2, lo                   // w1 = min(total,64)
    mov   w3, #0                           // offset
pcfg_loop:
    cmp   w3, w1
    b.ge  pcfg_none
    ldrb  w4, [x0, w3, uxtw]               // bLength
    cbz   w4, pcfg_none
    add   w5, w3, #1
    ldrb  w5, [x0, w5, uxtw]               // bDescriptorType
    cmp   w5, #5                           // ENDPOINT
    b.ne  pcfg_adv
    add   w5, w3, #2
    ldrb  w5, [x0, w5, uxtw]               // bEndpointAddress
    tst   w5, #0x80                        // IN?
    b.eq  pcfg_adv
    and   w6, w5, #0x0F
    ldr   x7, =kbd_ep
    str   w6, [x7]
    add   w6, w3, #4
    ldrh  w6, [x0, w6, uxtw]               // wMaxPacketSize
    and   w6, w6, #0x7FF
    ldr   x7, =kbd_mps
    str   w6, [x7]
    mov   w0, #1
    ret
pcfg_adv:
    add   w3, w3, w4
    b     pcfg_loop
pcfg_none:
    mov   w0, #0
    ret

// usb_poll: one interrupt-IN poll of the keyboard; decode the boot report into
// pad bits. Returns w0 = pad bits (0 if no keyboard / no key). Non-leaf.
usb_poll:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =usb_ok
    ldr   w0, [x0]
    cbz   w0, up_none
    // keyboard context (split)
    ldr   x0, =kbd_addr
    ldr   w0, [x0]
    ldr   x1, =kbd_mps
    ldr   w1, [x1]
    ldr   x2, =kbd_lspd
    ldr   w2, [x2]
    mov   w3, #1
    bl    set_ctx
    // interrupt IN, toggling DATA0/DATA1
    ldr   x0, =kbd_ep
    ldr   w0, [x0]
    mov   w1, #1                           // IN
    mov   w2, #3                           // interrupt
    ldr   x3, =usb_tgl
    ldr   w3, [x3]
    cbz   w3, up_d0
    mov   w3, #PID_DATA1
    b     up_pid
up_d0:
    mov   w3, #PID_DATA0
up_pid:
    ldr   w4, =usb_rpt
    mov   w5, #8
    bl    usb_chan
    cbnz  w0, up_decode                    // NAK/err -> keep last report (held)
    // success: flip the toggle
    ldr   x0, =usb_tgl
    ldr   w1, [x0]
    eor   w1, w1, #1
    str   w1, [x0]
up_decode:
    // map the first keycode (usb_rpt[2]) to a pad bit
    ldr   x0, =usb_rpt
    ldrb  w1, [x0, #2]
    mov   w0, #0
    cmp   w1, #0x1A                         // w
    b.eq  up_u
    cmp   w1, #0x52                         // Up arrow
    b.eq  up_u
    cmp   w1, #0x16                         // s
    b.eq  up_d
    cmp   w1, #0x51                         // Down arrow
    b.eq  up_d
    cmp   w1, #0x04                         // a
    b.eq  up_l
    cmp   w1, #0x50                         // Left arrow
    b.eq  up_l
    cmp   w1, #0x07                         // d
    b.eq  up_r
    cmp   w1, #0x4F                         // Right arrow
    b.eq  up_r
    cmp   w1, #0x28                         // Enter
    b.eq  up_a
    cmp   w1, #0x2C                         // Space
    b.eq  up_a
    cmp   w1, #0x2A                         // Backspace
    b.eq  up_b
    cmp   w1, #0x4C                         // Delete
    b.eq  up_b
    b     up_ret
up_u:
    mov   w0, #PAD_U
    b     up_ret
up_d:
    mov   w0, #PAD_D
    b     up_ret
up_l:
    mov   w0, #PAD_L
    b     up_ret
up_r:
    mov   w0, #PAD_R
    b     up_ret
up_a:
    mov   w0, #PAD_A
    b     up_ret
up_b:
    mov   w0, #PAD_B
    b     up_ret
up_none:
    mov   w0, #0
up_ret:
    ldp   x29, x30, [sp], #16
    ret

// ---- serial debug strings --------------------------------------------------
.align 2
d_boot:     .asciz "\r\n=== UnoDOS Pi boot (serial OK) ===\r\n"
d_crlf:     .asciz "\r\n"
d_init:     .asciz "[USB] init\r\n"
d_pwron:    .asciz "[USB] power on sent\r\n"
d_reset:    .asciz "[USB] core reset + host/FIFO ok\r\n"
d_hprt:     .asciz "[USB] HPRT="
d_conn:     .asciz "[USB] root device CONNECTED\r\n"
d_noconn:   .asciz "[USB] NO device on root port\r\n"
d_enumhub:  .asciz "[USB] enumerating hub\r\n"
d_hubrc:    .asciz "[USB] hub devdesc rc="
d_nports:   .asciz "[USB] hub nbrports="
d_pstat:    .asciz "[USB] port status w="
d_pspeed:   .asciz "[USB] port reset, status="
d_found:    .asciz "[USB] keyboard (low/full-speed) on port="
d_enumkbd:  .asciz "[USB] enumerating keyboard\r\n"
d_setaddr:  .asciz "[USB] set_address rc="
d_cfgrc:    .asciz "[USB] cfg-read rc="
d_cfgdat:   .asciz "[USB] cfg[0..3]="
d_kbdrc:    .asciz "[USB] kbd devdesc rc="
d_stage:    .asciz "[USB] failed at stage (1SET/2DAT/3STA)="
d_suss:     .asciz "[USB] SETUP SS hcint="
d_sucs:     .asciz "[USB] SETUP CS hcint="
d_ssint:    .asciz "[USB] fail-stage SS hcint="
d_csint:    .asciz "[USB] fail-stage CS hcint="
d_kbdep:    .asciz "[USB] kbd ep="
d_kbdmps:   .asciz "[USB] kbd mps="
d_ready:    .asciz "[USB] *** KEYBOARD READY ***\r\n"
d_fail:     .asciz "[USB] FAIL (enum aborted)\r\n"
.align 2
