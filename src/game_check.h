/*
 * The game check: the program runs only on the player's own copy of the
 * supported Steel Battalion release (USA, title 43430002, version 1).
 * The recompiled code is tied to that exact program, and the download holds
 * no game files, so a release build checks default.xbe before it starts.
 */
#ifndef TL_GAME_CHECK_H
#define TL_GAME_CHECK_H

#include <stddef.h>

/* The project's name: windows, messages, the saves folder. The ini and the
 * window title come from RECOMP_APP_NAME in CMakeLists.txt. */
#define TL_APP "Titanium Legion"

/* SHA-1 of the supported default.xbe. build.bat checks the same value. */
#define TL_XBE_SHA1 "338b3e1022f6e4e6a55aee430e091af8113f660e"

/* 1 if `xbe` (the whole default.xbe, `size` bytes) is the supported release.
 * `found` receives its SHA-1 as 40 lower-case hex digits (empty if it could
 * not be computed); it must hold 41 characters. */
int tl_game_check(const void *xbe, size_t size, char found[41]);

#endif /* TL_GAME_CHECK_H */
