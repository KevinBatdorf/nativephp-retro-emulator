; SameBoy's DMG bootstrap ROM (Expat license, see LICENSE-sameboy) with the
; logo scroll, chime and wait removed: identical post-boot state — VRAM logo
; tiles, tilemap, palette, audio registers, CPU registers — handed off on the
; first frame. Built with rgbds against sameboy/BootROMs includes; the built
; binary is vendored next to this file.

include "sameboot.inc"

SECTION "BootCode", ROM0[$0000]
Start:
    ld sp, $FFFE

; Clear VRAM
    ld hl, _VRAM
    xor a
.clearVRAMLoop
    ldi [hl], a
    bit 5, h
    jr z, .clearVRAMLoop

; Post-boot audio register state; no note is ever triggered
    ld a, AUDENA_ON
    ldh [rNR52], a
    ldh [rNR11], a
    ld a, $F3
    ldh [rNR12], a
    ldh [rNR51], a
    ld a, $77
    ldh [rNR50], a

; Final BG palette (the real boot ends here after its fade)
    ld a, %11_11_11_00
    ldh [rBGP], a

; Load logo from the cartridge header into VRAM, as the real boot leaves it
    ld de, NintendoLogo
    ld hl, _VRAM + $10
.loadLogoLoop
    ld a, [de]
    ld b, a
    call DoubleBitsAndWriteRow
    call DoubleBitsAndWriteRow
    inc de
    ld a, e
    xor LOW(NintendoLogoEnd)
    jr nz, .loadLogoLoop

; Trademark symbol
    ld de, TrademarkSymbol
    ld c, TrademarkSymbolEnd - TrademarkSymbol
.loadTrademarkSymbolLoop:
    ld a, [de]
    inc de
    ldi [hl], a
    inc hl
    dec c
    jr nz, .loadTrademarkSymbolLoop

; Tilemap
    ld a, $19                           ; Trademark symbol tile ID
    ld [_SCRN0 + 8 * SCRN_VX_B + 16], a
    ld hl, _SCRN0 + 9 * SCRN_VX_B + 15
    ld c, 12
.tilemapLoop
    dec a
    jr z, .tilemapDone
    ldd [hl], a
    dec c
    jr nz, .tilemapLoop
    ld l, $0F
    jr .tilemapLoop
.tilemapDone

; Logo at its final resting position, LCD on
    xor a
    ldh [rSCY], a
    ld a, LCDCF_ON | LCDCF_BLK01 | LCDCF_BGON
    ldh [rLCDC], a

; Set registers to match the original DMG boot
    lb hl, BOOTUP_A_DMG, %10110000
    push hl
    pop af
    ld hl, HeaderChecksum
    lb bc, 0, LOW(rNR13) ; $0013
    lb de, 0, $D8        ; $00D8

    jp BootGame

DoubleBitsAndWriteRow:
; Double the most significant 4 bits, b is shifted by 4
    ld a, 4
    ld c, 0
.doubleCurrentBit
    sla b
    push af
    rl c
    pop af
    rl c
    dec a
    jr nz, .doubleCurrentBit
    ld a, c
; Write as two rows
    ldi [hl], a
    inc hl
    ldi [hl], a
    inc hl
    ret

TrademarkSymbol:
    pusho
    opt b.X
    db %..XXXX..
    db %.X....X.
    db %X.XXX..X
    db %X.X..X.X
    db %X.XXX..X
    db %X.X..X.X
    db %.X....X.
    db %..XXXX..
    popo
TrademarkSymbolEnd:

SECTION "BootGame", ROM0[$00FE]
BootGame:
    ldh [rBANK], a ; unmap boot ROM
