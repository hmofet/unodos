; UnoDOS 3 - 8x8 Bitmap Font
; Complete ASCII character set (32-126)
; Each character is 8 bytes, one byte per row
; Bit 7 = leftmost pixel, Bit 0 = rightmost pixel

; Font table base - characters are accessed as:
; font_8x8 + (char - 32) * 8

font_8x8:

; ASCII 32: Space
char8_space:
    db 0b00000000
    db 0b00000000
    db 0b00000000
    db 0b00000000
    db 0b00000000
    db 0b00000000
    db 0b00000000
    db 0b00000000

; ASCII 33: !
char8_exclaim:
    db 0b00011000
    db 0b00011000
    db 0b00011000
    db 0b00011000
    db 0b00011000
    db 0b00000000
    db 0b00011000
    db 0b00000000

; ASCII 34: "
char8_dquote:
    db 0b01100110
    db 0b01100110
    db 0b01100110
    db 0b00000000
    db 0b00000000
    db 0b00000000
    db 0b00000000
    db 0b00000000

; ASCII 35: #
char8_hash:
    db 0b01100110
    db 0b01100110
    db 0b11111111
    db 0b01100110
    db 0b11111111
    db 0b01100110
    db 0b01100110
    db 0b00000000

; ASCII 36: $
char8_dollar:
    db 0b00011000
    db 0b00111110
    db 0b01100000
    db 0b00111100
    db 0b00000110
    db 0b01111100
    db 0b00011000
    db 0b00000000

; ASCII 37: %
char8_percent:
    db 0b01100010
    db 0b01100110
    db 0b00001100
    db 0b00011000
    db 0b00110000
    db 0b01100110
    db 0b01000110
    db 0b00000000

; ASCII 38: &
char8_ampersand:
    db 0b00111100
    db 0b01100110
    db 0b00111100
    db 0b00111000
    db 0b01100111
    db 0b01100110
    db 0b00111111
    db 0b00000000

; ASCII 39: '
char8_squote:
    db 0b00011000
    db 0b00011000
    db 0b00011000
    db 0b00000000
    db 0b00000000
    db 0b00000000
    db 0b00000000
    db 0b00000000

; ASCII 40: (
char8_lparen:
    db 0b00001100
    db 0b00011000
    db 0b00110000
    db 0b00110000
    db 0b00110000
    db 0b00011000
    db 0b00001100
    db 0b00000000

; ASCII 41: )
char8_rparen:
    db 0b00110000
    db 0b00011000
    db 0b00001100
    db 0b00001100
    db 0b00001100
    db 0b00011000
    db 0b00110000
    db 0b00000000

; ASCII 42: *
char8_asterisk:
    db 0b00000000
    db 0b01100110
    db 0b00111100
    db 0b11111111
    db 0b00111100
    db 0b01100110
    db 0b00000000
    db 0b00000000

; ASCII 43: +
char8_plus:
    db 0b00000000
    db 0b00011000
    db 0b00011000
    db 0b01111110
    db 0b00011000
    db 0b00011000
    db 0b00000000
    db 0b00000000

; ASCII 44: ,
char8_comma:
    db 0b00000000
    db 0b00000000
    db 0b00000000
    db 0b00000000
    db 0b00000000
    db 0b00011000
    db 0b00011000
    db 0b00110000

; ASCII 45: -
char8_minus:
    db 0b00000000
    db 0b00000000
    db 0b00000000
    db 0b01111110
    db 0b00000000
    db 0b00000000
    db 0b00000000
    db 0b00000000

; ASCII 46: .
char8_period:
    db 0b00000000
    db 0b00000000
    db 0b00000000
    db 0b00000000
    db 0b00000000
    db 0b00011000
    db 0b00011000
    db 0b00000000

; ASCII 47: /
char8_slash:
    db 0b00000010
    db 0b00000110
    db 0b00001100
    db 0b00011000
    db 0b00110000
    db 0b01100000
    db 0b01000000
    db 0b00000000

; ASCII 48: 0
char8_0:
    db 0b00111100
    db 0b01100110
    db 0b01101110
    db 0b01110110
    db 0b01100110
    db 0b01100110
    db 0b00111100
    db 0b00000000

; ASCII 49: 1
char8_1:
    db 0b00011000
    db 0b00111000
    db 0b00011000
    db 0b00011000
    db 0b00011000
    db 0b00011000
    db 0b01111110
    db 0b00000000

