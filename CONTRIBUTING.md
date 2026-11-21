# Contributing to OpenBGI

Thank you for helping. This page says what is welcome, what a change
has to respect, and how to send it.

- Questions and discussion: [Discord](https://discord.gg/3zFTJWUNt7)
- Bugs and proposals: <https://github.com/Cytlan/openbgi/issues>
- Coding rules: [CONVENTIONS.md](CONVENTIONS.md)
- How the code is organised: [docs/dev/](docs/dev/README.md)

## What helps most

| Contribution | Notes |
|---|---|
| Reports of games that do not work | See [docs/manual/help.md](docs/manual/help.md) for what to include. The output of `openbgi --detect GAME` and of `openbgi --list-missing GAME` is the most useful part. |
| Missing or wrong instructions | [docs/dev/embedding.md](docs/dev/embedding.md), "Implementing an original instruction" |
| Testing on real systems | Games are tested on Linux; testing on Windows, Haiku and macOS is ad hoc |
| Ports and hosts | [docs/dev/os_layer.md](docs/dev/os_layer.md) |
| Documentation | Corrections to any page under `docs/` |

Talk to us before starting something large, so that the work fits and
is not done twice.

## Rules a change must respect

1. **The original's behaviour is the specification.** A function keeps
   the original's results for the same inputs, result codes and quirks
   included. Where builds of the engine differ, write one variant per
   build and select it by generation
   ([docs/dev/versions.md](docs/dev/versions.md)).
2. **Languages.** C99 for the engine (C++ only in the Haiku host
   file, Objective-C only in the macOS host files). Python 3, standard
   library only, for build scripts. No other language.
3. **The core stays portable.** No system header and no platform `#if`
   outside `src/host/`. What the core needs of a system goes through
   `include/openbgi.h`. `make check-core` enforces this.
4. **All engine state lives in `OpenBGI_t`.** No new global or static
   variables that hold state.
5. **Few dependencies.** Do not add a library without discussing it
   first. Third-party code that is added must be listed in `NOTICES`
   with its licence.
6. **No new instructions without agreement.** OpenBGI's own `EF`
   family is kept small; propose an addition on the issue tracker
   first.
7. **OpenBGI starts no programs** for a game, and does not change a
   game's files.
8. **No DRM circumvention**, in code or in documents.
9. **No game data.** No files, pictures, scripts or text taken from a
   game in the tree, in tests or in issues. Tests build the small
   inputs they need.
10. **Comments describe what the code does** and how to use it. Do not
    put addresses or disassembly of the original executables into the
    source or the documents.
11. **The API only grows.** `include/openbgi.h` and
    `include/openbgi_ffmovie.h` are frozen as of v1.0. A change to them
    adds - a function, a constant, a member at the end of a struct that
    has `size` - and raises `OPENBGI_API_MINOR`. Nothing in them is
    removed, renamed or given another meaning
    ([docs/dev/embedding.md](docs/dev/embedding.md)).

## Before you send a change

    clang-format -i --style=file <the files you changed>
    make test
    make check
    make check-core
    make check-lib

- Everything must build without a compiler warning.
- Files listed in `.clang-format-ignore` are generated or third-party;
  do not format them.
- If you touched `src/host/win32/` and have MinGW and wine:
  `make PLATFORM=win32 test`. Without them: `make check-win`.
- If you touched the macOS or browser parts: `make check-macos`,
  `make check-wasm`.
- New behaviour comes with a unit test in `tests/` where it can be
  driven without a game ([docs/dev/testing.md](docs/dev/testing.md)).
- Update the page under `docs/` that describes what you changed.

## Sending it

Open a pull request at <https://github.com/Cytlan/openbgi>.

- One topic per pull request.
- Say what the change does and how you tested it: which checks, which
  system, which game and engine version if a game is involved.

## Licence

OpenBGI is licensed under the GNU General Public License, version 2
only (`LICENSE`). By contributing you agree that your contribution is
under the same licence. A new source file starts with:

    /*
     * This file is part of OpenBGI (https://openbgi.net).
     * SPDX-License-Identifier: GPL-2.0-only
     */
