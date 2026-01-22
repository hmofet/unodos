; UnoDOS Second Stage Loader
; Loaded at 0x0800:0000 (linear 0x8000)
; Sets up system and displays graphical Hello World

[BITS 16]
[ORG 0x0000]

; ============================================================================
; Signature and Entry Point
; ============================================================================

signature:
    dw 0x4E55               ; 'UN' signature for boot sector verification

entry:
    ; Set up segment registers for our location
    mov ax, 0x0800
    mov ds, ax
    mov es, ax

    ; Keep SS:SP from boot sector for now

    mov si, msg_stage2
    call print_string

    ; Detect and report memory
    call detect_memory

    ; Detect video adapter
    call detect_video

    ; Set up graphics mode and show Hello World
    call setup_graphics

    ; DEBUG: Skip main loop for clean coordinate test
    ; ; Main loop - clock and character demo
    ; call main_loop

    ; Halt after coordinate test
.halt:
    hlt
    jmp .halt

; ============================================================================
; Memory Detection
; ============================================================================

detect_memory:
    mov si, msg_detect_mem
    call print_string

    ; Get conventional memory size via INT 12h
    int 0x12                ; Returns KB in AX
    mov [mem_kb], ax

    ; Print memory size
    call print_decimal
    mov si, msg_kb
    call print_string

    ret

; ============================================================================
; Video Adapter Detection
; ============================================================================

detect_video:
    mov si, msg_detect_vid
    call print_string

    ; Check for CGA/EGA/VGA by reading BIOS equipment list
    int 0x11                ; Get equipment list in AX
    mov [equipment], ax

    ; Bits 4-5 indicate video mode:
    ; 00 = EGA/VGA, 01 = 40x25 CGA, 10 = 80x25 CGA, 11 = 80x25 MDA
    and ax, 0x30
    mov cl, 4
    shr ax, cl

    cmp al, 0
    je .ega_vga
    ; CGA (modes 01 or 10)
    mov si, msg_cga
    call print_string
    ret

.ega_vga:
    mov si, msg_ega
    call print_string
    ret

; ============================================================================
; Graphics Setup
; ============================================================================

setup_graphics:
    mov si, msg_setup_gfx
    call print_string

    ; CGA graphics mode (works for CGA and EGA/VGA)
    ; Set CGA 320x200 4-color mode (mode 4)
    mov ah, 0x00
    mov al, 0x04            ; 320x200, 4 colors
    int 0x10

    ; Clear all CGA video memory explicitly
    ; Mode 4 uses 0xB800:0000-0x3FFF (16KB)
    push es
    mov ax, 0xB800
    mov es, ax
    xor di, di
    xor ax, ax              ; Fill with 0 (background color)
    mov cx, 8192            ; 16KB / 2 = 8192 words
    cld
    rep stosw
    pop es

    ; Select color palette (cyan/magenta/white)
    mov ah, 0x0B
    mov bh, 0x01            ; Palette select
    mov bl, 0x01            ; Palette 1 (cyan, magenta, white)
    int 0x10

    ; Set background/border color
    mov ah, 0x0B
    mov bh, 0x00            ; Background color
    mov bl, 0x01            ; Blue background
    int 0x10

    ; DEBUG: Only draw coordinate markers, nothing else
    call draw_coordinate_test

    ret

; ============================================================================
; Coordinate Test - Draw markers at regular intervals
; ============================================================================

