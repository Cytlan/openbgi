# The libretro core: building and internals

For players: [../manual/libretro.md](../manual/libretro.md).

The core is the same engine behind another host,
`src/host/libretro/core.c`. It is one file with the fonts and the
movie decoder in it.

## Building

    make ffmpeg                                  # once per platform and architecture
    make libretro                                # bin/linux/libretro-x86_64/
    make libretro ARCH=x86
    make libretro PLATFORM=win32 ARCH=x86_64     # bin/windows/libretro-x86_64/
    make libretro PLATFORM=android ARCH=arm64 ANDROID_NDK=... ANDROID_DEPS=...

| Platform | Linked in | From the system |
|---|---|---|
| Linux | FFmpeg | `libfreetype.so.6`, `libvorbisfile.so.3` |
| Windows | FFmpeg, FreeType, Vorbis (`MINGW_DEPS`) | - |
| Android | FFmpeg, FreeType, Vorbis (`ANDROID_DEPS`) | - |

The Windows core draws text with FreeType, not GDI.

`make check-libretro` checks that the core exports the functions of
`libretro.h` and nothing else, needs no FFmpeg library at run time,
and plays a small game with `tools/retrofront.c`: loaded twice in one
process, with keyboard, mouse, pad and touch, on canvases of several
sizes, with options changed mid-run and a patch.

## How it runs

- One call of the frontend is one picture at 60 Hz. The core moves its
  clock on by a sixtieth of a second and runs the engine until it has
  caught up. Fast forward, slow motion and pause follow.
- Sound: 44100 Hz stereo, 735 frames per picture.
- Only what changed in the game's picture is scaled again
  (`src/host/os_present.c`). A call in which nothing changed hands the
  frontend no new picture.
- When a game ends, the core asks the frontend to close it. Restart
  starts the game in a new engine.
- The core takes the OS layer's file table and movie decoder and
  nothing else: no window, no dialogs, no registry, no shell. It does
  not change the frontend's current directory, starts no program and
  opens no link.

## The canvas

A frontend scales a core's picture as it likes and does not say how
large its window is. The core therefore hands over a canvas of the
display's size (the frontend's reported size, else 1920 x 1080, or the
"Screen size" option) with the game's picture centred on black, scaled
by the largest step that fits. With "the game's own" it hands over the
game's picture unscaled; that is the default on Android, where the
frontend's graphics processor scales better than the core's processor.

## Options and input settings

The pad, pointer and touch options lie over the `"input"` object of
`openbgi.json` ([input.md](input.md)): a value other than the default
wins. Start confirms by default here, where it cancels in the other
hosts.

## Not there

Save states; the frontend's VFS; a second pad; rumble; disc images
other than plain `.iso`.
