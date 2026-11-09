# ---------------------------------------------------------------------------
# AmigaOS port unit-test harnesses (single home, per the plan).
#
#   make run       build + run every host test (Linux x86, little-endian)
#   make host      build host tests only
#   make amigaos   cross-build every test for m68k (native or Bebbo docker),
#                  output in ../../build/amigaos/test (DH11:)
#   make minigl-smoke  build the PiStorm3D library/context pre-check
#   make target-run    cross-build + run the tests in Amiberry over IPC
#                      (extra driver args: make target-run TARGET_ARGS=--serial)
#   make clean
#
# Host tests are runnable from tests/amigaos/run_tests.sh, which is the
# documented single build+run entry point.
#
# The m68k build defines AMIGAOS + BIGENDIAN so the target paths/logging and
# swap layer are exercised, and uses the same flags as build/amigaos/Makefile
# (-fno-exceptions -fno-rtti, never -fomit-frame-pointer/-malign-int).
# Standalone 68k tests write their own DH11:<name>.log instead of reVC.log.
# ---------------------------------------------------------------------------

ROOT := $(abspath $(CURDIR)/../..)

HOST_CC       ?= cc
HOST_CXX      ?= c++
HOST_CFLAGS   := -O2 -Wall -Wextra -I$(ROOT)/src/core -I$(ROOT)/src/save
HOST_CXXFLAGS := -O2 -Wall -Wextra -std=gnu++11 -I$(ROOT)/src/core -I$(ROOT)/src/save \
                 -I$(ROOT)/src/control -I$(ROOT)/vendor/librw/src/mini

CROSS  ?= m68k-amigaos-
CC     := $(CROSS)gcc
CXX    := $(CROSS)g++
CPU    ?= 68040
FPU    ?= 68881
COMMON := -m$(CPU) -m$(FPU) -mhard-float -mcrt=nix20 \
          -fno-omit-frame-pointer -fno-builtin-sin -fno-builtin-cos \
          -fno-builtin-sinf -fno-builtin-cosf
AM_CFLAGS   := $(COMMON) -O2 -Wall -I$(ROOT)/src/core -I$(ROOT)/src/save \
               -DAMIGAOS -DBIGENDIAN
AM_CXXFLAGS := $(COMMON) -O2 -Wall -std=gnu++11 -fno-exceptions -fno-rtti \
               -fno-threadsafe-statics -I$(ROOT)/src/core -I$(ROOT)/src/save \
               -I$(ROOT)/src/control -I$(ROOT)/vendor/librw/src/mini -DAMIGAOS -DBIGENDIAN

# Same flags with the source root remapped to the container mount (/src), used
# by the docker fallback below: host $(ROOT) include paths do not exist inside
# the Bebbo image.
AM_CFLAGS_DOCKER   := $(subst $(ROOT)/,/src/,$(AM_CFLAGS))
AM_CXXFLAGS_DOCKER := $(subst $(ROOT)/,/src/,$(AM_CXXFLAGS))

# minigl.library 29.x bring-up test (needs the PiStorm3D SDK).
PISTORM3D ?= $(ROOT)/PiStorm3D/SDK
MGL_INC   := -I$(PISTORM3D)/minigl-shared-library/include

IMAGE ?= amigadev/m68k-amigaos-gcc:latest

C_TESTS   := endian_test math_smoke anglewrap_test asset_endian_test path_norm_test \
             parse_float_test objdata_scan_test envflags_test \
             matrix_test floatmath_test stream_channel_test
CXX_TESTS := stl_smoke runtime_smoke file_swap_test list_guard_test skinning_test \
             matrix_guard_test texture_decode_test bmp_test replay_endian_test \
             miniarray_test minilight_test rwministats_test frameprofile_test \
             miniatlas_test pool_index_test splash_test c64_intro_test
# Target-only tests (need the AmigaOS exec/dos headers, so never built on host).
AM_ONLY_C_TESTS := abi_layout_test stream_async_test locale_float_test

# Everything the test harness produces lives under build/amigaos/test/. The game build
# output (build/amigaos/) is only read, never written, by this Makefile.
BUILD     := $(ROOT)/build
GAME_OUT  := $(BUILD)/amigaos
TEST_OUT  := $(GAME_OUT)/test
HOST_OUT  := $(TEST_OUT)/host
AM_OUT    := $(TEST_OUT)

HOST_BINS := $(addprefix $(HOST_OUT)/,$(C_TESTS) $(CXX_TESTS))

# Keep Python from dropping __pycache__ into the source tree.
PY := PYTHONDONTWRITEBYTECODE=1 python3

TARGET_ARGS ?=

.PHONY: all host amigaos amigaos-libfix target-run run clean minigl-smoke

all: run

host: $(HOST_BINS)