draw_coordinate_test:
    pusha

    ; Set ES to CGA video memory FIRST - required for plot_pixel_white
    push es
    mov ax, 0xB800
    mov es, ax

    ; Comprehensive coordinate test to find visible area
    ; Y markers on left edge (X=4), every 20 pixels
    ; 0=Y0, 1=Y20, 2=Y40, 3=Y60, 4=Y80, 5=Y100, 6=Y120, 7=Y140, 8=Y160, 9=Y180

    mov word [draw_x], 4
    mov word [draw_y], 0
    mov al, '0'
    call draw_ascii_4x6

    mov word [draw_x], 4
    mov word [draw_y], 20
    mov al, '1'
    call draw_ascii_4x6

    mov word [draw_x], 4
    mov word [draw_y], 40
    mov al, '2'
    call draw_ascii_4x6

    mov word [draw_x], 4
    mov word [draw_y], 60
    mov al, '3'
    call draw_ascii_4x6

    mov word [draw_x], 4
    mov word [draw_y], 80
    mov al, '4'
    call draw_ascii_4x6

    mov word [draw_x], 4
    mov word [draw_y], 100
    mov al, '5'
    call draw_ascii_4x6

    mov word [draw_x], 4
    mov word [draw_y], 120
    mov al, '6'
    call draw_ascii_4x6

    mov word [draw_x], 4
    mov word [draw_y], 140
    mov al, '7'
    call draw_ascii_4x6

    mov word [draw_x], 4
    mov word [draw_y], 160
    mov al, '8'
    call draw_ascii_4x6

    mov word [draw_x], 4
    mov word [draw_y], 180
    mov al, '9'
    call draw_ascii_4x6

    ; X markers on top (Y=10), every 40 pixels
    ; A=X40, B=X80, C=X120, D=X160, E=X200, F=X240, G=X280, H=X310

    mov word [draw_x], 40
    mov word [draw_y], 10
    mov al, 'A'
    call draw_ascii_4x6

    mov word [draw_x], 80
    mov word [draw_y], 10
    mov al, 'B'
    call draw_ascii_4x6

    mov word [draw_x], 120
    mov word [draw_y], 10
    mov al, 'C'
    call draw_ascii_4x6

    mov word [draw_x], 160
    mov word [draw_y], 10
    mov al, 'D'
    call draw_ascii_4x6

    mov word [draw_x], 200
    mov word [draw_y], 10
    mov al, 'E'
    call draw_ascii_4x6

    mov word [draw_x], 240
    mov word [draw_y], 10
    mov al, 'F'
    call draw_ascii_4x6

    mov word [draw_x], 280
    mov word [draw_y], 10
    mov al, 'G'
    call draw_ascii_4x6

    mov word [draw_x], 310
    mov word [draw_y], 10
    mov al, 'H'
    call draw_ascii_4x6

    pop es
    popa
    ret

; ============================================================================
; CGA Graphics Hello World
; ============================================================================

draw_hello_gfx:
    push es

    ; CGA graphics memory at 0xB800
    mov ax, 0xB800
    mov es, ax

    ; Draw top border (y=50, x=60 to x=260)
    mov bx, 50              ; Y coordinate
    mov cx, 60              ; Start X
.top_border:
    call plot_pixel_white
    inc cx
    cmp cx, 260
    jl .top_border

    ; Draw bottom border (y=150)
    mov bx, 150
    mov cx, 60
.bottom_border:
    call plot_pixel_white
    inc cx
    cmp cx, 260
    jl .bottom_border

    ; Draw left border (x=60, y=50 to y=150)
    mov cx, 60
    mov bx, 50
.left_border:
    call plot_pixel_white
    inc bx
    cmp bx, 150
    jle .left_border

    ; Draw right border (x=259, y=50 to y=150)
    mov cx, 259
    mov bx, 50
