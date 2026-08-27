// ============================================================================
// UnoDOS / Cosmo Communicator (MediaTek MT6771 / Helio P70, AArch64) — M1-M3.
// ============================================================================
// The THIRD AArch64 world after rpi and pinephone. It reuses the rpi 640x480
// landscape core (the Cosmo is a keyboard clamshell, presented landscape) on the
// same GNU-as (GAS) dialect, retargeted to the MediaTek MT6771. Would be the first
// non-Linux OS ever booted on any MediaTek phone SoC. MINIMAL (CONTRACT-ARCH §9):
// one full-screen app at a time, directional nav.
//
// Boot: Planet's LK loads this payload (wrapped in an Android boot.img, slot p42)
// as the "kernel" per the arm64 protocol — DTB in x0, entry at 0x40080000. We
// DISABLE the TOPRGU watchdog first (LK armed it), then ADOPT LK's live framebuffer
// rather than bringing up the display: LK already lit the NT36672 panel and left it
// scanning a landscape 2160x1080 surface out of DRAM, and hands its base/size to us
// via the DTB videolfb properties. We centre our 640x480 UI in it and draw an 8x8
// font + 16x16 icons pixel by pixel (palette-indexed, so the Theme app recolours by
// swapping the table). Per-frame pacing reads the ARM generic timer (cntpct_el0).
//
// M1: boot -> disable WDT -> adopt LK's FB -> a rendered launcher (title + grid).
// M2: a generic-timer-paced loop, a d-pad selection highlight, A launches an app
//     full-screen, B returns. (The keyboard is the AW9523 I2C matrix — Phase 5; the
//     milestones are driven by the AUTOTEST scripted pad, like every other port.)
// M3: full-screen apps — SysInfo, live Clock, Notepad, Files, Theme (palette),
//     Music (silent until the Phase 9 AFE audio path), and Dostris.
//
// Contract-owned (Phase 4): screen geometry from unogen ([world.cosmo] ->
// gen/cosmo/sys_gen.inc). Bring-up register facts: research/COSMO-BRINGUP.md.
// ============================================================================

.include "../unodef/gen/cosmo/sys_gen.inc"   // SCRW/SCRH/SCRCOLS/SCRROWS
.include "build/gfxequ.inc"                   // NICONS/NTHEMES/MUSIC_COUNT

// ---- MediaTek MT6771 peripherals -------------------------------------------
// TOPRGU hardware watchdog. Planet's LK arms it before jumping to us and the Linux
// kernel would pet it; a payload that ignores it reboots after ~30 s, which looks
// exactly like a random crash. So we DISABLE it as the first thing _start does.
// WDT_MODE (base+0x00): bit0 = WDT_MODE_EN, and bits[31:16] must carry the key
// 0x2200 or the write is dropped. Writing 0x22000000 = key set, enable bit clear =
// watchdog off. From the vendor drivers/watchdog/mtk_wdt.c (see COSMO-BRINGUP.md).
.equ WDT_MODE,        0x10007000          // TOPRGU base + 0x00
.equ WDT_MODE_KEYDIS, 0x22000000          // key + enable-bit-clear = disable

// No mailbox, no MMIO frame timer, no PWM/UART/USB on the MT6771 bring-up path:
//  - the framebuffer is LK's, adopted at run time (fb_init), not negotiated;
//  - frame pacing reads the ARM architectural generic timer (cntpct_el0, no MMIO);
//  - input is the AW9523 keyboard matrix over I2C (Phase 5); until that lands the
//    harness AUTOTEST scripts drive the milestones, like every other fresh port;
//  - audio (MTK AFE) is Phase 9 — the Tracker/Music paths run silently.
// FRAME_TICKS: the MT6771 arch timer runs at ~13 MHz, so 13e6/60 ~= 216667 ticks per
// ~60 Hz frame. Real-hardware value is confirmed by reading CNTFRQ_EL0 in bring-up;
// the harness advances cntpct on its own so the exact value only paces real silicon.
.equ FRAME_TICKS,   216667                // ~13 MHz generic timer / 60 Hz

// Framebuffer pixel order (mailbox tag 0x48006): 0 = BGR, 1 = RGB.
// We store each pixel as a little-endian word 0xFFRRGGBB, so the bytes land in
// memory as [B,G,R,A]; the firmware must therefore read byte-0 as BLUE => BGR (0).
// (Requesting RGB(1) made real Pi 3 silicon read byte-0/blue as red -> the desktop
// rendered brown. The harness can't see this; it reads the word back directly.)
// Override for on-metal A/B testing: aarch64-linux-gnu-as --defsym FB_PIXEL_ORDER=1
.ifndef FB_PIXEL_ORDER
.equ FB_PIXEL_ORDER, 0
.endif

// pad bits (active-high) — same layout as the GBA port so AUTOTEST scripts match
.equ PAD_A,   0x01
.equ PAD_B,   0x02
.equ PAD_SEL, 0x04
.equ PAD_ST,  0x08
.equ PAD_R,   0x10
.equ PAD_L,   0x20
.equ PAD_U,   0x40
.equ PAD_D,   0x80

