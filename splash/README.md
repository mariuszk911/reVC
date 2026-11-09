# Boot-splash assets

- **`PiStorm3D.png`** — the splash master. Its bytes are embedded into the
  executable at build time by `build/amigaos/gen_splash.py` and decoded by
  `src/skel/amigaos/amiga_splash.cpp`; it is **not** shipped as a runtime asset.
  Currently 1280x178, four colours + 8-bit alpha, cropped tight to the logo (no
  transparent padding).
- **`PiStorm3D-fullres.png`** — the original 2170x725 source, kept only as the
  master for re-deriving/re-cropping `PiStorm3D.png`. It is not referenced by
  the build.

To change the logo, replace `PiStorm3D.png` (same filename) and rebuild; the
generator picks it up by timestamp. The renderer fits the image to
`screenWidth - 80` and centres it (`src/core/SplashScreen.h`), so keep the
master cropped to the visible logo and with an alpha channel.
