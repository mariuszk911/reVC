# reVC AmigaOS runtime flags

reVC for AmigaOS reads optional `REVC_*` variables from the AmigaDOS environment
at startup. They select optional rendering, audio, startup and diagnostic
features. The only supported render platform is `pistorm3d` (MiniGL /
`minigl.library`).

Set the flags before launching reVC, for example:

```amigados
SetEnv REVC_PROFILE 1
SetEnv REVC_MODE 640x480
SetEnv REVC_AHI_MODE 0x20001
```

Flags are read once at startup, so changing them after the game has started has
no effect. All flags are optional; unset values use the defaults below. To keep
settings permanently, set them in `S:Startup-Sequence` or in a launcher script.
The startup log records the effective values.

## Value syntax

- **Boolean:** `1`, `y`, `yes`, `true`, `on`; `0`, `n`, `no`, `off`, `false`
  (case-insensitive).
- **Integer:** decimal, with an optional `+` or `-` sign. Values outside a flag's
  documented range are clamped. `0` may have a flag-specific meaning; for
  example, `REVC_STREAMSTATS_SLOW_MS=0` disables the slow-update lines.
- **String:** `REVC_LOG` is a path, `REVC_MODE` is `WIDTHxHEIGHT`, and
  `REVC_ANV_MODEL` is a decimal model ID.
- **Hexadecimal:** `REVC_AHI_MODE` takes an AHI audio-mode ID, with or without
  the `0x` prefix.

A malformed boolean or decimal value in the main runtime flags stops startup
with a message in the log. Audio tuning values are more forgiving: invalid or
out-of-range values are clamped or fall back to their defaults.

---

## Startup and presentation

| Flag | Default | Description |
|---|---|---|
| `REVC_SKIPINTRO` | off | Skip the frontend and automatically finish in-engine cutscenes. The game starts without loading a save. |
| `REVC_NOSPLASH` | off | Skip both boot intros: the C64-style intro and the PiStorm3D logo splash. |
| `REVC_C64INTRO` | on | Show the C64-style boot intro before the PiStorm3D logo. Set `0` to go straight to the logo splash; `REVC_NOSPLASH=1` skips both. |
| `REVC_RANDOMSPLASH` | off | Use a random loading screen instead of the default one. This affects loading screens, not the boot intros. |
| `REVC_MODELVIEWER` | off | Start the model/vehicle viewer instead of the game menu. |
| `REVC_ANV_MODEL` | `0` | Decimal model ID to open first. Only used with `REVC_MODELVIEWER=1`; unsuitable IDs fall back to the default. |

**Windowed mode** is configured in `reVC.ini`, not with an environment flag:

```ini
[VideoMode]
Windowed=1
Width=640
Height=480
```

The window is fixed-size and cannot be resized. Closing it exits the game.

---

## Logging and boot diagnostics

| Flag | Default | Description |
|---|---|---|
| `REVC_LOG` | `reVC.log` in the game directory | Override the log path. |
| `REVC_DEBUGLOG` | `0` | Debug output level: `0` off, `1` normal, `2` verbose (file, loader and streamer tracing). |
| `REVC_SERIAL` | off | Mirror the log to the serial port. Serial output is baud-limited and can make loading noticeably slower; use it only for debugging. |
| `REVC_MINILOG` | off | Include extra MiniGL renderer bring-up and statistics messages in the log. |

---

## Display

| Flag | Default | Description |
|---|---|---|
| `REVC_MODE` | stored preference | Force a display size in both windowed and fullscreen modes, for example `REVC_MODE=640x480`. |

This port is draw-heavy: reducing draw distance and the number of cars and
pedestrians can improve frame rate more than lowering the resolution. A lower
resolution mainly helps with fill cost.

---

## Rendering