.right_border:
    call plot_pixel_white
    inc bx
    cmp bx, 150
    jle .right_border

    ; Draw "WELCOME TO" on first line (8x8 font)
    ; Centered: 10 chars * 12 pixels = 120 pixels, so start at (160-60)=100
    mov word [draw_x], 76
    mov word [draw_y], 70

    ; W
    mov si, char_W
    call draw_char
    ; E
    mov si, char_E
    call draw_char
    ; L
    mov si, char_L
    call draw_char
    ; C
    mov si, char_C
    call draw_char
    ; O
    mov si, char_O
    call draw_char
    ; M
    mov si, char_M
    call draw_char
    ; E
    mov si, char_E
    call draw_char

    ; Space
    add word [draw_x], 12

    ; T
    mov si, char_T
    call draw_char
    ; O
    mov si, char_O
    call draw_char

    ; Draw "UNODOS 3!" on second line
    mov word [draw_x], 88
    add word [draw_y], 16

    ; U
    mov si, char_U
    call draw_char
    ; N
    mov si, char_N
    call draw_char
    ; O
    mov si, char_O
    call draw_char
    ; D
    mov si, char_D
    call draw_char
    ; O
    mov si, char_O
    call draw_char
    ; S
    mov si, char_S
    call draw_char

    ; Space
    add word [draw_x], 12

    ; 3
    mov si, char_3
    call draw_char
    ; !
    mov si, char_excl
    call draw_char

    ; Draw version "v3.1.5" in smaller text (4x6 font)
    ; Centered below main text
    mov word [draw_x], 136
    add word [draw_y], 20

    ; v
    mov si, char_v_small
    call draw_char_small
    ; 3
    mov si, char_3_small
    call draw_char_small
    ; .
    mov si, char_dot_small
    call draw_char_small
    ; 1
    mov si, char_1_small
    call draw_char_small
    ; .
    mov si, char_dot_small
    call draw_char_small
    ; 0
    mov si, char_0_small
    call draw_char_small

.skip_text:
    ; Draw RAM info in top right
    call draw_ram_info

    ; Draw initial clock in top left
    call draw_clock

    pop es
    ret

; ============================================================================
; RAM Info Display (bottom right corner)
; ============================================================================

draw_ram_info:
    ; Draw "RAM:" label (top right corner)
    mov word [draw_x], 266
    mov word [draw_y], 4

    ; R
    mov si, char_R_small
    call draw_char_small
    ; A
    mov si, char_A_small
    call draw_char_small
    ; M
    mov si, char_M_small
    call draw_char_small
    ; :
    mov si, char_colon_small
    call draw_char_small

    ; Draw total RAM value (from mem_kb)
    mov ax, [mem_kb]
    mov word [draw_x], 290
    call draw_number_small

    ; K
    mov si, char_K_small
    call draw_char_small

    ; Next line: "Used:"
    mov word [draw_x], 260
    add word [draw_y], 8

    ; U
    mov si, char_U_small
    call draw_char_small
    ; s
    mov si, char_s_small
    call draw_char_small
    ; e
    mov si, char_e_small
    call draw_char_small
    ; d
    mov si, char_d_small
    call draw_char_small
    ; :
    mov si, char_colon_small
    call draw_char_small

    ; Calculate used memory (stage2 loaded at 0x8000, size ~8KB + boot sector)
    ; For now, estimate: boot sector (512) + stage2 (~8KB) + stack (~1KB) = ~10KB
    mov ax, 10
    mov word [draw_x], 290
    call draw_number_small

    ; K
    mov si, char_K_small
    call draw_char_small

    ; Next line: "Free:"
    mov word [draw_x], 260
    add word [draw_y], 8

    ; F
    mov si, char_F_small
    call draw_char_small
    ; r
    mov si, char_r_small
    call draw_char_small
    ; e
    mov si, char_e_small
    call draw_char_small
    ; e
    mov si, char_e_small
    call draw_char_small
    ; :
    mov si, char_colon_small
    call draw_char_small

    ; Calculate free memory (total - used)
    mov ax, [mem_kb]
    sub ax, 10              ; Subtract used (~10KB)
    mov word [draw_x], 290
    call draw_number_small

    ; K
    mov si, char_K_small
    call draw_char_small

    ret

; ============================================================================
; Clock Display (top left corner) - Reads from RTC
; ============================================================================

; Clock position (above white box at Y=50)
CLOCK_X     equ 4
CLOCK_Y     equ 40

