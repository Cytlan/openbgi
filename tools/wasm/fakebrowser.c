/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * fakebrowser.c - runs the WebAssembly back end on a host without a browser
 *
 * `make wasm-sim` links the engine with src/os/wasm/, tools/wasm/hostmock.c
 * (the asset server's half, on a local directory) and this file, which
 * stands in for the JavaScript side: the canvas is a buffer written out as
 * a picture, the clock is the host's, the sound goes to a WAV file when
 * asked, the input comes from a script.  Nothing of Emscripten is involved,
 * so the simulation covers the back end's own logic - paths, the overlay,
 * the cache, the presentation, the pump, the fonts' use - end to end with
 * real game data, which is what can be verified without emcc (docs/wasm.md).
 *
 *   cd <game directory> && bin/openbgi-wasmsim [engine arguments]
 *
 * Environment:
 *   BGI_WASMSIM_SECONDS=N   end after N seconds of running (default: run on)
 *   BGI_WASMSIM_SHOT=FILE   write the canvas as a PPM when the run ends
 *   BGI_WASMSIM_WAV=FILE    write the mixed sound as a 16-bit stereo WAV
 *   BGI_WASMSIM_OVERLAY=DIR the overlay directory (default /tmp/openbgi-wasmsim-overlay)
 *   BGI_WASMSIM_SCRIPT=".." input: words `wait S` (seconds), `click X Y`,
 *                           `rclick X Y`, `move X Y`, `key VK`, `shot FILE`
 *   BGI_WASMSIM_ANSWER=0|1  what the dialogs answer (default 1: yes / ok)
 */
#include "bgi/os.h"
#include "bgi/os_wasm.h"
#include "tools/wasm/hostmock.h"

#include <time.h>
#include <unistd.h>

// ---- time -----------------------------------------------------------------------------------

static double NowMs(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

double emscripten_get_now(void)
{
	return NowMs();
}

static double gStartMs;
static int gDeadlineS = -1;
static const char* gShotFile;
static int gEnded;

// ---- the canvas -------------------------------------------------------------------------------

static uint8_t* gCanvas; // RGBA
static int gCanvasW, gCanvasH;

static void WriteShot(const char* file)
{
	FILE* f;
	int x, y;
	if(!gCanvas || !file)
		return;
	f = fopen(file, "wb");
	if(!f)
		return;
	fprintf(f, "P6\n%d %d\n255\n", gCanvasW, gCanvasH);
	for(y = 0; y < gCanvasH; y++)
		for(x = 0; x < gCanvasW; x++)
			fwrite(gCanvas + ((size_t)y * gCanvasW + x) * 4, 1, 3, f);
	fclose(f);
	fprintf(stderr, "wasmsim: wrote %s (%dx%d)\n", file, gCanvasW, gCanvasH);
}

void WasmJs_CanvasSetup(int w, int h, int visible, int fullscreen)
{
	if(w != gCanvasW || h != gCanvasH)
	{
		free(gCanvas);
		gCanvas = (uint8_t*)calloc((size_t)w * h, 4);
		gCanvasW = w;
		gCanvasH = h;
		fprintf(stderr, "wasmsim: canvas %dx%d%s%s\n", w, h, visible ? "" : " (hidden)", fullscreen ? " full screen" : "");
	}
}

void WasmJs_Present(const void* rgba, int w, int h, int x, int y)
{
	int row;
	if(!gCanvas)
		return;
	for(row = 0; row < h; row++)
	{
		int cy = y + row, cw = w;
		if(cy < 0 || cy >= gCanvasH || x >= gCanvasW)
			continue;
		if(x + cw > gCanvasW)
			cw = gCanvasW - x;
		if(cw > 0)
			memcpy(gCanvas + ((size_t)cy * gCanvasW + x) * 4, (const uint8_t*)rgba + (size_t)row * w * 4, (size_t)cw * 4);
	}
}

void WasmJs_FillBlack(void)
{
	if(gCanvas)
		memset(gCanvas, 0, (size_t)gCanvasW * gCanvasH * 4);
}

void WasmJs_SetTitle(const char* utf8)
{
	fprintf(stderr, "wasmsim: title \"%s\"\n", utf8);
}

void WasmJs_SetCursor(int visible)
{
	BGI_UNUSED(visible);
}

int WasmJs_ScreenW(void)
{
	return 1920;
}

int WasmJs_ScreenH(void)
{
	return 1080;
}

int WasmJs_Confirm(const char* text, const char* caption, int kind)
{
	const char* a = getenv("BGI_WASMSIM_ANSWER");
	fprintf(stderr, "wasmsim: %s [%s] %s\n", kind ? "confirm" : "alert", caption, text);
	return kind ? (a ? atoi(a) != 0 : 1) : 1;
}

int WasmJs_Prompt(const char* title, const char* initial, char* out, int n)
{
	BGI_UNUSED(initial);
	BGI_UNUSED(out);
	BGI_UNUSED(n);
	fprintf(stderr, "wasmsim: prompt [%s] cancelled\n", title);
	return 0;
}

void WasmJs_ClipboardWrite(const char* utf8)
{
	BGI_UNUSED(utf8);
}

void WasmJs_InstallInput(void)
{
}

void WasmJs_OpenUrl(const char* utf8)
{
	fprintf(stderr, "wasmsim: open %s\n", utf8);
}

double WasmJs_DeviceMemoryBytes(void)
{
	return 4.0 * 1024 * 1024 * 1024;
}

void WasmJs_Ended(int code)
{
	fprintf(stderr, "wasmsim: the engine ended (%d) after %.1f s\n", code, (NowMs() - gStartMs) / 1000.0);
	WriteShot(gShotFile);
	gEnded = 1;
}

// ---- fonts: a hollow box per glyph, so that text shows where it is ---------------------------------

typedef struct FakeFont
{
	int height, width;
} FakeFont_t;
static FakeFont_t gFonts[256];
static int gFontCount;

int WasmJs_FontOpen(const char* faceUtf8, int height, int width, int bold, int italic)
{
	BGI_UNUSED(faceUtf8);
	BGI_UNUSED(bold);
	BGI_UNUSED(italic);
	if(gFontCount == 256)
		return 0;
	gFonts[gFontCount].height = height;
	gFonts[gFontCount].width = width;
	return gFontCount++;
}

// the advance of the stand-in glyph: its half- or full-width cell
int WasmJs_FontAdvance(int id, int cp)
{
	if(id < 0 || id >= gFontCount)
		return 0;
	return cp < 0x80 ? gFonts[id].width : gFonts[id].width * 2;
}

void WasmJs_FontRender(int id, int cp, unsigned char* buf, int pitch, int w, int h, int mono)
{
	int gw, gh, x, y;
	if(id < 0 || id >= gFontCount)
		return;
	gw = (cp < 0x80 ? gFonts[id].width : gFonts[id].width * 2) - 1; // half- or full-width cell
	gh = gFonts[id].height - 1;
	if(gw > w)
		gw = w;
	if(gh > h)
		gh = h;
	for(y = 1; y < gh - 1; y++)
	{
		for(x = 1; x < gw - 1; x++)
		{
			int edge = y == 1 || y == gh - 2 || x == 1 || x == gw - 2;
			if(!edge)
				continue;
			if(mono)
				buf[y * pitch + (x >> 3)] |= (unsigned char)(0x80 >> (x & 7));
			else
				buf[y * pitch + x] = 255;
		}
	}
}

// ---- audio: paced by the clock, optionally written to a WAV file --------------------------------------

static struct
{
	int rate;
	double nextTime; // seconds
	FILE* wav;
	uint32_t frames;
} gAudio;

static void WavHeader(FILE* f, int rate, uint32_t frames)
{
	uint32_t dataBytes = frames * 4, v;
	uint16_t s;
	fseek(f, 0, SEEK_SET);
	fwrite("RIFF", 1, 4, f);
	v = 36 + dataBytes;
	fwrite(&v, 4, 1, f);
	fwrite("WAVEfmt ", 1, 8, f);
	v = 16;
	fwrite(&v, 4, 1, f);
	s = 1;
	fwrite(&s, 2, 1, f); // PCM
	s = 2;
	fwrite(&s, 2, 1, f); // stereo
	v = (uint32_t)rate;
	fwrite(&v, 4, 1, f);
	v = (uint32_t)rate * 4;
	fwrite(&v, 4, 1, f);
	s = 4;
	fwrite(&s, 2, 1, f);
	s = 16;
	fwrite(&s, 2, 1, f);
	fwrite("data", 1, 4, f);
	fwrite(&dataBytes, 4, 1, f);
}

int WasmJs_AudioAvailable(void)
{
	return 1;
}

int WasmJs_AudioStart(int rate)
{
	const char* wav = getenv("BGI_WASMSIM_WAV");
	gAudio.rate = rate;
	gAudio.nextTime = 0;
	if(wav)
	{
		gAudio.wav = fopen(wav, "wb");
		if(gAudio.wav)
			WavHeader(gAudio.wav, rate, 0);
	}
	return 1;
}

void WasmJs_AudioStop(void)
{
	if(gAudio.wav)
	{
		WavHeader(gAudio.wav, gAudio.rate, gAudio.frames);
		fclose(gAudio.wav);
		gAudio.wav = NULL;
	}
}

int WasmJs_AudioWanted(void)
{
	double now = NowMs() / 1000.0, need;
	if(gAudio.nextTime < now)
		gAudio.nextTime = now + 0.03;
	need = now + 0.2 - gAudio.nextTime;
	return need > 0 ? (int)(need * gAudio.rate) : 0;
}

void WasmJs_AudioPush(const int16_t* pcm, int frames)
{
	if(gAudio.wav)
		fwrite(pcm, 4, (size_t)frames, gAudio.wav);
	gAudio.frames += (uint32_t)frames;
	gAudio.nextTime += (double)frames / gAudio.rate;
}

// ---- the input script and the sleep --------------------------------------------------------------------

static const char* gScript;
static double gScriptWaitUntil; // ms
static struct
{
	int pending, button, x, y;
	double atMs;
} gRelease; // a click holds the button for a while, as a hand does and the scripts' polling needs

static void ReleaseDue(void)
{
	if(gRelease.pending && NowMs() >= gRelease.atMs)
	{
		gRelease.pending = 0;
		OsWasm_PushEvent(WASM_EV_BUTTON, gRelease.button, 0, gRelease.x, gRelease.y);
	}
}

// run the script up to its next wait; the words are consumed as they execute
static void RunScript(void)
{
	char word[64], a[260];
	int n, x, y;
	if(!gScript)
		return;
	while(NowMs() >= gScriptWaitUntil)
	{
		while(*gScript == ' ' || *gScript == ';')
			gScript++;
		if(!*gScript)
		{
			gScript = NULL;
			return;
		}
		if(sscanf(gScript, "%63s%n", word, &n) != 1)
			return;
		gScript += n;
		if(strcmp(word, "wait") == 0 && sscanf(gScript, "%d%n", &x, &n) == 1)
		{
			gScript += n;
			gScriptWaitUntil = NowMs() + x * 1000.0;
		}
		else if((strcmp(word, "click") == 0 || strcmp(word, "rclick") == 0) && sscanf(gScript, "%d %d%n", &x, &y, &n) == 2)
		{
			int b = word[0] == 'r';
			gScript += n;
			ReleaseDue();
			OsWasm_PushEvent(WASM_EV_MOUSE_MOVE, x, y, 0, 0);
			OsWasm_PushEvent(WASM_EV_BUTTON, b, 1, x, y);
			gRelease.pending = 1;
			gRelease.button = b;
			gRelease.x = x;
			gRelease.y = y;
			gRelease.atMs = NowMs() + 60;
			gScriptWaitUntil = NowMs() + 80; // the next word comes after the release
			fprintf(stderr, "wasmsim: %s %d,%d\n", word, x, y);
		}
		else if(strcmp(word, "move") == 0 && sscanf(gScript, "%d %d%n", &x, &y, &n) == 2)
		{
			gScript += n;
			OsWasm_PushEvent(WASM_EV_MOUSE_MOVE, x, y, 0, 0);
		}
		else if(strcmp(word, "key") == 0 && sscanf(gScript, "%i%n", &x, &n) == 1)
		{
			gScript += n;
			OsWasm_PushEvent(WASM_EV_KEY_DOWN, x, 0, 0, 0);
			OsWasm_PushEvent(WASM_EV_KEY_UP, x, 0, 0, 0); // (the key-down event itself is what the engine acts on)
			fprintf(stderr, "wasmsim: key 0x%02x\n", x);
		}
		else if(strcmp(word, "shot") == 0 && sscanf(gScript, "%259s%n", a, &n) == 1)
		{
			gScript += n;
			WriteShot(a);
		}
		else
		{
			fprintf(stderr, "wasmsim: bad script word \"%s\"\n", word);
			gScript = NULL;
			return;
		}
	}
}

// what every return to the "browser" does: the script, the deadline
static void BrowserTurn(void)
{
	ReleaseDue();
	RunScript();
	if(gDeadlineS >= 0 && NowMs() - gStartMs > gDeadlineS * 1000.0 && !gEnded)
	{
		fprintf(stderr, "wasmsim: %d s are up\n", gDeadlineS);
		WriteShot(gShotFile);
		WasmJs_AudioStop();
		fflush(stderr);
		_exit(0);
	}
}

void WasmJs_YieldFast(void)
{
	struct timespec ts = {0, 100000L}; // the browser would be back within a fraction of a millisecond
	BrowserTurn();
	nanosleep(&ts, NULL);
}

void emscripten_sleep(unsigned ms)
{
	struct timespec ts;
	BrowserTurn();
	if(ms == 0)
		ms = 1;
	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (long)(ms % 1000) * 1000000L;
	nanosleep(&ts, NULL);
}

// ---- set-up (runs before main through a constructor) ---------------------------------------------

__attribute__((constructor)) static void Setup(void)
{
	const char* overlay = getenv("BGI_WASMSIM_OVERLAY");
	const char* secs = getenv("BGI_WASMSIM_SECONDS");
	char cwd[0x400];
	gStartMs = NowMs();
	if(getcwd(cwd, sizeof cwd))
		HostMock_SetRoot(cwd);
	OsWasm_SetOverlayRoot(overlay ? overlay : "/tmp/openbgi-wasmsim-overlay");
	gShotFile = getenv("BGI_WASMSIM_SHOT");
	gScript = getenv("BGI_WASMSIM_SCRIPT");
	if(secs)
		gDeadlineS = atoi(secs);
}
