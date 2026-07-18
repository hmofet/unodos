; ============================================================================
; UnoDOS/SMS — PSG audio + the Music app.
; The SMS sound chip is the SN76489 PSG (same family as the Genesis), written
; through I/O port $7F: 3 square-wave tone channels + 1 noise. The Music app
; plays a tune on channel 0, advancing one note per its frame duration; the
; window shows a progress bar so the screenshot proves the engine is running.
; (The actual timbre is an ear-check, like every other UnoDOS port's audio.)
; ============================================================================

PSG_PORT    EQU $7F

; ---- psg_play0: HL = 10-bit tone period -> channel 0 tone + full volume -----
psg_play0:
    ld a, l
    and $0F
    or $80                          ; latch ch0 tone, low 4 bits
    out (PSG_PORT), a
    srl h
    rr l
    srl h
    rr l
    srl h
    rr l
    srl h
    rr l                            ; HL >>= 4
    ld a, l
    and $3F
    out (PSG_PORT), a               ; data: high 6 bits
    ld a, $90                       ; ch0 volume = 0 (loudest)
    out (PSG_PORT), a
    ret

; ---- psg_silence: attenuate all four channels to off ----------------------
psg_silence:
    ld a, $9F
    out (PSG_PORT), a               ; ch0 off
    ld a, $BF
    out (PSG_PORT), a               ; ch1 off
    ld a, $DF
    out (PSG_PORT), a               ; ch2 off
    ld a, $FF
    out (PSG_PORT), a               ; ch3 (noise) off
    ret

; ---- music_window_open: -> A = 1 if a visible Music window exists ----------
music_window_open:
    xor a
    ld (g_tmp), a
mwo_loop:
    ld a, (g_tmp)
    cp MAXWIN
    jr nc, mwo_no
    call win_ptr
    ld a, (ix+WSTATE)
    or a
    jr z, mwo_next
    ld a, (ix+WPROC)
    cp 3
    jr z, mwo_yes
mwo_next:
    ld a, (g_tmp)
    inc a
    ld (g_tmp), a
    jr mwo_loop
mwo_yes:
    ld a, 1
    ret
mwo_no:
    xor a
    ret

; ---- music_load_note: program the PSG with note (m_idx) + set m_timer ------
music_load_note:
    ld a, (m_idx)
    ld l, a
    ld h, 0
    add hl, hl
    ld d, h
    ld e, l
    add hl, de                      ; m_idx*3
    ld de, music_song
    add hl, de                      ; hl -> note (dw period, db frames)
    ld e, (hl)
    inc hl
    ld d, (hl)                      ; de = period
    inc hl
    ld a, (hl)
    ld (m_timer), a                 ; frames
    ex de, hl                       ; hl = period
    jp psg_play0

; ---- music_init: start the tune (on Music-window create) ------------------
music_init:
    xor a
    ld (m_idx), a
    ld a, 1
    ld (m_play), a
    jp music_load_note

; ---- music_tick: per-frame; advance the note, silence on close ------------
music_tick:
    call music_window_open
    or a
    jr nz, mt_open
    ld a, (m_play)                  ; window gone: silence once
    or a
    ret z
    xor a
    ld (m_play), a
    jp psg_silence
mt_open:
    ld a, (m_play)
    or a
    ret z
    ld a, (v_gpause)                ; AUTOTEST freeze: hold the current note
    or a
    ret nz
    ld a, (m_timer)
    dec a
    ld (m_timer), a
    or a
    ret nz
    ld a, (m_idx)
    inc a
    cp MUSIC_COUNT
    jr c, mt_set
    xor a                           ; loop the tune
mt_set:
    ld (m_idx), a
    call music_load_note
    ld a, 3                         ; refresh just the Music window (proc 3), not
    call dirty_proc                 ; the whole scene (AUDIT-sms §1 P2)
    ret

; ---- music_draw: content for the Music window (ix = window entry) ----------
music_draw:
    ld a, 0
    ld hl, mu_l0
    call cput
    ld a, 1
    ld hl, mu_title
    call cput
    ld a, 3
    ld hl, mu_l1
    call cput
    ; note number (1-based) at line 3, right of the label
    ld a, (m_idx)
    inc a
    ld hl, g_numstr
    call put2
    xor a
    ld (hl), a
    ld a, (ix+WX)
    add a, 9
    ld d, a
    ld a, (ix+WY)
    add a, 4
    ld e, a
    ld hl, g_numstr
    ld c, T_FONT
    call puts_at
    ; progress bar at line 5: m_idx cyan cells (capped at 16)
    ld a, (ix+WX)
    inc a
    ld (dp_col), a
    ld a, (ix+WY)
    add a, 6
    ld (dp_row), a
    ld a, (m_idx)
    cp 16
    jr c, md_w
    ld a, 16
md_w:
    or a
    jr nz, md_nz
    inc a                           ; always show at least 1 so the bar exists
md_nz:
    ld (dp_w), a
    ld a, 1
    ld (dp_h), a
    ld a, T_SOLC
    ld (dp_tile), a
    call fill_cells
    ret

mu_l0:    db "Music player",0
mu_title: db "Ode to Joy",0
mu_l1:    db "Note:",0

; ---- AUTOTEST: open the Music app and let the tune advance several notes ----
  IFDEF AUTOTEST_MUSIC
auto_script:
    db 2,208, 28, 0
    db 2,208, 28, PAD_B1           ; click the Music icon -> launch + play
    db 4,208, 28, 0
    db 200,128,96, 0               ; let ~8 notes play (engine advances by frame)
    db 0,0,0,0                     ; end -> freeze on the current note for the shot
  ENDIF