run: host
	@for t in $(HOST_BINS); do "$$t" || exit 1; done
	@$(PY) parse_target_logs.py --self-test
	@$(PY) parse_framelog.py --self-test
	@echo "tests: all host tests passed"

# Cross-build, then run inside Amiberry over IPC and parse the logs. Keep
# `all`/`run` emulator-free; only this target touches the emulator.
target-run: amigaos
	$(PY) run_target.py $(TARGET_ARGS)

$(HOST_OUT)/%: %.c
	@mkdir -p "$(HOST_OUT)"
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $< -lm

$(HOST_OUT)/%: %.cpp
	@mkdir -p "$(HOST_OUT)"
	$(HOST_CXX) $(HOST_CXXFLAGS) -o $@ $< -lm

# Build the float-ABI override once. libnix's float *f functions return in d0
# while hard-float callers read fp0, so every cross test (C and C++) links this
# as production does -- even the tests that use no float math today, so a future
# *f use cannot silently fall back to the broken soft-float libm. (anglewrap_test
# and matrix_test exercise it via WrapRadianAngle/fmodf.) libfix_math.c
# #includes the production src/skel/amigaos/amiga_libfix.c and must be compiled
# with -fno-builtin.
amigaos-libfix:
	@mkdir -p "$(AM_OUT)"
	@if command -v $(CC) >/dev/null 2>&1; then \
	  $(CC) $(AM_CFLAGS) -fno-builtin -c libfix_math.c -o "$(AM_OUT)/libfix_math.o" || exit 1; \
	else \
	  docker run --rm -v "$(ROOT)":/src -w /src/tests/amigaos "$(IMAGE)" \
	    $(CC) $(AM_CFLAGS_DOCKER) -fno-builtin -c libfix_math.c -o /src/build/amigaos/test/libfix_math.o || exit 1; \
	  docker run --rm -v "$(ROOT)":/src "$(IMAGE)" \
	    chmod a+rw /src/build/amigaos/test/libfix_math.o || exit 1; \
	fi

# Cross-build every test for m68k. Uses a native $(CC) when present, otherwise
# the Bebbo docker image. Binaries land in build/amigaos/test (DH11:) and are made
# world-executable so the DH11 mount does not report "Permission denied".
amigaos: amigaos-libfix
	@mkdir -p "$(AM_OUT)"
	@for t in $(C_TESTS) $(AM_ONLY_C_TESTS); do \
	  echo "== $$t (C) =="; \
	  if command -v $(CC) >/dev/null 2>&1; then \
	    $(CC) $(AM_CFLAGS) -o "$(AM_OUT)/$$t" "$$t.c" "$(AM_OUT)/libfix_math.o" -lm || exit 1; \
	    chmod a+rwx "$(AM_OUT)/$$t" 2>/dev/null || true; \
	  else \
	    docker run --rm -v "$(ROOT)":/src -w /src/tests/amigaos "$(IMAGE)" \
	      $(CC) $(AM_CFLAGS_DOCKER) -o /src/build/amigaos/test/$$t "$$t.c" /src/build/amigaos/test/libfix_math.o -lm || exit 1; \
	    docker run --rm -v "$(ROOT)":/src "$(IMAGE)" \
	      chmod a+rwx /src/build/amigaos/test/$$t || exit 1; \
	  fi; \
	done
	@for t in $(CXX_TESTS); do \
	  echo "== $$t (C++) =="; \
	  if command -v $(CXX) >/dev/null 2>&1; then \
	    $(CXX) $(AM_CXXFLAGS) -o "$(AM_OUT)/$$t" "$$t.cpp" "$(AM_OUT)/libfix_math.o" -lm || exit 1; \
	    chmod a+rwx "$(AM_OUT)/$$t" 2>/dev/null || true; \
	  else \
	    docker run --rm -v "$(ROOT)":/src -w /src/tests/amigaos "$(IMAGE)" \
	      $(CXX) $(AM_CXXFLAGS_DOCKER) -o /src/build/amigaos/test/$$t "$$t.cpp" /src/build/amigaos/test/libfix_math.o -lm || exit 1; \
	    docker run --rm -v "$(ROOT)":/src "$(IMAGE)" \
	      chmod a+rwx /src/build/amigaos/test/$$t || exit 1; \
	  fi; \
	done
	@echo "== serial_mark (C helper, no float) =="
	@if command -v $(CC) >/dev/null 2>&1; then \
	  $(CC) $(AM_CFLAGS) -o "$(AM_OUT)/serial_mark" serial_mark.c || exit 1; \
	  chmod a+rwx "$(AM_OUT)/serial_mark" 2>/dev/null || true; \
	else \
	  docker run --rm -v "$(ROOT)":/src -w /src/tests/amigaos "$(IMAGE)" \
	    $(CC) $(AM_CFLAGS_DOCKER) -o /src/build/amigaos/test/serial_mark serial_mark.c || exit 1; \
	  docker run --rm -v "$(ROOT)":/src "$(IMAGE)" \
	    chmod a+rwx /src/build/amigaos/test/serial_mark || exit 1; \
	fi
	@if [ -f "$(GAME_OUT)/reVc_pistorm3d/TEXT/american.gxt" ]; then \
	  cp "$(GAME_OUT)/reVc_pistorm3d/TEXT/american.gxt" "$(AM_OUT)/american.gxt"; \
	  chmod a+rw "$(AM_OUT)/american.gxt" 2>/dev/null || true; \
	  echo "tests: staged american.gxt fixture"; \
	else \
	  echo "tests: WARNING american.gxt not staged; asset_endian_test will SKIP"; \
	fi
	@if [ -f "$(GAME_OUT)/reVc_pistorm3d/models/generic.txd" ]; then \
	  cp "$(GAME_OUT)/reVc_pistorm3d/models/generic.txd" "$(AM_OUT)/generic.txd"; \
	  chmod a+rw "$(AM_OUT)/generic.txd" 2>/dev/null || true; \
	  echo "tests: staged generic.txd fixture (game drawer)"; \
	elif [ -f "$(ROOT)/gamefiles/models/generic.txd" ]; then \
	  cp "$(ROOT)/gamefiles/models/generic.txd" "$(AM_OUT)/generic.txd"; \
	  chmod a+rw "$(AM_OUT)/generic.txd" 2>/dev/null || true; \
	  echo "tests: staged generic.txd fixture (gamefiles)"; \
	else \
	  echo "tests: WARNING generic.txd not staged; texture_decode_test will SKIP the smoke"; \
	fi
	@$(PY) gen_target_script.py --out "$(AM_OUT)/run_tests" \
	  --manifest "$(AM_OUT)/target_manifest.json" \
	  $(C_TESTS) $(CXX_TESTS) $(AM_ONLY_C_TESTS)
	@chmod a+rwx "$(AM_OUT)/run_tests" 2>/dev/null || true
	@chmod a+rw "$(AM_OUT)/target_manifest.json" 2>/dev/null || true
	@echo "tests: cross-built into $(AM_OUT)"

