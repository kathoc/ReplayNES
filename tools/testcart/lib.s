; ============================================================================================
;  lib.s - PPU / OAM / text helpers shared by every screen
; ============================================================================================

; Main loop: returns once an NMI happened since the last call (at once if it already did).
wait_nmi:
        lda nmi_seen
@w:     cmp nmi_count
        beq @w
        lda nmi_count
        sta nmi_seen
        rts

; Returns right after the next NMI (in vblank).
wait_vblank:
        lda nmi_count
@w:     cmp nmi_count
        beq @w
        sta nmi_seen
        inc nmi_seen
        rts

; VRAM list written by the main loop, copied in vblank. Entry: hi, lo, len (bit 7 = vertical).
flush_vbuf:
        ldx #0
@next:  lda vbuf,x
        beq @done
        sta PPUADDR
        lda vbuf+1,x
        sta PPUADDR
        lda vbuf+2,x
        bpl @horiz
        lda ppu_ctrl
        ora #%00000100
        sta PPUCTRL
        lda vbuf+2,x
        and #$7F
        bne @len                ; always (lengths are 1..127)
@horiz: lda ppu_ctrl
        sta PPUCTRL
        lda vbuf+2,x
@len:   tay
        inx
        inx
        inx
@copy:  lda vbuf,x
        sta PPUDATA
        inx
        dey
        bne @copy
        beq @next
@done:  rts

; Copies pal_buf to the palette (rendering off or vblank) and points the address away from it.
write_palette:
        lda #$3F
        sta PPUADDR
        ldx #0
        stx PPUADDR
@p:     lda pal_buf,x
        sta PPUDATA
        inx
        cpx #32
        bne @p
        lda #$3F
        sta PPUADDR
        lda #0
        sta PPUADDR
        sta PPUADDR
        sta PPUADDR
        rts

; Both nametables (+ attributes) to tile 0, rendering off.
clear_nametables:
        bit PPUSTATUS
        lda #$20
        sta PPUADDR
        lda #$00
        sta PPUADDR
        ldy #8                  ; 8 x 256 bytes = $2000-$27FF
        tax
@c:     sta PPUDATA
        inx
        bne @c
        dey
        bne @c
        rts

oam_hide_all:
        lda #$FF
        ldx #0
@h:     sta oam,x
        inx
        bne @h
        lda #0
        sta oam_ptr
        rts

; Hides the sprites from oam_ptr to the end of OAM.
oam_finish:
        ldx oam_ptr
        lda #$FF
@h:     sta oam,x
        inx
        inx
        inx
        inx
        bne @h
        rts

; Adds one sprite: A = tile, tmp0 = x, tmp1 = y (screen line of its top row), tmp2 = attributes.
oam_add:
        ldx oam_ptr
        sta oam+1,x
        lda tmp1
        sec
        sbc #1                  ; sprites are drawn one line below their Y
        sta oam,x
        lda tmp2
        sta oam+2,x
        lda tmp0
        sta oam+3,x
        inx
        inx
        inx
        inx
        stx oam_ptr
        rts

; Sprite text: ptr0 = string (0-terminated), tmp0 = x, tmp1 = y, tmp2 = attributes.
oam_text:
        ldy #0
@c:     lda (ptr0),y
        beq @done
        sty tmp3
        jsr oam_add
        lda tmp0
        clc
        adc #8
        sta tmp0
        ldy tmp3
        iny
        bne @c
@done:  rts

; Sets the VRAM address: A = high, X = low.
ppu_addr:
        bit PPUSTATUS
        sta PPUADDR
        stx PPUADDR
        rts

; Draws a list of strings (rendering off). ptr0 -> { hi, lo, bytes..., 0 } ... $FF.
draw_list:
        ldy #0
@entry: lda (ptr0),y
        cmp #$FF
        beq @done
        bit PPUSTATUS
        sta PPUADDR
        iny
        lda (ptr0),y
        sta PPUADDR
        iny
@chars: lda (ptr0),y
        beq @end
        sta PPUDATA
        iny
        bne @chars
        inc ptr0+1              ; long string crossing 256 bytes of the list
        jmp @chars
@end:   iny
        bne @entry
        inc ptr0+1
        jmp @entry
@done:  rts

; Fills Y bytes with A at the current VRAM address.
fill_vram:
@f:     sta PPUDATA
        dey
        bne @f
        rts

; Copies 32 palette bytes from ptr0 to pal_buf.
load_pal:
        ldy #31
@p:     lda (ptr0),y
        sta pal_buf,y
        dey
        bpl @p
        lda #1
        sta pal_dirty
        rts

; Writes 64 attribute bytes from ptr0 to nametable 0 (rendering off).
load_attr:
        lda #$23
        ldx #$C0
        jsr ppu_addr
        ldy #0
@a:     lda (ptr0),y
        sta PPUDATA
        iny
        cpy #64
        bne @a
        rts

; Fills the 64 attribute bytes of nametable 0 with A (rendering off).
fill_attr:
        pha
        lda #$23
        ldx #$C0
        jsr ppu_addr
        pla
        ldy #64
        jmp fill_vram

; ---------------------------------------------------------------- VRAM list (main loop side)
; Starts an entry: A = address high, X = address low, Y = length (bit 7 = vertical).
; Returns X = index of the first data byte in vbuf. The caller stores the data at vbuf,x.
vb_start:
        stx tmp7
        ldx vbuf_len
        sta vbuf,x
        lda tmp7
        sta vbuf+1,x
        tya
        sta vbuf+2,x
        and #$7F
        clc
        adc #3
        adc vbuf_len
        sta vbuf_len
        inx
        inx
        inx
        rts

