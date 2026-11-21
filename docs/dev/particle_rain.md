# The particle engine and the rain generator

The simulations behind the "C0 xx" instructions. The screen objects
that own them (`src/gfx/particle_screen.c`, `src/gfx/rain_screen.c`)
and the handlers (`src/vm/ops_ext1.c`) are described in their own
comments.

Code: `src/gfx/particle/` (`engine.c`, `types.c`, `patterns.c`,
`wind.c`), `src/gfx/rain.c`.

## Particles

One engine per particle screen (`ParticleEngine_t`): 4096 particle
slots; per (type, pattern) a maximum count, a live count and a birth
interval; a camera; a wind generator. The tick is 10 ms by default; 0
pauses the engine. Positions are 24.8 fixed point.

| Type | State | Motion | Drawing |
|---|---|---|---|
| 0 | A velocity vector | Moves by it every tick; lives until it leaves the world | Effect 0x20, level 0 |
| 1 | A life in ticks, two velocity keys per axis, a "stretch", fade-in and fade-out | Velocity interpolated between the keys over the stretch; at its end a new key and a new stretch are drawn | The level fades in and out with age |

**Patterns.** Eight per type, shared by every screen ("C0 25" /
"C0 2D" set them, "C0 24" / "C0 2C" bind a bitmap). A pattern holds
the spawn box, the velocity base and variance per axis, the wind
weight, and for type 1 the life, stretch, fades and effect mode.
Script values arrive as 16.16 and are stored as 24.8.

Binding a bitmap makes 32 pre-scaled copies (32/32 down to 1/32,
bilinear), which stand in for perspective scaling when drawing. The
source must be RGB32 or ARGB32.

- From 1.494, "C0 18" gives a pattern an animation of up to 32 frames;
  each particle advances its frame at a rate drawn at birth.
- From 1.636, "C0 1A" gives a type 1 pattern an alternate image,
  cross-faded by a level 0 .. 0x100 ("C0 1B").

**Randomness.** `Rnd(n) = ((rand() << 15) | rand()) % |n + 1|` on the
engine's own generator (seeded by "80 00"). The order of the draws is
fixed and matters for reproducing a scene:

| Event | Draws, in order |
|---|---|
| Type 0 birth | x, z, vx, vy, vz |
| Type 1 birth | life, x, z, first key (x, y, z), second key (x, y, z), stretch |
| Type 1 end of stretch | new key (x, y, z), stretch |
| Wind | hold length, target vector (x, y, z); glide length |

A start position is `Rnd(2 * spread) - spread`, a velocity
`base - var + Rnd(2 * var)`, a life or stretch `base + Rnd(var)`.
`Rnd(-1)` divides by zero in the original; here it yields 0.

**Wind** (`wind.c`). Two alternating phases: hold the current vector
for `holdT + Rnd(holdVarT)` ticks, then glide linearly to a new target
over `moveT + Rnd(moveVarT)` ticks. Each tick the vector is added to
every particle's position, scaled by its wind weight, before the
particle's own step.

**Ticking** (`ParticleEngine_Update`). While `lastTick <= now`: step
the wind, apply it, step every particle, remove those that are dead or
outside the world (|x| <= 16000, 0 <= y <= 16000, |z| <= 8000). If the
engine fell more than 500 ms behind, it does not catch up: the clock
restarts and every birth schedule is reset.

**Births.** Per (type, pattern): while `nextSpawn <= now` and the
group is below its maximum, one particle is born. A full group goes
idle until a death makes room. Births beyond 4096 in total are
destroyed at once (the original leaks them).

`ParticleEngine_Prerun(ms)` advances the simulation by `ms` and then
restores the clock, so that a screen starts populated.

**Rendering** (`ParticleEngine_Render`):

1. Move each live particle into camera space: subtract the camera,
   rotate about x, y, z, truncating to integer after each rotation.
   Particles with z < 0 are skipped.
2. Sort by decreasing z; equal depths keep slot order.
3. Pick the pre-scaled copy `32 - 32 * dist / ((z >> 8) + dist)` and
   project around the screen's centre, with 32-bit products as in the
   original.

"C0 09" registers a particle screen for periodic redraws
(`Ptcl_AutoTick`).

## Rain

`RainGen_t`: a list of drops, the parameters (spawn box, fall
direction, distance per stage and streak length in 24.8, streak and
clear colours, drops per stage, milliseconds per stage) and the view
(camera, three angles in tenths of a degree, the projection distance,
which is also the near plane). A drop is a segment from `head` to
`tail = head - streak`.

**Stages** (`RainGen_Update`). Elapsed time is turned into whole
stages. If 500 ms or more passed, only the clock is resynchronised. A
stage moves every drop by `step`, removes those whose tail is at or
below `yBottom`, then spawns `perStage` drops at random places in the
box. `RainGen_Start(prerollMs)` sets the clock back so that the first
update runs the pre-roll's stages at once.

**Drawing** (`RainGen_Render`). The surface is cleared, then for each
drop:

- Both ends go through `RainGen_ToView`: subtract the camera, rotate
  about z, x, y in integer arithmetic. The original's quirk is kept:
  the second line of the z rotation uses the already rotated x.
- A drop with either end at or before the near plane is skipped.
- The ends are projected as `dist * x / z`, `dist * y / z`.
- The colour's alpha is attenuated by the tail's depth.

**The line rasteriser** (`LineRenderer_t`). One-pixel lines with a
20.12 DDA and an inclusive clip rectangle. It reproduces the original
exactly, including the asymmetries of its clipping: a wide line never
draws the last clip row, and a steep line's left-going branch stops at
x < 0 instead of the clip's left edge.

**The frame driver** (`Rain_FrameTick`). Runs every pass when effects
are enabled ("C0 4F"). When the next effect frame is due it redraws
the rain screens and requests a present. The frame interval is
`1000 / fps` ms, 50 ms by default.