; Draw clock (HH:MM:SS format)
draw_clock:
    pusha

    ; Set draw position first
    mov word [draw_x], CLOCK_X
    mov word [draw_y], CLOCK_Y

    ; DEBUG: Draw static "12:34" to test if draw_ascii_4x6 works
    mov al, '1'
    call draw_ascii_4x6
    mov al, '2'
    call draw_ascii_4x6
    mov al, ':'
    call draw_ascii_4x6
    mov al, '3'
    call draw_ascii_4x6
    mov al, '4'
    call draw_ascii_4x6

    ; DEBUG: Draw X position markers at Y=95 to find visible X range
    ; Each marker shows X/50: 0,1,2,3,4,5,6 at X=0,50,100,150,200,250,300

    ; X=0 marker "0"
    mov word [draw_x], 0
    mov word [draw_y], 95
    mov al, '0'
    call draw_ascii_4x6

    ; X=50 marker "1"
    mov word [draw_x], 50
    mov word [draw_y], 95
    mov al, '1'
    call draw_ascii_4x6

    ; X=100 marker "2"
    mov word [draw_x], 100
    mov word [draw_y], 95
    mov al, '2'
    call draw_ascii_4x6

    ; X=150 marker "3"
    mov word [draw_x], 150
    mov word [draw_y], 95
    mov al, '3'
    call draw_ascii_4x6

    ; X=200 marker "4"
    mov word [draw_x], 200
    mov word [draw_y], 95
    mov al, '4'
    call draw_ascii_4x6

    ; X=250 marker "5"
    mov word [draw_x], 250
    mov word [draw_y], 95
    mov al, '5'
    call draw_ascii_4x6

    ; X=300 marker "6"
    mov word [draw_x], 300
    mov word [draw_y], 95
    mov al, '6'
    call draw_ascii_4x6

    popa
    ret

    ; Original RTC code commented out for debugging
    ; Read RTC time using BIOS INT 1Ah, AH=02h
    ;mov ah, 0x02
    ;int 0x1A
    ;jc .clock_error         ; If carry set, RTC not available
    ;... (rest commented)

.clock_error:
    ; RTC not available - draw dashes (unused for now)
    popa
    ret

; Draw a BCD byte as two decimal digits using 4x6 font
; Input: AL = BCD value (e.g., 0x23 = 23)
draw_bcd_small:
    push ax
    push bx

    mov bl, al              ; Save original

    ; High nibble (tens digit)
    shr al, 4
    and al, 0x0F
    add al, '0'             ; Convert to ASCII
    call draw_ascii_4x6

    ; Low nibble (ones digit)
    mov al, bl
    and al, 0x0F
    add al, '0'             ; Convert to ASCII
    call draw_ascii_4x6

    pop bx
    pop ax
    ret

; Clear clock area (to prevent overdraw artifacts)
; Clock is at y=40, spans 6 rows (y=40-45), row 20-22 in CGA
clear_clock_area:
    pusha
    push es

    mov ax, 0xB800
    mov es, ax

    ; Even rows (y=40,42,44 -> row 20,21,22)
    mov di, 20 * 80         ; Start at row 20
    mov cx, 3               ; 3 rows
.clear_even:
    push cx
    push di
    mov cx, 6               ; 12 bytes = 6 words (covers ~48 pixels)
    xor ax, ax
    rep stosw
    pop di
    add di, 80              ; Next row
    pop cx
    loop .clear_even

    ; Odd rows (y=41,43,45 -> row 20,21,22 in odd bank)
    mov di, 20 * 80 + 0x2000
    mov cx, 3
.clear_odd:
    push cx
    push di
    mov cx, 6
    xor ax, ax
    rep stosw
    pop di
    add di, 80
    pop cx
    loop .clear_odd

    pop es
    popa
    ret

; RTC storage
rtc_hours:      db 0
rtc_minutes:    db 0
rtc_seconds:    db 0

; ============================================================================
; Character Demo - Cycles through all ASCII characters
; ============================================================================

