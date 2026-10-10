; ============================================================================================
;  scr_meter.s - rapid-fire meter (連射測定) for A and B
;
;  Input is polled once in the NMI and three more times per frame from the main loop (strobe +
;  8 reads each). Every change of A / B seen by any poll is an edge event: press (released ->
;  held) or release (held -> released), stamped with the frame number. On real hardware this
;  also catches taps shorter than a frame; in an emulator the pads change once per frame, so
;  the resolution there is 1/60 s.
;
;  10-second test: starts with the first press. The window is 600 frame samples: the sample
;  before the first press and the 599 after it, so at most 599 transitions ("edges") and 300
;  presses fit (60 samples: 59 edges, 30 presses). Per button: presses, presses per second,
;  edges, best 60-sample window (presses / edges), mean and deviation of press-to-press and
;  release-to-release intervals, mean hold (press -> release) and gap (release -> press), duty,
;  the last hold / gap, fastest press interval, best 10 s (kept until power off), and a
;  histogram of press intervals. Intervals and holds / gaps longer than 30 frames (0.5 s) are
;  pauses and stay out of the means and deviations.
;
;  Turbo check: per button the last period (press to press), presses per second, hold / gap,
;  duty, the range of the last 8 periods, and a 30-frame trace of the button.
; ============================================================================================

; ---- event queue (filled by the polls)
mq_type = big + $00             ; 32: bit 0 = button (0 A, 1 B), bit 1 = press
mq_flo  = big + $20             ; 32: frame
mq_fhi  = big + $40

; ---- per button (index 0 = A, 1 = B)
m_cnt_lo   = big + $60          ; presses in the run
m_cnt_hi   = big + $62
m_ecnt_lo  = big + $64          ; edges (presses + releases) in the run
m_ecnt_hi  = big + $66
m_lastp_lo = big + $68          ; frame of the last press / release
m_lastp_hi = big + $6A
m_lastr_lo = big + $6C
m_lastr_hi = big + $6E
m_flags    = big + $70          ; bit 0 seen a press, bit 1 seen a release
pp_n_lo    = big + $72          ; press-to-press intervals <= 30 frames: count, sum, sum of squares
pp_n_hi    = big + $74
pp_s1_lo   = big + $76
pp_s1_hi   = big + $78
pp_s2_0    = big + $7A
pp_s2_1    = big + $7C
pp_s2_2    = big + $7E
rr_n_lo    = big + $80          ; release-to-release intervals (same layout, +$0E)
hd_n_lo    = big + $8E          ; holds <= 30 frames: count, sum
hd_n_hi    = big + $90
hd_s1_lo   = big + $92
hd_s1_hi   = big + $94
gp_n_lo    = big + $96          ; gaps <= 30 frames: count, sum
gp_n_hi    = big + $98
gp_s1_lo   = big + $9A
gp_s1_hi   = big + $9C
m_lhold    = big + $9E          ; last hold / gap (frames, 255 = more)
m_lgap     = big + $A0
m_minpp    = big + $A2          ; fastest press interval (255 = none)
m_peakp    = big + $A4          ; most presses / edges in 60 samples
m_peake    = big + $A6
m_hist_lo  = big + $A8          ; 2 x 4 bins: <=2, 3, 4, 5+ frames (A: +0..3, B: +4..7)
m_hist_hi  = big + $B2
r_rate_lo  = big + $BC          ; results: presses per second x10
r_rate_hi  = big + $BE
r_ppavg_lo = big + $C0          ; x100 frames
r_ppavg_hi = big + $C2
r_ppsd_lo  = big + $C4
r_ppsd_hi  = big + $C6
r_rravg_lo = big + $C8
r_rravg_hi = big + $CA
r_rrsd_lo  = big + $CC
r_rrsd_hi  = big + $CE
r_hdavg_lo = big + $D0
r_hdavg_hi = big + $D2
r_gpavg_lo = big + $D4
r_gpavg_hi = big + $D6
r_duty     = big + $D8          ; percent (255 = none)
q_head     = big + $DA          ; 60-sample windows: 4 queues (press A, press B, edge A, edge B)
q_tail     = big + $DE          ; of event frames (low byte), 64 entries each at qbuf + 64 * id
t_per      = big + $E2          ; turbo: last period, hold, gap, duty, min / max of the ring
t_hold     = big + $E4
t_gap      = big + $E6
t_duty     = big + $E8
t_min      = big + $EA
t_max      = big + $EC
t_ridx     = big + $EE
t_rcnt     = big + $F0
t_lastev_lo = big + $F2         ; frame of the last edge (idle / held detection)
t_lastev_hi = big + $F4
m_mode     = big + $FE          ; 0 = 10-second test, 1 = turbo check (kept across redraws)
qbuf       = $0500
t_ring     = $0600              ; 8 periods per button: A $0600, B $0608
best_e     = persist + 4        ; best 10-second edge counts (A, B; 16 bit)

; ---- zero page (screen state)
mt_x     = sv + 0               ; button being processed
mt_ev    = sv + 1
mt_flo   = sv + 2               ; event frame
mt_fhi   = sv + 3
mt_state = sv + 4               ; 0 ready, 1 running, 2 done
mt_t0lo  = sv + 5               ; frame of the first press of the run
mt_t0hi  = sv + 6
mt_cool  = sv + 7               ; frames before a finished test can restart
mt_item  = sv + 8               ; round-robin display item
mt_calc  = sv + 9               ; round-robin statistics step
mt_shown = sv + 10              ; status line currently shown
w_pos    = sv + 11              ; trace column 0-29
w_prev   = sv + 12              ; 2: level of the previous trace column
st_n     = sv + 14              ; statistics input / output
st_s1    = sv + 16
st_s2    = sv + 18              ; 3 bytes
st_avg   = sv + 21
st_sd    = sv + 23
mt_iv    = sv + 25              ; interval being added (frames, capped at 255)
mt_tmp   = sv + 26              ; 2
mt_base  = sv + 28              ; window queue offset in qbuf
mk_n     = kz + 0               ; meter poll loop
mk_left  = kz + 1
mk_pad   = kz + 2

COL_A = 13                      ; value columns
COL_B = 22
WIN   = 599                     ; last frame of the 10-second window, relative to the first press
STAT_MAX = 30                   ; longer intervals / holds / gaps are pauses

; ============================================================================ polls
; Enqueues the A / B changes of pad byte A at frame mp_frame. Uses A, X, Y (NMI safe).
meter_edges:
        and #BTN_A|BTN_B
        tay
        eor mp_state
        beq @done
        pha
        and #BTN_A
        beq @nb_a
        tya
        and #BTN_A
        beq @rel_a
        lda #%10
        bne @qa
