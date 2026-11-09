#!/bin/sh
# Cross-build the Phase 0 SFX de-risk spike (tests/amigaos/ahi_sfx_test.c).
#
# Standalone AHI client: reads the real audio/sfx.SDT+sfx.RAW (byteswapped to
# native), proves the one-shot + looped AHI tag paths, checks 43-channel /
# 128-sound / 22050 Hz allocation, runs many simultaneous loops, and times
# fseek+fread+byteswap for small/median/max samples.
#
# Output: build/amigaos/test/ahi_sfx_test   (mounted as DH11: on B1260)
# Run from the game drawer (holds Audio/) so the log lands there:
#   CD DH12: && DH11:ahi_sfx_test   -> writes DH12:ahi_sfx_test.log
#
# Uses a native m68k-amigaos-gcc if present, else the amigadev/m68k-amigaos-gcc
# docker image via flatpak-spawn (same pattern as the other build scripts).
set -e

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${OUT:-$ROOT/build/amigaos/test}
SRC=$ROOT/tests/amigaos/ahi_sfx_test.c
FLAGS="-m68040 -mhard-float -mcrt=nix20 -O2"

mkdir -p "$OUT"

# AHI headers come from the Bebbo NDK (built-in), like the game build.
CMD="m68k-amigaos-gcc $FLAGS $SRC -o $OUT/ahi_sfx_test && chmod a+rwx $OUT/ahi_sfx_test"

if command -v m68k-amigaos-gcc >/dev/null 2>&1; then
	sh -c "$CMD"
else
	flatpak-spawn --host docker run --rm -v "$ROOT:$ROOT" -w "$ROOT" \
		amigadev/m68k-amigaos-gcc:latest sh -c "$CMD"
fi

chmod a+rwx "$OUT/ahi_sfx_test" 2>/dev/null || true
ls -l "$OUT/ahi_sfx_test"
