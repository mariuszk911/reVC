# ---------------------------------------------------------------------------
# reVC - AmigaOS 68k / minigl.library (PiStorm3D) cross-build
#
# Designed to run *inside* the Bebbo toolchain container:
#   amigadev/m68k-amigaos-gcc:with-make
# with the repository mounted at /src, i.e. this directory is /src/build/amigaos.
# The host driver build/amigaos/build.sh mounts and invokes `make` for you.
#
# Overridable knobs (env or command line):
#   CROSS=            toolchain prefix                (default m68k-amigaos-)
#   CPU=68040         -mCPU                            (also 68060)
#   FPU=68881         -mfpu=                            (ignored for 68060 built-in)
#   OPT=-O2           global optimisation level
#   FP_LEAN=1         compile out the detailed frame-profiler spans (lean
#                     overlay; REVC_PROFILE=1 then reports frame/fps only).
#                     Accepts 1/yes/true/on like build.py; toggling it forces a
#                     full rebuild via the flags stamp below.
#   LIBRW_PLATFORM=MINI  librw backend (MINI; use NULL for a pre-M2 link smoke)
#   RENDER=pistorm3d  render platform (only pistorm3d; selects the minigl.library
#                     29.x SDK linked and the reVc_$(RENDER) staging directory)
#   OUTDIR=           staging directory (default $(CURDIR)/reVc_$(RENDER))
#   GAMEFILES=        overlay data dir copied into OUTDIR (default <root>/gamefiles)
#   PISTORM3D=        minigl.library 29.x client SDK root (default <root>/PiStorm3D/SDK)
#   STACK=            large task stack in bytes via libnix swapstack.o
#                     (default 2097152; STACK=0 disables)
#   SWAPSTACK=        override path to libnix swapstack.o (else auto-resolved)
#   LIBNIX_FIX=       extra override object (optional; src/skel/amigaos/
#                     amiga_libfix.c is always discovered and linked)
#   GIT_SHA1=         version string baked into the binary
#
# GCC 6.5 note: some translation units must be compiled at -O1 because
# `-O2 -malign-int` crashes the compiler. Add per-file overrides at the bottom
# of this file, e.g.:
#   $(OBJ)/src/animation/AnimManager.o: CXXFLAGS += -O1
# ---------------------------------------------------------------------------

CURDIR      := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
ROOT        := $(abspath $(CURDIR)/../..)
# Render platform must be known before OBJ/GEN (per-render object tree, so
# switching RENDER never reuses the other platform's objects).
RENDER      ?= pistorm3d
# FP_LEAN accepts the same spellings as build.py (1/yes/true/on, any case).
# Canonicalised so the define and the printed config cannot drift; the flags
# stamp below forces a rebuild whenever it changes.
FP_LEAN_RAW := $(shell echo '$(FP_LEAN)' | tr 'A-Z' 'a-z')
ifneq ($(filter 1 yes true on,$(FP_LEAN_RAW)),)
FP_LEAN_ON := 1
else
FP_LEAN_ON := 0
endif
OBJ         := $(CURDIR)/obj/$(RENDER)
GEN         := $(CURDIR)/gen/$(RENDER)
# Per-render binary too: staging must never pick up the other platform's link.
TARGET      := $(CURDIR)/reVC-$(RENDER)
TARGET_DB   := $(TARGET).db

# ---- Toolchain ------------------------------------------------------------
CROSS       ?= m68k-amigaos-
CC          := $(CROSS)gcc
CXX         := $(CROSS)g++
AR          := $(CROSS)ar
STRIP       := $(CROSS)strip
OBJCOPY     := $(CROSS)objcopy

# ---- Target CPU / FPU ------------------------------------------------------
# Bebbo GCC 6.5 uses -m68881 (not -mfpu=68881) plus -mhard-float to emit FPU
# instructions; FPU is mandatory for this port.
CPU         ?= 68040
FPU         ?= 68881
CPUFLAGS    := -m$(CPU) -m$(FPU) -mhard-float

# Never add -fomit-frame-pointer: it reliably broke 3D on real hardware (RTCW).
OPT         ?= -O2

