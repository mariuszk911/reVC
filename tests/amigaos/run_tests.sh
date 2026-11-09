#!/bin/sh
# Single build+run entry point for the AmigaOS port test harnesses.
#
#   ./run_tests.sh            build + run the host (x86) tests
#   ./run_tests.sh amigaos    also cross-build the m68k variants into
#                             ../../build/amigaos/test (DH11:), native or docker
#   ./run_tests.sh all        host tests + m68k cross-build (emulator-free)
#   ./run_tests.sh target     cross-build, run inside Amiberry over IPC, and
#                             parse the logs; extra args go to run_target.py
#                             (e.g. --serial / --no-reset / --tests ...)
#
# All build artifacts go under build/amigaos/test/ (host binaries, m68k
# binaries, generated script, staged fixture, logs); the source tree and the
# rest of build/amigaos/ game output are never written. Python bytecode is
# disabled so no __pycache__ appears in tests/amigaos/.
#
# The m68k binaries write their own DH11:<log>.log when run on the target; the
# host tests print to stdout and must all pass.
set -e
cd "$(dirname "$0")"

PYTHONDONTWRITEBYTECODE=1
export PYTHONDONTWRITEBYTECODE

MODE=${1:-host}
if [ $# -gt 0 ]; then shift; fi

case "$MODE" in
host)    make run ;;
amigaos) make amigaos ;;
all)     make run && make amigaos ;;
target)  make amigaos && python3 run_target.py "$@" ;;
*)       echo "usage: $0 [host|amigaos|all|target] [run_target args]" >&2; exit 2 ;;
esac
