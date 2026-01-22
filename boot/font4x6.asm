; UnoDOS 3 - 4x6 Bitmap Font (Small)
; Complete ASCII character set (32-126)
; Each character is 6 bytes, one byte per row
; Only upper 4 bits used (bits 7-4), lower bits ignored
; Bit 7 = leftmost pixel, Bit 4 = rightmost pixel

; Font table base - characters are accessed as:
; font_4x6 + (char - 32) * 6

font_4x6:

; ASCII 32: Space
char4_space:
    db 0b00000000
    db 0b00000000
    db 0b00000000
    db 0b00000000
    db 0b00000000
    db 0b00000000

; ASCII 33: !
char4_exclaim:
    db 0b01000000
    db 0b01000000
    db 0b01000000
    db 0b00000000
    db 0b01000000
    db 0b00000000

; ASCII 34: "
char4_dquote:
    db 0b10100000
    db 0b10100000
    db 0b00000000
    db 0b00000000
    db 0b00000000
    db 0b00000000

; ASCII 35: #
char4_hash:
    db 0b10100000
    db 0b11110000
    db 0b10100000
    db 0b11110000
    db 0b10100000
    db 0b00000000

; ASCII 36: $
char4_dollar:
    db 0b01000000
    db 0b01110000
    db 0b11000000
    db 0b00110000
    db 0b11100000
    db 0b01000000

; ASCII 37: %
char4_percent:
    db 0b10010000
    db 0b00100000
    db 0b01000000
    db 0b10010000
    db 0b00000000
    db 0b00000000

; ASCII 38: &
char4_ampersand:
    db 0b01000000
    db 0b10100000
    db 0b01000000
    db 0b10100000
    db 0b01010000
    db 0b00000000

; ASCII 39: '
char4_squote:
    db 0b01000000
    db 0b01000000
    db 0b00000000
    db 0b00000000
    db 0b00000000
    db 0b00000000

; ASCII 40: (
char4_lparen:
    db 0b00100000
    db 0b01000000
    db 0b01000000
    db 0b01000000
    db 0b00100000
    db 0b00000000

; ASCII 41: )
char4_rparen:
    db 0b01000000
    db 0b00100000
    db 0b00100000
    db 0b00100000
    db 0b01000000
    db 0b00000000

; ASCII 42: *
char4_asterisk:
    db 0b00000000
    db 0b10100000
    db 0b01000000
    db 0b10100000
    db 0b00000000
    db 0b00000000

; ASCII 43: +
char4_plus:
    db 0b00000000
    db 0b01000000
    db 0b11100000
    db 0b01000000
    db 0b00000000
    db 0b00000000

; ASCII 44: ,
char4_comma:
    db 0b00000000
    db 0b00000000
    db 0b00000000
    db 0b01000000
    db 0b01000000
    db 0b10000000

; ASCII 45: -
char4_minus:
    db 0b00000000
    db 0b00000000
    db 0b11100000
    db 0b00000000
    db 0b00000000
    db 0b00000000

; ASCII 46: .
char4_period:
    db 0b00000000
    db 0b00000000
    db 0b00000000
    db 0b00000000
    db 0b01000000
    db 0b00000000

; ASCII 47: /
char4_slash:
    db 0b00010000
    db 0b00100000
    db 0b01000000
    db 0b10000000
    db 0b00000000
    db 0b00000000

; ASCII 48: 0
char4_0:
    db 0b01100000
    db 0b10010000
    db 0b10010000
    db 0b10010000
    db 0b01100000
    db 0b00000000

; ASCII 49: 1
char4_1:
    db 0b00100000
    db 0b01100000
    db 0b00100000
    db 0b00100000
    db 0b01110000
    db 0b00000000

; ASCII 50: 2
char4_2:
    db 0b01100000
    db 0b10010000
    db 0b00100000
    db 0b01000000
    db 0b11110000
    db 0b00000000

; ASCII 51: 3
char4_3:
    db 0b11100000
    db 0b00010000
    db 0b01100000
    db 0b00010000
    db 0b11100000
    db 0b00000000

; ASCII 52: 4
char4_4:
    db 0b10010000
    db 0b10010000
    db 0b11110000
    db 0b00010000
    db 0b00010000
    db 0b00000000

; ASCII 53: 5
char4_5:
    db 0b11110000
    db 0b10000000
    db 0b11100000
    db 0b00010000
    db 0b11100000
    db 0b00000000

