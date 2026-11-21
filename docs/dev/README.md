# Developer documentation

For people who build, change, port or embed OpenBGI.

| Start here | |
|---|---|
| [architecture.md](architecture.md) | Principles, how a game runs, the layers, the source tree |
| [building.md](building.md) | Toolchains and dependencies for every system; targets and settings |
| [testing.md](testing.md) | The checks a change must pass; the test host |
| [`CONTRIBUTING.md`](../../CONTRIBUTING.md), [`CONVENTIONS.md`](../../CONVENTIONS.md) | How to contribute; the coding rules |

| The engine | |
|---|---|
| [vm.md](vm.md) | The script machine: threads, scheduler, main loop, pointers, tracing |
| [versions.md](versions.md), [opcodes.md](opcodes.md) | Engine builds, profiles, game detection; every opcode by version |
| [os_layer.md](os_layer.md) | Hosts and platform back ends |
| [embedding.md](embedding.md) | The core as a library; writing a host; adding instructions |
| [sound.md](sound.md), [input.md](input.md), [launcher.md](launcher.md) | Subsystems |
| [panel_system.md](panel_system.md), [particle_rain.md](particle_rain.md), [child_edit_dialogs.md](child_edit_dialogs.md), [bfmovie.md](bfmovie.md) | Instruction families |
| [program_file.md](program_file.md) | A game's check of the engine's executable |
| [libretro.md](libretro.md), [wasm.md](wasm.md) | The libretro core; the browser build |

| File formats | |
|---|---|
| [formats/](formats/README.md) | Archives, compression, pictures, sound, programs, scenario files, frame sequences |

Related: the tools and source languages are described for modders in
[../modding/](../modding/README.md); what a player sees in
[../manual/](../manual/README.md).
