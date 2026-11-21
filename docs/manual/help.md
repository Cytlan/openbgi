# Getting help

OpenBGI is a work in progress. Not every game works yet.

## First checks

1. Is the game listed as working? The table of games is in
   `README.md`, which comes with OpenBGI and is shown at
   <https://github.com/Cytlan/openbgi>.
2. Are you on the latest version? `openbgi --version`, and
   <https://github.com/Cytlan/openbgi/releases>.
3. Does the game ask for its disc? Add the option `--no-disc-check`
   ([games.md](games.md)).
4. Does the game have DRM (copy protection that needs a key, an online
   activation or the original disc in a drive)? OpenBGI does not
   support games with DRM and will not.

## Reporting a game that does not work

Report it on the issue tracker, <https://github.com/Cytlan/openbgi/issues>,
or on Discord, <https://discord.gg/3zFTJWUNt7>.

Include:

| What | How to get it |
|---|---|
| OpenBGI's version and your system | `openbgi --version`; for example "Windows 11, 64-bit" |
| The game: title, publisher, edition (disc, download, language) | |
| How OpenBGI identifies it | The output of `openbgi --detect GAME` |
| What happens, and when | "Stops with an error after the title", "no sound in the opening" |
| The error | The text of the message box, and `BGIError.txt` ([files.md](files.md)) |
| A picture, if something looks wrong | A screenshot |

On Windows, run `openbgi.com --detect GAME` in a console to see the
output.

Do not send the game's files, and do not post links to them.

## Questions

Ask on Discord: <https://discord.gg/3zFTJWUNt7>.

Website: <https://openbgi.net>
