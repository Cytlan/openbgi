/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * inframe.h - the dialogs the engine draws in the frame
 *             (src/sys/inframe.c); not part of the original engine
 *
 * When the host shows no dialogs of its own (OpenBGI_Host_t.caps without
 * OPENBGI_CAP_DIALOGS, or --dialogs=frame), the message boxes and the
 * scripts' dialogs are the dialog toolkit's (bgi/ui.h), drawn over the
 * window's picture (Screen_Overlay) with a caption bar of their own.  A
 * request the frame takes (Inframe_Takes, asked when it is made) is not
 * handed to the host: the frame after it shows its dialog, the input
 * calls of openbgi.h go to the dialog while it is up (the game gets
 * none), and the button that ends it answers the request.  The dialog
 * can be dragged by its caption.  A refusing button's complaint (the
 * profile's) is a message box over the dialog.
 */
#ifndef BGI_INFRAME_H_
#define BGI_INFRAME_H_

#include "bgi/common.h"
#include "openbgi.h"

// 1 when a request of `kind` (OPENBGI_ASK_*) made now is drawn in the frame
int Inframe_Takes(OpenBGI_t* o, int kind);
/* At the end of a frame: the next request the frame takes goes up, the
 * dialog follows the picture's size and is painted again when it changed;
 * the frame's textInput and its wait */
void Inframe_Frame(OpenBGI_t* o, OpenBGI_Frame_t* f);
// 1 while a dialog is up: the input below is its
int Inframe_Up(OpenBGI_t* o);
/* what has the focus in the dialog that takes the input (for a pad's
 * directions, src/sys/pad.c): 0 while none is up */
#define INFRAME_FOCUS_FIELD  1 // a text field
#define INFRAME_FOCUS_PICKER 2 // a field that steps through numbers or choices with Up and Down
#define INFRAME_FOCUS_BUTTON 3
#define INFRAME_FOCUS_LIST   4
int Inframe_Focus(OpenBGI_t* o);
/* The input while a dialog is up (OpenBGI_Key, ...); a key or button the
 * game's state already noted.  The key's flags are OPENBGI_KEY_*, the
 * button OPENBGI_BUTTON_*, positions the picture's pixels. */
void Inframe_Key(OpenBGI_t* o, int vk, int down, int flags);
void Inframe_Pointer(OpenBGI_t* o, int x, int y);
void Inframe_Button(OpenBGI_t* o, int button, int down, int x, int y);
void Inframe_Wheel(OpenBGI_t* o, int delta);
void Inframe_Text(OpenBGI_t* o, const char* utf8);
/* The display settings (the settings key): up when no dialog is, down
 * again when they are up; nothing where the host does not fit the window
 * to its screen (MainWin_ViewOn).  They answer no request: what is chosen
 * holds at once. */
void Inframe_Settings(OpenBGI_t* o);
int Inframe_SettingsUp(OpenBGI_t* o);
// OpenBGI_Stop, OpenBGI_Delete: the dialog goes (its request goes with the rest)
void Inframe_Free(OpenBGI_t* o);
// the dialogs' typeface (Typeface_Close it), also the engine's child windows' captions'; NULL when none could be had
struct Typeface* Inframe_OpenFont(OpenBGI_t* o);
// the same face with a cell of `height` pixels (the on-screen keyboard's keys)
struct Typeface* Inframe_OpenFontSized(OpenBGI_t* o, int height);

#endif // BGI_INFRAME_H_
