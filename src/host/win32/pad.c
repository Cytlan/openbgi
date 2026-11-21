/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * pad.c - the pads of the Win32 back end (OS_PadPoll of host/os.h):
 *         XInput, and the joystick API for the pads XInput does not serve
 *
 * XInput's library is looked for when the pads are first asked for
 * (xinput1_4.dll of Windows 8 on, then the older ones); a system without
 * one has the joystick API alone.  Every two seconds the four slots of
 * XInput and the sixteen joysticks of the multimedia API (joyGetPosEx)
 * that are on no port yet are looked at, and one that answers gets the
 * first free port of four.  XInput's pads show up as joysticks as well:
 * a joystick whose HID device XInput serves (the device's path says so,
 * OsPads_PathIsXInput) is left to XInput.  Each call asks the pads on the
 * ports for their state, and host/os_pads.c makes it the engine's: an
 * XInput pad by what its controls are, a joystick by its buttons'
 * numbers, as DirectInput numbers them.  A pad that stops answering frees
 * its port.
 */
#include "sys_internal.h"
#include "host/os_pads.h"

#define PORTS     4
#define RESCAN_MS 2000 // the slots and joysticks without a port are asked this often
#define XI_SLOTS  4    // XUSER_MAX_COUNT
#define JOYS      16   // the joysticks of the multimedia API (joyGetNumDevs)

enum
{
	SOURCE_NONE, // the port is free
	SOURCE_XINPUT,
	SOURCE_JOYSTICK
};

// XINPUT_STATE, as XInputGetState fills it (the library is loaded by name, so its header is not needed)
typedef struct XiState
{
	DWORD packet;
	WORD buttons;
	BYTE leftTrigger, rightTrigger;
	SHORT lx, ly, rx, ry;
} XiState_t;
typedef DWORD(WINAPI* XiGetState_t)(DWORD slot, XiState_t* state);

typedef struct Pad
{
	int source;               // SOURCE_*
	UINT index;               // XInput's slot, or the joystick's number
	JOYCAPSW caps;            // a joystick: its axes' ranges and what it has
	OpenBGI_PadState_t state; // what the engine gets
} Pad_t;

static Pad_t gPads[PORTS];
static uint32_t gScanAt;    // when the slots and joysticks were looked at last (OS_TicksMs)
static int gScanned;        // .. at all
static int gXiLooked;       // the library was looked for
static HMODULE gXiLibrary;  // XInput's library; NULL without one
static XiGetState_t gXiGet; // its XInputGetState

