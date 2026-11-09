# tests/amigaos — test harness & testing methodology

Detailed reference for the test harness and its testing methodology.
Covers how to write and run standalone tests and how the target harness drives
Amiberry.

## Testing methodology (do this FIRST, before running the full game)

Running the full game on Amiberry/PiStorm is slow (minutes per iteration) and
crash-prone. **Whenever a bug is in a unit-testable function (matrix math, float
formatting, byte-swap, stream parsing, etc.), write a small standalone test app
and run that instead of the game.**

Two tiers, in order of preference:

1. **Host-side test (fastest, no emulator).** Compile the pure-C++/C code on
   Linux x86 with a tiny `main()`. Catches logic bugs in seconds.
   Example: `/tmp/kilo/matrix_test.cpp` (librw Matrix ops) — run `g++ … && ./a.out`.

2. **Standalone AmigaOS test (cross-compile + run on the emulator).** A tiny
   `.c`/`.cpp` linked against the same objects, which **writes its own log file**
   (e.g. `freopen("DH11:test.log","w",stdout)`) so the harness reads it directly
   from `build/amigaos/` — do NOT route output through `reVC.log`.
   Compile: `m68k-amigaos-gcc -m68040 -m68881 -mhard-float -mcrt=nix20 -O0 -o test test.c -lm`.
   Run on the emulator via **Ramiga+E → type `DH11:test` → Return** (Execute Command),
   then read `build/amigaos/test.log`.
   Example: `tests/amigaos/floatmath_test.c`; build both the buggy and fixed
   variants with `tests/amigaos/build_floatmath.sh`. Inputs must be `volatile` or
   gcc constant-folds the calls and the test falsely passes.

   Driving the emulator from the agent: launch on the host
   (`flatpak-spawn --host env DISPLAY=${DISPLAY:-:0} XDG_RUNTIME_DIR=$HOME/.amiberry-run
   /usr/bin/amiberry -f "$AMIBERRY_CONFIG" -G`, with `AMIBERRY_CONFIG` pointing at the
   `B1260_serialtest.uae` config) and run commands through the IPC socket with
   `tests/amigaos/ipc_run.py` (needs the amiberry-mcp `src` on `PYTHONPATH` via
   `AMIBERRY_MCP_SRC`). The `XDG_RUNTIME_DIR` override is required when driving from
   the VS Code flatpak: the sandbox's `$XDG_RUNTIME_DIR/amiberry.sock` is a symlink to
   `$HOME/.amiberry-run/amiberry.sock`, so Amiberry must create its socket there or
   the MCP/IPC tools report "socket not found". Run `run_target.py` with the venv
   python from the amiberry-mcp checkout, not system python3 (missing
   websockets/jeepney).

Use `%f` (never `%g`) in these tests, but treat the printed text as unreliable —
assert on the numeric value. See `docs/platform-gotchas.md`.

## Test harnesses (details)

Single home: **`tests/amigaos/`**; one entry point: `tests/amigaos/run_tests.sh`
(`host` | `amigaos` | `all` | `target`), backed by `tests/amigaos/Makefile`
(`make run` builds and runs the host suite; `make amigaos` cross-builds into
`build/amigaos/test` (DH11:) via `m68k-amigaos-gcc` or the Bebbo docker image;
`make target-run` also runs them in Amiberry and parses the logs). `all` stays
emulator-free. `build/amigaos/Makefile`'s `smoke` target delegates to it.
**All test build files land under `build/amigaos/test/`** (host binaries in
`build/amigaos/test/host/`, m68k binaries, the generated script, the staged
`american.gxt` and target logs); the source tree and the rest of the
`build/amigaos/` game output are never written by the harness, and Python
bytecode is disabled so no `__pycache__` appears. The `B1260_serialtest.uae`
Amiberry config mounts `build/amigaos/test` as `DH11:`.
Host-safe pure-logic coverage: endianness, float ABI, angle wrap, real GXT
round-trip, save swap, path normalization, list guard, CPU-skinning blend/compose
(`skinning_test`, `vendor/librw/src/mini/miniskin.h`), client-array submission
policy/chunk math (`miniarray_test`, `vendor/librw/src/mini/miniarray.h`),
DXT1/3/5 + palette +
colour-conversion + box-downscale texture decode (`texture_decode_test`,
`vendor/librw/src/mini/minidecode.h`, with an independent per-texel reference and
a real `generic.txd` smoke), BMP header/palette/pixel decode (`bmp_test`,
`vendor/librw/src/mini/minibmp.h`; LE fields, row padding/order, BI_RGB vs
BI_BITFIELDS alpha), STL, C++ runtime, the `object.dat`
tokenizer/parsers (`parse_float_test`, plus `objdata_scan_test` on target), the
frame profiler core (`frameprofile_test`, `src/core/FrameProfile.h`; ring/window,
slow-frame predicate, tenths formatter, `FP_OUTSIDE` subset exclusion,
render-group mapping), the MINI render-stats aggregation (`rwministats_test`,
`vendor/librw/src/mini/rwministats.h`; frame clear preserving the caps,
cur->last publish, clear-all) and the target-log parser
(`parse_target_logs.py --self-test`). `parse_framelog.py` gained `--prof` /
`--prof-slow` for the `prof:` aggregate/per-frame lines.
`tools/amiga-smoke/` no longer exists (migrated here).

