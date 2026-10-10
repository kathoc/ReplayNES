; ============================================================================================
;  ReplayNES Test Cartridge  -  a test / demo program for the NES (NROM-128, mapper 0)
;
;  Written from scratch for the ReplayNES project. Program, tiles and font: CC0-1.0 (public
;  domain dedication); see tools/testcart/README.md. Assemble with tools/testcart/build.py.
;
;  Screens (Up/Down + A/Start on the title; in a test: Select = next test, Start = menu):
;    1 Palette chart   all 64 colours ($00-$3F) with emphasis / greyscale bits (raster kernel)
;    2 Colour bars     bars, grey ramp, sharpness patterns, emphasis / greyscale
;    3 Sprites         8 bouncing sprites, sprite-0 hit, 8-per-line limit + overflow flag, flicker
;    4 Scrolling       horizontal / vertical / diagonal / manual, split scroll via sprite-0 hit
;    5 Controllers     both pads, all 8 buttons, raw bytes (hold Select+Start for the menu)
;    6 Rapid-fire meter  10-second test (presses, per second, best second, interval average /
;                      deviation / histogram, best record) and a turbo checker (period / duty)
;    7 Sound test      pulse 1/2 (duty), triangle, noise, scale
;
;  Input is read once per frame (strobe $4016, 8 reads of $4016/$4017, bits 0-1 so Famicom
;  expansion pads work too; no DPCM is ever played, so the reads cannot be corrupted).
;  One frame = 1/60 s is therefore the timing resolution: at most 30 presses per second.
; ============================================================================================

; -------------------------------------------------------------------- hardware registers
PPUCTRL   = $2000
PPUMASK   = $2001
PPUSTATUS = $2002
OAMADDR   = $2003
PPUSCROLL = $2005
PPUADDR   = $2006
PPUDATA   = $2007
SQ1_VOL   = $4000
SQ1_SWEEP = $4001
SQ1_LO    = $4002
SQ1_HI    = $4003
SQ2_VOL   = $4004
SQ2_SWEEP = $4005
SQ2_LO    = $4006
SQ2_HI    = $4007
TRI_LINEAR = $4008
TRI_LO    = $400A
TRI_HI    = $400B
NOISE_VOL = $400C
NOISE_LO  = $400E
NOISE_HI  = $400F
DMC_FREQ  = $4010
OAMDMA    = $4014
SND_CHN   = $4015
JOY1      = $4016
JOY2      = $4017

; Pad bits as read (first bit read ends up in bit 7).
BTN_A      = $80
BTN_B      = $40
BTN_SELECT = $20
BTN_START  = $10
BTN_UP     = $08
BTN_DOWN   = $04
BTN_LEFT   = $02
BTN_RIGHT  = $01

CTRL_BASE  = %10001000      ; NMI on, sprites from $1000, background from $0000, +1 increment
MASK_SHOW  = %00011110      ; background + sprites, including the leftmost 8 pixels

; Screens
SCR_TITLE   = 0
SCR_PALETTE = 1
SCR_BARS    = 2
SCR_SPRITES = 3
SCR_SCROLL  = 4
SCR_PADS    = 5
SCR_METER   = 6
SCR_SOUND   = 7
NUM_SCREENS = 8

; -------------------------------------------------------------------- zero page
tmp0 = $00
tmp1 = $01
tmp2 = $02
tmp3 = $03
tmp4 = $04
tmp5 = $05
tmp6 = $06
tmp7 = $07
ptr0 = $08                  ; 2 bytes
ptr1 = $0A                  ; 2 bytes
ptr2 = $0C                  ; 2 bytes

nmi_count   = $10           ; +1 every NMI
nmi_ready   = $11           ; main loop prepared OAM + VRAM buffer for the next vblank
nmi_ppu     = $12           ; NMI may touch the PPU (0 while a screen is being drawn)
ppu_ctrl    = $13
ppu_mask    = $14
scroll_x    = $15
scroll_y    = $16
pad1        = $17           ; buttons held this frame
pad2        = $18
pad1_prev   = $19
pad2_prev   = $1A
pad1_new    = $1B           ; pressed this frame (edge)
pad2_new    = $1C
vbuf_len    = $1D
pal_dirty   = $1E
screen      = $1F
next_screen = $20           ; $FF = stay
status_snap = $21           ; PPUSTATUS read at the start of vblank (bit 6 hit, bit 5 overflow)
frame       = $22           ; frames since the screen was opened (wraps)
sfx_t1      = $23           ; remaining frames of the pulse-1 / pulse-2 blips
sfx_t2      = $24
menu_sel    = $25
emph        = $26           ; colour emphasis bits 0-2 (R, G, B) for palette / bar screens
grey        = $27           ; greyscale bit
oam_ptr     = $28
sfx_off     = $29           ; 1 = no blips (sound test owns the channels)
polling     = $2A           ; main loop is reading the pads: the NMI leaves them alone
mp_state    = $2B           ; meter: A/B state at the last poll
mq_head     = $2C           ; meter event queue (written by polls)
mq_tail     = $2D           ; (read by the meter screen)
mp_frame    = $2E           ; 2 bytes: meter frame number (+1 every NMI on the meter screen)

