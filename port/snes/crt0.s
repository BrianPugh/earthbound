; crt0.s -- 65816 C runtime startup for SNES port
;
; This is scaffolding.  The exact syntax depends on the chosen assembler
; (ca65 for vbcc, or the compiler's built-in assembler for Calypsi).
; The logic below is written in ca65 syntax for reference.

.include "vectors.s"

.segment "BANK00"

; Reset vector entry point
RESET:
    sei                     ; Disable interrupts
    clc
    xce                     ; Switch to 65816 native mode

    ; Set up stack at $1FFF (top of low RAM)
    rep #$30                ; 16-bit A, X, Y
    lda #$1FFF
    tcs                     ; Transfer A to stack pointer

    ; Set direct page to $0000
    lda #$0000
    tcd                     ; Transfer A to direct page register

    ; Force blank during initialization
    sep #$20                ; 8-bit A
    lda #$80
    sta $2100               ; INIDISP = force blank

    ; Clear WRAM (optional but recommended)
    ; TODO: DMA fill $7E0000-$7FFFFF with zeros

    ; Initialize hardware registers to known state
    ; TODO: Clear all PPU registers ($2101-$2133)
    ; TODO: Clear all CPU I/O registers ($4200-$420D)

    ; Jump to C main()
    ; The exact calling convention depends on the compiler.
    ; For vbcc:  jsl _main
    ; For Calypsi: jsl main
    jsl _main

    ; If main() returns (shouldn't happen), loop forever
@HALT:
    wai
    bra @HALT
