/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * audio.c - the WebAssembly back end's sound output, CD audio and movies
 *
 * The sound library pulls 16-bit stereo frames through OS_AudioStart()'s
 * callback.  A page cannot run that callback from the audio thread - the
 * mixer decodes music straight from the archive, which here means a
 * websocket round trip through Asyncify, impossible from a Web Audio
 * callback - so the engine thread pumps it instead: OsWasm_AudioPump, called
 * from the back end's yield (window.c), asks the page how many frames keep
 * the output about 200 ms ahead, mixes them and hands them over as an
 * AudioBuffer scheduled right after the previous one.  Until the page has
 * seen a user gesture the AudioContext stays suspended and the frames are
 * mixed against the clock and dropped, so that fades and loops keep their
 * timing like the silent machine of the POSIX back end.
 *
 * CD audio through MCI, PlaySound and the DirectShow movie graph have no
 * counterpart on a page: every call reports failure, which the engine
 * handles.
 *
 * The interface is the audio section of os.h; OsWasm_AudioPump is declared
 * in os_wasm.h.  The WasmJs_* functions are the page side (EM_JS);
 * tools/wasm/fakebrowser.c stands in for them on a host.
 */
#include "bgi/os.h"
#include "bgi/os_wasm.h"

#include <emscripten/emscripten.h>

#define AUDIO_PUMP_MAX 8192 // frames mixed per callback at most: the size of gAudio.buf

static struct
{
	int running;        // 1 between a successful OS_AudioStart and OS_AudioStop
	int rate;           // the output rate in Hz
	OsAudioFill_t fill; // the mixer's pull callback
	void* user;         // its argument
	int16_t* buf;       // AUDIO_PUMP_MAX interleaved stereo frames the callback fills
} gAudio;

// 1 when the browser has Web Audio (an AudioContext constructor)
// clang-format off
EM_JS(int, WasmJs_AudioAvailable, (void), {
	return (typeof AudioContext !== 'undefined' || typeof webkitAudioContext !== 'undefined') ? 1 : 0;
});
// clang-format on

/* Open the output: the page-side record Module.bgiAudio {rate: the mixer's
 * rate in Hz, ctx: the AudioContext, nextTime: where the next buffer is
 * scheduled (seconds on the context's clock, or the wall clock while it is
 * suspended), lead: how far ahead the output is kept, 0.2 s}.  The context
 * is asked for at `rate`; a browser that refuses the rate gets one at its
 * own and resamples the buffers, which are always created at `rate`.  0 when
 * there is no Web Audio or no context could be made. */
// clang-format off
EM_JS(int, WasmJs_AudioStart, (int rate), {
	var AC = typeof AudioContext !== 'undefined' ? AudioContext : (typeof webkitAudioContext !== 'undefined' ? webkitAudioContext : null);
	if(!AC)
		return 0;
	var a = Module.bgiAudio = { rate: rate, ctx: null, nextTime: 0, virtualTime: 0, lead: 0.2 };
	try
	{
		a.ctx = new AC({ sampleRate: rate });
	}
	catch(e)
	{
		try
		{
			a.ctx = new AC();
		}
		catch(e2)
		{
			return 0;
		}
	}
	// the context may only start after a user gesture: the page's input handlers
	// (window.c, WasmJs_InstallInput) call Module.onUserGesture on a click or a key
	var prev = Module['onUserGesture'];
	Module['onUserGesture'] = function() {
		if(prev)
			prev();
		if(a.ctx && a.ctx.state === 'suspended')
			a.ctx.resume();
	};
	return 1;
});
// clang-format on

// close the context and drop the record; nothing scheduled plays on
// clang-format off
EM_JS(void, WasmJs_AudioStop, (void), {
	var a = Module.bgiAudio;
	if(a && a.ctx)
		a.ctx.close();
	Module.bgiAudio = null;
});
// clang-format on

/* How many frames would keep the output `lead` seconds ahead of the clock
 * (0 when it is).  The clock is the context's while it runs and the wall
 * clock while it is suspended, so the mixer keeps producing - and the
 * fades and loops keep their timing - before the first user gesture. */
// clang-format off
EM_JS(int, WasmJs_AudioWanted, (void), {
	var a = Module.bgiAudio;
	if(!a)
		return 0;
	var now, running = a.ctx && a.ctx.state === 'running';
	if(running !== a.wasRunning)
	{ // the clock changes with the state (the context's time runs from zero once it starts)
		a.wasRunning = running;
		a.nextTime = 0;
	}
	if(running)
		now = a.ctx.currentTime;
	else
		now = performance.now() / 1000; // silent: the wall clock paces the mixer
	if(a.nextTime < now)
		a.nextTime = now + 0.03; // an underrun (or the first call): start 30 ms ahead
	var need = now + a.lead - a.nextTime;
	return need > 0 ? Math.floor(need * a.rate) : 0;
});
// clang-format on

/* `frames` interleaved stereo frames (int16_t, left then right) at `pcm`,
 * scheduled right after the previous ones.  While the context is not
 * running the frames are dropped, but the schedule advances all the same. */
