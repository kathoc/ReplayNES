; ============================================================================================
;  scr_bars.s - colour bars, grey ramp, sharpness patterns, emphasis / greyscale
; ============================================================================================

bars_init:
        ; bars: rows 2-13, 8 bars of 4 tiles
        lda #2
        sta tmp4
@row:   lda tmp4
        jsr row_addr
        jsr ppu_addr
        ldx #0
@bar:   lda bars_tiles,x
        sta PPUDATA
        sta PPUDATA
        sta PPUDATA
        sta PPUDATA
        inx
        cpx #8
        bne @bar
        inc tmp4
        lda tmp4
        cmp #14
        bne @row
        ; grey ramp: rows 16-19, 6 steps of 4 tiles from column 4
        lda #16
        sta tmp4
@grow:  lda tmp4
        jsr row_addr
        pha
        txa
        ora #4
        tax
        pla
        jsr ppu_addr
        ldx #0
@step:  lda ramp_tiles,x
        sta PPUDATA
        sta PPUDATA
        sta PPUDATA
        sta PPUDATA
        inx
        cpx #6
        bne @step
        inc tmp4
        lda tmp4
        cmp #20
        bne @grow
        ; sharpness patches: rows 22-23, 4 tiles each at columns 2, 9, 16, 23
        lda #22
        sta tmp4
@prow:  lda tmp4
        jsr row_addr
        jsr ppu_addr
        lda #0
        sta PPUDATA
        sta PPUDATA
        ldx #0
@patch: lda patch_tiles,x
        ldy #4
        jsr fill_vram
        lda #0
        ldy #3
        jsr fill_vram
        inx
        cpx #4
        bne @patch
        inc tmp4
        lda tmp4
        cmp #24
        bne @prow
        lda #<bars_text
        sta ptr0
        lda #>bars_text
        sta ptr0+1
        jsr draw_list
        lda #<bars_attr
        sta ptr0
        lda #>bars_attr
        sta ptr0+1
        jsr load_attr
        lda #<bars_pal
        sta ptr0
        lda #>bars_pal
        sta ptr0+1
        jsr load_pal
        jmp bars_status

bars_update:
        jsr emph_input
bars_status:
        jsr fmt_emph
        lda #$23
        ldx #$76                ; row 27, col 22
        ldy #10
        jmp vb_txt

; Bars (palette A: $30 $28 $2C, B: $2A $24 $16, C: $12 ...): white, yellow, cyan, green,
; magenta, red, blue, black.
bars_tiles: .byte T_SOLID1, T_SOLID2, T_SOLID3, T_SOLID1, T_SOLID2, T_SOLID3, T_SOLID1, T_BLANK
; Grey ramp: black, $2D, $00 (palette C colours 2, 3), $10, $3D, $20 (palette D colours 1-3).
ramp_tiles: .byte T_BLANK, T_SOLID2, T_SOLID3, T_SOLID1, T_SOLID2, T_SOLID3
patch_tiles: .byte T_VSTRIPE1, T_VSTRIPE2, T_CHECK1, T_HSTRIPE1

bars_text:
        .byte $20, $22, "Color bars", 0                                  ; row 1
        .byte $21, $C0, "Wht Yel Cyn Grn Mag Red Blu Blk", 0              ; row 14
        .byte $22, $84, " 0F  2D  00  10  3D  20", 0                     ; row 20
        .byte $23, $22, "V1px", 0                                        ; row 25
        .byte $23, $29, "V2px", 0
        .byte $23, $30, "Dots", 0
        .byte $23, $37, "H1px", 0
        .byte $23, $62, "A:Emphasis  B:Gray", 0                          ; row 27
        .byte $FF

bars_pal:
        .byte $0F, $30, $28, $2C,  $0F, $2A, $24, $16,  $0F, $12, $2D, $00,  $0F, $10, $3D, $20
        .byte $0F, $30, $10, $00,  $0F, $30, $10, $00,  $0F, $30, $10, $00,  $0F, $30, $10, $00

; Bars rows 2-13: columns 0-11 palette A (0), 12-23 palette B (1), 24-27 palette C (2).
; Ramp rows 16-19: columns 8-15 palette C (2), 16-27 palette D (3). Everything else palette A.
bars_attr:
        .byte $00, $00, $00, $55, $55, $55, $AA, $AA      ; rows 0-3 (row 0-1 text: palette A)
        .byte $00, $00, $00, $55, $55, $55, $AA, $AA      ; rows 4-7
        .byte $00, $00, $00, $55, $55, $55, $AA, $AA      ; rows 8-11
        .byte $00, $00, $00, $05, $05, $05, $0A, $0A      ; rows 12-13 bars, 14-15 text
        .byte $00, $00, $AA, $AA, $FF, $FF, $FF, $00      ; rows 16-19 ramp
        .byte $00, $00, $00, $00, $00, $00, $00, $00      ; rows 20-23
        .byte $00, $00, $00, $00, $00, $00, $00, $00      ; rows 24-27
        .byte $00, $00, $00, $00, $00, $00, $00, $00      ; rows 28-29