static void LoadXInput(void)
{
	static const WCHAR* const kNames[] = {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"};
	size_t i;
	if(gXiLooked)
		return;
	gXiLooked = 1;
	for(i = 0; i < sizeof kNames / sizeof kNames[0] && !gXiGet; i++)
	{
		gXiLibrary = LoadLibraryW(kNames[i]);
		if(!gXiLibrary)
			continue;
		gXiGet = (XiGetState_t)(void (*)(void))GetProcAddress(gXiLibrary, "XInputGetState");
		if(!gXiGet)
		{
			FreeLibrary(gXiLibrary);
			gXiLibrary = NULL;
		}
	}
}

// 1 when XInput serves the HID device of this vendor and product
static int ServedByXInput(UINT vendor, UINT product)
{
	RAWINPUTDEVICELIST* list;
	UINT count = 0, i;
	int served = 0;
	if(GetRawInputDeviceList(NULL, &count, sizeof list[0]) != 0 || !count)
		return 0;
	list = (RAWINPUTDEVICELIST*)malloc(count * sizeof list[0]);
	if(!list)
		return 0;
	count = GetRawInputDeviceList(list, &count, sizeof list[0]);
	if(count == (UINT)-1)
		count = 0;
	for(i = 0; i < count && !served; i++)
	{
		RID_DEVICE_INFO info;
		WCHAR wide[512];
		char path[512];
		UINT size = sizeof info, n;
		if(list[i].dwType != RIM_TYPEHID)
			continue;
		memset(&info, 0, sizeof info);
		info.cbSize = sizeof info;
		if(GetRawInputDeviceInfoW(list[i].hDevice, RIDI_DEVICEINFO, &info, &size) == (UINT)-1)
			continue;
		if(info.hid.dwVendorId != vendor || info.hid.dwProductId != product)
			continue;
		size = sizeof wide / sizeof wide[0] - 1; // (characters, for the name)
		memset(wide, 0, sizeof wide);
		if(GetRawInputDeviceInfoW(list[i].hDevice, RIDI_DEVICENAME, wide, &size) == (UINT)-1)
			continue;
		for(n = 0; n < sizeof path - 1 && wide[n]; n++)
			path[n] = wide[n] < 128 ? (char)wide[n] : '?';
		path[n] = 0;
		served = OsPads_PathIsXInput(path);
	}
	free(list);
	return served;
}

// the pad's state now; 0 when it does not answer (any more)
static int Read(const Pad_t* p, OpenBGI_PadState_t* out)
{
	if(p->source == SOURCE_XINPUT)
	{
		XiState_t st;
		memset(&st, 0, sizeof st);
		if(!gXiGet || gXiGet(p->index, &st) != ERROR_SUCCESS)
			return 0;
		OsPads_XInput(st.buttons, st.leftTrigger, st.rightTrigger, st.lx, st.ly, st.rx, st.ry, out);
		return 1;
	}
	if(p->source == SOURCE_JOYSTICK)
	{
		const JOYCAPSW* c = &p->caps;
		JOYINFOEX info;
		uint32_t pos[6], min[6], max[6];
		memset(&info, 0, sizeof info);
		info.dwSize = sizeof info;
		info.dwFlags = JOY_RETURNX | JOY_RETURNY | JOY_RETURNZ | JOY_RETURNR | JOY_RETURNU | JOY_RETURNV | JOY_RETURNPOVCTS | JOY_RETURNBUTTONS;
		if(joyGetPosEx(p->index, &info) != JOYERR_NOERROR)
			return 0;
		pos[0] = info.dwXpos, min[0] = c->wXmin, max[0] = c->wXmax;
		pos[1] = info.dwYpos, min[1] = c->wYmin, max[1] = c->wYmax;
		pos[2] = info.dwZpos, min[2] = c->wZmin, max[2] = c->wZmax;
		pos[3] = info.dwRpos, min[3] = c->wRmin, max[3] = c->wRmax;
		pos[4] = info.dwUpos, min[4] = c->wUmin, max[4] = c->wUmax;
		pos[5] = info.dwVpos, min[5] = c->wVmin, max[5] = c->wVmax;
		OsPads_Joystick(info.dwButtons, c->wCaps, info.dwPOV, pos, min, max, out);
		return 1;
	}
	return 0;
}

static int OnPort(int source, UINT index)
{
	int port;
	for(port = 0; port < PORTS; port++)
		if(gPads[port].source == source && gPads[port].index == index)
			return 1;
	return 0;
}

// the pad onto the first free port when it answers; the port's bit, 0 when there was none
static uint32_t Take(int source, UINT index, const JOYCAPSW* caps)
{
	int port;
	for(port = 0; port < PORTS && gPads[port].source != SOURCE_NONE; port++)
	{
	}
	if(port == PORTS)
		return 0;
	memset(&gPads[port], 0, sizeof gPads[port]);
	gPads[port].source = source;
	gPads[port].index = index;
	if(caps)
		gPads[port].caps = *caps;
	if(!Read(&gPads[port], &gPads[port].state))
	{
		gPads[port].source = SOURCE_NONE;
		return 0;
	}
	return 1u << port;
}

// the slots and joysticks that are on no port: those that answer onto the free ports; a bit per port taken
static uint32_t Scan(void)
{
	uint32_t taken = 0;
	UINT i, joys;
	LoadXInput();
	for(i = 0; gXiGet && i < XI_SLOTS; i++)
		if(!OnPort(SOURCE_XINPUT, i))
			taken |= Take(SOURCE_XINPUT, i, NULL);
	joys = joyGetNumDevs();
	for(i = 0; i < joys && i < JOYS; i++)
	{
		JOYCAPSW caps;
		JOYINFOEX info;
		if(OnPort(SOURCE_JOYSTICK, i))
			continue;
		memset(&caps, 0, sizeof caps);
		memset(&info, 0, sizeof info);
		info.dwSize = sizeof info;
		info.dwFlags = JOY_RETURNBUTTONS;
		if(joyGetDevCapsW(i, &caps, sizeof caps) != JOYERR_NOERROR || joyGetPosEx(i, &info) != JOYERR_NOERROR)
			continue;
		if(gXiGet && ServedByXInput(caps.wMid, caps.wPid))
			continue; // XInput has it
		taken |= Take(SOURCE_JOYSTICK, i, &caps);
	}
	return taken;
}

uint32_t OS_PadPoll(void)
{
	uint32_t changed = 0, now = OS_TicksMs();
	int port;
	if(!gScanned || now - gScanAt >= RESCAN_MS)
	{
		gScanned = 1;
		gScanAt = now;
		changed |= Scan();
	}
	for(port = 0; port < PORTS; port++)
	{
		Pad_t* p = &gPads[port];
		OpenBGI_PadState_t now;
		if(p->source == SOURCE_NONE || (changed & (1u << port)))
			continue; // (one taken just now was read then)
		if(!Read(p, &now))
		{ // the pad went
			p->source = SOURCE_NONE;
			changed |= 1u << port;
		}
		else if(memcmp(&now, &p->state, sizeof now) != 0)
		{
			p->state = now;
			changed |= 1u << port;
		}
	}
	return changed;
}

const OpenBGI_PadState_t* OS_PadState(int port)
{
	return port >= 0 && port < PORTS && gPads[port].source != SOURCE_NONE ? &gPads[port].state : NULL;
}

void OS_PadShutdown(void)
{
	memset(gPads, 0, sizeof gPads);
	gScanned = 0;
	if(gXiLibrary)
		FreeLibrary(gXiLibrary);
	gXiLibrary = NULL;
	gXiGet = NULL;
	gXiLooked = 0;
}