; ASCII 54: 6
char4_6:
    db 0b01100000
    db 0b10000000
    db 0b11100000
    db 0b10010000
    db 0b01100000
    db 0b00000000

; ASCII 55: 7
char4_7:
    db 0b11110000
    db 0b00010000
    db 0b00100000
    db 0b01000000
    db 0b01000000
    db 0b00000000

; ASCII 56: 8
char4_8:
    db 0b01100000
    db 0b10010000
    db 0b01100000
    db 0b10010000
    db 0b01100000
    db 0b00000000

; ASCII 57: 9
char4_9:
    db 0b01100000
    db 0b10010000
    db 0b01110000
    db 0b00010000
    db 0b01100000
    db 0b00000000

; ASCII 58: :
char4_colon:
    db 0b00000000
    db 0b01000000
    db 0b00000000
    db 0b01000000
    db 0b00000000
    db 0b00000000

; ASCII 59: ;
char4_semicolon:
    db 0b00000000
    db 0b01000000
    db 0b00000000
    db 0b01000000
    db 0b10000000
    db 0b00000000

; ASCII 60: <
char4_less:
    db 0b00100000
    db 0b01000000
    db 0b10000000
    db 0b01000000
    db 0b00100000
    db 0b00000000

; ASCII 61: =
char4_equal:
    db 0b00000000
    db 0b11100000
    db 0b00000000
    db 0b11100000
    db 0b00000000
    db 0b00000000

; ASCII 62: >
char4_greater:
    db 0b10000000
    db 0b01000000
    db 0b00100000
    db 0b01000000
    db 0b10000000
    db 0b00000000

; ASCII 63: ?
char4_question:
    db 0b01100000
    db 0b00010000
    db 0b00100000
    db 0b00000000
    db 0b00100000
    db 0b00000000

; ASCII 64: @
char4_at:
    db 0b01100000
    db 0b10010000
    db 0b10110000
    db 0b10000000
    db 0b01100000
    db 0b00000000

; ASCII 65: A
char4_A:
    db 0b01100000
    db 0b10010000
    db 0b11110000
    db 0b10010000
    db 0b10010000
    db 0b00000000

; ASCII 66: B
char4_B:
    db 0b11100000
    db 0b10010000
    db 0b11100000
    db 0b10010000
    db 0b11100000
    db 0b00000000

; ASCII 67: C
char4_C:
    db 0b01110000
    db 0b10000000
    db 0b10000000
    db 0b10000000
    db 0b01110000
    db 0b00000000

; ASCII 68: D
char4_D:
    db 0b11100000
    db 0b10010000
    db 0b10010000
    db 0b10010000
    db 0b11100000
    db 0b00000000

; ASCII 69: E
char4_E:
    db 0b11110000
    db 0b10000000
    db 0b11100000
    db 0b10000000
    db 0b11110000
    db 0b00000000

; ASCII 70: F
char4_F:
    db 0b11110000
    db 0b10000000
    db 0b11100000
    db 0b10000000
    db 0b10000000
    db 0b00000000

; ASCII 71: G
char4_G:
    db 0b01110000
    db 0b10000000
    db 0b10110000
    db 0b10010000
    db 0b01100000
    db 0b00000000

; ASCII 72: H
char4_H:
    db 0b10010000
    db 0b10010000
    db 0b11110000
    db 0b10010000
    db 0b10010000
    db 0b00000000

; ASCII 73: I
char4_I:
    db 0b11100000
    db 0b01000000
    db 0b01000000
    db 0b01000000
    db 0b11100000
    db 0b00000000

; ASCII 74: J
char4_J:
    db 0b00110000
    db 0b00010000
    db 0b00010000
    db 0b10010000
    db 0b01100000
    db 0b00000000

; ASCII 75: K
char4_K:
    db 0b10010000
    db 0b10100000
    db 0b11000000
    db 0b10100000
    db 0b10010000
    db 0b00000000

; ASCII 76: L
char4_L:
    db 0b10000000
    db 0b10000000
    db 0b10000000
    db 0b10000000
    db 0b11110000
    db 0b00000000

; ASCII 77: M
char4_M:
    db 0b10010000
    db 0b11110000
    db 0b10010000
    db 0b10010000
    db 0b10010000
    db 0b00000000