// Dostris geometry (board cells; rendered at 16px cells on the bigger screen)
.equ BW, 10
.equ BH, 14
.equ CELL, 16
.equ BORG_X, 224
.equ BORG_Y, 64
.equ FALLRATE, 30

// Paint geometry (cursor-driven cell canvas + a palette row one 'up' away)
.equ PCW, 36                              // canvas columns
.equ PCH, 24                              // canvas rows
.equ PCELL, 12                            // cell pixels
.equ PCO_X, 16                            // canvas origin x
.equ PCO_Y, 24                            // canvas origin y
.equ NSWATCH, 8                           // palette swatches
.equ PSW_W, 26                            // swatch pitch
.equ PSW_Y, (PCO_Y + PCH*PCELL + 10)      // swatch row y

// Pac-Man geometry (28x25 maze, tile-stepped, one 16px cell per maze tile)
.equ PM_COLS, 28
.equ PM_ROWS, 25
.equ PM_CELL, 16
.equ PMO_X, 16
.equ PMO_Y, 24
.equ GSIZE, 20                            // ghost struct: GX,GY,GDIR,GST,GTMR (words)
.equ FRIGHT_STEPS, 45
.equ PM_STEPFRAMES, 4                     // game step every N frames

// OutLast geometry (pseudo-3D road: 20 perspective bands over 40 logical cols)
.equ OL_BANDS, 20
.equ OL_COLW, 16                          // pixels per logical column (640/40)
.equ OL_BH, 22                            // band pixel height
.equ OLO_Y, 24                            // road top y
.equ OL_RATE, 4                           // frames per scroll step

// Tracker geometry (pattern grid: 16 rows x 4 channels, auto-plays leftmost voice)
.equ NT_ROWS, 16
.equ NT_CH, 4
.equ TK_STEPF, 12                         // frames per playback step
.equ TKO_X, 60
.equ TKO_Y, 44
.equ TK_RH, 22
.equ TK_CW, 80

// ---- fixed RAM layout (MT6771 DRAM starts at 0x40000000) -------------------
// Payload links at 0x40080000 (LK's kernel_addr). Stack/VARS/FBINFO sit above it;
// all clear of the LK reserved-memory carveouts (ram_console @0x54400000+, etc.).
.equ STACK_TOP, 0x40200000
.equ VARS,      0x40300000               // cleared at boot
.equ FBINFO,    0x40320000               // adopted framebuffer base/pitch (NOT cleared)
.equ fb_base,   FBINFO+0                  // 8 bytes (draw origin into LK's FB)
.equ fb_pitch,  FBINFO+8                  // 4 bytes (bytes per row = LK panel pitch)
.equ dtb_ptr,   FBINFO+16                 // 8 bytes: DTB pointer LK passed in x0
// Adopt LK's live framebuffer. LK lights the NT36672 panel (rotated 270 -> landscape
// PANEL_W x PANEL_H) and hands its base+size to the kernel via the DTB properties
// atag,videolfb-fb_base_l / -vramSize. Our SCRW x SCRH (640x480) UI is centred in it.
// fb_init reads the base from FBINFO (harness pre-seeds it; the DTB videolfb walk is
// the Phase 2 task) and falls back to COSMO_FB. See research/COSMO-BRINGUP.md: the FB
// base is NOT static (LK reserves it top-down under 0x80000000, g_fb_size 0x1F90000),
// so COSMO_FB is only a last resort; the real value must come from the DTB at run time.
.equ PANEL_W,   2160                      // LK landscape panel width
.equ PANEL_H,   1080                      // LK landscape panel height
.equ COSMO_FB,  0x7E070000                // fallback FB base (0x80000000 - g_fb_size)
.equ COSMO_PITCH, (PANEL_W*4)             // 2160 * 4 = 8640 bytes/row

// Harmless DRAM sink for peripherals not yet ported to the MT6771. The app cores
// (music, RNG seed) were written for the Pi's PWM tone path and 1 MHz system timer;
// on the Cosmo, audio is Phase 9 (MTK AFE) and there is no such MMIO timer. Point
// those addresses at scratch DRAM so the shared code assembles and runs: audio writes
// are dropped (silent Tracker/Music), and the RNG seed reads a fixed word (games are
// deterministic until a real entropy source is wired). Placed where USB_AREA used to
// sit (VARS+0x2000), clear of fbuf/canvas/maze.
.equ COSMO_SCRATCH, VARS+0x2000
.equ CM_PWMCTL,     COSMO_SCRATCH+0
.equ CM_PWMDIV,     COSMO_SCRATCH+4
.equ PWM_CTL,       COSMO_SCRATCH+8
.equ PWM_RNG1,      COSMO_SCRATCH+12
.equ PWM_DAT1,      COSMO_SCRATCH+16
.equ SYS_TIMER_CLO, COSMO_SCRATCH+20      // RNG seed source (fixed until Phase 5+)