# Standalone minigl.library 29.x test (SDK GL types + import library). Run on
# real hardware; not part of the host suite.
minigl-smoke:
	@mkdir -p "$(AM_OUT)"
	$(CC) $(COMMON) -O2 -Wall $(MGL_INC) -o "$(AM_OUT)/minigl_smoke" minigl_smoke.c \
	      -L$(PISTORM3D)/lib -lminigl -lm
	chmod a+rwx "$(AM_OUT)/minigl_smoke" 2>/dev/null || true

clean:
	rm -rf "$(HOST_OUT)"
	rm -rf host
	rm -f "$(AM_OUT)/anglewrap_test" "$(AM_OUT)/file_swap_test" \
	      "$(AM_OUT)/list_guard_test" "$(AM_OUT)/skinning_test" \
	      "$(AM_OUT)/matrix_guard_test" "$(AM_OUT)/texture_decode_test" \
	      "$(AM_OUT)/bmp_test" "$(AM_OUT)/replay_endian_test" \
	      "$(AM_OUT)/matrix_test" "$(AM_OUT)/stream_channel_test" \
	      "$(AM_OUT)/minilight_test" "$(AM_OUT)/rwministats_test" \
	      "$(AM_OUT)/frameprofile_test" \
	      "$(AM_OUT)/miniatlas_test" "$(AM_OUT)/splash_test" \
	      "$(AM_OUT)/c64_intro_test" \
	      "$(AM_OUT)/abi_layout_test" "$(AM_OUT)/stream_async_test" \
	      "$(AM_OUT)/asset_endian_test" \
	      "$(AM_OUT)/path_norm_test" "$(AM_OUT)/endian_test" \
	      "$(AM_OUT)/math_smoke" "$(AM_OUT)/stl_smoke" "$(AM_OUT)/runtime_smoke" \
	      "$(AM_OUT)/parse_float_test" "$(AM_OUT)/objdata_scan_test" \
	      "$(AM_OUT)/envflags_test" "$(AM_OUT)/floatmath_test" \
	      "$(AM_OUT)/libfix_math.o" "$(AM_OUT)/serial_mark" \
	      "$(AM_OUT)/run_tests" "$(AM_OUT)/target_manifest.json" \
	      "$(AM_OUT)/american.gxt" "$(AM_OUT)/generic.txd" \
	      "$(AM_OUT)/target_done.txt" \
	      "$(AM_OUT)/target_serial.log"