Target harness (`run_tests.sh target`, or `make -C tests/amigaos target-run`):
`gen_target_script.py` generates `build/amigaos/test/run_tests` (+ `target_manifest.json`)
from `$(C_TESTS) $(CXX_TESTS)` and stages `american.gxt` / `generic.txd`. The generated script
is drive/path agnostic (bare names, drawer-relative logs), so the drawer can be
mounted anywhere; `serial_mark` emits `TARGET:*` sentinels over `serial.device`
and writes `target_done.txt` on `TARGET:ALLDONE`. `run_target.py` attaches to
(or launches) Amiberry over IPC, hard-resets, and types
`CD <drawer> && Execute run_tests` (`--amiga-dir`, default `DH11:`; the OS 3.2
Shell supports `&&`, so the drive-agnostic relative script runs with the drawer
as cwd), then waits for the done-file/serial and reports
`PASS/FAIL/SKIP` (non-zero on `FAIL`/`MISSING`). Extra args pass
through (`--serial`, `--no-reset`, `--tests ...`). Real-HW manual path: `CD`
to the drawer and `Execute run_tests` by hand, then
`run_target.py --no-ipc --logs-dir <dir>` parses collected logs with no emulator.
Each test's terminal line must be prefixed with its Makefile name
(`<name>: all ok` / `<name>: N failure(s)`); the log filename may differ
(e.g. `anglewrap_test` -> `anglewrap.log`).

## AHI audio bring-up spike (`ahi_test`)

Standalone, render-agnostic AHI client (`ahi_test.c`, build `build_ahi_test.sh`) that proves the
AHI path works before any `sampman_ahi` backend: opens `ahi.device`, enumerates audio modes,
allocates the installed (Paula) driver, plays an audible one-shot 16-bit 1 s / 1 kHz sine
(unmute AHI to hear it) and waits for the end-of-sound hook — the pass criterion is the hook,
not audibility. It writes `ahi_test.log` in the current directory and prints a terminal line.

- **Not wired into the auto-suite** (manual spike, like `qpu_light_test`): `Makefile` /
  `run_tests.sh`'s test lists are deliberately untouched.
- Build: `tests/amigaos/build_ahi_test.sh` -> `build/amigaos/test/ahi_test` (DH11:). The AHI 6.0
  developer headers live in `m68k-amigaos-ahi/` (gitignored, referenced not vendored); `AHI_SDK`
  defaults to `m68k-amigaos-ahi/Developer/Include/C`. A missing SDK is a hard error, not a skip.
- Run: `CD DH11: && ahi_test`; read `build/amigaos/test/ahi_test.log`. Expect `ahi_test: all ok`,
  or `ahi_test: SKIP (no ahi.device)` when the device is absent (a manual-run success signal).
  Real device/alloc/load/play errors exit non-zero.
- `B1260_serialtest.uae` has AHI installed (`ahi.device`, `paula.audio`); audio may be muted — the
  end-of-sound hook, not audio output, is the pass criterion.

## AHI SFX backend de-risk spike (`ahi_sfx_test`)

`ahi_sfx_test.c` (+ `build_ahi_sfx.sh`) is the Phase 0 spike for the `sampman_ahi` backend.
It reads the real `audio/sfx.SDT` +
`audio/sfx.RAW` (byteswapped to native 68k), asserts the SDT structure, times `fseek`+`fread`+
byteswap for small/median/max samples, proves the **one-shot** (`AHIP_LoopSound, AHI_NOSOUND`)
and **looped** (`AHIP_LoopOffset/Length`) tag paths, allocates 43 channels / 128 sounds at
22050 Hz, and runs 43 simultaneous loops. `CD DH12: && DH11:ahi_sfx_test` (the drawer holding
`Audio/`), then read `DH12:ahi_sfx_test.log`; expect `ahi_sfx_test: all ok` (or `SKIP (no
ahi.device)`). Not wired into the auto-suite.

Measured on B1260 Amiberry (Paula, 2026-10-12): alloc 43ch/128sounds/22050 succeeded (negotiated
22030 Hz); read+swap small(14 B)/median(34.7 KB)/max(218 KB) = 0.7/8.5/52 ms; one-shot hook fires
at start **and** end (`SoundFunc` is per play-segment, so looping fires once per loop wrap — the
backend detects one-shot end by duration instead); 43 loops ran clean.

**AHI is disabled by default** (`REVC_AHI=1` opts in). On the real Emu68/PiStorm3D box the AHI
drivers were unstable (Paula `AHI_AllocAudio` hangs; the USB card wedges Emu68's xhci with
`No TD found` and freezes), so HW audio is deferred — see `docs/PORT_STATUS.md` (2026-10-12). For
live serial bring-up, `socat FILE:/dev/ttyUSB0,b9600,raw,echo=0 TCP:127.0.0.1:<mcp-port>` bridges
the Amiga serial (Exec `RawPutChar`, `REVC_SERIAL=1`) to the `serial-tcp-log` MCP; note Emu68's USB
`xhci` debug flood can saturate 9600 baud.
