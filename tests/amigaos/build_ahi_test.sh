#!/bin/sh
# Cross-build the AHI bring-up spike (tests/amigaos/ahi_test.c).
#
# Standalone, render-agnostic AHI client test: opens ahi.device, enumerates
# audio modes, plays a one-shot 16-bit sine and waits for the end-of-sound
# hook. Runs on the B1260 Amiberry target (DH11:) and on real PiStorm3D HW.
#
# Output: build/amigaos/test/ahi_test   (mounted as DH11: on B1260)
# Run from a writable drawer:
#   CD DH11: && ahi_test        -> writes DH11:ahi_test.log
#
# Uses a native m68k-amigaos-gcc if present, else the amigadev/m68k-amigaos-gcc
# docker image via flatpak-spawn (same pattern as the other build scripts).
set -e

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
AHI_SDK=${AHI_SDK:-$ROOT/m68k-amigaos-ahi/Developer/Include/C}
OUT=${OUT:-$ROOT/build/amigaos/test}
SRC=$ROOT/tests/amigaos/ahi_test.c
FLAGS="-m68040 -mhard-float -mcrt=nix20 -O2"

# -I$AHI_SDK must precede the toolchain NDK so this SDK's 6.0 headers win;
# <inline/macros.h>, <exec/*>, <utility/tagitem.h>, <libraries/iffparse.h>
# still resolve from the NDK (the SDK ships no macros.h).
if [ ! -f "$AHI_SDK/devices/ahi.h" ]; then
	echo "AHI SDK headers not found at $AHI_SDK (set AHI_SDK=<Developer/Include/C>)" >&2
	exit 1
fi

mkdir -p "$OUT"

# chmod inside the same shell as the compile: docker writes the binary as root,
# so the sandbox-side chmod below cannot fix it (and the Amiberry dir handler
# then reports "file is write-protected" when the guest tries to run it).
CMD="m68k-amigaos-gcc $FLAGS -I$AHI_SDK $SRC -lm -o $OUT/ahi_test && chmod a+rwx $OUT/ahi_test"

if command -v m68k-amigaos-gcc >/dev/null 2>&1; then
	sh -c "$CMD"
else
	flatpak-spawn --host docker run --rm -v "$ROOT:$ROOT" -w "$ROOT" \
		amigadev/m68k-amigaos-gcc:latest sh -c "$CMD"
fi

chmod a+rwx "$OUT/ahi_test" 2>/dev/null || true
ls -l "$OUT/ahi_test"
