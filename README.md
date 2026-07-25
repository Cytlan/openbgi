OpenBGI
=======

This project aims to reverse-engineer the Ethornell Buriko General Interpreter (BGI), and create an open source re-implementation of the engine.

We now have a Discord server: https://discord.gg/3zFTJWUNt7

Workflow
--------

Currently, the workflow is to use `Ghidra` for analysing the engine, then reimplement opcodes and support functions on a per-opcode basis.

The strategy is just to implement an opcode, run the game and see if it crashes. It's a pretty crude technique, but it'll work for now.

Milestones reached:
* Reimplemented enough basic opcodes for Tayutama to run.
* Standalone engine is being worked on.

Architecture
------------

While we're still working on reimplementing opcodes, but it now makes sense to compile a stand-alone program. Uur strategy is still to hook into an existing engine and redirect functions and opcodes to a shared library, and reimplementing this into the standalone version.

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
