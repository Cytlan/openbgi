/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * os_gl.h - the picture shown through OpenGL: the last step of a back end
 *           whose system has a graphics processor to scale with (os_gl.c;
 *           builds with BGI_GL: Linux and Windows)
 *
 * The engine and its compositor are as they are without it: the frame's
 * picture comes in rows of pixels.  Here it becomes a texture, and the
 * graphics processor draws it into the window at the size it is shown
 * at - by the nearest pixel, bilinearly, bicubically (Catmull-Rom), or
 * through one of the shader chains the program carries (the Anime4K modes,
 * which make an anime-style picture larger with small networks; the files
 * are under shaders/ and are read by os_glhook.c).
 *
 * A back end finds OpenGL at run time, makes a context current on the
 * game's window and hands over how functions are found by their names:
 *
 *     if(OsGl_Wanted() && <a context is current> && OsGl_Start(Load)) ...
 *     OsGl_Blit(surface, x, y, w, h, x, y, picW, picH);     // what a frame changed
 *     if(OsGl_Draw(clientW, clientH, px, py, pw, ph, rule)) <swap the buffers>
 *     OsGl_Stop();                                          // before the context goes
 *
 * OsGl_Start refuses a context below OpenGL 3.0 and one of a software
 * renderer (llvmpipe and its kin, Windows' own): scaling on the processor
 * through such a one is slower than the back end's own scaler.
 * BGI_GL=off keeps OpenGL out altogether, BGI_GL=force takes a software
 * renderer too, BGI_GL=old also goes the way of OpenGL 3.0 (the oldest
 * that will do) on a newer one.
 */
#ifndef BGI_HOST_OS_GL_H
#define BGI_HOST_OS_GL_H

#include "host/os.h"

typedef void (*OsGlProc_t)(void);
// an OpenGL function by its name; NULL when the context has none of that name
typedef OsGlProc_t (*OsGlLoader_t)(const char* name);

#define OS_GL_OFF   0 // OsGl_Wanted: BGI_GL=off
#define OS_GL_AUTO  1 // not said: where there is a graphics processor
#define OS_GL_FORCE 2 // BGI_GL=force: a software renderer will do too
#define OS_GL_OLD   3 // BGI_GL=old: as force, and as a system with OpenGL 3.0 alone would have it (no core context, GLSL 1.30): for trying that way

/* The shader files the program carries, as tools/mkpack.py packs them
 * (kept by the caller); before the first window.  Without them there are
 * no filters beyond the three. */
void OS_UseShaders(const unsigned char* pack, unsigned int size);

int OsGl_Wanted(void);
/* With a context current: 1 when OsGl_Start would take it, with nothing
 * set up - for a back end that has to know before the game's window is
 * there, and asks on a window made for the question.  Says on stderr why
 * not. */
int OsGl_Usable(OsGlLoader_t load);
/* With a context current: 1 when it will do, and everything is set up in
 * it.  Says on stderr what it found. */
int OsGl_Start(OsGlLoader_t load);
void OsGl_Stop(void); // with the context current; also without a start
int OsGl_On(void);

/* w x h pixels of `s` from (srcX, srcY) are the picture's at (dstX,
 * dstY); the picture is picW x picH (another size than before: it starts
 * black again). */
void OsGl_Blit(const OsSurface_t* s, int dstX, int dstY, int w, int h, int srcX, int srcY, int picW, int picH);

/* The picture into the rectangle (x, y, w, h) of the window's client area
 * of targetW x targetH pixels, black around it, by the rule
 * (OPENBGI_SCALE_*: the three, or OPENBGI_SCALE_OWN + n for filter n of
 * OsGl_FilterName, with OPENBGI_SCALE_HIGH its larger networks).  1 when
 * it was drawn: the caller swaps the buffers. */
int OsGl_Draw(int targetW, int targetH, int x, int y, int w, int h, int rule);
int OsGl_Black(int targetW, int targetH); // all of it black; 1 as OsGl_Draw

// the name of filter n beyond the three (0 on); NULL when there is none: OpenGL is not on, or the program carries no shaders
const char* OsGl_FilterName(int n);

#endif // BGI_HOST_OS_GL_H