@rel_a: lda #%00
@qa:    jsr mq_push
@nb_a:  pla
        and #BTN_B
        beq @done
        tya
        and #BTN_B
        beq @rel_b
        lda #%11
        bne @qb
@rel_b: lda #%01
@qb:    jsr mq_push
@done:  sty mp_state
        rts

mq_push:
        ldx mq_head
        sta mq_type,x
        lda mp_frame
        sta mq_flo,x
        lda mp_frame+1
        sta mq_fhi,x
        inx
        txa
        and #31
        sta mq_head
        rts

; Main loop part of the frame: three more polls about a quarter frame apart, until the NMI.
meter_kernel:
        lda nmi_count
        sta mk_n
        lda #3
        sta mk_left
@poll:  ldy #6
@wait:  ldx #0                  ; ~1280 cycles
@d:     dex
        bne @d
        lda nmi_count
        cmp mk_n
        bne @r                  ; the next frame started: back to the main loop
        dey
        bne @wait
        jsr meter_poll
        dec mk_left
        bne @poll
@r:     rts

meter_poll:
        lda #1
        sta polling             ; the NMI must not strobe the pad between our reads
        sta JOY1
        sta mk_pad
        lsr a
        sta JOY1
@r:     lda JOY1
        and #%00000011
        cmp #1
        rol mk_pad
        bcc @r
        lda mk_pad
        jsr meter_edges
        lda #0
        sta polling
        rts

; ============================================================================ init
meter_init:
        lda #0
        sta mq_tail
        sta mq_head
        sta mt_state
        sta mt_item
        sta mt_calc
        sta w_pos
        sta w_prev
        sta w_prev+1
        lda #$FF
        sta mt_shown
        lda pad1
        and #BTN_A|BTN_B
        sta mp_state
        jsr meter_clear
        lda m_mode
        bne @turbo
        lda #<meter_text
        sta ptr0
        lda #>meter_text
        sta ptr0+1
        jsr draw_list
        lda #<meter_attr
        sta ptr0
        lda #>meter_attr
        sta ptr0+1
        jmp @common
@turbo: lda #<turbo_text
        sta ptr0
        lda #>turbo_text
        sta ptr0+1
        jsr draw_list
        ; trace baselines: rows 14 and 17, columns 1-30
        lda #$21
        ldx #$C1
        jsr ppu_addr
        lda #T_WAVE_LL
        ldy #30
        jsr fill_vram
        lda #$22
        ldx #$21
        jsr ppu_addr
        lda #T_WAVE_LL
        ldy #30
        jsr fill_vram
        lda #<turbo_attr
        sta ptr0
        lda #>turbo_attr
        sta ptr0+1
@common:
        jsr load_attr
        lda #<meter_pal
        sta ptr0
        lda #>meter_pal
        sta ptr0+1
        jsr load_pal
        ; draw every value once now (rendering is off)
        ldx #0
@all:   stx mt_item
        jsr meter_item
        ldx vbuf_len
        lda #0
        sta vbuf,x
        jsr flush_vbuf
        lda #0
        sta vbuf_len
        ldx mt_item
        inx
        cpx #ITEMS
        bne @all
        lda #0
        sta mt_item
        jsr meter_status
        ldx vbuf_len
        lda #0
        sta vbuf,x
        jsr flush_vbuf
        lda #0
        sta vbuf_len
        rts

; Clears the statistics of both buttons (a new run).
meter_clear:
        ldx #$60
        lda #0
@c:     sta big,x
        inx
        cpx #$FE
        bne @c
        ldx #1
@b:     lda #255
        sta m_minpp,x
        sta m_lhold,x
        sta m_lgap,x
        sta r_duty,x
        sta t_per,x
        dex
        bpl @b
        rts

; ============================================================================ update
meter_update:
        lda pad1_new
        and #BTN_LEFT|BTN_RIGHT
        beq @nomode
        lda m_mode
        eor #1
        sta m_mode
        lda #SCR_METER          ; redraw in the other mode
        sta next_screen
        rts
@nomode:
        lda pad1_new
        and #BTN_DOWN
        beq @noreset
        lda #SCR_METER          ; reset: redraw, clear
        sta next_screen
        rts
@noreset:
        ; events
@ev:    ldx mq_tail
        cpx mq_head
        beq @noev
        lda mq_type,x
        sta mt_ev
        lda mq_flo,x
        sta mt_flo
        lda mq_fhi,x
        sta mt_fhi
        inx
        txa
        and #31
        sta mq_tail
        jsr meter_event
        jmp @ev
@noev:  jsr window_prune_all
        lda m_mode
        bne @turbo
        jsr test_clock
        jsr meter_calc
        jsr meter_time
        jsr meter_next_item
        jsr meter_next_item
        jmp meter_status
@turbo: jsr turbo_trace
        jsr meter_next_item
        jmp meter_status

meter_next_item:
        ldx mt_item
        inx
        lda m_mode
        beq @t
        cpx #ITEMS_TURBO
        bcc @ok
        ldx #0
@t:     cpx #ITEMS
        bcc @ok
        ldx #0
@ok:    stx mt_item
        lda vbuf_len
        cmp #VBUF_SOFT_LIMIT-24
        bcs @r
        jmp meter_item
@r:     rts

; One edge event (mt_ev, mt_flo/hi).
meter_event:
        lda mt_ev
        and #1
        sta mt_x
        tax
        lda mt_ev
        and #2
        beq @nobeep
        txa                     ; press blip: A high, B lower
        bne @bb
        lda #36
        ldy #2
        jsr blip1
        jmp @nobeep
@bb:    lda #31
        ldy #2
        jsr blip2
@nobeep:
        lda m_mode
        beq @test
        jmp turbo_event
@test:  lda mt_state
        beq @ready
        cmp #2
        bne @running
        lda mt_cool             ; done: a press after the cool-down starts again
        bne @ignore
@ready: lda mt_ev
        and #2
        beq @ignore             ; runs start with a press
        jsr meter_clear
        lda mt_flo
        sta mt_t0lo
        lda mt_fhi
        sta mt_t0hi
        lda #1
        sta mt_state
@running:
        ; outside the window (frame > t0 + WIN - 1)? then the run is over
        jsr frames_since_t0     ; m_a = frame - t0
        lda m_a
        cmp #<WIN
        lda m_a+1
        sbc #>WIN
        bcc @in
        jsr test_finish
@ignore:
        rts
@in:    ldx mt_x
        inc m_ecnt_lo,x
        bne @e1
        inc m_ecnt_hi,x
@e1:    lda mt_x                ; edges in the last 60 samples
        ora #2
        jsr window_push
        ldx mt_x
        cmp m_peake,x
        bcc @pe
        sta m_peake,x
@pe:    lda mt_ev
        and #2
        bne @press
        jmp @release
