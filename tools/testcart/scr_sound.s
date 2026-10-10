; ============================================================================================
;  scr_sound.s - test tones: pulse 1 / 2 (4 duties), triangle, noise (2 modes, 16 rates), scale
; ============================================================================================
sn_sel   = sv + 0               ; selected row 0-4
sn_on    = sv + 1               ; 5 bytes: playing
sn_note  = sv + 6               ; 3 bytes: pulse 1, pulse 2, triangle note (0 = C2 .. 47 = B5)
sn_duty  = sv + 9               ; 2 bytes: pulse duty 0-3
sn_nrate = sv + 11              ; noise period index 0-15
sn_nmode = sv + 12              ; noise mode 0 long / 1 short
sn_step  = sv + 13              ; scale step
sn_timer = sv + 14
sn_dirty = sv + 15              ; row to redraw ($FF none)
sn_row   = sv + 16              ; row being formatted
SN_ROWS  = 5
SN_ROW0  = 5                    ; screen row of the first item

snd_init:
        lda #1
        sta sfx_off
        lda #<snd_text
        sta ptr0
        lda #>snd_text
        sta ptr0+1
        jsr draw_list
        lda #<snd_pal
        sta ptr0
        lda #>snd_pal
        sta ptr0+1
        jsr load_pal
        ldx #4
        lda #0
@c:     sta sn_on,x
        dex
        bpl @c
        lda #33                 ; A4
        sta sn_note
        lda #28                 ; E4
        sta sn_note+1
        lda #21                 ; A3
        sta sn_note+2
        lda #2
        sta sn_duty
        lda #1
        sta sn_duty+1
        lda #4
        sta sn_nrate
        lda #0
        sta sn_nmode
        sta sn_sel
        ldx #0                  ; draw every row now (rendering is off)
@r:     stx sn_dirty
        jsr snd_row
        ldx vbuf_len
        lda #0
        sta vbuf,x
        jsr flush_vbuf
        lda #0
        sta vbuf_len
        ldx sn_dirty
        inx
        cpx #SN_ROWS
        bne @r
        lda #$FF
        sta sn_dirty
        jmp snd_cursor

snd_update:
        lda pad1_new
        and #BTN_UP
        beq @nu
        dec sn_sel
        bpl @nu
        lda #SN_ROWS-1
        sta sn_sel
@nu:    lda pad1_new
        and #BTN_DOWN
        beq @nd
        inc sn_sel
        lda sn_sel
        cmp #SN_ROWS
        bcc @nd
        lda #0
        sta sn_sel
@nd:    ldx sn_sel
        lda pad1_new
        and #BTN_A
        beq @na
        lda sn_on,x
        eor #1
        sta sn_on,x
        jsr snd_apply
        ldx sn_sel
        stx sn_dirty
@na:    lda pad1_new
        and #BTN_B
        beq @nb
        jsr snd_mode
        ldx sn_sel
        stx sn_dirty
@nb:    lda pad1_new
        and #BTN_LEFT|BTN_RIGHT
        beq @nlr
        jsr snd_pitch
        ldx sn_sel
        stx sn_dirty
@nlr:   jsr snd_scale_tick
        lda sn_dirty
        bmi @nodraw
        tax
        jsr snd_row
        lda #$FF
        sta sn_dirty
@nodraw:
snd_cursor:
        lda #0
        sta oam_ptr
        lda #16
        sta tmp0
        lda sn_sel
        asl a
        asl a
        asl a
        asl a                   ; rows are 2 apart: 16 px
        clc
        adc #SN_ROW0*8
        sta tmp1
        lda #0
        sta tmp2
        lda #S_ARROW
        jsr oam_add
        jmp oam_finish

; B: duty (pulses) or mode (noise).
snd_mode:
        ldx sn_sel
        cpx #2
        bcs @notpulse
        lda sn_duty,x
        clc
        adc #1
        and #3
        sta sn_duty,x
        jmp snd_apply
