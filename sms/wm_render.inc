; ============================================================================
; UnoDOS/SMS — rendering, the M3 app content, and the AUTOTEST input script.
; Included by kernel.asm (labels resolve globally in sjasmplus). ix = window
; entry in the content renderers.
; ============================================================================

; ============================================================================
; Rendering
; ============================================================================
maybe_redraw:
    ld a, (v_dirty)
    or a
    ret z
    xor a
    ld (v_dirty), a
    ; fall through to redraw_all

redraw_all:
    ; clear the damage region to tile 0 (clipped fill == clear_names at full clip)
    xor a
    ld (dp_col), a
    ld (dp_row), a
    ld a, SCRCOLS
    ld (dp_w), a
    ld a, 28
    ld (dp_h), a
    xor a
    ld (dp_tile), a
    call fill_cells
    call draw_desktop_base
    xor a
    ld (dr_slot), a                 ; z index 0..zcount-1 (bottom..top)
ra_loop:
    ld a, (v_zcount)
    ld b, a
    ld a, (dr_slot)
    cp b
    jr nc, ra_done
    ld l, a
    ld h, 0
    ld de, zlist
    add hl, de
    ld a, (hl)                      ; slot at this z
    call draw_window
    ld a, (dr_slot)
    inc a
    ld (dr_slot), a
    jr ra_loop
ra_done:
    call clip_reset                 ; restore full-screen clip after every redraw
    ret

; ---- draw_desktop_base: title bar + the labelled icon grid ----------------
draw_desktop_base:
    ld hl, NAME_BASE
    call vram_set_addr
    ld b, SCRCOLS
ddb_tb:
    ld a, T_FONTINV
    out (VDP_DATA), a
    xor a
    out (VDP_DATA), a
    djnz ddb_tb
    ld d, 1
    ld e, 0
    ld hl, s_title
    ld c, T_FONTINV
    call puts_at
    ; icon grid: col=(i&3)*8+1, row=3+7*(i/4)
    ld ix, icon_tbl
    ld c, 0                         ; icon index
ddb_ig:
    ld a, c
    and 3
    add a, a
    add a, a
    add a, a
    inc a
    ld d, a                         ; col
    ld a, c
    srl a
    srl a
    ld b, a                         ; group
    add a, a
    add a, a
    add a, b
    add a, b
    add a, b
    add a, 3
    ld e, a                         ; row
    ld a, c
    add a, a
    add a, a
    add a, T_ICONS                  ; TL = T_ICONS + i*4
    push bc
    push de
    ld c, a
    call draw_icon
    pop de
    inc e
    inc e
    ld l, (ix+0)
    ld h, (ix+1)
    ld c, T_FONT
    call puts_at
    pop bc
    inc ix
    inc ix
    inc c
    ld a, c
    cp NICONS
    jr nz, ddb_ig
    ret

; ---- draw_icon: D=col E=row, C=TL tile (TL,TR,BL,BR consecutive) ----------
; draw_icon - D=col E=row, C=TL tile (TL,TR,BL,BR consecutive). Each of the four
; cells is clipped to the damage window via put_cell.
draw_icon:
    push de
    push bc
    call put_cell                   ; TL at (d,e)
    inc d
    inc c
    call put_cell                   ; TR at (d+1,e)
    dec d
    inc e
    inc c
    call put_cell                   ; BL at (d,e+1)
    inc d
    inc c
    call put_cell                   ; BR at (d+1,e+1)
    pop bc
    pop de
    ret

