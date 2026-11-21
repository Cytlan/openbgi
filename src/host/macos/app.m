/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * app.m - the macOS back end's application: its menu, the event pump,
 *         the modal tick and the clipboard (src/host/os.h)
 *
 * The host's loop is the engine's (OS_RunEngine), so the application's
 * own loop never runs: OS_PumpMessages takes one event at a time and
 * hands it to AppKit, which calls the window's view (window.m).  What
 * AppKit tells a delegate from within other calls - the focus, a
 * resize, the close box - waits in a queue for the pump (Mac_Post).
 *
 * The application object is made when a window or a box first needs it,
 * so a start that is for one of the tools never has one.
 */
#import "macos_internal.h"

OsHostHandlers_t gMacIn;

static BOOL gAppReady;
static volatile sig_atomic_t gQuitPosted; // a signal asked for the quit: the next pump raises `quit`
static void (*gModalTick)(void* user);    // OS_SetModalTick
static void* gModalTickUser;
static NSTimer* gTickTimer;

// ---- the queue -----------------------------------------------------------------------------------

#define MAC_QUEUE 64

typedef struct MacEvent
{
	int type, a, b, c;
	char* path; // owned
} MacEvent_t;

static MacEvent_t gQueue[MAC_QUEUE];
static int gQueueHead, gQueueCount;

void Mac_Post(int type, int a, int b, int c, const char* path)
{
	MacEvent_t* e;
	if(gQueueCount == MAC_QUEUE)
		return; // (nobody pumps: the oldest are the ones that count)
	e = &gQueue[(gQueueHead + gQueueCount++) % MAC_QUEUE];
	e->type = type;
	e->a = a;
	e->b = b;
	e->c = c;
	e->path = path ? strdup(path) : NULL;
}

// one event of the queue to the host's handlers; 1 when there was one
static int Deliver(void)
{
	MacEvent_t e;
	if(!gQueueCount)
		return 0;
	e = gQueue[gQueueHead];
	gQueueHead = (gQueueHead + 1) % MAC_QUEUE;
	gQueueCount--;
	switch(e.type)
	{
		case MAC_EV_FOCUS:
			if(gMacIn.focus)
				gMacIn.focus(gMacIn.user, e.a);
			break;
		case MAC_EV_MINIMIZED:
			if(gMacIn.minimized)
				gMacIn.minimized(gMacIn.user, e.a);
			break;
		case MAC_EV_RESIZED:
			if(gMacIn.resized)
				gMacIn.resized(gMacIn.user, e.a, e.b);
			break;
		case MAC_EV_CLOSE:
			if(gMacIn.close)
				gMacIn.close(gMacIn.user);
			break;
		case MAC_EV_PAINT:
			if(gMacIn.paint)
				gMacIn.paint(gMacIn.user);
			break;
		case MAC_EV_DISPLAY:
			if(gMacIn.displayChanged)
				gMacIn.displayChanged(gMacIn.user);
			break;
		case MAC_EV_DROP:
			if(gMacIn.dropFile && e.path)
				gMacIn.dropFile(gMacIn.user, e.path);
			break;
		case MAC_EV_KEY:
			if(gMacIn.key)
				gMacIn.key(gMacIn.user, e.a, e.b, e.c);
			break;
		case MAC_EV_DEBUG_CLOSE:
			if(gMacIn.debugClose)
				gMacIn.debugClose(gMacIn.user);
			break;
		default:
			break;
	}
	free(e.path);
	return 1;
}

// ---- the application -----------------------------------------------------------------------------

@interface BGIAppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation BGIAppDelegate