; Demo area: y=95 (between "UNODOS 3!" at Y=86 and version at Y=106)
; Using 4x6 font, can fit ~53 chars per row (320 / 6 = 53)
DEMO_START_X    equ 65
DEMO_START_Y    equ 95
DEMO_CHAR_WIDTH equ 6
DEMO_CHARS_PER_ROW equ 52

; ============================================================================
; Main Loop - Updates clock and runs character demo
; ============================================================================

main_loop:
    ; Draw initial clock
    call draw_clock
    ; Run character demo (also updates clock)
    call char_demo_loop
    jmp main_loop

; ============================================================================
; Clock Update Loop - Independent clock updater
; ============================================================================

clock_loop:
    call draw_clock
    call delay_short
    ret

; ============================================================================
; Character Demo Loop
; ============================================================================

char_demo_loop:
    ; Clear the demo area first
    call clear_demo_area

    ; Reset position
    mov word [draw_x], DEMO_START_X
    mov word [draw_y], DEMO_START_Y

    ; Start with space (ASCII 32)
    mov byte [demo_char], 32

.draw_next_char:
    ; Update clock display
    call draw_clock

    ; Draw current character
    mov al, [demo_char]
    call draw_ascii_4x6

    ; Small delay between characters for visual effect
    mov cx, 8
.char_delay:
    call delay_short
    loop .char_delay

    ; Next character
    inc byte [demo_char]

    ; Check if we've reached end of printable ASCII (126)
    cmp byte [demo_char], 127
    jb .draw_next_char

    ; All characters displayed - pause before clearing
    mov cx, 60
.pause_delay:
    call draw_clock         ; Keep updating clock during pause
    call delay_short
    loop .pause_delay

    ret

; Clear the demo area (inside white box, between text lines)
clear_demo_area:
    pusha
    push es

    mov ax, 0xB800
    mov es, ax

    ; Clear even scanlines (y=95-100, CGA uses y/2)
    ; y=95: row 47, y=100: row 50
    ; Even rows start at offset = row * 80
    ; Only clear inside box area (x=65 to x=255, ~190 pixels = 48 bytes)
    ; Starting at x=65: byte offset = 65/4 = 16

    ; Clear rows 47-49 in even bank
    mov di, 47 * 80 + 16    ; Start at row 47, x=64
    mov cx, 3               ; 3 rows
.clear_even:
    push cx
    push di
    mov cx, 24              ; 48 bytes = 24 words (covers ~192 pixels)
    xor ax, ax
    rep stosw
    pop di
    add di, 80
    pop cx
    loop .clear_even

    ; Clear odd scanlines (add 0x2000)
    mov di, 47 * 80 + 16 + 0x2000
    mov cx, 3
.clear_odd:
    push cx
    push di
    mov cx, 24
    xor ax, ax
    rep stosw
    pop di
    add di, 80
    pop cx
    loop .clear_odd

    pop es
    popa
    ret

; Short delay loop
delay_short:
    pusha
    mov cx, 0x1000
.loop:
    nop
    nop
    loop .loop
    popa
    ret

; Demo variables
demo_char:  db 32

; Draw a number using small font (AX = number to draw)
; Draws right-aligned at current draw_x position
draw_number_small:
    pusha

    ; Convert number to digits and store on stack
    mov bx, 10
    xor cx, cx              ; Digit counter

.divide_loop:
    xor dx, dx
    div bx                  ; AX = AX / 10, DX = remainder
    push dx                 ; Save digit
    inc cx
    test ax, ax
    jnz .divide_loop

    ; Now print digits from stack
.print_loop:
    pop ax                  ; Get digit (0-9)

    ; Calculate address of digit character
    mov bx, 6               ; Each small char is 6 bytes
    mul bx                  ; AX = digit * 6
    add ax, char_0_small    ; Add base address
    mov si, ax

    call draw_char_small
    loop .print_loop

    popa
    ret

; ============================================================================
; Generic Text Rendering Functions (using font tables)
; ============================================================================

