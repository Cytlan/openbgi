/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * os_wasm.h - helpers shared by the files of the WebAssembly back end
 *             (src/os/wasm/, *.c); nothing above the OS layer includes this.
 *
 * The back end runs the engine on the browser's main thread, compiled with
 * Emscripten's Asyncify so that the engine's blocking calls (a file read, a
 * sleep) suspend into the browser's event loop and resume where they were.
 * Everything the browser provides arrives through the WasmHost_* functions
 * below, implemented in host.c as C forwarders to EM_JS / EM_ASYNC_JS
 * functions (WasmHostJs_*) against tools/wasm/client.js; on a host, where
 * the stub emscripten.h drops the JavaScript, tools/wasm/hostmock.c defines
 * the WasmHostJs_* functions over a local directory, so that fs.c (the file
 * layer, the cache and the overlay) can be tested (tests/wasmfs.c) and the
 * whole engine run without a browser (make wasm-sim, with
 * tools/wasm/fakebrowser.c for the page side).  docs/wasm.md describes how
 * the pieces fit.
 */
#ifndef BGI_OS_WASM_H_
#define BGI_OS_WASM_H_

#include "bgi/common.h"

// ---- the asset protocol (tools/wasm/server.js): read-only, paths relative to the game directory ----

/* Paths are UTF-8 with '/' separators and no leading one ("" is the game
 * directory); the server resolves them without regard to ASCII case.  Each
 * call that waits for the server suspends the engine (Asyncify) until the
 * reply is in. */

/* STAT: 0 found (out = attrs, size low, size high, mtime low, mtime high as
 * a FILETIME) or -1 missing / unreachable */
int WasmHost_Stat(const char* relPath, uint32_t out[5]);
/* READ: `len` bytes from the 64-bit offset (offLo, offHi) into `dst`; the
 * bytes copied (fewer at the end of the file), -1 on failure */
int WasmHost_Read(const char* relPath, uint32_t offLo, uint32_t offHi, uint32_t len, void* dst);
/* LIST: the byte size of the directory's entry records (server.js: u32
 * attrs, u32 size low, u32 size high, u16 name length, the name in UTF-8,
 * all little-endian), -1 when it is not a directory; the records are then
 * taken with WasmHost_ListTake into a buffer of that size, which must
 * happen before the next LIST */
int WasmHost_ListSize(const char* relPath);
void WasmHost_ListTake(void* dst);
// the overlay (what the engine wrote) changed: persist it when the platform can (debounced)
void WasmHost_OverlayChanged(void);
/* OS_Init: open the connection to the asset server (1 ok) and mount the
 * persistent overlay directory, loading what an earlier visit saved */
int WasmHost_Connect(void);
void WasmHost_OverlayMount(const char* dir);
// fill the two-byte Shift-JIS -> Unicode table (0x10000 entries indexed by the code, 0 = unmapped; zeroed by the caller)
void WasmHost_SjisTable(uint32_t* table);

// ---- fs.c -------------------------------------------------------------------------

/* Where the overlay lives: a directory of the local (in-memory) file system
 * holding every file the engine created, under its path relative to the
 * game directory in lower case, and ".openbgi/" with the back end's own
 * files (the registry emulation, the list of deleted paths).  Set before
 * OS_Init (host.c sets "/overlay", the test a scratch directory). */
void OsWasm_SetOverlayRoot(const char* dir);
const char* OsWasm_OverlayRoot(void);
// the limit of the block cache of server files in bytes (default 512 MB); a lower limit evicts at once
void OsWasm_SetCacheLimit(uint32_t bytes);
// cache statistics for the page: bytes held, blocks held, requests made to the server
void OsWasm_CacheStats(uint32_t* bytes, uint32_t* blocks, uint32_t* requests);
// OS_Init (after the overlay is mounted) / OS_Shutdown: the overlay directories and the tombstones; the caches
void OsWasm_FsInit(void);
void OsWasm_FsShutdown(void);
/* the engine's path (Shift-JIS, '\\', an optional drive) as a UTF-8 path
 * relative to the game directory: '/' separators, "." and ".." resolved,
 * "" for the root; a path without a drive or leading separator starts at
 * the current directory (OS_SetCurrentDir) */
void OsWasm_RelPath(const char* win, char* out, size_t n);

// ---- sjis.c -------------------------------------------------------------------------

// the conversions the other files use for the text they hand the page; the table is built on first use
uint32_t OsWasm_SjisChar(uint16_t code);                       // one two-byte code -> Unicode (0xfffd when unmapped)
void OsWasm_SjisToUtf8(const char* sjis, char* out, size_t n); // a string to UTF-8 in `out` (n bytes), NUL-terminated
int OsWasm_UnicodeToSjis(uint32_t cp, char out[2]);            // the bytes written, 0 when CP932 has no code

// ---- window.c -----------------------------------------------------------------------

// the page's event handlers (host.c) queue input here; the pump delivers it
#define WASM_EV_KEY_DOWN   1  // a = virtual key, b = repeat
#define WASM_EV_KEY_UP     2  // a = virtual key
#define WASM_EV_MOUSE_MOVE 3  // a = x, b = y
#define WASM_EV_BUTTON     4  // a = button (0 L 1 R 2 M 3 X1 4 X2), b = down, c = x, d = y
#define WASM_EV_WHEEL      5  // a = delta in WHEEL_DELTA units
#define WASM_EV_ACTIVATE   6  // a = active
#define WASM_EV_CLOSE      7  // the page asked the engine to end
#define WASM_EV_CHAR       8  // a = code point typed (the text entry)
#define WASM_EV_SYSKEY     9  // a = virtual key pressed with Alt
#define WASM_EV_PAINT      10 // the page wants the window drawn again (it was resized or shown)
// queue an event (coordinates in canvas pixels); the oldest is dropped when the queue of 256 is full
void OsWasm_PushEvent(int type, int a, int b, int c, int d);
/* return to the browser for `ms` milliseconds (emscripten_sleep), or for
 * one message round trip when `ms` is 0: the page paints, the websocket
 * delivers, the input handlers run; the audio pump runs first */
void OsWasm_Yield(int ms);

// ---- audio.c ------------------------------------------------------------------------

// mix what keeps the output ahead and hand it to the page; called from OsWasm_Yield
void OsWasm_AudioPump(void);

#endif // BGI_OS_WASM_H_
