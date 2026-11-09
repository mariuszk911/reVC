# reVC — AmigaOS 68k fork

This is a fork of [reVC](https://github.com/mrxenginner/reVC), the
reverse-engineered Grand Theft Auto: Vice City source, with a port to classic
AmigaOS on 68k.

The port targets 68040/68060 systems with an FPU, using PiStorm/Emu68, an RTG
display and MiniGL (`minigl.library`). It adds an AmigaOS platform layer, AHI
audio with optional `mpega.library` streamed music, asynchronous asset loading,
MiniGL rendering with CPU vertex lighting and MatFX vehicle reflections,
windowed desktop mode, boot/loading splash screens, profiling and streaming
diagnostics, and a host/target test harness.

The port is still in progress. The upstream reVC README remains in
[`README.md`](README.md); fork-specific runtime and build options are in
[`FUN_WITH_FLAGS.md`](FUN_WITH_FLAGS.md).

## Requirements

### To run

- A complete **GTA Vice City PC v1.0** installation. Game assets are not
  included; you must own the game.
- A classic AmigaOS 68k system or emulator with a 68040/68060 CPU and FPU,
  PiStorm/Emu68, and an RTG display.
- `minigl.library` 29.x in `LIBS:` (30.x is only needed for the experimental
  `REVC_DIRECT` path).
- An AHI device for audio. `mpega.library` is optional and enables streamed
  audio such as radio, ambience and cutscene music.
- The game must be started from its own directory.

### To build

- A Linux or other POSIX build host with Python 3.
- Either the Bebbo `m68k-amigaos-gcc` toolchain in `PATH`, or Docker.
- The PiStorm3D MiniGL SDK, containing:
  - `lib/libminigl.a`
  - `minigl-shared-library/include/`
  - a sibling `bin/minigl.library`
- The `vendor/librw` submodule. A source snapshot should already include it;
  in a Git checkout run `git submodule update --init --recursive`.

## Building

If you are building from a Git checkout, initialize the submodules from the
repository root:

```sh
git submodule update --init --recursive
```

### 1. Point the build at the PiStorm3D SDK

```sh
export PISTORM3D="$PWD/sdks/PiStorm3D/SDK"   # adjust to your SDK location
```

The build scripts also auto-detect `PiStorm3D/SDK` and `sdks/PiStorm3D/SDK`.
For a Docker build, keep the SDK inside this repository so it can be mounted
into the container.

### 2. Build with Docker

If `m68k-amigaos-gcc` is not installed, build the local toolchain image once:

```sh
docker build -t amigadev/m68k-amigaos-gcc:with-make build/amigaos
```

Then build and stage:

```sh
./build.sh
```

### 3. Or build natively

With the Bebbo toolchain in `PATH`:

```sh
./build.sh
```

Useful build commands:

```sh
./build.sh --info      # show the resolved configuration
./build.sh --clean     # rebuild from scratch
./build.sh --help      # all options
FP_LEAN=1 ./build.sh   # lean profiler build
```

The default CPU/FPU target is `68040`/`68881` with hard-float. Other common
knobs include `CPU`, `FPU`, `OPT`, `STACK`, `GAMEFILES`, `OUTDIR` and `IMAGE`.

### Build outputs

- `build/amigaos/reVC-pistorm3d` — stripped executable
- `build/amigaos/reVC-pistorm3d.db` — executable with symbols for debugging
- `build/amigaos/reVC-pistorm3d.map` — linker map
- `build/amigaos/reVc_pistorm3d/` — install overlay

## Running

1. Copy the contents of `build/amigaos/reVc_pistorm3d/` over your GTA Vice City
   PC v1.0 installation.
2. Copy `minigl.library` to `LIBS:`. If you want streamed audio, also copy
   `mpega.library` to `LIBS:`.
3. From a Shell, `CD` to the game directory and run:

```amigados
reVC
```

`reVC.db` is the same executable with symbols, useful for crash reports and
debugging. The game writes `reVC.log` and creates its settings/saves in the
game directory.

Windowed mode is configured in `reVC.ini`:

```ini
[VideoMode]
Windowed=1
Width=640
Height=480
```

## Notes

- `REVC_AHI` is enabled by default. Set `REVC_AHI=0` for silent mode.
- `REVC_DIRECT` is experimental and requires `minigl.library` 30.x with the
  VideoCore.card 2.0 backend. Leave it off with 29.x libraries.
- This port is draw-heavy: reducing draw distance and car/pedestrian density
  usually gains more frame rate than lowering the resolution.
- Leave profiling, streaming and serial diagnostics off for normal play.

See [`FUN_WITH_FLAGS.md`](FUN_WITH_FLAGS.md) for all runtime and build options.

## Tests

The host-side test suite runs without an emulator:

```sh
tests/amigaos/run_tests.sh host
```

Target tests require an AmigaOS environment with the test drawer mounted.

## License and credits

This fork keeps the upstream reVC licence: MIT with a Proof-of-Usage condition.
See [`LICENSE.md`](LICENSE.md) and [`PROOF_OF_USAGE.md`](PROOF_OF_USAGE.md).

Grand Theft Auto: Vice City and its assets are the property of their respective
owners. No game assets are included in this repository.