; ---- draw_window: A = slot -> chrome + app content ------------------------
draw_window:
    call win_ptr
    ld a, (ix+WSTATE)
    or a
    ret z
    ; title bar fill (inverted blanks = white)
    ld a, (ix+WX)
    ld (dp_col), a
    ld a, (ix+WY)
    ld (dp_row), a
    ld a, (ix+WW)
    ld (dp_w), a
    ld a, 1
    ld (dp_h), a
    ld a, T_FONTINV
    ld (dp_tile), a
    call fill_cells
    ; title text
    ld a, (ix+WX)
    inc a
    ld d, a
    ld a, (ix+WY)
    ld e, a
    ld l, (ix+WTITLE)
    ld h, (ix+WTITLE+1)
    ld c, T_FONTINV
    call puts_at
    ; close box 'X'
    ld a, (ix+WX)
    add a, (ix+WW)
    sub 2
    ld d, a
    ld a, (ix+WY)
    ld e, a
    ld hl, s_closebox
    ld c, T_FONTINV
    call puts_at
    ; body fill (blue content)
    ld a, (ix+WX)
    inc a
    ld (dp_col), a
    ld a, (ix+WY)
    inc a
    ld (dp_row), a
    ld a, (ix+WW)
    sub 2
    ld (dp_w), a
    ld a, (ix+WH)
    sub 2
    ld (dp_h), a
    xor a
    ld (dp_tile), a
    call fill_cells
    ; left border
    ld a, (ix+WX)
    ld (dp_col), a
    ld a, (ix+WY)
    inc a
    ld (dp_row), a
    ld a, 1
    ld (dp_w), a
    ld a, (ix+WH)
    sub 2
    ld (dp_h), a
    ld a, T_EDGEL
    ld (dp_tile), a
    call fill_cells
    ; right border
    ld a, (ix+WX)
    add a, (ix+WW)
    dec a
    ld (dp_col), a
    ld a, (ix+WY)
    inc a
    ld (dp_row), a
    ld a, 1
    ld (dp_w), a
    ld a, (ix+WH)
    sub 2
    ld (dp_h), a
    ld a, T_EDGER
    ld (dp_tile), a
    call fill_cells
    ; bottom-left corner
    ld a, (ix+WX)
    ld (dp_col), a
    ld a, (ix+WY)
    add a, (ix+WH)
    dec a
    ld (dp_row), a
    ld a, 1
    ld (dp_w), a
    ld (dp_h), a
    ld a, T_CORNBL
    ld (dp_tile), a
    call fill_cells
    ; bottom edge
    ld a, (ix+WX)
    inc a
    ld (dp_col), a
    ld a, (ix+WY)
    add a, (ix+WH)
    dec a
    ld (dp_row), a
    ld a, (ix+WW)
    sub 2
    ld (dp_w), a
    ld a, 1
    ld (dp_h), a
    ld a, T_EDGEB
    ld (dp_tile), a
    call fill_cells
    ; bottom-right corner
    ld a, (ix+WX)
    add a, (ix+WW)
    dec a
    ld (dp_col), a
    ld a, (ix+WY)
    add a, (ix+WH)
    dec a
    ld (dp_row), a
    ld a, 1
    ld (dp_w), a
    ld (dp_h), a
    ld a, T_CORNBR
    ld (dp_tile), a
    call fill_cells
    jp app_draw_content

; ---- clip_reset: clip window = whole screen (no clipping) -----------------
clip_reset:
    xor a
    ld (v_clip_c0), a
    ld (v_clip_r0), a
    ld a, SCRCOLS
    ld (v_clip_c1), a
    ld a, 28                        ; SCRROWS visible rows
    ld (v_clip_r1), a
    ret

; ---- put_cell: write tile C at cell (D=col,E=row) iff inside the clip window.
; Preserves BC, DE, HL. -----------------------------------------------------
put_cell:
    push bc
    push de
    push hl
    ld a, (v_clip_c0)
    ld b, a
    ld a, d
    cp b
    jr c, pc_out                    ; d < c0
    ld a, (v_clip_c1)
    ld b, a
    ld a, d
    cp b
    jr nc, pc_out                   ; d >= c1
    ld a, (v_clip_r0)
    ld b, a
    ld a, e
    cp b
    jr c, pc_out                    ; e < r0
    ld a, (v_clip_r1)
    ld b, a
    ld a, e
    cp b
    jr nc, pc_out                   ; e >= r1
    ld h, 0
    ld l, e
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl                      ; row*32
    ld a, d
    add a, l
    ld l, a
    jr nc, pc_1
    inc h
pc_1:
    add hl, hl                      ; *2
    ld de, NAME_BASE
    add hl, de
    call vram_set_addr
    ld a, c
    out (VDP_DATA), a
    xor a
    out (VDP_DATA), a
pc_out:
    pop hl
    pop de
    pop bc
    ret

; ---- dirty_proc: A = proc. Mark only that app's window dirty (AUDIT-sms §1 P2):
; find the visible window owning proc A, set the clip to its rect so the deferred
; redraw_all repaints just that window (redraw_all still composites all windows
; bottom-up within the clip, so z-order stays correct -> byte-identical), then
; set v_dirty. Falls back to a full-screen dirty if the window isn't found. ----
dirty_proc:
    ld c, a                         ; c = target proc
    ld b, 0                         ; b = slot
