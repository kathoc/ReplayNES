; ============================================================================================
;  sound.s - APU set-up, short blips for presses / UI, note tables
; ============================================================================================

apu_init:
        lda #$0F
        sta SND_CHN             ; pulse 1, pulse 2, triangle, noise (never DMC)
        lda #$08
        sta SQ1_SWEEP           ; sweep off (negate set: no muting of low notes)
        sta SQ2_SWEEP
        lda #$40
        sta JOY2                ; 4-step frame counter, no IRQ
        ; fall through
snd_stop_all:
        lda #$30                ; constant volume 0, length counter halted
        sta SQ1_VOL
        sta SQ2_VOL
        sta NOISE_VOL
        lda #$80                ; triangle: linear counter reload 0 = silent
        sta TRI_LINEAR
        lda #0
        sta sfx_t1
        sta sfx_t2
        rts

; Called once per frame from the NMI: ends blips.
sound_tick:
        lda sfx_t1
        beq @n1
        dec sfx_t1
        bne @n1
        lda #$30
        sta SQ1_VOL
@n1:    lda sfx_t2
        beq @n2
        dec sfx_t2
        bne @n2
        lda #$30
        sta SQ2_VOL
@n2:    rts

; UI blip on pulse 1: A = 1 screen change, 2 toggle, 3 cursor move.
sfx_ui:
        ldx sfx_off
        bne @r
        tax
        lda ui_note-1,x
        ldy #4
        jmp blip1
@r:     rts
ui_note: .byte 29, 33, 24          ; note numbers (see note_period): F4, A4, C4

; Short tone on pulse 1: A = note number (0 = C2 .. 47 = B5), Y = frames.
blip1:
        ldx sfx_off
        bne @r
        sty sfx_t1
        asl a
        tax
        lda #%10111000          ; duty 50 %, halt, constant volume 8
        sta SQ1_VOL
        lda note_period,x
        sta SQ1_LO
        lda note_period+1,x
        ora #%11111000          ; length counter load (ignored: halted)
        sta SQ1_HI
@r:     rts

; Short tone on pulse 2: A = note number, Y = frames.
blip2:
        ldx sfx_off
        bne @r
        sty sfx_t2
        asl a
        tax
        lda #%01111000          ; duty 25 %, halt, constant volume 8
        sta SQ2_VOL
        lda note_period,x
        sta SQ2_LO
        lda note_period+1,x
        ora #%11111000
        sta SQ2_HI
@r:     rts

; Pulse timer periods for C2..B5 (48 notes): round(1789773 / (16 * f)) - 1, f = 440 * 2^((n-33)/12).
; The triangle plays (period + 1) / 2 - 1 for the same pitch.
note_period:
        .word 1709, 1613, 1523, 1437, 1356, 1280, 1208, 1140, 1076, 1016, 959, 905   ; octave 2
        .word 854, 806, 761, 718, 678, 640, 604, 570, 538, 507, 479, 452             ; octave 3
        .word 427, 403, 380, 359, 338, 319, 301, 284, 268, 253, 239, 225             ; octave 4
        .word 213, 201, 189, 179, 169, 159, 150, 142, 134, 126, 119, 112             ; octave 5
; Rounded frequencies in Hz for the display.
note_hz:
        .word 65, 69, 73, 78, 82, 87, 92, 98, 104, 110, 117, 123                     ; octave 2
        .word 131, 139, 147, 156, 165, 175, 185, 196, 208, 220, 233, 247             ; octave 3
        .word 262, 277, 294, 311, 330, 349, 370, 392, 415, 440, 466, 494             ; octave 4
        .word 523, 554, 587, 622, 659, 698, 740, 784, 831, 880, 932, 988             ; octave 5
note_names: .byte "C-C#D-D#E-F-F#G-G#A-A#B-"
