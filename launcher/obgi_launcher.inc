// This file is part of OpenBGI (https://openbgi.net).
// SPDX-License-Identifier: GPL-2.0-only
//
// obgi_launcher.inc - OpenBGI's own instructions as the launcher (launcher.bpc) calls
// them, and the numbers that go with them (docs/modding/obgi.md).  Everything else the program does
// with the engine's instructions as the games have them; obgi.generate_pattern, which makes
// its pictures, is wrapped in paint.inc.

// ---- the host's folders -----------------------------------------------------------------
#define OBGI_DIR_OPEN       obgi.dir_open       // path, flags -> h            read a folder's listing (0: cannot be read)
#define OBGI_DIR_COUNT      obgi.dir_count      // h -> n                      the number of entries
#define OBGI_DIR_ENTRY      obgi.dir_entry      // h, i, dst, size -> kind     entry i's name; 0 none, 1 file, 2 folder
#define OBGI_DIR_CLOSE      obgi.dir_close      // h ->                        forget the listing

// ---- games ------------------------------------------------------------------------------
#define OBGI_GAME_PROBE     obgi.game_probe     // path, info -> found         is there a game (folder, .zip, .iso)? fills a GameInfo
#define OBGI_LAUNCH_GAME    obgi.launch_game    // path, profile, patches, options -> ok   does not return when the game starts

// ---- the settings (openbgi.json) --------------------------------------------------------
// path "": the launcher's own settings - and, only to read, the lists "profiles" and "games"
// and what the engine says of the host: "home" (where a folder browser starts), "separator"
// ("/" or "\") and "language" ("en" or "ja": the language of the engine's own messages).
// "dropped" is the list of the paths dropped on the window (after sys.dragdrop_enable(1))
// that the program has not taken yet; setting value i of it to 0 takes that one.
// Setting "preview" to a game's path tells the engine that the game is in the list: it
// has the host find the icon of the game's own program, once, and make a first picture
// of a game that has none (a few seconds of the game, run out of sight; not every host
// does).  "previews" is the number of pictures that came of that since the program began.
// A game's path: what is kept of that game.  The engine writes the file by itself.
#define OBGI_CFG_GET        obgi.cfg_get        // path, key, i, dst, size -> len   value i; -1 when there is none
#define OBGI_CFG_SET        obgi.cfg_set        // path, key, i, value -> ok   value 0 deletes; i == the number of values appends

// OBGI_DIR_OPEN flags
#define DIR_HIDDEN          1           // list hidden entries too (after the others of their kind)
#define DIR_GAMES           2           // a search for games: the engine may look ahead at what could be a game among the entries
#define DIR_DEEPER          4           // .. and list the folders among them ahead (each as with DIR_GAMES)

// OBGI_DIR_ENTRY kinds
#define ENTRY_FILE          1
#define ENTRY_FOLDER        2

// What OBGI_GAME_PROBE fills in
struct GameInfo {
    char name[128];         // the game's name: its openbgi.json entry's, else its product name, else ""
    char profile[16];       // the profile the detection chose ("1.69/444")
    char product[64];       // the product name its programs check for, "" when they do not
    int kind;               // GAME_FOLDER, GAME_ZIP or GAME_ISO
};

#define GAME_FOLDER         1
#define GAME_ZIP            2
#define GAME_ISO            3