dpr_loop:
    ld a, b
    cp MAXWIN
    jr nc, dpr_full                 ; not found -> full redraw (safe)
    push bc
    ld a, b
    call win_ptr                    ; ix = wintab + slot*16
    ld a, (ix+WSTATE)
    or a
    jr z, dpr_next                  ; not a live window
    ld a, (ix+WPROC)
    cp c
    jr z, dpr_found
dpr_next:
    pop bc
    inc b
    jr dpr_loop
dpr_found:
    ld a, (ix+WX)
    ld (v_clip_c0), a
    ld d, a
    ld a, (ix+WW)
    add a, d
    ld (v_clip_c1), a
    ld a, (ix+WY)
    ld (v_clip_r0), a
    ld d, a
    ld a, (ix+WH)
    add a, d
    ld (v_clip_r1), a
    pop bc
    ld a, 1
    ld (v_dirty), a
    ret
dpr_full:
    call clip_reset
    ld a, 1
    ld (v_dirty), a
    ret

; ---- fill_cells: dp_col,dp_row,dp_w,dp_h,dp_tile. Clipped to the clip window
; (default = whole screen), so a clipped redraw_all touches only the damage. ---
fill_cells:
    ; --- clip the rect (dp_col,dp_row,dp_w,dp_h) to [c0,c1) x [r0,r1) ---
    ; horizontal: right=col+w; col=max(col,c0); right=min(right,c1); w=right-col
    ld a, (dp_col)
    ld b, a
    ld a, (dp_w)
    add a, b
    ld c, a                         ; c = right
    ld a, (v_clip_c0)
    ld b, a
    ld a, (dp_col)
    cp b
    jr nc, fc_c0ok
    ld a, b                         ; col = c0
    ld (dp_col), a
fc_c0ok:
    ld a, (v_clip_c1)
    cp c
    jr nc, fc_c1ok
    ld c, a                         ; right = c1
fc_c1ok:
    ld a, c
    ld b, a
    ld a, (dp_col)
    ld c, a
    ld a, b
    sub c                           ; w = right - col
    ret c                           ; right < col -> nothing
    ret z
    ld (dp_w), a
    ; vertical: bottom=row+h; row=max(row,r0); bottom=min(bottom,r1); h=bottom-row
    ld a, (dp_row)
    ld b, a
    ld a, (dp_h)
    add a, b
    ld c, a                         ; c = bottom
    ld a, (v_clip_r0)
    ld b, a
    ld a, (dp_row)
    cp b
    jr nc, fc_r0ok
    ld a, b
    ld (dp_row), a
fc_r0ok:
    ld a, (v_clip_r1)
    cp c
    jr nc, fc_r1ok
    ld c, a
fc_r1ok:
    ld a, c
    ld b, a
    ld a, (dp_row)
    ld c, a
    ld a, b
    sub c                           ; h = bottom - row
    ret c
    ret z
    ld (dp_h), a
    ; --- draw ---
    ld b, a                         ; rows
fc_rl:
    push bc
    ld a, (dp_col)
    ld b, a
    ld a, (dp_row)
    ld c, a
    call cell_addr
    call vram_set_addr
    ld a, (dp_w)
    ld b, a
    ld a, (dp_tile)
fc_cl:
    out (VDP_DATA), a
    push af
    xor a
    out (VDP_DATA), a
    pop af
    djnz fc_cl
    ld a, (dp_row)
    inc a
    ld (dp_row), a
    pop bc
    djnz fc_rl
    ret

; ---- puts_at: D=col E=row, HL=str (NUL-term), C=tile base for ASCII 32 ----
puts_at:
    ; --- row clip: draw nothing unless r0 <= row(e) < r1 ---
    ld a, (v_clip_r0)
    ld b, a
    ld a, e
    cp b
    ret c                           ; e < r0
    ld a, (v_clip_r1)
    ld b, a
    ld a, e
    cp b
    ret nc                          ; e >= r1
    ; --- left clip: skip chars left of c0, advancing col(d) to c0 ---
    ld a, (v_clip_c0)
    ld b, a
    ld a, d
    cp b
    jr nc, pa_lclr                  ; d >= c0
