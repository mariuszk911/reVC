#!/bin/sh
# Build reVC for AmigaOS 68k.
#
#   ./build.sh                 incremental build and stage
#   ./build.sh --clean         remove build output, then build from scratch
#   ./build.sh ARGS...         extra ARGS go to build/amigaos/build.py
#
# Runs natively when m68k-amigaos-gcc is in PATH, otherwise uses the Bebbo
# Docker image (IMAGE). Under the VS Code Flatpak sandbox, where docker is not
# in PATH, it re-executes itself on the host through flatpak-spawn.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
IMAGE=${IMAGE:-amigadev/m68k-amigaos-gcc:with-make}
CROSS=${CROSS:-m68k-amigaos-}

CLEAN=0
INFO=0
while [ $# -gt 0 ]; do
	case $1 in
		--clean) CLEAN=1; shift ;;
		--info) INFO=1; shift ;;
		-h|--help)
			cat <<'EOF'
Build reVC for AmigaOS 68k.

  ./build.sh                 incremental build and stage
  ./build.sh --clean         remove build output, then build from scratch
  ./build.sh --info          print the resolved build configuration and exit
  ./build.sh ARGS...         extra ARGS go to build/amigaos/build.py

Runs natively when m68k-amigaos-gcc is in PATH, otherwise uses the Bebbo
Docker image. Under the VS Code Flatpak sandbox it re-executes on the host.
Environment: PISTORM3D, IMAGE, CROSS, CPU, FPU, LIBRW_PLATFORM, RENDER, OPT,
STACK, GAMEFILES, OUTDIR, SWAPSTACK, LIBNIX_FIX, FP_LEAN.

FP_LEAN=1 compiles the detailed frame-profiler spans out (REVC_PROFILE=1 then
reports frame/fps only); it is off by default.
EOF
			exit 0 ;;
		*) break ;;
	esac
done

# PiStorm3D minigl.library SDK (headers + import library). Rebase a relative
# path before validating it.
if [ -z "${PISTORM3D:-}" ]; then
	for d in "$ROOT/PiStorm3D/SDK" "$ROOT/sdks/PiStorm3D/SDK"; do
		if [ -f "$d/lib/libminigl.a" ]; then
			PISTORM3D=$d
			break
		fi
	done
