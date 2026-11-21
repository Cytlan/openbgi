/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tools.h - the command-line tools inside the program (docs/modding/README.md);
 *           implemented in src/tools/
 *
 * The tools are commands of the program itself:
 *
 *     openbgi dump ...     every archive of a game into files to edit  (bgidump.c)
 *     openbgi pack ...     the archives again, from a dump             (bgipack.c)
 *     openbgi scn ...      scenario files                              (bgiscn.c)
 *     openbgi asm ...      programs as listings                        (bpasm.c)
 *     openbgi bpc ...      programs in the source language             (bpasm.c)
 *     openbgi help [COMMAND]
 *
 * and each is the program under another name too: started as bgidump,
 * bgipack, bgiscn, bpasm or bpc - a link to the program or a copy of it,
 * with or without what the build puts after the name ("-x86_64", ".exe") -
 * it is that tool, and its arguments are the tool's.  `make` puts those
 * links beside the program.
 *
 * A build has them when BGI_TOOLS is defined (the Makefile's TOOLS: the
 * desktop programs and the test host).  An entry point calls Tools_Run
 * before anything else and leaves with the status it gives when it
 * returns 1.
 */
#ifndef BGI_TOOLS_TOOLS_H_
#define BGI_TOOLS_TOOLS_H_

#include <stdio.h>

/* Run the tool the program was started as, or the one its first argument
 * names, with the status of its run in `*status`: 1 when there was one
 * (`openbgi help`, --help and -h as the first argument count: the
 * engine's options and the commands, status 0), 0 when the arguments are
 * the engine's.  `argv` is main's; the folder of a game that is named
 * like a command is given as "./dump". */
int Tools_Run(int argc, char** argv, int* status);

/* The tools: argv[0] is the name to show in their messages ("openbgi
 * dump", "bgidump"), the arguments follow.  The value is the exit status;
 * a usage error ends the process with the status 2, --help with 0. */
int Tool_Dump(int argc, char** argv);
int Tool_Pack(int argc, char** argv);
int Tool_Scn(int argc, char** argv);
int Tool_Asm(int argc, char** argv);
int Tool_Bpc(int argc, char** argv);

/* "usage: PROG LINE" for the first of `lines` (ended by NULL) and the
 * others under it, each after the program's name again */
void Tools_Usage(FILE* out, const char* prog, const char* const* lines);

#endif