@notpulse:
        cpx #3
        bne @r
        lda sn_nmode
        eor #1
        sta sn_nmode
        jmp snd_apply
@r:     rts

; Left / Right: note (pulses, triangle) or rate (noise).
snd_pitch:
        ldx sn_sel
        cpx #3
        beq @noise
        bcs @r
        lda pad1_new
        and #BTN_LEFT
        beq @up
        lda sn_note,x
        beq @r
        dec sn_note,x
        jmp snd_apply
@up:    lda sn_note,x
        cmp #47
        bcs @r
        inc sn_note,x
        jmp snd_apply
@noise: lda pad1_new
        and #BTN_LEFT
        beq @nup
        lda sn_nrate
        beq @r
        dec sn_nrate
        jmp snd_apply
@nup:   lda sn_nrate
        cmp #15
        bcs @r
        inc sn_nrate
        jmp snd_apply
@r:     rts

; Writes the APU registers of row sn_sel.
snd_apply:
        ldx sn_sel
        cpx #0
        bne @p2
        lda sn_on
        beq @off1
        lda sn_duty
        jsr duty_vol
        sta SQ1_VOL
        lda sn_note
        asl a
        tay
        lda note_period,y
        sta SQ1_LO
        lda note_period+1,y
        ora #$F8
        sta SQ1_HI
        rts
@off1:  lda #$30
        sta SQ1_VOL
        rts
@p2:    cpx #1
        bne @tri
        lda sn_on+1
        beq @off2
        lda sn_duty+1
        jsr duty_vol
        sta SQ2_VOL
        lda sn_note+1
        asl a
        tay
        lda note_period,y
        sta SQ2_LO
        lda note_period+1,y
        ora #$F8
        sta SQ2_HI
        rts
@off2:  lda #$30
        sta SQ2_VOL
        rts
@tri:   cpx #2
        bne @noi
        lda sn_on+2
        beq @off3
        lda #$FF
        sta TRI_LINEAR
        lda sn_note+2
        asl a
        tay
        lda note_period,y       ; (P + 1) / 2 - 1
        clc
        adc #1
        sta tmp0
        lda note_period+1,y
        adc #0
        lsr a
        ror tmp0
        sta tmp1
        lda tmp0
        sec
        sbc #1
        sta TRI_LO
        lda tmp1
        sbc #0
        ora #$F8
        sta TRI_HI
        rts
@off3:  lda #$80
        sta TRI_LINEAR
        rts
@noi:   cpx #3
        bne @scale
        lda sn_on+3
        beq @off4
        lda #$3A
        sta NOISE_VOL
        lda sn_nmode
        lsr a
        ror a                   ; mode into bit 7
        ora sn_nrate
        sta NOISE_LO
        lda #$F8
        sta NOISE_HI
        rts
@off4:  lda #$30
        sta NOISE_VOL
        rts
@scale: lda #0
        sta sn_step
        sta sn_timer
        lda sn_on+4
        bne @r
        lda #$30
        sta SQ1_VOL
        lda sn_on               ; pulse 1 was playing its own tone: restore it
        beq @r
        lda #0
        sta sn_sel
        jsr snd_apply
        lda #4
        sta sn_sel
@r:     rts

; A = duty 0-3 -> $4000 value (constant volume 10, halted).
duty_vol:
        lsr a
        ror a
        ror a
        ora #$3A
        rts

; Scale: C major up and down on pulse 1, one note every 12 frames.
snd_scale_tick:
        lda sn_on+4
        beq @r
        dec sn_timer
        bpl @r
        lda #11
        sta sn_timer
        ldx sn_step
        lda scale_notes,x
        asl a
        tay
        lda #$BA                ; duty 50 %, volume 10
        sta SQ1_VOL
        lda note_period,y
        sta SQ1_LO
        lda note_period+1,y
        ora #$F8
        sta SQ1_HI
        inx
        cpx #14
        bcc @s
        ldx #0
@s:     stx sn_step
@r:     rts
scale_notes: .byte 24, 26, 28, 29, 31, 33, 35, 36, 35, 33, 31, 29, 28, 26

