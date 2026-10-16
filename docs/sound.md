# Sound: the Wave Master library and the BW container

Sources: `src/snd/wm/*.c` (the library: `wavemaster.c` its life cycle and
timer, `record.c` the per-channel records and fades, `bgm.c` the music
channels, `se.c` the effect slots, `voice.c` the voices and the software
mixer), `src/snd/bw/*.c` (the streams: `stream.c`, `source.c`, one file
per codec), `src/snd/sound.c` (the sound manager); headers
`inc/bgi/snd/wavemaster.h`, `inc/bgi/snd/bw.h`, `inc/bgi/sound.h`.  The
original's library is "WaveLib" (its error strings name
`WaveLib\src\DSSpeakerModel.cpp` and `DSStreamSpeaker.cpp`); its version
string is "2.0010DS8".

## Layers

    "A0 xx" handlers (src/vm/ops_snd.c)
        -> sound manager (src/snd/sound.c): master volumes, file search,
           retry prompts, archive names
            -> Wave Master API (Wm_*, src/snd/wm/): 16 BGM channels,
               64 effect slots, fades, the 20 ms timer, voices
                -> streams (src/snd/bw/): sources, the BW header, codecs
                    -> OS layer: files (OS_File*), audio output (OS_AudioStart)

The original mixes with DirectSound: each voice is a secondary buffer, an
effect's buffer holds the whole decoded sound, a music buffer is 5 x 100 ms
refilled by a thread from a 4-second ring that a "pump" thread fills from
the decoder.  The reimplementation keeps everything above the voice
unchanged and replaces the DirectSound part with a software mixer that
runs in the OS layer's audio thread at 44.1 kHz stereo (the original's
primary buffer format): effects play from their decoded samples, music is
pulled from its decoder through the same ring / restart logic as the pump
(`Voice_Pump` in `voice.c`).  The mixer has one external tap
(`Wm_SetExternalMix`): a callback that adds to the 32-bit accumulator
after the voices, which the ffmpeg movie player (`src/sys/moviefx.c`)
uses for a movie's sound track; `Wm_MixRunning` tells whether the mixer
is being pulled at all.

## Records and volumes

Every channel and slot has a record (`WmRecord_t` in `src/snd/wm/wm_internal.h`):

| field | note |
|-------|------|
| `active` | something is loaded |
| `voice` | the voice (the DirectSound wrapper object in the original) |
| `stream` | the decoder object |
| `master` | 0 .. 0x80, `Wm_*SetMasterVolume` |
| `aux` | 0x80, never written anywhere |
| `fadeVol` | fade of the channel volume, `Wm_BgmFadeVolume` |
| `volume` | 0 .. 0x80 (the play / open argument) |
| `fade` | fade of the fade level, `Wm_*FadeIn` / `FadeOut` |
| `fadeLevel` | 0x80 = full |

A fade is {active, start tick, end tick, from, to}; the 20 ms timer
(`Records_Tick`) steps every active fade with
`value = from + (to - from) * (now - start) / (end - start)` in floating
point (`Fade_Step`), ends it at the target, and re-applies the level.

The level (`Record_Level`) is an attenuation in dB:

    level = int((0x200 - fadeLevel - volume - aux - master) * 0.375)
    level >= 48  ->  0x80 (silence)

so every volume step is 0.375 dB and anything 48 dB down is cut.  The voice
turns it into DirectSound units `-100 * level` clamped at -10000
(`Voice_SetVolume`).  Pans are 0 .. 0x80 (0x40 centred) and become
`(pan - 0x40) * 2` (-128 .. 128), which the voice maps to
`(p / 128)^3 * 10000` DirectSound pan units (`Voice_SetPan`).  The
reimplementation applies these as linear gains (`10^(dB / 20)`).

## The API (`Wm_*`)

Every entry takes the library's lock (`Wm_Lock`), runs the operation and
converts the C++ exception the original may throw (an int) into the
result:

| code | meaning |
|-----:|---------|
| 0 | ok |
| 1 | already initialised |
| 5 | the sound device could not be opened |
| 0xc | file not found |
| 0xe | not a BW file / unknown codec / the decoder refused it |
| 0x13 | nothing loaded in the slot or channel |
| 0x14 | the library is not running (not initialised, or its timer is off) |
| 0x15 | channel >= 16 or slot >= 64 |
| 0x16 | the voice could not be created (DirectSound) |
| 0x17 | the voice could not be started |

`Wm_Init` creates the voices and the DirectSound device (the mixer here),
`Wm_StartTimer` is `SetTimer(hwnd, 1, 20 ms)`; most calls require both
("running" = flags 3).  The timer is a window timer in the original; here
the message pump calls `Wm_TimerPoll()` whenever it has nothing else to
do.

Effects: `Wm_SeLoad(slot, file, fadeInMs, gain)` takes a whole BW file in
memory (the background loader read it), decodes it completely, and burns a
linear fade-in of `fadeInMs` into the samples (`WmStream_FadeIn`; the
`fadeInMs` argument of "A0 21", 0 for "A0 20").  `Wm_SePlay` resets the
pan and volume, stops and restarts the voice.  Effects never loop.

