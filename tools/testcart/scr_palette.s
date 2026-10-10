; ============================================================================================
;  scr_palette.s - all 64 colours at once
;
;  A frame shows 25 colours at most, so the chart rewrites the palette between its 16 rows
;  (one hue per row, luminance $0x/$1x/$2x/$3x across). Each row is 7 lines of colour and 5
;  black lines; in those lines rendering is off and one palette entry is written per horizontal
;  blank: colour 3 of BG palettes 0-3 ($3F03/07/0B/0F). Writing an entry briefly shows its old
;  colour where the beam is, so each write sits inside the horizontal blank, and leaves the
;  address on an entry that is black ($3F04/08/0C/10). Then the address is set to the next row
;  and rendering turns on again in the blank before it. The timing is counted from a sprite-0
;  hit at x=216 of line 22; a row (12 lines) is exactly 1364 CPU cycles.
; ============================================================================================
k_i   = kz + 0                  ; row being prepared (0-15 colours, 16 = footer)
k_on  = kz + 1                  ; PPUMASK with rendering
k_off = kz + 2                  ; PPUMASK without rendering (same emphasis / greyscale)

; Kernel timing (CPU cycles); see the cycle notes in pal_kernel.
KD0 = 98                        ; sprite-0 hit detected -> first row
KD1 = 10                        ; before the first write
KD5 = 82                        ; before rendering on

pal_init:
        lda #<pal_text
        sta ptr0
        lda #>pal_text
        sta ptr0+1
        jsr draw_list
        ; rows 3-18: hue digit, 4 swatches
        lda #0
        sta tmp4                ; hue
@row:   lda tmp4
        clc
        adc #3
        jsr row_addr            ; A:X = address of column 0 of that row
        jsr ppu_addr
        lda #' '
        sta PPUDATA
        ldx tmp4
        lda hex_digits,x
        sta PPUDATA
        lda #' '
        sta PPUDATA
        ldx #0
@sw:    lda pal_swatch_row,x
        sta PPUDATA
        inx
        cpx #27
        bne @sw
        inc tmp4
        lda tmp4
        cmp #16
        bne @row
        lda #<pal_attr
        sta ptr0
        lda #>pal_attr
        sta ptr0+1
        jsr load_attr
        lda #<pal_pal
        sta ptr0
        lda #>pal_pal
        sta ptr0+1
        jsr load_pal
        jmp pal_update_status

; A = row (0-29) -> A = high, X = low byte of the nametable 0 address of column 0.
row_addr:
        sta tmp7
        lsr a
        lsr a
        lsr a
        ora #$20
        pha
        lda tmp7
        asl a
        asl a
        asl a
        asl a
        asl a
        tax
        pla
        rts

pal_update:
        jsr emph_input
pal_update_status:
        lda #0
        sta oam_ptr
        lda #216                ; sprite 0: one pixel at (216, 22), hidden behind the bottom-left
                                ; pixel of the "x" in "$3x"
        sta tmp0
        lda #22
        sta tmp1
        lda #%00100000
        sta tmp2
        lda #S_DOT
        jsr oam_add
        jsr oam_finish
        jsr fmt_emph
        lda #$20
        ldx #$36                ; row 1, col 22
        ldy #10
        jmp vb_txt

; ----------------------------------------------------------------- the raster kernel
; Runs from the main loop right after the frame's update (in vblank). Page aligned: no branch
; or table access inside crosses a page (that would add a cycle).
        .align 256
pal_kernel:
        lda ppu_mask
        sta k_on
        and #%11100001
        sta k_off
        lda #0
        sta k_i
@clr:   bit PPUSTATUS           ; wait for the pre-render line to clear the sprite-0 flag
        bvs @clr
@hit:   bit PPUSTATUS           ; sprite 0 hits at x = 254 of line 22
        bvc @hit
        .delay KD0
; One iteration = 12 lines = 1364 cycles: line 7 of the previous row (blank) turns rendering
; off, the horizontal blanks of lines 7, 8, 9 and 10 write one palette entry each (W1-W4, 114 /
; 113 / 114 cycles apart), line 11 points the address at the next row and its blank turns
; rendering back on.
@iter:  ldx k_i                 ; 3
        lda k_off               ; 3
        sta PPUMASK             ; 4   rendering off (this line is blank anyway)
        ldy kern_c0,x           ; 4
        lda #$3F                ; 2
        .delay KD1
        ldx #$03                ; 2
        sta PPUADDR             ; 4   W1 (cycle 0)
        stx PPUADDR             ; 4   address = $3F03: its old colour shows ...
        sty PPUDATA             ; 4   ... until this write moves it to $3F04 (black)
        ldx k_i                 ; 3
        ldy kern_c1,x           ; 4
        .delay 93
        ldx #$07                ; 2
        sta PPUADDR             ; 4   W2 (cycle 114)
        stx PPUADDR
        sty PPUDATA
        ldx k_i
        ldy kern_c2,x
        .delay 92
        ldx #$0B
        sta PPUADDR             ; 4   W3 (cycle 227)
        stx PPUADDR
        sty PPUDATA
        ldx k_i
        ldy kern_c3,x
        .delay 93
        ldx #$0F
        sta PPUADDR             ; 4   W4 (cycle 341)
        stx PPUADDR
        sty PPUDATA             ;     address $3F10 = backdrop (black)
        ldx k_i                 ; 3   (cycle 353)
        lda kern_vhi,x          ; 4
        sta PPUADDR             ; 4
        lda kern_vlo,x          ; 4
        sta PPUADDR             ; 4   address = next row, fine Y 0 (shows backdrop)
        lda k_on                ; 3   (cycle 375)
        .delay KD5
        sta PPUMASK             ; 4   rendering on in the horizontal blank of line 11
        .delay 1364-(375+KD5+4)-10-5-(18+KD1)
        inc k_i                 ; 5
        lda k_i                 ; 3
        cmp #17                 ; 2
        beq @after              ; 2 (not taken)
        jmp @iter               ; 3
