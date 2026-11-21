/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * hostmock.h - a host-side stand-in for the browser half of the WebAssembly
 *              back end's file transport (src/host/wasm/host.c)
 *
 * Implements the WasmHostJs_* functions host.c would get from client.js and
 * server.js, against a directory on the local disk, with the server's
 * semantics (relative paths, case-insensitive components, ranged reads,
 * read-only).  Used by tests/wasmfs.c and by the simulator
 * tools/wasm/fakebrowser.c, both built with -Itools/wasmstub.
 */
#ifndef BGI_WASM_HOSTMOCK_H_
#define BGI_WASM_HOSTMOCK_H_

#include "bgi/common.h"

void HostMock_SetRoot(const char* dir); // the "game directory" (default: the current directory)
void HostMock_Counts(int* stat, int* read, int* list, int* changed);

#endif // BGI_WASM_HOSTMOCK_H_