fi
case ${PISTORM3D:-} in
	"") ;;
	/*) ;;
	*) PISTORM3D=$ROOT/$PISTORM3D ;;
esac
if [ -z "${PISTORM3D:-}" ] || [ ! -f "$PISTORM3D/lib/libminigl.a" ]; then
	echo "error: PiStorm3D SDK not found; set PISTORM3D=/path/to/SDK" >&2
	exit 1
fi

# Effective settings, forwarded explicitly to the container paths.
CPU=${CPU:-68040}
FPU=${FPU:-68881}
LIBRW_PLATFORM=${LIBRW_PLATFORM:-MINI}
RENDER=${RENDER:-pistorm3d}
OPT=${OPT:--O2}
FP_LEAN=${FP_LEAN:-}
STACK=${STACK:-2097152}
GAMEFILES=${GAMEFILES:-$ROOT/gamefiles}
case $GAMEFILES in
	/*) ;;
	*) GAMEFILES=$ROOT/$GAMEFILES ;;
esac
case ${OUTDIR:-} in
	"" | /*) ;;
	*) OUTDIR=$ROOT/$OUTDIR ;;
esac

if [ "$CLEAN" = 1 ]; then
	set -- --clean "$@"
fi

if [ "$INFO" = 1 ]; then
	if command -v "${CROSS}gcc" >/dev/null 2>&1; then
		MODE_DESC="native ($(command -v "${CROSS}gcc"))"
	elif command -v docker >/dev/null 2>&1; then
		MODE_DESC="docker ($IMAGE)"
	elif command -v flatpak-spawn >/dev/null 2>&1; then
		MODE_DESC="docker on the host, via flatpak-spawn ($IMAGE)"
	else
		MODE_DESC="unavailable (no ${CROSS}gcc and no docker)"
	fi
	if [ "$STACK" = 0 ]; then
		STACK_DESC="disabled"
	else
		STACK_DESC="$STACK bytes"
	fi
	cat <<EOF
reVC AmigaOS 68k build configuration

  repository:   $ROOT
  build mode:   $MODE_DESC
  SDK:          $PISTORM3D
  render:       $RENDER
  librw:        $LIBRW_PLATFORM
  cpu/fpu:      $CPU / $FPU
  optimisation: $OPT
  profiler:     $([ "$FP_LEAN" = 1 ] && echo "lean (REVC_FP_LEAN)" || echo "full")
  stack:        $STACK_DESC
  gamefiles:    $GAMEFILES
  staging:      ${OUTDIR:-$ROOT/build/amigaos/reVc_$RENDER}
  binary:       $ROOT/build/amigaos/reVC-$RENDER

Detailed toolchain/source info: build/amigaos/build.py --info
EOF
	exit 0
fi

# Native toolchain: build.py reads the settings from the environment.
if command -v "${CROSS}gcc" >/dev/null 2>&1; then
	exec python3 "$ROOT/build/amigaos/build.py" --pistorm3d "$PISTORM3D" "$@"
fi

# No docker in this sandbox: hand the invocation to the host.
if ! command -v docker >/dev/null 2>&1; then
	if command -v flatpak-spawn >/dev/null 2>&1; then
		exec flatpak-spawn --host env \
			PISTORM3D="$PISTORM3D" IMAGE="$IMAGE" CROSS="$CROSS" \
			CPU="$CPU" FPU="$FPU" LIBRW_PLATFORM="$LIBRW_PLATFORM" \
			RENDER="$RENDER" OPT="$OPT" STACK="$STACK" FP_LEAN="$FP_LEAN" \
			GAMEFILES="$GAMEFILES" OUTDIR="${OUTDIR:-}" \
			SWAPSTACK="${SWAPSTACK:-}" LIBNIX_FIX="${LIBNIX_FIX:-}" \
			"$ROOT/build.sh" "$@"
	fi
	echo "error: neither ${CROSS}gcc nor docker is available" >&2
	echo "       install the Bebbo toolchain or Docker" >&2
	exit 1
fi

# Translate a host path inside the repo to its /src mount point.
to_container() {
	case $1 in
		"$ROOT"/*) printf '/src/%s' "${1#"$ROOT"/}" ;;
		*) return 1 ;;
	esac
}

C_PISTORM3D=$(to_container "$PISTORM3D") || {
	echo "error: PISTORM3D must be inside $ROOT for the Docker build" >&2
	exit 1
}
C_GAMEFILES=$(to_container "$GAMEFILES") || {
	echo "error: GAMEFILES must be inside $ROOT for the Docker build" >&2
	exit 1
}
C_OUTDIR=""
if [ -n "${OUTDIR:-}" ]; then
	C_OUTDIR=$(to_container "$OUTDIR") || {
		echo "error: OUTDIR must be inside $ROOT for the Docker build" >&2
		exit 1
	}
fi
C_SWAPSTACK=${SWAPSTACK:-}
if [ -n "$C_SWAPSTACK" ]; then
	C_SWAPSTACK=$(to_container "$C_SWAPSTACK") || C_SWAPSTACK=${SWAPSTACK:-}
fi
C_LIBNIX_FIX=${LIBNIX_FIX:-}
if [ -n "$C_LIBNIX_FIX" ]; then
	C_LIBNIX_FIX=$(to_container "$C_LIBNIX_FIX") || C_LIBNIX_FIX=${LIBNIX_FIX:-}
fi

# Artifacts from older root-run containers are not writable by the invoking
# user (and would make --clean fail); hand them over once.
STALE_OWNER=0
for p in obj gen reVC-pistorm3d reVC-pistorm3d.db reVC-pistorm3d.map \
	reVc_pistorm3d/reVC reVc_pistorm3d/reVC.db; do
	if [ -e "$ROOT/build/amigaos/$p" ] && [ ! -w "$ROOT/build/amigaos/$p" ]; then
		STALE_OWNER=1
		break
	fi
done
if [ "$STALE_OWNER" = 1 ]; then
	docker run --rm -v "$ROOT":/src "$IMAGE" \
		chown -R "$(id -u):$(id -g)" /src/build/amigaos
fi

exec docker run --rm -i \
	--user "$(id -u):$(id -g)" \
	-e HOME=/tmp \
	-v "$ROOT":/src \
	-w /src/build/amigaos \
	-e CPU="$CPU" -e FPU="$FPU" \
	-e LIBRW_PLATFORM="$LIBRW_PLATFORM" -e RENDER="$RENDER" \
	-e OPT="$OPT" -e CROSS="$CROSS" -e STACK="$STACK" -e FP_LEAN="$FP_LEAN" \
	-e GAMEFILES="$C_GAMEFILES" -e OUTDIR="$C_OUTDIR" \
	-e SWAPSTACK="$C_SWAPSTACK" -e LIBNIX_FIX="$C_LIBNIX_FIX" \
	"$IMAGE" \
	python3 build.py --pistorm3d "$C_PISTORM3D" "$@"
