/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * pad.c - the pads of the WebAssembly back end (OS_PadPoll of host/os.h):
 *         the browser's Gamepad API
 *
 * A browser shows a pad to a page once one of its buttons was pressed
 * while the page had the focus; from then on navigator.getGamepads() has
 * it at an index.  Each call reads the first four indices, and
 * host/os_pads.c makes what was read the engine's state: a pad with the
 * API's "standard" mapping by what its controls are, another by its
 * buttons' numbers.
 */
#include "host/os.h"
#include "host/os_pads.h"
#include "host/os_wasm.h"

#include <string.h>
#include <emscripten/emscripten.h>

#define PORTS 4

/* The pad at `index` of navigator.getGamepads() into out[0 .. 5]: its
 * pressed buttons (a bit each, the first 32), its first four axes in
 * thousandths, and 1 when its mapping is the standard one.  0 when there
 * is none. */
// clang-format off
EM_JS(int, WasmJs_PadRead, (int index, uint32_t* out), {
	var pads = null;
	try {
		pads = (typeof navigator !== 'undefined' && navigator.getGamepads) ? navigator.getGamepads() : null;
	} catch(e) { // (a frame that may not use the pads)
	}
	var p = pads ? pads[index] : null;
	if(!p || !p.connected)
		return 0;
	var b = 0, i;
	for(i = 0; i < p.buttons.length && i < 32; i++)
		if(p.buttons[i].pressed || p.buttons[i].value > 0.5)
			b |= 1 << i;
	HEAPU32[out >> 2] = b >>> 0;
	for(i = 0; i < 4; i++)
		HEAPU32[(out >> 2) + 1 + i] = Math.round((i < p.axes.length ? p.axes[i] : 0) * 1000) >>> 0;
	HEAPU32[(out >> 2) + 5] = p.mapping === 'standard' ? 1 : 0;
	return 1;
})
// clang-format on

static OpenBGI_PadState_t gState[PORTS];
static uint8_t gThere[PORTS]; // a pad is at the index

uint32_t OS_PadPoll(void)
{
	uint32_t changed = 0;
	int port;
	for(port = 0; port < PORTS; port++)
	{
		uint32_t raw[6];
		OpenBGI_PadState_t now;
		memset(raw, 0, sizeof raw);
		if(!WasmJs_PadRead(port, raw))
		{
			if(gThere[port])
				changed |= 1u << port; // the pad went
			gThere[port] = 0;
			continue;
		}
		OsPads_Gamepad((int)raw[5], raw[0], (int32_t)raw[1], (int32_t)raw[2], (int32_t)raw[3], (int32_t)raw[4], &now);
		if(!gThere[port] || memcmp(&now, &gState[port], sizeof now) != 0)
			changed |= 1u << port;
		gThere[port] = 1;
		gState[port] = now;
	}
	return changed;
}

const OpenBGI_PadState_t* OS_PadState(int port)
{
	return port >= 0 && port < PORTS && gThere[port] ? &gState[port] : NULL;
}

void OS_PadShutdown(void)
{
	memset(gThere, 0, sizeof gThere);
}
