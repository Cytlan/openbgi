/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * pad.c - the pads of the POSIX back end (OS_PadPoll of host/os.h): on
 *         Linux the kernel's event devices; elsewhere none
 *
 * Every two seconds the event devices of /dev/input (BGI_PAD_DIR names
 * another directory; an empty value turns the pads off) that are not open
 * yet are looked at: one that is a gamepad or a joystick
 * (OsPads_EvdevIsPad, by its buttons and axes) and that may be read gets
 * the first free port of four.  A device that is no pad is remembered and
 * not opened again until the directory changes.  Each call reads what the
 * open devices reported without waiting, and host/os_pads.c makes the
 * events the engine's pad state.  A device that went away frees its port.
 *
 * The devices are read directly, without a library: a user can read the
 * pads' event devices where the system gives the seat's user access to
 * them (the "uaccess" tag of udev's joystick rule, as the desktop
 * distributions have it) or as a member of the group "input".
 */
#include "host/os.h"
#include "host/os_pads.h"

#include <string.h>

#define PORTS 4

#if defined(__linux__) && !defined(__ANDROID__)

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/input.h>

#define RESCAN_MS 2000 // the directory is looked at again this often
#define NODES     256  // event devices told apart ("event0" .. "event255")

typedef struct Pad
{
	int fd;            // -1: the port is free
	int node;          // its device, "event<node>"
	OsPadsEvdev_t dev; // what it is, and its state
} Pad_t;

static Pad_t gPads[PORTS];
static int gReady;                 // gPads is set up
static uint32_t gScanAt;           // when the directory was looked at last (OS_TicksMs)
static int gScanned;               // .. at all
static uint8_t gNotPad[NODES / 8]; // devices seen to be no pad (or not readable), until the directory changes
static time_t gDirTime;            // the directory's modification time then
static long gDirTimeNs;

static const char* Dir(void)
{
	const char* d = getenv("BGI_PAD_DIR");
	return d ? d : "/dev/input";
}

static int Bit(const uint8_t* bits, int n)
{
	return (bits[n / 8] >> (n % 8)) & 1;
}

// the device's keys and axes as they are now, through the decoder (when it is opened, and after events were lost)
static void Sync(Pad_t* p)
{
	uint8_t keys[OSPADS_KEY_BYTES];
	struct input_absinfo info;
	int i;
	OsPads_EvdevReset(&p->dev);
	memset(keys, 0, sizeof keys);
	if(ioctl(p->fd, EVIOCGKEY(sizeof keys), keys) >= 0)
		for(i = 0; i < OSPADS_KEY_BYTES * 8; i++)
			if(Bit(keys, i))
				OsPads_EvdevEvent(&p->dev, EV_KEY, i, 1);
	for(i = 0; i < OSPADS_ABS_COUNT; i++)
		if(p->dev.abs[i].has && ioctl(p->fd, EVIOCGABS(i), &info) >= 0)
			OsPads_EvdevEvent(&p->dev, EV_ABS, i, info.value);
}

// device `node` on a free port when it is a pad that can be read; 1 when it was taken
static int Open(int node)
{
	char path[0x200], name[128];
	uint8_t keyBits[OSPADS_KEY_BYTES], absBits[OSPADS_ABS_COUNT / 8];
	OsPadsAxis_t abs[OSPADS_ABS_COUNT];
	struct input_id id;
	int fd, i, port;
	for(port = 0; port < PORTS && gPads[port].fd >= 0; port++)
	{
	}
	if(port == PORTS)
		return 0;
	snprintf(path, sizeof path, "%s/event%d", Dir(), node);
	fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if(fd < 0)
		return 0;
	memset(keyBits, 0, sizeof keyBits);
	memset(absBits, 0, sizeof absBits);
	memset(abs, 0, sizeof abs);
	memset(&id, 0, sizeof id);
	name[0] = 0;
	if(ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keyBits), keyBits) < 0 || ioctl(fd, EVIOCGBIT(EV_ABS, sizeof absBits), absBits) < 0 ||
		!OsPads_EvdevIsPad(keyBits, absBits))
	{
		close(fd);
		return 0;
	}
	for(i = 0; i < OSPADS_ABS_COUNT; i++)
	{
		struct input_absinfo info;
		if(Bit(absBits, i) && ioctl(fd, EVIOCGABS(i), &info) >= 0)
		{
			abs[i].has = 1;
			abs[i].min = info.minimum;
			abs[i].max = info.maximum;
		}
	}
	if(ioctl(fd, EVIOCGNAME(sizeof name - 1), name) < 0)
		name[0] = 0;
	name[sizeof name - 1] = 0;
	if(ioctl(fd, EVIOCGID, &id) < 0)
		memset(&id, 0, sizeof id);
	gPads[port].fd = fd;
	gPads[port].node = node;
	OsPads_EvdevInit(&gPads[port].dev, keyBits, abs, name, id.vendor);
	Sync(&gPads[port]);
	return 1;
}

