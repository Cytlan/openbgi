/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * host.c - the browser side of the WebAssembly back end's file layer
 *
 * The WasmHost_* functions of fs.c are implemented here against the
 * websocket client of tools/wasm/client.js (linked as --pre-js).  Each one
 * that waits for the server is an EM_ASYNC_JS function: with Asyncify the
 * engine's call suspends until the promise settles, so fs.c reads a file as
 * synchronously as the native back ends do.
 *
 * Also here: what the page calls into the engine (the exported functions at
 * the end), and the Shift-JIS table built from the browser's TextDecoder.
 *
 * The interface is os_wasm.h (WasmHost_*).  Each WasmHost_ function is a
 * plain C forwarder to a WasmHostJs_ function of the same name, which is
 * the one Emscripten implements in JavaScript; on a host, where the stub
 * emscripten.h drops the JavaScript, tools/wasm/hostmock.c defines the
 * WasmHostJs_ functions over a local directory instead, so this file is
 * also part of tests/wasmfs.c and the simulator (make wasm-sim).
 */
#include "bgi/os.h"
#include "bgi/os_wasm.h"

#include <emscripten/emscripten.h>

// ---- the asset protocol ------------------------------------------------------------------

// STAT: out[5] = attrs, size low, size high, mtime low, mtime high; -1 when the path is missing or the server unreachable
// clang-format off
EM_ASYNC_JS(int, WasmHostJs_Stat, (const char* path, uint32_t* out), {
	var r = await BgiNet.stat(UTF8ToString(path));
	if(r.status !== 0)
		return -1;
	HEAPU32[out >> 2] = r.attrs;
	HEAPU32[(out >> 2) + 1] = r.sizeLo;
	HEAPU32[(out >> 2) + 2] = r.sizeHi;
	HEAPU32[(out >> 2) + 3] = r.mtimeLo;
	HEAPU32[(out >> 2) + 4] = r.mtimeHi;
	return 0;
});
// clang-format on

// READ: `len` bytes from the 64-bit offset (lo, hi) into dst; the bytes copied (fewer at the end), -1 on failure
// clang-format off
EM_ASYNC_JS(int, WasmHostJs_Read, (const char* path, uint32_t offLo, uint32_t offHi, uint32_t len, void* dst), {
	var bytes = await BgiNet.read(UTF8ToString(path), offLo >>> 0, offHi >>> 0, len >>> 0);
	if(bytes === null)
		return -1;
	if(bytes.length > (len >>> 0))
		bytes = bytes.subarray(0, len >>> 0); // never more than asked for
	HEAPU8.set(bytes, dst);
	return bytes.length;
});
// clang-format on

/* LIST: the byte size of the directory's records, -1 when it is not a
 * directory; the records themselves are parked in BgiNet.lastList until
 * WasmHostJs_ListTake copies them into the buffer the caller allocated for
 * that size */
// clang-format off
EM_ASYNC_JS(int, WasmHostJs_ListSize, (const char* path), {
	var raw = await BgiNet.list(UTF8ToString(path));
	BgiNet.lastList = raw;
	return raw === null ? -1 : raw.length;
});
// clang-format on

// copy the parked records to dst and let them go (synchronous: no round trip)
// clang-format off
EM_JS(void, WasmHostJs_ListTake, (void* dst), {
	if(BgiNet.lastList)
		HEAPU8.set(BgiNet.lastList, dst);
	BgiNet.lastList = null;
});
// clang-format on

/* the connection: Module.bgiFsUrl (the page's ?fs= parameter), else "fs"
 * next to the page on its own host, wss:// when the page came over https;
 * 1 when connected.  The cache limit the page chose (Module.bgiCacheLimit,
 * bytes, from ?cache=) is applied here through the openbgi_set_cache_limit
 * export below. */
// clang-format off
EM_ASYNC_JS(int, WasmHostJs_Connect, (void), {
	var url = Module['bgiFsUrl'];
	if(!url)
	{ // next to the page: a page at /bgi/ talks to /bgi/fs, so a proxy prefix needs no setting
		var path = typeof location !== 'undefined' ? location.pathname : '/';
		var dir = path.substring(0, path.lastIndexOf('/') + 1);
		url = (typeof location !== 'undefined' && location.protocol === 'https:' ? 'wss://' : 'ws://') +
			(typeof location !== 'undefined' ? location.host : '127.0.0.1:8000') + dir + 'fs';
	}
	try
	{
		await BgiNet.connect(url);
	}
	catch(e)
	{
		return 0;
	}
	if(Module['bgiCacheLimit'] > 0)
		Module['_openbgi_set_cache_limit'](Module['bgiCacheLimit']);
	return 1;
});
// clang-format on

/* the overlay is an IDBFS mount at `dir`: what the engine wrote in an
 * earlier visit comes back from IndexedDB (FS.syncfs(true)) before anything
 * reads it; without IDBFS the directory is plain MEMFS and the overlay lives
 * for the page only */