Music: `Wm_BgmOpenFile` / `OpenArc` stream one file; the "pair" forms take
an intro and a loop part.  When both names are the same the single file is
opened with an explicit loop flag (`mode ? 3 : 1`), otherwise two Vorbis
streams are chained (`WmStream_OpenPair`).  A single file opened without
the pair form loops when *the file* says so: the header's loop flag (+18)
and restart frame (+1C) become the stream's `loopFlag` / `loopStart`,
which is exactly what `WmStream_Continues` and `WmStream_Restart` read,
and the pair form merely overrides those two header words
(`WmStream_SetLoop`).  Every background music of the games carries
`loop = 1` with its loop point (an intro of `start` frames, then the
repeated part); background voices and the full-length songs carry 0.  The
sound scripts read the flag themselves ("81 30" of the file's bytes
24..27) to refuse "PlaySE" for a looping file.  `Wm_BgmIsPlaying` and
`Wm_BgmLoopCount` are what "A0 15" pushes / stores: the first is the
voice's playing status, the second is the number of loop restarts, minus
one while the latest restart is less than four seconds old
(`WmStream_LoopCount`).

`gain` is a per-sample multiplier applied by every decoder, passed as a
double: 1.0 everywhere except "A0 21", whose 16.16 argument is converted.

## The BW container

    +00 header size (0x40)   +04 "bw  "
    +08 coded bytes          +0C frames (samples per channel)
    +10 sample rate          +14 channels (1 or 2)
    +18 loop flag            +1C loop start (frames): the stream restarts
                                 there when it reaches its end
    +20 .. +2F  2 x {predictor, step}: the ADPCM states for a loop restart
    +30 codec                +38 codec 2: loop restart bit position
                             +3C codec 2: step shift parameter

Codecs (`+30`):

* **0 - 4-bit ADPCM** (`adpcm.c`).  One nibble per sample, the low nibble
  of a byte first; stereo alternates the channel state per sample.  Code =
  sign (bit 3) + magnitude m (bits 0..2):
  `delta = ((2m + 1) * step) >> 3`, `sample = pred ± delta`,
  `step = (T[m] * step) >> 6` with `T = {57, 57, 57, 57, 77, 102, 128, 153}`,
  clamped to 0x7f .. 0x6000.  The predictor keeps the unclamped sample.
  Initial state {0, 0x7f}; a loop restart reloads the header's states.
  Mono streams decoded in odd-sized requests keep the extra decoded sample
  of the last byte for the next request.
* **1 - 16-bit PCM** (`pcm.c`).
* **2 - Huffman-coded 8-bit ADPCM** (`huffman.c`).  After the header: 8
  unused bytes, a 0x400-byte tree (a 32-bit root, then for each inner
  node `i >= 0x100` two int16 children at byte `4 + (i - 0x100) * 4`;
  values below 0x100 are the codes), then the bit stream, MSB first,
  windowed in 0x400-byte chunks.  Code = sign (bit 7) + magnitude m:
  `unit = 1 << ((param + 1) / 2)`, `delta = ((2m + 1) * step) / (2 unit)`,
  `step = step * (m < unit ? 57 : 77 + 25.6 (m - unit)) / 64` (truncating),
  clamped as above.  A restart seeks to `0x448 + loopBit / 8`.
* **3 - Ogg Vorbis** (`vorbis.c`) through libvorbisfile with callbacks
  that hide the 0x40-byte header; 16-bit output.  The pair streams keep
  two `OggVorbis_File`s and switch between them.

Every decoder multiplies the output by `gain` and clamps.

## The pump and looping

The pump (`Voice_Pump`) keeps the ring at least half a second ahead: it
asks the decoder for half a second at a time; when fewer frames come back
it asks the stream whether it continues (`WmStream_Continues`: the
header's loop flag of a single stream, or for a pair "always after the
intro, then `mode`") and if so calls the stream's restart
(`WmStream_Restart`: loop count + 1, a 4-second grace stamp, the decoder
back to the loop start), else it marks the stream done.  The voice stops
when the ring is drained after that.

## Sources

Files (`CreateFile` in the original), archive entries (the library has
its own index reader for the "PackFile" and, from 1.573 on, the "BURIKO
ARC20" archives the BGM comes from, entry names compared lower-cased) and
memory blocks (used for effects and for the decoded output of whole-file
streams); `source.c`.

## Build options

`VORBIS=vorbisfile` (default when `pkg-config vorbisfile` succeeds),
`VORBIS=stb` (drop `stb_vorbis.c` into `src/snd/`), `VORBIS=none` - without
a decoder, codec-3 sounds play as silence of the right length so that games
whose every effect is Vorbis still run.  Output is ALSA when its headers
are present at build time, else a silent clock that keeps the fades and
loop counters running.  `docs/building.md` has the details.

`BGI_SND_DEBUG=1` in the environment reports every music open (codec,
format, length, the loop specification) and the pump's restarts and ends
on stderr.