// clang-format off
EM_JS(void, WasmJs_AudioPush, (const int16_t* pcm, int frames), {
	var a = Module.bgiAudio;
	if(!a || frames <= 0)
		return;
	if(a.ctx && a.ctx.state === 'running')
	{
		var buf = a.ctx.createBuffer(2, frames, a.rate);
		var l = buf.getChannelData(0), r = buf.getChannelData(1);
		var base = pcm >> 1; // the byte address as an index into HEAP16
		for(var i = 0; i < frames; i++)
		{
			l[i] = HEAP16[base + i * 2] / 32768;
			r[i] = HEAP16[base + i * 2 + 1] / 32768;
		}
		var src = a.ctx.createBufferSource();
		src.buffer = buf;
		src.connect(a.ctx.destination);
		src.start(a.nextTime);
	}
	a.nextTime += frames / a.rate;
});
// clang-format on

int OS_AudioAvailable(void)
{
	return WasmJs_AudioAvailable();
}

/* Open the page's output at `rate` Hz and remember the mixer's callback;
 * the frames are pulled from OsWasm_AudioPump, not from a thread.  0 when
 * the output is open already, Web Audio is missing or the buffer could not
 * be allocated. */
int OS_AudioStart(int rate, OsAudioFill_t fill, void* user)
{
	if(gAudio.running)
		return 0;
	if(!WasmJs_AudioStart(rate))
		return 0;
	gAudio.buf = (int16_t*)malloc((size_t)AUDIO_PUMP_MAX * 2 * sizeof(int16_t));
	if(!gAudio.buf)
	{
		WasmJs_AudioStop();
		return 0;
	}
	gAudio.rate = rate;
	gAudio.fill = fill;
	gAudio.user = user;
	gAudio.running = 1;
	return 1;
}

// close the output; no callback runs after it returns (none runs between pumps anyway)
void OS_AudioStop(void)
{
	if(!gAudio.running)
		return;
	WasmJs_AudioStop();
	free(gAudio.buf);
	memset(&gAudio, 0, sizeof gAudio);
}

/* The pump: OsWasm_Yield (window.c) calls this before every return to the
 * browser.  It asks the page how many frames keep the output ahead and has
 * the mixer fill them, AUDIO_PUMP_MAX at a time. */
void OsWasm_AudioPump(void)
{
	int frames;
	if(!gAudio.running)
		return;
	frames = WasmJs_AudioWanted();
	while(frames > 0)
	{
		int n = frames > AUDIO_PUMP_MAX ? AUDIO_PUMP_MAX : frames;
		gAudio.fill(gAudio.user, gAudio.buf, n);
		WasmJs_AudioPush(gAudio.buf, n);
		frames -= n;
	}
}

// ---- MCI, PlaySound, movies: not on a page ---------------------------------------------------

// every call fails as os.h allows: no device (0), OS_MOVIE_FAILED, a movie that is not running

uint32_t OS_MciOpenCdAudio(const char* element)
{
	BGI_UNUSED(element);
	return 0;
}

int OS_MciSetTimeFormatTmsf(uint32_t dev)
{
	BGI_UNUSED(dev);
	return 0;
}

int OS_MciClose(uint32_t dev)
{
	BGI_UNUSED(dev);
	return 0;
}

int OS_MciStatus(uint32_t dev, uint32_t item, uint32_t* out)
{
	BGI_UNUSED(dev);
	BGI_UNUSED(item);
	*out = 0;
	return 0;
}

int OS_MciPlay(uint32_t dev, uint32_t from, uint32_t to, int notify)
{
	BGI_UNUSED(dev);
	BGI_UNUSED(from);
	BGI_UNUSED(to);
	BGI_UNUSED(notify);
	return 0;
}

int OS_MciStop(uint32_t dev)
{
	BGI_UNUSED(dev);
	return 0;
}

int OS_PlaySoundFile(const char* path)
{
	BGI_UNUSED(path);
	return 0;
}

int OS_MovieOpen(const char* path)
{
	BGI_UNUSED(path);
	return OS_MOVIE_FAILED;
}

int OS_MovieStart(int x, int y, int w, int h, int32_t volume, int32_t* lengthMs)
{
	BGI_UNUSED(x);
	BGI_UNUSED(y);
	BGI_UNUSED(w);
	BGI_UNUSED(h);
	BGI_UNUSED(volume);
	*lengthMs = 0;
	return 0;
}

void OS_MovieStop(void)
{
}

int OS_MovieIsRunning(void)
{
	return 0;
}

void OS_MovieSetVolume(int32_t vol)
{
	BGI_UNUSED(vol);
}

// no helper programs in the browser: the movie player's ffmpeg route is unavailable
OsPipeProc_t* OS_PipeProcStart(const char* const* argv)
{
	BGI_UNUSED(argv);
	return NULL;
}

int OS_PipeProcRead(OsPipeProc_t* p, void* buf, int n)
{
	BGI_UNUSED(p);
	BGI_UNUSED(buf);
	BGI_UNUSED(n);
	return -1;
}

void OS_PipeProcStop(OsPipeProc_t* p)
{
	BGI_UNUSED(p);
}

void OS_PathNative(const char* path, char* out, size_t n)
{
	snprintf(out, n, "%s", path);
}
