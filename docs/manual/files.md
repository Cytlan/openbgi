# Where files are kept

What OpenBGI writes, and where.

## Your data folder

| System | Folder |
|---|---|
| Windows | `%APPDATA%\OpenBGI\` |
| Linux | `~/.local/share/openbgi/` (or `$XDG_DATA_HOME/openbgi/`) |
| macOS | `~/Library/Application Support/OpenBGI/` |
| Haiku | `~/config/settings/OpenBGI/` |

| In it | What it is |
|---|---|
| `openbgi.json` | OpenBGI's settings: the launcher's folders and games, pad settings, each game's display settings, the engine profiles |
| `games/<name>/` | What a game wrote that could not go into its own folder (see below) |
| `pictures/`, `icons/` | The launcher's pictures and icons of your games |
| `launcher/` | The launcher's own program as source files; OpenBGI writes the ones that are missing |

## Saves and a game's settings

| How the game is played | Where its saves go |
|---|---|
| From its folder, and the folder can be written | Into the game's folder, exactly as the original program does |
| From a `.zip` or an `.iso` | `games/<name of the archive>/` in your data folder |
| From a folder that cannot be written, with `--overlay`, or with a patch | `games/<name of the folder>/` in your data folder |

- A file in `games/<name>/` wins over the game's file of the same name.
- OpenBGI may add a folder `.openbgi/` beside the saves (a cache, and
  notes it needs to load saves correctly). Keep it with the saves.
- `BGI_DATA=<folder>` in the environment puts `games/` somewhere else.

The original program keeps a few settings (window mode and the like) in
the Windows registry. OpenBGI does the same on Windows. Elsewhere they
are in a text file:

| System | File |
|---|---|
| Linux | `~/.config/bgi/registry.txt` |
| macOS | `~/Library/Application Support/bgi/registry.txt` |
| Haiku | `~/config/settings/bgi/registry.txt` |

## openbgi.json

OpenBGI uses the first `openbgi.json` it finds:

1. the file named by `--games=FILE` or `BGI_GAMES`
2. the game's own folder
3. the folder of the OpenBGI program (a portable copy)
4. your data folder

A new file is written to your data folder. A file that cannot be
written is skipped, unless it contains `"staticConfig": true` at its
top level: then it is used as it is and nothing is ever saved.

To make a portable copy on Windows: unpack the zip, start OpenBGI once,
and move `openbgi.json` from `%APPDATA%\OpenBGI\` beside `openbgi.exe`.

## Error file

When a game's script fails, OpenBGI shows the message and writes it to
`BGIError.txt` where the game's saves go. Attach that file when you
report a problem ([help.md](help.md)).