pa_lcl:
    ld a, (hl)
    or a
    ret z                           ; string ends before the clip window
    inc hl
    inc d
    ld a, (v_clip_c0)
    ld b, a
    ld a, d
    cp b
    jr c, pa_lcl                    ; d < c0
pa_lclr:
    ; avail = c1 - d cells before the right clip edge
    ld a, (v_clip_c1)
    sub d
    ret c
    ret z
    push af                         ; save avail
    push hl
    ld h, 0
    ld l, e
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl                      ; row*32
    ld b, 0
    ld a, d
    add a, l
    ld l, a
    ld a, b
    adc a, h
    ld h, a                         ; +col
    add hl, hl                      ; *2
    ld de, NAME_BASE
    add hl, de
    call vram_set_addr
    pop hl
    pop af
    ld b, a                         ; b = remaining cells (right clip)
ps_loop:
    ld a, (hl)
    or a
    ret z
    sub 32
    add a, c
    out (VDP_DATA), a
    xor a
    out (VDP_DATA), a
    inc hl
    djnz ps_loop
    ret

; ---- cput: A=content line, HL=string -> draw at (WX+1, WY+1+line) ----------
cput:
    push hl
    ld d, (ix+WX)
    inc d
    add a, (ix+WY)
    inc a
    ld e, a
    pop hl
    ld c, T_FONT
    jp puts_at

; ============================================================================
; Apps (M3): content renderers dispatched by WPROC.
; ============================================================================
app_draw_content:
    ld a, (ix+WPROC)
    or a
    jp z, sysinfo_draw
    cp 1
    jp z, clock_draw
    cp 2
    jp z, notepad_draw
    cp 3
    jp z, music_draw
    cp 4
    jp z, files_draw
    cp 5
    jp z, theme_draw
    cp 7
    jp z, dostris_draw
    cp 10
    jp z, paint_draw
    cp 6
    jp z, tracker_draw
    cp 8
    jp z, outlast_draw
    cp 9
    jp z, pacman_draw
    jp generic_draw

sysinfo_draw:
    ld a, 0
    ld hl, si_l0
    call cput
    ld a, 1
    ld hl, si_l1
    call cput
    ld a, 2
    ld hl, si_l2
    call cput
    ld a, 3
    ld hl, si_l3
    call cput
    ld a, 4
    ld hl, si_l4
    call cput
    ret

clock_draw:
    call clock_format
    ld a, 0
    ld hl, cl_lbl
    call cput
    ld a, 2
    ld hl, clk_str
    call cput
    ret

notepad_draw:
    ld a, 0
    ld hl, np_l0
    call cput
    ld a, 1
    ld hl, np_l1
    call cput
    ld a, 2
    ld hl, np_l2
    call cput
    ld a, 4
    ld hl, np_l3
    call cput
    ret

files_draw:
    ld a, 0
    ld hl, fi_l0
    call cput
    ld a, 2
    ld hl, fi_l1
    call cput
    ld a, 3
    ld hl, fi_l2
    call cput
    ld a, 4
    ld hl, fi_l3
    call cput
    ld a, 5
    ld hl, fi_l4
    call cput
    ret

theme_draw:
    ld a, 0
    ld hl, th_l0
    call cput
    ld a, 2
    ld hl, th_l1
    call cput
    ld a, 3
    ld hl, th_l2
    call cput
    ; preset digit
    ld a, (v_theme_sel)
    add a, '0'
    ld (th_dig), a
    xor a
    ld (th_dig+1), a
    ld a, 4
    ld hl, th_dig
    call cput
    ret

generic_draw:
    ld a, 0
    ld l, (ix+WTITLE)
    ld h, (ix+WTITLE+1)
    call cput
    ld a, 2
    ld hl, ge_l0
    call cput
    ld a, 3
    ld hl, ge_l1
    call cput
    ret

; ---- clock_format: build clk_str "HH:MM:SS" from v_hh/v_mm/v_ss -----------
clock_format:
    ld hl, clk_str
    ld a, (v_hh)
    call put2
    ld (hl), ':'
    inc hl
    ld a, (v_mm)
    call put2
    ld (hl), ':'
    inc hl
    ld a, (v_ss)
    call put2
    ld (hl), 0
    ret
; put2: A = 0..99 -> two ASCII digits at (HL), HL += 2
put2:
    ld b, '0'-1