; ASCII 50: 2
char8_2:
    db 0b00111100
    db 0b01100110
    db 0b00000110
    db 0b00001100
    db 0b00110000
    db 0b01100000
    db 0b01111110
    db 0b00000000

; ASCII 51: 3
char8_3:
    db 0b00111100
    db 0b01100110
    db 0b00000110
    db 0b00011100
    db 0b00000110
    db 0b01100110
    db 0b00111100
    db 0b00000000

; ASCII 52: 4
char8_4:
    db 0b00001100
    db 0b00011100
    db 0b00111100
    db 0b01101100
    db 0b01111110
    db 0b00001100
    db 0b00001100
    db 0b00000000

; ASCII 53: 5
char8_5:
    db 0b01111110
    db 0b01100000
    db 0b01111100
    db 0b00000110
    db 0b00000110
    db 0b01100110
    db 0b00111100
    db 0b00000000

; ASCII 54: 6
char8_6:
    db 0b00111100
    db 0b01100110
    db 0b01100000
    db 0b01111100
    db 0b01100110
    db 0b01100110
    db 0b00111100
    db 0b00000000

; ASCII 55: 7
char8_7:
    db 0b01111110
    db 0b01100110
    db 0b00001100
    db 0b00011000
    db 0b00011000
    db 0b00011000
    db 0b00011000
    db 0b00000000

; ASCII 56: 8
char8_8:
    db 0b00111100
    db 0b01100110
    db 0b01100110
    db 0b00111100
    db 0b01100110
    db 0b01100110
    db 0b00111100
    db 0b00000000

; ASCII 57: 9
char8_9:
    db 0b00111100
    db 0b01100110
    db 0b01100110
    db 0b00111110
    db 0b00000110
    db 0b01100110
    db 0b00111100
    db 0b00000000

; ASCII 58: :
char8_colon:
    db 0b00000000
    db 0b00000000
    db 0b00011000
    db 0b00011000
    db 0b00000000
    db 0b00011000
    db 0b00011000
    db 0b00000000

; ASCII 59: ;
char8_semicolon:
    db 0b00000000
    db 0b00000000
    db 0b00011000
    db 0b00011000
    db 0b00000000
    db 0b00011000
    db 0b00011000
    db 0b00110000

; ASCII 60: <
char8_less:
    db 0b00000110
    db 0b00001100
    db 0b00011000
    db 0b00110000
    db 0b00011000
    db 0b00001100
    db 0b00000110
    db 0b00000000

; ASCII 61: =
char8_equal:
    db 0b00000000
    db 0b00000000
    db 0b01111110
    db 0b00000000
    db 0b01111110
    db 0b00000000
    db 0b00000000
    db 0b00000000

; ASCII 62: >
char8_greater:
    db 0b01100000
    db 0b00110000
    db 0b00011000
    db 0b00001100
    db 0b00011000
    db 0b00110000
    db 0b01100000
    db 0b00000000

; ASCII 63: ?
char8_question:
    db 0b00111100
    db 0b01100110
    db 0b00000110
    db 0b00001100
    db 0b00011000
    db 0b00000000
    db 0b00011000
    db 0b00000000

; ASCII 64: @
char8_at:
    db 0b00111100
    db 0b01100110
    db 0b01101110
    db 0b01101110
    db 0b01100000
    db 0b01100010
    db 0b00111100
    db 0b00000000

; ASCII 65: A
char8_A:
    db 0b00011000
    db 0b00111100
    db 0b01100110
    db 0b01111110
    db 0b01100110
    db 0b01100110
    db 0b01100110
    db 0b00000000

; ASCII 66: B
char8_B:
    db 0b01111100
    db 0b01100110
    db 0b01100110
    db 0b01111100
    db 0b01100110
    db 0b01100110
    db 0b01111100
    db 0b00000000

; ASCII 67: C
char8_C:
    db 0b00111100
    db 0b01100110
    db 0b01100000
    db 0b01100000
    db 0b01100000
    db 0b01100110
    db 0b00111100
    db 0b00000000

; ASCII 68: D
char8_D:
    db 0b01111000
    db 0b01101100
    db 0b01100110
    db 0b01100110
    db 0b01100110
    db 0b01101100
    db 0b01111000
    db 0b00000000

