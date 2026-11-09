#ifndef __GTA_AMIGA_SPLASH_H__
#define __GTA_AMIGA_SPLASH_H__

/*
 * AmigaOS boot splash: the PiStorm3D logo over black, centred and fitted to
 * the screen with a fade in/out.
 *
 * The logo PNG is compiled into the executable (build/amigaos/gen_splash.py),
 * so the shipped game needs no external asset. AmigaOS only.
 *
 * Call AmigaSplashShow() once from main(), after the MiniGL context exists and
 * before the game, its loading screens, cutscenes or intros start. It blocks
 * until the splash has been up for at least splash::kTotalMs, pumping window
 * events so the game can still be quit, then frees the logo texture. If the
 * logo cannot be prepared it returns immediately (false) and the game boots
 * normally.
 */

bool AmigaSplashShow(void);

#endif // __GTA_AMIGA_SPLASH_H__
