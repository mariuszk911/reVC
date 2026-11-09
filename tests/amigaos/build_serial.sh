#!/bin/sh
# Cross-build the serial-port smoke test.
# Native when m68k-amigaos-gcc is on PATH, otherwise via the Bebbo docker image.
# Run from tests/amigaos. Output lands in ../../build/amigaos/test (DH11:).
set -e
CC=${CC:-m68k-amigaos-gcc}
OUT=${OUT:-../../build/amigaos/test}
SRC=serial_test.c
FLAGS="-m68040 -m68881 -mhard-float -mcrt=nix20 -O0"
IMAGE=${IMAGE:-amigadev/m68k-amigaos-gcc:latest}

mkdir -p "$OUT"

if command -v "$CC" >/dev/null 2>&1; then
	$CC $FLAGS -o "$OUT/serial_test" "$SRC"
else
	ROOT=$(CDPATH= cd -- ../.. && pwd)
	docker run --rm -v "$ROOT":/src -w /src/tests/amigaos "$IMAGE" \
		m68k-amigaos-gcc $FLAGS \
		-o /src/build/amigaos/test/serial_test "$SRC"
	docker run --rm -v "$ROOT":/src "$IMAGE" \
		chmod a+rwx /src/build/amigaos/test/serial_test
fi

ls -l "$OUT/serial_test"
