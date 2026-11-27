/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * window.m - the macOS back end's main window (src/host/os.h)
 *
 * One window with one view stands in for the engine's main window.  The
 * view's layer is Metal's: the picture is a texture the engine's blits
 * fill (present.m), drawn over the view at the display's own resolution.
 * Lengths are points, so a window of the picture's size shows it at two
 * pixels a pixel on a display that has them; the scaling is the graphics
 * processor's.
 *
 * As on X11, full screen switches no display mode: the window, without a
 * frame, covers the screen, and the mode the engine asked for is scaled
 * to fit, centred (below a camera housing where the screen has one),
 * with every coordinate the engine sees or hands out translated.  A
 * windowed client area of another size than the picture
 * (OS_WindowSetPicture) is handled by the same mapping.
 *
 * Keys are Windows virtual keys: a letter, digit or sign by what the
 * key types in the keyboard's layout, the others by their place.
 * Option is Alt, Control is Ctrl; Command is the system's and not handed
 * over.  While the engine takes text (OS_TextInput) the keys go through
 * the input method, whose text in the making is shown in a label at the
 * field; the engine draws the text entry and the pop-up windows itself.
 */
#import "macos_internal.h"

#include <math.h>

@interface BGIWindow : NSWindow
@end

@interface BGIView : NSView <NSTextInputClient, NSWindowDelegate>
- (void)layerResized;
@end

static BGIWindow* gWin; // nil before OS_WindowCreate and after OS_WindowDestroy
static BGIView* gView;
static NSTextField* gMarkedLabel;           // the input method's text in the making
static NSString* gTitle = @"";              // the window's title, kept across its styles
static int gClientW = 800, gClientH = 600;  // the engine's client size
static int gFullscreen;                     // the window covers the screen
static int gModeW, gModeH;                  // the display mode (full screen) or picture (windowed) the engine draws
static int gMapOn;                          // the mode is scaled into the window
static int gScaleRule;                      // OS_WindowSetScaling: OPENBGI_SCALE_*
static double gScale = 1.0, gOffX, gOffY;   // mode -> view points
static int gResizable;                      // OS_WindowSetResizable
static int gUserSized;                      // and the player gave the window a size of his own
static int gAspectW, gAspectH;              // OS_WindowSetAspect: the proportions the client area keeps meanwhile (0: any)
static int gMinimized;
static int gDirty;                          // the picture changed and was not shown yet
static int gCursorShape = OS_CURSOR_ARROW;  // OS_CursorSetShape
static int gCursorCount;                    // ShowCursor: visible while >= 0
static NSCursor* gCursorBlank;
static int gDragAccept;                     // OS_DragAccept
static int gTextOn;                         // the engine takes typed text (OS_TextInput) ..
static int gTextX, gTextY, gTextW, gTextH;  // .. for the field there (the picture's pixels)
static NSString* gMarked = @"";             // the input method's text in the making
static NSMutableString* gPending;           // text the input method handed over during a key press
static BOOL gInKeyDown;
static unsigned char gKeyDown[256];         // the virtual keys whose press went to the engine
static double gWheel;                       // what a trackpad scrolled and was not a notch yet
static int gDcToken;                        // what OS_WindowGetDc hands out

NSWindow* Mac_MainWindow(void)
{
	return gWin;
}

// ---- geometry ------------------------------------------------------------------------------------

static double gPrimaryH; // 0: to be asked for again

void Mac_ScreensChanged(void)
{
	gPrimaryH = 0;
}

// the height of the screen the coordinates count from: Cocoa's origin is its bottom left, the engine's its top left
static double PrimaryHeight(void)
{
	if(gPrimaryH <= 0)
	{
		NSArray<NSScreen*>* screens = [NSScreen screens];
		gPrimaryH = screens.count > 0 ? screens[0].frame.size.height : 768.0;
	}
	return gPrimaryH;
}

// a point of the view counted from its top left, as the engine counts (the view's own origin is its bottom left)
static NSPoint FromBottom(NSPoint p)
{
	return NSMakePoint(p.x, (gView ? gView.bounds.size.height : 0) - p.y);
}

// what a camera housing takes of the screen's top in full screen
static double SafeTop(void)
{
	if(@available(macOS 12.0, *))
	{
		NSScreen* s = gWin ? gWin.screen : nil;
		if(s)
			return s.safeAreaInsets.top;
	}
	return 0.0;
}

// the part of the view the picture may take
static void TargetRect(double* x, double* y, double* w, double* h)
{
	NSSize size = gView ? gView.bounds.size : NSMakeSize(gClientW, gClientH);
	*x = 0;
	*y = 0;
	*w = size.width;
	*h = size.height;
	if(gFullscreen)
	{
		double top = SafeTop();
		*y = top;
		*h -= top;
	}
}

// the picture the engine draws: the mode it named, else the size of what it handed over; 0 before either
static int gPicW, gPicH;

static void PictureSize(void)
{
	gPicW = gModeW;
	gPicH = gModeH;
	if(gPicW <= 0 || gPicH <= 0)
	{
		gPicW = gPicH = 0;
		MacPresent_Size(&gPicW, &gPicH);
	}
}

/* the scale and offset that fit the picture into the view: the smaller
 * of the two ratios (a whole number of the display's pixels to one of the
 * picture's where OPENBGI_SCALE_WHOLE asks for one and the picture is not
 * made smaller), centred.  No mapping while a windowed client area is of
 * the picture's size. */
static void Recompute(void)
{
	double tx, ty, tw, th, sx, sy;
	TargetRect(&tx, &ty, &tw, &th);
	PictureSize();
	gMapOn = 0;
	gScale = 1.0;
	gOffX = gOffY = 0;
	if(gPicW <= 0 || gPicH <= 0 || tw < 1 || th < 1 || (!gFullscreen && (int)tw == gPicW && (int)th == gPicH))
		return;
	gMapOn = 1;
	sx = tw / gPicW;
	sy = th / gPicH;
	gScale = sx < sy ? sx : sy;
	if((gScaleRule & OPENBGI_SCALE_WHOLE) != 0)
	{
		double pixels = gWin ? gWin.backingScaleFactor : 1.0, k = floor(gScale * pixels + 0.0001);
		if(pixels > 0 && k >= 1)
			gScale = k / pixels;
	}
	gOffX = tx + (tw - gPicW * gScale) / 2;
	gOffY = ty + (th - gPicH * gScale) / 2;
}