@after: rts
        .assert (>pal_kernel) == (>@after), "palette kernel must stay in one page"

; Colours written per row: hue i of luminance 0-3; row 16 = footer (black).
        .align 128
kern_c0: .byte $00, $01, $02, $03, $04, $05, $06, $07, $08, $09, $0A, $0B, $0C, $0D, $0E, $0F, $0F
kern_c1: .byte $10, $11, $12, $13, $14, $15, $16, $17, $18, $19, $1A, $1B, $1C, $1D, $1E, $1F, $0F
kern_c2: .byte $20, $21, $22, $23, $24, $25, $26, $27, $28, $29, $2A, $2B, $2C, $2D, $2E, $2F, $0F
kern_c3: .byte $30, $31, $32, $33, $34, $35, $36, $37, $38, $39, $3A, $3B, $3C, $3D, $3E, $3F, $0F
; Nametable rows shown by each chart row: rows 3-18, then the footer (row 20). As $2006 values
; of the PPU's internal address: bits 12-14 are the fine Y scroll, so $2060 would start the row at
; its third pixel line; $0060 = nametable 0, row 3, fine Y 0.
kern_vhi: .byte $00, $00, $00, $00, $00, $01, $01, $01, $01, $01, $01, $01, $01, $02, $02, $02, $02
kern_vlo: .byte $60, $80, $A0, $C0, $E0, $00, $20, $40, $60, $80, $A0, $C0, $E0, $00, $20, $40, $80
        .assert (>kern_c0) == (>(kern_vlo+16)), "kernel tables must not cross a page"

; 27 tiles from column 3: four 6-tile swatches with 1-tile gaps.
pal_swatch_row:
        .byte T_SWATCH, T_SWATCH, T_SWATCH, T_SWATCH, T_SWATCH, T_SWATCH, 0
        .byte T_SWATCH, T_SWATCH, T_SWATCH, T_SWATCH, T_SWATCH, T_SWATCH, 0
        .byte T_SWATCH, T_SWATCH, T_SWATCH, T_SWATCH, T_SWATCH, T_SWATCH, 0
        .byte T_SWATCH, T_SWATCH, T_SWATCH, T_SWATCH, T_SWATCH, T_SWATCH

pal_text:
        .byte $20, $22, "NES palette", 0                    ; row 1
        .byte $20, $31, "Emph", 0
        .byte $20, $44, "$0x", 0                            ; row 2: column headers
        .byte $20, $4B, "$1x", 0
        .byte $20, $52, "$2x", 0
        .byte $20, $59, "$3x", 0
        .byte $22, $82, "A:Emphasis  B:Gray", 0            ; row 20 (footer)
        .byte $FF

; Swatch columns 3-8 palette 0, 10-15 palette 1, 17-22 palette 2, 24-29 palette 3.
pal_attr:
        .byte $00, $00, $44, $55, $AA, $AA, $FF, $FF,  $00, $00, $44, $55, $AA, $AA, $FF, $FF
        .byte $00, $00, $44, $55, $AA, $AA, $FF, $FF,  $00, $00, $44, $55, $AA, $AA, $FF, $FF
        .byte $00, $00, $44, $55, $AA, $AA, $FF, $FF,  $00, $00, $44, $55, $AA, $AA, $FF, $FF
        .byte $00, $00, $44, $55, $AA, $AA, $FF, $FF,  $00, $00, $44, $55, $AA, $AA, $FF, $FF

; BG palettes: backdrop black, colour 1 white (text), colour 2 grey (sync dot), colour 3 = swatch.
pal_pal:
        .byte $0F, $30, $10, $0F,  $0F, $30, $10, $0F,  $0F, $30, $10, $0F,  $0F, $30, $10, $0F
        .byte $0F, $30, $10, $0F,  $0F, $30, $10, $0F,  $0F, $30, $10, $0F,  $0F, $30, $10, $0F
