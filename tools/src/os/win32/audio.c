/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * audio.c - the Win32 back end: PCM output, CD audio through MCI, PlaySound,
 *           the DirectSound presence check and the DirectShow movie graph
 *           (inc/bgi/os.h)
 *
 * The original's sound library ("Wave Master") streams into DirectSound
 * buffers; the reimplementation mixes in software (src/snd/wm/) and only
 * needs a device that pulls 16-bit stereo PCM, which waveOut provides
 * here with four 20 ms buffers refilled from a thread.  The original's
 * DirectSound 8 check (creating the object and releasing it again) is
 * kept as the "audio available" test since the engine refuses to start
 * without it.
 *
 * The movie player is the original's DirectShow code call for call: the
 * filter graph with IGraphBuilder::RenderFile, the video in a child
 * window of the main window and the graph's events delivered as WM_APP,
 * which the main window procedure (wndproc.c) answers by draining them
 * through OsWin32_MovieDrainEvents.  Everything here runs on the main
 * thread except WaveThread.
 */
#include "bgi/os_win32.h"
#include <mmsystem.h>
#include <objbase.h>
#include <dshow.h>

#include "bgi/os.h"

// -------------------------------------------------------------------------
// PCM output: waveOut with a refill thread
// -------------------------------------------------------------------------

#define WO_BUFFERS   4  // buffers in flight
#define WO_PERIOD_MS 20 // the length of each (ms)

static HWAVEOUT gWaveOut;              // the open device; NULL while stopped
static WAVEHDR gWaveHdr[WO_BUFFERS];   // one header per buffer, prepared while the device is open
static int16_t* gWaveData[WO_BUFFERS]; // the buffers' samples (interleaved stereo)
static int gWaveFrames;                // frames per buffer (rate / 50)
static HANDLE gWaveEvent;              // signalled by the driver when a buffer is done (CALLBACK_EVENT)
static HANDLE gWaveThread;             // the refill thread
static volatile LONG gWaveStop;        // 1 asks the thread to end
static OsAudioFill_t gWaveFill;        // the mixer's callback and its argument
static void* gWaveUser;

static void WaveQueue(int i) // mix one buffer and hand it to the device
{
	gWaveFill(gWaveUser, gWaveData[i], gWaveFrames);
	gWaveHdr[i].dwFlags &= ~WHDR_DONE;
	waveOutWrite(gWaveOut, &gWaveHdr[i], sizeof gWaveHdr[i]);
}

// the refill thread: wait for a done buffer (or two periods, so a missed event cannot starve the device) and requeue every finished one
static DWORD WINAPI WaveThread(LPVOID arg)
{
	BGI_UNUSED(arg);
	while(!gWaveStop)
	{
		int i;
		WaitForSingleObject(gWaveEvent, WO_PERIOD_MS * 2);
		if(gWaveStop)
			break;
		for(i = 0; i < WO_BUFFERS; i++)
			if(gWaveHdr[i].dwFlags & WHDR_DONE)
				WaveQueue(i);
	}
	return 0;
}

/* Open the wave mapper for 16-bit stereo at `rate` Hz, prime the four
 * buffers through `fill` and start the refill thread at time-critical
 * priority; a running output is stopped first.  0 when the event or the
 * device cannot be created (nothing is left open). */