| Flag | Default | Description |
|---|---|---|
| `REVC_VERTEXARRAY` | on | Use client vertex arrays for geometry. Set `0` to force immediate-mode drawing for troubleshooting or comparison. |
| `REVC_LOCKARRAYS` | on | Use locked vertex arrays where supported. Set `0` to disable locking. |
| `REVC_LOCKWIN` | on | Submit large meshes in smaller locked windows so they fit the MiniGL limits. Set `0` to disable the windows. |
| `REVC_DIRECT` | off | Experimental fast path for `minigl.library` 30.x with the VideoCore.card 2.0 backend. Requires `REVC_VERTEXARRAY=1`; it has no effect with immediate mode and does not cover skinned geometry, 2D/3D drawing or MatFX. Leave it off with 29.x libraries. |
| `REVC_CLEAR` | off | Clear the colour buffer each frame. The performance effect depends on the graphics driver; test on your own setup. |
| `REVC_MATFX` | on | Vehicle body reflections (MatFX environment mapping). Set `0` for a matte look or to reduce rendering cost. |

---

## Audio

Audio is provided through AHI and is enabled by default; older instructions that
tell you to set `REVC_AHI=1` are no longer necessary. If `ahi.device` is missing,
audio automatically degrades to silent. Streamed audio additionally requires
`mpega.library` (Aminet `util/libs/mpega_library.lha`) in `LIBS:`.

| Flag | Default | Description |
|---|---|---|
| `REVC_AHI` | on | Enable AHI audio. Set `0` for silent mode. When enabled, the AHI audio-mode requester appears at boot. |
| `REVC_AHI_MIXFREQ` | `22050` | Requested AHI mix rate in Hz. Range 4000..48000; some Paula modes accept a narrower range. The actual rate is shown in the log. |
| `REVC_AHI_MODE` | AHI preferred mode | Force an AHI audio mode by hexadecimal audio ID, for example `0x20001`. The startup log lists available modes; pick one there if you hear nothing. |
| `REVC_AHI_CACHE` | `8192` KB | Sound-effect sample cache size. Range 2048..65536 KB; a larger cache keeps more samples resident. |
| `REVC_AHI_SOUNDS` | `128` | Number of AHI sound slots. Range 12..512. Fewer slots can use less memory but cause more sample reloads. |
| `REVC_AHI_BUSES` | `2` | Output buses: `2` = separate SFX and streamed-audio buses; `1` = streams mixed into the SFX bus. |
| `REVC_AHI_BUSBOOST` | on | Compensate for the level drop caused by using two AHI channels. Only applies with `REVC_AHI_BUSES=2`; set `0` for the raw, quieter two-bus mix. |
| `REVC_AHI_MUSIC` | on | Enable streamed audio: radio, ambience, cutscene music and mission/police speech. Set `0` to keep streams silent while leaving sound effects unchanged. |
| `REVC_AHI_DECODE_LEAD` | `250` ms | Decoded-audio lead per stream. Range 20..2000 ms; a larger lead rides out short hitches at the cost of memory and latency. |
| `REVC_AHI_MUSICVOL` | `128` | Extra music gain, 0..128 (`128` = unity). Useful when streamed radio is too quiet or too loud relative to sound effects. |
| `REVC_AHI_LEAD` | `250` ms | How much mixed audio is queued ahead of AHI. Range 20..1000 ms; raise it if audio breaks up when frames run long. |
| `REVC_AHI_MIXTASK` | on | Refill the sound-effect ring from a background task instead of the game loop, so audio keeps running during menus and level loads. Requires `REVC_AHI_BUSES=2`; set `0` for the synchronous path. |
| `REVC_AHI_MUSIC_QUALITY` | `2` | Streamed-audio decode quality, 0..2. Lower values use less CPU at some quality cost. |
| `REVC_AHI_MUSIC_DIV` | `1` | Streamed-audio output division: `1` = native rate, `2` or `4` = lower decode rate and less CPU. Values other than 1, 2 or 4 use 1. |
| `REVC_AHI_TRACE` | off | Dense per-operation audio logging. Adds a lot of log I/O; do not use it while measuring performance. |
| `REVC_AHI_STREAMTEST` | `-1` | Force a streamed-track index for testing. `-1` disables it. Not intended for normal play. |

---

## Streaming

