#!/bin/sh
# Validate that amiga_libfix.o defines the float functions and routes their
# bodies to the double (FPU) functions.
set -e
CFLAGS="-m68040 -m68881 -mhard-float -mcrt=nix20 -fno-omit-frame-pointer \
        -fno-builtin-sin -fno-builtin-cos -fno-builtin-sinf -fno-builtin-cosf \
        -fno-builtin -O2 -std=gnu11"
m68k-amigaos-gcc $CFLAGS -c src/skel/amigaos/amiga_libfix.c -o /tmp/libfix.o
echo "COMPILED"
m68k-amigaos-nm /tmp/libfix.o | grep -E " [TtU] (_sinf|_cosf|_sqrtf|_sin|_cos|_sqrt|_atan2)"
