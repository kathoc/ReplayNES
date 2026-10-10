; ============================================================================================
;  scr_sprites.s - 8 bouncing sprites, sprite-0 hit, 8 sprites per line + overflow flag
; ============================================================================================
NBALLS   = 8
s_x      = big + 0              ; ball positions / velocities (NBALLS each)
s_y      = big + 8
s_dx     = big + 16
s_dy     = big + 24
s0_x     = sv + 0               ; sprite 0 (crosshair) position
s0_y     = sv + 1
s_count  = sv + 2               ; sprites on the test line: 8 or 9
s_flick  = sv + 3               ; 1 = rotate the sprite order every frame
s_rot    = sv + 4
s_hit    = sv + 5               ; last shown values (to update the text only on change)
s_ovf    = sv + 6

BOX_L = 16                      ; ball area (pixels)
BOX_R = 232
BOX_T = 32
BOX_B = 120
LINE_Y = 168                    ; the 8/9-sprites test line

spr_init:
        lda #<spr_text
        sta ptr0
        lda #>spr_text
        sta ptr0+1
        jsr draw_list
        jsr spr_frame
        ; sprite-0 target: a solid block at rows 6-9, columns 21-26 (palette 1)
        lda #6
        sta tmp4
@t:     lda tmp4
        jsr row_addr
        pha
        txa
        ora #21
        tax
        pla
        jsr ppu_addr
        lda #T_SOLID2
        ldy #6
        jsr fill_vram
        inc tmp4
        lda tmp4
        cmp #10
        bne @t
        lda #<spr_attr
        sta ptr0
        lda #>spr_attr
        sta ptr0+1
        jsr load_attr
        lda #<spr_pal
        sta ptr0
        lda #>spr_pal
        sta ptr0+1
        jsr load_pal
        ldx #NBALLS-1
@b:     lda ball_init_x,x
        sta s_x,x
        lda ball_init_y,x
        sta s_y,x
        lda ball_init_dx,x
        sta s_dx,x
        lda ball_init_dy,x
        sta s_dy,x
        dex
        bpl @b
        lda #60
        sta s0_x
        lda #96
        sta s0_y
        lda #9
        sta s_count
        lda #0
        sta s_flick
        lda #$FF
        sta s_hit
        sta s_ovf
        jmp spr_build

; Box around the ball area: rows 3-16, columns 1-30.
spr_frame:
        lda #$20
        ldx #$61
        jsr ppu_addr
        lda #T_BOX_TL
        sta PPUDATA
        lda #T_BOX_H
        ldy #28
        jsr fill_vram
        lda #T_BOX_TR
        sta PPUDATA
        lda #4
        sta tmp4
@side:  lda tmp4
        jsr row_addr
        inx
        jsr ppu_addr
        lda #T_BOX_V
        sta PPUDATA
        lda tmp4
        jsr row_addr
        pha
        txa
        ora #30
        tax
        pla
        jsr ppu_addr
        lda #T_BOX_V
        sta PPUDATA
        inc tmp4
        lda tmp4
        cmp #16
        bne @side
        lda #$22
        ldx #$01
        jsr ppu_addr
        lda #T_BOX_BL
        sta PPUDATA
        lda #T_BOX_H
        ldy #28
        jsr fill_vram
        lda #T_BOX_BR
        sta PPUDATA
        rts

spr_update:
        ; D-pad moves sprite 0 (1 px per frame, 2 with B held)
        ldy #1
        lda pad1
        and #BTN_B
        beq @slow
        iny
@slow:  sty tmp3
        lda pad1
        and #BTN_LEFT
        beq @nl
        lda s0_x
        sec
        sbc tmp3
        bcc @nl
        sta s0_x
@nl:    lda pad1
        and #BTN_RIGHT
        beq @nr
        lda s0_x
        clc
        adc tmp3
        cmp #249
        bcs @nr
        sta s0_x
@nr:    lda pad1
        and #BTN_UP
        beq @nu
        lda s0_y
        sec
        sbc tmp3
        cmp #20
        bcc @nu
        sta s0_y
@nu:    lda pad1
        and #BTN_DOWN
        beq @nd
        lda s0_y
        clc
        adc tmp3
        cmp #130
        bcs @nd
        sta s0_y
@nd:    ; A alone: 8 or 9 sprites on the test line; A with B held: flicker on / off
        lda pad1_new
        and #BTN_A
        beq @na
        lda pad1
        and #BTN_B
        bne @flick
        lda s_count
        eor #(8^9)
        sta s_count
        lda #2
        jsr sfx_ui
        jmp @na
@flick: lda s_flick
        eor #1
        sta s_flick
        lda #2
        jsr sfx_ui
@na:    ; move the balls
        ldx #NBALLS-1
@ball:  lda s_x,x
        clc
        adc s_dx,x
        cmp #BOX_L
        bcc @bx
        cmp #BOX_R
        bcc @okx