.equ v_pad,    VARS+0
.equ v_padp,   VARS+4
.equ v_pade,   VARS+8
.equ v_inapp,  VARS+12
.equ v_sel,    VARS+16
.equ v_selp,   VARS+20
.equ v_app,    VARS+24
.equ v_dirty,  VARS+28
.equ v_frac,   VARS+32
.equ v_ss,     VARS+36
.equ v_mm,     VARS+40
.equ v_hh,     VARS+44
.equ v_theme,  VARS+48
.equ m_idx,    VARS+52
.equ m_timer,  VARS+56
.equ m_play,   VARS+60
.equ pf_hl,    VARS+64
.equ pf_clk,   VARS+68
.equ pf_pc,    VARS+72
.equ pf_score, VARS+76
.equ d_fg,     VARS+80
.equ d_bg,     VARS+84
.equ g_type,   VARS+88
.equ g_rot,    VARS+92
.equ g_px,     VARS+96
.equ g_py,     VARS+100
.equ g_state,  VARS+104
.equ g_fall,   VARS+108
.equ g_lines,  VARS+112
.equ g_seed,   VARS+116
.equ g_tx,     VARS+120
.equ g_ty,     VARS+124
.equ g_srot,   VARS+128
.equ g_row,    VARS+132
.equ g_lt,     VARS+136
.equ g_pt,     VARS+140
.equ mlo,      VARS+144
.equ g_oldpx,  VARS+148
.equ g_oldpy,  VARS+152
.equ g_oldrot, VARS+156
.equ a_idx,    VARS+160
.equ a_tmr,    VARS+164
.equ a_pad,    VARS+168
.equ a_gpause, VARS+172
.equ p_cx,     VARS+176                   // Paint cursor cell x
.equ p_cy,     VARS+180                   // Paint cursor cell y (==PCH => palette row)
.equ p_col,    VARS+184                   // Paint current colour index
.equ pm_x,     VARS+188                   // Pac-Man: pac tile x
.equ pm_y,     VARS+192                   // pac tile y
.equ pm_dir,   VARS+196                   // pac direction (0U 1L 2D 3R)
.equ pm_ndir,  VARS+200                   // pac queued direction
.equ pm_score, VARS+204
.equ pm_lives, VARS+208
.equ pm_level, VARS+212
.equ pm_dots,  VARS+216                   // remaining dots
.equ pm_mode,  VARS+220                   // scatter/chase schedule index
.equ pm_modet, VARS+224                   // mode step timer
.equ pm_fr,    VARS+228                   // fright timer (steps)
.equ pm_kills, VARS+232                   // ghosts eaten this fright
.equ pm_st,    VARS+236                   // 0 play / 1 over
.equ pm_sc,    VARS+240                   // step counter (parity)
.equ pm_tgx,   VARS+244                   // ghost target tile (steer)
.equ pm_tgy,   VARS+248
.equ pm_ft,    VARS+252                   // frame timer for step pacing
.equ pm_gh,    VARS+256                   // 3 ghosts * GSIZE bytes (ends VARS+316)
.equ ol_carx,  VARS+316                   // OutLast: car column (0..39)
.equ ol_scroll,VARS+320                   // road scroll position
.equ ol_dist,  VARS+324                   // distance (score)
.equ ol_over,  VARS+328                   // crashed flag
.equ ol_ctr,   VARS+332                   // frames-until-scroll counter
.equ tk_crow,  VARS+336                   // Tracker: cursor row
.equ tk_cch,   VARS+340                   // cursor channel
.equ tk_prow,  VARS+344                   // playing row
.equ tk_ptmr,  VARS+348                   // playback step timer
.equ fl_sel,   VARS+352                   // Files: selected file
.equ fl_view,  VARS+356                   // Files: 0 list / 1 viewing
.equ np_saved, VARS+360                   // Notepad: save-feedback flag
.equ palette,  VARS+0x200                 // 16 XRGB words
.equ clk_str,  VARS+0x240                 // 9 bytes
.equ numstr,   VARS+0x250                 // 6 bytes
.equ g_board,  VARS+0x260                 // BW*BH bytes
.equ pcanvas,  VARS+0x400                 // PCW*PCH Paint canvas (cleared by paint_init)
.equ pm_maze,  VARS+0x800                 // PM_COLS*PM_ROWS mutable maze (copied at new game)
.equ tk_pat,   VARS+0xC00                 // NT_ROWS*NT_CH tracker pattern (cleared at init)
.equ fbuf,     VARS+0x1000                // 4 KB file-view scratch buffer

// (No USB on the MT6771 bring-up path — the keyboard is the AW9523 I2C matrix,
//  brought up in Phase 5. Input is AUTOTEST-scripted until then.)

.section .text
.global _start
// ARM64 Linux Image header (64 bytes). Planet's LK loads our payload (wrapped in an
// Android boot.img) as the "kernel" and enters it per the arm64 boot protocol. code0
// branches over the header to the real entry; text_offset 0x80000 -> LK's load base
// 0x40000000 + 0x80000 = 0x40080000 = our link address. Transparent to the harness,
// which jumps straight to code0. magic 0x644d5241 = "ARM\x64".
_start:
    b     _entry                           // code0
    .word 0                                // code1
    .quad 0x00080000                       // text_offset
    .quad 0x00400000                       // image_size (advisory)
    .quad 0                                // flags
    .quad 0                                // res2
    .quad 0                                // res3
    .quad 0                                // res4
    .word 0x644d5241                       // magic "ARM\x64"
    .word 0                                // res5 (PE COFF offset)