m_a = $30                   ; 32-bit math accumulator (little endian)
m_b = $34                   ; 32-bit operand
m_r = $38                   ; remainder / scratch
m_t = $3C                   ; scratch

sv = $40                    ; $40-$6F: state of the current screen (each screen names its own)
kz = $70                    ; $70-$7D: raster kernel variables
nmi_seen = $7E              ; nmi_count when the main loop last woke up

; -------------------------------------------------------------------- RAM
oam     = $0200
vbuf    = $0300             ; VRAM update list: hi, lo, len (bit 7 = +32), data ... ; 0 ends it
VBUF_SOFT_LIMIT = 72        ; queue optional updates only below this (vblank budget)
pal_buf = $0380             ; 32 palette bytes, written in vblank when pal_dirty
txt     = $03A0             ; 32-byte text scratch for number formatting
big     = $0400             ; $0400-$06FF: large per-screen state
persist = $0700             ; kept across soft reset (guarded by a magic number)
best_a      = persist + 0   ; best 10-second press counts (16 bit)
best_b      = persist + 2
persist_magic = persist + $FC

; Tile numbers T_* (background) and S_* (sprites) come from chr.py.

.org $C000
.include "lib.s"
.include "math.s"
.include "sound.s"
.include "scr_title.s"
.include "scr_palette.s"
.include "scr_bars.s"
.include "scr_sprites.s"
.include "scr_scroll.s"
.include "scr_pads.s"
.include "scr_meter.s"
.include "scr_sound.s"

; -------------------------------------------------------------------- screen dispatch
screen_init_lo:   .byte <title_init-1, <pal_init-1, <bars_init-1, <spr_init-1, <scr_init-1, <pads_init-1, <meter_init-1, <snd_init-1
screen_init_hi:   .byte >title_init-1, >pal_init-1, >bars_init-1, >spr_init-1, >scr_init-1, >pads_init-1, >meter_init-1, >snd_init-1
screen_upd_lo:    .byte <title_update-1, <pal_update-1, <bars_update-1, <spr_update-1, <scr_update-1, <pads_update-1, <meter_update-1, <snd_update-1
screen_upd_hi:    .byte >title_update-1, >pal_update-1, >bars_update-1, >spr_update-1, >scr_update-1, >pads_update-1, >meter_update-1, >snd_update-1
screen_kern_lo:   .byte <no_kernel-1, <pal_kernel-1, <no_kernel-1, <no_kernel-1, <scr_kernel-1, <no_kernel-1, <no_kernel-1, <no_kernel-1
screen_kern_hi:   .byte >no_kernel-1, >pal_kernel-1, >no_kernel-1, >no_kernel-1, >scr_kernel-1, >no_kernel-1, >no_kernel-1, >no_kernel-1

; Calls table entry X of the table pair at (lo, hi) via RTS (addresses stored minus one).
call_init:
        lda screen_init_hi,x
        pha
        lda screen_init_lo,x
        pha
        rts
call_update:
        lda screen_upd_hi,x
        pha
        lda screen_upd_lo,x
        pha
        rts
call_kernel:
        lda screen_kern_hi,x
        pha
        lda screen_kern_lo,x
        pha
        rts
no_kernel:
        rts

; -------------------------------------------------------------------- reset
reset:
        sei
        cld
        ldx #$40
        stx JOY2                ; APU frame IRQ off
        ldx #$FF
        txs
        inx
        stx PPUCTRL
        stx PPUMASK
        stx DMC_FREQ            ; no DMC IRQ (and DPCM is never used)
        bit PPUSTATUS
@vbl1:  bit PPUSTATUS
        bpl @vbl1
        txa
@clear: sta $00,x               ; clear RAM except the persistent page
        sta $0100,x
        sta $0200,x
        sta $0300,x
        sta $0400,x
        sta $0500,x
        sta $0600,x
        inx
        bne @clear
        ldx #3                  ; persistent page valid?
@magic: lda persist_magic,x
        cmp magic_text,x
        bne @wipe
        dex
        bpl @magic
        jmp @kept
@wipe:  lda #0
        tax
@wipe1: sta persist,x
        inx
        bne @wipe1
        ldx #3
@wipe2: lda magic_text,x
        sta persist_magic,x
        dex
        bpl @wipe2
@kept:
@vbl2:  bit PPUSTATUS
        bpl @vbl2
        jsr apu_init
        jsr oam_hide_all
        lda #CTRL_BASE
        sta ppu_ctrl
        sta PPUCTRL             ; NMI on
        lda #SCR_TITLE
        sta next_screen

