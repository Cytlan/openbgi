# Modding and translating

How to take a game apart, change it, and hand the change out. For games
of the BGI/Ethornell engine, with the tools that are part of `openbgi`.

## The workflow

1. Dump the game: `openbgi dump GAMEDIR game.dump`. Every archive
   becomes a folder of editable files: scripts as source text, pictures
   as PNG, sounds as Ogg Vorbis, tables as JSON.
2. Edit the files.
3. Pack: `openbgi pack --changed game.dump out`.
4. Zip `out` as a patch and play: `openbgi --patch=patch.zip GAMEDIR`.

Nothing writes into the game's folder at any step.

## Pages

| Page | What it covers |
|---|---|
| [dump-pack.md](dump-pack.md) | `openbgi dump` and `openbgi pack`: every asset of a game |
| [scenario.md](scenario.md) | The story scripts and `openbgi scn`; translating |
| [programs.md](programs.md) | The system programs: `openbgi bpc` (source) and `openbgi asm` (listings) |
| [patches.md](patches.md) | Making and using patch zips |
| [obgi.md](obgi.md) | OpenBGI's own instructions: UTF-8, right-to-left text, word wrap, scrolling messages, patterns |
| [launcher.md](launcher.md) | Changing OpenBGI's launcher |
| [debugger.md](debugger.md) | Watching and stepping a game's scripts |

## The tools

    openbgi dump [options] GAMEDIR OUTDIR
    openbgi pack [options] DUMPDIR OUTDIR
    openbgi scn ...
    openbgi asm ...
    openbgi bpc ...
    openbgi help [COMMAND]

- `openbgi COMMAND --help` prints a command's arguments.
- On Windows, type `openbgi` in a console: that runs `openbgi.com`,
  which shows its output there.
- Looking into one archive without dumping: `openbgi --list=ARCHIVE`,
  `openbgi --extract=ARCHIVE --out=DIR` (`--raw`: as stored;
  `--convert`: pictures as `.bmp`, sounds as `.wav`).
- A game folder named like a command is given as `./NAME`.

## Text encoding

The games store text as Shift-JIS (CP932). Sources are UTF-8; the tools
convert. A language outside CP932 needs the game switched to UTF-8,
which works on OpenBGI for every engine version: [obgi.md](obgi.md).

## Reference

File formats and the instruction set are described for developers in
`docs/dev/formats/` and `docs/dev/versions.md`.