_entry:
    // x0 holds the DTB pointer LK passed (arm64 boot protocol). Save it BEFORE any
    // register churn — fb_init walks it for the LK framebuffer (Phase 2), and it is
    // the only source of the run-time FB base (which is not a static address).
    mov   x19, x0                         // keep DTB out of the way of the WDT write
    // --- park secondary cores; only core 0 runs UnoDOS ---
    mrs   x1, mpidr_el1
    and   x1, x1, #0xFF
    cbz   x1, core0
hang:
    wfe
    b     hang
core0:
    ldr   x0, =STACK_TOP
    mov   sp, x0
    // --- DISABLE THE WATCHDOG FIRST (decision #3 in the port plan) ---
    // LK armed the TOPRGU watchdog; if we never pet it the device reboots in ~30 s.
    // Write the key with the enable bit clear to turn it off. This must precede any
    // slow bring-up so a stall can't be masked by a spontaneous reboot.
    ldr   x0, =WDT_MODE
    ldr   w1, =WDT_MODE_KEYDIS
    str   w1, [x0]
    dsb   sy
    // --- run cache-off, MMU-on (LK may hand off with caches enabled) ---
    // We draw into LK's framebuffer, which LK's display engine scans out of DRAM by
    // DMA; a stale cached FB line would show as garbage. Disable D/I caches but keep
    // the MMU (so DRAM stays Normal/unaligned-tolerant, just uncached -> coherent FB).
    mov   x2, #0x1004                     // bits 2 (C/D$) | 12 (I/I$); leave M (MMU) set
    mrs   x0, CurrentEL
    lsr   x0, x0, #2
    and   x0, x0, #3
    cmp   x0, #2
    b.lt  cache_el1
    mrs   x1, sctlr_el2
    bic   x1, x1, x2
    msr   sctlr_el2, x1
    b     cache_done
cache_el1:
    mrs   x1, sctlr_el1
    bic   x1, x1, x2
    msr   sctlr_el1, x1
cache_done:
    dsb   sy
    isb
    // clear the variable block (does not touch FBINFO)
    ldr   x0, =VARS
    mov   w2, #256                        // 256 words = 1KB
mclr:
    str   wzr, [x0], #4
    subs  w2, w2, #1
    b.ne  mclr
    ldr   x0, =dtb_ptr                    // stash the DTB pointer for fb_init
    str   x19, [x0]
    bl    fb_init                         // adopt LK's live framebuffer
    bl    fs_init                         // load/format the USV1 disk
    bl    draw_launcher                   // show the desktop
mainloop:
    bl    wait_vblank
    bl    render_partials
    bl    read_keys
    bl    clock_advance
    bl    update
    ldr   x0, =v_dirty
    ldr   w1, [x0]
    cbz   w1, mainloop
    bl    full_redraw
    b     mainloop

// ============================================================================
// framebuffer bring-up — ADOPT LK's live framebuffer (no display bring-up)
// ============================================================================
// LK lit the NT36672 panel (rotated 270 -> landscape PANEL_W x PANEL_H) and left a
// framebuffer scanning out of DRAM. We draw into THAT buffer; we never program the
// display engine. The panel FB base comes from FBINFO (the harness pre-seeds
// fb_base/fb_pitch; on hardware the Phase 2 DTB videolfb walk of dtb_ptr fills them),
// else COSMO_FB. Then centre our SCRW x SCRH UI: the draw origin becomes
// fb_base + top*pitch + left*4, with fb_pitch kept at the panel stride. Leaf.
//
// TODO (Phase 2): walk the FDT at dtb_ptr for the properties atag,videolfb-fb_base_l
// and atag,videolfb-vramSize and use those instead of the COSMO_FB fallback — the FB
// base is allocated dynamically by LK (mblock_reserve_ext), so it is NOT a fixed
// address and must be read from the DTB on real hardware. See research/COSMO-BRINGUP.md.
fb_init:
    ldr   x0, =fb_base
    ldr   x3, [x0]                        // panel FB base (0 if not pre-seeded)
    cbnz  x3, fb_have_base
    ldr   x3, =COSMO_FB                   // fallback (dynamic on HW — see above)
fb_have_base:
    ldr   x0, =fb_pitch
    ldr   w4, [x0]                        // panel pitch (0 if not pre-seeded)
    cbnz  w4, fb_have_pitch
    mov   w4, #COSMO_PITCH
fb_have_pitch:
    // clear the whole panel to black (wipes LK's boot logo), then centre our UI
    mov   x7, x3                          // FB base
    mov   w8, #PANEL_H
    umull x8, w8, w4                      // total bytes = PANEL_H * pitch
    add   x8, x7, x8                      // end address
