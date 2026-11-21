/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * dbgwin.m - the macOS back end's window for the debugger (src/host/os.h)
 *
 * A second window whose view shows the surface the debugger presents, a
 * point a pixel; its keys, mouse and wheel go to the host's debug*
 * handlers, its close box as debugClose.
 */
#import "macos_internal.h"

@interface BGIDebugView : NSView <NSWindowDelegate>
@end

static NSWindow* gDebugWin;
static BGIDebugView* gDebugView;

@implementation BGIDebugView

- (BOOL)acceptsFirstResponder
{
	return YES;
}

- (BOOL)acceptsFirstMouse:(NSEvent*)event
{
	BGI_UNUSED(event);
	return YES;
}

// a key with the character it typed (0: none)
- (void)key:(NSEvent*)e down:(int)down
{
	int vk = Mac_EventVk(e, NULL), ch = 0;
	NSString* typed = e.characters;
	if(down && typed.length > 0)
	{
		unichar c = [typed characterAtIndex:0];
		if(c >= 0x20 && c != 0x7f && !(c >= 0xf700 && c <= 0xf8ff))
			ch = c;
	}
	if(gMacIn.debugKey && (vk || ch))
		gMacIn.debugKey(gMacIn.user, vk, down, ch);
}

- (void)keyDown:(NSEvent*)e
{
	[self key:e down:1];
}

- (void)keyUp:(NSEvent*)e
{
	[self key:e down:0];
}

// the mouse in the surface's pixels, from its top left; button -1: a move
- (void)mouse:(NSEvent*)e button:(int)button down:(int)down
{
	NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
	if(gMacIn.debugMouse)
		gMacIn.debugMouse(gMacIn.user, (int)p.x, (int)(self.bounds.size.height - p.y), button, down);
}

- (void)mouseDown:(NSEvent*)e
{
	[self mouse:e button:0 down:1];
}

- (void)mouseUp:(NSEvent*)e
{
	[self mouse:e button:0 down:0];
}

- (void)rightMouseDown:(NSEvent*)e
{
	[self mouse:e button:1 down:1];
}

- (void)rightMouseUp:(NSEvent*)e
{
	[self mouse:e button:1 down:0];
}

- (void)otherMouseDown:(NSEvent*)e
{
	[self mouse:e button:2 down:1];
}

- (void)otherMouseUp:(NSEvent*)e
{
	[self mouse:e button:2 down:0];
}

- (void)mouseMoved:(NSEvent*)e
{
	[self mouse:e button:-1 down:0];
}

- (void)mouseDragged:(NSEvent*)e
{
	[self mouse:e button:-1 down:0];
}

- (void)scrollWheel:(NSEvent*)e
{
	double d = e.scrollingDeltaY;
	if(d != 0 && gMacIn.debugWheel)
		gMacIn.debugWheel(gMacIn.user, d > 0 ? 120 : -120);
}

- (BOOL)windowShouldClose:(NSWindow*)sender
{
	BGI_UNUSED(sender);
	Mac_Post(MAC_EV_DEBUG_CLOSE, 0, 0, 0, NULL);
	return NO;
}

@end

int OS_DebugWindowOpen(int w, int h, const char* title)
{
	if(gDebugWin || w <= 0 || h <= 0)
		return 0;
	Mac_AppInit();
	@autoreleasepool
	{
		NSRect content = NSMakeRect(0, 0, w, h);
		gDebugWin = [[NSWindow alloc] initWithContentRect:content
												styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable
												  backing:NSBackingStoreBuffered
													defer:NO];
		gDebugView = [[BGIDebugView alloc] initWithFrame:content];
		if(!gDebugWin || !gDebugView)
		{
			gDebugWin = nil;
			gDebugView = nil;
			return 0;
		}
		gDebugView.wantsLayer = YES;
		gDebugView.layer.contentsGravity = kCAGravityResize;
		gDebugView.layer.magnificationFilter = kCAFilterNearest;
		gDebugWin.releasedWhenClosed = NO;
		gDebugWin.acceptsMouseMovedEvents = YES;
		gDebugWin.title = Mac_Text(title);
		gDebugWin.contentView = gDebugView;
		gDebugWin.delegate = gDebugView;
		[gDebugWin makeFirstResponder:gDebugView];
		[gDebugWin makeKeyAndOrderFront:nil];
	}
	return 1;
}

void OS_DebugWindowClose(void)
{
	if(!gDebugWin)
		return;
	@autoreleasepool
	{
		gDebugWin.delegate = nil;
		[gDebugWin orderOut:nil];
		[gDebugWin close];
		gDebugWin = nil;
		gDebugView = nil;
	}
}

// the whole surface: its pixels copied into an image, which the view's layer shows
void OS_DebugWindowPresent(const OsSurface_t* s)
{
	if(!gDebugView || !s || !s->pixels || s->bpp != 32 || s->width <= 0 || s->height <= 0)
		return;
	@autoreleasepool
	{
		size_t row = (size_t)s->width * 4;
		NSMutableData* data = [NSMutableData dataWithLength:row * (size_t)s->height];
		CGDataProviderRef provider;
		CGColorSpaceRef space;
		CGImageRef image;
		uint8_t* out = (uint8_t*)data.mutableBytes;
		int y;
		if(!out)
			return;
		for(y = 0; y < s->height; y++)
			memcpy(out + (size_t)y * row, (const uint8_t*)s->pixels + (ptrdiff_t)y * s->pitch, row);
		provider = CGDataProviderCreateWithCFData((__bridge CFDataRef)data);
		space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
		// (a pixel is 0x00RRGGBB: blue first in memory, the fourth byte unused)
		image = CGImageCreate((size_t)s->width, (size_t)s->height, 8, 32, row, space,
			(CGBitmapInfo)kCGBitmapByteOrder32Little | (CGBitmapInfo)kCGImageAlphaNoneSkipFirst, provider, NULL, false,
			kCGRenderingIntentDefault);
		if(image)
			gDebugView.layer.contents = (__bridge id)image;
		CGImageRelease(image);
		CGColorSpaceRelease(space);
		CGDataProviderRelease(provider);
	}
}