// mode (engine) coordinates <-> view points
static double ToViewX(double x)
{
	return gMapOn ? x * gScale + gOffX : x;
}
static double ToViewY(double y)
{
	return gMapOn ? y * gScale + gOffY : y;
}
static int FromViewX(double x)
{
	return (int)floor(gMapOn ? (x - gOffX) / gScale : x);
}
static int FromViewY(double y)
{
	return (int)floor(gMapOn ? (y - gOffY) / gScale : y);
}

// the client area's top left on the screen, the engine's way
static void ClientOrigin(double* x, double* y)
{
	NSRect content;
	*x = *y = 0;
	if(!gWin)
		return;
	content = [gWin contentRectForFrameRect:gWin.frame];
	*x = content.origin.x;
	*y = PrimaryHeight() - (content.origin.y + content.size.height);
}

static int TitleBarHeight(void);

static NSWindowStyleMask WindowedStyle(void)
{
	NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable;
	if(gResizable)
		style |= NSWindowStyleMaskResizable;
	return style;
}

// the proportions a window with a sizing border keeps while the player sizes it (after every change of its style)
static void ApplyAspect(void)
{
	if(!gWin || gFullscreen)
		return;
	if(gResizable && gAspectW > 0 && gAspectH > 0)
		gWin.contentAspectRatio = NSMakeSize(gAspectW, gAspectH);
	else
		gWin.contentResizeIncrements = NSMakeSize(1, 1); // (which ends the proportions)
}

/* The framed window with a client area of w x h, its frame's top left at
 * (x, y).  A client area larger than the screen has room for - a picture
 * of 1920 x 1080 on a notebook's screen - is made smaller in proportion,
 * and the picture is scaled into it like into any client area of another
 * size; the title bar stays below the menu bar. */
static void PlaceWindow(int x, int y, int w, int h)
{
	NSScreen* screen = gWin.screen ? gWin.screen : [NSScreen mainScreen];
	NSRect frame;
	double cw = w, ch = h;
	if(screen && w > 0 && h > 0)
	{
		NSRect visible = screen.visibleFrame;
		double roomW = visible.size.width, roomH = visible.size.height - TitleBarHeight();
		double fit = 1.0;
		if(roomW >= 320 && cw > roomW)
			fit = roomW / cw;
		if(roomH >= 240 && ch * fit > roomH)
			fit = roomH / ch;
		cw = floor(cw * fit);
		ch = floor(ch * fit);
	}
	frame = [gWin frameRectForContentRect:NSMakeRect(0, 0, cw, ch)];
	frame.origin.x = x;
	frame.origin.y = PrimaryHeight() - y - frame.size.height;
	if(screen)
	{
		NSRect visible = screen.visibleFrame;
		if(cw < w && frame.origin.y < visible.origin.y)
			frame.origin.y = visible.origin.y;
		if(NSMaxY(frame) > NSMaxY(visible))
			frame.origin.y = NSMaxY(visible) - frame.size.height;
		if(cw < w && frame.origin.x + frame.size.width > NSMaxX(visible))
			frame.origin.x = NSMaxX(visible) - frame.size.width;
		if(cw < w && frame.origin.x < visible.origin.x)
			frame.origin.x = visible.origin.x;
	}
	[gWin setFrame:frame display:YES];
}

// ---- the picture ---------------------------------------------------------------------------------

/* How the picture is scaled.  BGI_MAC_FILTER=nearest, bilinear or bicubic
 * says it for a run.  Otherwise: the engine's rule where the window is of
 * another size than the picture, and the sharper one where only the
 * display's own two pixels a point enlarge it - unless the player chose
 * the rule (OPENBGI_SCALE_CHOSEN), which then holds there too. */
static int Filter(void)
{
	static int forced = -2;
	if(forced == -2)
	{
		const char* e = getenv("BGI_MAC_FILTER");
		forced = -1;
		if(e && strcmp(e, "nearest") == 0)
			forced = MAC_FILTER_NEAREST;
		else if(e && strcmp(e, "bilinear") == 0)
			forced = MAC_FILTER_BILINEAR;
		else if(e && strcmp(e, "bicubic") == 0)
			forced = MAC_FILTER_BICUBIC;
	}
	if(forced >= 0)
		return forced;
	if(!gMapOn && (gScaleRule & OPENBGI_SCALE_CHOSEN) == 0)
		return MAC_FILTER_BICUBIC;
	switch(gScaleRule & OPENBGI_SCALE_RULE)
	{
		case OPENBGI_SCALE_BICUBIC: return MAC_FILTER_BICUBIC;
		case OPENBGI_SCALE_NEAREST: return MAC_FILTER_NEAREST;
		default: return MAC_FILTER_BILINEAR;
	}
}

// the picture over the view; left for later while nobody can see the window
static void Present(void)
{
	double scale, x = 0, y = 0, w = 0, h = 0;
	int tw = 0, th = 0;
	if(!gWin || !gView)
		return;
	if(![gWin isVisible] || [gWin isMiniaturized] || (gWin.occlusionState & NSWindowOcclusionStateVisible) == 0)
	{
		gDirty = 1;
		return;
	}
	gDirty = 0;
	scale = gWin.backingScaleFactor;
	if(MacPresent_Size(&tw, &th))
	{
		if(gMapOn)
		{
			x = gOffX;
			y = gOffY;
			w = gPicW * gScale;
			h = gPicH * gScale;
		}
		else
		{
			w = tw;
			h = th;
		}
	}
	MacPresent_Draw(x * scale, y * scale, w * scale, h * scale, Filter());
}

void Mac_PresentIfDirty(void)
{
	if(gDirty)
		Present();
}

// the "device context" is a token: nothing is acquired, and the picture is shown by the next pump
OsDc_t* OS_WindowGetDc(void)
{
	return gWin ? &gDcToken : NULL;
}

void OS_WindowReleaseDc(OsDc_t* dc)
{
	BGI_UNUSED(dc);
}

