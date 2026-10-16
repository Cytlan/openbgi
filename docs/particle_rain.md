# The particle engine and the rain generator

What `src/gfx/particle/*.c` (`engine.c` the simulation and the automatic
redraw list, `types.c` the two particle kinds, `patterns.c` the pattern
tables and the pre-scaled bitmaps, `wind.c` the wind) and `src/gfx/rain.c`
do, as implemented.  The screen objects that own them - the particle
screen in `src/gfx/particle_screen.c` and the rain screen in
`src/gfx/rain_screen.c` - and the "C0 xx" instructions that drive them
(`src/vm/ops_ext1.c`) are described in those files' comments; this file
is about the simulations themselves.

## The particle engine

### Data model

One engine per particle screen (`ParticleEngine_t`): 4096 particle slots, per (type, pattern) the maximum count, the
live count, the birth interval and the time of the next birth, a camera
(position in 24.8, the sines and cosines of three angles as doubles, a
projection distance) and a wind generator.  The tick length `interval` is
10 ms by default; 0 pauses the engine.

Particles are polymorphic (a four-entry vtable: destroy, bitmap, level,
step) with a common base: type, pattern, alive flag, x / y / z in 24.8,
the wind weight, the effect mode they are drawn with and a fixed
transparency level.

* **Type 0 (ParticleA)**: a velocity vector.  It moves by that
  vector every tick and lives until it leaves the world.  Drawn with
  effect 0x20 at level 0.
* **Type 1 (ParticleB)**: a life in ticks, an age, two velocity
  keys per axis, a "stretch" length and position, fade-in and fade-out
  tick counts.  Every tick it moves by the velocity interpolated linearly
  between the two keys at its position in the stretch; at the end of a
  stretch the second key becomes the first, a new second key is drawn, a
  new stretch length is drawn.  The level (transparency) is
  `0x100 - 256 * age / fadeIn` while fading in and `0x100 - 256 * left /
  fadeOut` while fading out, 0 in between; its effect mode comes from the
  pattern.

The eight **patterns** per type are global tables shared by every screen
("C0 25" / "C0 2D" set them, "C0 24" / "C0 2C" bind a bitmap).  A pattern
holds the spawn box (an x half-width, a fixed start y, a z half-width),
the velocity base and variance per axis, the wind weight, and for type 1
the life, the stretch, the fades and the effect mode.  Script values
arrive in 16.16 and are stored shifted right by 8 (24.8); spreads and
variances are stored as absolute values.