@press: inc m_cnt_lo,x
        bne @c1
        inc m_cnt_hi,x
@c1:    lda mt_x                ; presses in the last 60 samples
        jsr window_push
        ldx mt_x
        cmp m_peakp,x
        bcc @pp
        sta m_peakp,x
@pp:    lda m_flags,x
        and #1
        beq @nopp
        jsr since_lastp         ; mt_iv = frame - last press (capped 255)
        jsr hist_add
        ldx mt_x
        lda mt_iv
        cmp m_minpp,x
        bcs @nomin
        sta m_minpp,x
@nomin: lda #0                  ; press-to-press group at offset 0
        jsr interval_add
@nopp:  ldx mt_x
        lda m_flags,x
        and #2
        beq @nogap
        jsr since_lastr
        ldx mt_x
        lda mt_iv
        sta m_lgap,x
        cmp #STAT_MAX+1
        bcs @nogap
        clc
        adc gp_s1_lo,x
        sta gp_s1_lo,x
        bcc @g1
        inc gp_s1_hi,x
@g1:    inc gp_n_lo,x
        bne @nogap
        inc gp_n_hi,x
@nogap: ldx mt_x
        lda mt_flo
        sta m_lastp_lo,x
        lda mt_fhi
        sta m_lastp_hi,x
        lda m_flags,x
        ora #1
        sta m_flags,x
        rts
@release:
        lda m_flags,x
        and #2
        beq @norr
        jsr since_lastr
        lda #rr_n_lo-pp_n_lo    ; release-to-release group
        jsr interval_add
@norr:  ldx mt_x
        lda m_flags,x
        and #1
        beq @nohold
        jsr since_lastp
        ldx mt_x
        lda mt_iv
        sta m_lhold,x
        cmp #STAT_MAX+1
        bcs @nohold
        clc
        adc hd_s1_lo,x
        sta hd_s1_lo,x
        bcc @h1
        inc hd_s1_hi,x
@h1:    inc hd_n_lo,x
        bne @nohold
        inc hd_n_hi,x
@nohold:
        ldx mt_x
        lda mt_flo
        sta m_lastr_lo,x
        lda mt_fhi
        sta m_lastr_hi,x
        lda m_flags,x
        ora #2
        sta m_flags,x
        rts

; m_a = event frame - t0 (16 bit).
frames_since_t0:
        lda mt_flo
        sec
        sbc mt_t0lo
        sta m_a
        lda mt_fhi
        sbc mt_t0hi
        sta m_a+1
        rts

; mt_iv = event frame - last press / release of mt_x, capped at 255. X = mt_x.
since_lastp:
        ldx mt_x
        lda mt_flo
        sec
        sbc m_lastp_lo,x
        sta mt_iv
        lda mt_fhi
        sbc m_lastp_hi,x
        jmp cap_iv
since_lastr:
        ldx mt_x
        lda mt_flo
        sec
        sbc m_lastr_lo,x
        sta mt_iv
        lda mt_fhi
        sbc m_lastr_hi,x
cap_iv: beq @ok
        lda #255
        sta mt_iv
@ok:    rts

; Adds mt_iv to the interval group at offset A (0 = press-to-press, $0E = release-to-release)
; of button mt_x, if it is not a pause.
interval_add:
        clc
        adc mt_x
        tax                     ; X = offset + button
        lda mt_iv
        cmp #STAT_MAX+1
        bcs @r
        inc pp_n_lo,x
        bne @n1
        inc pp_n_hi,x
@n1:    clc
        adc pp_s1_lo,x
        sta pp_s1_lo,x
        bcc @s1
        inc pp_s1_hi,x
@s1:    stx mt_tmp
        lda mt_iv
        jsr square8             ; m_a = iv^2 (uses X)
        ldx mt_tmp
        clc
        lda m_a
        adc pp_s2_0,x
        sta pp_s2_0,x
        lda m_a+1
        adc pp_s2_1,x
        sta pp_s2_1,x
        bcc @r
        inc pp_s2_2,x
@r:     rts

; Histogram of press intervals: <=2, 3, 4, 5+ frames (30, 20, 15, <=12 presses/s).
hist_add:
        lda mt_iv
        cmp #5
        bcc @small
        lda #3
        bne @bin
@small: tax
        lda hist_bin,x
@bin:   ldx mt_x
        beq @a
        clc
        adc #4
@a:     tax
        inc m_hist_lo,x
        bne @r
        inc m_hist_hi,x
@r:     rts
hist_bin: .byte 0, 0, 0, 1, 2

; Sliding 60-sample window. A = queue (0/1 presses of A/B, 2/3 edges of A/B). Pushes the event
; frame (low byte), then returns A = entries with frames f-58 .. f.
window_push:
        sta mt_tmp
        lsr a                   ; offset = id * 64
        ror a
        ror a
        and #$C0
        sta mt_base
        ldx mt_tmp
        lda q_head,x            ; full (63 entries)? drop the oldest first
        clc
        adc #1
        and #63
        cmp q_tail,x
        bne @room
        lda q_tail,x
        clc
        adc #1
        and #63
        sta q_tail,x
@room:  lda q_head,x
        ora mt_base
        tay
        lda mt_flo
        sta qbuf,y
        lda q_head,x
        clc
        adc #1
        and #63
        sta q_head,x
        lda mt_flo
        jsr window_prune
        lda q_head,x
        sec
        sbc q_tail,x
        and #63
        rts

; Drops entries of queue X (base mt_base) that are 59 or more frames older than frame A.
window_prune:
        sta mt_tmp+1
@drop:  lda q_tail,x
        cmp q_head,x
        beq @r
        ora mt_base
        tay
        lda mt_tmp+1
        sec
        sbc qbuf,y
        cmp #59
        bcc @r
        lda q_tail,x
        clc
        adc #1
        and #63
        sta q_tail,x
        jmp @drop
@r:     rts

; Once per frame: prune all four queues against the current frame (no stale low bytes).
window_prune_all:
        ldx #3
@q:     stx mt_tmp
        txa
        lsr a
        ror a
        ror a
        and #$C0
        sta mt_base
        lda mp_frame
        jsr window_prune
        ldx mt_tmp
        dex
        bpl @q
        rts

; Ends the run: final rate, best records.
test_finish:
        lda #2
        sta mt_state
        lda #60
        sta mt_cool
        lda #40
        ldy #12
        jsr blip1
        ldx #1
@b:     txa
        asl a
        tay                     ; Y = 0 / 2: 16-bit record offset
        lda m_cnt_lo,x
        cmp best_a,y
        lda m_cnt_hi,x
        sbc best_a+1,y
        bcc @nob
        lda m_cnt_lo,x
        sta best_a,y
        lda m_cnt_hi,x
        sta best_a+1,y