// BitBlt: the pixels into the picture's texture
void OS_BlitToWindow(OsDc_t* dc, int dstX, int dstY, int w, int h, const OsSurface_t* src, int srcX, int srcY)
{
	BGI_UNUSED(dc);
	if(!gWin || !src)
		return;
	@autoreleasepool
	{
		int beforeW = 0, beforeH = 0, nowW = 0, nowH = 0;
		MacPresent_Size(&beforeW, &beforeH);
		if(MacPresent_Upload(src, dstX, dstY, w, h, srcX, srcY))
			gDirty = 1;
		MacPresent_Size(&nowW, &nowH);
		if(nowW != beforeW || nowH != beforeH)
			Recompute(); // (a picture of another size)
	}
}

void OS_WindowFillBlack(void)
{
	if(!gWin)
		return;
	@autoreleasepool
	{
		MacPresent_Clear();
		gDirty = 1;
		Present();
	}
}

// ---- the keys ------------------------------------------------------------------------------------

/* The virtual keys of the keys by their place (the key codes of a Mac's
 * keyboard, which name places and not signs): what a key that types
 * nothing is, and what one that types something is on a keyboard whose
 * layout has no Latin letters. */
static const unsigned char kVkOfCode[128] = {
	'A', 'S', 'D', 'F', 'H', 'G', 'Z', 'X', 'C', 'V', 0, 'B', 'Q', 'W', 'E', 'R',                               // 0x00
	'Y', 'T', '1', '2', '3', '4', '6', '5', 0xbb, '9', '7', 0xbd, '8', '0', 0xdd, 'O',                          // 0x10
	'U', 0xdb, 'I', 'P', 0x0d, 'L', 'J', 0xde, 'K', 0xba, 0xdc, 0xbc, 0xbf, 'N', 'M', 0xbe,                     // 0x20
	0x09, 0x20, 0xc0, 0x08, 0, 0x1b, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,                                              // 0x30 (the modifiers: flagsChanged)
	0x80, 0x6e, 0, 0x6a, 0, 0x6b, 0, 0x0c, 0, 0, 0, 0x6f, 0x0d, 0, 0x6d, 0x81,                                  // 0x40 (F17, the keypad, F18)
	0x82, 0xbb, 0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x83, 0x68, 0x69, 0xdc, 0xe2, 0x6c,             // 0x50 (F19, the keypad, F20, the JIS keys)
	0x74, 0x75, 0x76, 0x72, 0x77, 0x78, 0, 0x7a, 0, 0x7c, 0x7f, 0x7d, 0, 0x79, 0, 0x7b,                         // 0x60 (F5 .. F16)
	0, 0x7e, 0x2d, 0x24, 0x21, 0x2e, 0x73, 0x23, 0x71, 0x22, 0x70, 0x25, 0x27, 0x28, 0x26, 0,                   // 0x70 (F15, Help, the block of six, the arrows)
};