; Queues Y bytes from txt to VRAM address A:X.
vb_txt:
        sty tmp6
        jsr vb_start
        ldy #0
@c:     lda txt,y
        sta vbuf,x
        inx
        iny
        cpy tmp6
        bne @c
        rts

; Queues a 0-terminated string at ptr0 to VRAM address A:X.
vb_string:
        pha
        ldy #0
@len:   lda (ptr0),y
        beq @got
        iny
        bne @len
@got:   pla
        sty tmp6
        jsr vb_start
        ldy #0
@c:     cpy tmp6
        beq @done
        lda (ptr0),y
        sta vbuf,x
        inx
        iny
        bne @c
@done:  rts

; ---------------------------------------------------------------- number formatting
; All formatters write to txt,y and advance Y.
dig = $03C0                     ; 5 decimal digits, most significant first

; A (0-255) as 3 digits, leading zeros shown as spaces (the last digit always shown).
fmt_dec3:
        sta m_a
        lda #0
        sta m_a+1
        lda #3
        sta tmp4
        ; fall through
; m_a (16 bit, destroyed) as tmp4 (1..5) right-aligned digits, leading zeros as spaces.
; Digits by repeated subtraction of powers of ten (fast: the split-scroll update must finish
; well before its sprite-0 hit).
fmt_dec16:
        sty tmp3
        ldx #0
@pow:   lda #0
        sta dig,x
@sub:   lda m_a
        sec
        sbc dec_pow_lo,x
        sta tmp6
        lda m_a+1
        sbc dec_pow_hi,x
        bcc @next
        sta m_a+1
        lda tmp6
        sta m_a
        inc dig,x
        bne @sub
@next:  inx
        cpx #4
        bne @pow
        lda m_a
        sta dig+4
        ldy tmp3
        lda #5
        sec
        sbc tmp4
        tax
        lda #$80
        sta tmp5                ; still blanking leading zeros
@e:     lda dig,x
        bne @nz
        cpx #4
        beq @nz
        bit tmp5
        bpl @nz
        lda #' '
        bne @put
@nz:    ora #'0'
        pha
        lda #0
        sta tmp5
        pla
@put:   sta txt,y
        iny
        inx
        cpx #5
        bne @e
        rts

dec_pow_lo: .byte <10000, <1000, <100, <10
dec_pow_hi: .byte >10000, >1000, >100, >10

; Fixed point (x10) value in m_a (16 bit) as "dd.d" with tmp4 integer digits.
fmt_fix1:
        lda #10
        jsr div16_8
        pha                     ; tenths
        jsr fmt_dec16
        lda #'.'
        sta txt,y
        iny
        pla
        ora #'0'
        sta txt,y
        iny
        rts

; Writes A as two hex digits to txt,y (Y += 2).
fmt_hex2:
        pha
        lsr a
        lsr a
        lsr a
        lsr a
        tax
        lda hex_digits,x
        sta txt,y
        iny
        pla
        and #$0F
        tax
        lda hex_digits,x
        sta txt,y
        iny
        rts
hex_digits: .byte "0123456789ABCDEF"

; Copies a 0-terminated string at ptr0 into txt,y (Y advances).
fmt_str:
        sty tmp3
        ldy #0
@c:     lda (ptr0),y
        beq @done
        sty tmp6
        ldy tmp3
        sta txt,y
        inc tmp3
        ldy tmp6
        iny
        bne @c
@done:  ldy tmp3
        rts

; Fills txt,y with A, X times (Y advances).
fmt_fill:
@f:     sta txt,y
        iny
        dex
        bne @f
        rts

; Emphasis / greyscale status for the palette and bar screens: "R G B  Gray" style text.
; Writes 10 characters to txt (Y = 0).
fmt_emph:
        ldy #0
        lda emph
        and #1
        beq @r0
        lda #'R'
        bne @r1
@r0:    lda #'-'
@r1:    sta txt,y
        iny
        lda emph
        and #2
        beq @g0
        lda #'G'
        bne @g1
@g0:    lda #'-'
@g1:    sta txt,y
        iny
        lda emph
        and #4
        beq @b0
        lda #'B'
        bne @b1
@b0:    lda #'-'
@b1:    sta txt,y
        iny
        lda #' '
        sta txt,y
        iny
        sta txt,y
        iny
        lda grey
        beq @noG
        ldx #0
@gc:    lda gray_on_text,x
        sta txt,y
        iny
        inx
        cpx #5
        bne @gc
        rts
@noG:   ldx #5
        lda #' '
        jmp fmt_fill
gray_on_text: .byte "Gray "

; A/B on the palette / bar screens: emphasis bits and greyscale; updates ppu_mask.
emph_input:
        lda pad1_new
        and #BTN_A
        beq @noA
        inc emph
        lda emph
        and #7
        sta emph
        lda #2
        jsr sfx_ui
@noA:   lda pad1_new
        and #BTN_B
        beq @noB
        lda grey
        eor #1
        sta grey
        lda #2
        jsr sfx_ui
@noB:   lda emph
        asl a
        asl a
        asl a
        asl a
        asl a
        ora grey
        ora #MASK_SHOW
        sta ppu_mask
        rts
