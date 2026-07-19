#!/usr/bin/env bash
# Fetch freely-licensed homebrew test ROMs for the Phase 11 multi-system verify.
# Commercial ROMs must never enter this repo; these are all open/homebrew:
#
#   nestest.nes       — Kevin Horton's NES CPU test ROM (public domain)
#   dmg-acid2.gb      — Matt Currie's Game Boy PPU test (MIT)
#   helloworld.sfc    — krom (Peter Lemon) SNES HelloWorld (public domain)
#   helloworld.md     — SGDK hello-world sample (MIT)
#   gba-shades.gba    — jsmolka gba-tests PPU shades (MIT) — needs a dev BIOS to boot
#   cgb-acid2.gbc     — Matt Currie's Game Boy Color PPU test (MIT)
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
fetch gba-shades.gba    "https://github.com/jsmolka/gba-tests/raw/master/ppu/shades.gba"
fetch cgb-acid2.gbc     "https://github.com/mattcurrie/cgb-acid2/releases/download/v1.1/cgb-acid2.gbc"

ls -la "$DEST"