// the event devices that are not open yet: the pads among them onto the free ports; a bit per port taken
static uint32_t Scan(void)
{
	struct stat st;
	struct dirent* e;
	DIR* d;
	uint32_t taken = 0;
	int port;
	if(!*Dir())
		return 0;
	if(stat(Dir(), &st) == 0 && (st.st_mtime != gDirTime || st.st_mtim.tv_nsec != gDirTimeNs))
	{ // a device came or went: every one may be another now
		gDirTime = st.st_mtime;
		gDirTimeNs = st.st_mtim.tv_nsec;
		memset(gNotPad, 0, sizeof gNotPad);
	}
	d = opendir(Dir());
	if(!d)
		return 0;
	while((e = readdir(d)) != NULL)
	{
		char* end;
		long node;
		if(strncmp(e->d_name, "event", 5) != 0 || e->d_name[5] < '0' || e->d_name[5] > '9')
			continue;
		node = strtol(e->d_name + 5, &end, 10);
		if(*end || node < 0 || node >= NODES || Bit(gNotPad, (int)node))
			continue;
		for(port = 0; port < PORTS && !(gPads[port].fd >= 0 && gPads[port].node == node); port++)
		{
		}
		if(port < PORTS)
			continue; // open already
		if(Open((int)node))
		{
			for(port = 0; port < PORTS; port++)
				if(gPads[port].fd >= 0 && gPads[port].node == node)
					taken |= 1u << port;
		}
		else
			gNotPad[node / 8] |= (uint8_t)(1 << (node % 8));
	}
	closedir(d);
	return taken;
}

uint32_t OS_PadPoll(void)
{
	uint32_t changed = 0, now = OS_TicksMs();
	int port;
	if(!gReady)
	{
		for(port = 0; port < PORTS; port++)
			gPads[port].fd = -1;
		gReady = 1;
	}
	if(!gScanned || now - gScanAt >= RESCAN_MS)
	{
		gScanned = 1;
		gScanAt = now;
		changed |= Scan();
	}
	for(port = 0; port < PORTS; port++)
	{
		Pad_t* p = &gPads[port];
		struct input_event ev[32];
		ssize_t n;
		if(p->fd < 0)
			continue;
		while((n = read(p->fd, ev, sizeof ev)) > 0)
		{
			size_t i, count = (size_t)n / sizeof ev[0];
			for(i = 0; i < count; i++)
			{
				if(ev[i].type == EV_SYN && ev[i].code == SYN_DROPPED)
				{ // events were lost: the state as the device has it
					OpenBGI_PadState_t was = p->dev.state;
					Sync(p);
					changed |= memcmp(&was, &p->dev.state, sizeof was) != 0 ? 1u << port : 0;
				}
				else if(OsPads_EvdevEvent(&p->dev, ev[i].type, ev[i].code, ev[i].value))
					changed |= 1u << port;
			}
		}
		if(n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
		{ // the device went
			close(p->fd);
			p->fd = -1;
			changed |= 1u << port;
			gScanned = 0; // (and what took its place is looked for at once)
		}
	}
	return changed;
}

const OpenBGI_PadState_t* OS_PadState(int port)
{
	return gReady && port >= 0 && port < PORTS && gPads[port].fd >= 0 ? &gPads[port].dev.state : NULL;
}

void OS_PadShutdown(void)
{
	int port;
	for(port = 0; gReady && port < PORTS; port++)
		if(gPads[port].fd >= 0)
		{
			close(gPads[port].fd);
			gPads[port].fd = -1;
		}
	gScanned = 0;
	memset(gNotPad, 0, sizeof gNotPad);
}

#else // no pads here (Haiku, macOS, Android)

uint32_t OS_PadPoll(void)
{
	return 0;
}

const OpenBGI_PadState_t* OS_PadState(int port)
{
	BGI_UNUSED(port);
	return NULL;
}

void OS_PadShutdown(void)
{
}

#endif
