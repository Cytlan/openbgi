# Frame sequences and video textures: the instructions

The engine's side of the BF_Movie frame sequences (1.69/472 on). The
file format: [formats/bfmovie.md](formats/bfmovie.md).

Code: `src/gfx/bmseq.c` (the registry), `src/vm/ops_gfx0.c`,
`ops_gfx1.c`, `ops_gfx2.c` (the handlers), `src/wait/wait_load.c` (the
loader waits).

## Frame sequences

Loaded movies are records on a list, numbered from a global counter. A
record may have one clone that shares its data under a second number.

| Opcode | Arguments -> result | Does |
|---|---|---|
| `90 F4` | `&no, &info, name, arc` | Loads the file through the loader. `info[5]` receives width, height, pixel mode, frame rate, frame count. The wait pushes 0 OK, 1, or 2 not a movie. |
| `90 F5` | `no -> r` | Release: 0 OK, 3 no such number |
| `90 F6` | `bmp, no, frame -> r` | Decodes the frame into the slot: 0 OK, 3 no such number, 4 no such frame, 5 the slot's size or pixel mode differs |
| `90 F7` | `no, &out -> r` | A clone under a new number: 0 OK, 3, 7 already cloned |
| `80 53` | | The next `90 F6` runs inside a wait object, which pushes the result: 0, 3, 5, 8 decode failed |
| `92 F1` | `&no, &info, name, arc, streamed` | `90 F4` with a flag. Flag 0: as `90 F4`. Flag set: the file must be a streamed movie (type 0x10000 or 0x10001); only its header, quantisation tables and frame offsets are read, and `90 F6` reads each frame from the file. The wait pushes 0 OK, 1 no name, 2 not a streamed movie. |
| `92 F0` | `arc, name, x, y, w, h -> length` | A movie in a common format played from an archive entry |

The scripts drive playback themselves: they call `80 53` + `90 F6` for
each frame on a timer, then invalidate the sprite that shows the slot.
A decode writes the slot's pixels and tells nobody.

## Video textures

A bitmap slot can carry a player that decodes a streamed BF_Movie into
the slot by itself (`92 F2`: name, arc, a, b, bmp; or `91 F0`).

OpenBGI has no such player. Every attach answers 1 ("the player could
not be made") and the other calls answer as for a slot without a
texture, which is what the scripts expect of a machine that cannot play
video; they fall back to still pictures.

| Opcode | Arguments -> result | |
|---|---|---|
| `91 F1` | `bmp, &state -> r` | The player's state |
| `91 F2`, `91 F6` | `bmp -> r` | Release |
| `91 F3`, `92 F4` | `bmp, v -> r` | Control |
| `91 F4` | `bmp, &p, a, b, c -> r` | Control |
| `91 F5` | `bmp -> r` | Advance |
| `91 F7` | `bmp, &frame -> r` | The slot's last frame number |

All answer 4 for a slot without a bitmap and 1 for one without a
texture.

## Related bitmap instructions (1.494 on)

| Opcode | Arguments | Does |
|---|---|---|
| `91 16` | `slot, periodX, phaseX, ampX, periodY, phaseY, ampY` | A sine-wave vector field (`Vec_Sine`, `src/gfx/vecmaps.c`): the x displacement of row y is `16 sin(2 pi (y + phaseY) / periodY) ampY`, and likewise for columns |
| `91 1D` | `dst, src, mode, colour, level` | `src` into `dst` through a colour mode (`BmpOp_ColourMode`, `src/gfx/bmpops.c`): 0 copy, 1 blend toward `src XOR colour`, 2 luminance tint, 3 blend toward the colour keeping alpha, 4 add `colour * level / 256` saturating. `level` is 0 .. 0x100. |
| `91 1B` | `dst, src, cx, cy, level` | The "gathering add" (`Blit_GatherAdd`, `src/gfx/blit_scale.c`): every pixel of `src` is drawn toward the centre by the 16.16 rates `cx`, `cy` and added to `dst` with saturation |