; Draw a null-terminated string using 8x8 font
; Input: SI = pointer to null-terminated string
;        draw_x, draw_y = starting position
; Modifies: draw_x (advances for each character)
draw_string_8x8:
    pusha

.loop:
    lodsb                       ; Get next character
    test al, al                 ; Check for null terminator
    jz .done

    ; Calculate font table offset: (char - 32) * 8
    sub al, 32                  ; Convert ASCII to font index
    jb .skip_char               ; Skip if < 32
    cmp al, 95                  ; Check if > 126 (95 = 126-32+1)
    jae .skip_char

    xor ah, ah
    shl ax, 3                   ; Multiply by 8
    add ax, font_8x8            ; Add font base address
    push si
    mov si, ax
    call draw_char              ; Draw the character
    pop si
    jmp .loop

.skip_char:
    add word [draw_x], 12       ; Skip space for unprintable chars
    jmp .loop

.done:
    popa
    ret

; Draw a null-terminated string using 4x6 font
; Input: SI = pointer to null-terminated string
;        draw_x, draw_y = starting position
; Modifies: draw_x (advances for each character)
draw_string_4x6:
    pusha

.loop:
    lodsb                       ; Get next character
    test al, al                 ; Check for null terminator
    jz .done

    ; Calculate font table offset: (char - 32) * 6
    sub al, 32                  ; Convert ASCII to font index
    jb .skip_char               ; Skip if < 32
    cmp al, 95                  ; Check if > 126
    jae .skip_char

    xor ah, ah
    mov bl, 6
    mul bl                      ; AX = index * 6
    add ax, font_4x6            ; Add font base address
    push si
    mov si, ax
    call draw_char_small        ; Draw the character
    pop si
    jmp .loop

.skip_char:
    add word [draw_x], 6        ; Skip space for unprintable chars
    jmp .loop

.done:
    popa
    ret

; Draw a single ASCII character using 8x8 font
; Input: AL = ASCII character
;        draw_x, draw_y = position
; Modifies: draw_x (advances by 12)
draw_ascii_8x8:
    pusha

    ; Calculate font table offset: (char - 32) * 8
    sub al, 32
    jb .skip
    cmp al, 95
    jae .skip

    xor ah, ah
    shl ax, 3                   ; Multiply by 8
    add ax, font_8x8
    mov si, ax
    call draw_char
    jmp .done

.skip:
    add word [draw_x], 12

.done:
    popa
    ret

; Draw a single ASCII character using 4x6 font
; Input: AL = ASCII character
;        draw_x, draw_y = position
; Modifies: draw_x (advances by 6)
draw_ascii_4x6:
    pusha

    ; Calculate font table offset: (char - 32) * 6
    sub al, 32
    jb .skip
    cmp al, 95
    jae .skip

    xor ah, ah
    mov bl, 6
    mul bl
    add ax, font_4x6
    mov si, ax
    call draw_char_small
    jmp .done

.skip:
    add word [draw_x], 6

.done:
    popa
    ret

; Plot a white pixel (color 3)
; Input: CX = X coordinate (0-319), BX = Y coordinate (0-199)
; Preserves all registers
plot_pixel_white:
    pusha                   ; Save all general registers

    ; Save input values to temp storage
    mov [.save_x], cx
    mov [.save_y], bx

    ; Calculate memory address
    ; For CGA 320x200 4-color:
    ; Byte offset = (Y/2) * 80 + (X/4)
    ; If Y is odd, add 0x2000

    ; Calculate row offset
    mov ax, bx              ; AX = Y
    shr ax, 1               ; AX = Y / 2
    mov cx, 80
    mul cx                  ; AX = (Y/2) * 80, DX = high word (ignored)
    mov di, ax              ; DI = row offset

    ; Add column offset
    mov ax, [.save_x]       ; AX = X
    shr ax, 1
    shr ax, 1               ; AX = X / 4
    add di, ax              ; DI = byte offset

    ; Check if Y was odd
    mov ax, [.save_y]
    test al, 1
    jz .even_row
    add di, 0x2000          ; Odd rows are in second bank