pt_t:
    inc b
    sub 10
    jr nc, pt_t
    add a, 10
    ld (hl), b
    inc hl
    add a, '0'
    ld (hl), a
    inc hl
    ret

; ---- theme: cycle the desktop palette live --------------------------------
theme_cycle:
    ld a, (v_theme_sel)
    inc a
    cp NTHEMES
    jr c, tc_s
    xor a
tc_s:
    ld (v_theme_sel), a
    ; fall through
apply_theme:
    ld a, (v_theme_sel)
    ld l, a
    ld h, 0
    ld de, theme_presets
    add hl, de
    ld c, (hl)                      ; desktop colour byte
    xor a
    out (VDP_CTRL), a
    ld a, $C0
    out (VDP_CTRL), a
    ld a, c
    out (VDP_DATA), a               ; CRAM[0]
    ld a, 2
    out (VDP_CTRL), a
    ld a, $C0
    out (VDP_CTRL), a
    ld a, c
    out (VDP_DATA), a               ; CRAM[2] (font bg follows the desktop)
    ret

; ============================================================================
; Per-frame app ticks (the COOP scheduler floor): the clock advances time and
; live-updates its open window once a second.
; ============================================================================
app_ticks:
    ld a, (v_frac)
    inc a
    cp 60
    jr nc, at_second
    ld (v_frac), a
    ret
at_second:
    xor a
    ld (v_frac), a
    ld a, (v_ss)
    inc a
    cp 60
    jr nc, at_minute
    ld (v_ss), a
    jr at_changed
at_minute:
    xor a
    ld (v_ss), a
    ld a, (v_mm)
    inc a
    cp 60
    jr nc, at_hour
    ld (v_mm), a
    jr at_changed
at_hour:
    xor a
    ld (v_mm), a
    ld a, (v_hh)
    inc a
    cp 24
    jr nc, at_daywrap
    ld (v_hh), a
    jr at_changed
at_daywrap:
    xor a
    ld (v_hh), a
at_changed:
    xor a
    ld (la_i), a
clu_scan:
    ld a, (la_i)
    cp MAXWIN
    ret nc
    call win_ptr
    ld a, (ix+WSTATE)
    or a
    jr z, clu_next
    ld a, (ix+WPROC)
    cp 1
    jr nz, clu_next
    call clock_format
    ld a, 2
    ld hl, clk_str
    call cput
    ret
clu_next:
    ld a, (la_i)
    inc a
    ld (la_i), a
    jr clu_scan

; ============================================================================
; AUTOTEST: drive a scripted pad sequence into the same input path so a single
; screenshot proves the WM. The ROM simulates the pad; nothing is faked.
; Each script step = db frames, curx, cury, pad. frames=0 ends (then idle).
; ============================================================================
  IFDEF AUTOTEST
auto_input:
    ld a, (v_auto_tmr)
    or a
    jr nz, ai_run
    ld a, (v_auto_idx)
    add a, a
    add a, a                        ; *4
    ld e, a
    ld d, 0
    ld hl, auto_script
    add hl, de
    ld a, (hl)                      ; frames
    or a
    jr z, ai_hold
    ld (v_auto_tmr), a
    inc hl
    ld a, (hl)
    ld (v_curx), a
    inc hl
    ld a, (hl)
    ld (v_cury), a
    inc hl
    ld a, (hl)
    ld (ai_pad), a
    ld a, (v_auto_idx)
    inc a
    ld (v_auto_idx), a
ai_run:
    ld a, (v_auto_tmr)
    dec a
    ld (v_auto_tmr), a
    ld a, (v_pad)
    ld (v_pad_prev), a
    ld a, (ai_pad)
    ld (v_pad), a
    ld a, (v_pad_prev)
    cpl
    ld b, a
    ld a, (v_pad)
    and b
    ld (v_pad_edge), a
    ret
ai_hold:
    ld a, 1
    ld (v_gpause), a                ; script done: freeze game gravity for a stable shot
    ld a, (v_pad)
    ld (v_pad_prev), a
    xor a
    ld (ai_pad), a
    ld (v_pad), a
    ld (v_pad_edge), a
    ret

B1 EQU PAD_B1
  IFNDEF AUTOTEST_DOSTRIS
  IFNDEF AUTOTEST_MUSIC
  IFNDEF AUTOTEST_PAINT
  IFNDEF AUTOTEST_TRACKER
  IFNDEF AUTOTEST_OUTLAST
  IFNDEF AUTOTEST_PACMAN
