# File formats

The engine's own formats, as OpenBGI reads and writes them. All
numbers are little-endian; text is Shift-JIS (code page 932) unless a
game switches to UTF-8.

| Page | Formats |
|---|---|
| [archives.md](archives.md) | `PackFile` and `BURIKO ARC20` archives; DSC compression; the scrambling generator; SDC and DCFS save data; the global database; how a file is found |
| [pictures.md](pictures.md) | Raw pictures; CompressedBG versions 1 and 2 |
| [sound.md](sound.md) | BW sound files and their codecs |
| [programs.md](programs.md) | `._bp` programs: the bytecode files |
| [scenario.md](scenario.md) | Scenario files: the container, the token stream, the 16-bit scenes |
| [bfmovie.md](bfmovie.md) | BF_Movie frame sequences |

To look at real files:

    openbgi --list=ARCHIVE                     # every entry and what it is
    openbgi --extract=ARCHIVE --out=DIR --raw  # entries as stored
    openbgi --extract=ARCHIVE --out=DIR        # with the DSC layer removed
    openbgi dump GAMEDIR OUTDIR                # everything, converted

Not documented here: the save files a game writes through its scripts
(their content is the game's), and the layout of the engine's memory
areas that the system programs address by number.