main_loop:
        lda next_screen
        bmi @stay
        jsr switch_screen
@stay:  jsr wait_nmi
        inc frame
        jsr global_input
        ldx screen
        jsr call_update
        ldx vbuf_len            ; terminate the VRAM list
        lda #0
        sta vbuf,x
        lda #1
        sta nmi_ready
        ldx screen
        jsr call_kernel         ; raster effects of this frame (palette chart, split scroll)
        jmp main_loop

magic_text: .byte "RNTC"

; Start = menu, Select = next test. The controller screen exits with Select+Start held.
global_input:
        lda screen
        beq @done               ; the title handles its own input
        cmp #SCR_PADS
        beq @pads
        lda pad1_new
        and #BTN_START
        beq @nostart
        lda #SCR_TITLE
        sta next_screen
        rts
@nostart:
        lda pad1_new
        and #BTN_SELECT
        beq @done
        ldx screen
        inx
        cpx #NUM_SCREENS
        bne @set
        ldx #1
@set:   stx next_screen
@done:  rts
@pads:  lda pad1
        and #BTN_SELECT|BTN_START
        cmp #BTN_SELECT|BTN_START
        bne @done
        lda #SCR_TITLE
        sta next_screen
        rts

; Turns the screen off, draws the new one and lets the next vblank turn it on again.
switch_screen:
        lda #0
        sta nmi_ppu             ; NMI must not touch the PPU while we draw
        jsr wait_vblank
        lda #0
        sta PPUMASK             ; rendering off (we are in vblank)
        lda next_screen
        sta screen
        lda #$FF
        sta next_screen
        lda #0
        sta frame
        sta vbuf_len
        sta vbuf
        sta scroll_x
        sta scroll_y
        sta sfx_off
        jsr snd_stop_all
        lda #CTRL_BASE
        sta ppu_ctrl
        sta PPUCTRL
        lda #MASK_SHOW
        sta ppu_mask
        jsr clear_nametables
        jsr oam_hide_all
        ldx screen
        jsr call_init           ; draws with rendering off, fills pal_buf and OAM
        jsr write_palette
        lda #0
        sta pal_dirty
        ldx vbuf_len
        lda #0
        sta vbuf,x
        lda #1
        sta nmi_ppu
        sta nmi_ready
        lda nmi_count           ; the main loop waits for the vblank that turns the screen on
        sta nmi_seen
        lda #1                  ; a short blip for every screen change
        jsr sfx_ui
        rts

; -------------------------------------------------------------------- NMI / IRQ
nmi:
        pha
        txa
        pha
        tya
        pha
        lda nmi_ppu
        beq @no_ppu
        lda PPUSTATUS           ; resets the $2005/$2006 latch; flags of the frame just drawn
        sta status_snap
        lda nmi_ready
        beq @scroll
        lda #0
        sta OAMADDR
        lda #>oam
        sta OAMDMA
        jsr flush_vbuf
        lda pal_dirty
        beq @nopal
        jsr write_palette
        lda #0
        sta pal_dirty
@nopal: lda #0
        sta nmi_ready
        sta vbuf_len
        sta vbuf
@scroll:
        lda ppu_ctrl
        sta PPUCTRL
        lda scroll_x
        sta PPUSCROLL
        lda scroll_y
        sta PPUSCROLL
        lda ppu_mask
        sta PPUMASK
@no_ppu:
        lda polling
        bne @skip_pads          ; the main loop is in the middle of reading the pads
        jsr read_pads
        jmp @pads_done
@skip_pads:
        lda #0
        sta pad1_new
        sta pad2_new
@pads_done:
        lda screen
        cmp #SCR_METER
        bne @no_meter
        inc mp_frame
        bne @mf
        inc mp_frame+1
@mf:    lda polling
        bne @no_meter
        lda pad1
        jsr meter_edges         ; poll 1 of the frame
@no_meter:
        jsr sound_tick
        inc nmi_count
        pla
        tay
        pla
        tax
        pla
irq:    rti

; Both pads: strobe, then 8 reads each. Bits 0-1 so a Famicom expansion pad works as well.
read_pads:
        lda pad1
        sta pad1_prev
        lda pad2
        sta pad2_prev
        lda #1
        sta JOY1
        sta pad2                ; ring counter: the 1 shifts out after 8 reads
        lsr a
        sta JOY1
@loop:  lda JOY1
        and #%00000011
        cmp #1
        rol pad1
        lda JOY2
        and #%00000011
        cmp #1
        rol pad2
        bcc @loop
        lda pad1_prev
        eor #$FF
        and pad1
        sta pad1_new
        lda pad2_prev
        eor #$FF
        and pad2
        sta pad2_new
        rts

; -------------------------------------------------------------------- vectors
.org $FFFA
        .word nmi, reset, irq