fb_clrpanel:
    str   wzr, [x7], #4
    cmp   x7, x8
    b.lo  fb_clrpanel
    dsb   sy
    // centre: top = (PANEL_H - SCRH)/2 rows, left = (PANEL_W - SCRW)/2 pixels
    mov   w5, #((PANEL_H-SCRH)/2)
    umull x6, w5, w4                      // top rows * pitch
    add   x3, x3, x6
    mov   w5, #(((PANEL_W-SCRW)/2)*4)     // left pixels * 4 bytes
    add   x3, x3, x5
    ldr   x0, =fb_base
    str   x3, [x0]                        // draw origin (centred in the panel)
    ldr   x0, =fb_pitch
    str   w4, [x0]                        // stride = panel pitch
    ret

// wait_vblank: pace one frame off the ARM architectural generic timer (~60 Hz).
// cntpct_el0 is a no-MMIO monotonic counter (MT6771 ~13 MHz); the Unicorn harness
// advances it on its own, so this needs no MMIO model there.
wait_vblank:
    mrs   x0, cntpct_el0
    ldr   x1, =FRAME_TICKS
    add   x1, x0, x1
wv1:
    mrs   x0, cntpct_el0
    cmp   x0, x1
    b.lo  wv1
    ret

// read_keys: real input via the PL011 UART serial console (GPIO14/15). Each received
// byte is one keypress; WASD = d-pad, Enter/Space = A, Backspace/DEL = B. Held state
// lasts one frame, so nav reads edges and Dostris reads a per-press step. AUTOTEST
// builds replace this with the scripted pad (auto_input). Leaf.
read_keys:
.ifdef AUTOTEST
    b     auto_input
.endif
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =v_pad                      // v_padp = v_pad
    ldr   w1, [x0]
    ldr   x2, =v_padp
    str   w1, [x2]
    // Hardware keyboard input is the AW9523 I2C GPIO-matrix (Phase 5) — not yet wired.
    // Until then the non-AUTOTEST build reports no keys each frame; the ASCII->pad
    // decode below is retained for the Phase 5 keyboard driver to feed once it lands.
    b     rk_none
rk_uart:
    and   w4, w4, #0xFF
    mov   w5, #0
    cmp   w4, #'w'
    b.eq  rk_u
    cmp   w4, #'W'
    b.eq  rk_u
    cmp   w4, #'s'
    b.eq  rk_d
    cmp   w4, #'S'
    b.eq  rk_d
    cmp   w4, #'a'
    b.eq  rk_l
    cmp   w4, #'A'
    b.eq  rk_l
    cmp   w4, #'d'
    b.eq  rk_r
    cmp   w4, #'D'
    b.eq  rk_r
    cmp   w4, #0x0D                        // Enter
    b.eq  rk_a
    cmp   w4, #' '
    b.eq  rk_a
    cmp   w4, #0x08                        // Backspace
    b.eq  rk_b
    cmp   w4, #0x7F                        // DEL
    b.eq  rk_b
    b     rk_store
rk_u:
    mov   w5, #PAD_U
    b     rk_store
rk_d:
    mov   w5, #PAD_D
    b     rk_store
rk_l:
    mov   w5, #PAD_L
    b     rk_store
rk_r:
    mov   w5, #PAD_R
    b     rk_store
rk_a:
    mov   w5, #PAD_A
    b     rk_store
rk_b:
    mov   w5, #PAD_B
rk_store:
    ldr   x0, =v_pad
    str   w5, [x0]
    b     rk_edge
rk_none:
    ldr   x0, =v_pad
    str   wzr, [x0]
rk_edge:
    ldr   x0, =v_pad
    ldr   w1, [x0]
    ldr   x2, =v_padp
    ldr   w2, [x2]
    mvn   w2, w2
    and   w1, w1, w2                       // edges = new & ~prev
    ldr   x0, =v_pade
    str   w1, [x0]
    ldp   x29, x30, [sp], #16
    ret

