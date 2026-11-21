# Patches

A patch is a zip archive, or a folder, of files that are laid over a
game while it runs. The game's own files are not changed. Use a patch
to hand out a translation or a fix.

## What goes into a patch

The root of the patch is the game's folder.

| Put in the patch | Effect |
|---|---|
| A file with the name of one of the game's files (`data01000.arc`, `sysprg.arc`, `movie.mpg`) | It is read instead of the game's file |
| A file with the name of an archive entry (`title._bp`, `_01`), in the engine's own format | It is read instead of that entry: a loose file wins over an archive |
| A new file | It is there as if it were in the game's folder |

- Names match without regard to upper and lower case.
- A zip that holds everything inside one top folder is read from inside
  that folder.
- Whatever a translation's instructions say to "copy into the game's
  folder" works as a patch when zipped as it is.

## Making one

From a dump ([dump-pack.md](dump-pack.md)):

    openbgi pack --changed game.dump out
    cd out && zip -0 -r ../translation.zip .

`--changed` writes only the archives in which something changed.

From single compiled files ([scenario.md](scenario.md),
[programs.md](programs.md)):

    openbgi scn -c -s sysprg.arc _01.bsc -o patch/_01
    openbgi bpc title.bpc -o patch/title._bp
    cd patch && zip -r ../translation.zip .

Store large files without compression (`zip -0`, or
`zip -n .arc:.ogg:.mpg`): OpenBGI then reads them in place. A
compressed file is unpacked into memory when opened, and one over 32 MB
into a cache folder first.

## Using one

| Where | How |
|---|---|
| Launcher | **Edit game**, **Add patch**, **Save** |
| Command line | `openbgi --patch=translation.zip GAME`; repeat `--patch=` for several |
| libretro core | Put the zip into the game's `patches` folder and choose it in the core options |

- A later patch wins over an earlier one.
- With a patch, the game's folder is not written into: saves go to the
  data folder (`games/<name>/`), and a file there wins over the patch.

## What a patch cannot do

- Run on the original engine. Copy the files into a copy of the game
  for that.
- Use OpenBGI's own instructions ([obgi.md](obgi.md)) and still run on
  the original engine.
