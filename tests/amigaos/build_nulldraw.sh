#!/bin/sh
# Cross-build nulldraw_test: glDrawElements submission-vs-size probe.
#
# Target: PiStorm3D real HW (needs the shared minigl.library 29.x; Amiberry's
# B1260 config has no MiniGL context, so it cannot run there). Links the SDK's
# import lib, so run the binary with minigl.library reachable (e.g. next to the
# game, or in LIBS:).
#
# Output: build/amigaos/nulldraw_test
# Run from anywhere; uses a native m68k-amigaos-gcc if present, else the
# amigadev/m68k-amigaos-gcc docker image via flatpak-spawn.
set -e

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SDK=${PISTORM3D:-$ROOT/PiStorm3D/SDK}
OUT=${OUT:-$ROOT/build/amigaos}
INC=$SDK/minigl-shared-library/include
SRC=$ROOT/tests/amigaos/nulldraw_test.c
FLAGS="-m68040 -mhard-float -mcrt=nix20 -O2"

if [ ! -f "$INC/proto/minigl.h" ]; then
	echo "SDK headers not found at $INC (set PISTORM3D=<SDK root>)" >&2
	exit 1
fi

mkdir -p "$OUT"

CMD="m68k-amigaos-gcc $FLAGS -I$INC $SRC -L$SDK/lib -lminigl -lm -o $OUT/nulldraw_test"

if command -v m68k-amigaos-gcc >/dev/null 2>&1; then
	sh -c "$CMD"
else
	flatpak-spawn --host docker run --rm -v "$ROOT:$ROOT" -w "$ROOT" \
		amigadev/m68k-amigaos-gcc:latest sh -c "$CMD"
fi

chmod a+rwx "$OUT/nulldraw_test" 2>/dev/null || true
ls -l "$OUT/nulldraw_test"