# ---- SDK ------------------------------------------------------------------
# PiStorm3D shared minigl.library 29.x SDK (dispatch API + import lib). Headers
# (minigl-shared-library/include, incl. <proto/minigl.h>) and lib/libminigl.a.
PISTORM3D     ?= $(ROOT)/PiStorm3D/SDK
PISTORM3D_INC ?= $(PISTORM3D)/minigl-shared-library/include
PISTORM3D_LIB ?= $(PISTORM3D)/lib

LIBRW_PLATFORM ?= MINI

# ---- Render platform ------------------------------------------------------
# pistorm3d : PiStorm3D shared minigl.library 29.x dispatch API
#             (<proto/minigl.h>), linked against the import lib in
#             $(PISTORM3D_LIB). The output overlay goes to reVc_$(RENDER).
GAMEFILES     ?= $(ROOT)/gamefiles
OUTDIR        ?= $(CURDIR)/reVc_$(RENDER)

RENDER_INCLUDES := -I$(PISTORM3D_INC)
RENDER_LIBS := -L$(PISTORM3D_LIB) -lminigl

GIT_SHA1    ?= $(shell git -C $(ROOT) rev-parse --short=12 HEAD 2>/dev/null || echo unknown)

# ---- Common flags ---------------------------------------------------------
# GTA_PC comes from src/core/config.h; only target/endianness belongs here.
DEFINES := \
	-DAMIGAOS -DBIGENDIAN -DLIBRW \
	-DRW_$(LIBRW_PLATFORM) \
	-DUSE_OUR_VERSIONING -DNO_HOOK_MALLOC

# REVC_FP_LEAN: compile the per-slot/per-phase profiler spans out. The frame and
# fps numbers still work (REVC_PROFILE=1); the detailed overlay page falls back
# to the compact rows. Use it to A/B profiling overhead itself.
ifeq ($(FP_LEAN_ON),1)
DEFINES += -DREVC_FP_LEAN
endif

# Header dependency tracking for the generic compile rules below. The *.d files
# live next to the objects in $(OBJ), so a header edit rebuilds exactly its
# dependents instead of relying on mtime of the .cpp only.
DEPFLAGS := -MMD -MP

WARNINGS := -Wall -Wno-unused -Wno-unused-parameter -Wno-sign-compare -Wno-reorder

# libnix is required: newlib (the toolchain default) has no <dirent.h>.
# Do NOT add -malign-int: it grows AmigaOS exec/NDK structs (struct Node 14->16)
# so Task/IntuiMessage/... offsets no longer match the OS ABI. The toolchain
# default (2-byte-aligned int) is the correct AmigaOS ABI.
COMMONFLAGS := $(CPUFLAGS) -mcrt=nix20 -fno-omit-frame-pointer \
	-fno-builtin-sin -fno-builtin-cos \
	-fno-builtin-sinf -fno-builtin-cosf $(WARNINGS)

CFLAGS   := $(COMMONFLAGS) $(OPT) -std=gnu11
CXXFLAGS := $(COMMONFLAGS) $(OPT) -std=gnu++11 \
	-fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-strict-aliasing

# ---- Include paths --------------------------------------------------------
# fakerw must shadow librw's public headers first.
SRC_DIRS := $(shell find $(ROOT)/src -type d 2>/dev/null)
INCLUDES := \
	-I$(ROOT)/src/fakerw \
	$(addprefix -I,$(SRC_DIRS)) \
	-I$(ROOT)/vendor/librw \
	-I$(ROOT)/vendor/librw/src \
	-I$(GEN) \
	$(RENDER_INCLUDES)

# ---- Sources --------------------------------------------------------------
# Game code: drop the other platforms' skeletons and the pthread/semaphore
# streaming backends (AMIGAOS uses CdStream_amiga.cpp).
REVC_SRCS := $(shell find $(ROOT)/src \
	\( -path '*/skel/glfw/*' -o -path '*/skel/sdl2/*' -o -path '*/skel/win/*' -o -path '*/skel/android/*' -o -path '*/skel/generic/*' \) -prune -o \
	\( -path '*/audio/oal/*' -o -path '*/audio/mss/*' -o -path '*/audio/eax/*' \) -prune -o \
	\( -name 'CdStream_posix.cpp' -o -name 'CdStream_win.cpp' -o -name 'CdStream_ee.c' \) -prune -o \
	\( -name '*.cpp' -o -name '*.c' \) -print 2>/dev/null)

