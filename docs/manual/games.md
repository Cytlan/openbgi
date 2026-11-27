# Games and the launcher

How OpenBGI finds your games and starts them.

## What a game is

| You have | OpenBGI plays |
|---|---|
| The game's installed folder (it holds `system.arc`, `sysprg.arc`, ...) | the folder |
| A `.zip` archive of that folder | the archive, without unpacking it |
| An `.iso` image of the game's disc | the image, without mounting it |

- A `.zip` or `.iso` is only read. What the game writes goes to your
  data folder ([files.md](files.md)).
- A folder that can be written gets the game's saves and settings, as
  with the original program. A folder that cannot be written (a disc, a
  share) is played like an archive.
- Disc images must be plain `.iso` files. Other image formats
  (`.mdf`, `.bin`/`.cue`, ...) are not read.
- In a `.zip`, store large files (`.arc`, `.ogg`, movies) without
  compression (`zip -0`). Compressed files over 32 MB are unpacked once
  into a cache before the game starts.

## The launcher

Start OpenBGI without a game and the launcher opens.

Adding games:

1. Click **Folders**.
2. Click **Add folder**.
3. Go to the folder that holds your games and click **Add this folder**.
   A `.zip` or `.iso` in the list can be added by itself.
4. Click **Back**.

OpenBGI lists every game directly in an added folder and one level
below it: folders of games, `.zip` and `.iso` files. A game that was
never played gets a picture of its title after a few seconds.

| Button | What it does |
|---|---|
| **Start game** | Starts the selected game. When the game ends, the list is back. |
| **Folders** | Adds and removes the places that are searched. |
| **Edit game** | Sets the name shown, the compatibility (engine profile), extra options and patches; **Remove game** hides the game. |
| **Sort: ...** | Orders the list by name or by last played. |
| **Look: ...** | Changes the background. |

Press F5 to search the folders again. The window can be resized.

While a game runs, its window shows the game's own icon when OpenBGI
finds one in the game's folder.

## Drag and drop

On Windows and Linux, drop games on the launcher's window from a file
manager, one or several at a time:

| You drop | Result |
|---|---|
| A folder | It is added and searched like any other place. |
| A `.zip` or `.iso` with a game in it | It is added as a game. |
| An `.arc` file of a game | The game's folder is added. |
| Anything else | Nothing is added; a message says so. |

The dropped game is selected. A game you removed comes back when you
drop it again.

## Without the launcher

- `openbgi GAME` starts a game directly; GAME is a folder, a `.zip`, an
  `.iso`, or any file in the game's folder
  ([command-line.md](command-line.md)).
- On Linux, drop a folder, `.zip` or `.iso` on OpenBGI's desktop entry,
  or use **Open with**.
- On Windows, drop it on `openbgi.exe`.

## Patches and translations

A patch is a zip archive (or a folder) of files that replace the game's
files of the same name: a translation, a fix. The game itself is not
changed.

1. Select the game and click **Edit game**.
2. Click **Add patch** and choose the patch. Add several if needed; a
   later one wins over an earlier one.
3. Click **Save**.

From a command line: `openbgi --patch=translation.zip GAME`.

A patch whose instructions say "copy these files into the game's
folder" works as a patch once you zip those files.

## Things you may meet

| What happens | What to do |
|---|---|
| The game asks for its disc or says it is not registered | **Edit game**, **Options**: add `--no-disc-check`. |
| The game is listed but does not start, or stops with an error | See [help.md](help.md). |
| An opening movie is skipped | Its format is not one OpenBGI decodes; the game goes on. |
| The game wants to start another program (a setup tool, a web updater) | OpenBGI starts no programs. Web links and documents are opened. |
| Text is in the wrong language | OpenBGI's own messages follow your system: English, or Japanese on a Japanese system. `--lang=en` or `--lang=ja` overrides. The game's text is the game's. |
