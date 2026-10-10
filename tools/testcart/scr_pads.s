; ============================================================================================
;  scr_pads.s - both controllers, all 8 buttons, live (hold Select+Start for the menu)
; ============================================================================================
pd_shown = sv + 0               ; 2 bytes: button state currently drawn for pad 1 / 2
pd_raw   = sv + 2               ; 2 bytes: raw value currently printed ($FF.. = force)
pd_p     = sv + 4               ; pad being processed
pd_row   = sv + 5               ; 2 bytes: frame top row of each pad
PAD_COL  = 2                    ; frame left column

pads_init:
        lda #4
        sta pd_row
        lda #16
        sta pd_row+1
        lda #0
        sta pd_p
@pad:   ldx pd_p
        lda pd_row,x
        sta tmp4
        lda #<pad_template
        sta ptr1
        lda #>pad_template
        sta ptr1+1
        lda #7
        sta tmp5
@row:   lda tmp4
        jsr row_addr
        pha
        txa
        ora #PAD_COL
        tax
        pla
        jsr ppu_addr
        ldy #0
@t:     lda (ptr1),y
        sta PPUDATA
        iny
        cpy #26
        bne @t
        lda ptr1
        clc
        adc #26
        sta ptr1
        bcc @nc
        inc ptr1+1
@nc:    inc tmp4
        dec tmp5
        bne @row
        inc pd_p
        lda pd_p
        cmp #2
        bne @pad
        lda #<pads_text
        sta ptr0
        lda #>pads_text
        sta ptr0+1
        jsr draw_list
        lda #<pads_attr
        sta ptr0
        lda #>pads_attr
        sta ptr0+1
        jsr load_attr
        lda #<pads_pal
        sta ptr0
        lda #>pads_pal
        sta ptr0+1
        jsr load_pal
        lda #0
        sta pd_shown
        sta pd_shown+1
        sta pd_raw              ; the text already shows $00
        sta pd_raw+1
        rts

pads_update:
        ; press blips: pad 1 on pulse 1, pad 2 on pulse 2, one pitch per button
        lda pad1_new
        jsr first_bit
        bmi @nb1
        lda pad_notes,x
        ldy #3
        jsr blip1
@nb1:   lda pad2_new
        jsr first_bit
        bmi @nb2
        lda pad_notes,x
        ldy #3
        jsr blip2
@nb2:   lda #0
        sta pd_p
@pad:   ldx pd_p
        lda pad1,x
        cmp pd_raw,x
        beq @noraw
        jsr pads_raw
@noraw: ldx pd_p
        lda pad1,x
        eor pd_shown,x
        beq @next
        sta tmp6                ; changed bits
        lda #0
        sta tmp5                ; button index 0-7 (bit 7 first)
@bit:   asl tmp6
        bcc @nobit
        lda vbuf_len
        cmp #VBUF_SOFT_LIMIT
        bcs @next               ; out of vblank budget: the rest next frame
        jsr pads_button
@nobit: inc tmp5
        lda tmp5
        cmp #8
        bne @bit
@next:  inc pd_p
        lda pd_p
        cmp #2
        bne @pad
        rts

; Index (0 = bit 7) of the highest set bit of A in X; N flag set when A = 0.
first_bit:
        ldx #0
        cmp #0
        beq @none
@l:     asl a
        bcs @found
        inx
        bne @l
@found: txa
        rts
@none:  lda #$FF
        rts

; Redraws button tmp5 of pad pd_p and flips its bit in pd_shown.
pads_button:
        ldx tmp5
        lda bit_mask,x
        ldx pd_p
        eor pd_shown,x
        sta pd_shown,x
        ldy tmp5
        and bit_mask,y
        sta tmp2                ; non-zero = now pressed
        ; VRAM address of the button's top-left tile
        lda pd_row,x
        clc
        adc btn_row,y
        jsr row_addr
        sta ptr2+1
        txa
        clc
        adc #PAD_COL
        ldy tmp5
        adc btn_col,y
        sta ptr2
        lda btn_kind,y
        beq @single
        cmp #1
        beq @pill
        ; round 2x2: tiles T_ROUND0..3 (+4 pressed)
        lda #T_ROUND0
        jsr pressed_offset4
        sta tmp3
        lda ptr2+1
        ldx ptr2
        ldy #2
        jsr vb_start
        lda tmp3
        sta vbuf,x
        clc
        adc #1
        sta vbuf+1,x
        lda ptr2
        clc
        adc #32
        tax
        lda ptr2+1
        adc #0
        ldy #2
        jsr vb_start
        lda tmp3
        clc
        adc #2
        sta vbuf,x
        adc #1
        sta vbuf+1,x
        rts
@pill:  lda #T_PILL_L
        ldx tmp2
        beq @pl
        lda #T_PILL_L_ON
@pl:    sta tmp3
        lda ptr2+1
        ldx ptr2
        ldy #2
        jsr vb_start
        lda tmp3
        sta vbuf,x
        clc
        adc #1
        sta vbuf+1,x
        rts
@single:
        ldy tmp5
        lda btn_tile,y
        ldx tmp2
        beq @sl
        clc
        adc #1
@sl:    sta tmp3
        lda ptr2+1
        ldx ptr2
        ldy #1
        jsr vb_start
        lda tmp3
        sta vbuf,x
        rts

pressed_offset4:
        ldx tmp2
        beq @r
        clc
        adc #4
@r:     rts

