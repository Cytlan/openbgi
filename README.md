OpenBGI
=======

This project aims to reverse-engineer the Ethornell Buriko General Interpreter (BGI), and create an open source re-implementation of the engine.

We now have a Discord server: https://discord.gg/3zFTJWUNt7

Status
------

While we're still working on reimplementing opcodes, but it now makes sense to compile a stand-alone program.

It's still early days, but we are able to enter the main menu and settings screen of at least one game by executing Buriko Programs (`._bp` scripts.)

We're still exploring graphics routines.

![OpenBGI and Ethornell side-by-side](preview.png)

Currently, we're only exploring graphics, not sound.

Workflow
--------

Currently, the workflow is to use `Ghidra` for analysing the engine, and injecting a DLL into the process for live analysis and producing logs.

Using these logs, we implement similar behaviour in OpenBGI.

Milestones reached:
* Reimplemented enough basic opcodes for Tayutama to run.
* Standalone engine is being worked on.


Building
--------

While cross-platform compatibility is our goal, the build chain currenly is only configured for Linux.

Before you do anything else, you need to install the various tools and dependencies needed:
```sh
apt install build-essential gcc-mingw-w64-i686
```

Building the ebgube is as simple as running:
```sh
make
```

Finally, run the engine in the directory of the game files:
```sh
cd /path/to/the/game
/path/to/openbgi
```

The engine expect *all* game resources to have been extracted from the game using the `unarc` tool in the `tools` directory. In due time we will support loading directly from unmodified game assets, but for now it helps development to have assets extracted.

Contact
-------

Feel free to join our Discord server: https://discord.gg/3zFTJWUNt7