; ASCII 78: N
char4_N:
    db 0b10010000
    db 0b11010000
    db 0b10110000
    db 0b10010000
    db 0b10010000
    db 0b00000000

; ASCII 79: O
char4_O:
    db 0b01100000
    db 0b10010000
    db 0b10010000
    db 0b10010000
    db 0b01100000
    db 0b00000000

; ASCII 80: P
char4_P:
    db 0b11100000
    db 0b10010000
    db 0b11100000
    db 0b10000000
    db 0b10000000
    db 0b00000000

; ASCII 81: Q
char4_Q:
    db 0b01100000
    db 0b10010000
    db 0b10010000
    db 0b10110000
    db 0b01110000
    db 0b00000000

; ASCII 82: R
char4_R:
    db 0b11100000
    db 0b10010000
    db 0b11100000
    db 0b10100000
    db 0b10010000
    db 0b00000000

; ASCII 83: S
char4_S:
    db 0b01110000
    db 0b10000000
    db 0b01100000
    db 0b00010000
    db 0b11100000
    db 0b00000000

; ASCII 84: T
char4_T:
    db 0b11100000
    db 0b01000000
    db 0b01000000
    db 0b01000000
    db 0b01000000
    db 0b00000000

; ASCII 85: U
char4_U:
    db 0b10010000
    db 0b10010000
    db 0b10010000
    db 0b10010000
    db 0b01100000
    db 0b00000000

; ASCII 86: V
char4_V:
    db 0b10010000
    db 0b10010000
    db 0b10010000
    db 0b01100000
    db 0b01100000
    db 0b00000000

; ASCII 87: W
char4_W:
    db 0b10010000
    db 0b10010000
    db 0b10010000
    db 0b11110000
    db 0b10010000
    db 0b00000000

; ASCII 88: X
char4_X:
    db 0b10010000
    db 0b01100000
    db 0b01100000
    db 0b01100000
    db 0b10010000
    db 0b00000000

; ASCII 89: Y
char4_Y:
    db 0b10010000
    db 0b10010000
    db 0b01100000
    db 0b01000000
    db 0b01000000
    db 0b00000000

; ASCII 90: Z
char4_Z:
    db 0b11110000
    db 0b00100000
    db 0b01000000
    db 0b10000000
    db 0b11110000
    db 0b00000000

; ASCII 91: [
char4_lbracket:
    db 0b01100000
    db 0b01000000
    db 0b01000000
    db 0b01000000
    db 0b01100000
    db 0b00000000

; ASCII 92: \
char4_backslash:
    db 0b10000000
    db 0b01000000
    db 0b00100000
    db 0b00010000
    db 0b00000000
    db 0b00000000

; ASCII 93: ]
char4_rbracket:
    db 0b01100000
    db 0b00100000
    db 0b00100000
    db 0b00100000
    db 0b01100000
    db 0b00000000

; ASCII 94: ^
char4_caret:
    db 0b01000000
    db 0b10100000
    db 0b00000000
    db 0b00000000
    db 0b00000000
    db 0b00000000

; ASCII 95: _
char4_underscore:
    db 0b00000000
    db 0b00000000
    db 0b00000000
    db 0b00000000
    db 0b00000000
    db 0b11110000

