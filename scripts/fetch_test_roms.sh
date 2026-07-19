#!/usr/bin/env bash
# Fetch freely-licensed homebrew test ROMs for the Phase 11 multi-system verify.
# Commercial ROMs must never enter this repo; these are all open/homebrew:
#
#   nestest.nes       — Kevin Horton's NES CPU test ROM (public domain)
#   dmg-acid2.gb      — Matt Currie's Game Boy PPU test (MIT)
#   helloworld.sfc    — krom (Peter Lemon) SNES HelloWorld (public domain)
#   helloworld.md     — SGDK hello-world sample (MIT)
#   cherilperils.sg   — Mojon Twins "Cheril Perils Classic" SG-1000 (LGPL-3.0)
#   helloworld.pce    — krom (Peter Lemon) PC Engine HelloWorld (public domain)
#   spritepriority.ws — Robert Peip's WonderSwan sprite test (GPL-2.0)
#   sprite.sms        — krom (Peter Lemon) Master System sprite test (public domain)
#   gba-shades.gba    — jsmolka gba-tests PPU shades (MIT) — needs a dev BIOS to boot
#   cgb-acid2.gbc     — Matt Currie's Game Boy Color PPU test (MIT)
#   pipes.a26         — albf's Pipes 2600 homebrew (MIT)
set -euo pipefail

DEST="$(cd "$(dirname "$0")/.." && pwd)/tests/roms"
mkdir -p "$DEST"

fetch() {
    local file="$1" url="$2"
    if [[ -f "$DEST/$file" ]]; then
        echo "✓ $file (cached)"
    else
        curl -sfL -o "$DEST/$file" "$url"
        echo "↓ $file"
    fi
}

fetch nestest.nes       "https://github.com/christopherpow/nes-test-roms/raw/master/other/nestest.nes"
fetch dmg-acid2.gb      "https://github.com/mattcurrie/dmg-acid2/releases/download/v1.0/dmg-acid2.gb"
fetch helloworld.sfc    "https://raw.githubusercontent.com/PeterLemon/SNES/master/HelloWorld/HelloWorld.sfc"
fetch helloworld.md     "https://raw.githubusercontent.com/Stephane-D/SGDK/master/sample/basics/hello-world/out/release/rom.bin"
fetch cherilperils.sg   "https://raw.githubusercontent.com/mojontwins/loves_the_sg1000/master/examples/cheril_perils_ntsc.sg"
fetch helloworld.pce    "https://raw.githubusercontent.com/PeterLemon/PCE/master/VDC/HelloWorld/HelloWorld.pce"
fetch spritepriority.ws "https://raw.githubusercontent.com/MiSTer-devel/WonderSwan_MiSTer/main/testroms/spritepriority/spritepriority.ws"
fetch sprite.sms        "https://raw.githubusercontent.com/PeterLemon/SMS/master/VDP/Sprite/Sprite.sms"
fetch gba-shades.gba    "https://github.com/jsmolka/gba-tests/raw/master/ppu/shades.gba"
fetch cgb-acid2.gbc     "https://github.com/mattcurrie/cgb-acid2/releases/download/v1.1/cgb-acid2.gbc"
fetch pipes.a26         "https://raw.githubusercontent.com/albf/pipes-2600/master/pipe2600.bin"

ls -la "$DEST"
