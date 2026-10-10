; ============================================================================================
;  math.s - unsigned integer arithmetic for the rapid-fire statistics (all preserve Y)
; ============================================================================================

; m_a (16 bit) / A (8 bit): quotient -> m_a, remainder -> A. Uses X.
div16_8:
        sta m_r+2
        lda #0
        ldx #16
@l:     asl m_a
        rol m_a+1
        rol a
        bcs @sub                ; remainder grew past 8 bits: always >= divisor
        cmp m_r+2
        bcc @no
@sub:   sbc m_r+2
        inc m_a
@no:    dex
        bne @l
        rts

; m_a (32 bit) / m_b (16 bit): quotient -> m_a, remainder -> m_r (16 bit). Uses X.
div32_16:
        lda #0
        sta m_r
        sta m_r+1
        ldx #32
@l:     asl m_a
        rol m_a+1
        rol m_a+2
        rol m_a+3
        rol m_r
        rol m_r+1
        bcs @sub                ; 17-bit remainder: always >= divisor
        lda m_r
        cmp m_b
        lda m_r+1
        sbc m_b+1
        bcc @no
@sub:   lda m_r
        sbc m_b
        sta m_r
        lda m_r+1
        sbc m_b+1
        sta m_r+1
        inc m_a
@no:    dex
        bne @l
        rts

; m_a (32 bit) * m_b (16 bit, destroyed) -> m_a (low 32 bits). Uses X, m_r.
mul32_16:
        lda #0
        sta m_r
        sta m_r+1
        sta m_r+2
        sta m_r+3
        ldx #16
@l:     lsr m_b+1
        ror m_b
        bcc @noadd
        clc
        lda m_r
        adc m_a
        sta m_r
        lda m_r+1
        adc m_a+1
        sta m_r+1
        lda m_r+2
        adc m_a+2
        sta m_r+2
        lda m_r+3
        adc m_a+3
        sta m_r+3
@noadd: asl m_a
        rol m_a+1
        rol m_a+2
        rol m_a+3
        dex
        bne @l
        lda m_r
        sta m_a
        lda m_r+1
        sta m_a+1
        lda m_r+2
        sta m_a+2
        lda m_r+3
        sta m_a+3
        rts

; Integer square root: floor(sqrt(m_a)) (32 bit, destroyed) -> m_r (16 bit). Uses X, m_b, m_t.
isqrt32:
        lda #0
        sta m_r
        sta m_r+1
        sta m_t
        sta m_t+1
        sta m_t+2
        ldx #16
@l:     asl m_a                 ; remainder = remainder * 4 + next two bits
        rol m_a+1
        rol m_a+2
        rol m_a+3
        rol m_t
        rol m_t+1
        rol m_t+2
        asl m_a
        rol m_a+1
        rol m_a+2
        rol m_a+3
        rol m_t
        rol m_t+1
        rol m_t+2
        asl m_r                 ; root *= 2
        rol m_r+1
        lda m_r                 ; trial = root * 2 + 1
        asl a
        sta m_b
        lda m_r+1
        rol a
        sta m_b+1
        lda #0
        rol a
        sta m_b+2
        inc m_b
        lda m_t                 ; remainder >= trial ?
        cmp m_b
        lda m_t+1
        sbc m_b+1
        lda m_t+2
        sbc m_b+2
        bcc @no
        lda m_t
        sbc m_b
        sta m_t
        lda m_t+1
        sbc m_b+1
        sta m_t+1
        lda m_t+2
        sbc m_b+2
        sta m_t+2
        inc m_r
@no:    dex
        bne @l
        rts

; A * A -> m_a (16 bit). Uses X.
square8:
        sta m_b
        sta m_r+2
        lda #0
        sta m_a+1
        ldx #8
@l:     asl a
        rol m_a+1
        asl m_b
        bcc @no
        clc
        adc m_r+2
        bcc @no
        inc m_a+1
@no:    dex
        bne @l
        sta m_a
        rts

; Clears m_a (32 bit) and loads A into its low byte.
m_a_load8:
        sta m_a
        lda #0
        sta m_a+1
        sta m_a+2
        sta m_a+3
        rts