; ASCII 69: E
char8_E:
    db 0b01111110
    db 0b01100000
    db 0b01100000
    db 0b01111000
    db 0b01100000
    db 0b01100000
    db 0b01111110
    db 0b00000000

; ASCII 70: F
char8_F:
    db 0b01111110
    db 0b01100000
    db 0b01100000
    db 0b01111000
    db 0b01100000
    db 0b01100000
    db 0b01100000
    db 0b00000000

; ASCII 71: G
char8_G:
    db 0b00111100
    db 0b01100110
    db 0b01100000
    db 0b01101110
    db 0b01100110
    db 0b01100110
    db 0b00111100
    db 0b00000000

; ASCII 72: H
char8_H:
    db 0b01100110
    db 0b01100110
    db 0b01100110
    db 0b01111110
    db 0b01100110
    db 0b01100110
    db 0b01100110
    db 0b00000000

; ASCII 73: I
char8_I:
    db 0b00111100
    db 0b00011000
    db 0b00011000
    db 0b00011000
    db 0b00011000
    db 0b00011000
    db 0b00111100
    db 0b00000000

; ASCII 74: J
char8_J:
    db 0b00011110
    db 0b00001100
    db 0b00001100
    db 0b00001100
    db 0b00001100
    db 0b01101100
    db 0b00111000
    db 0b00000000

; ASCII 75: K
char8_K:
    db 0b01100110
    db 0b01101100
    db 0b01111000
    db 0b01110000
    db 0b01111000
    db 0b01101100
    db 0b01100110
    db 0b00000000

; ASCII 76: L
char8_L:
    db 0b01100000
    db 0b01100000
    db 0b01100000
    db 0b01100000
    db 0b01100000
    db 0b01100000
    db 0b01111110
    db 0b00000000

; ASCII 77: M
char8_M:
    db 0b01100011
    db 0b01110111
    db 0b01111111
    db 0b01101011
    db 0b01100011
    db 0b01100011
    db 0b01100011
    db 0b00000000

; ASCII 78: N
char8_N:
    db 0b01100110
    db 0b01110110
    db 0b01111110
    db 0b01111110
    db 0b01101110
    db 0b01100110
    db 0b01100110
    db 0b00000000

; ASCII 79: O
char8_O:
    db 0b00111100
    db 0b01100110
    db 0b01100110
    db 0b01100110
    db 0b01100110
    db 0b01100110
    db 0b00111100
    db 0b00000000

; ASCII 80: P
char8_P:
    db 0b01111100
    db 0b01100110
    db 0b01100110
    db 0b01111100
    db 0b01100000
    db 0b01100000
    db 0b01100000
    db 0b00000000

; ASCII 81: Q
char8_Q:
    db 0b00111100
    db 0b01100110
    db 0b01100110
    db 0b01100110
    db 0b01100110
    db 0b00111100
    db 0b00001110
    db 0b00000000

; ASCII 82: R
char8_R:
    db 0b01111100
    db 0b01100110
    db 0b01100110
    db 0b01111100
    db 0b01111000
    db 0b01101100
    db 0b01100110
    db 0b00000000

; ASCII 83: S
char8_S:
    db 0b00111100
    db 0b01100110
    db 0b01100000
    db 0b00111100
    db 0b00000110
    db 0b01100110
    db 0b00111100
    db 0b00000000

; ASCII 84: T
char8_T:
    db 0b01111110
    db 0b00011000
    db 0b00011000
    db 0b00011000
    db 0b00011000
    db 0b00011000
    db 0b00011000
    db 0b00000000

; ASCII 85: U
char8_U:
    db 0b01100110
    db 0b01100110
    db 0b01100110
    db 0b01100110
    db 0b01100110
    db 0b01100110
    db 0b00111100
    db 0b00000000

; ASCII 86: V
char8_V:
    db 0b01100110
    db 0b01100110
    db 0b01100110
    db 0b01100110
    db 0b01100110
    db 0b00111100
    db 0b00011000
    db 0b00000000

; ASCII 87: W
char8_W:
    db 0b01100011
    db 0b01100011
    db 0b01100011
    db 0b01101011
    db 0b01111111
    db 0b01110111
    db 0b01100011
    db 0b00000000

