// This file is part of OpenBGI (https://openbgi.net).
// SPDX-License-Identifier: GPL-2.0-only
//
// strings.inc - the texts of the launcher (launcher.bpc), in UTF-8: their numbers in
// the table gT, and the English ones.  The program shows them through the names below;
// strings_en fills the table at start, and a translation (strings_ja.inc) puts its own over
// them when the engine's messages are in its language (obgi.language).
//
// A translation is a copy of strings_ja.inc: a function that assigns the same entries.

#define TXT_WINDOW_TITLE        gT[0]
#define TXT_TITLE               gT[1]

// the subtitles of the screens
#define TXT_SUB_GAMES           gT[2]
#define TXT_SUB_FOLDERS         gT[3]
#define TXT_SUB_ADD_FOLDER      gT[4]
#define TXT_SUB_ADD_PATCH       gT[5]
#define TXT_SUB_EDIT            gT[6]

// what an empty list says
#define TXT_NO_GAMES            gT[7]
#define TXT_NO_GAMES_HINT       gT[8]
#define TXT_NO_FOLDERS          gT[9]
#define TXT_NO_PATCHES          gT[10]
#define TXT_EMPTY_FOLDER        gT[11]
#define TXT_BAD_FOLDER          gT[12]
#define TXT_SCANNING            gT[13]

// the buttons
#define TXT_BTN_FOLDERS         gT[14]
#define TXT_BTN_EDIT            gT[15]
#define TXT_BTN_START           gT[16]
#define TXT_BTN_ADD_FOLDER      gT[17]
#define TXT_BTN_REMOVE_FOLDER   gT[18]
#define TXT_BTN_BACK            gT[19]
#define TXT_BTN_ADD_CURRENT     gT[20]
#define TXT_BTN_USE_CURRENT     gT[21]
#define TXT_BTN_CANCEL          gT[22]
#define TXT_BTN_ADD_PATCH       gT[23]
#define TXT_BTN_REMOVE_PATCH    gT[24]
#define TXT_BTN_REMOVE_GAME     gT[25]
#define TXT_BTN_SAVE            gT[26]
#define TXT_BTN_SORT_NAME       gT[27]
#define TXT_BTN_SORT_RECENT     gT[28]
#define TXT_BTN_BG_RANDOM       gT[29]
#define TXT_BTN_BG_WAVES        gT[30]
#define TXT_BTN_BG_SHAPES       gT[31]

// the "Edit game" screen
#define TXT_FIELD_NAME          gT[32]
#define TXT_FIELD_COMPAT        gT[33]
#define TXT_FIELD_OPTIONS       gT[34]
#define TXT_PATCHES             gT[35]
#define TXT_COMPAT_AUTO         gT[36]
#define TXT_COMPAT_AUTO_ONLY    gT[37]
#define TXT_OPTIONS_NONE        gT[38]
#define TXT_RENAME_TITLE        gT[39]
#define TXT_OPTIONS_TITLE       gT[40]
#define TXT_ASK_REMOVE          gT[41]

// the rows and the panel beside them
#define TXT_DUPLICATE           gT[42]
#define TXT_UP                  gT[43]
#define TXT_COMPUTER            gT[44]
#define TXT_ELLIPSIS            gT[45]
#define TXT_NEVER_PLAYED        gT[46]
#define TXT_LAST_PLAYED         gT[47]
#define TXT_TIME_HM             gT[48]
#define TXT_TIME_M              gT[49]
#define TXT_TIME_SHORT          gT[50]
#define TXT_ROW_PLAYED          gT[51]
#define TXT_INFO_ENGINE         gT[52]
#define TXT_INFO_PLAYED         gT[53]
#define TXT_INFO_TIME           gT[54]
#define TXT_INFO_PATCHES        gT[55]
#define TXT_INFO_WHERE          gT[56]
#define TXT_KIND_FOLDER         gT[57]
#define TXT_KIND_ZIP            gT[58]
#define TXT_KIND_ISO            gT[59]
#define TXT_NO_PICTURE          gT[60]
#define TXT_ENGINE_CHOSEN       gT[61]