// ============================================================================
// framebuffer primitives  (32bpp XRGB; addr = fb_base + y*pitch + x*4)
// ============================================================================
// pchar: w0=px w1=py w2=ascii ; colours from d_fg/d_bg (palette indices). Leaf.
pchar:
    sub   w2, w2, #32
    ldr   x3, =font_data
    add   x3, x3, w2, uxtw #3
    ldr   x4, =fb_base
    ldr   x4, [x4]
    ldr   x5, =fb_pitch
    ldr   w5, [x5]
    umull x6, w1, w5
    add   x4, x4, x6
    add   x4, x4, w0, uxtw #2             // x4 = pixel address
    ldr   x6, =palette
    ldr   x7, =d_fg
    ldr   w7, [x7]
    ldr   w7, [x6, w7, uxtw #2]           // fg colour
    ldr   x8, =d_bg
    ldr   w8, [x8]
    ldr   w8, [x6, w8, uxtw #2]           // bg colour
    mov   w9, #8
pchar_row:
    ldrb  w10, [x3], #1
    mov   w11, #0x80
pchar_col:
    tst   w10, w11
    csel  w12, w7, w8, ne
    str   w12, [x4], #4
    lsr   w11, w11, #1
    cbnz  w11, pchar_col
    add   x4, x4, x5
    sub   x4, x4, #32                     // back to next row start (8px * 4B)
    subs  w9, w9, #1
    b.ne  pchar_row
    ret

// pstr: w0=px w1=py x2=strptr (NUL-terminated); colours from d_fg/d_bg
pstr:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    stp   x21, x22, [sp, #-16]!
    mov   w19, w0
    mov   w20, w1
    mov   x21, x2
pstr_l:
    ldrb  w2, [x21], #1
    cbz   w2, pstr_d
    mov   w0, w19
    mov   w1, w20
    bl    pchar
    add   w19, w19, #8
    b     pstr_l
pstr_d:
    ldp   x21, x22, [sp], #16
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// frect: w0=x w1=y w2=w w3=h ; colour from d_fg. Leaf.
frect:
    ldr   x4, =palette
    ldr   x5, =d_fg
    ldr   w5, [x5]
    ldr   w4, [x4, w5, uxtw #2]           // colour word
    ldr   x10, =fb_base
    ldr   x10, [x10]
    ldr   x11, =fb_pitch
    ldr   w11, [x11]
fr_row:
    cbz   w3, fr_done
    umull x6, w1, w11
    add   x6, x10, x6
    add   x6, x6, w0, uxtw #2
    mov   w7, w2
fr_col:
    str   w4, [x6], #4
    subs  w7, w7, #1
    b.ne  fr_col
    add   w1, w1, #1
    sub   w3, w3, #1
    b     fr_row
fr_done:
    ret

// picon: w0=px w1=py w2=icon idx -> 16x16 icon. Leaf.
picon:
    ldr   x3, =icon_data
    lsl   w11, w2, #8                     // icon*256
    add   x3, x3, w11, uxtw
    ldr   x9, =palette
    ldr   x4, =fb_base
    ldr   x4, [x4]
    ldr   x5, =fb_pitch
    ldr   w5, [x5]
    umull x6, w1, w5
    add   x4, x4, x6
    add   x4, x4, w0, uxtw #2
    mov   w6, #16
pic_row:
    mov   w7, #16
pic_col:
    ldrb  w8, [x3], #1
    ldr   w10, [x9, w8, uxtw #2]
    str   w10, [x4], #4
    subs  w7, w7, #1
    b.ne  pic_col
    add   x4, x4, x5
    sub   x4, x4, #64                     // 16px * 4B
    subs  w6, w6, #1
    b.ne  pic_row
    ret

// setfb: w0=fg index, w1=bg index
setfb:
    ldr   x2, =d_fg
    str   w0, [x2]
    ldr   x2, =d_bg
    str   w1, [x2]
    ret

// set_fg: w0=fg index
set_fg:
    ldr   x1, =d_fg
    str   w0, [x1]
    ret

// load_palette: copy theme_pals[v_theme] (16 words) -> palette
load_palette:
    ldr   x0, =v_theme
    ldr   w0, [x0]
    ldr   x1, =theme_pals
    lsl   w4, w0, #6                      // theme*64 bytes
    add   x1, x1, w4, uxtw
    ldr   x2, =palette
    mov   w3, #16
lp_l:
    ldr   w0, [x1], #4
    str   w0, [x2], #4
    subs  w3, w3, #1
    b.ne  lp_l
    ret

// clear_screen: fill the whole framebuffer with palette[0] (the desktop colour)
clear_screen:
    stp   x29, x30, [sp, #-16]!
    mov   w0, #0
    bl    set_fg
    mov   w0, #0
    mov   w1, #0
    mov   w2, #SCRW
    mov   w3, #SCRH
    bl    frect
    ldp   x29, x30, [sp], #16
    ret

// two_digits: w0 = 0..99 -> w1 = tens char, w0 = units char. Leaf.
two_digits:
    mov   w1, #'0'
td_l:
    cmp   w0, #10
    b.lo  td_d
    sub   w0, w0, #10
    add   w1, w1, #1
    b     td_l
td_d:
    add   w0, w0, #'0'
    ret

// ============================================================================
// launcher (M1) — 4-column icon grid, selected label inverted
// ============================================================================
draw_launcher:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    bl    load_palette
    bl    clear_screen
    // title bar: white strip + inverted title
    mov   w0, #1
    bl    set_fg
    mov   w0, #0
    mov   w1, #0
    mov   w2, #SCRW
    mov   w3, #16
    bl    frect
    mov   w0, #0
    mov   w1, #1
    bl    setfb
    mov   w0, #8
    mov   w1, #4
    ldr   x2, =s_title
    bl    pstr
    // icon grid
    mov   w19, #0
dl_item:
    mov   w0, w19
    bl    icon_x
    mov   w20, w0
    mov   w0, w19
    bl    icon_y
    mov   w1, w0
    mov   w0, w20
    mov   w2, w19
    bl    picon
    mov   w0, w19
    bl    draw_label_for
    add   w19, w19, #1
    cmp   w19, #NICONS
    b.ne  dl_item
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// icon_x: w0=index -> w0 = pixel x of its column. Leaf.
icon_x:
    and   w0, w0, #3
    mov   w1, #150
    mul   w0, w0, w1
    add   w0, w0, #36
    ret
// icon_y: w0=index -> w0 = pixel y of its row group. Leaf.
icon_y:
    lsr   w0, w0, #2                      // row = i/4
    mov   w1, #120
    mul   w0, w0, w1
    add   w0, w0, #48
    ret

// draw_label_for: w0 = icon index -> draw its label (normal, inverted if selected)
draw_label_for:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    stp   x21, x22, [sp, #-16]!
    mov   w19, w0
    bl    icon_x
    mov   w20, w0                         // x
    mov   w0, w19
    bl    icon_y
    add   w21, w0, #20                    // label y = icon y + 20
    ldr   x0, =v_sel
    ldr   w0, [x0]
    cmp   w0, w19
    b.ne  dlf_normal
    mov   w0, #0                          // selected -> inverted
    mov   w1, #1
    b     dlf_set
dlf_normal:
    mov   w0, #1
    mov   w1, #0
dlf_set:
    bl    setfb
    ldr   x1, =icon_lbl
    ldr   w2, [x1, w19, uxtw #2]          // label ptr (32-bit address)
    mov   w0, w20
    mov   w1, w21
    bl    pstr
    ldp   x21, x22, [sp], #16
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

draw_highlight:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =v_selp
    ldr   w0, [x0]
    bl    draw_label_for
    ldr   x0, =v_sel
    ldr   w0, [x0]
    bl    draw_label_for
    ldp   x29, x30, [sp], #16
    ret

// ============================================================================
// input / navigation (M2)
// ============================================================================
update:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =v_inapp
    ldr   w0, [x0]
    cbnz  w0, up_app
    bl    nav_input
    ldp   x29, x30, [sp], #16
    ret
up_app:
    ldr   x0, =v_pade
    ldr   w0, [x0]
    tst   w0, #PAD_B
    b.eq  up_disp
    bl    enter_launcher
    ldp   x29, x30, [sp], #16
    ret
up_disp:
    ldr   x0, =v_app
    ldr   w0, [x0]
    cmp   w0, #3
    b.ne  up_d1
    bl    music_tick
up_d1:
    ldr   x0, =v_app
    ldr   w0, [x0]
    cmp   w0, #5
    b.ne  up_d2
    bl    theme_input
up_d2:
    ldr   x0, =v_app
    ldr   w0, [x0]
    cmp   w0, #7
    b.ne  up_dp
    bl    dostris_update
up_dp:
    ldr   x0, =v_app
    ldr   w0, [x0]
    cmp   w0, #10
    b.ne  up_pm
    bl    paint_update
up_pm:
    ldr   x0, =v_app
    ldr   w0, [x0]
    cmp   w0, #9
    b.ne  up_ol
    bl    pacman_update
up_ol:
    ldr   x0, =v_app
    ldr   w0, [x0]
    cmp   w0, #8
    b.ne  up_tk
    bl    outlast_update
up_tk:
    ldr   x0, =v_app
    ldr   w0, [x0]
    cmp   w0, #6
    b.ne  up_fl
    bl    tracker_update
up_fl:
    ldr   x0, =v_app
    ldr   w0, [x0]
    cmp   w0, #4
    b.ne  up_np
    bl    files_update
up_np:
    ldr   x0, =v_app
    ldr   w0, [x0]
    cmp   w0, #2
    b.ne  up_d3
    bl    notepad_update
up_d3:
    ldp   x29, x30, [sp], #16
    ret

nav_input:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =v_pade
    ldr   w0, [x0]
    tst   w0, #PAD_A
    b.eq  nav_dir
    ldr   x0, =v_sel
    ldr   w0, [x0]
    ldr   x1, =v_app
    str   w0, [x1]
    ldr   x1, =v_inapp
    mov   w2, #1
    str   w2, [x1]
    bl    enter_app
    ldp   x29, x30, [sp], #16
    ret
nav_dir:
    ldr   x0, =v_pade
    ldr   w0, [x0]
    tst   w0, #PAD_U
    b.eq  nd1
    bl    sel_up
nd1:
    ldr   x0, =v_pade
    ldr   w0, [x0]
    tst   w0, #PAD_D
    b.eq  nd2
    bl    sel_down
nd2:
    ldr   x0, =v_pade
    ldr   w0, [x0]
    tst   w0, #PAD_L
    b.eq  nd3
    bl    sel_left
nd3:
    ldr   x0, =v_pade
    ldr   w0, [x0]
    tst   w0, #PAD_R
    b.eq  nd4
    bl    sel_right
nd4:
    ldp   x29, x30, [sp], #16
    ret

// grid navigation: L/R +-1 (wrap), U/D +-4 (clamp). Leaf helpers.
sel_right:
    ldr   x2, =v_sel
    ldr   w0, [x2]
    ldr   x3, =v_selp
    str   w0, [x3]
    add   w0, w0, #1
    cmp   w0, #NICONS
    csel  w0, wzr, w0, hs
    str   w0, [x2]
    b     mark_hl
sel_left:
    ldr   x2, =v_sel
    ldr   w0, [x2]
    ldr   x3, =v_selp
    str   w0, [x3]
    cbnz  w0, sl_dec
    mov   w0, #NICONS
sl_dec:
    sub   w0, w0, #1
    str   w0, [x2]
    b     mark_hl
sel_down:
    ldr   x2, =v_sel
    ldr   w0, [x2]
    add   w1, w0, #4
    cmp   w1, #NICONS
    b.hs  sd_no
    ldr   x3, =v_selp
    str   w0, [x3]
    str   w1, [x2]
    b     mark_hl
sd_no:
    ret
sel_up:
    ldr   x2, =v_sel
    ldr   w0, [x2]
    cmp   w0, #4
    b.lo  su_no
    ldr   x3, =v_selp
    str   w0, [x3]
    sub   w0, w0, #4
    str   w0, [x2]
    b     mark_hl
su_no:
    ret
mark_hl:
    ldr   x0, =pf_hl
    mov   w1, #1
    str   w1, [x0]
    ret

enter_app:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =v_app
    ldr   w0, [x0]
    cmp   w0, #7
    b.ne  ea1
    bl    dostris_init
ea1:
    ldr   x0, =v_app
    ldr   w0, [x0]
    cmp   w0, #3
    b.ne  ea_pt
    bl    music_init
ea_pt:
    ldr   x0, =v_app
    ldr   w0, [x0]
    cmp   w0, #10
    b.ne  ea_pm
    bl    paint_init
ea_pm:
    ldr   x0, =v_app
    ldr   w0, [x0]
    cmp   w0, #9
    b.ne  ea_ol
    bl    pacman_init
ea_ol:
    ldr   x0, =v_app
    ldr   w0, [x0]
    cmp   w0, #8
    b.ne  ea_tk
    bl    outlast_init
ea_tk:
    ldr   x0, =v_app
    ldr   w0, [x0]
    cmp   w0, #6
    b.ne  ea_fl
    bl    tracker_init
ea_fl:
    ldr   x0, =v_app
    ldr   w0, [x0]
    cmp   w0, #4
    b.ne  ea_np
    bl    files_init
ea_np:
    ldr   x0, =v_app
    ldr   w0, [x0]
    cmp   w0, #2
    b.ne  ea2
    bl    notepad_init
ea2:
    ldr   x0, =v_dirty
    mov   w1, #1
    str   w1, [x0]
    ldp   x29, x30, [sp], #16
    ret

enter_launcher:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =v_inapp
    str   wzr, [x0]
    bl    music_silence
    ldr   x0, =v_dirty
    mov   w1, #1
    str   w1, [x0]
    ldp   x29, x30, [sp], #16
    ret

// ============================================================================
// render_partials: small in-loop framebuffer writes
// ============================================================================
render_partials:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =v_inapp
    ldr   w0, [x0]
    cbnz  w0, rp_app
    ldr   x0, =pf_hl
    ldr   w1, [x0]
    cbz   w1, rp_done
    bl    draw_highlight
    ldr   x0, =pf_hl
    str   wzr, [x0]
rp_done:
    ldp   x29, x30, [sp], #16
    ret
rp_app:
    ldr   x0, =v_app
    ldr   w0, [x0]
    cmp   w0, #1
    b.eq  rp_clock
    cmp   w0, #3
    b.eq  rp_music
    cmp   w0, #7
    b.eq  rp_dostris
    ldp   x29, x30, [sp], #16
    ret
rp_clock:
    ldr   x0, =pf_clk
    ldr   w1, [x0]
    cbz   w1, rp_done
    bl    draw_clock_time
    ldr   x0, =pf_clk
    str   wzr, [x0]
    ldp   x29, x30, [sp], #16
    ret
rp_music:
    ldr   x0, =pf_score
    ldr   w1, [x0]
    cbz   w1, rp_done
    bl    draw_music_status
    ldr   x0, =pf_score
    str   wzr, [x0]
    ldp   x29, x30, [sp], #16
    ret
rp_dostris:
    ldr   x0, =pf_pc
    ldr   w1, [x0]
    cbz   w1, rp_done
    bl    draw_piece_partial
    ldr   x0, =pf_pc
    str   wzr, [x0]
    ldp   x29, x30, [sp], #16
    ret

// full_redraw: whole-screen redraw (launcher or app)
full_redraw:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =v_dirty
    str   wzr, [x0]
    ldr   x0, =pf_hl
    str   wzr, [x0]
    ldr   x0, =pf_pc
    str   wzr, [x0]
    ldr   x0, =v_inapp
    ldr   w0, [x0]
    cbnz  w0, fr_app2
    bl    draw_launcher
    ldp   x29, x30, [sp], #16
    ret
fr_app2:
    bl    draw_app
    ldp   x29, x30, [sp], #16
    ret

    .include "apps.inc.s"
    .include "dostris.inc.s"
    .include "paint.inc.s"
    .include "pacman.inc.s"
    .include "outlast.inc.s"
    .include "tracker.inc.s"
    .include "fs.inc.s"
