/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * ui_internal.h - shared between the dialog files of the POSIX back end
 *                 (uikit.c, msgbox.c, dialogs.c): the dialogs of the
 *                 dialog toolkit (src/include/bgi/ui.h) as windows of their own
 *
 * A dialog is begun with X11Ui_Begin (zeroed, the UI font set), filled in
 * by a builder of ui.h or by the Ui_*Add calls, given its title and run
 * with X11Ui_Run, which returns the id of the button that ended it; the
 * entries are read back from its fields afterwards.
 */
#ifndef BGI_OS_POSIX_UI_INTERNAL_H
#define BGI_OS_POSIX_UI_INTERNAL_H

#include "host/os_x11.h"
#include "host/os_common.h"
#include "bgi/msg.h"
#include "bgi/ui.h"

#include <stdio.h>

// Zero the dialog and open the UI font into `font` (kept until X11Ui_Run is over), the dialog's font.
void X11Ui_Begin(UiDialog_t* d, UiFont_t* font);
/* Create the window (a fixed-size dialog for the window manager) centred
 * on the screen, run the modal loop and close the font; the id of the
 * button that ended it.  A refusing button handler's complaint is shown
 * in a message box over the dialog.  Without memory or a font the dialog
 * counts as cancelled: the cancel button's id, or 0 when it has none. */
int X11Ui_Run(UiDialog_t* d);

#endif