@nob:   lda m_ecnt_lo,x
        cmp best_e,y
        lda m_ecnt_hi,x
        sbc best_e+1,y
        bcc @noe
        lda m_ecnt_lo,x
        sta best_e,y
        lda m_ecnt_hi,x
        sta best_e+1,y
@noe:   dex
        bpl @b
        rts

; Ends a running test once the window is over even without events; counts the cool-down.
test_clock:
        lda mt_state
        cmp #1
        bne @notrun
        lda mp_frame
        sec
        sbc mt_t0lo
        sta m_a
        lda mp_frame+1
        sbc mt_t0hi
        sta m_a+1
        lda m_a
        cmp #<WIN
        lda m_a+1
        sbc #>WIN
        bcc @r
        jmp test_finish
@notrun:
        cmp #2
        bne @r
        lda mt_cool
        beq @r
        dec mt_cool
@r:     rts

; ============================================================================ statistics
; Every frame both rates, plus one of 4 steps: (A, B) x (interval groups, hold / gap / duty).
meter_calc:
        ldx #0
        jsr calc_rate
        ldx #1
        jsr calc_rate
        lda mt_calc
        clc
        adc #1
        cmp #4
        bcc @ok
        lda #0
@ok:    sta mt_calc
        lsr a
        sta mt_x                ; button
        lda mt_calc
        and #1
        bne @odd
        ; even steps: both interval groups of the button
        lda #0
        jsr calc_group
        lda #rr_n_lo-pp_n_lo
        jmp calc_group
@odd:   jmp calc_holdgap

; Presses per second x10: count when the run is done, else count * 600 / samples so far.
calc_rate:
        stx mt_x
        lda m_cnt_lo,x
        sta m_a
        lda m_cnt_hi,x
        sta m_a+1
        lda mt_state
        cmp #2
        beq @done
        cmp #1
        bne @zero
        lda #0
        sta m_a+2
        sta m_a+3
        lda #<600
        sta m_b
        lda #>600
        sta m_b+1
        jsr mul32_16
        lda mp_frame            ; samples = frame - t0 + 2 (the one before the first press)
        sec
        sbc mt_t0lo
        sta m_b
        lda mp_frame+1
        sbc mt_t0hi
        sta m_b+1
        lda m_b
        clc
        adc #2
        sta m_b
        bcc @nc
        inc m_b+1
@nc:    lda m_b+1               ; never more than 600 samples
        cmp #>600
        bcc @div
        lda m_b
        cmp #<600
        bcc @div
        lda #<600
        sta m_b
        lda #>600
        sta m_b+1
@div:   jsr div32_16
@done:  ldx mt_x
        lda m_a
        sta r_rate_lo,x
        lda m_a+1
        sta r_rate_hi,x
        rts
@zero:  ldx mt_x
        lda #0
        sta r_rate_lo,x
        sta r_rate_hi,x
        rts

; Mean and deviation (x100 frames) of the interval group at offset A of button mt_x.
calc_group:
        clc
        adc mt_x
        sta mt_tmp
        tax
        lda pp_n_lo,x
        sta st_n
        lda pp_n_hi,x
        sta st_n+1
        lda pp_s1_lo,x
        sta st_s1
        lda pp_s1_hi,x
        sta st_s1+1
        lda pp_s2_0,x
        sta st_s2
        lda pp_s2_1,x
        sta st_s2+1
        lda pp_s2_2,x
        sta st_s2+2
        jsr mean_dev
        lda mt_tmp              ; results: r_ppavg (+0) or r_rravg (+8)
        sec
        sbc mt_x
        beq @pp
        lda #8
@pp:    clc
        adc mt_x
        tax
        lda st_avg
        sta r_ppavg_lo,x
        lda st_avg+1
        sta r_ppavg_hi,x
        lda st_sd
        sta r_ppsd_lo,x
        lda st_sd+1
        sta r_ppsd_hi,x
        rts

; st_n, st_s1, st_s2 -> st_avg = 100 * s1 / n, st_sd = 100 * sqrt(n * s2 - s1^2) / n
; (population deviation), both rounded; $FFFF when n = 0.
mean_dev:
        lda st_n
        ora st_n+1
        bne @have
        lda #$FF
        sta st_avg
        sta st_avg+1
        sta st_sd
        sta st_sd+1
        rts
@have:  ; mean
        lda st_s1
        sta m_a
        lda st_s1+1
        sta m_a+1
        lda #0
        sta m_a+2
        sta m_a+3
        lda #100
        sta m_b
        lda #0
        sta m_b+1
        jsr mul32_16
        jsr add_half_n
        jsr div_by_n
        lda m_a
        sta st_avg
        lda m_a+1
        sta st_avg+1
        ; N = n * s2 - s1^2
        lda st_s2
        sta m_a
        lda st_s2+1
        sta m_a+1
        lda st_s2+2
        sta m_a+2
        lda #0
        sta m_a+3
        lda st_n
        sta m_b
        lda st_n+1
        sta m_b+1
        jsr mul32_16
        ldx #3
@sv:    lda m_a,x
        sta m_t,x
        dex
        bpl @sv
        lda st_s1
        sta m_a
        lda st_s1+1
        sta m_a+1
        lda #0
        sta m_a+2
        sta m_a+3
        lda st_s1
        sta m_b
        lda st_s1+1
        sta m_b+1
        jsr mul32_16
        sec
        lda m_t
        sbc m_a
        sta m_a
        lda m_t+1
        sbc m_a+1
        sta m_a+1
        lda m_t+2
        sbc m_a+2
        sta m_a+2
        lda m_t+3
        sbc m_a+3
        sta m_a+3
        ; T = sqrt(100 N) when 100 N fits in 32 bits, else 10 sqrt(N)
        lda m_a+3               ; 100 N < 2^32 when N < $028F0000
        cmp #2
        bcc @small
        bne @big
        lda m_a+2
        cmp #$8F
        bcs @big
@small:
        lda #100
        sta m_b
        lda #0
        sta m_b+1
        jsr mul32_16
        jsr isqrt32
        lda m_r
        sta m_a
        lda m_r+1
        sta m_a+1
        jmp @t
@big:   jsr isqrt32
        lda m_r
        sta m_a
        lda m_r+1
        sta m_a+1
        lda #0
        sta m_a+2
        sta m_a+3
        lda #10
        sta m_b
        lda #0
        sta m_b+1
        jsr mul32_16
@t:     lda #0                  ; sd x100 = T * 10 / n
        sta m_a+2
        sta m_a+3
        lda #10
        sta m_b
        lda #0
        sta m_b+1
        jsr mul32_16
        jsr add_half_n
        jsr div_by_n
        lda m_a
        sta st_sd
        lda m_a+1
        sta st_sd+1
        rts

