; ============================================================================================
;  scr_scroll.s - background scrolling over a 512x240 world (two nametables, vertical mirroring)
;
;  The world is a checkerboard of 16x16 squares labelled A1..H5 every 4 x 3 squares. The bottom
;  rows 26-29 of the left nametable hold a status bar. In "Split" mode that bar is pinned to the
;  top of the screen: the frame starts at Y=208, a sprite-0 hit on the bar's bottom line marks
;  line 31, and the X/Y/nametable of the playfield are set mid-frame ($2006/$2005/$2005/$2006).
; ============================================================================================
sc_x     = sv + 0               ; 0..511
sc_xh    = sv + 1
sc_y     = sv + 2               ; 0..239
sc_mode  = sv + 3               ; 0 H, 1 V, 2 diagonal, 3 D-pad, 4 split
sc_speed = sv + 4               ; 1..4 pixels per frame
sc_shown = sv + 5               ; mode/speed currently drawn ($FF = redraw)
sk_lo    = kz + 0               ; split kernel: second $2006 value
sk_wait  = sv + 6               ; frames until sprite 0 is surely in the PPU's OAM

MODE_SPLIT = 4
SPLIT_D = 7                     ; cycles after the sprite-0 hit (x=200, line 31) before the writes

scr_init:
        lda #$20
        sta tmp5                ; nametable high byte: $20, then $24
@nt:    lda tmp5
        ldx #0
        jsr ppu_addr
        lda #0
        sta tmp4                ; row
@row:   ldx #0                  ; column
@col:   stx tmp3
        jsr world_tile
        sta PPUDATA
        ldx tmp3
        inx
        cpx #32
        bne @col
        inc tmp4
        lda tmp4
        cmp #30
        bne @row
        lda tmp5
        clc
        adc #4
        sta tmp5
        cmp #$28
        bne @nt
        ; status bar, nametable 0 rows 26-29
        lda #$23
        ldx #$40
        jsr ppu_addr
        lda #0
        ldy #96
        jsr fill_vram           ; rows 26-28
        lda #$23
        ldx #$A0
        jsr ppu_addr
        lda #T_HUDLINE
        ldy #32
        jsr fill_vram           ; row 29
        lda #<scr_text
        sta ptr0
        lda #>scr_text
        sta ptr0+1
        jsr draw_list
        lda #<scr_pal
        sta ptr0
        lda #>scr_pal
        sta ptr0+1
        jsr load_pal
        lda #0
        sta sc_x
        sta sc_xh
        sta sc_y
        sta sc_mode
        lda #1
        sta sc_speed
        lda #$FF
        sta sc_shown
        jmp scr_present

; Tile of world square at nametable tmp5 ($20/$24), row tmp4, column X.
world_tile:
        txa
        lsr a                   ; square x within this nametable
        ldy tmp5
        cpy #$24
        bne @left
        ora #16
@left:  sta tmp0                ; sx 0..31
        lda tmp4
        lsr a
        sta tmp1                ; sy 0..14
        clc
        adc tmp0
        and #1
        sta tmp2                ; parity: 0 -> colour 2, 1 -> colour 3
        lda tmp4
        and #1
        bne @plain              ; labels only in the top row of a square
        lda tmp0
        and #3
        bne @plain
        ldy tmp1
        lda label_row,y
        bmi @plain
        sta tmp6                ; 0..4
        txa
        and #1
        bne @digit
        lda tmp0
        lsr a
        lsr a                   ; 0..7 -> A..H
        jmp @label
@digit: lda tmp6
        clc
        adc #8                  ; 1..5 follow A..H in the tile list
@label: ldy tmp2
        beq @p2
        clc
        adc #T_W2_A
        rts
@p2:    clc
        adc #T_W_A
        rts
@plain: lda tmp2
        beq @c2
        lda #T_SOLID3
        rts
@c2:    lda #T_SOLID2
        rts
label_row: .byte 0, $FF, $FF, 1, $FF, $FF, 2, $FF, $FF, 3, $FF, $FF, 4, $FF, $FF

scr_update:
        lda pad1_new
        and #BTN_A
        beq @na
        inc sc_mode
        lda sc_mode
        cmp #5
        bcc @m_ok
        lda #0
        sta sc_mode
@m_ok:  lda #2
        sta sk_wait             ; the sync sprite reaches the PPU with the next vblank
        jsr sfx_ui
@na:    lda pad1_new
        and #BTN_B
        beq @nb
        lda sc_speed
        and #3
        clc
        adc #1
        sta sc_speed
        lda #2
        jsr sfx_ui
@nb:    lda sc_mode
        cmp #3
        bcs @manual             ; D-pad and split: moved by the D-pad
        cmp #1
        beq @vert               ; V: Y only
        jsr sc_right            ; H, diagonal, split: X
        lda sc_mode
        beq @moved              ; H: X only
@vert:  jsr sc_down
        jmp @moved
@manual:
        lda pad1
        and #BTN_RIGHT
        beq @m1
        jsr sc_right
@m1:    lda pad1
        and #BTN_LEFT
        beq @m2
        jsr sc_left
@m2:    lda pad1
        and #BTN_DOWN
        beq @m3
        jsr sc_down
@m3:    lda pad1
        and #BTN_UP
        beq @moved
        jsr sc_up
@moved:
scr_present:
        ; registers for the next frame
        lda sc_mode
        cmp #MODE_SPLIT
        beq @split
        lda sc_x
        sta scroll_x
        lda sc_y
        sta scroll_y
        lda sc_xh
        and #1
        ora #CTRL_BASE
        sta ppu_ctrl
        jmp @spr