Binding a bitmap makes **32 pre-scaled copies** (`Pattern_SetBitmap` in
`patterns.c`): scale 32/32 down to 1/32, each `Bmp_Alloc`'d at `(w * s) >>
16` by `(h * s) >> 16` and filled with `Blit_Scale` (bilinear).  The
source must be RGB32 or ARGB32.  These copies stand in for perspective
scaling at render time.

From 1.494 "C0 18" replaces a pattern's picture by an **animation** of up
to 32 frames (each scaled the same way), every particle advancing its
frame by a rate drawn at birth from the two 16.16 rates the control mode
of "C0 1F" derives from the instruction's numbers.  From 1.640 a type 1
pattern also has an **alternate image** ("C0 1A"): a second animation
cross-faded into the picture by a level of 0 .. 0x100 ("C0 1B" changes
the level; 0 shows the picture alone, 0x100 the alternate frame alone),
which `prtclctrl._bp` uses to fade a new animation in before it makes it
the pattern's animation.  The two are mixed per particle at draw time
(`Pattern_MixAlt`: both frames centred in a bitmap of the larger size,
`Blit_CrossfadeTo`).

### Randomness

Every draw uses `Rnd(n) = ((rand() << 15) | rand()) % |n + 1|` (`Ptcl_Rnd`
in `patterns.c`), with the engine's own generator (`BGI_Rand`, seeded by
"80 00").  The order of the draws is fixed and matters for reproducing a
scene:

* type 0 birth: x, z, vx, vy, vz
* type 1 birth: life, x, z, first key (x, y, z), second key (x, y, z),
  stretch
* type 1 end of stretch: new key (x, y, z), stretch
* wind: hold length, target vector (x, y, z); glide length

A start position is `Rnd(2 * spread) - spread`, a velocity `base - var +
Rnd(2 * var)`, a life or stretch `base + Rnd(var)`.  (`Rnd(-1)` divides
by zero in the original; here it yields 0.)

### The wind

`Wind_t` (`wind.c`) alternates two phases: **hold** the current vector
for `holdT + Rnd(holdVarT)` ticks, then **glide** linearly to a freshly
drawn target over `moveT + Rnd(moveVarT)` ticks; the times are given in
ms by "C0 2x" and converted to ticks with the engine's interval (a zero
interval disables the wind).  The vector of the current tick is added to
every particle's position scaled by its wind weight (`pos += wind *
weight >> 8`, `Particle_ApplyForce`) before the particle's own step.

### Ticking and births

`ParticleEngine_Update` runs the simulation up to the current
time: while `lastTick <= now` it steps the wind, applies it to every
particle, steps every particle and removes those that are dead or outside
the world (|x| <= 16000, 0 <= y <= 16000, |z| <= 8000, in whole units),
advancing `lastTick` by the interval each time.  If the engine fell more
than 500 ms behind it does not catch up: the clock restarts at `now` and
every group's birth schedule is reset.

Births follow per (type, pattern): while `nextSpawn <= now` and the group
is below its maximum, one particle is born and `nextSpawn` moves on by
the group's interval; a full group goes idle (`nextSpawn = 0`) until a
death makes room, at which point the next update restarts its schedule
from `now`.  Births beyond 4096 total are destroyed at once (the original
leaks them).

`ParticleEngine_Prerun(ms)` ("C0 0x" pre-roll) advances the simulation one
millisecond at a time `ms` times and then puts the clock and the birth
schedule back exactly as they were, so the screen starts populated.

### Rendering

`ParticleEngine_Render` draws into the screen's
surface at the centre (cx, cy):

1. every live particle is moved into camera space: subtract the camera
   position, rotate about x by A, about y by B, about z by C, truncating
   to integer after each rotation (double arithmetic on the 24.8 values);
   particles with z < 0 are behind the camera and skipped;
2. the rest are inserted into a list sorted by decreasing z (equal depths
   keep slot order), so far particles are drawn first;
3. for each, the pre-scaled copy index is `32 - 32 * dist / ((z >> 8) +
   dist)` (0 = full size; nothing is drawn past 1/32), the screen position
   is `cx + x * dist / ((dist << 8) + z) - w / 2` and `cy - y * dist /
   ((dist << 8) + z) - h / 2` with 32-bit products as in the original, and
   the copy is blitted with the particle's effect mode and level
   (`Bmp_Blit`, clipped);
4. the rectangle each blit touched, clipped to the surface, is appended to
   the caller's list and the count returned.

### The automatic redraw list

"C0 09" registers a particle screen for periodic redraws (0 unregisters).
Each pass `Ptcl_AutoTick` redraws every registered screen whose
time has come, if it is visible and not below the draw limit, drops
screens that no longer exist, moves the next time to the first grid point
after now, and requests a dirty present when anything was redrawn.

## The rain generator

### Data model

`RainGen_t`: a doubly linked list of drops, the parameters
(`RainParams_t`: the spawn box, the fall direction, the distance per
stage and the streak length in 24.8, the streak colour and the clear
colour, drops per stage, milliseconds per stage, a timer selector), the
view (`RainView_t`: camera position, three angles in tenths of a degree,
the projection distance that doubles as the near plane), the derived
per-stage motion `step = dir * (fall >> 8)` and the streak vector
`streak = dir * (length >> 8)`, and the sines and cosines of the angles
scaled by 256.  A drop is a segment: `head` is the spawned point, `tail`
= head - streak.

The defaults of a new generator are a +-100 box, direction (0, -1, 0),
a fall of 60, a length of 350, white, 100 drops per stage; the rain
screen overwrites all of them from the script's parameters.

### Stages

`RainGen_Update` turns the elapsed time into whole stages: if
500 ms or more passed since the last update (a stall, or a pre-roll that
long) it only resynchronises the clock; otherwise it runs one stage per
`stageMs` elapsed and keeps the remainder.  A stage first moves
every drop by `step` and removes the ones whose tail is at or below
`yBottom`, then spawns `perStage` drops: head at a random x in [xMin,
xMax), y = `yTop - rand() % 100`, z in [zMin, zMax), tail = head - streak.
`RainGen_Start(prerollMs)` sets the clock back by the pre-roll and empties
the list, so the next update runs the first `prerollMs / stageMs` stages
at once (as long as that is under 500 ms).  The random draws use the
engine's generator in the order x, y, z per drop.

### Projection and drawing

`RainGen_Render` clears the 32-bit surface to the clear colour
and draws every drop:

* both ends go through `RainGen_ToView`: subtract the camera,
  rotate about z by C - with the quirk that the second line of the
  rotation uses the already rotated x (`y1 = (sinC * x1 + cosC * y) >> 8`)
  - then about x by B, then about y by A, in integer arithmetic with the
  x 256 sines;
* a drop with either end at or before the near plane (`z <= dist`) is
  skipped;
* the ends are projected as `dist * x / z`, `dist * y / z` and placed
  around the surface centre (y flipped);
* the colour's alpha is attenuated by the tail's depth: `alpha * dist /
  (z / 8)`, never above the configured alpha (the original divides by
  zero for z < 8; here the alpha is kept);
* the segment is drawn by the line rasteriser.

### The line rasteriser

`LineRenderer_t` (the top of `rain.c`) draws one-pixel-wide
lines of a 32-bit colour into a 32-bit surface with an inclusive clip
rectangle, stepping along the major axis with a 20.12 DDA.  It is kept
exactly as the original, including the asymmetries of its clipping:

* the trivial reject compares both ends with the clip box, then the line
  is ordered so that y increases;
* a wide line (dx > dy) steps x one pixel at a time; when it starts left
  of (or right of) the clip it extrapolates y to x = 0 (or to the right
  edge) rather than to the clip's left edge, skips rows above the top,
  and stops when the next row passes the bottom - which means the last
  clip row is never drawn by a wide line;
* a steep line steps y one pixel at a time; its left-going branch stops
  at x < 0 instead of at the clip's left edge, and the right-going branch
  compares the fixed-point x with the plain left edge while skipping.

### The frame driver

`Rain_FrameTick` runs every pass when the effects are enabled
("C0 4F"): when the next effect frame is due it asks the graphics manager
to redraw the rain screens (`Gfx_RedrawRains`, which redraws the first
screen that can be redrawn per frame and reports whether it did), requests
a dirty present when one was redrawn, and moves the next frame to the
first grid point after now.  The frame interval is `1000 / fps` ms, 50 ms
(20 fps) by default; "C0 4F" also resets the schedule and requests a full
present.  `Rain_UpdateAll` / `Ptcl_UpdateAll` run the simulations of every
screen once per pass through the graphics manager.