add_half_n:
        lda st_n+1
        lsr a
        sta m_b+1
        lda st_n
        ror a
        clc
        adc m_a
        sta m_a
        lda m_b+1
        adc m_a+1
        sta m_a+1
        bcc @r
        inc m_a+2
        bne @r
        inc m_a+3
@r:     rts

div_by_n:
        lda st_n
        sta m_b
        lda st_n+1
        sta m_b+1
        jmp div32_16

; Mean hold, mean gap (x100 frames) and duty = hold / (hold + gap) of button mt_x.
calc_holdgap:
        ldx mt_x
        lda hd_n_lo,x
        sta st_n
        lda hd_n_hi,x
        sta st_n+1
        lda hd_s1_lo,x
        sta st_s1
        lda hd_s1_hi,x
        sta st_s1+1
        jsr mean_only
        ldx mt_x
        lda st_avg
        sta r_hdavg_lo,x
        lda st_avg+1
        sta r_hdavg_hi,x
        lda gp_n_lo,x
        sta st_n
        lda gp_n_hi,x
        sta st_n+1
        lda gp_s1_lo,x
        sta st_s1
        lda gp_s1_hi,x
        sta st_s1+1
        jsr mean_only
        ldx mt_x
        lda st_avg
        sta r_gpavg_lo,x
        lda st_avg+1
        sta r_gpavg_hi,x
        ; duty
        lda #255
        sta r_duty,x
        lda r_hdavg_hi,x
        cmp #$FF
        beq @r
        lda r_gpavg_hi,x
        cmp #$FF
        beq @r
        lda r_hdavg_lo,x
        clc
        adc r_gpavg_lo,x
        sta m_b
        lda r_hdavg_hi,x
        adc r_gpavg_hi,x
        sta m_b+1
        ora m_b
        beq @r
        lda r_hdavg_lo,x
        sta m_a
        lda r_hdavg_hi,x
        sta m_a+1
        lda #0
        sta m_a+2
        sta m_a+3
        lda m_b                 ; m_a * 100 needs m_b: keep the sum in mt_tmp
        sta mt_tmp
        lda m_b+1
        sta mt_tmp+1
        lda #100
        sta m_b
        lda #0
        sta m_b+1
        jsr mul32_16
        lda mt_tmp              ; + sum / 2 (rounding)
        lsr mt_tmp+1
        ror a
        clc
        adc m_a
        sta m_a
        lda mt_tmp+1
        adc m_a+1
        sta m_a+1
        bcc @nc
        inc m_a+2
@nc:    ldx mt_x
        lda r_hdavg_lo,x
        clc
        adc r_gpavg_lo,x
        sta m_b
        lda r_hdavg_hi,x
        adc r_gpavg_hi,x
        sta m_b+1
        jsr div32_16
        ldx mt_x
        lda m_a
        sta r_duty,x
@r:     rts

mean_only:
        lda st_n
        ora st_n+1
        bne @have
        lda #$FF
        sta st_avg
        sta st_avg+1
        rts
@have:  lda st_s1
        sta m_a
        lda st_s1+1
        sta m_a+1
        lda #0
        sta m_a+2
        sta m_a+3
        lda #100
        sta m_b
        lda #0
        sta m_b+1
        jsr mul32_16
        jsr add_half_n
        jsr div_by_n
        lda m_a
        sta st_avg
        lda m_a+1
        sta st_avg+1
        rts

; ============================================================================ turbo check
turbo_event:
        ldx mt_x
        lda mt_flo
        sta t_lastev_lo,x
        lda mt_fhi
        sta t_lastev_hi,x
        lda mt_ev
        and #2
        beq @release
        ; press: gap since the release, period since the last press
        lda m_flags,x
        and #2
        beq @nogap
        jsr since_lastr
        ldx mt_x
        lda mt_iv
        sta t_gap,x
@nogap: ldx mt_x
        lda m_flags,x
        and #1
        beq @noper
        jsr since_lastp
        ldx mt_x
        lda mt_iv
        sta t_per,x
        jsr turbo_ring
        jsr turbo_duty
@noper: ldx mt_x
        lda mt_flo
        sta m_lastp_lo,x
        lda mt_fhi
        sta m_lastp_hi,x
        lda m_flags,x
        ora #1
        sta m_flags,x
        rts
@release:
        lda m_flags,x
        and #1
        beq @nohold
        jsr since_lastp
        ldx mt_x
        lda mt_iv
        sta t_hold,x
@nohold:
        ldx mt_x
        lda mt_flo
        sta m_lastr_lo,x
        lda mt_fhi
        sta m_lastr_hi,x
        lda m_flags,x
        ora #2
        sta m_flags,x
        rts

; Adds t_per to the 8-period ring of mt_x; t_min / t_max over the ring.
turbo_ring:
        ldx mt_x
        lda t_ridx,x
        sta mt_tmp
        clc
        adc #1
        and #7
        sta t_ridx,x
        txa
        asl a
        asl a
        asl a
        ora mt_tmp
        tay                     ; ring slot
        lda t_per,x
        sta t_ring,y
        lda t_rcnt,x
        cmp #8
        bcs @full
        inc t_rcnt,x
@full:  lda #255
        sta t_min,x
        lda #0
        sta t_max,x
        txa
        asl a
        asl a
        asl a
        tay
        lda t_rcnt,x
        sta mt_tmp
@m:     lda t_ring,y
        cmp t_min,x
        bcs @nmin
        sta t_min,x
@nmin:  cmp t_max,x
        bcc @nmax
        sta t_max,x
@nmax:  iny
        dec mt_tmp
        bne @m
        rts

; Duty of the last cycle = hold * 100 / period (hold is from the release inside that period).
turbo_duty:
        ldx mt_x
        lda #255
        sta t_duty,x
        lda m_flags,x
        and #2
        beq @r
        lda t_hold,x
        cmp t_per,x
        bcs @r
        jsr m_a_load8
        ldx mt_x
        lda #100
        sta m_b
        lda #0
        sta m_b+1
        jsr mul32_16
        ldx mt_x
        lda t_per,x
        lsr a
        clc
        adc m_a
        sta m_a
        bcc @nc
        inc m_a+1
@nc:    ldx mt_x
        lda t_per,x
        sta m_b
        lda #0
        sta m_b+1
        jsr div32_16
        ldx mt_x
        lda m_a
        sta t_duty,x
@r:     rts

; Traces: one column per frame for A (row 14) and B (row 17), with a cursor ahead.
turbo_trace:
        ldx #0
@b:     stx mt_x
        lda pad1
        and trace_mask,x
        beq @lo
        lda #1
