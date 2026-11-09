#!/bin/sh
# Cross-build the 68k float-ABI unit test in two variants:
#   floatmath_buggy - as the game links today (soft-float float libm)
#   floatmath_fixed - with libfix_math.o overriding the float functions
# Run from tests/amigaos. Output lands in ../../build/amigaos/test (DH11:).
set -e
CC=${CC:-m68k-amigaos-gcc}
OUT=${OUT:-../../build/amigaos/test}
FLAGS="-m68040 -m68881 -mhard-float -mcrt=nix20 -fno-omit-frame-pointer \
       -fno-builtin-sin -fno-builtin-cos -fno-builtin-sinf -fno-builtin-cosf \
       -O0 -DAMIGAOS"

mkdir -p "$OUT"

$CC $FLAGS -DLOGFILE='"DH11:floatmath_buggy.log"' \
    -o "$OUT/floatmath_buggy" floatmath_test.c -lm

$CC $FLAGS -fno-builtin -c libfix_math.c -o "$OUT/libfix_math.o"
$CC $FLAGS -DLOGFILE='"DH11:floatmath_fixed.log"' \
    -o "$OUT/floatmath_fixed" floatmath_test.c "$OUT/libfix_math.o" -lm
rm -f "$OUT/libfix_math.o"

chmod a+rwx "$OUT/floatmath_buggy" "$OUT/floatmath_fixed" 2>/dev/null || true
ls -l "$OUT/floatmath_buggy" "$OUT/floatmath_fixed"