| Flag | Default | Description |
|---|---|---|
| `REVC_ASYNC_STREAM` | on | Load assets in the background. Set `0` to force the synchronous loader. |
| `REVC_STREAMSTATS` | off | Periodically log streaming statistics and slow streaming updates. Useful when diagnosing hitches; leave it off for normal play. |
| `REVC_STREAMSTATS_FRAMES` | `150` | Number of streaming updates between statistics reports. |
| `REVC_STREAMSTATS_SLOW_MS` | `20` ms | Threshold for reporting a slow streaming update. `0` disables the slow-update lines. |

---

## Profiling and texture diagnostics

| Flag | Default | Description |
|---|---|---|
| `REVC_PROFILE` | off | Show the on-screen frame profiler. **F10** cycles hidden, compact and detailed pages; **Shift+F10** writes a snapshot. Leave it off for normal play. |
| `REVC_PROFILE_MS` | `0` (auto) | Slow-frame threshold for the profiler, in milliseconds. `0` selects the automatic value. |
| `REVC_TEXUNIQ` | off | Count distinct textures bound per frame. Diagnostic information only. |
| `REVC_TEXMEMLOG` | off | Log texture-memory totals periodically and at shutdown. Useful when investigating missing or white textures; diagnostic information only. |

---

## Performance quick reference

- Leave all diagnostics off for normal play.
- Reduce draw distance and car/pedestrian density first; this is a draw-heavy
  game, so those settings can gain more than lowering the resolution.
- On a 30.x MiniGL library, try `REVC_DIRECT=1`; on 29.x, leave it off.
- If vehicle reflections cost too much, set `REVC_MATFX=0`.
- If streamed audio causes stutter, try `REVC_AHI_MUSIC_QUALITY=0`,
  `REVC_AHI_MUSIC_DIV=2`, or `REVC_AHI_BUSES=1`.
- If audio breaks up during long frames, increase `REVC_AHI_LEAD` or
  `REVC_AHI_DECODE_LEAD`.
- If textures look wrong, enable `REVC_TEXMEMLOG=1` temporarily and check the
  log.

---

## Obsolete flags

These names may appear in older scripts or logs. They are accepted but have no
effect.

| Obsolete flag | Use instead |
|---|---|
| `REVC_WINDOWED` | `[VideoMode] Windowed` in `reVC.ini` |
| `REVC_MOUSEREL` | Relative mouse-look is always used; no switch is needed. |
| `REVC_AHI_REQUEST` | The AHI requester follows `REVC_AHI`. |
| `REVC_AHI_CHANNELS` | No replacement. |
| `REVC_AUDIO_REFLECTIONS` | No replacement; audio reflections are always off. |
| `REVC_DRAWSORT` | No replacement. |
| `REVC_MBLUR`, `REVC_MBLUR_INTERVAL` | No replacement; screen motion blur is unavailable. |
| `REVC_MODELDRAW` | Use `REVC_PROFILE`. |
| `REVC_MINILIGHT`, `REVC_MINILIGHT_QPU`, `REVC_MINILIGHT_QPU_PRELIT`, `REVC_MINILIGHT_QPU_PRELIT_MODULATE` | No replacement; CPU vertex lighting is always used. |
| `REVC_TEXLOADLOG`, `REVC_TEXLOADLOG_MS` | Use `REVC_STREAMSTATS`. |

---

## Build-time options

These are selected when building reVC, not through the AmigaDOS environment.

| Option | Default | Description |
|---|---|---|
| `FP_LEAN=1` (build environment) or `REVC_FP_LEAN` (compiler define) | off | Build a lean profiler: the detailed per-phase timing spans are compiled out. `REVC_PROFILE` still reports frame time and FPS. |

Older build switches `REVC_FRAMELOG` and `REVC_DBG_LAYER` are obsolete; use
`REVC_PROFILE` instead.

---

## Notes

- Keep profiling, streaming, texture and serial diagnostics off for normal play;
  they add log traffic and can slow the game.
- `REVC_MATFX=0` is the quickest way to rule out vehicle-reflection cost.
- `REVC_VERTEXARRAY=0`, `REVC_LOCKARRAYS=0` and `REVC_LOCKWIN=0` are
  troubleshooting/rollback switches. Leave them at their defaults for normal use.
- `REVC_SERIAL=1` is useful for crash debugging, but serial output is slow; do
  not leave it enabled for normal play.