int OS_AudioStart(int rate, OsAudioFill_t fill, void* user)
{
	WAVEFORMATEX fmt;
	int i;
	if(gWaveOut)
		OS_AudioStop();
	gWaveFill = fill;
	gWaveUser = user;
	gWaveFrames = rate / (1000 / WO_PERIOD_MS);
	memset(&fmt, 0, sizeof fmt);
	fmt.wFormatTag = WAVE_FORMAT_PCM;
	fmt.nChannels = 2;
	fmt.nSamplesPerSec = (DWORD)rate;
	fmt.wBitsPerSample = 16;
	fmt.nBlockAlign = 4;
	fmt.nAvgBytesPerSec = (DWORD)rate * 4;
	gWaveEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
	if(!gWaveEvent)
		return 0;
	if(waveOutOpen(&gWaveOut, WAVE_MAPPER, &fmt, (DWORD_PTR)gWaveEvent, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR)
	{
		CloseHandle(gWaveEvent);
		gWaveEvent = NULL;
		gWaveOut = NULL;
		return 0;
	}
	for(i = 0; i < WO_BUFFERS; i++)
	{
		gWaveData[i] = (int16_t*)BGI_Alloc((size_t)gWaveFrames * 4);
		memset(&gWaveHdr[i], 0, sizeof gWaveHdr[i]);
		gWaveHdr[i].lpData = (LPSTR)gWaveData[i];
		gWaveHdr[i].dwBufferLength = (DWORD)gWaveFrames * 4;
		waveOutPrepareHeader(gWaveOut, &gWaveHdr[i], sizeof gWaveHdr[i]);
	}
	gWaveStop = 0;
	for(i = 0; i < WO_BUFFERS; i++) // prime the queue
		WaveQueue(i);
	gWaveThread = CreateThread(NULL, 0, WaveThread, NULL, 0, NULL);
	if(gWaveThread)
		SetThreadPriority(gWaveThread, THREAD_PRIORITY_TIME_CRITICAL);
	return 1;
}

/* Stop the thread (asked to end, woken, waited for up to a second), reset
 * the device so every buffer comes back, free the buffers and close the
 * device and the event.  No fill callback runs after this returns. */
void OS_AudioStop(void)
{
	int i;
	if(!gWaveOut)
		return;
	InterlockedExchange(&gWaveStop, 1);
	if(gWaveThread)
	{
		SetEvent(gWaveEvent);
		WaitForSingleObject(gWaveThread, 1000);
		CloseHandle(gWaveThread);
		gWaveThread = NULL;
	}
	waveOutReset(gWaveOut);
	for(i = 0; i < WO_BUFFERS; i++)
	{
		waveOutUnprepareHeader(gWaveOut, &gWaveHdr[i], sizeof gWaveHdr[i]);
		BGI_Free(gWaveData[i]);
		gWaveData[i] = NULL;
	}
	waveOutClose(gWaveOut);
	gWaveOut = NULL;
	CloseHandle(gWaveEvent);
	gWaveEvent = NULL;
}

// 1 when CoCreateInstance(CLSID_DirectSound8, .., IID_IDirectSound8) succeeds (the object is released at once): DirectSound 8 is installed
int OS_AudioAvailable(void)
{
	// {3901CC3F-84B5-4FA4-BA35-AA8172B8A09B} / {C50A7E93-F395-4834-9EF6-7FA99DE50966}
	static const GUID clsidDirectSound8 = {0x3901cc3f, 0x84b5, 0x4fa4, {0xba, 0x35, 0xaa, 0x81, 0x72, 0xb8, 0xa0, 0x9b}};
	static const GUID iidDirectSound8 = {0xc50a7e93, 0xf395, 0x4834, {0x9e, 0xf6, 0x7f, 0xa9, 0x9d, 0xe5, 0x09, 0x66}};
	IUnknown* p = NULL;
	if(FAILED(CoCreateInstance(&clsidDirectSound8, NULL, CLSCTX_INPROC_SERVER, &iidDirectSound8, (void**)&p)) || !p)
		return 0;
	IUnknown_Release(p);
	return 1;
}

// -------------------------------------------------------------------------
// CD audio (mciSendCommand)
// -------------------------------------------------------------------------

// MCI_OPEN of the "cdaudio" device type, with the drive ("X:") as the element when given; the device id, 0 when the open fails
uint32_t OS_MciOpenCdAudio(const char* element)
{
	MCI_OPEN_PARMSW op;
	WCHAR wide[0x104];
	DWORD flags = MCI_OPEN_TYPE;
	memset(&op, 0, sizeof op);
	op.lpstrDeviceType = L"cdaudio";
	if(element && *element)
	{
		Win32_ToWide(element, wide, (int)BGI_COUNTOF(wide));
		op.lpstrElementName = wide; // "X:"
		flags |= MCI_OPEN_ELEMENT;
	}
	if(mciSendCommandW(0, MCI_OPEN, flags, (DWORD_PTR)&op) != 0)
		return 0;
	return (uint32_t)op.wDeviceID;
}

// MCI_SET MCI_SET_TIME_FORMAT to TMSF (track, minute, second, frame); 1 = ok
int OS_MciSetTimeFormatTmsf(uint32_t dev)
{
	MCI_SET_PARMS sp;
	memset(&sp, 0, sizeof sp);
	sp.dwTimeFormat = MCI_FORMAT_TMSF;
	return mciSendCommandW((MCIDEVICEID)dev, MCI_SET, MCI_SET_TIME_FORMAT, (DWORD_PTR)&sp) == 0;
}

int OS_MciClose(uint32_t dev) // MCI_CLOSE; 1 = ok
{
	MCI_GENERIC_PARMS gp;
	memset(&gp, 0, sizeof gp);
	return mciSendCommandW((MCIDEVICEID)dev, MCI_CLOSE, 0, (DWORD_PTR)&gp) == 0;
}

// MCI_STATUS of `item` (OS_MCI_STATUS_MODE, OS_MCI_STATUS_TRACKS, ..) into *out; 1 = ok
int OS_MciStatus(uint32_t dev, uint32_t item, uint32_t* out)
{
	MCI_STATUS_PARMS sp;
	memset(&sp, 0, sizeof sp);
	sp.dwItem = item;
	if(mciSendCommandW((MCIDEVICEID)dev, MCI_STATUS, MCI_STATUS_ITEM, (DWORD_PTR)&sp) != 0)
		return 0;
	*out = (uint32_t)sp.dwReturn;
	return 1;
}

// MCI_PLAY from `from` to `to` (in the device's time format); with `notify` the MM_MCINOTIFY goes to the main window, which raises `mci_notify`; 1 = ok
int OS_MciPlay(uint32_t dev, uint32_t from, uint32_t to, int notify)
{
	MCI_PLAY_PARMS pp;
	DWORD flags = MCI_FROM | MCI_TO | (notify ? MCI_NOTIFY : 0);
	memset(&pp, 0, sizeof pp);
	pp.dwCallback = (DWORD_PTR)gWin32MainWnd;
	pp.dwFrom = from;
	pp.dwTo = to;
	return mciSendCommandW((MCIDEVICEID)dev, MCI_PLAY, flags, (DWORD_PTR)&pp) == 0;
}

int OS_MciStop(uint32_t dev) // MCI_STOP; 1 = ok
{
	MCI_GENERIC_PARMS gp;
	memset(&gp, 0, sizeof gp);
	return mciSendCommandW((MCIDEVICEID)dev, MCI_STOP, 0, (DWORD_PTR)&gp) == 0;
}

// PlaySound of a file, asynchronous, nothing when the file is missing or a sound is busy (SND_NODEFAULT | SND_NOWAIT); 1 when accepted
int OS_PlaySoundFile(const char* path)
{
	WCHAR wide[WIN32_WPATH];
	Win32_ToWide(path, wide, (int)BGI_COUNTOF(wide));
	return PlaySoundW(wide, gWin32Instance, SND_FILENAME | SND_NODEFAULT | SND_ASYNC | SND_NOWAIT) != 0;
}

// -------------------------------------------------------------------------
// movies: the DirectShow graph
// -------------------------------------------------------------------------

// the graph of the open movie and the interfaces taken from it; all NULL without one
static IGraphBuilder* gGraph;
static IMediaControl* gControl;
static IMediaEventEx* gEvent;
static IMediaSeeking* gSeek;
static IBasicAudio* gAudio;
static IVideoWindow* gVideo;
static int gVideoShown; // the video window is owned by the main window and shown (OS_MovieStart .. the release)

// RenderFile results the original tells apart (vfwmsgs.h)
#define BGI_VFW_E_NO_AUDIO_HARDWARE  0x80040256 // no sound card: the movie can still play without sound
#define BGI_VFW_S_VIDEO_NOT_RENDERED 0x00040257 // a stream was left out, success otherwise
#define BGI_VFW_S_AUDIO_NOT_RENDERED 0x00040258

// the graph and the five interfaces; 0 when one is missing (what exists stays for the release)
static int GraphCreate(void)
{
	if(FAILED(CoCreateInstance(&CLSID_FilterGraph, NULL, CLSCTX_INPROC_SERVER, &IID_IGraphBuilder, (void**)&gGraph)))
		return 0;
	if(FAILED(IGraphBuilder_QueryInterface(gGraph, &IID_IMediaControl, (void**)&gControl)))
		return 0;
	if(FAILED(IGraphBuilder_QueryInterface(gGraph, &IID_IMediaEventEx, (void**)&gEvent)))
		return 0;
	if(FAILED(IGraphBuilder_QueryInterface(gGraph, &IID_IVideoWindow, (void**)&gVideo)))
		return 0;
	if(FAILED(IGraphBuilder_QueryInterface(gGraph, &IID_IMediaSeeking, (void**)&gSeek)))
		return 0;
	if(FAILED(IGraphBuilder_QueryInterface(gGraph, &IID_IBasicAudio, (void**)&gAudio)))
		return 0;
	return 1;
}

/* Release whatever GraphCreate took, in the original's order: the video
 * window is hidden and disowned before its interface goes, and the main
 * window is shown and brought to the front again (the video window had
 * the focus). */
static void GraphRelease(void)
{
	if(gAudio)
	{
		IBasicAudio_Release(gAudio);
		gAudio = NULL;
	}
	if(gSeek)
	{
		IMediaSeeking_Release(gSeek);
		gSeek = NULL;
	}
	if(gVideo)
	{
		IVideoWindow_put_Visible(gVideo, 0); // OAFALSE
		IVideoWindow_put_Owner(gVideo, (OAHWND)0);
		IVideoWindow_Release(gVideo);
		gVideo = NULL;
		if(gWin32MainWnd)
		{
			ShowWindow(gWin32MainWnd, SW_SHOWNORMAL);
			SetForegroundWindow(gWin32MainWnd);
		}
	}
	if(gEvent)
	{
		IMediaEventEx_Release(gEvent);
		gEvent = NULL;
	}
	if(gControl)
	{
		IMediaControl_Release(gControl);
		gControl = NULL;
	}
	if(gGraph)
	{
		IGraphBuilder_Release(gGraph);
		gGraph = NULL;
	}
	gVideoShown = 0;
}

/* "90 F0": build the graph for the file (RenderFile of the path converted
 * to UTF-16, wide.c); an open movie is stopped first.  The result
 * maps RenderFile's: S_OK → OS_MOVIE_OK, the two partial successes and
 * the missing sound card → their OS_MOVIE_* codes with the graph kept,
 * anything else → OS_MOVIE_FAILED with the graph released. */
int OS_MovieOpen(const char* path)
{
	WCHAR wide[WIN32_WPATH];
	HRESULT hr;
	if(gGraph)
		OS_MovieStop();
	if(!GraphCreate())
	{
		GraphRelease();
		return OS_MOVIE_FAILED;
	}
	Win32_ToWide(path, wide, (int)BGI_COUNTOF(wide));
	hr = IGraphBuilder_RenderFile(gGraph, wide, NULL);
	switch((uint32_t)hr)
	{
		case 0: return OS_MOVIE_OK;
		case BGI_VFW_E_NO_AUDIO_HARDWARE: return OS_MOVIE_NO_AUDIO_DEVICE;
		case BGI_VFW_S_VIDEO_NOT_RENDERED: return OS_MOVIE_VIDEO_NOT_RENDERED;
		case BGI_VFW_S_AUDIO_NOT_RENDERED: return OS_MOVIE_AUDIO_NOT_RENDERED;
		default:
			GraphRelease();
			return OS_MOVIE_FAILED;
	}
}

/* Show the video as a child of the main window at the client rectangle
 * (x, y, w, h), set the volume (IBasicAudio: -10000 .. 0), direct the
 * graph's events to the main window as WM_APP, run, and report the length
 * in ms (the stop position converted to media time).  0 without a complete
 * graph or when a video window call fails. */
int OS_MovieStart(int x, int y, int w, int h, int32_t volume, int32_t* lengthMs)
{
	LONGLONG earliest = 0, latest = 0, stop = 0, converted = 0;
	GUID format;
	if(!gGraph || !gSeek || !gAudio || !gVideo || !gEvent || !gControl)
		return 0;
	IMediaSeeking_GetAvailable(gSeek, &earliest, &latest); // asked for, never used, as in the original
	IBasicAudio_put_Volume(gAudio, volume);
	if(FAILED(IVideoWindow_put_Owner(gVideo, (OAHWND)gWin32MainWnd)))
		return 0;
	if(FAILED(IVideoWindow_put_WindowStyle(gVideo, WS_CHILD | WS_CLIPSIBLINGS)))
		return 0;
	if(FAILED(IVideoWindow_SetWindowPosition(gVideo, x, y, w, h)))
		return 0;
	if(FAILED(IMediaEventEx_SetNotifyWindow(gEvent, (OAHWND)gWin32MainWnd, WM_APP, 0)))
		return 0;
	gVideoShown = 1;
	IMediaControl_Run(gControl);
	// the stop position in media time (100 ns units) -> ms
	IMediaSeeking_GetStopPosition(gSeek, &stop);
	IMediaSeeking_GetTimeFormat(gSeek, &format);
	IMediaSeeking_ConvertTimeFormat(gSeek, &converted, &TIME_FORMAT_MEDIA_TIME, stop, &format);
	*lengthMs = (int32_t)(converted / 10000);
	return 1;
}

// stop the graph and release everything (the video window goes with it); harmless without a graph
void OS_MovieStop(void)
{
	if(gControl)
		IMediaControl_Stop(gControl); // the original stops only while playing; stopping a stopped graph is a no-op
	GraphRelease();
}

int OS_MovieIsRunning(void) // current position < stop position; 0 without a graph
{
	LONGLONG cur = 0, stop = 0;
	if(!gSeek)
		return 0;
	IMediaSeeking_GetPositions(gSeek, &cur, &stop);
	return cur < stop;
}

void OS_MovieSetVolume(int32_t vol) // IBasicAudio::put_Volume (-10000 .. 0); ignored without a graph
{
	if(gAudio)
		IBasicAudio_put_Volume(gAudio, vol);
}

// take every pending event off the graph (freeing its parameters) and report whether an EC_COMPLETE was among them; 0 without a graph
int OsWin32_MovieDrainEvents(void)
{
	long code = 0;
	LONG_PTR p1 = 0, p2 = 0;
	int completed = 0;
	if(!gEvent)
		return 0;
	while(SUCCEEDED(IMediaEventEx_GetEvent(gEvent, &code, &p1, &p2, 0)))
	{
		IMediaEventEx_FreeEventParams(gEvent, code, p1, p2);
		if(code == EC_COMPLETE)
			completed = 1;
	}
	return completed;
}

int OsWin32_MovieWindowShown(void)
{
	return gVideoShown;
}
