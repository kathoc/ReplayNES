; ============================================================================================
;  scr_title.s - title screen and menu
; ============================================================================================
MENU_ITEMS = 7
MENU_ROW   = 13                 ; first menu row
MENU_COL   = 10

title_init:
        lda #<title_text
        sta ptr0
        lda #>title_text
        sta ptr0+1
        jsr draw_list
        ; big letters: "REPLAYNES" (row 4) and "TEST CARTRIDGE" (row 7)
        lda #<big_replaynes
        sta ptr1
        lda #>big_replaynes
        sta ptr1+1
        lda #$20
        sta tmp0
        lda #$87                ; row 4, col 7
        sta tmp1
        jsr draw_big
        lda #<big_testcart
        sta ptr1
        lda #>big_testcart
        sta ptr1+1
        lda #$20
        sta tmp0
        lda #$E2                ; row 7, col 2
        sta tmp1
        jsr draw_big
        lda #<title_attr
        sta ptr0
        lda #>title_attr
        sta ptr0+1
        jsr load_attr
        lda #<title_pal
        sta ptr0
        lda #>title_pal
        sta ptr0+1
        jsr load_pal
        jmp title_sprites

; Big text: ptr1 -> letter tile bases (0 = space), $FF ends; tmp0:tmp1 = VRAM address of the
; top-left tile. Each letter is 2x2 tiles: base, base+1 / base+2, base+3.
draw_big:
        lda tmp0
        ldx tmp1
        jsr ppu_addr
        ldy #0
@top:   lda (ptr1),y
        cmp #$FF
        beq @bottom
        cmp #0
        beq @sp1
        sta PPUDATA
        clc
        adc #1
        sta PPUDATA
        iny
        bne @top
@sp1:   sta PPUDATA
        sta PPUDATA
        iny
        bne @top
@bottom:
        lda tmp1
        clc
        adc #32
        tax
        lda tmp0
        adc #0
        jsr ppu_addr
        ldy #0
@bot:   lda (ptr1),y
        cmp #$FF
        beq @done
        cmp #0
        beq @sp2
        clc
        adc #2
        sta PPUDATA
        adc #1
        sta PPUDATA
        iny
        bne @bot
@sp2:   sta PPUDATA
        sta PPUDATA
        iny
        bne @bot
@done:  rts

title_update:
        lda pad1_new
        and #BTN_UP
        beq @noup
        dec menu_sel
        bpl @moved
        lda #MENU_ITEMS-1
        sta menu_sel
        bpl @moved
@noup:  lda pad1_new
        and #BTN_DOWN|BTN_SELECT
        beq @nodown
        inc menu_sel
        lda menu_sel
        cmp #MENU_ITEMS
        bcc @moved
        lda #0
        sta menu_sel
@moved: lda #3
        jsr sfx_ui
@nodown:
        lda pad1_new
        and #BTN_A|BTN_START
        beq title_sprites
        ldx menu_sel
        inx
        stx next_screen
title_sprites:
        lda #0
        sta oam_ptr
        lda frame               ; cursor nudges right and back every 16 frames
        lsr a
        lsr a
        lsr a
        and #3
        tax
        lda cursor_wobble,x
        clc
        adc #(MENU_COL-2)*8
        sta tmp0
        lda menu_sel
        asl a
        asl a
        asl a
        clc
        adc #MENU_ROW*8
        sta tmp1
        lda #0
        sta tmp2
        lda #S_ARROW
        jsr oam_add
        jmp oam_finish
cursor_wobble: .byte 0, 1, 2, 1

big_replaynes:
        .byte T_BIG_R, T_BIG_E, T_BIG_P, T_BIG_L, T_BIG_A, T_BIG_Y, T_BIG_N, T_BIG_E, T_BIG_S, $FF
big_testcart:
        .byte T_BIG_T, T_BIG_E, T_BIG_S, T_BIG_T, 0, T_BIG_C, T_BIG_A, T_BIG_R, T_BIG_T, T_BIG_R
        .byte T_BIG_I, T_BIG_D, T_BIG_G, T_BIG_E, $FF

title_text:
        .byte $21, $4B, "Version 1.0", 0                            ; row 10
        .byte $21, $A0+MENU_COL, "Palette chart", 0                  ; row 13
        .byte $21, $C0+MENU_COL, "Color bars", 0
        .byte $21, $E0+MENU_COL, "Sprites", 0
        .byte $22, $00+MENU_COL, "Scrolling", 0
        .byte $22, $20+MENU_COL, "Controllers", 0
        .byte $22, $40+MENU_COL, "Rapid-fire meter", 0
        .byte $22, $60+MENU_COL, "Sound test", 0                     ; row 19
        .byte $22, $C4, "Up/Down: choose   A: open", 0              ; row 22
        .byte $22, $E2, "In a test: Select = next test", 0          ; row 23
        .byte $23, $04, "Start = back to this menu", 0              ; row 24
        .byte $23, $43, "ReplayNES project - CC0 1.0", 0            ; row 26
        .byte $FF

title_pal:
        .byte $0F, $30, $10, $27,  $0F, $31, $21, $02,  $0F, $30, $10, $00,  $0F, $30, $10, $27
        .byte $0F, $27, $16, $30,  $0F, $30, $10, $00,  $0F, $30, $10, $00,  $0F, $30, $10, $00

; Rows 4-5 (big "REPLAYNES") palette 1, rows 6-9 (big "TEST CARTRIDGE") palette 2.
title_attr:
        .res 8, $00                     ; rows 0-3
        .res 8, %10100101               ; rows 4-5 palette 1, rows 6-7 palette 2
        .res 8, %00001010               ; rows 8-9 palette 2
        .res 40, $00