; Redraws row X (name, setting, note / rate, frequency, on).
snd_row:
        stx sn_row
        lda #' '
        ldy #0
        ldx #27
        jsr fmt_fill
        ldx sn_row
        lda snd_names_lo,x
        sta ptr0
        lda snd_names_hi,x
        sta ptr0+1
        ldy #0
        jsr fmt_str
        ldx sn_row
        cpx #2
        bcs @nodu
        lda sn_duty,x
        asl a
        asl a
        tax
        ldy #9
@du:    lda duty_text,x
        sta txt,y
        iny
        inx
        txa
        and #3
        bne @du
@nodu:  ldx sn_row
        cpx #3
        bcs @nonote
        lda sn_note,x
        jsr fmt_note            ; at txt+14: "A-4 440Hz"
        jmp @state
@nonote:
        cpx #3
        bne @state
        ldy #9
        lda sn_nmode
        beq @long
        lda #'s'
        sta txt,y
        lda #'h'
        sta txt+1,y
        lda #'r'
        sta txt+2,y
        lda #'t'
        sta txt+3,y
        jmp @rate
@long:  lda #'l'
        sta txt,y
        lda #'o'
        sta txt+1,y
        lda #'n'
        sta txt+2,y
        lda #'g'
        sta txt+3,y
@rate:  ldy #14
        lda #'r'
        sta txt,y
        iny
        lda #'a'
        sta txt,y
        iny
        lda #'t'
        sta txt,y
        iny
        lda #'e'
        sta txt,y
        iny
        lda sn_nrate
        jsr fmt_dec3
@state: ldx sn_row
        lda sn_on,x
        beq @off
        lda #'O'
        sta txt+25
        lda #'N'
        sta txt+26
        jmp @out
@off:   lda #'-'
        sta txt+25
        sta txt+26
@out:   lda sn_row
        asl a
        clc
        adc #SN_ROW0
        jsr row_addr
        pha
        txa
        ora #4
        tax
        pla
        ldy #27
        jmp vb_txt

; Note A at txt+14: "A-4  440Hz".
fmt_note:
        sta tmp5
        ldx #0                  ; octave
@o:     cmp #12
        bcc @got
        sbc #12
        inx
        bne @o
@got:   asl a
        tay
        txa
        clc
        adc #'2'
        sta txt+16
        lda note_names,y
        sta txt+14
        lda note_names+1,y
        sta txt+15
        lda tmp5
        asl a
        tax
        lda note_hz,x
        sta m_a
        lda note_hz+1,x
        sta m_a+1
        lda #3
        sta tmp4
        ldy #19
        jsr fmt_dec16
        lda #'H'
        sta txt+22
        lda #'z'
        sta txt+23
        rts

duty_text: .byte "12% 25% 50% 75% "
snd_names_lo: .byte <snm0, <snm1, <snm2, <snm3, <snm4
snd_names_hi: .byte >snm0, >snm1, >snm2, >snm3, >snm4
snm0: .byte "Pulse 1", 0
snm1: .byte "Pulse 2", 0
snm2: .byte "Triangle", 0
snm3: .byte "Noise", 0
snm4: .byte "Scale (pulse 1)", 0

snd_text:
        .byte $20, $22, "Sound test", 0                         ; row 1
        .byte $20, $64, "Channel  Mode Note Freq  On", 0         ; row 3
        .byte $22, $E2, "Up/Down: channel", 0                    ; row 23
        .byte $23, $02, "A: play / stop", 0
        .byte $23, $22, "B: duty / noise mode", 0
        .byte $23, $42, "Left/Right: pitch / rate", 0
        .byte $FF

snd_pal:
        .byte $0F, $30, $10, $00,  $0F, $30, $10, $00,  $0F, $30, $10, $00,  $0F, $30, $10, $00
        .byte $0F, $27, $16, $30,  $0F, $30, $10, $00,  $0F, $30, $10, $00,  $0F, $30, $10, $00
