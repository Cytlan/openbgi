# The command line

    openbgi [OPTIONS] [GAME [the game's own arguments]]

GAME is a game's folder, a `.zip` or `.iso` of one, or any file in the
game's folder. Without GAME, OpenBGI plays the game in the current
folder, or shows the launcher when there is none.

On Windows, use `openbgi.com` in a console to see the output.

## Playing

| Option | Meaning |
|---|---|
| `--overlay` | Play a folder without writing into it; saves go to your data folder |
| `--patch=FILE` | Lay a zip archive or a folder of files over the game; may be repeated, a later one wins |
| `--lang=en` / `--lang=ja` | Language of OpenBGI's own messages (also `BGI_LANG`) |
| `--no-disc-check` | Treat the game as registered; skips the disc check of older games |
| `--disc-marker` | Let the game see the marker file of a disc copy (the original's behaviour: the game then refuses to run from a disc copy) |
| `--movies=decoder` / `--movies=player` | Movies by the built-in decoder, or by the system's player first (Windows) |
| `--dialogs=frame` / `--dialogs=host` | Message boxes drawn inside the game's picture, or as system windows |
| `--child-windows=frame` | Child windows drawn inside the game's picture (the default on Windows, Linux and macOS) |
| `--child-windows=host` | Child windows as windows of their own, where the system has them (Windows, Linux). They do not follow the desktop's scale or a resized window |
| `--stand-in-font=gothic` / `mincho` / `auto` | Which built-in font replaces a font that is not installed |
| `--own-pad-keys=on` / `off` | Use or ignore the pad keys a game sets itself |

## Compatibility

OpenBGI detects which engine version a game needs. These override it.

| Option | Meaning |
|---|---|
| `--engine=NAME` | Use this engine profile (`--list-engines` prints the names) |
| `--productname=NAME` | The product name the game's scripts ask for |
| `--games=FILE` | Use this `openbgi.json` |
| `--enable=FAMILY:OP`, `--disable=FAMILY:OP` | Switch a single instruction on or off |

## The launcher

| Option | Meaning |
|---|---|
| `--launcher=dialog` | Ask for the game with the system's folder dialog instead |
| `--preview=none` | Do not make title pictures of games that were never played |

## Information

| Option | Prints |
|---|---|
| `--version` | The version |
| `--help` | The options |
| `--detect` | How the game is identified |
| `--list-engines` | The engine profiles |
| `--list-missing` | The instructions of the game's profile that OpenBGI does not implement |
| `--list=ARCHIVE` | The entries of an `.arc` archive |
| `--extract=ARCHIVE` | Writes an archive's entries into a folder (`--out=DIR`, `--entry=NAME`, `--raw`, `--convert`) |
| `--debug`, `--debug=pause` | Opens the debugger with the game |

## Tools

`openbgi COMMAND` runs one of the modding tools: `dump`, `pack`, `scn`,
`asm`, `bpc`. `openbgi help COMMAND` prints a command's arguments. They
are described in the modding documents of the source repository
(`docs/modding/`).

A game folder named like a command is given as `./NAME`.

## The game's own arguments

Arguments after GAME go to the game as they would to the original
program, for example `"Do not use mutex."` to run a second copy from
the same folder.

## Environment

| Variable | Meaning |
|---|---|
| `BGI_LANG` | `en` or `ja`, as `--lang=` |
| `BGI_GAMES` | An `openbgi.json` to use, as `--games=` |
| `BGI_ENGINE` | An engine profile, as `--engine=` |
| `BGI_DATA` | Another place for `games/` ([files.md](files.md)) |
| `BGI_DPI` | The desktop's scale in dots per inch, when OpenBGI reads it wrongly: `96` is 100 %, `144` is 150 %, `192` is 200 %. On Windows also `off`: OpenBGI leaves the scaling to Windows, which enlarges the whole window as a picture |
| `BGI_GL` | Linux and Windows: `off` never shows the picture through OpenGL (the processor scales it, and Windows switches the display's mode for full screen); `force` uses OpenGL even when software draws it; `old` also takes the path of OpenGL 3.0 on a newer one, for trying it |
| `BGI_FONT_GOTHIC`, `BGI_FONT_MINCHO`, `BGI_FONT` | A font file to use for the games' Gothic or Mincho text (Linux, macOS, Haiku) |