.even_row:

    ; Calculate bit position within byte
    ; Each pixel is 2 bits, 4 pixels per byte
    ; Pixel 0 is bits 7-6, pixel 1 is bits 5-4, etc.
    mov ax, [.save_x]       ; AX = X
    and ax, 3               ; Get pixel position (0-3)

    ; Calculate shift amount: (3 - position) * 2
    mov cx, 3
    sub cl, al
    shl cl, 1               ; Shift amount in CL

    ; Read current byte, set our pixel to white (11b)
    mov al, [es:di]
    mov ah, 0x03            ; Color 3 (white)
    shl ah, cl              ; Shift color to correct position

    ; Create mask to clear old pixel (2 bits at position)
    mov bl, 0x03
    shl bl, cl
    not bl                  ; Now BL has 0s where we want to write

    and al, bl              ; Clear old pixel
    or al, ah               ; Set new pixel
    mov [es:di], al

    popa                    ; Restore all general registers
    ret

.save_x: dw 0
.save_y: dw 0

; Draw 8x8 character
; Input: SI = pointer to 8-byte character bitmap
;        draw_x, draw_y = top-left position
draw_char:
    pusha                   ; Save all registers

    mov bx, [draw_y]        ; BX = current Y
    mov bp, 8               ; BP = row counter

.row_loop:
    lodsb                   ; Get row bitmap into AL
    mov ah, al              ; AH = bitmap for this row
    mov cx, [draw_x]        ; CX = current X
    mov dx, 8               ; DX = column counter

.col_loop:
    test ah, 0x80           ; Check leftmost bit
    jz .skip_pixel
    call plot_pixel_white   ; Plot at (CX, BX)
.skip_pixel:
    shl ah, 1               ; Next bit
    inc cx                  ; Next X
    dec dx                  ; Decrement column counter
    jnz .col_loop

    inc bx                  ; Next Y
    dec bp                  ; Decrement row counter
    jnz .row_loop

    add word [draw_x], 12   ; Advance to next character position

    popa                    ; Restore all registers
    ret

; Draw 4x6 small character
; Input: SI = pointer to 6-byte character bitmap
;        draw_x, draw_y = top-left position
draw_char_small:
    pusha                   ; Save all registers

    mov bx, [draw_y]        ; BX = current Y
    mov bp, 6               ; BP = row counter (6 rows)

.row_loop_small:
    lodsb                   ; Get row bitmap into AL
    mov ah, al              ; AH = bitmap for this row
    mov cx, [draw_x]        ; CX = current X
    mov dx, 4               ; DX = column counter (4 columns)

.col_loop_small:
    test ah, 0x80           ; Check leftmost bit
    jz .skip_pixel_small
    call plot_pixel_white   ; Plot at (CX, BX)
.skip_pixel_small:
    shl ah, 1               ; Next bit
    inc cx                  ; Next X
    dec dx                  ; Decrement column counter
    jnz .col_loop_small

    inc bx                  ; Next Y
    dec bp                  ; Decrement row counter
    jnz .row_loop_small

    add word [draw_x], 6    ; Advance to next character position (4 + 2 spacing)

    popa                    ; Restore all registers
    ret

; ============================================================================
; Utility Functions
; ============================================================================

; Print null-terminated string (uses BIOS, works in text mode)
; Need to switch to text mode for debug output
print_string:
    push ax
    push bx
    push si
.loop:
    lodsb
    test al, al
    jz .done
    mov ah, 0x0E
    mov bh, 0
    int 0x10
    jmp .loop
.done:
    pop si
    pop bx
    pop ax
    ret

; Print AX as decimal number
print_decimal:
    push ax
    push bx
    push cx
    push dx

    mov bx, 10
    xor cx, cx              ; Digit counter

.divide:
    xor dx, dx
    div bx                  ; AX = AX / 10, DX = remainder
    push dx                 ; Save digit
    inc cx
    test ax, ax
    jnz .divide