; ASCII 88: X
char8_X:
    db 0b01100110
    db 0b01100110
    db 0b00111100
    db 0b00011000
    db 0b00111100
    db 0b01100110
    db 0b01100110
    db 0b00000000

; ASCII 89: Y
char8_Y:
    db 0b01100110
    db 0b01100110
    db 0b01100110
    db 0b00111100
    db 0b00011000
    db 0b00011000
    db 0b00011000
    db 0b00000000

; ASCII 90: Z
char8_Z:
    db 0b01111110
    db 0b00000110
    db 0b00001100
    db 0b00011000
    db 0b00110000
    db 0b01100000
    db 0b01111110
    db 0b00000000

; ASCII 91: [
char8_lbracket:
    db 0b00111100
    db 0b00110000
    db 0b00110000
    db 0b00110000
    db 0b00110000
    db 0b00110000
    db 0b00111100
    db 0b00000000

; ASCII 92: \
char8_backslash:
    db 0b01000000
    db 0b01100000
    db 0b00110000
    db 0b00011000
    db 0b00001100
    db 0b00000110
    db 0b00000010
    db 0b00000000

; ASCII 93: ]
char8_rbracket:
    db 0b00111100
    db 0b00001100
    db 0b00001100
    db 0b00001100
    db 0b00001100
    db 0b00001100
    db 0b00111100
    db 0b00000000

; ASCII 94: ^
char8_caret:
    db 0b00011000
    db 0b00111100
    db 0b01100110
    db 0b00000000
    db 0b00000000
    db 0b00000000
    db 0b00000000
    db 0b00000000

; ASCII 95: _
char8_underscore:
    db 0b00000000
    db 0b00000000
    db 0b00000000
    db 0b00000000
    db 0b00000000
    db 0b00000000
    db 0b00000000
    db 0b11111111