// Quit of the menu, a log-out: the window's close box, which the engine answers (it may ask first)
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender
{
	BGI_UNUSED(sender);
	Mac_Post(MAC_EV_CLOSE, 0, 0, 0, NULL);
	return NSTerminateCancel;
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender
{
	BGI_UNUSED(sender);
	return NO;
}

// a folder or a file dragged onto the application's icon, or opened with it: as one dropped on the window
- (void)application:(NSApplication*)sender openFiles:(NSArray<NSString*>*)filenames
{
	for(NSString* name in filenames)
		Mac_Post(MAC_EV_DROP, 0, 0, 0, name.fileSystemRepresentation);
	[sender replyToOpenOrPrint:NSApplicationDelegateReplySuccess];
}

- (void)applicationDidChangeScreenParameters:(NSNotification*)notification
{
	BGI_UNUSED(notification);
	Mac_ScreensChanged();
	Mac_Post(MAC_EV_DISPLAY, 0, 0, 0, NULL);
}

// "Full Screen" of the Window menu: the key the engine switches with, Alt+Enter
- (void)toggleFullScreenBGI:(id)sender
{
	BGI_UNUSED(sender);
	Mac_Post(MAC_EV_KEY, 0x0d, 1, OPENBGI_KEY_SYSTEM, NULL);
	Mac_Post(MAC_EV_KEY, 0x0d, 0, OPENBGI_KEY_SYSTEM, NULL);
}

@end

static BGIAppDelegate* gDelegate;

// the menu bar: the application's menu (Hide, Quit) and a Window menu (Minimize, Full Screen)
static void MakeMenu(void)
{
	NSMenu* bar = [[NSMenu alloc] initWithTitle:@""];
	NSMenu* app = [[NSMenu alloc] initWithTitle:@""];
	NSMenu* win = [[NSMenu alloc] initWithTitle:@"Window"];
	NSMenuItem* appItem = [[NSMenuItem alloc] initWithTitle:@"" action:NULL keyEquivalent:@""];
	NSMenuItem* winItem = [[NSMenuItem alloc] initWithTitle:@"Window" action:NULL keyEquivalent:@""];
	NSMenuItem* item;

	[app addItemWithTitle:@"About OpenBGI" action:@selector(orderFrontStandardAboutPanel:) keyEquivalent:@""];
	[app addItem:[NSMenuItem separatorItem]];
	[app addItemWithTitle:@"Hide OpenBGI" action:@selector(hide:) keyEquivalent:@"h"];
	item = [app addItemWithTitle:@"Hide Others" action:@selector(hideOtherApplications:) keyEquivalent:@"h"];
	item.keyEquivalentModifierMask = NSEventModifierFlagOption | NSEventModifierFlagCommand;
	[app addItemWithTitle:@"Show All" action:@selector(unhideAllApplications:) keyEquivalent:@""];
	[app addItem:[NSMenuItem separatorItem]];
	[app addItemWithTitle:@"Quit OpenBGI" action:@selector(terminate:) keyEquivalent:@"q"];

	[win addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
	item = [win addItemWithTitle:@"Full Screen" action:@selector(toggleFullScreenBGI:) keyEquivalent:@"f"];
	item.keyEquivalentModifierMask = NSEventModifierFlagControl | NSEventModifierFlagCommand;
	item.target = gDelegate;

	appItem.submenu = app;
	winItem.submenu = win;
	[bar addItem:appItem];
	[bar addItem:winItem];
	NSApp.mainMenu = bar;
	NSApp.windowsMenu = win;
}

void Mac_AppInit(void)
{
	if(gAppReady)
		return;
	gAppReady = YES;
	@autoreleasepool
	{
		[NSApplication sharedApplication];
		[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
		gDelegate = [[BGIAppDelegate alloc] init];
		NSApp.delegate = gDelegate;
		MakeMenu();
		[NSApp finishLaunching];
		/* a timer in the modes of AppKit's own loops (a box, a picker, a
		 * menu held open): the modal tick of OS_SetModalTick while one runs */
		gTickTimer = [NSTimer timerWithTimeInterval:0.02
											repeats:YES
											  block:^(NSTimer* timer) {
												  BGI_UNUSED(timer);
												  if(gModalTick)
													  gModalTick(gModalTickUser);
											  }];
		[[NSRunLoop currentRunLoop] addTimer:gTickTimer forMode:NSRunLoopCommonModes];
	}
}

// ---- the host's calls ----------------------------------------------------------------------------

void OS_SetHostHandlers(const OsHostHandlers_t* h)
{
	if(h)
		gMacIn = *h;
	else
		memset(&gMacIn, 0, sizeof gMacIn);
}

/* SIGTERM / SIGINT (src/host/posix/process.c): `quit` from the next
 * pump, so that the engine shuts down in order.  Only a flag is set. */
void OsPosix_RequestClose(void)
{
	gQuitPosted = 1;
}

/* One message: the quit a signal asked for, then what the windows
 * reported, then one event of the application's; 1 when something was
 * delivered.  A picture that waits to be shown is shown first. */
int OS_PumpMessages(void)
{
	if(gQuitPosted)
	{
		gQuitPosted = 0;
		if(gMacIn.quit)
			gMacIn.quit(gMacIn.user);
		return 1;
	}
	if(!gAppReady)
		return 0;
	@autoreleasepool
	{
		NSEvent* ev;
		Mac_PresentIfDirty();
		if(Deliver())
			return 1;
		ev = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:nil inMode:NSDefaultRunLoopMode dequeue:YES];
		if(!ev)
			return 0;
		// (AppKit keeps a key's release from the view while Command is held: the window takes it directly)
		if(ev.type == NSEventTypeKeyUp && (ev.modifierFlags & NSEventModifierFlagCommand) != 0)
			[NSApp.keyWindow sendEvent:ev];
		else
			[NSApp sendEvent:ev];
	}
	return 1;
}

void OS_SetModalTick(void (*tick)(void* user), void* user)
{
	gModalTick = tick;
	gModalTickUser = user;
}

// SetClipboardData(CF_TEXT): the text on the general pasteboard; 1 when it was taken
int OS_ClipboardSetText(const char* text)
{
	int ok;
	@autoreleasepool
	{
		NSPasteboard* pb = [NSPasteboard generalPasteboard];
		[pb clearContents];
		ok = [pb setString:Mac_Text(text) forType:NSPasteboardTypeString] ? 1 : 0;
	}
	return ok;
}

// ---- text ----------------------------------------------------------------------------------------

NSString* Mac_Text(const char* text)
{
	size_t n;
	char* utf8;
	NSString* s;
	if(!text || !*text)
		return @"";
	n = strlen(text) * 3 + 4;
	utf8 = (char*)malloc(n);
	if(!utf8)
		return @"";
	OsCommon_SjisToUtf8(text, utf8, n);
	s = [NSString stringWithUTF8String:utf8];
	free(utf8);
	return s ? s : @"";
}

void Mac_FromString(NSString* s, char* out, size_t n)
{
	const char* utf8 = s ? s.UTF8String : NULL;
	if(!n)
		return;
	out[0] = 0;
	if(!utf8)
		return;
	if(gOsTextUtf8)
		snprintf(out, n, "%s", utf8);
	else
		Sjis_FromUtf8(utf8, out, n);
}