// the virtual key of a sign a key types without Shift, as Windows names the keys of a US keyboard; 0: none
static int VkOfChar(unichar c)
{
	if(c >= 'a' && c <= 'z')
		return 'A' + (c - 'a');
	if((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
		return c;
	switch(c)
	{
		case ';': return 0xba;
		case '=': return 0xbb;
		case ',': return 0xbc;
		case '-': return 0xbd;
		case '.': return 0xbe;
		case '/': return 0xbf;
		case '`': return 0xc0;
		case '[': return 0xdb;
		case '\\': return 0xdc;
		case ']': return 0xdd;
		case '\'': return 0xde;
		default: return 0;
	}
}

// a key that types a sign: its virtual key follows the layout
static int Types(int vk)
{
	return (vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z') || (vk >= 0xba && vk <= 0xc0) || (vk >= 0xdb && vk <= 0xde);
}

int Mac_EventVk(NSEvent* e, int* flags)
{
	unsigned short code = e.keyCode;
	int vk = code < 128 ? kVkOfCode[code] : 0;
	if(vk == 0 || Types(vk))
	{
		NSString* typed = e.charactersIgnoringModifiers;
		int byLayout = typed.length > 0 ? VkOfChar([typed characterAtIndex:0]) : 0;
		if(byLayout)
			vk = byLayout;
	}
	if(flags)
	{
		*flags = [e isARepeat] ? OPENBGI_KEY_REPEAT : 0;
		// Alt+key and F10 are "system" keys (WM_SYSKEYDOWN)
		if((e.modifierFlags & NSEventModifierFlagOption) != 0 || vk == 0x79)
			*flags |= OPENBGI_KEY_SYSTEM;
	}
	return vk;
}

// a key to the host; a release only of a key whose press went there
static void SendKey(int vk, int down, int flags)
{
	if(vk <= 0 || vk > 255)
		return;
	if(down)
		gKeyDown[vk] = 1;
	else if(!gKeyDown[vk])
		return;
	else
		gKeyDown[vk] = 0;
	if(gMacIn.key)
		gMacIn.key(gMacIn.user, vk, down, flags);
}

// a modifier's state, to the host when it changed
static void SendModifier(int vk, BOOL down, int flags)
{
	if((gKeyDown[vk] != 0) != (down != NO))
		SendKey(vk, down ? 1 : 0, flags);
}

// text to the host's `text`, without what is no text: control characters and the function keys' private signs
static void SendText(NSString* s)
{
	NSMutableString* clean;
	NSUInteger i;
	if(!gMacIn.text || s.length == 0)
		return;
	clean = [NSMutableString string];
	for(i = 0; i < s.length; i++)
	{
		unichar c = [s characterAtIndex:i];
		if(c < 0x20 || c == 0x7f || (c >= 0xf700 && c <= 0xf8ff))
			continue;
		[clean appendString:[s substringWithRange:NSMakeRange(i, 1)]];
	}
	if(clean.length > 0)
		gMacIn.text(gMacIn.user, clean.UTF8String);
}

// the input method's text in the making, in a label under the field the engine draws
static void ShowMarked(void)
{
	NSRect frame, bounds;
	if(!gMarkedLabel || !gView)
		return;
	if(gMarked.length == 0)
	{
		gMarkedLabel.hidden = YES;
		return;
	}
	gMarkedLabel.stringValue = gMarked;
	[gMarkedLabel sizeToFit];
	frame = gMarkedLabel.frame;
	bounds = gView.bounds;
	frame.origin.x = ToViewX(gTextX);
	frame.origin.y = ToViewY(gTextY + gTextH) + 2; // (from the top: under the field, or over it at the picture's end)
	if(frame.origin.x + frame.size.width > bounds.size.width)
		frame.origin.x = bounds.size.width - frame.size.width;
	if(frame.origin.x < 0)
		frame.origin.x = 0;
	if(frame.origin.y + frame.size.height > bounds.size.height)
		frame.origin.y = ToViewY(gTextY) - frame.size.height - 2;
	if(frame.origin.y < 0)
		frame.origin.y = 0;
	frame.origin.y = bounds.size.height - frame.origin.y - frame.size.height;
	gMarkedLabel.frame = frame;
	gMarkedLabel.hidden = NO;
}

// a cursor nobody sees: 16 x 16 pixels, all of them clear
static NSCursor* BlankCursor(void)
{
	NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL
																	pixelsWide:16
																	pixelsHigh:16
																 bitsPerSample:8
															   samplesPerPixel:4
																	  hasAlpha:YES
																	  isPlanar:NO
																colorSpaceName:NSDeviceRGBColorSpace
																  bitmapFormat:NSBitmapFormatAlphaNonpremultiplied
																   bytesPerRow:16 * 4
																  bitsPerPixel:32];
	NSImage* image = [[NSImage alloc] initWithSize:NSMakeSize(16, 16)];
	if(rep && rep.bitmapData)
	{
		memset(rep.bitmapData, 0, 16 * 16 * 4);
		[image addRepresentation:rep];
	}
	return [[NSCursor alloc] initWithImage:image hotSpot:NSZeroPoint];
}

static NSCursor* CurrentCursor(void)
{
	if(gCursorCount < 0)
	{
		if(!gCursorBlank)
			gCursorBlank = BlankCursor();
		return gCursorBlank;
	}
	return gCursorShape == OS_CURSOR_CUSTOM ? [NSCursor pointingHandCursor] : [NSCursor arrowCursor];
}

static void ApplyCursor(void)
{
	if(!gWin || !gView)
		return;
	@autoreleasepool
	{
		[gWin invalidateCursorRectsForView:gView];
	}
}

// ---- the window and its view ---------------------------------------------------------------------

@implementation BGIWindow

// (a window without a frame, as in full screen, takes no keys unless it says so)
- (BOOL)canBecomeKeyWindow
{
	return YES;
}

- (BOOL)canBecomeMainWindow
{
	return YES;
}

@end

@implementation BGIView

- (BOOL)isOpaque
{
	return YES;
}

- (BOOL)acceptsFirstResponder
{
	return YES;
}

// the click that brings the window to the front is a click, as on Windows
- (BOOL)acceptsFirstMouse:(NSEvent*)event
{
	BGI_UNUSED(event);
	return YES;
}

- (BOOL)wantsUpdateLayer
{
	return YES;
}

- (CALayer*)makeBackingLayer
{
	return [CAMetalLayer layer];
}

- (void)updateLayer
{
	// (the layer's content is Metal's)
}

// the view or the display's pixels a point changed: the layer in pixels, the mapping, the picture again
- (void)layerResized
{
	CAMetalLayer* layer = (CAMetalLayer*)self.layer;
	double scale = self.window ? self.window.backingScaleFactor : 1.0;
	NSSize size = self.bounds.size;
	if(!layer || size.width < 1 || size.height < 1)
		return;
	layer.contentsScale = scale;
	layer.drawableSize = CGSizeMake(size.width * scale, size.height * scale);
	if(self != gView)
		return; // (not the window's yet)
	Recompute();
	ShowMarked();
	Present();
}

- (void)setFrameSize:(NSSize)newSize
{
	[super setFrameSize:newSize];
	[self layerResized];
}

- (void)viewDidChangeBackingProperties
{
	[super viewDidChangeBackingProperties];
	[self layerResized];
}

- (void)resetCursorRects
{
	[super resetCursorRects];
	[self addCursorRect:self.bounds cursor:CurrentCursor()];
}

// ---- the mouse

- (void)button:(int)button down:(int)down event:(NSEvent*)e
{
	NSPoint p = FromBottom([self convertPoint:e.locationInWindow fromView:nil]);
	int flags = 0;
	// WM_?BUTTONDBLCLK: every second press of a quick row, for the left and right buttons only
	if(down && button <= OPENBGI_BUTTON_RIGHT && e.clickCount > 1 && e.clickCount % 2 == 0)
		flags = OPENBGI_CLICK_DOUBLE;
	if(gMacIn.button)
		gMacIn.button(gMacIn.user, button, down, FromViewX(p.x), FromViewY(p.y), flags);
}

- (void)moved:(NSEvent*)e
{
	NSPoint p = FromBottom([self convertPoint:e.locationInWindow fromView:nil]);
	if(gMacIn.pointer)
		gMacIn.pointer(gMacIn.user, FromViewX(p.x), FromViewY(p.y));
}

// the buttons beyond the first two: 2 the middle one, 3 and 4 "back" and "forward"
static int OtherButton(NSEvent* e)
{
	NSInteger n = e.buttonNumber;
	return n == 2 ? OPENBGI_BUTTON_MIDDLE : n == 3 ? OPENBGI_BUTTON_X1 : n == 4 ? OPENBGI_BUTTON_X2 : -1;
}

- (void)mouseDown:(NSEvent*)e
{
	[self button:OPENBGI_BUTTON_LEFT down:1 event:e];
}

- (void)mouseUp:(NSEvent*)e
{
	[self button:OPENBGI_BUTTON_LEFT down:0 event:e];
}

- (void)rightMouseDown:(NSEvent*)e
{
	[self button:OPENBGI_BUTTON_RIGHT down:1 event:e];
}

- (void)rightMouseUp:(NSEvent*)e
{
	[self button:OPENBGI_BUTTON_RIGHT down:0 event:e];
}

- (void)otherMouseDown:(NSEvent*)e
{
	int b = OtherButton(e);
	if(b >= 0)
		[self button:b down:1 event:e];
}

- (void)otherMouseUp:(NSEvent*)e
{
	int b = OtherButton(e);
	if(b >= 0)
		[self button:b down:0 event:e];
}

- (void)mouseMoved:(NSEvent*)e
{
	[self moved:e];
}

- (void)mouseDragged:(NSEvent*)e
{
	[self moved:e];
}

- (void)rightMouseDragged:(NSEvent*)e
{
	[self moved:e];
}

- (void)otherMouseDragged:(NSEvent*)e
{
	[self moved:e];
}

/* the wheel in notches of WHEEL_DELTA, + away from the user: a wheel's
 * step is a notch or more, a trackpad's points add up to them */
- (void)scrollWheel:(NSEvent*)e
{
	int notches = 0;
	if(e.hasPreciseScrollingDeltas)
	{
		gWheel += e.scrollingDeltaY;
		while(gWheel >= 10.0)
		{
			gWheel -= 10.0;
			notches++;
		}
		while(gWheel <= -10.0)
		{
			gWheel += 10.0;
			notches--;
		}
	}
	else
	{
		double d = e.scrollingDeltaY;
		notches = d > 0 ? (int)ceil(d) : d < 0 ? (int)floor(d) : 0;
		if(notches > 5)
			notches = 5;
		if(notches < -5)
			notches = -5;
	}
	if(notches && gMacIn.wheel)
		gMacIn.wheel(gMacIn.user, notches * 120);
}

// ---- the keyboard

- (void)keyDown:(NSEvent*)e
{
	int flags = 0, vk = Mac_EventVk(e, &flags);
	if(gTextOn)
	{
		/* through the input method: a key it takes for a text in the
		 * making is no key for the engine; the text it hands over is */
		BOOL composing = gMarked.length > 0;
		if(!gPending)
			gPending = [NSMutableString string];
		[gPending setString:@""];
		gInKeyDown = YES;
		[self interpretKeyEvents:@[e]];
		gInKeyDown = NO;
		if(!composing && gMarked.length == 0)
			SendKey(vk, 1, flags);
		SendText(gPending);
		[gPending setString:@""];
		return;
	}
	SendKey(vk, 1, flags);
	// the text the key typed, as WM_CHAR follows WM_KEYDOWN
	if((e.modifierFlags & (NSEventModifierFlagCommand | NSEventModifierFlagControl | NSEventModifierFlagOption)) == 0)
		SendText(e.characters);
}

- (void)keyUp:(NSEvent*)e
{
	int flags = 0, vk = Mac_EventVk(e, &flags);
	SendKey(vk, 0, flags & OPENBGI_KEY_SYSTEM);
}

// Shift, Control and Option, each as one key; Caps Lock as a press and a release
- (void)flagsChanged:(NSEvent*)e
{
	NSEventModifierFlags f = e.modifierFlags;
	SendModifier(0x10, (f & NSEventModifierFlagShift) != 0, 0);
	SendModifier(0x11, (f & NSEventModifierFlagControl) != 0, 0);
	SendModifier(0x12, (f & NSEventModifierFlagOption) != 0, OPENBGI_KEY_SYSTEM);
	if(e.keyCode == 0x39)
	{
		SendKey(0x14, 1, 0);
		SendKey(0x14, 0, 0);
	}
}

// ---- the input method (NSTextInputClient)

- (void)insertText:(id)string replacementRange:(NSRange)replacementRange
{
	NSString* s = [string isKindOfClass:[NSAttributedString class]] ? [(NSAttributedString*)string string] : (NSString*)string;
	BGI_UNUSED(replacementRange);
	gMarked = @"";
	ShowMarked();
	if(gInKeyDown && gPending)
		[gPending appendString:s];
	else
		SendText(s); // (from a palette, not a key)
}

- (void)setMarkedText:(id)string selectedRange:(NSRange)selectedRange replacementRange:(NSRange)replacementRange
{
	NSString* s = [string isKindOfClass:[NSAttributedString class]] ? [(NSAttributedString*)string string] : (NSString*)string;
	BGI_UNUSED(selectedRange);
	BGI_UNUSED(replacementRange);
	gMarked = s ? [s copy] : @"";
	ShowMarked();
}

- (void)unmarkText
{
	gMarked = @"";
	ShowMarked();
}

- (BOOL)hasMarkedText
{
	return gMarked.length > 0;
}

- (NSRange)markedRange
{
	return gMarked.length > 0 ? NSMakeRange(0, gMarked.length) : NSMakeRange(NSNotFound, 0);
}

- (NSRange)selectedRange
{
	return NSMakeRange(NSNotFound, 0);
}

- (NSArray<NSAttributedStringKey>*)validAttributesForMarkedText
{
	return @[];
}

- (NSAttributedString*)attributedSubstringForProposedRange:(NSRange)range actualRange:(NSRangePointer)actualRange
{
	BGI_UNUSED(range);
	BGI_UNUSED(actualRange);
	return nil;
}

- (NSUInteger)characterIndexForPoint:(NSPoint)point
{
	BGI_UNUSED(point);
	return 0;
}

// where the text is on the screen: the field, for the input method's own list of candidates
- (NSRect)firstRectForCharacterRange:(NSRange)range actualRange:(NSRangePointer)actualRange
{
	double w = gTextW * (gMapOn ? gScale : 1.0), h = gTextH * (gMapOn ? gScale : 1.0);
	NSRect r = NSMakeRect(ToViewX(gTextX), self.bounds.size.height - ToViewY(gTextY) - h, w, h);
	BGI_UNUSED(range);
	BGI_UNUSED(actualRange);
	if(!self.window)
		return NSZeroRect;
	return [self.window convertRectToScreen:[self convertRect:r toView:nil]];
}

- (void)doCommandBySelector:(SEL)selector
{
	BGI_UNUSED(selector); // (Return, the arrows and their like are keys: keyDown hands them over)
}

// ---- files dropped on the window

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender
{
	BGI_UNUSED(sender);
	return gDragAccept ? NSDragOperationCopy : NSDragOperationNone;
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender
{
	NSArray* urls;
	if(!gDragAccept)
		return NO;
	urls = [[sender draggingPasteboard] readObjectsForClasses:@[[NSURL class]] options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];
	for(NSURL* url in urls)
		Mac_Post(MAC_EV_DROP, 0, 0, 0, url.fileSystemRepresentation);
	return urls.count > 0;
}

// ---- what the window reports (NSWindowDelegate)

// the close box: the engine decides (the window goes when it destroys it)
- (BOOL)windowShouldClose:(NSWindow*)sender
{
	BGI_UNUSED(sender);
	Mac_Post(MAC_EV_CLOSE, 0, 0, 0, NULL);
	return NO;
}

- (void)windowDidBecomeKey:(NSNotification*)notification
{
	BGI_UNUSED(notification);
	Mac_Post(MAC_EV_FOCUS, 1, 0, 0, NULL);
}

- (void)windowDidResignKey:(NSNotification*)notification
{
	BGI_UNUSED(notification);
	memset(gKeyDown, 0, sizeof gKeyDown); // (their releases go elsewhere)
	Mac_Post(MAC_EV_FOCUS, 0, 0, 0, NULL);
}

- (void)windowDidMiniaturize:(NSNotification*)notification
{
	BGI_UNUSED(notification);
	gMinimized = 1;
	Mac_Post(MAC_EV_MINIMIZED, 1, 0, 0, NULL);
}

- (void)windowDidDeminiaturize:(NSNotification*)notification
{
	BGI_UNUSED(notification);
	gMinimized = 0;
	gDirty = 1;
	Mac_Post(MAC_EV_MINIMIZED, 0, 0, 0, NULL);
}

// the player gave a window with a sizing border another size: the engine hears of it
- (void)windowDidResize:(NSNotification*)notification
{
	NSSize size = self.bounds.size;
	int w = (int)size.width, h = (int)size.height;
	BGI_UNUSED(notification);
	if(gFullscreen || !gResizable || (w == gClientW && h == gClientH))
		return;
	gClientW = w;
	gClientH = h;
	gUserSized = 1;
	Recompute();
	Mac_Post(MAC_EV_RESIZED, w, h, 0, NULL);
}

// seen again: what changed meanwhile
- (void)windowDidChangeOcclusionState:(NSNotification*)notification
{
	BGI_UNUSED(notification);
	Mac_PresentIfDirty();
}

- (void)windowDidChangeScreen:(NSNotification*)notification
{
	BGI_UNUSED(notification);
	[self layerResized];
}

@end

// ---- the window ----------------------------------------------------------------------------------

static void SetTitle(const char* t)
{
	char title[0x400];
	OsCommon_MainTitle(title, sizeof title, t);
	gTitle = Mac_Text(title);
	if(gWin)
		gWin.title = gTitle;
}

int OS_WindowCreate(const OsWindowDesc_t* d)
{
	if(gWin)
		return 0;
	Mac_AppInit();
	@autoreleasepool
	{
		NSRect content = NSMakeRect(0, 0, d->width, d->height);
		BGIWindow* win;
		BGIView* view;
		gResizable = 0; // (a new window is of the engine's size until a script says otherwise)
		gUserSized = 0;
		gAspectW = gAspectH = 0;
		gFullscreen = 0;
		gClientW = d->width;
		gClientH = d->height;
		win = [[BGIWindow alloc] initWithContentRect:content styleMask:WindowedStyle() backing:NSBackingStoreBuffered defer:NO];
		view = [[BGIView alloc] initWithFrame:content];
		view.wantsLayer = YES;
		if(!win || !view || !MacPresent_Attach((CAMetalLayer*)view.layer))
			return 0;
		gWin = win;
		gView = view;
		win.releasedWhenClosed = NO;
		win.backgroundColor = [NSColor blackColor];
		win.acceptsMouseMovedEvents = YES;
		win.contentView = view;
		win.delegate = view;
		[win makeFirstResponder:view];
		gMarkedLabel = [NSTextField labelWithString:@""];
		gMarkedLabel.drawsBackground = YES;
		gMarkedLabel.backgroundColor = [NSColor textBackgroundColor];
		gMarkedLabel.textColor = [NSColor textColor];
		gMarkedLabel.font = [NSFont systemFontOfSize:16];
		gMarkedLabel.hidden = YES;
		[view addSubview:gMarkedLabel];
		if(gDragAccept)
			[view registerForDraggedTypes:@[NSPasteboardTypeFileURL]];
		gMarked = @"";
		gMinimized = 0;
		memset(gKeyDown, 0, sizeof gKeyDown);
		SetTitle(d->title);
		PlaceWindow(d->x, d->y, d->width, d->height);
		[view layerResized];
	}
	return 1;
}

void OS_WindowDestroy(void)
{
	if(!gWin)
		return;
	@autoreleasepool
	{
		if(gFullscreen)
			[NSApp setPresentationOptions:NSApplicationPresentationDefault];
		gFullscreen = 0;
		gWin.delegate = nil;
		[gWin orderOut:nil];
		[gWin close];
		MacPresent_Detach();
		gMarkedLabel = nil;
		gView = nil;
		gWin = nil;
		gDirty = 0;
	}
}

// the window on the screen, in front and with the keys, or off it
void OS_WindowShow(int show)
{
	if(!gWin)
		return;
	@autoreleasepool
	{
		if(show)
		{
			[gWin makeKeyAndOrderFront:nil];
			[NSApp activateIgnoringOtherApps:YES];
			gDirty = 1;
			Present();
		}
		else
			[gWin orderOut:nil];
	}
}

void OS_WindowSetTitle(const char* t)
{
	@autoreleasepool
	{
		SetTitle(t);
	}
}

// the frame's corner to (x, y); ignored in full screen
void OS_WindowMove(int x, int y)
{
	if(!gWin || gFullscreen)
		return;
	@autoreleasepool
	{
		PlaceWindow(x, y, gClientW, gClientH);
	}
}

int OS_WindowMinimized(void)
{
	return gMinimized;
}

void OS_WindowMinimize(void)
{
	if(!gWin)
		return;
	@autoreleasepool
	{
		[gWin miniaturize:nil];
	}
}

// the main screen in points; 1024 x 768 without one
void OS_ScreenSize(int* w, int* h)
{
	*w = 1024;
	*h = 768;
	gPrimaryH = 0; // (asked for again)
	@autoreleasepool
	{
		NSArray<NSScreen*>* screens = [NSScreen screens];
		if(screens.count > 0)
		{
			*w = (int)screens[0].frame.size.width;
			*h = (int)screens[0].frame.size.height;
		}
	}
}

// the frame: a title bar and no borders
static int TitleBarHeight(void)
{
	NSRect frame = [NSWindow frameRectForContentRect:NSMakeRect(0, 0, 100, 100)
										   styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable];
	return (int)(frame.size.height - 100);
}

void OS_FrameMetrics(int* fw, int* fh)
{
	*fw = 0;
	@autoreleasepool
	{
		*fh = TitleBarHeight();
	}
}

int OS_FrameTop(void)
{
	int top;
	@autoreleasepool
	{
		top = TitleBarHeight();
	}
	return top;
}

// GetWindowRect: the frame's rectangle on the screen; in full screen the display mode at (0, 0)
void OS_WindowGetRect(int* x, int* y, int* w, int* h)
{
	*x = *y = 0;
	*w = gFullscreen ? gModeW : gClientW;
	*h = gFullscreen ? gModeH : gClientH;
	if(!gWin || gFullscreen)
		return;
	@autoreleasepool
	{
		NSRect frame = gWin.frame;
		*x = (int)frame.origin.x;
		*y = (int)(PrimaryHeight() - (frame.origin.y + frame.size.height));
		*w = (int)frame.size.width;
		*h = (int)frame.size.height;
	}
}

// screen -> client coordinates; in full screen the engine's "screen" is the display mode, which is the client area
void OS_ScreenToClient(int* x, int* y)
{
	double cx, cy;
	if(gFullscreen)
		return;
	@autoreleasepool
	{
		ClientOrigin(&cx, &cy);
	}
	*x -= (int)cx;
	*y -= (int)cy;
}

void OS_ClientToScreen(int* x, int* y)
{
	double cx, cy;
	if(gFullscreen)
		return;
	@autoreleasepool
	{
		ClientOrigin(&cx, &cy);
	}
	*x += (int)cx;
	*y += (int)cy;
}

void OS_WindowSetResizable(int on)
{
	gResizable = on != 0;
	if(!gWin || gFullscreen)
		return;
	@autoreleasepool
	{
		gWin.styleMask = WindowedStyle();
		gWin.contentMinSize = NSMakeSize(320, 240);
		gWin.collectionBehavior = NSWindowCollectionBehaviorFullScreenNone; // (the green button zooms: full screen is the engine's)
		gWin.title = gTitle;
		ApplyAspect();
		[gWin makeFirstResponder:gView];
	}
}

void OS_WindowSetAspect(int w, int h)
{
	if(w <= 0 || h <= 0)
		w = h = 0;
	if(w == gAspectW && h == gAspectH)
		return;
	gAspectW = w;
	gAspectH = h;
	@autoreleasepool
	{
		ApplyAspect();
	}
}

// ---- the window's screen -------------------------------------------------------------------------

int OS_WindowFits(void)
{
	return 1;
}

// sizes are in points here, which the system scales to the display's pixels itself
int OS_WindowDpi(void)
{
	return 96;
}

// what the window's screen has room for beside the menu bar and the Dock, the engine's way
void OS_WindowWorkArea(int* x, int* y, int* w, int* h)
{
	*x = *y = 0;
	*w = 1024;
	*h = 768;
	@autoreleasepool
	{
		NSScreen* screen = gWin && gWin.screen ? gWin.screen : [NSScreen mainScreen];
		if(screen)
		{
			NSRect visible = screen.visibleFrame;
			*x = (int)ceil(visible.origin.x);
			*y = (int)ceil(PrimaryHeight() - NSMaxY(visible));
			*w = (int)floor(visible.size.width);
			*h = (int)floor(visible.size.height);
		}
	}
}

// no IME status window to show or hide
void OS_ImeStatus(int open)
{
	BGI_UNUSED(open);
}

// the keys through the input method while the engine takes text, for the field there; what it was making is dropped at the end
void OS_TextInput(int on, int x, int y, int w, int h)
{
	gTextOn = on != 0;
	gTextX = x;
	gTextY = y;
	gTextW = w;
	gTextH = h;
	if(!gView)
		return;
	@autoreleasepool
	{
		if(!gTextOn && gMarked.length > 0)
		{
			[gView.inputContext discardMarkedText];
			gMarked = @"";
		}
		ShowMarked();
	}
}

// the engine draws the text entry and the pop-up windows in the frame
const OpenBGI_TextEntry_t* OS_TextEntry(void)
{
	return NULL;
}

const OpenBGI_ChildWindows_t* OS_ChildWindows(void)
{
	return NULL;
}

// the message boxes are the system's alerts (dialogs.m); the scripts' dialogs are the engine's, in the frame
uint32_t OS_HostCaps(void)
{
	return OPENBGI_CAP_MESSAGES | OPENBGI_CAP_POINTER | OPENBGI_CAP_KEYBOARD;
}

// ---- display modes -------------------------------------------------------------------------------

// no mode switch: the requested mode is remembered and scaled onto the screen (Recompute); bpp is ignored
int OS_DisplaySetMode(int fullscreen, int w, int h, int bpp)
{
	BGI_UNUSED(fullscreen);
	BGI_UNUSED(bpp);
	if(w <= 0 || h <= 0)
		return 0;
	gModeW = w;
	gModeH = h;
	return 1;
}

void OS_DisplayRestore(void)
{
}

// the window without its frame over the whole screen, the Dock and the menu bar out of the way, the mode scaled onto it
void OS_WindowSetFullscreen(int modeW, int modeH)
{
	if(!gWin)
		return;
	@autoreleasepool
	{
		NSScreen* screen = gWin.screen ? gWin.screen : [NSScreen mainScreen];
		gModeW = modeW;
		gModeH = modeH;
		gFullscreen = 1;
		[NSApp setPresentationOptions:NSApplicationPresentationHideDock | NSApplicationPresentationHideMenuBar];
		gWin.styleMask = NSWindowStyleMaskBorderless;
		if(screen)
			[gWin setFrame:screen.frame display:YES];
		[gWin makeKeyAndOrderFront:nil];
		[gWin makeFirstResponder:gView];
		[gView layerResized];
	}
}

// back to (or still) a framed window of clientW x clientH at the outer origin (x, y)
void OS_WindowSetWindowed(int x, int y, int clientW, int clientH)
{
	if(!gWin)
		return;
	@autoreleasepool
	{
		if(gFullscreen)
		{
			gFullscreen = 0;
			[NSApp setPresentationOptions:NSApplicationPresentationDefault];
			gWin.styleMask = WindowedStyle();
			gWin.title = gTitle;
			ApplyAspect();
		}
		else if(gResizable && gUserSized && clientW == gClientW && clientH == gClientH)
			return; // the size the player gave it: nothing to do
		gUserSized = 0;
		gClientW = clientW;
		gClientH = clientH;
		PlaceWindow(x, y, clientW, clientH);
		[gWin makeFirstResponder:gView];
		[gView layerResized];
	}
}

// the rule of the scaling; the picture is shown again by it
void OS_WindowSetScaling(int rule)
{
	int steps = (rule ^ gScaleRule) & OPENBGI_SCALE_WHOLE;
	gScaleRule = rule;
	if(steps)
		Recompute(); // (the steps decide how large the picture is)
	gDirty = 1;
}

int OS_WindowScales(void)
{
	return 1;
}

// no filters beyond the three rules, and every blit is shown as it is made
const char* OS_WindowFilterName(int n)
{
	BGI_UNUSED(n);
	return NULL;
}

void OS_WindowPresent(void)
{
}

// no surface is said here: no layer
void OS_WindowLayer(const unsigned char* pixels, int w, int h, int pitch, int x, int y, int cw, int ch)
{
	BGI_UNUSED(pixels);
	BGI_UNUSED(w);
	BGI_UNUSED(h);
	BGI_UNUSED(pitch);
	BGI_UNUSED(x);
	BGI_UNUSED(y);
	BGI_UNUSED(cw);
	BGI_UNUSED(ch);
}

// the picture the engine draws: scaled into a windowed client area of another size (Recompute)
void OS_WindowSetPicture(int w, int h)
{
	if(gFullscreen || w <= 0 || h <= 0)
		return;
	gModeW = w;
	gModeH = h;
	Recompute();
	gDirty = 1;
}

/* The icon: the application's in the Dock, from the largest picture (a
 * window has none of its own here).  The pictures are blue, green, red,
 * alpha, not multiplied in. */
void OS_WindowSetIcon(const OpenBGI_Icon_t* icons, int count)
{
	if(count <= 0 || !icons || !icons[0].pixels || icons[0].width <= 0 || icons[0].height <= 0)
		return;
	Mac_AppInit();
	@autoreleasepool
	{
		int w = icons[0].width, h = icons[0].height, i;
		NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL
																		pixelsWide:w
																		pixelsHigh:h
																	 bitsPerSample:8
																   samplesPerPixel:4
																		  hasAlpha:YES
																		  isPlanar:NO
																	colorSpaceName:NSDeviceRGBColorSpace
																	  bitmapFormat:NSBitmapFormatAlphaNonpremultiplied
																	   bytesPerRow:w * 4
																	  bitsPerPixel:32];
		unsigned char* out = rep ? rep.bitmapData : NULL;
		NSImage* image;
		if(!out)
			return;
		for(i = 0; i < w * h; i++)
		{
			const uint8_t* p = icons[0].pixels + (size_t)i * 4;
			out[i * 4 + 0] = p[2];
			out[i * 4 + 1] = p[1];
			out[i * 4 + 2] = p[0];
			out[i * 4 + 3] = p[3];
		}
		image = [[NSImage alloc] initWithSize:NSMakeSize(w, h)];
		[image addRepresentation:rep];
		NSApp.applicationIconImage = image;
	}
}

int OS_WindowHasIcons(void)
{
	return 1;
}

void OS_DragAccept(int accept)
{
	gDragAccept = accept != 0;
	if(!gView)
		return;
	@autoreleasepool
	{
		if(gDragAccept)
			[gView registerForDraggedTypes:@[NSPasteboardTypeFileURL]];
		else
			[gView unregisterDraggedTypes];
	}
}

// ---- cursor --------------------------------------------------------------------------------------

void OS_CursorSetShape(int shape)
{
	gCursorShape = shape;
	ApplyCursor();
}

// ShowCursor's counter: +1 / -1, the cursor is visible while the count is not negative
void OS_CursorShow(int show)
{
	gCursorCount += show ? 1 : -1;
	ApplyCursor();
}

/* the pointer in screen coordinates (in full screen: mode coordinates,
 * since that is the engine's screen; over a scaled picture: screen
 * coordinates whose client part is in the picture's pixels) */
void OS_CursorGetPos(int* x, int* y)
{
	@autoreleasepool
	{
		NSPoint m = [NSEvent mouseLocation];
		double sx = m.x, sy = PrimaryHeight() - m.y, cx, cy;
		*x = (int)floor(sx);
		*y = (int)floor(sy);
		if(gWin && gMapOn)
		{
			ClientOrigin(&cx, &cy);
			*x = (gFullscreen ? 0 : (int)cx) + FromViewX(sx - cx);
			*y = (gFullscreen ? 0 : (int)cy) + FromViewY(sy - cy);
		}
	}
}

// SetCursorPos: the pointer there, through the same mapping
void OS_CursorSetPos(int x, int y)
{
	double sx = x, sy = y, cx, cy;
	if(gWin && gMapOn)
	{
		@autoreleasepool
		{
			ClientOrigin(&cx, &cy);
		}
		sx = cx + ToViewX(gFullscreen ? x : x - (int)cx);
		sy = cy + ToViewY(gFullscreen ? y : y - (int)cy);
	}
	CGWarpMouseCursorPosition(CGPointMake(sx, sy));
	CGAssociateMouseAndMouseCursorPosition(true); // (a warp holds the pointer still for a moment otherwise)
}