; ASCII 96: `
char8_backtick:
    db 0b00110000
    db 0b00011000
    db 0b00001100
    db 0b00000000
    db 0b00000000
    db 0b00000000
    db 0b00000000
    db 0b00000000

; ASCII 97: a
char8_a:
    db 0b00000000
    db 0b00000000
    db 0b00111100
    db 0b00000110
    db 0b00111110
    db 0b01100110
    db 0b00111110
    db 0b00000000

; ASCII 98: b
char8_b:
    db 0b01100000
    db 0b01100000
    db 0b01111100
    db 0b01100110
    db 0b01100110
    db 0b01100110
    db 0b01111100
    db 0b00000000

; ASCII 99: c
char8_c:
    db 0b00000000
    db 0b00000000
    db 0b00111100
    db 0b01100000
    db 0b01100000
    db 0b01100000
    db 0b00111100
    db 0b00000000

; ASCII 100: d
char8_d:
    db 0b00000110
    db 0b00000110
    db 0b00111110
    db 0b01100110
    db 0b01100110
    db 0b01100110
    db 0b00111110
    db 0b00000000

; ASCII 101: e
char8_e:
    db 0b00000000
    db 0b00000000
    db 0b00111100
    db 0b01100110
    db 0b01111110
    db 0b01100000
    db 0b00111100
    db 0b00000000

; ASCII 102: f
char8_f:
    db 0b00011100
    db 0b00110110
    db 0b00110000
    db 0b01111000
    db 0b00110000
    db 0b00110000
    db 0b00110000
    db 0b00000000

; ASCII 103: g
char8_g:
    db 0b00000000
    db 0b00000000
    db 0b00111110
    db 0b01100110
    db 0b01100110
    db 0b00111110
    db 0b00000110
    db 0b01111100

; ASCII 104: h
char8_h:
    db 0b01100000
    db 0b01100000
    db 0b01111100
    db 0b01100110
    db 0b01100110
    db 0b01100110
    db 0b01100110
    db 0b00000000

; ASCII 105: i
char8_i:
    db 0b00011000
    db 0b00000000
    db 0b00111000
    db 0b00011000
    db 0b00011000
    db 0b00011000
    db 0b00111100
    db 0b00000000

; ASCII 106: j
char8_j:
    db 0b00000110
    db 0b00000000
    db 0b00000110
    db 0b00000110
    db 0b00000110
    db 0b00000110
    db 0b01100110
    db 0b00111100

; ASCII 107: k
char8_k:
    db 0b01100000
    db 0b01100000
    db 0b01100110
    db 0b01101100
    db 0b01111000
    db 0b01101100
    db 0b01100110
    db 0b00000000

; ASCII 108: l
char8_l:
    db 0b00111000
    db 0b00011000
    db 0b00011000
    db 0b00011000
    db 0b00011000
    db 0b00011000
    db 0b00111100
    db 0b00000000

; ASCII 109: m
char8_m:
    db 0b00000000
    db 0b00000000
    db 0b01100110
    db 0b01111111
    db 0b01111111
    db 0b01101011
    db 0b01100011
    db 0b00000000

; ASCII 110: n
char8_n:
    db 0b00000000
    db 0b00000000
    db 0b01111100
    db 0b01100110
    db 0b01100110
    db 0b01100110
    db 0b01100110
    db 0b00000000

; ASCII 111: o
char8_o:
    db 0b00000000
    db 0b00000000
    db 0b00111100
    db 0b01100110
    db 0b01100110
    db 0b01100110
    db 0b00111100
    db 0b00000000

; ASCII 112: p
char8_p:
    db 0b00000000
    db 0b00000000
    db 0b01111100
    db 0b01100110
    db 0b01100110
    db 0b01111100
    db 0b01100000
    db 0b01100000

; ASCII 113: q
char8_q:
    db 0b00000000
    db 0b00000000
    db 0b00111110
    db 0b01100110
    db 0b01100110
    db 0b00111110
    db 0b00000110
    db 0b00000110

; ASCII 114: r
char8_r:
    db 0b00000000
    db 0b00000000
    db 0b01111100
    db 0b01100110
    db 0b01100000
    db 0b01100000
    db 0b01100000
    db 0b00000000

; ASCII 115: s
char8_s:
    db 0b00000000
    db 0b00000000
    db 0b00111110
    db 0b01100000
    db 0b00111100
    db 0b00000110
    db 0b01111100
    db 0b00000000

; ASCII 116: t
char8_t:
    db 0b00011000
    db 0b00011000
    db 0b01111110
    db 0b00011000
    db 0b00011000
    db 0b00011000
    db 0b00001110
    db 0b00000000

; ASCII 117: u
char8_u:
    db 0b00000000
    db 0b00000000
    db 0b01100110
    db 0b01100110
    db 0b01100110
    db 0b01100110
    db 0b00111110
    db 0b00000000

; ASCII 118: v
char8_v:
    db 0b00000000
    db 0b00000000
    db 0b01100110
    db 0b01100110
    db 0b01100110
    db 0b00111100
    db 0b00011000
    db 0b00000000

; ASCII 119: w
char8_w:
    db 0b00000000
    db 0b00000000
    db 0b01100011
    db 0b01101011
    db 0b01111111
    db 0b01111111
    db 0b00110110
    db 0b00000000

; ASCII 120: x
char8_x:
    db 0b00000000
    db 0b00000000
    db 0b01100110
    db 0b00111100
    db 0b00011000
    db 0b00111100
    db 0b01100110
    db 0b00000000

; ASCII 121: y
char8_y:
    db 0b00000000
    db 0b00000000
    db 0b01100110
    db 0b01100110
    db 0b01100110
    db 0b00111110
    db 0b00000110
    db 0b01111100

; ASCII 122: z
char8_z:
    db 0b00000000
    db 0b00000000
    db 0b01111110
    db 0b00001100
    db 0b00011000
    db 0b00110000
    db 0b01111110
    db 0b00000000

; ASCII 123: {
char8_lbrace:
    db 0b00001110
    db 0b00011000
    db 0b00011000
    db 0b01110000
    db 0b00011000
    db 0b00011000
    db 0b00001110
    db 0b00000000

; ASCII 124: |
char8_pipe:
    db 0b00011000
    db 0b00011000
    db 0b00011000
    db 0b00000000
    db 0b00011000
    db 0b00011000
    db 0b00011000
    db 0b00000000

; ASCII 125: }
char8_rbrace:
    db 0b01110000
    db 0b00011000
    db 0b00011000
    db 0b00001110
    db 0b00011000
    db 0b00011000
    db 0b01110000
    db 0b00000000

; ASCII 126: ~
char8_tilde:
    db 0b00110010
    db 0b01001100
    db 0b00000000
    db 0b00000000
    db 0b00000000
    db 0b00000000
    db 0b00000000
    db 0b00000000

font_8x8_end:
