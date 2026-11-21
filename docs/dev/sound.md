# Sound

The sound library ("Wave Master" in the sources; "WaveLib" in the
original) and the engine's sound output. The file format:
[formats/sound.md](formats/sound.md).

## Layers

| Layer | Code | Does |
|---|---|---|
| "A0 xx" handlers | `src/vm/ops_snd.c` | The scripts' instructions |
| Sound manager | `src/snd/sound.c` | Master volumes, file search, archive names |
| Library (`Wm_*`) | `src/snd/wm/` | 16 music channels, 64 effect slots, fades, the 20 ms timer, voices, the mixer |
| Streams | `src/snd/bw/` | Sources (file, archive entry, memory), the BW header, one file per codec |
| Sound out | `src/snd/sndout.c` | Runs the mixer on the host's clock; the ring the host reads |
| Host | `OpenBGI_AudioRead` | Plays what it reads |

The original mixes with DirectSound, one secondary buffer per voice.
OpenBGI keeps everything above the voice and replaces DirectSound with
a software mixer at 44.1 kHz stereo. Effects play from their decoded
samples; music is pulled from its decoder through a ring
(`Voice_Pump`, `voice.c`). Decoded movies add their sound through
`Wm_SetExternalMix`.

## The sound out

- The library, the fades and the mixer run on the engine's thread. No
  lock is taken.
- The mixer runs at the start and end of every frame and whenever the
  host calls `OpenBGI_RunAudio`, for the time that passed on the host's
  clock, in pieces of 1024 frames.
- Output goes into a ring of 32768 frames (0.74 s). The host drains it
  from any thread with `OpenBGI_AudioRead`. The two sides share two
  counters and nothing else.
- The frame for millisecond t is `floor(t * 44100 / 1000)`, so the mix
  is the same however time is cut into calls. The test host relies on
  this.

`OpenBGI_SetAudioLead(L)` says how far ahead of the reader the mixer
runs:

| L | Use | Behaviour |
|---|---|---|
| 0 | Tools, tests | Nobody plays the sound. The mixer runs for the time that passed; overflow is dropped. Fades, loops and "is it playing" keep their timing. |
| > 0 (60 ms in the program) | A host that plays it | Between L and 2 L frames stay unread. The voices follow the output's pace, whatever the drift between the clocks. |

The lead is the host's setting. It holds across `OpenBGI_Stop` and
`OpenBGI_Start`, and for a game the launcher starts. A host that plays
the sound must set one: with L = 0 and an output that goes on reading,
every long frame leaves more unread sound in the ring, up to its whole
length.

A host that waits long between frames, or shows a box of its own, calls
`OpenBGI_RunAudio` meanwhile.

A sound that starts is heard after the sum of:

| Part | Delay |
|---|---|
| The ring | L to 2 L: 60 to 120 ms in the program |
| The back end's buffers | 60 to 80 ms: four periods of 20 ms with ALSA (a device may grant more) and waveOut, three with AudioQueue |
| The system's mixer or sound server | Its own |

## Volumes

Each channel and slot has a record (`WmRecord_t`, `wm_internal.h`):
`master`, `aux` (the channel's own volume, "A0 1C" from 1.553 and
"A0 2C" from 1.588), `volume`, `fadeLevel`, each 0 .. 0x80, plus fades
of the volume and of the fade level.

    level = int((0x200 - fadeLevel - volume - aux - master) * 0.375)    dB down
    level >= 48  ->  silence

From 1.588 the level is `int((0x200 - sum) / 2.66666666)` and is not
cut at 48; only a single volume at 0 silences.

- A fade is linear between two ticks, stepped by the 20 ms timer
  (`Records_Tick`, `Fade_Step`).
- Pan is 0 .. 0x80 (0x40 centred), mapped to `((pan - 0x40) / 64)^3`
  of full pan.
- OpenBGI applies levels as linear gains, `10^(dB / 20)`.

## Result codes of `Wm_*`

| Code | Meaning |
|---|---|
| 0 | OK |
| 1 | Already initialised |
| 5 | The machine has no sound output |
| 0xc | File not found |
| 0xe | Not a BW file, unknown codec, or the decoder refused it |
| 0x13 | Nothing loaded in the slot or channel |
| 0x14 | The library is not running |
| 0x15 | Channel >= 16 or slot >= 64 |
| 0x16, 0x17 | The voice could not be created, or started |

The original's timer is a window timer. Here the message pump calls
`Wm_TimerPoll` when idle, and `OpenBGI_RunAudio` does while a request
holds the engine.

## Effects

`Wm_SeLoad(slot, file, fadeInMs, gain, rate)` decodes a whole BW file
from memory.

- A fade-in is burned into the samples ("A0 21").
- A rate other than 1 ("A0 27" / "A0 28", 1.547 on: voice speed)
  stretches the sound in time at the same pitch: chunks of 30 ms, taken
  every `chunk * rate` frames, joined with 5 ms crossfades
  (`WmStream_TimeStretch`).
- Effects never loop.

## Music

- `Wm_BgmOpenFile` / `OpenArc` stream one file. It loops when its
  header says so (loop flag and loop start).
- The pair forms take an intro and a loop part. With the same name
  twice, the single file is opened with an explicit loop flag;
  otherwise two Vorbis streams are chained.
- The pump keeps the ring half a second ahead. When the decoder runs
  out, it restarts the stream at its loop start if the stream
  continues, else marks it done.
- `Wm_BgmLoopCount` is the number of restarts, minus one while the
  latest restart is less than four seconds old.

## Wave files ("A0 C0")

The original plays them with `PlaySound`, beside DirectSound, at full
volume. Here `Wm_PlayWave` (`wave.c`) plays PCM of 8 or 16 bits on a
mixer voice of its own; the library's volumes and fades do not reach
it. Compressed WAVE formats are refused.

## Build and debugging

- `VORBIS=vorbisfile`, `stb` or `none` ([building.md](building.md)).
  Without a decoder, Vorbis sounds play as silence of the right length.
- `BGI_SND_DEBUG=1` reports on standard error, with lines that begin
  with `snd:`:
  - every music open and the pump's restarts;
  - once a second, the least and the most sound that was mixed and not
    yet read (`snd: out:`), with the lead and the frames dropped;
  - with ALSA, the period and buffer the device granted, and once a
    second how much sound waits in the device and how many periods the
    engine could not fill (`snd: device:`).