.print_digits:
    pop dx
    add dl, '0'
    mov ah, 0x0E
    mov al, dl
    mov bh, 0
    int 0x10
    loop .print_digits

    pop dx
    pop cx
    pop bx
    pop ax
    ret

; ============================================================================
; Data
; ============================================================================

; Variables
mem_kb:         dw 0
equipment:      dw 0
draw_x:         dw 0
draw_y:         dw 0

; Messages
msg_stage2:     db 'Stage2 loaded', 0x0D, 0x0A, 0
msg_detect_mem: db 'Memory: ', 0
msg_kb:         db ' KB', 0x0D, 0x0A, 0
msg_detect_vid: db 'Video: ', 0
msg_cga:        db 'CGA', 0x0D, 0x0A, 0
msg_ega:        db 'EGA/VGA', 0x0D, 0x0A, 0
msg_setup_gfx:  db 'Init graphics...', 0x0D, 0x0A, 0

; ============================================================================
; Font Data - Complete ASCII Character Sets
; ============================================================================

; Include 8x8 font (95 characters, 760 bytes)
%include "font8x8.asm"

; Include 4x6 font (95 characters, 570 bytes)
%include "font4x6.asm"

; Legacy aliases for backward compatibility with existing code
; These point into the font tables at the appropriate offsets
char_H  equ font_8x8 + ('H' - 32) * 8
char_E  equ font_8x8 + ('E' - 32) * 8
char_L  equ font_8x8 + ('L' - 32) * 8
char_O  equ font_8x8 + ('O' - 32) * 8
char_W  equ font_8x8 + ('W' - 32) * 8
char_R  equ font_8x8 + ('R' - 32) * 8
char_D  equ font_8x8 + ('D' - 32) * 8
char_excl equ font_8x8 + ('!' - 32) * 8
char_C  equ font_8x8 + ('C' - 32) * 8
char_M  equ font_8x8 + ('M' - 32) * 8
char_T  equ font_8x8 + ('T' - 32) * 8
char_U  equ font_8x8 + ('U' - 32) * 8
char_N  equ font_8x8 + ('N' - 32) * 8
char_S  equ font_8x8 + ('S' - 32) * 8
char_3  equ font_8x8 + ('3' - 32) * 8

; Small font aliases
char_v_small equ font_4x6 + ('v' - 32) * 6
char_0_small equ font_4x6 + ('0' - 32) * 6
char_1_small equ font_4x6 + ('1' - 32) * 6
char_2_small equ font_4x6 + ('2' - 32) * 6
char_3_small equ font_4x6 + ('3' - 32) * 6
char_4_small equ font_4x6 + ('4' - 32) * 6
char_5_small equ font_4x6 + ('5' - 32) * 6
char_6_small equ font_4x6 + ('6' - 32) * 6
char_7_small equ font_4x6 + ('7' - 32) * 6
char_8_small equ font_4x6 + ('8' - 32) * 6
char_9_small equ font_4x6 + ('9' - 32) * 6
char_dot_small equ font_4x6 + ('.' - 32) * 6
char_R_small equ font_4x6 + ('R' - 32) * 6
char_A_small equ font_4x6 + ('A' - 32) * 6
char_M_small equ font_4x6 + ('M' - 32) * 6
char_colon_small equ font_4x6 + (':' - 32) * 6
char_K_small equ font_4x6 + ('K' - 32) * 6
char_U_small equ font_4x6 + ('U' - 32) * 6
char_s_small equ font_4x6 + ('s' - 32) * 6
char_e_small equ font_4x6 + ('e' - 32) * 6
char_d_small equ font_4x6 + ('d' - 32) * 6
char_F_small equ font_4x6 + ('F' - 32) * 6
char_r_small equ font_4x6 + ('r' - 32) * 6

; ============================================================================
; Padding
; ============================================================================

; Pad to fill sectors (we're loading 16 sectors = 8KB)
times 8192 - ($ - $$) db 0

