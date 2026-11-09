#!/bin/sh
# ---------------------------------------------------------------------------
# reVC - AmigaOS 68k cross-build driver.
#
# Runs build/amigaos/Makefile either natively (when m68k-amigaos-gcc is in
# PATH, e.g. an /opt/amiga install) or inside the Bebbo toolchain container
# `amigadev/m68k-amigaos-gcc:with-make` (repository mounted at /src).
#
# Usage:
#   build/amigaos/build.sh [--native|--docker] [make targets/args...]
#
# Render platform (RENDER=):
#   pistorm3d  PiStorm3D shared minigl.library 29.x (the only platform). Needs
#              PISTORM3D=<sdk> (minigl-shared-library/include + lib/libminigl.a).
#
# Environment:
#   RENDER=pistorm3d          render platform (selects SDK + reVc_<render> outdir)
#   PISTORM3D=<sdk>           minigl.library 29.x SDK (default <root>/PiStorm3D/SDK)
#   GAMEFILES=<root>/gamefiles  overlay data copied into the output
#   OUTDIR=                   staging dir (default <build>/reVc_<render>)
#   LIBRW_PLATFORM=MINI       librw backend (NULL for a pre-M2 link smoke)
#   CPU=68040 / FPU=68881     -mCPU / -mfpu=
#   STACK=      large task stack in bytes via libnix swapstack.o (default 2 MB)
#   FP_LEAN=1   compile out the detailed frame-profiler spans (lean overlay)
#   SWAPSTACK=  LIBNIX_FIX=   override swapstack.o / optional libnix fix object
#   IMAGE=  JOBS=             container image / parallel jobs
# ---------------------------------------------------------------------------
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)

IMAGE=${IMAGE:-amigadev/m68k-amigaos-gcc:with-make}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}
RENDER=${RENDER:-pistorm3d}
PISTORM3D=${PISTORM3D:-"$ROOT/PiStorm3D/SDK"}
GAMEFILES=${GAMEFILES:-"$ROOT/gamefiles"}
OUTDIR=${OUTDIR:-}
LIBRW_PLATFORM=${LIBRW_PLATFORM:-MINI}
CPU=${CPU:-68040}
FPU=${FPU:-68881}
STACK=${STACK:-2097152}
FP_LEAN=${FP_LEAN:-}
SWAPSTACK=${SWAPSTACK:-}
LIBNIX_FIX=${LIBNIX_FIX:-}
MODE=${MODE:-auto}

MAKE_ARGS=""
for arg in "$@"; do
	case "$arg" in
		--native) MODE=native ;;
		--docker) MODE=docker ;;
		*) MAKE_ARGS="$MAKE_ARGS $arg" ;;
	esac
done

if [ "$MODE" = auto ]; then
	if command -v m68k-amigaos-gcc >/dev/null 2>&1; then
		MODE=native
	elif command -v docker >/dev/null 2>&1; then
		MODE=docker
	else
		echo "error: neither m68k-amigaos-gcc (PATH) nor docker is available" >&2
		echo "       install the Bebbo toolchain or Docker, see M0 of the port plan" >&2
		exit 1
	fi
fi

if [ ! -f "$ROOT/vendor/librw/src/engine.cpp" ]; then
	echo "warning: vendor/librw is not populated; run:" >&2
	echo "         git submodule update --init --recursive" >&2
fi

if [ ! -f "$PISTORM3D/lib/libminigl.a" ]; then
	echo "warning: minigl.library import library not found at '$PISTORM3D/lib/libminigl.a'" >&2
	echo "         set PISTORM3D=/path/to/PiStorm3D/SDK (see M0)" >&2
fi

# Empty overrides would defeat the Makefile defaults (OUTDIR ?= ..., SWAPSTACK
# autodetect), so only pass the optional ones when they are set. Keep each
# NAME=value as its own argument so paths with whitespace survive.
# shellcheck disable=SC2086
if [ "$MODE" = native ]; then
	set -- RENDER="$RENDER" PISTORM3D="$PISTORM3D" \
		GAMEFILES="$GAMEFILES" \
		LIBRW_PLATFORM="$LIBRW_PLATFORM" CPU="$CPU" FPU="$FPU" \
		STACK="$STACK"
	if [ -n "$OUTDIR" ]; then
		set -- "$@" OUTDIR="$OUTDIR"
	fi
	if [ -n "$SWAPSTACK" ]; then
		set -- "$@" SWAPSTACK="$SWAPSTACK"
	fi
	if [ -n "$LIBNIX_FIX" ]; then
		set -- "$@" LIBNIX_FIX="$LIBNIX_FIX"
	fi
	if [ -n "$FP_LEAN" ]; then
		set -- "$@" FP_LEAN="$FP_LEAN"
	fi
	exec make -C "$SCRIPT_DIR" -j"$JOBS" "$@" $MAKE_ARGS
fi

if ! command -v docker >/dev/null 2>&1; then
	echo "error: docker not found (use --native with a PATH toolchain)" >&2
	exit 1
fi

# Translate repo-relative paths for the container (mounted at /src).
to_container() {
	case "$1" in
		"$ROOT"/*) printf '/src/%s' "${1#"$ROOT"/}" ;;
		*)         printf '%s' "$1" ;;
	esac
}
C_PISTORM3D=$(to_container "$PISTORM3D")
C_GAMEFILES=$(to_container "$GAMEFILES")

set -- RENDER="$RENDER" PISTORM3D="$C_PISTORM3D" \
	GAMEFILES="$C_GAMEFILES" \
	LIBRW_PLATFORM="$LIBRW_PLATFORM" CPU="$CPU" FPU="$FPU" \
	STACK="$STACK"
if [ -n "$OUTDIR" ]; then
	set -- "$@" OUTDIR="$OUTDIR"
fi
if [ -n "$SWAPSTACK" ]; then
	set -- "$@" SWAPSTACK="$SWAPSTACK"
fi
if [ -n "$LIBNIX_FIX" ]; then
	set -- "$@" LIBNIX_FIX="$LIBNIX_FIX"
fi
if [ -n "$FP_LEAN" ]; then
	set -- "$@" FP_LEAN="$FP_LEAN"
fi

exec docker run --rm -i \
	-v "$ROOT":/src \
	-w /src/build/amigaos \
	"$IMAGE" \
	make -C /src/build/amigaos -j"$JOBS" "$@" $MAKE_ARGS