# librw core + selected backend. Backend `mini/` is additive; other backends
# (d3d/, gl/, ps2/) are excluded so no glad/D3D code is compiled.
LIBRW_SRCS := \
	$(wildcard $(ROOT)/vendor/librw/src/*.cpp) \
	$(wildcard $(ROOT)/vendor/librw/src/lodepng/*.cpp)

ifeq ($(LIBRW_PLATFORM),MINI)
LIBRW_SRCS += $(wildcard $(ROOT)/vendor/librw/src/mini/*.cpp)
endif

ALL_SRCS := $(REVC_SRCS) $(LIBRW_SRCS)

REVC_OBJS  := $(patsubst $(ROOT)/%.cpp,$(OBJ)/%.o,$(patsubst $(ROOT)/%.c,$(OBJ)/%.o,$(REVC_SRCS)))
LIBRW_OBJS := $(patsubst $(ROOT)/%.cpp,$(OBJ)/%.o,$(LIBRW_SRCS))
GEN_OBJS   := $(GEN)/GitSHA1.o $(GEN)/splash_logo.o
OBJS       := $(REVC_OBJS) $(LIBRW_OBJS) $(GEN_OBJS)

# Boot-splash logo embedded into the binary (build-time generated; see
# gen_splash.py). The PNG is a build input, not a runtime asset, and its path
# comes from gen_splash.py so the two build drivers cannot drift.
GEN_SPLASH_PY := $(CURDIR)/gen_splash.py
SPLASH_PNG    := $(shell python3 $(GEN_SPLASH_PY) --print-input)

# ---- Large task stack (libnix swapstack.o) --------------------------------
# swapstack.o swaps to a stack whose size is the global `__stack` longword
# (generated below). The CLI/Shell stack is far too small for reVC and was the
# prime suspect for the first on-target Guru; set STACK=0 to disable.
STACK     ?= 2097152
ifneq ($(STACK),0)
SWAPSTACK ?= $(shell $(CC) -mcrt=nix20 -print-file-name=swapstack.o)
STACK_C   := $(GEN)/amiga_stack.c
STACK_OBJ := $(GEN)/amiga_stack.o
endif

EXTRA_OBJS :=
ifneq ($(strip $(STACK_OBJ)),)
EXTRA_OBJS += $(STACK_OBJ) $(SWAPSTACK)
endif
ifneq ($(strip $(LIBNIX_FIX)),)
EXTRA_OBJS += $(LIBNIX_FIX)
endif

# ---- Link -----------------------------------------------------------------
LDFLAGS := $(CPUFLAGS) -mcrt=nix20 -Wl,-Map=$(CURDIR)/reVC-$(RENDER).map
# minigl.library import lib (PISTORM3D_LIB).
LDLIBS  := $(RENDER_LIBS) -lm

# ---- Build-config stamp ---------------------------------------------------
# make does not track variable changes, so a changed define (FP_LEAN) or flag
# set would otherwise relink the existing objects. Stamp the effective flags in
# the object tree; every object and the link depend on it, and it is only
# rewritten when the text differs, so unchanged builds stay incremental. The
# name is driver-specific because build.py shares this object directory and
# keeps its own stamp: a shared file would make every driver switch rebuild.
FLAGS_STAMP := $(OBJ)/.flags-make
FLAGS_ARGS  := '$(DEFINES)' '$(CXXFLAGS)' '$(CFLAGS)' '$(INCLUDES)'
$(shell mkdir -p $(OBJ) && (printf '%s\n' $(FLAGS_ARGS) | cmp -s - $(FLAGS_STAMP) || printf '%s\n' $(FLAGS_ARGS) > $(FLAGS_STAMP)))

# ---- Rules ----------------------------------------------------------------
.PHONY: all clean print-config smoke stage verify-libfix

all: $(TARGET) $(TARGET_DB) verify-libfix stage

# Assemble the reVc_$(RENDER) test overlay (binary + minigl.library +
# gamefiles/ + FILES.txt/INSTALL.txt).
stage: $(TARGET_DB)
	python3 $(CURDIR)/stage.py --render $(RENDER) --outdir "$(OUTDIR)" \
		--bin "$(TARGET)" --bin-db "$(TARGET_DB)" \
		--gamefiles "$(GAMEFILES)" --pistorm3d "$(PISTORM3D)"

# M0/M3 smoke/unit tests (STL, C++ runtime, endianness, math, angle-wrap,
# save-swap) for m68k. Host tests: tests/amigaos/run_tests.sh.
smoke:
	$(MAKE) -C $(ROOT)/tests/amigaos amigaos CROSS=$(CROSS) CPU=$(CPU) FPU=$(FPU)

# Unstripped link, then keep a symbol copy (.db) and strip the shipped binary.
# Never strip a linked binary: on this toolchain `strip` drops HUNK_RELOC32, and
# the resulting executable fails to load. Re-link with -s instead, which lets the
# linker omit the symbol hunk while keeping relocations. The map is regenerated
# by that link, so CHECK_LIBFIX must run again on the stripped binary.
# $(TARGET) is unstripped whenever this recipe runs: make has just linked it, or
# build.py produced a stripped binary and then refreshed $(TARGET_DB)'s mtime
# (see its stripped re-link), which leaves this target up to date.
$(TARGET_DB): $(TARGET)
	cp -f $(TARGET) $(TARGET_DB)
	$(CXX) $(LDFLAGS) -s -o $(TARGET) $(OBJS) $(EXTRA_OBJS) $(LDLIBS)
	@$(CHECK_LIBFIX)
	@touch $@	# re-link gave $(TARGET) a newer mtime; keep the next make from re-copying the stripped binary over the symbols

$(TARGET): $(OBJS) $(EXTRA_OBJS) $(FLAGS_STAMP)
	$(CXX) $(LDFLAGS) -o $@ $(OBJS) $(EXTRA_OBJS) $(LDLIBS)
	@$(CHECK_LIBFIX)

$(GEN)/GitSHA1.cpp: $(ROOT)/src/extras/GitSHA1.cpp.in
	@mkdir -p $(GEN)
	sed 's/@GIT_SHA1@/$(GIT_SHA1)/' $< > $@

$(GEN)/GitSHA1.o: $(GEN)/GitSHA1.cpp
	$(CXX) $(CXXFLAGS) $(DEFINES) $(INCLUDES) -c $< -o $@

# Boot-splash logo bytes (embedded; no external asset at runtime).
$(GEN)/splash_logo.cpp: $(SPLASH_PNG) $(GEN_SPLASH_PY)
	@mkdir -p $(GEN)
	python3 $(GEN_SPLASH_PY) $< $@

$(GEN)/splash_logo.o: $(GEN)/splash_logo.cpp
	$(CXX) $(CXXFLAGS) $(DEFINES) $(INCLUDES) -c $< -o $@

# Generated libnix swapstack size (see STACK above).
ifneq ($(strip $(STACK_C)),)
$(STACK_C):
	@mkdir -p $(GEN)
	@printf '/* generated for libnix swapstack.o */\nunsigned long __stack = $(STACK)UL;\n' > $@
$(STACK_OBJ): $(STACK_C)
	$(CC) $(CFLAGS) $(DEFINES) $(INCLUDES) -c $< -o $@
endif

$(OBJ)/%.o: $(ROOT)/%.cpp $(FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(DEFINES) $(INCLUDES) $(DEPFLAGS) -c $< -o $@

$(OBJ)/%.o: $(ROOT)/%.c $(FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEFINES) $(INCLUDES) $(DEPFLAGS) -c $< -o $@

# Generated by -MMD above; the find is empty on the first parse of a clean tree.
-include $(shell find $(OBJ) -name '*.d' 2>/dev/null)

# Symbols for crash reports; keeps the stripped binary small.
$(TARGET).sym: $(TARGET_DB)
	$(OBJCOPY) --only-keep-debug $(TARGET_DB) $@ 2>/dev/null || true

# ---- Mandatory libnix float-math overrides --------------------------------
# src/skel/amigaos/amiga_libfix.c is discovered automatically (REVC_SRCS) and
# must always be compiled with -fno-builtin and linked: without these *f
# overrides the soft-float libm returns in d0 while -mhard-float callers read
# fp0, and the entity/frame matrices go to ~1e15. Each *f must be DEFINED BY
# amiga_libfix.c.o in the linker map -- plain symbol presence is not enough,
# because the toolchain's generic soft-float libm (lib__m__.a) defines the same
# names, so a link that dropped amiga_libfix.o would still list them. Runs at
# the end of every link and as `make verify-libfix`.
LIBFIX_SYMS := sqrtf sinf cosf tanf asinf acosf atanf atan2f floorf ceilf \
               rintf fabsf fmodf powf expf logf log10f log2f round copysign
define CHECK_LIBFIX
map="$(CURDIR)/reVC-$(RENDER).map"; \
if [ ! -f "$$map" ]; then \
  echo "verify-libfix: ERROR: $$map not found"; exit 1; \
fi; \
defined=$$(awk '/^[ \t]+\.[^ \t]+[ \t]+0x[0-9a-fA-F]+[ \t]+0x[0-9a-fA-F]+[ \t]+.*\.o$$/ { obj=$$NF } /^[ \t]+0x[0-9a-fA-F]+[ \t]+[A-Za-z_][A-Za-z0-9_]*$$/ && obj ~ /amiga_libfix([.]c)?[.]o$$/ { print $$NF }' "$$map" | sort -u); \
missing=""; \
for s in $(LIBFIX_SYMS); do \
  echo "$$defined" | grep -qx "$$s" || missing="$$missing $$s"; \
done; \
if [ -n "$$missing" ]; then \
  echo "verify-libfix: ERROR: libnix float overrides not defined by amiga_libfix.o:$$missing"; \
  echo "  amiga_libfix.c must be in REVC_SRCS and built with -fno-builtin"; \
  exit 1; \
fi; \
if grep -q 'lib__m__[.]a(' "$$map"; then \
  echo "verify-libfix: ERROR: soft-float lib__m__.a is linked (*f routed through a soft double):"; \
  grep -o 'lib__m__[.]a([^)]*)' "$$map" | sort -u | sed 's/^/  /'; \
  exit 1; \
fi; \
echo "verify-libfix: OK ($(RENDER) $(TARGET))"
endef
verify-libfix: $(TARGET_DB)
	@$(CHECK_LIBFIX)

clean:
	rm -rf $(CURDIR)/obj $(CURDIR)/gen
	rm -f $(CURDIR)/reVC $(CURDIR)/reVC.db $(CURDIR)/reVC.map $(CURDIR)/reVC.sym
	rm -f $(CURDIR)/reVC-*

print-config:
	@echo "ROOT          = $(ROOT)"
	@echo "CPU           = $(CPU) FPU=$(FPU)"
	@echo "LIBRW_PLATFORM= $(LIBRW_PLATFORM)"
	@echo "RENDER        = $(RENDER)  (outdir $(OUTDIR))"
	@echo "PISTORM3D     = $(PISTORM3D)"
	@echo "SWAPSTACK     = $(SWAPSTACK)"
	@echo "STACK         = $(STACK)"
	@echo "LIBNIX_FIX    = $(LIBNIX_FIX)"
	@echo "FP_LEAN       = $(FP_LEAN_ON) (from '$(FP_LEAN)')"
	@echo "REVC sources  = $(words $(REVC_SRCS))"
	@echo "librw sources = $(words $(LIBRW_SRCS))"

# ---- Per-file optimisation overrides (GCC 6.5 workarounds) ----------------
# -O2 with -malign-int makes GCC 6.5 emit invalid assembly / ICE for these files.
$(OBJ)/src/save/GenericGameStorage.o: CXXFLAGS += -O1
$(OBJ)/src/peds/Ped.o: CXXFLAGS += -O1
# -O2 byte-value dataflow is unsound on 68k (MOVE.B/CLR.B do not clear the
# upper 24 bits). GetCarTuning()/UsesPoliceRadio() returned garbage in d0,
# so in-car radio called StartStreamedFile() with an out-of-range track.
$(OBJ)/src/audio/MusicManager.o: CXXFLAGS += -O1
# Gameplay head/IK codegen test: the same 68k -O2 codegen issue affects
# float dataflow in the head IK and animation frame paths.
$(OBJ)/src/peds/PedIK.o: CXXFLAGS += -O1
$(OBJ)/src/animation/FrameUpdate.o: CXXFLAGS += -O1
$(OBJ)/src/animation/RpAnimBlend.o: CXXFLAGS += -O1
$(OBJ)/src/animation/AnimBlendNode.o: CXXFLAGS += -O1

# libnix float-math overrides must not see the float functions as builtins,
# or gcc rewrites "(float)sin(x)" back into a call to sinf (infinite recursion).
$(OBJ)/src/skel/amigaos/amiga_libfix.o: CFLAGS += -fno-builtin