@bx:    lda s_dx,x
        eor #$FF
        clc
        adc #1
        sta s_dx,x
        lda s_x,x
        clc
        adc s_dx,x
@okx:   sta s_x,x
        lda s_y,x
        clc
        adc s_dy,x
        cmp #BOX_T
        bcc @by
        cmp #BOX_B
        bcc @oky
@by:    lda s_dy,x
        eor #$FF
        clc
        adc #1
        sta s_dy,x
        lda s_y,x
        clc
        adc s_dy,x
@oky:   sta s_y,x
        dex
        bpl @ball
        inc s_rot
        jsr spr_build
        jmp spr_status

; OAM: 0 = crosshair (sprite 0), 1-8 balls, then the test line.
spr_build:
        lda #0
        sta oam_ptr
        lda s0_x
        sta tmp0
        lda s0_y
        sta tmp1
        lda #0
        sta tmp2
        lda #S_CROSS
        jsr oam_add
        ldx #0
@b:     stx tmp3
        lda s_x,x
        sta tmp0
        lda s_y,x
        sta tmp1
        lda ball_pal,x
        sta tmp2
        lda #S_BALL
        jsr oam_add
        ldx tmp3
        inx
        cpx #NBALLS
        bne @b
        ; the test line: s_count numbered boxes, 24 px apart; with flicker the order rotates
        lda #0
        sta tmp4
@l:     lda tmp4
        ldx s_flick
        beq @nofl
        clc
        adc s_rot
@mod:   cmp s_count
        bcc @nofl
        sec
        sbc s_count
        jmp @mod
@nofl:  sta tmp3                ; position index of this OAM slot
        asl a
        asl a
        asl a
        sta tmp0
        asl a
        clc
        adc tmp0                ; * 24
        adc #20
        sta tmp0
        lda #LINE_Y
        sta tmp1
        lda #0
        sta tmp2
        lda tmp3
        clc
        adc #'1'
        jsr oam_add
        inc tmp4
        lda tmp4
        cmp s_count
        bne @l
        jmp oam_finish

; "Sprite 0 hit: yes/no" (row 23) and "Overflow flag: yes/no" (row 24), only on change.
spr_status:
        lda status_snap
        and #$40
        cmp s_hit
        beq @nohit
        sta s_hit
        jsr yes_no
        lda #$22
        ldx #$F1
        ldy #3
        jsr vb_txt
@nohit: lda status_snap
        and #$20
        cmp s_ovf
        beq @noovf
        sta s_ovf
        jsr yes_no
        lda #$23
        ldx #$11
        ldy #3
        jsr vb_txt
@noovf: rts

; A = 0 / non-zero -> "no " / "yes" in txt.
yes_no:
        ldx #2
        cmp #0
        beq @no
@y:     lda yes_text,x
        sta txt,x
        dex
        bpl @y
        rts
@no:    lda no_text,x
        sta txt,x
        dex
        bpl @no
        rts
yes_text: .byte "yes"
no_text:  .byte "no "

ball_pal:     .byte 1, 2, 3, 1, 2, 3, 1, 2
ball_init_x:  .byte 30, 70, 110, 150, 190, 60, 130, 200
ball_init_y:  .byte 40, 70, 50, 90, 60, 100, 36, 110
ball_init_dx: .byte 1, 2, $FF, 1, $FE, 1, 2, $FF
ball_init_dy: .byte 1, $FF, 2, 1, $FF, $FE, 1, 2

spr_text:
        .byte $20, $22, "Sprites", 0                                ; row 1
        .byte $21, $55, "Target", 0                                  ; row 10, under the block
        .byte $22, $62, "At most 8 sprites per line:", 0       ; row 19
        .byte $22, $E2, "Sprite 0 hit:", 0                           ; row 23
        .byte $23, $02, "Overflow flag:", 0                          ; row 24
        .byte $23, $42, "D-Pad: move sprite 0 (B fast)", 0           ; row 26
        .byte $23, $62, "A: 8 or 9   B+A: flicker", 0                ; row 27
        .byte $FF

spr_pal:
        .byte $0F, $30, $10, $00,  $0F, $30, $16, $00,  $0F, $30, $10, $00,  $0F, $30, $10, $00
        .byte $0F, $30, $10, $02,  $0F, $16, $06, $36,  $0F, $2A, $0A, $3A,  $0F, $21, $01, $31

; Target block (rows 6-9, columns 21-26) uses palette 1.
spr_attr:
        .byte $00, $00, $00, $00, $00, $00, $00, $00      ; rows 0-3
        .byte $00, $00, $00, $00, $00, $50, $50, $00      ; rows 4-7: cols 20-27, rows 6-7
        .byte $00, $00, $00, $00, $00, $05, $05, $00      ; rows 8-9
        .res 40, $00