@lo:    sta mt_tmp              ; level now
        lda w_prev,x
        asl a
        ora mt_tmp
        clc
        adc #T_WAVE_LL
        sta mt_tmp+1            ; tile
        lda mt_tmp
        sta w_prev,x
        lda trace_hi,x
        sta ptr0+1
        lda trace_lo,x
        clc
        adc w_pos
        tax
        lda ptr0+1
        ldy #1
        jsr vb_start
        lda mt_tmp+1
        sta vbuf,x
        ; cursor in the next column
        ldx mt_x
        ldy w_pos
        iny
        cpy #30
        bcc @cur
        ldy #0
@cur:   tya
        clc
        adc trace_lo,x
        tax
        lda ptr0+1
        ldy #1
        jsr vb_start
        lda #T_WAVE_CURSOR
        sta vbuf,x
        ldx mt_x
        inx
        cpx #2
        bne @b
        ldy w_pos
        iny
        cpy #30
        bcc @np
        ldy #0
@np:    sty w_pos
        rts
trace_mask: .byte BTN_A, BTN_B
trace_hi:   .byte $21, $22      ; row 14 / row 17, column 1
trace_lo:   .byte $C1, $21

; ============================================================================ display
; Items: rows of values (both buttons), the time bar, histogram rows. ITEMS in total.
ITEMS = 19                      ; 10-second test: 14 rows, time bar, 4 histogram rows
ITEMS_TURBO = 6

meter_item:
        lda m_mode
        beq @test
        lda mt_item             ; turbo: rows 5-10 = items 0-5, the rest nothing
        cmp #6
        bcs @r
        jmp turbo_row
@test:  lda mt_item
        cmp #14
        bcs @not_row
        jmp test_row
@not_row:
        cmp #14
        bne @hist
        jmp time_bar
@hist:  cmp #19
        bcs @r
        jmp hist_row
@r:     rts

; Value row mt_item (0-13) of the 10-second test: row 5 + item, A then B.
test_row:
        lda #0
        sta mt_x
@b:     jsr blank8
        ldy #0
        jsr @call
        jsr queue_field
        inc mt_x
        lda mt_x
        cmp #2
        bne @b
        rts
@call:  lda mt_item
        asl a
        tax
        lda test_fmt+1,x
        pha
        lda test_fmt,x
        pha
        rts

; Queues txt[0..7], right-aligned, at row 5 + mt_item (or turbo row), column A / B.
queue_field:
        ldx #7
@f:     lda txt,x
        cmp #' '
        bne @found
        dex
        bpl @f
        bmi @q
@found: stx tmp6
        lda #7
        sec
        sbc tmp6
        beq @q
        sta tmp5                ; shift right by 7 - last
@mv:    txa
        clc
        adc tmp5
        tay
        lda txt,x
        sta txt,y
        dex
        bpl @mv
        ldx tmp5
        dex
        lda #' '
@sp:    sta txt,x
        dex
        bpl @sp
@q:     lda mt_item
        clc
        adc #5
        jsr row_addr
        pha
        txa
        ldx mt_x
        ora col_ab,x
        tax
        pla
        ldy #8
        jmp vb_txt
col_ab: .byte COL_A, COL_B

blank8:
        lda #' '
        ldx #7
@b:     sta txt,x
        dex
        bpl @b
        rts

; Formatters write txt from Y = 0 for button mt_x.
test_fmt:
        .word f_presses-1, f_rate-1, f_edges-1, f_best1-1, f_ppavg-1, f_ppsd-1, f_rravg-1
        .word f_rrsd-1, f_hold-1, f_gap-1, f_duty-1, f_last-1, f_fast-1, f_best10-1

f_presses:
        ldx mt_x
        lda m_cnt_lo,x
        sta m_a
        lda m_cnt_hi,x
        sta m_a+1
        jmp fmt_u16_min
f_rate: ldx mt_x
        lda r_rate_lo,x
        sta m_a
        lda r_rate_hi,x
        sta m_a+1
        lda #3
        sta tmp4
        jsr fmt_fix1
        rts
f_edges:
        ldx mt_x
        lda m_ecnt_lo,x
        sta m_a
        lda m_ecnt_hi,x
        sta m_a+1
        jmp fmt_u16_min
f_best1:
        ldx mt_x
        lda m_peakp,x
        jsr fmt_dec_min
        lda #'/'
        sta txt,y
        iny
        ldx mt_x
        lda m_peake,x
        jsr fmt_dec_min
        rts
f_ppavg:
        ldx mt_x
        lda r_ppavg_lo,x
        sta m_a
        lda r_ppavg_hi,x
        jmp f_x100
f_ppsd: ldx mt_x
        lda r_ppsd_lo,x
        sta m_a
        lda r_ppsd_hi,x
        jmp f_x100
f_rravg:
        ldx mt_x
        lda r_rravg_lo,x
        sta m_a
        lda r_rravg_hi,x
        jmp f_x100
f_rrsd: ldx mt_x
        lda r_rrsd_lo,x
        sta m_a
        lda r_rrsd_hi,x
        jmp f_x100
f_hold: ldx mt_x
        lda r_hdavg_lo,x
        sta m_a
        lda r_hdavg_hi,x
        jmp f_x100
f_gap:  ldx mt_x
        lda r_gpavg_lo,x
        sta m_a
        lda r_gpavg_hi,x
f_x100: sta m_a+1               ; "dd.ddF", or " -" when there is no data ($FFFF)
        and m_a
        cmp #$FF
        beq f_none
        lda #2
        sta tmp4
        jsr fmt_fix2
        lda #'F'
        sta txt,y
        rts
f_none: lda #'-'
        sta txt+3
        rts
f_duty: ldx mt_x
        lda r_duty,x
        cmp #255
        beq f_none
        jsr fmt_dec3
        lda #'%'
        sta txt,y
        rts
f_last: ldx mt_x
        lda m_lhold,x
        cmp #255
        beq f_none
        lda m_lgap,x
        cmp #255
        beq f_none
        lda m_lhold,x
        jsr fmt_dec_min
        lda #'/'
        sta txt,y
        iny
        ldx mt_x
        lda m_lgap,x
        jsr fmt_dec_min
        lda #'F'
        sta txt,y
        rts
f_fast: ldx mt_x
        lda m_minpp,x
        cmp #255
        beq f_none
        jsr fmt_dec3
        lda #'F'
        sta txt,y
        rts
f_best10:
        lda mt_x
        asl a
        tax
        lda best_a,x            ; presses per second x10 = presses in 10 s
        sta m_a
        lda best_a+1,x
        sta m_a+1
        stx mt_tmp
        lda #2
        sta tmp4
        jsr fmt_fix1
        lda #'/'
        sta txt,y
        iny
        ldx mt_tmp
        lda best_e,x
        sta m_a
        lda best_e+1,x
        sta m_a+1
        jmp fmt_u16_min

; m_a (16 bit) with as many digits as it needs.
fmt_u16_min:
        ldx #4
