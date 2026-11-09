#ifndef __GTA_AMIGA_C64_INTRO_H__
#define __GTA_AMIGA_C64_INTRO_H__

/*
 * AmigaOS C64 "crunching" boot intro: a procedural 4.5 s pastiche of a C64
 * loading screen (40x25 PETSCII garbage, raster-bar border, typewriter status
 * text, fake progress bar, track/sector counter, blinking drive LED and build
 * ID), followed by the existing PiStorm3D logo splash.
 *
 * Everything is generated at runtime: no video codec, no external asset and no
 * new library. The model/geometry lives in src/core/C64Intro.h (host-tested);
 * this is only the renderer. AmigaOS only.
 *
 * Call AmigaC64IntroShow() once from main(), after the MiniGL context exists
 * and before AmigaSplashShow(). It blocks for about c64intro::kTotalMs, pumping
 * window events so the game can still be quit. Returns false and boots the
 * game normally if the texture cannot be created or uploaded.
 */

bool AmigaC64IntroShow(void);

#endif // __GTA_AMIGA_C64_INTRO_H__