; ASCII 96: `
char4_backtick:
    db 0b10000000
    db 0b01000000
    db 0b00000000
    db 0b00000000
    db 0b00000000
    db 0b00000000

; ASCII 97: a
char4_a:
    db 0b00000000
    db 0b01100000
    db 0b00010000
    db 0b01110000
    db 0b01110000
    db 0b00000000

; ASCII 98: b
char4_b:
    db 0b10000000
    db 0b11100000
    db 0b10010000
    db 0b10010000
    db 0b11100000
    db 0b00000000

; ASCII 99: c
char4_c:
    db 0b00000000
    db 0b01110000
    db 0b10000000
    db 0b10000000
    db 0b01110000
    db 0b00000000

; ASCII 100: d
char4_d:
    db 0b00010000
    db 0b01110000
    db 0b10010000
    db 0b10010000
    db 0b01110000
    db 0b00000000

; ASCII 101: e
char4_e:
    db 0b00000000
    db 0b01100000
    db 0b10110000
    db 0b11000000
    db 0b01100000
    db 0b00000000

; ASCII 102: f
char4_f:
    db 0b00110000
    db 0b01000000
    db 0b11100000
    db 0b01000000
    db 0b01000000
    db 0b00000000

; ASCII 103: g
char4_g:
    db 0b00000000
    db 0b01110000
    db 0b10010000
    db 0b01110000
    db 0b00010000
    db 0b01100000

; ASCII 104: h
char4_h:
    db 0b10000000
    db 0b11100000
    db 0b10010000
    db 0b10010000
    db 0b10010000
    db 0b00000000

; ASCII 105: i
char4_i:
    db 0b01000000
    db 0b00000000
    db 0b01000000
    db 0b01000000
    db 0b01000000
    db 0b00000000

; ASCII 106: j
char4_j:
    db 0b00100000
    db 0b00000000
    db 0b00100000
    db 0b00100000
    db 0b00100000
    db 0b11000000

; ASCII 107: k
char4_k:
    db 0b10000000
    db 0b10100000
    db 0b11000000
    db 0b11000000
    db 0b10100000
    db 0b00000000

; ASCII 108: l
char4_l:
    db 0b01000000
    db 0b01000000
    db 0b01000000
    db 0b01000000
    db 0b00110000
    db 0b00000000

; ASCII 109: m
char4_m:
    db 0b00000000
    db 0b11100000
    db 0b10110000
    db 0b10010000
    db 0b10010000
    db 0b00000000

; ASCII 110: n
char4_n:
    db 0b00000000
    db 0b11100000
    db 0b10010000
    db 0b10010000
    db 0b10010000
    db 0b00000000

; ASCII 111: o
char4_o:
    db 0b00000000
    db 0b01100000
    db 0b10010000
    db 0b10010000
    db 0b01100000
    db 0b00000000

; ASCII 112: p
char4_p:
    db 0b00000000
    db 0b11100000
    db 0b10010000
    db 0b11100000
    db 0b10000000
    db 0b10000000

; ASCII 113: q
char4_q:
    db 0b00000000
    db 0b01110000
    db 0b10010000
    db 0b01110000
    db 0b00010000
    db 0b00010000

; ASCII 114: r
char4_r:
    db 0b00000000
    db 0b10110000
    db 0b11000000
    db 0b10000000
    db 0b10000000
    db 0b00000000

; ASCII 115: s
char4_s:
    db 0b00000000
    db 0b01110000
    db 0b11000000
    db 0b00110000
    db 0b11100000
    db 0b00000000

; ASCII 116: t
char4_t:
    db 0b01000000
    db 0b11100000
    db 0b01000000
    db 0b01000000
    db 0b00110000
    db 0b00000000

; ASCII 117: u
char4_u:
    db 0b00000000
    db 0b10010000
    db 0b10010000
    db 0b10010000
    db 0b01110000
    db 0b00000000

; ASCII 118: v
char4_v:
    db 0b00000000
    db 0b10010000
    db 0b10010000
    db 0b01100000
    db 0b01100000
    db 0b00000000

; ASCII 119: w
char4_w:
    db 0b00000000
    db 0b10010000
    db 0b10010000
    db 0b10110000
    db 0b11100000
    db 0b00000000

; ASCII 120: x
char4_x:
    db 0b00000000
    db 0b10010000
    db 0b01100000
    db 0b01100000
    db 0b10010000
    db 0b00000000

; ASCII 121: y
char4_y:
    db 0b00000000
    db 0b10010000
    db 0b10010000
    db 0b01110000
    db 0b00010000
    db 0b01100000

; ASCII 122: z
char4_z:
    db 0b00000000
    db 0b11110000
    db 0b00100000
    db 0b01000000
    db 0b11110000
    db 0b00000000

; ASCII 123: {
char4_lbrace:
    db 0b00100000
    db 0b01000000
    db 0b11000000
    db 0b01000000
    db 0b00100000
    db 0b00000000

; ASCII 124: |
char4_pipe:
    db 0b01000000
    db 0b01000000
    db 0b00000000
    db 0b01000000
    db 0b01000000
    db 0b00000000

; ASCII 125: }
char4_rbrace:
    db 0b10000000
    db 0b01000000
    db 0b01100000
    db 0b01000000
    db 0b10000000
    db 0b00000000

; ASCII 126: ~
char4_tilde:
    db 0b01010000
    db 0b10100000
    db 0b00000000
    db 0b00000000
    db 0b00000000
    db 0b00000000

font_4x6_end:
