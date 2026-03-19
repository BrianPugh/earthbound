; vectors.s -- 65816 interrupt vectors for SNES port
;
; Scaffolding.  Written in ca65 syntax for reference.

.segment "BANK00"

; VBlank NMI handler
; Called every frame (~16.6ms on NTSC) when VBlank begins.
; This is the window for VRAM/OAM/CGRAM DMA transfers.
NMI_HANDLER:
    ; TODO: Save registers (PHA, PHX, PHY, PHD, PHB)
    ; TODO: Set vblank_flag = 1 (for platform_timer_frame_end to detect)
    ; TODO: Optionally perform DMA transfers here
    ; TODO: Increment NMI frame counter
    ; TODO: Restore registers
    rti

; IRQ handler (active/BRK)
IRQ_HANDLER:
    rti

; Unused vectors point here
EMPTY_HANDLER:
    rti

; ============================================================
; SNES native mode vectors at $00:FFE0-$00:FFFF
; ============================================================
.segment "HEADER"

; Native mode vectors ($FFE0-$FFEF)
;   $FFE0: unused
;   $FFE2: unused
;   $FFE4: COP
;   $FFE6: BRK
;   $FFE8: ABORT
;   $FFEA: NMI (VBlank)
;   $FFEC: unused
;   $FFEE: IRQ

; Emulation mode vectors ($FFF0-$FFFF)
;   $FFF0: unused
;   $FFF2: unused
;   $FFF4: COP
;   $FFF6: unused
;   $FFF8: ABORT
;   $FFFA: NMI
;   $FFFC: RESET
;   $FFFE: IRQ/BRK

; Note: The actual vector table placement depends on the linker config.
; The HEADER segment in snes_port.cfg must place these at $00FFC0+.
; Below is the logical layout; exact .WORD/.ADDR directives depend on
; the assembler and linker used.
;
; .org $FFE0
; .word EMPTY_HANDLER    ; $FFE0 - unused
; .word EMPTY_HANDLER    ; $FFE2 - unused
; .word EMPTY_HANDLER    ; $FFE4 - COP
; .word EMPTY_HANDLER    ; $FFE6 - BRK
; .word EMPTY_HANDLER    ; $FFE8 - ABORT
; .word NMI_HANDLER      ; $FFEA - NMI (VBlank)
; .word EMPTY_HANDLER    ; $FFEC - unused
; .word IRQ_HANDLER      ; $FFEE - IRQ
;
; .word EMPTY_HANDLER    ; $FFF0 - unused
; .word EMPTY_HANDLER    ; $FFF2 - unused
; .word EMPTY_HANDLER    ; $FFF4 - COP (emulation)
; .word EMPTY_HANDLER    ; $FFF6 - unused
; .word EMPTY_HANDLER    ; $FFF8 - ABORT (emulation)
; .word NMI_HANDLER      ; $FFFA - NMI (emulation)
; .word RESET            ; $FFFC - RESET
; .word IRQ_HANDLER      ; $FFFE - IRQ/BRK (emulation)
