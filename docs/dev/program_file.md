# The engine's own program file ("81 3F")

`81 3F` (1.492) gives a program the path of the executable the engine
runs in. Kira Kira Curtain Call (English edition) reads that file,
sums every fifth byte, and each story file compares the sum with a
number it carries; on a mismatch the story file ends.

OpenBGI is not that executable, and the executable beside a game
cannot be relied on. The engine cannot know the expected number, but
the computation runs in its own VM, where it can be followed. This is
an integrity check, not copy protection.

## What the engine does

1. **A file of its own.** From the first `81 3F` on, the game's
   programs see a file `BGI.exe` in the game's folder. The file layer
   serves it from memory, before any real file of that name. It has
   1493 bytes (`PROGRAM_IMAGE_SIZE`) that mean nothing and are the same
   for every game (`Program_Image`, `src/fs/paths.c`).
2. **Marks.** What a program gets from that file is marked: the bytes
   that `80 30`, `80 31` or `81 30` load, the size from `80 35` or
   `81 35`, a digest from `80 E9`.
3. **Marks travel** with the value: through arithmetic and bit
   operations (`20` .. `2B`), into and out of a 32-bit variable, with a
   copy of marked bytes (`60`), and with a value read through a marked
   pointer.
4. **Equality.** `30` and `31` find a marked value equal to an unmarked
   one. `63` finds marked bytes equal to unmarked ones.

Left as they are:

- A marked value compared with 0 or -1 (a program's test for "was
  there a file").
- Two marked values, and ordered comparisons (`32` .. `35`).

Not answered: a check that does not end in one of those comparisons (a
value used as a key, a range test), and bytes read with the file
accessor (`81 28` ..).

## How marks are kept

| Kind | Where | Holds while |
|---|---|---|
| Stack value | A mark byte beside the value (`Thread_t.stackMark`) | Until the value is popped; a plain push clears it |
| Cell | A 32-bit variable a marked value was stored in, by address | A load there reads the value that was stored |
| Run | A stretch of bytes a loader instruction wrote, by address | Every byte is still what was written |

Because memory marks are validated against content, what an engine
service writes there later is never taken for marked. Every place that
frees or moves script memory calls `Follow_Released` (`80 21`, the end
of a thread, a growing local heap, resizing the global memory).

## Across runs

A game's saves hold its global memory, so a marked value there is
saved as a plain number. A run that loads the save would compare that
number unmarked.

- `Follow_Keep` writes a note, `.openbgi/follow.txt` in the game's
  folder (its folder in the user's data when the game's own cannot be
  written): one line `global <offset> <value>` in hexadecimal for every
  variable of the global memory that holds a marked value. It runs
  when the global memory is written out (`80 78`, `80 32`) and when the
  run ends.
- `Follow_Start` reads the note at start. A load of such a variable
  that reads the noted value is marked.
- Without the note, a run that loads a save fails the check at the next
  story file unless a choice came first (the game recomputes the sum at
  choices).

A save made by the original engine holds the number the game expects
and needs none of this.

## Cost

Until a program asks for the file, the instructions test one counter
(`o->vm.follow.live`). The mark byte beside the stack costs about 6 %
on a loop of nothing but loads, arithmetic, stores and comparisons.

## Code

| File | Content |
|---|---|
| `src/include/bgi/follow.h`, `src/vm/follow.c` | Cells, runs, the note |
| `src/vm/ops_base.c` | `BINOP_FOLLOW`, `BINOP_EQUAL`, loads and stores |
| `src/vm/ops_sys.c`, `ops_sys81.c` | The loader instructions, `Vm_FollowRange`, `81 3F` |
| `src/fs/paths.c`, `src/fs/fs.c` | The file |
| `src/core/dynmem.c`, `src/vm/thread.c`, `src/fs/savefile.c` | Where memory goes and where it is saved |
| `tests/vm.c`, group "program file" | Tests |
