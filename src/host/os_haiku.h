/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * os_haiku.h - the Haiku additions to the POSIX back end, implemented in
 *              src/host/haiku/haiku.cpp
 *
 * On Haiku the POSIX back end runs as it is, with the X11 window code on
 * Xlibe (Haiku's Xlib compatibility layer, no X server).  The few
 * services that have no POSIX or Xlib counterpart there go through the
 * native kits, whose interfaces are C++; this header is their C side and
 * compiles as C and as C++.  Only the files of src/host/posix/ include it,
 * and only when BGI_HAIKU is defined (PLATFORM=haiku, see the Makefile).
 */
#ifndef BGI_OS_HAIKU_H_
#define BGI_OS_HAIKU_H_

#include <stdint.h>

// C linkage from haiku.cpp as well
#ifdef __cplusplus
#define OS_HAIKU_API extern "C"
#else
#define OS_HAIKU_API
#endif

/* the host's callback, as OsAudioFill_t (os.h): `frames` frames of
 * interleaved 16-bit stereo into `samples`; the frames it had */
typedef int (*OsHaikuAudioFill_t)(void* user, int16_t* samples, int frames);

/* Start a BSoundPlayer (Media Kit) at `rate` Hz that calls `fill` on its
 * own thread for every buffer it plays; 1 when it runs, 0 when the media
 * server or the player is not available (the caller then reads the
 * engine's sound at the clock's pace, as without a sound device). */
OS_HAIKU_API int OsHaiku_AudioOpen(int rate, OsHaikuAudioFill_t fill, void* user);
// stop and delete the player; no callback runs once this returns
OS_HAIKU_API void OsHaiku_AudioClose(void);

// SetCursorPos: move the pointer to (x, y) in screen coordinates (Xlibe has no XWarpPointer)
OS_HAIKU_API void OsHaiku_SetMousePosition(int x, int y);

#endif // BGI_OS_HAIKU_H_
