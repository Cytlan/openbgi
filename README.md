# OpenBGI

**OpenBGI v1.0**

OpenBGI is an open-source reimplementation, in C99, of Ethornell's
"Buriko General Interpreter" (BGI), the visual-novel engine.  It is
reverse-engineered function by function from the `tayutama.exe` build
(ver 1.69, build 444.2, with the "Wave Master" sound library) and
extended to run the scripts of the engine's other builds, from ver 1.58
to ver 1.669 (`docs/dev/versions.md`).  Every function keeps the original's
results for the same inputs, including its result codes and quirks.  The
Windows specifics sit behind an OS layer (`src/host/os.h`) with a Win32
back end, a Linux back end (X11, FreeType, ALSA; it also builds for Haiku,
on Xlibe and the Media Kit, for macOS, on AppKit, Metal and AudioToolbox, and -
for now without a display - for Android) and a WebAssembly back end (a
canvas, the game's files over a websocket; `docs/dev/wasm.md`).

* Website: [OpenBGI.net](https://openbgi.net)
* Discord: <https://discord.gg/3zFTJWUNt7>
* Issues: <https://github.com/Cytlan/openbgi/issues>

**OpenBGI is a work in progress.**  Expect bugs and missing pieces;
please report bugs and issues on the
[issue tracker](https://github.com/Cytlan/openbgi/issues) or in our
[Discord](https://discord.gg/3zFTJWUNt7).

**OpenBGI is not a Visual Novel development platform.**  If you wish to
develop a VN on the Ethornell/BGI platform, then you must contact Buriko
Co., Ltd.

## The API, and what stays stable

v1.0 separates the engine into a **core**, which runs the games, and a
public **API** through which a program drives it (`include/openbgi.h`).
The program hands the core its files, its input and a clock, and takes
the picture and the sound. OpenBGI's own program, its libretro core and
its browser build all use the core this way, and it is the way to embed
the engine in another program or to bring it to another system.

- **The API is stable.** Anything that uses the v1.0 API keeps working
  with newer versions of OpenBGI within major version 1 of the API
  (`OPENBGI_API_MAJOR`). The core may change behind it; within that
  major version the API only grows and will not break.
- **The core is not.** Everything behind the API may change in any
  version, breaking changes included. Code that touches the core's
  internals gets no guarantee of stability.
- **Forks of v0.1.** v0.1 had no API, and v1.0 rebuilt the engine's
  internals to make one. A fork of v0.1 may not be compatible with v1.0
  and cannot be expected to migrate to it cleanly.

Technical details: [docs/dev/embedding.md](docs/dev/embedding.md).

## Games

The games we have been working on, with the engine build each one needs
(`docs/dev/versions.md` has the details of the builds):

| Game | Product name | Engine version | Status |
|---|---|---|---|
| Nursery Rhyme (trial) | `NurseryRhymeTrial` | 1.58 | Very playable |
| Nursery Rhyme | `NurseryRhyme` | 1.64 | Very playable |
| Itsuka, Todoku, Ano Sora ni. | `ItsukaTodokuAnoSorani` | 1.66 | Very playable |
| Tayutama -Kiss on my Deity- (trial; the reference build) | `Tayutama` | 1.69 build 444 | Very playable |
| Tayutama -Kiss on my Deity- | `Tayutama` | 1.69 build 451 | Very playable |
| D.C. \~Da Capo\~ (English edition) | `DCENG` | 1.69 build 461 | Mostly playable |
| Tayutama -It's happy days- | `TayutamaHD` | 1.69 build 472 | Very playable |
| Kira Kira Curtain Call (English edition) | `KiraKiraCurtainCallENG` | 1.492 | Mostly playable |
| Prism Rhythm | `PrismRhythm` | 1.494 | Very playable |
| Diamic Days | `DiamicDays` | 1.529 | Very playable |
| Gakuou -THE ROYAL SEVEN STARS- | `Gackoh` | 1.535 | Very playable |
| Gakuou -It's Heartful Days!!- | `Gackoh_HD` | 1.547 | Very playable |
| Hanairo Heptagram | `HanairoHeptagram` | 1.553 | Very playable |
| Magical Charming! | `MagicalCharming!` | 1.573 | Very playable |
| Sekai to Sekai no Mannaka de | `SekaiToSekaiNoMannakaDe` | 1.588 | Very playable |
| Unmei Senjou no Phi | `UnmeisenjouNoPhi` | 1.599 | Very playable |
| Kodomo no Asobi | `KodomoNoAsobi` | 1.616 | Very playable |
| Sen no Hatou, Tsukisome no Kouki (trial) | `SenNoHatouTsukisomeNoKouki` | 1.636 | Testing in progress |
| Tayutama 2 -After Stories- | `Tayutama2AS` | 1.640 | Very playable |
| Wakabairo no Quartet | `WakabaironoQuartet` | 1.653 | Very playable |
| Nekotsuku, Sakura. | `NekoTsukuSakura` | 1.654 | Mostly playable |
| Madohi Shiroki no Kamikakushi | `MadoiShirokinoKamikakushi` | 1.659 | Very playable |
| Yumahorome | `Yumahorome` | 1.662 | Very playable |
| Arcana Alchemia | `ArcanaAlchemia` | 1.667 | Very playable |
| Haruka Ao no Hanayome ni | `HarukaAonoHanayomeni` | 1.669 | Very playable |

### Definitions

* Verified - Extensive test against all aspects of the game; Visuals, paths, choices, saving, loading, system settings, and a complete 1:1 verification against executed bytecode. Visuals are indistinguishable from the original release.
* Very playable - No noticeable glitches were encountered during gameplay. Skipping, saving, reloading, sound, cutscenes and choices all work as expected through extended gameplay. Not comprehensively tested, but confidently playable from start to finish.
* Mostly playable - Same as very playable, but more limited gameplay testing.
* Playable - Some visual glitches noticed, or incomplete gameplay testing.
* Somewhat playable - Incomplete gameplay testing. Obvious glitches noticed. May crash.
* Boots - Title screen reached, but will crash in-game. Gameplay testing impossible.
* Testing in progress - Runs basic boot routine, but nothing more is known at this time.

### Testing scope

Apart from the trial of Sen no Hatou, Tsukisome no Kouki (August), whose
build is surveyed but not yet playable, Fortune Arterial (August),
which boots (`--no-disc-check`), and Yoake mae yori Ruri iro na
(August, 1.658.2 on the 1.659 profile), whose prologue plays, only Lump
of Sugar games have been tested so far.  We are interested in testing other BGI-based games: if you have one, tell us how it goes in
our [Discord](https://discord.gg/3zFTJWUNt7).

Testing happens on a case-by-case basis, and to exercise newly implemented opcodes and subsystems. There is currently no extensive testkit against individual games, except booting up a game and actually playing it.

## Getting OpenBGI

Download a release for Windows, Linux, macOS or Haiku from
<https://github.com/Cytlan/openbgi/releases>, then follow
[docs/manual/install.md](docs/manual/install.md).

To build it yourself on Linux:

    make
    bin/linux/openbgi-x86_64 /path/to/game

Every other system, the dependencies and the options are in
[docs/dev/building.md](docs/dev/building.md).

## Documentation

| For | Start at | Covers |
|---|---|---|
| Players | [docs/manual/](docs/manual/README.md) | Installing, adding games, controls, where files are kept, the command line, getting help |
| Modders and translators | [docs/modding/](docs/modding/README.md) | Extracting and rebuilding a game's files, the scripts, patches, OpenBGI's text features |
| Developers | [docs/dev/](docs/dev/README.md) | Architecture, building, file formats, the virtual machine, embedding the core |

To contribute, read [CONTRIBUTING.md](CONTRIBUTING.md) and
[CONVENTIONS.md](CONVENTIONS.md).

## Special Thanks and Honourable Mentions

### Special Thanks

* Darios Sawm / Buriko Co., Ltd.
* [Anonymous]
* minirop
* Galladite
* txt231
* Wenni
* 56se
* makoroviz
* Funix

### Honourable Mentions

* The BGI tools of the community:
  * [GARbro](https://github.com/morkt/GARbro) (morkt)
  * [arc_unpacker](https://github.com/vn-tools/arc_unpacker) (vn-tools)
  * [EthornellTools](https://github.com/arcusmaximus/EthornellTools)
    (arcusmaximus)
  * [VNTranslationTools](https://github.com/arcusmaximus/VNTranslationTools)
    (arcusmaximus)
  * [Bgi_tools](https://github.com/mchubby/Bgi_tools) (mchubby)
  * [engine_bgi](https://github.com/mnakamura1337/engine_bgi)
    (mnakamura1337)
  * [ethornell-tools](https://github.com/Jair4x/ethornell-tools) (Jair4x)
  * [bgi-extractor](https://github.com/mirinnano/bgi-extractor)
    (mirinnano)
  * [BGIextractor](https://github.com/myzert/BGIextractor) (myzert)
* [BGI](https://github.com/txt231/BGI) by txt231
* [ethornell-rs](https://github.com/xmoezzz/ethornell-rs) (xmoezzz)
* Operation Bellflower - [Adventures in the Ethornell
  Engine](https://www.operationbellflower.com/blog/adventures-in-the-ethornell-engine/part-1)

## License

OpenBGI is licensed under the GNU General Public License, version 2
(GPL-2.0); see `LICENSE`.

OpenBGI includes 3rd-party code and libraries. Their licenses and
copyright notices are reproduced in `NOTICES` which is shipped with all builds.