@split: lda #0
        sta scroll_x
        lda #208                ; rows 26-29 at the top: status bar
        sta scroll_y
        lda #CTRL_BASE
        sta ppu_ctrl
@spr:   jsr scr_sprites
        ; X / Y digits in the status bar (row 27)
        lda sc_x
        sta m_a
        lda sc_xh
        sta m_a+1
        lda #3
        sta tmp4
        ldy #0
        jsr fmt_dec16
        lda #$23
        ldx #$73                ; row 27, col 19
        ldy #3
        jsr vb_txt
        lda sc_y
        ldy #0
        jsr fmt_dec3
        lda #$23
        ldx #$7A                ; row 27, col 26
        ldy #3
        jsr vb_txt
        ; mode and speed (row 28) on change
        lda sc_mode
        asl a
        asl a
        asl a
        ora sc_speed
        cmp sc_shown
        beq @done
        sta sc_shown
        jsr mode_name
        ldy #0
        jsr fmt_str
        lda #' '
        sta txt,y
        iny
        lda #'x'
        sta txt,y
        iny
        lda sc_speed
        ora #'0'
        sta txt,y
        lda #$23
        ldx #$88                ; row 28, col 8
        ldy #11
        jmp vb_txt
@done:  rts

; ptr0 = name of sc_mode (8 characters).
mode_name:
        ldx sc_mode
        lda mode_names_lo,x
        sta ptr0
        lda mode_names_hi,x
        sta ptr0+1
        rts
mode_names_lo: .byte <mn0, <mn1, <mn2, <mn3, <mn4
mode_names_hi: .byte >mn0, >mn1, >mn2, >mn3, >mn4
mn0: .byte "H-scroll", 0
mn1: .byte "V-scroll", 0
mn2: .byte "Diagonal", 0
mn3: .byte "D-Pad   ", 0
mn4: .byte "Split   ", 0          ; status bar fixed, D-pad scrolls

; Sprites: 0 = split sync pixel (or hidden); outside split, a readout over the world.
scr_sprites:
        lda #0
        sta oam_ptr
        lda #200
        sta tmp0
        lda #31
        sta tmp1
        lda #%00100000
        sta tmp2
        lda sc_mode
        cmp #MODE_SPLIT
        beq @s0
        lda #$F8                ; below the picture: no hit
        sta tmp1
@s0:    lda #S_DOT
        jsr oam_add
        lda sc_mode
        cmp #MODE_SPLIT
        beq @fin
        jsr mode_name
        lda #16
        sta tmp0
        lda #186
        sta tmp1
        lda #0
        sta tmp2
        jsr oam_text
        ; "X:nnn" and "Y:nnn"
        lda sc_x
        sta m_a
        lda sc_xh
        sta m_a+1
        lda #3
        sta tmp4
        ldy #2
        jsr fmt_dec16
        lda #'X'
        sta txt
        lda #':'
        sta txt+1
        lda #0
        sta txt+5
        lda #<txt
        sta ptr0
        lda #>txt
        sta ptr0+1
        lda #16
        sta tmp0
        lda #194
        sta tmp1
        jsr oam_text
        lda sc_y
        ldy #2
        jsr fmt_dec3
        lda #'Y'
        sta txt
        lda #':'
        sta txt+1
        lda #0
        sta txt+5
        lda #16
        sta tmp0
        lda #202
        sta tmp1
        jsr oam_text
@fin:   jmp oam_finish

sc_right:
        lda sc_x
        clc
        adc sc_speed
        sta sc_x
        lda sc_xh
        adc #0
        and #1
        sta sc_xh
        rts
sc_left:
        lda sc_x
        sec
        sbc sc_speed
        sta sc_x
        lda sc_xh
        sbc #0
        and #1
        sta sc_xh
        rts
sc_down:
        lda sc_y
        clc
        adc sc_speed
        cmp #240
        bcc @ok
        sbc #240
@ok:    sta sc_y
        rts
sc_up:
        lda sc_y
        sec
        sbc sc_speed
        bcs @ok
        adc #240
@ok:    sta sc_y
        rts

; Split: after the sprite-0 hit at x=200 of line 31, point the PPU at (sc_x, sc_y). The last
; write ($2006) copies the address during the horizontal blank of line 31.
scr_kernel:
        lda sc_mode
        cmp #MODE_SPLIT
        bne @r
        lda sk_wait             ; without the sprite there is no hit: never wait for it (a
        beq @go                 ; $2002 poll at the start of vblank can swallow the NMI)
        dec sk_wait
        rts
@go:
        lda sc_y
        and #$F8
        asl a
        asl a
        sta sk_lo
        lda sc_x
        lsr a
        lsr a
        lsr a
        ora sk_lo
        sta sk_lo
        ldx sc_y
        ldy sc_x
        lda sc_xh
        asl a
        asl a                   ; nametable bits for the first $2006 write
@clr:   bit PPUSTATUS
        bvs @clr
@hit:   bit PPUSTATUS
        bvc @hit
        .delay SPLIT_D
        sta PPUADDR
        stx PPUSCROLL
        sty PPUSCROLL
        lda sk_lo
        sta PPUADDR
@r:     rts

scr_text:
        .byte $23, $62, "Scrolling", 0                  ; row 27
        .byte $23, $71, "X:", 0
        .byte $23, $78, "Y:", 0
        .byte $23, $82, "Mode:", 0                      ; row 28
        .byte $23, $94, "A:mode B:spd", 0
        .byte $FF

scr_pal:
        .byte $0F, $30, $01, $11,  $0F, $30, $01, $11,  $0F, $30, $01, $11,  $0F, $30, $01, $11
        .byte $0F, $30, $10, $0F,  $0F, $30, $10, $0F,  $0F, $30, $10, $0F,  $0F, $30, $10, $0F