@d:     lda m_a+1
        cmp pow10_hi,x
        bcc @less
        bne @got
        lda m_a
        cmp pow10_lo,x
        bcs @got
@less:  dex
        bne @d
@got:   inx
        stx tmp4
        jmp fmt_dec16
pow10_lo: .byte <1, <10, <100, <1000, <10000
pow10_hi: .byte >1, >10, >100, >1000, >10000

; A as 1-3 digits without padding.
fmt_dec_min:
        cmp #10
        bcc @one
        cmp #100
        bcc @two
        sta m_a
        lda #0
        sta m_a+1
        lda #3
        bne @n
@two:   sta m_a
        lda #0
        sta m_a+1
        lda #2
        bne @n
@one:   sta m_a
        lda #0
        sta m_a+1
        lda #1
@n:     sta tmp4
        jmp fmt_dec16

; x100 value in m_a as "d.dd" with tmp4 integer digits.
fmt_fix2:
        lda #100
        jsr div16_8
        pha
        jsr fmt_dec16
        lda #'.'
        sta txt,y
        iny
        pla
        ldx #0
@t:     cmp #10
        bcc @o
        sbc #10
        inx
        bne @t
@o:     pha
        txa
        ora #'0'
        sta txt,y
        iny
        pla
        ora #'0'
        sta txt,y
        iny
        rts

; Remaining time "dd.dd" (row 20, column 6).
meter_time:
        lda mt_state
        beq @full
        cmp #2
        beq @zero
        lda mt_t0lo             ; remaining = t0 + WIN - frame (frames)
        clc
        adc #<WIN
        sta m_a
        lda mt_t0hi
        adc #>WIN
        sta m_a+1
        lda m_a
        sec
        sbc mp_frame
        sta m_a
        lda m_a+1
        sbc mp_frame+1
        sta m_a+1
        bcs @calc
@zero:  lda #0
        sta m_a
        sta m_a+1
        beq @calc
@full:  lda #<600
        sta m_a
        lda #>600
        sta m_a+1
@calc:  lda m_a                 ; centiseconds = frames * 5 / 3
        sta mt_tmp
        lda m_a+1
        sta mt_tmp+1
        asl m_a
        rol m_a+1
        asl m_a
        rol m_a+1
        lda m_a
        clc
        adc mt_tmp
        sta m_a
        lda m_a+1
        adc mt_tmp+1
        sta m_a+1
        lda #3
        jsr div16_8
        lda #2
        sta tmp4
        ldy #0
        jsr fmt_fix2
        lda #$22
        ldx #$86
        ldy #5
        jmp vb_txt

; Progress bar: row 20, columns 13-28 (128 px for 600 frames).
time_bar:
        lda mt_state
        beq @none
        cmp #2
        beq @all
        lda mp_frame
        sec
        sbc mt_t0lo
        sta m_a
        lda mp_frame+1
        sbc mt_t0hi
        sta m_a+1
        ldx #5                  ; * 32
@x32:   asl m_a
        rol m_a+1
        dex
        bne @x32
        lda #150
        jsr div16_8
        lda m_a+1
        bne @all
        lda m_a
        cmp #129
        bcc @px
@all:   lda #128
        bne @px
@none:  lda #0
@px:    sta mt_tmp
        ldy #0
@t:     lda mt_tmp
        cmp #8
        bcc @part
        sbc #8
        sta mt_tmp
        lda #T_BAR8
        bne @put
@part:  clc
        adc #T_BAR0
        pha
        lda #0
        sta mt_tmp
        pla
@put:   sta txt,y
        iny
        cpy #16
        bne @t
        lda #$22
        ldx #$8D
        ldy #16
        jmp vb_txt

; Histogram row (item 15-18 -> bin 0-3, screen rows 22-25): bars scaled to the largest bin.
hist_row:
        sec
        sbc #15
        sta mt_tmp+1            ; bin
        ; largest bin of both buttons
        lda #0
        sta st_n
        sta st_n+1
        ldx #7
@max:   lda m_hist_lo,x
        cmp st_n
        lda m_hist_hi,x
        sbc st_n+1
        bcc @nm
        lda m_hist_lo,x
        sta st_n
        lda m_hist_hi,x
        sta st_n+1
@nm:    dex
        bpl @max
        ldx #0
@b:     stx mt_x
        lda mt_tmp+1
        cpx #0
        beq @a
        clc
        adc #4
@a:     tax
        lda m_hist_lo,x         ; px = bin * 64 / max
        sta m_a
        lda m_hist_hi,x
        sta m_a+1
        lda #0
        sta m_a+2
        sta m_a+3
        ldx #6
@x64:   asl m_a
        rol m_a+1
        rol m_a+2
        dex
        bne @x64
        lda st_n
        ora st_n+1
        beq @empty
        lda st_n
        sta m_b
        lda st_n+1
        sta m_b+1
        jsr div32_16
        lda m_a
        jmp @draw
@empty: lda #0
@draw:  sta mt_tmp
        ldy #0
@t:     lda mt_tmp
        cmp #8
        bcc @part
        sbc #8
        sta mt_tmp
        lda #T_BAR8
        bne @put
@part:  clc
        adc #T_BAR0
        pha
        lda #0
        sta mt_tmp
        pla
@put:   sta txt,y
        iny
        cpy #8
        bne @t
        lda mt_tmp+1
        clc
        adc #22
        jsr row_addr
        pha
        txa
        ldx mt_x
        ora col_ab,x
        tax
        pla
        ldy #8
        jsr vb_txt
        ldx mt_x
        inx
        cpx #2
        bne @b
        rts

; Turbo rows (item 0-5 -> rows 5-10).
turbo_row:
        lda #0
        sta mt_x
@b:     jsr blank8
        ldy #0
        jsr @call
        jsr queue_field
        inc mt_x
        lda mt_x
        cmp #2
        bne @b
        rts
@call:  lda mt_item
        asl a
        tax
        lda turbo_fmt+1,x
        pha
        lda turbo_fmt,x
        pha
        rts

turbo_fmt:
        .word t_state-1, t_period-1, t_rate-1, t_holdgap-1, t_dutyf-1, t_range-1

t_state:
        ldx mt_x
        lda mp_frame            ; frames since the last edge
        sec
        sbc t_lastev_lo,x
        sta m_a
        lda mp_frame+1
        sbc t_lastev_hi,x
        bne @still
        lda m_a
        cmp #60
        bcs @still
        lda m_flags,x
        and #1
        beq @idle
        lda #<str_turbo
        ldx #>str_turbo
        jmp @put
@still: lda mp_state
        ldx mt_x
        and trace_mask,x
        beq @idle
        lda #<str_held
        ldx #>str_held
        jmp @put
@idle:  lda #<str_idle
        ldx #>str_idle
