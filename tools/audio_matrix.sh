#!/usr/bin/env bash
# Render a GB/GBC ROM's audio under every combination of the plugin's audio
# options, on this Mac, in seconds — no device, no deploy. Listen to the WAVs
# and the winner becomes the default.
#
#   tools/audio_matrix.sh <rom> [seconds] [outdir]
#
# Each WAV is named for the settings that produced it:
#   hp1-dc0-fade0.wav = SameBoy's own default mode, no extra DC blocker, no fade
set -euo pipefail

ROM="${1:?usage: tools/audio_matrix.sh <rom> [seconds] [outdir]}"
SECONDS_LEN="${2:-20}"
OUTDIR="${3:-build/audio-matrix}"

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

BIN="build/audio_matrix"
mkdir -p "$(dirname "$BIN")" "$OUTDIR"

# SameBoy's Makefile filters these four out when the debugger is disabled.
CORE=$(/bin/ls sameboy/Core/*.c | grep -v -e debugger.c -e sm83_disassembler.c \
                                        -e symbol_hash.c -e cheat_search.c)

echo "compiling $BIN …"
clang -O2 -std=gnu11 -D_GNU_SOURCE -DGB_INTERNAL -DGB_DISABLE_DEBUGGER \
      -DGB_DISABLE_CHEAT_SEARCH -DGB_VERSION='"1.0.3"' \
      -I sameboy -o "$BIN" tools/audio_matrix.c $CORE

# Names describe the sound, not the flags. Two are reference points:
#   stock-sameboy = exactly what the SameBoy Mac app plays
#   ours-today    = exactly what the plugin ships right now
n=0
for hp in 0 1 2; do
  case $hp in
    0) mode="raw-dc-offset" ;;
    1) mode="hardware-filter" ;;
    2) mode="dc-removed" ;;
  esac
  for dc in 0 1; do
    for fade in 0 200; do
      n=$((n + 1))
      name="$mode"
      [ "$dc" = 1 ] && name="$name-plus-blocker"
      [ "$fade" = 200 ] && name="$name-plus-fadein"
      [ "$hp$dc$fade" = "100" ] && name="$name-STOCK-SAMEBOY"
      [ "$hp$dc$fade" = "21200" ] && name="$name-OURS-TODAY"
      "$BIN" "$ROM" "$OUTDIR/$(printf '%02d' $n)-$name.wav" \
             "$SECONDS_LEN" "$hp" "$dc" "$fade"
    done
  done
done

cat > "$OUTDIR/README.txt" <<'EOF'
The same passage, rendered under every audio setting. Filenames say what each
one is:

  raw-dc-offset    the chip's output untouched (largest DC offset)
  hardware-filter  models the real Game Boy's RC filter — SameBoy's own default
  dc-removed       DC tracked from the sound registers and subtracted

  plus-blocker     an extra DC blocker layered on top (the plugin's own addition)
  plus-fadein      200 ms fade-in when audio starts

Two files are the reference points:
  ..-STOCK-SAMEBOY  what the SameBoy Mac app plays
  ..-OURS-TODAY     what the plugin ships right now

Pick the one that sounds best; it becomes the default and the rest get deleted.
EOF

echo
echo "rendered $(/bin/ls "$OUTDIR"/*.wav | wc -l | tr -d ' ') files in $OUTDIR"
echo "open $OUTDIR to listen — README.txt explains the names"
