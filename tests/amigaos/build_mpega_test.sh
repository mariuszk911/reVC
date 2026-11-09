#!/bin/sh
# Cross-build the mpega.library stream-decoder validation (tests/amigaos/mpega_test.c).
#
# Standalone mpega.library client: opens a .adf radio file through a bs_access
# hook that undoes the 0x22 XOR, proves the GCC struct ABI + OpenLibrary work,
# decodes, seeks, and measures the wall-clock decode rate. Defines its own
# MPEGA_CTRL/MPEGA_STREAM via the vendored SDK headers.
#
# Output: build/amigaos/test/mpega_test   (mounted as DH11: on B1260)
# Run from the game drawer (holds Audio/) so the log lands there:
#   CD DH12: && DH11:mpega_test   -> writes DH12:mpega_test.log
#
# Needs mpega.library installed as LIBS:mpega.library (Aminet
# util/libs/mpega_library.lha); the test reports SKIP if it is absent.
#
# Uses a native m68k-amigaos-gcc if present, else the amigadev docker image.
set -e

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${OUT:-$ROOT/build/amigaos/test}
SRC=$ROOT/tests/amigaos/mpega_test.c
MPEGA_INC=$ROOT/src/audio/mpega
FLAGS="-m68040 -mhard-float -mcrt=nix20 -O2 -I$MPEGA_INC"

mkdir -p "$OUT"

# Big stack. The Workbench "Execute a file" requester / Shell only hands a CLI
# ~4 KB of stack, but mpega_test decodes into 2x1152 S16 WORD buffers (4.6 KB)
# and calls into mpega.library (a few KB more), so it overflows the default
# stack and corrupts adjacent memory -> intermittent Guru #80000004 (illegal
# instruction) / address error.  Use the same libnix swapstack.o mechanism as
# the game (build.py --stack): link swapstack.o and define the global
# `unsigned long __stack` with the desired size.
STACK=${STACK:-262144}
STACK_C=$OUT/mpega_stack.c
STACK_SRC="unsigned long __stack = ${STACK}UL;"
if [ ! -f "$STACK_C" ] || [ "$(cat "$STACK_C")" != "$STACK_SRC" ]; then
	printf '%s\n' "$STACK_SRC" > "$STACK_C"
fi

if command -v m68k-amigaos-gcc >/dev/null 2>&1; then
	SWAP=$(m68k-amigaos-gcc -mcrt=nix20 -print-file-name=swapstack.o)
	m68k-amigaos-gcc $FLAGS "$SRC" "$STACK_C" "$SWAP" -o "$OUT/mpega_test"
	chmod a+rwx "$OUT/mpega_test" 2>/dev/null || true
else
	flatpak-spawn --host docker run --rm -v "$ROOT:$ROOT" -w "$ROOT" \
		amigadev/m68k-amigaos-gcc:latest sh -c \
		"SWAP=\$(m68k-amigaos-gcc -mcrt=nix20 -print-file-name=swapstack.o); \
		 m68k-amigaos-gcc $FLAGS $SRC $STACK_C \$SWAP -o $OUT/mpega_test; \
		 chmod a+rwx $OUT/mpega_test"
fi

chmod a+rwx "$OUT/mpega_test" 2>/dev/null || true
ls -l "$OUT/mpega_test"