@put:   sta ptr0
        stx ptr0+1
        ldy #0
        jsr fmt_str
        rts
str_turbo: .byte "Turbo", 0
str_held:  .byte "Held", 0
str_idle:  .byte "Idle", 0

t_period:
        ldx mt_x
        lda t_per,x
        cmp #255
        beq t_none
        jsr fmt_dec3
        lda #'F'
        sta txt,y
        rts
t_none: lda #'-'
        sta txt+2
        rts
t_rate: ldx mt_x                ; presses per second x10 = 600 / period
        lda t_per,x
        cmp #255
        beq t_none
        cmp #0
        beq t_none
        sta m_b
        lda #0
        sta m_b+1
        lda #<600
        sta m_a
        lda #>600
        sta m_a+1
        lda #0
        sta m_a+2
        sta m_a+3
        jsr div32_16
        lda #3
        sta tmp4
        ldy #0
        jsr fmt_fix1
        rts
t_holdgap:
        ldx mt_x
        lda t_per,x
        cmp #255
        beq t_none
        lda t_hold,x
        jsr fmt_dec3
        lda #'/'
        sta txt,y
        iny
        ldx mt_x
        lda t_gap,x
        jsr fmt_dec_min
        lda #'F'
        sta txt,y
        rts
t_dutyf:
        ldx mt_x
        lda t_duty,x
        cmp #255
        beq t_none
        jsr fmt_dec3
        lda #'%'
        sta txt,y
        rts
t_range:
        ldx mt_x
        lda t_rcnt,x
        beq t_none
        lda t_min,x
        jsr fmt_dec3
        lda #'-'
        sta txt,y
        iny
        ldx mt_x
        lda t_max,x
        jsr fmt_dec_min
        lda #'F'
        sta txt,y
        rts

; Status line (row 27) when the state changes.
meter_status:
        lda m_mode
        beq @test
        lda #3
        bne @have
@test:  lda mt_state
        cmp #2
        bne @have
        lda mt_cool
        beq @have               ; 2 = done, may restart
        lda #4                  ; done, cooling down
@have:  cmp mt_shown
        beq @r
        sta mt_shown
        tax
        lda status_lo,x
        sta ptr0
        lda status_hi,x
        sta ptr0+1
        lda #$23
        ldx #$62
        jmp vb_string
@r:     rts
status_lo: .byte <st0, <st1, <st2, <st3, <st4
status_hi: .byte >st0, >st1, >st2, >st3, >st4
st0: .byte "Press A or B to start    ", 0
st1: .byte "Measuring...             ", 0
st2: .byte "Done. Press A/B to retry ", 0
st3: .byte "Tap or hold a turbo button", 0
st4: .byte "Done.                    ", 0

meter_text:
        .byte $20, $21, "Rapid-fire meter  10 s test", 0          ; row 1
        .byte $20, $41, "600 samples: max 599 edges", 0            ; row 2
        .byte $20, $61, "4 polls/frame; emulator 1/60 s", 0        ; row 3
        .byte $20, $94, "A", 0                                     ; row 4
        .byte $20, $9D, "B", 0
        .byte $20, $A1, "Presses", 0                               ; row 5
        .byte $20, $C1, "Presses/s", 0
        .byte $20, $E1, "Edges", 0
        .byte $21, $01, "Best 1s P/E", 0
        .byte $21, $21, "Press int.", 0
        .byte $21, $41, " deviation", 0
        .byte $21, $61, "Release int", 0
        .byte $21, $81, " deviation", 0
        .byte $21, $A1, "Hold avg", 0
        .byte $21, $C1, "Gap avg", 0
        .byte $21, $E1, "Duty", 0
        .byte $22, $01, "Last h/g", 0
        .byte $22, $21, "Fastest int", 0
        .byte $22, $41, "Best 10 s", 0                             ; row 18
        .byte $22, $81, "Time", 0                                  ; row 20
        .byte $22, $A1, "Press interval", 0                        ; row 21
        .byte $22, $C1, "2F  30/s", 0                              ; rows 22-25
        .byte $22, $E1, "3F  20/s", 0
        .byte $23, $01, "4F  15/s", 0
        .byte $23, $21, "5F+ <=12", 0
        .byte $23, $81, "Left/Right: turbo  Down: reset", 0        ; row 28
        .byte $FF

turbo_text:
        .byte $20, $21, "Rapid-fire meter  turbo check", 0        ; row 1
        .byte $20, $41, "Period = press to press", 0               ; row 2
        .byte $20, $61, "4 polls/frame; emulator 1/60 s", 0        ; row 3
        .byte $20, $94, "A", 0                                     ; row 4
        .byte $20, $9D, "B", 0
        .byte $20, $A1, "State", 0                                 ; row 5
        .byte $20, $C1, "Period", 0
        .byte $20, $E1, "Presses/s", 0
        .byte $21, $01, "Hold/gap", 0
        .byte $21, $21, "Duty", 0
        .byte $21, $41, "Range (8)", 0                             ; row 10
        .byte $21, $A1, "A", 0                                     ; row 13
        .byte $22, $01, "B", 0                                     ; row 16
        .byte $22, $61, "1 column = 1 frame (1/60 s)", 0           ; row 19
        .byte $23, $81, "Left/Right: 10 s test", 0                 ; row 28
        .byte $FF

meter_pal:      ; 0 labels (grey), 1 values (white) + A bars, 2 B bars, 3 time bar
        .byte $0F, $10, $00, $00,  $0F, $30, $10, $16,  $0F, $30, $10, $12,  $0F, $30, $10, $2A
        .byte $0F, $30, $10, $00,  $0F, $30, $10, $00,  $0F, $30, $10, $00,  $0F, $30, $10, $00

; Rows 0-3 white; rows 4-19 labels (columns 0-11) grey, values white; row 20 time digits white,
; bar (columns 12-29) palette 3; rows 22-25 A bars palette 1, B bars palette 2; rows 26-29 white.
meter_attr:
        .byte $55, $55, $55, $55, $55, $55, $55, $55
        .byte $00, $00, $00, $55, $55, $55, $55, $55
        .byte $00, $00, $00, $55, $55, $55, $55, $55
        .byte $00, $00, $00, $55, $55, $55, $55, $55
        .byte $00, $00, $00, $55, $55, $55, $55, $55
        .byte $00, $04, $05, $5F, $5F, $9F, $AF, $23
        .byte $50, $50, $50, $55, $55, $59, $5A, $52
        .byte $55, $55, $55, $55, $55, $55, $55, $55
turbo_attr:
        .byte $55, $55, $55, $55, $55, $55, $55, $55
        .byte $00, $00, $00, $55, $55, $55, $55, $55
        .byte $00, $00, $00, $55, $55, $55, $55, $55
        .res 40, $55