// clang-format off
EM_ASYNC_JS(void, WasmHostJs_OverlayMount, (const char* dir), {
	var d = UTF8ToString(dir);
	try
	{
		FS.mkdir(d);
	}
	catch(e)
	{
		// exists already
	}
	if(typeof IDBFS === 'undefined')
		return; // no IndexedDB: the overlay lives for the page only
	try
	{
		FS.mount(IDBFS, {}, d);
	}
	catch(e)
	{
		return;
	}
	await new Promise(function(resolve) {
		FS.syncfs(true, function(err) {
			if(err)
				console.warn('openbgi: loading the overlay failed:', err);
			resolve();
		});
	});
});
// clang-format on

// ---- the C entry points of os_wasm.h: forwarders (see the header comment) ----------------

int WasmHost_Connect(void)
{
	return WasmHostJs_Connect();
}

void WasmHost_OverlayMount(const char* dir)
{
	WasmHostJs_OverlayMount(dir);
}

int WasmHost_Stat(const char* relPath, uint32_t out[5])
{
	return WasmHostJs_Stat(relPath, out);
}

int WasmHost_Read(const char* relPath, uint32_t offLo, uint32_t offHi, uint32_t len, void* dst)
{
	return WasmHostJs_Read(relPath, offLo, offHi, len, dst);
}

int WasmHost_ListSize(const char* relPath)
{
	return WasmHostJs_ListSize(relPath);
}

void WasmHost_ListTake(void* dst)
{
	WasmHostJs_ListTake(dst);
}

// ---- the overlay's persistence --------------------------------------------------------------

/* The overlay directory is the IDBFS mount of WasmHostJs_OverlayMount;
 * FS.syncfs(false) copies MEMFS to IndexedDB.  The sync is debounced by
 * half a second so that a save of many small files syncs once; a page
 * closed within that half second loses the last write. */
// clang-format off
EM_JS(void, WasmHostJs_OverlayChanged, (void), {
	if(typeof FS === 'undefined' || !FS.syncfs)
		return;
	if(Module.bgiSyncTimer)
		clearTimeout(Module.bgiSyncTimer);
	Module.bgiSyncTimer = setTimeout(function() {
		Module.bgiSyncTimer = 0;
		FS.syncfs(false, function(err) {
			if(err)
				console.warn('openbgi: saving the overlay failed:', err);
		});
	}, 500);
});
// clang-format on

void WasmHost_OverlayChanged(void)
{
	WasmHostJs_OverlayChanged();
}

// ---- Shift-JIS -----------------------------------------------------------------------------

/* Fill the 0x10000-entry table indexed by the two-byte code (lead << 8 |
 * trail) with the Unicode scalar of every code of CP932, decoding each one
 * through the browser's "shift_jis" TextDecoder; entries without a code
 * stay 0 (the caller zeroes the table).  The browser's decoder is the
 * Windows table (WHATWG Encoding Standard), which is what the engine's text
 * was written against.  Without the decoder the table stays empty and
 * sjis.c maps everything to U+FFFD. */
// clang-format off
EM_JS(void, WasmHostJs_SjisTable, (uint32_t* table), {
	var dec;
	try
	{
		dec = new TextDecoder('shift_jis', { fatal: true });
	}
	catch(e)
	{
		console.warn('openbgi: no shift_jis decoder in this browser; Japanese text will not convert');
		return;
	}
	var pair = new Uint8Array(2);
	for(var lead = 0x81; lead <= 0xfc; lead++)
	{
		if(lead > 0x9f && lead < 0xe0)
			continue; // 0xA0 .. 0xDF are not lead bytes (the single-byte katakana live there)
		pair[0] = lead;
		for(var trail = 0x40; trail <= 0xfc; trail++)
		{
			if(trail === 0x7f)
				continue;
			pair[1] = trail;
			var s;
			try
			{
				s = dec.decode(pair);
			}
			catch(e)
			{
				continue; // not a code (fatal: true throws instead of substituting)
			}
			if(s.length === 0)
				continue;
			var cp = s.codePointAt(0);
			if(cp === 0xfffd)
				continue;
			HEAPU32[(table >> 2) + ((lead << 8) | trail)] = cp;
		}
	}
});
// clang-format on

void WasmHost_SjisTable(uint32_t* table)
{
	WasmHostJs_SjisTable(table);
}

// ---- what the page calls (the input events go to window.c's openbgi_push_event) ----------------

// Module._openbgi_set_cache_limit(bytes): the block cache limit (WasmHostJs_Connect applies ?cache=)
EMSCRIPTEN_KEEPALIVE void openbgi_set_cache_limit(uint32_t bytes)
{
	OsWasm_SetCacheLimit(bytes);
}

// Module._openbgi_cache_stat(which) for the page's status line (shell.html polls it once a second)
EMSCRIPTEN_KEEPALIVE uint32_t openbgi_cache_stat(int which) // 0 bytes held, 1 blocks, 2 requests made
{
	uint32_t bytes, blocks, requests;
	OsWasm_CacheStats(&bytes, &blocks, &requests);
	return which == 0 ? bytes : which == 1 ? blocks
										   : requests;
}
