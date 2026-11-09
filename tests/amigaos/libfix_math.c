/*
 * Float-math overrides for the 68k unit test.
 *
 * The overrides are defined once, in the production source below, and included
 * here so the unit test exercises exactly the same override set that ships in
 * reVC. That keeps the two from drifting: if a new *f override is added to
 * amiga_libfix.c, floatmath_test.c covers it automatically.
 *
 * MUST be compiled with -fno-builtin (see build_floatmath.sh), otherwise gcc
 * turns "(float)sin(x)" back into a call to sinf -> infinite recursion.
 */
#include "../../src/skel/amigaos/amiga_libfix.c"