; Open Clock, SysInfo, Theme (in an order that keeps each icon clickable),
; cycle the Theme palette, then drag the Theme window by its title bar — one
; screenshot that exercises launch + z-order + live content + theme + drag.
auto_script:
    db 5, 76, 28, 0                ; settle on the Clock icon
    db 2, 76, 28, B1               ; click -> launch Clock (top-right)
    db 4, 76, 28, 0
    db 2, 12, 28, 0                ; move to the SysInfo icon
    db 2, 12, 28, B1               ; click -> launch SysInfo
    db 4, 12, 28, 0
    db 2, 76, 86, 0                ; move to the Theme icon
    db 2, 76, 86, B1               ; click -> launch Theme
    db 4, 76, 86, 0
    db 2, 96,108, B1               ; click inside Theme content -> cycle palette
    db 4, 96,108, 0
    db 2, 84, 90, B1               ; grab the Theme title bar (begin drag)
    db 2, 70,120, B1               ; drag down-left (window follows the cursor)
    db 2, 52,150, B1               ; drag further
    db 4, 52,150, 0                ; release (drop)
    db 40, 52,150, 0               ; settle for the capture
    db 0, 0, 0, 0                  ; end -> idle
  ENDIF ; AUTOTEST_PACMAN
  ENDIF ; AUTOTEST_OUTLAST
  ENDIF ; AUTOTEST_TRACKER
  ENDIF ; AUTOTEST_PAINT
  ENDIF ; AUTOTEST_MUSIC
  ENDIF ; AUTOTEST_DOSTRIS
  ENDIF ; AUTOTEST

; ============================================================================
; Data: strings, the desktop icon labels, the app-window definition table.
; ============================================================================
s_title:    db "UnoDOS 3",0
s_closebox: db "X",0

; icon label table (parallel to mkdata.py ICON order + paint); these strings
; double as window titles in app_def_tab.
icon_tbl:
    dw l_sysinfo, l_clock, l_notepad, l_music, l_files, l_theme
    dw l_tracker, l_dostris, l_outlast, l_pacman, l_paint
l_sysinfo:  db "SysInfo",0
l_clock:    db "Clock",0
l_notepad:  db "Notepad",0
l_music:    db "Music",0
l_files:    db "Files",0
l_theme:    db "Theme",0
l_tracker:  db "Tracker",0
l_dostris:  db "Dostris",0
l_outlast:  db "OutLast",0
l_pacman:   db "Pac-Man",0
l_paint:    db "Paint",0

; app_def_tab: per proc -> db x,y,w,h ; dw title  (cells)
app_def_tab:
    db  2, 2,18, 8
    dw l_sysinfo
    db 15, 4,13, 6
    dw l_clock
    db  3,12,22,10
    dw l_notepad
    db  6, 6,18,10
    dw l_music
    db  4,10,16,11
    dw l_files
    db  9,11,18, 9
    dw l_theme
    db  4, 3,22,18
    dw l_tracker
    db  6, 4,20,14
    dw l_dostris
    db  4, 4,24,13
    dw l_outlast
    db  5, 3,22,18
    dw l_pacman
    db  4, 4,22,16
    dw l_paint

; ---- app content strings -----
si_l0: db "UnoDOS 3 - SMS",0
si_l1: db "Z80 @ 3.58MHz",0
si_l2: db "VDP 315-5124",0
si_l3: db "256x192 Mode4",0
si_l4: db "8KB work RAM",0

cl_lbl: db "System clock",0

np_l0: db "Notepad",0
np_l1: db "The first UnoDOS",0
np_l2: db "port on Z80.",0
np_l3: db "- contract-driven",0

fi_l0: db "Files - apps:",0
fi_l1: db "SYSINFO  CLOCK",0
fi_l2: db "NOTEPAD  FILES",0
fi_l3: db "THEME    MUSIC",0
fi_l4: db "(no disk yet)",0

th_l0: db "Theme presets",0
th_l1: db "Click to cycle",0
th_l2: db "Preset:",0

ge_l0: db "UnoDOS 3 / SMS",0
ge_l1: db "(demo window)",0

; theme presets: the desktop background colour (SMS %00BBGGRR)
NTHEMES EQU 4
theme_presets:
    db $20      ; blue
    db $08      ; green
    db $02      ; maroon
    db $15      ; grey