// messages
#define TXT_LAUNCH_FAILED       gT[62]
#define TXT_NOT_A_GAME          gT[63]

#define TXT_COUNT               64

strings_en() {
    TXT_WINDOW_TITLE = "OpenBGI - Select game";
    TXT_TITLE = "OpenBGI";

    TXT_SUB_GAMES = "Select Game";
    TXT_SUB_FOLDERS = "Game folders";
    TXT_SUB_ADD_FOLDER = "Add a folder of games, or a single game";
    TXT_SUB_ADD_PATCH = "Add patch";
    TXT_SUB_EDIT = "Edit game";

    TXT_NO_GAMES = "No games found";
    TXT_NO_GAMES_HINT = "Add the folder your games are in with \"Folders\"";
    TXT_NO_FOLDERS = "No folders added";
    TXT_NO_PATCHES = "No patches";
    TXT_EMPTY_FOLDER = "This folder is empty";
    TXT_BAD_FOLDER = "This folder cannot be read";
    TXT_SCANNING = "Looking for games…";

    TXT_BTN_FOLDERS = "Folders";
    TXT_BTN_EDIT = "Edit game";
    TXT_BTN_START = "Start game";
    TXT_BTN_ADD_FOLDER = "Add folder";
    TXT_BTN_REMOVE_FOLDER = "Remove";
    TXT_BTN_BACK = "Back";
    TXT_BTN_ADD_CURRENT = "Add this folder";
    TXT_BTN_USE_CURRENT = "Use this folder";
    TXT_BTN_CANCEL = "Cancel";
    TXT_BTN_ADD_PATCH = "Add patch";
    TXT_BTN_REMOVE_PATCH = "Remove patch";
    TXT_BTN_REMOVE_GAME = "Remove game";
    TXT_BTN_SAVE = "Save";
    TXT_BTN_SORT_NAME = "Sort: name";
    TXT_BTN_SORT_RECENT = "Sort: recent";
    TXT_BTN_BG_RANDOM = "Look: random";
    TXT_BTN_BG_WAVES = "Look: waves";
    TXT_BTN_BG_SHAPES = "Look: shapes";

    TXT_FIELD_NAME = "Name";
    TXT_FIELD_COMPAT = "Compatibility";
    TXT_FIELD_OPTIONS = "Options";
    TXT_PATCHES = "Patches - a later one wins over an earlier one";
    TXT_COMPAT_AUTO = "Automatic (%s)";
    TXT_COMPAT_AUTO_ONLY = "Automatic";
    TXT_OPTIONS_NONE = "none";
    TXT_RENAME_TITLE = "Name of the game";
    TXT_OPTIONS_TITLE = "Options of the command line";
    TXT_ASK_REMOVE = "Remove \"%s\" from the list?\n\nIt stays hidden until its folder is removed and added again.";

    TXT_DUPLICATE = "%s (%d : %s)";
    TXT_UP = "../";
    TXT_COMPUTER = "Computer";
    TXT_ELLIPSIS = "…";
    TXT_NEVER_PLAYED = "Not played yet";
    TXT_LAST_PLAYED = "Last played %s";
    TXT_TIME_HM = "%d h %02d min";
    TXT_TIME_M = "%d min";
    TXT_TIME_SHORT = "under a minute";
    TXT_ROW_PLAYED = "%s  ·  %s  ·  %s";
    TXT_INFO_ENGINE = "Engine";
    TXT_INFO_PLAYED = "Last played";
    TXT_INFO_TIME = "Play time";
    TXT_INFO_PATCHES = "Patches";
    TXT_INFO_WHERE = "Where";
    TXT_KIND_FOLDER = "Folder";
    TXT_KIND_ZIP = "Zip archive";
    TXT_KIND_ISO = "Disc image";
    TXT_NO_PICTURE = "A picture of the game appears here once it was played";
    TXT_ENGINE_CHOSEN = "%s (chosen)";

    TXT_LAUNCH_FAILED = "\"%s\" could not be started.";
    TXT_NOT_A_GAME = "No game was found in \"%s\".";
}