; Prints "$hh %bbbbbbbb" for pad pd_p (label row).
pads_raw:
        ldx pd_p
        lda pad1,x
        sta pd_raw,x
        sta tmp3
        ldy #0
        lda #'$'
        sta txt,y
        iny
        lda tmp3
        jsr fmt_hex2
        lda #' '
        sta txt,y
        iny
        lda #'%'
        sta txt,y
        iny
        ldx #8
@b:     asl tmp3
        lda #'0'
        adc #0
        sta txt,y
        iny
        dex
        bne @b
        ldx pd_p
        lda pd_row,x
        sec
        sbc #1
        jsr row_addr
        pha
        txa
        ora #17
        tax
        pla
        ldy #13
        jmp vb_txt

bit_mask:  .byte $80, $40, $20, $10, $08, $04, $02, $01
; Buttons in bit order: A, B, Select, Start, Up, Down, Left, Right (relative to the frame).
btn_col:   .byte 22, 18, 8, 14, 3, 3, 2, 4
btn_row:   .byte 2, 2, 3, 3, 2, 4, 3, 3
btn_kind:  .byte 2, 2, 1, 1, 0, 0, 0, 0
btn_tile:  .byte 0, 0, 0, 0, T_DPAD_UP, T_DPAD_DOWN, T_DPAD_LEFT, T_DPAD_RIGHT
pad_notes: .byte 36, 31, 26, 29, 40, 33, 35, 38

; 26 x 7 tiles: frame, D-pad, Select / Start, B / A (unpressed).
pad_template:
        .byte T_PAD_TL, T_PAD_H_TOP, T_PAD_H_TOP, T_PAD_H_TOP, T_PAD_H_TOP, T_PAD_H_TOP, T_PAD_H_TOP, T_PAD_H_TOP, T_PAD_H_TOP, T_PAD_H_TOP, T_PAD_H_TOP, T_PAD_H_TOP, T_PAD_H_TOP
        .byte T_PAD_H_TOP, T_PAD_H_TOP, T_PAD_H_TOP, T_PAD_H_TOP, T_PAD_H_TOP, T_PAD_H_TOP, T_PAD_H_TOP, T_PAD_H_TOP, T_PAD_H_TOP, T_PAD_H_TOP, T_PAD_H_TOP, T_PAD_H_TOP, T_PAD_TR
        .byte T_PAD_V_L, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, T_PAD_V_R
        .byte T_PAD_V_L, 0, 0, T_DPAD_UP, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, T_ROUND0, T_ROUND1, 0, 0, T_ROUND0, T_ROUND1, 0, T_PAD_V_R
        .byte T_PAD_V_L, 0, T_DPAD_LEFT, T_DPAD_C, T_DPAD_RIGHT, 0, 0, 0, T_PILL_L, T_PILL_R, 0, 0, 0, 0, T_PILL_L, T_PILL_R, 0, 0, T_ROUND2, T_ROUND3, 0, 0, T_ROUND2, T_ROUND3, 0, T_PAD_V_R
        .byte T_PAD_V_L, 0, 0, T_DPAD_DOWN, 0, 0, "Select", 0, "Start", 0, "B", 0, 0, 0, "A", 0, T_PAD_V_R
        .byte T_PAD_V_L, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, T_PAD_V_R
        .byte T_PAD_BL, T_PAD_H_BOT, T_PAD_H_BOT, T_PAD_H_BOT, T_PAD_H_BOT, T_PAD_H_BOT, T_PAD_H_BOT, T_PAD_H_BOT, T_PAD_H_BOT, T_PAD_H_BOT, T_PAD_H_BOT, T_PAD_H_BOT, T_PAD_H_BOT
        .byte T_PAD_H_BOT, T_PAD_H_BOT, T_PAD_H_BOT, T_PAD_H_BOT, T_PAD_H_BOT, T_PAD_H_BOT, T_PAD_H_BOT, T_PAD_H_BOT, T_PAD_H_BOT, T_PAD_H_BOT, T_PAD_H_BOT, T_PAD_H_BOT, T_PAD_BR
pad_template_end:
        .assert pad_template_end - pad_template == 26*7, "pad template must be 26 x 7"

pads_text:
        .byte $20, $22, "Controllers", 0                         ; row 1
        .byte $20, $62, "Player 1", 0                            ; row 3
        .byte $20, $71, "$00 %00000000", 0
        .byte $21, $E2, "Player 2", 0                            ; row 15
        .byte $21, $F1, "$00 %00000000", 0
        .byte $23, $02, "Hold Select+Start: menu", 0              ; row 24
        .byte $23, $42, "Read once per frame: strobe,", 0         ; row 26
        .byte $23, $62, "8 reads of $4016 / $4017", 0             ; row 27
        .byte $FF

; Palette 1 for the A / B buttons (rows 6-7 and 18-19, columns 20-21 and 24-25).
pads_attr:
        .byte $00, $00, $00, $00, $00, $00, $00, $00
        .byte $00, $00, $00, $00, $00, $10, $10, $00
        .byte $00, $00, $00, $00, $00, $00, $00, $00
        .byte $00, $00, $00, $00, $00, $00, $00, $00
        .byte $00, $00, $00, $00, $00, $10, $10, $00
        .byte $00, $00, $00, $00, $00, $00, $00, $00
        .byte $00, $00, $00, $00, $00, $00, $00, $00
        .byte $00, $00, $00, $00, $00, $00, $00, $00

pads_pal:
        .byte $0F, $10, $00, $30,  $0F, $10, $06, $27,  $0F, $10, $00, $30,  $0F, $10, $00, $30
        .byte $0F, $30, $10, $00,  $0F, $30, $10, $00,  $0F, $30, $10, $00,  $0F, $30, $10, $00
