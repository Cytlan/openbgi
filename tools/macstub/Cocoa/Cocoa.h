/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Cocoa.h - a stand-in for the part of Foundation, AppKit and Core
 * Graphics the macOS back end uses (src/host/macos/), for
 * `make check-macos`; never used by the real build.
 *
 * Declarations only, as the back end spells them: the check finds what
 * does not parse and what does not fit these, not what a Mac's own
 * headers would say.
 */
#ifndef MACSTUB_COCOA_H_
#define MACSTUB_COCOA_H_

#pragma GCC system_header

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ---- the language's own -----------------------------------------------------------------------
typedef signed char BOOL;
#define YES ((BOOL)1)
#define NO  ((BOOL)0)
#define nil ((id)0)
typedef long NSInteger;
typedef unsigned long NSUInteger;
typedef unsigned short unichar;
typedef double NSTimeInterval;
typedef double CGFloat;
#define NSNotFound ((NSInteger)0x7fffffffffffffffL)

typedef struct NSRange { NSUInteger location, length; } NSRange;
typedef NSRange* NSRangePointer;
static inline NSRange NSMakeRange(NSUInteger loc, NSUInteger len) { NSRange r = {loc, len}; return r; }

typedef struct CGPoint { CGFloat x, y; } CGPoint;
typedef struct CGSize { CGFloat width, height; } CGSize;
typedef struct CGRect { CGPoint origin; CGSize size; } CGRect;
typedef CGPoint NSPoint;
typedef CGSize NSSize;
typedef CGRect NSRect;
typedef struct NSEdgeInsets { CGFloat top, left, bottom, right; } NSEdgeInsets;
static inline CGPoint CGPointMake(CGFloat x, CGFloat y) { CGPoint p = {x, y}; return p; }
static inline CGSize CGSizeMake(CGFloat w, CGFloat h) { CGSize s = {w, h}; return s; }
static inline NSPoint NSMakePoint(CGFloat x, CGFloat y) { NSPoint p = {x, y}; return p; }
static inline NSSize NSMakeSize(CGFloat w, CGFloat h) { NSSize s = {w, h}; return s; }
static inline NSRect NSMakeRect(CGFloat x, CGFloat y, CGFloat w, CGFloat h) { NSRect r = {{x, y}, {w, h}}; return r; }
static inline CGFloat NSMaxY(NSRect r) { return r.origin.y + r.size.height; }
static inline CGFloat NSMaxX(NSRect r) { return r.origin.x + r.size.width; }
extern const NSPoint NSZeroPoint;
extern const NSRect NSZeroRect;

// ---- Foundation -------------------------------------------------------------------------------
@class NSString, NSArray<ObjectType>, NSDictionary<KeyType, ObjectType>, NSURL, NSError, NSTimer, NSDate, NSData;

typedef struct { unsigned long state; id __unsafe_unretained* itemsPtr; unsigned long* mutationsPtr; unsigned long extra[5]; } NSFastEnumerationState;
@protocol NSFastEnumeration
- (NSUInteger)countByEnumeratingWithState:(NSFastEnumerationState*)state objects:(id __unsafe_unretained[])buffer count:(NSUInteger)len;
@end

@protocol NSObject
- (BOOL)isKindOfClass:(Class)c;
@end

__attribute__((objc_root_class))
@interface NSObject <NSObject>
+ (instancetype)alloc;
- (instancetype)init;
+ (Class)class;
- (id)copy;
@end

@interface NSString : NSObject
+ (instancetype)stringWithUTF8String:(const char*)s;
@property(readonly) const char* UTF8String;
@property(readonly) const char* fileSystemRepresentation;
@property(readonly) NSUInteger length;
@property(readonly, copy) NSString* pathExtension;
- (unichar)characterAtIndex:(NSUInteger)i;
- (NSString*)substringWithRange:(NSRange)r;
- (BOOL)isEqualToString:(NSString*)s;
- (NSArray<NSString*>*)componentsSeparatedByString:(NSString*)s;
- (NSString*)stringByTrimmingCharactersInSet:(id)set;
@end
@interface NSConstantString : NSString
@end
@interface NSMutableString : NSString
+ (instancetype)string;
- (void)setString:(NSString*)s;
- (void)appendString:(NSString*)s;
@end
typedef NSString* NSAttributedStringKey;
typedef NSString* NSRunLoopMode;
typedef NSString* NSPasteboardType;
typedef NSString* NSPasteboardReadingOptionKey;
@interface NSAttributedString : NSObject
@property(readonly, copy) NSString* string;
@end
@interface NSCharacterSet : NSObject
+ (NSCharacterSet*)whitespaceCharacterSet;
@end
@interface NSNumber : NSObject
+ (NSNumber*)numberWithBool:(BOOL)b;
@end
@interface NSArray<ObjectType> : NSObject <NSFastEnumeration>
+ (instancetype)arrayWithObjects:(const ObjectType[])objects count:(NSUInteger)n;
@property(readonly) NSUInteger count;
- (ObjectType)objectAtIndexedSubscript:(NSUInteger)i;
@end
@interface NSMutableArray<ObjectType> : NSArray<ObjectType>
+ (instancetype)array;
- (void)addObject:(ObjectType)o;
@end
@interface NSDictionary<KeyType, ObjectType> : NSObject
+ (instancetype)dictionaryWithObjects:(const ObjectType[])objects forKeys:(const KeyType[])keys count:(NSUInteger)n;
@end
@interface NSError : NSObject
@property(readonly, copy) NSString* localizedDescription;
@end
@interface NSURL : NSObject
+ (NSURL*)fileURLWithPath:(NSString*)path isDirectory:(BOOL)dir;
@property(readonly, copy) NSString* path;
@property(readonly) const char* fileSystemRepresentation;
@end
@interface NSData : NSObject
@end
@interface NSMutableData : NSData
+ (instancetype)dataWithLength:(NSUInteger)n;
@property(readonly) void* mutableBytes;
@end
@interface NSDate : NSObject
@end
@interface NSNotification : NSObject
@end
@interface NSTimer : NSObject
+ (NSTimer*)timerWithTimeInterval:(NSTimeInterval)t repeats:(BOOL)r block:(void (^)(NSTimer* timer))block;
@end
extern NSRunLoopMode const NSDefaultRunLoopMode;
extern NSRunLoopMode const NSRunLoopCommonModes;
@interface NSRunLoop : NSObject
+ (NSRunLoop*)currentRunLoop;
- (void)addTimer:(NSTimer*)t forMode:(NSRunLoopMode)mode;
@end

// ---- Core Graphics ----------------------------------------------------------------------------
typedef int32_t CGError;
typedef uint32_t CGBitmapInfo;
typedef const struct MacstubCFData* CFDataRef;
typedef const struct MacstubCFString* CFStringRef;
typedef struct MacstubCGColorSpace* CGColorSpaceRef;
typedef struct MacstubCGDataProvider* CGDataProviderRef;
typedef struct MacstubCGImage* CGImageRef;
enum { kCGImageAlphaNoneSkipFirst = 6 };
enum { kCGBitmapByteOrder32Little = 2 << 12 };
typedef enum { kCGRenderingIntentDefault } CGColorRenderingIntent;
extern const CFStringRef kCGColorSpaceSRGB;
CGError CGWarpMouseCursorPosition(CGPoint p);
CGError CGAssociateMouseAndMouseCursorPosition(bool connected);
CGColorSpaceRef CGColorSpaceCreateWithName(CFStringRef name);
void CGColorSpaceRelease(CGColorSpaceRef s);
CGDataProviderRef CGDataProviderCreateWithCFData(CFDataRef d);
void CGDataProviderRelease(CGDataProviderRef p);
CGImageRef CGImageCreate(size_t w, size_t h, size_t bitsPerComponent, size_t bitsPerPixel, size_t bytesPerRow, CGColorSpaceRef space,
	CGBitmapInfo info, CGDataProviderRef provider, const CGFloat* decode, bool interpolate, CGColorRenderingIntent intent);
void CGImageRelease(CGImageRef i);

// ---- AppKit -----------------------------------------------------------------------------------
@class NSWindow, NSView, NSEvent, NSMenu, NSMenuItem, NSScreen, NSColor, NSFont, NSImage, NSCursor, NSPasteboard, NSTextInputContext,
	NSApplication, NSButton, CALayer;

typedef NSUInteger NSEventModifierFlags;
enum
{
	NSEventModifierFlagCapsLock = 1 << 16,
	NSEventModifierFlagShift = 1 << 17,
	NSEventModifierFlagControl = 1 << 18,
	NSEventModifierFlagOption = 1 << 19,
	NSEventModifierFlagCommand = 1 << 20,
};
typedef NSUInteger NSEventType;
enum { NSEventTypeKeyDown = 10, NSEventTypeKeyUp = 11 };
typedef unsigned long long NSEventMask;
#define NSEventMaskAny (~0ULL)
typedef NSUInteger NSWindowStyleMask;
enum
{
	NSWindowStyleMaskBorderless = 0,
	NSWindowStyleMaskTitled = 1,
	NSWindowStyleMaskClosable = 2,
	NSWindowStyleMaskMiniaturizable = 4,
	NSWindowStyleMaskResizable = 8,
};
typedef NSUInteger NSBackingStoreType;
enum { NSBackingStoreBuffered = 2 };
typedef NSUInteger NSWindowOcclusionState;
typedef NSUInteger NSWindowCollectionBehavior;
enum
{
	NSWindowCollectionBehaviorFullScreenNone = 1 << 9
};
enum { NSWindowOcclusionStateVisible = 2 };
typedef NSUInteger NSApplicationPresentationOptions;
enum { NSApplicationPresentationDefault = 0, NSApplicationPresentationHideDock = 2, NSApplicationPresentationHideMenuBar = 8 };
typedef NSInteger NSApplicationActivationPolicy;
enum { NSApplicationActivationPolicyRegular = 0 };
typedef NSUInteger NSApplicationTerminateReply;
enum { NSTerminateCancel = 0, NSTerminateNow = 1 };
typedef NSUInteger NSApplicationDelegateReply;
enum { NSApplicationDelegateReplySuccess = 0 };
typedef NSUInteger NSDragOperation;
enum { NSDragOperationNone = 0, NSDragOperationCopy = 1 };
typedef NSInteger NSModalResponse;
enum { NSModalResponseOK = 1, NSAlertFirstButtonReturn = 1000, NSAlertSecondButtonReturn = 1001 };
typedef NSUInteger NSAlertStyle;
enum { NSAlertStyleWarning = 0, NSAlertStyleInformational = 1, NSAlertStyleCritical = 2 };
typedef NSUInteger NSBitmapFormat;
enum { NSBitmapFormatAlphaNonpremultiplied = 2 };
extern NSString* const NSDeviceRGBColorSpace;
extern NSPasteboardType const NSPasteboardTypeString;
extern NSPasteboardType const NSPasteboardTypeFileURL;
extern NSPasteboardReadingOptionKey const NSPasteboardURLReadingFileURLsOnlyKey;

@interface NSResponder : NSObject
- (void)interpretKeyEvents:(NSArray<NSEvent*>*)events;
@end

@interface NSEvent : NSObject
@property(readonly) NSEventType type;
@property(readonly) NSEventModifierFlags modifierFlags;
@property(readonly) unsigned short keyCode;
@property(readonly, copy) NSString* characters;
@property(readonly, copy) NSString* charactersIgnoringModifiers;
@property(readonly, getter=isARepeat) BOOL ARepeat;
@property(readonly) NSPoint locationInWindow;
@property(readonly) NSInteger clickCount;
@property(readonly) NSInteger buttonNumber;
@property(readonly) BOOL hasPreciseScrollingDeltas;
@property(readonly) CGFloat scrollingDeltaY;
+ (NSPoint)mouseLocation;
@end

@interface NSScreen : NSObject
+ (NSArray<NSScreen*>*)screens;
+ (NSScreen*)mainScreen;
@property(readonly) NSRect frame;
@property(readonly) NSRect visibleFrame;
@property(readonly) NSEdgeInsets safeAreaInsets;
@end

@interface NSColor : NSObject
+ (NSColor*)blackColor;
+ (NSColor*)textBackgroundColor;
+ (NSColor*)textColor;
@end
@interface NSFont : NSObject
+ (NSFont*)systemFontOfSize:(CGFloat)size;
@end
@interface NSImageRep : NSObject
@end
@interface NSBitmapImageRep : NSImageRep
- (instancetype)initWithBitmapDataPlanes:(unsigned char**)planes
							  pixelsWide:(NSInteger)w
							  pixelsHigh:(NSInteger)h
						   bitsPerSample:(NSInteger)bps
						 samplesPerPixel:(NSInteger)spp
								hasAlpha:(BOOL)alpha
								isPlanar:(BOOL)planar
						  colorSpaceName:(NSString*)space
							bitmapFormat:(NSBitmapFormat)format
							 bytesPerRow:(NSInteger)rowBytes
							bitsPerPixel:(NSInteger)bpp;
@property(readonly) unsigned char* bitmapData;
@end
@interface NSImage : NSObject
- (instancetype)initWithSize:(NSSize)size;
- (void)addRepresentation:(NSImageRep*)rep;
@end
@interface NSCursor : NSObject
- (instancetype)initWithImage:(NSImage*)image hotSpot:(NSPoint)p;
+ (NSCursor*)arrowCursor;
+ (NSCursor*)pointingHandCursor;
@end
@interface NSPasteboard : NSObject
+ (NSPasteboard*)generalPasteboard;
- (NSInteger)clearContents;
- (BOOL)setString:(NSString*)s forType:(NSPasteboardType)type;
- (NSArray*)readObjectsForClasses:(NSArray<Class>*)classes options:(NSDictionary<NSPasteboardReadingOptionKey, id>*)options;
@end
@protocol NSDraggingInfo <NSObject>
@property(readonly) NSPasteboard* draggingPasteboard;
@end
@interface NSTextInputContext : NSObject
- (void)discardMarkedText;
@end
@protocol NSTextInputClient
- (void)insertText:(id)string replacementRange:(NSRange)replacementRange;
- (void)doCommandBySelector:(SEL)selector;
- (void)setMarkedText:(id)string selectedRange:(NSRange)selectedRange replacementRange:(NSRange)replacementRange;
- (void)unmarkText;
- (NSRange)selectedRange;
- (NSRange)markedRange;
- (BOOL)hasMarkedText;
- (NSAttributedString*)attributedSubstringForProposedRange:(NSRange)range actualRange:(NSRangePointer)actualRange;
- (NSArray<NSAttributedStringKey>*)validAttributesForMarkedText;
- (NSRect)firstRectForCharacterRange:(NSRange)range actualRange:(NSRangePointer)actualRange;
- (NSUInteger)characterIndexForPoint:(NSPoint)point;
@end

@interface NSView : NSResponder
- (instancetype)initWithFrame:(NSRect)frame;
@property(readonly) NSRect bounds;
@property NSRect frame;
@property(readonly, weak) NSWindow* window;
@property(strong) CALayer* layer;
@property BOOL wantsLayer;
@property(getter=isHidden) BOOL hidden;
@property(readonly, strong) NSTextInputContext* inputContext;
- (void)addSubview:(NSView*)view;
- (NSPoint)convertPoint:(NSPoint)p fromView:(NSView*)view;
- (NSRect)convertRect:(NSRect)r toView:(NSView*)view;
- (void)setFrameSize:(NSSize)size;
- (void)viewDidChangeBackingProperties;
- (void)resetCursorRects;
- (void)addCursorRect:(NSRect)r cursor:(NSCursor*)c;
- (void)registerForDraggedTypes:(NSArray<NSPasteboardType>*)types;
- (void)unregisterDraggedTypes;
@end
@interface NSControl : NSView
@property(copy) NSString* stringValue;
@property(strong) NSFont* font;
- (void)sizeToFit;
@end
@interface NSTextField : NSControl
+ (instancetype)labelWithString:(NSString*)s;
@property BOOL drawsBackground;
@property(copy) NSColor* backgroundColor;
@property(copy) NSColor* textColor;
@end
@interface NSButton : NSControl
@property(copy) NSString* keyEquivalent;
@end

@protocol NSWindowDelegate <NSObject>
@end
@interface NSWindow : NSResponder
- (instancetype)initWithContentRect:(NSRect)r styleMask:(NSWindowStyleMask)style backing:(NSBackingStoreType)backing defer:(BOOL)defer;
+ (NSRect)frameRectForContentRect:(NSRect)r styleMask:(NSWindowStyleMask)style;
- (NSRect)frameRectForContentRect:(NSRect)r;
- (NSRect)contentRectForFrameRect:(NSRect)r;
@property(readonly) NSRect frame;
- (void)setFrame:(NSRect)r display:(BOOL)display;
@property(readonly, strong) NSScreen* screen;
@property(copy) NSString* title;
@property NSWindowStyleMask styleMask;
@property(strong) NSView* contentView;
@property(weak) id<NSWindowDelegate> delegate;
@property(getter=isReleasedWhenClosed) BOOL releasedWhenClosed;
@property(copy) NSColor* backgroundColor;
@property BOOL acceptsMouseMovedEvents;
@property NSSize contentMinSize;
@property NSSize contentAspectRatio;
@property NSSize contentResizeIncrements;
@property NSWindowCollectionBehavior collectionBehavior;
@property(readonly) CGFloat backingScaleFactor;
@property(readonly) NSWindowOcclusionState occlusionState;
- (BOOL)isVisible;
- (BOOL)isMiniaturized;
- (void)miniaturize:(id)sender;
- (void)makeKeyAndOrderFront:(id)sender;
- (void)orderOut:(id)sender;
- (void)close;
- (BOOL)makeFirstResponder:(NSResponder*)r;
- (void)invalidateCursorRectsForView:(NSView*)view;
- (NSRect)convertRectToScreen:(NSRect)r;
- (void)sendEvent:(NSEvent*)e;
@end

@interface NSMenuItem : NSObject
- (instancetype)initWithTitle:(NSString*)title action:(SEL)action keyEquivalent:(NSString*)key;
+ (NSMenuItem*)separatorItem;
@property NSEventModifierFlags keyEquivalentModifierMask;
@property(weak) id target;
@property(strong) NSMenu* submenu;
@end
@interface NSMenu : NSObject
- (instancetype)initWithTitle:(NSString*)title;
- (void)addItem:(NSMenuItem*)item;
- (NSMenuItem*)addItemWithTitle:(NSString*)title action:(SEL)action keyEquivalent:(NSString*)key;
@end

@protocol NSApplicationDelegate <NSObject>
@end
@interface NSApplication : NSResponder
+ (NSApplication*)sharedApplication;
@property(weak) id<NSApplicationDelegate> delegate;
@property(strong) NSMenu* mainMenu;
@property(strong) NSMenu* windowsMenu;
@property(readonly, weak) NSWindow* keyWindow;
@property(strong) NSImage* applicationIconImage;
- (BOOL)setActivationPolicy:(NSApplicationActivationPolicy)policy;
- (void)finishLaunching;
- (NSEvent*)nextEventMatchingMask:(NSEventMask)mask untilDate:(NSDate*)date inMode:(NSRunLoopMode)mode dequeue:(BOOL)dequeue;
- (void)sendEvent:(NSEvent*)e;
- (void)activateIgnoringOtherApps:(BOOL)flag;
- (void)setPresentationOptions:(NSApplicationPresentationOptions)options;
- (void)replyToOpenOrPrint:(NSApplicationDelegateReply)reply;
- (void)terminate:(id)sender;
- (void)hide:(id)sender;
- (void)hideOtherApplications:(id)sender;
- (void)unhideAllApplications:(id)sender;
- (void)orderFrontStandardAboutPanel:(id)sender;
@end
extern NSApplication* NSApp;

@interface NSAlert : NSObject
@property(copy) NSString* messageText;
@property(copy) NSString* informativeText;
@property NSAlertStyle alertStyle;
- (NSButton*)addButtonWithTitle:(NSString*)title;
- (NSModalResponse)runModal;
@end
@interface NSSavePanel : NSWindow
+ (NSSavePanel*)savePanel;
@property(copy) NSArray<NSString*>* allowedFileTypes;
@property(copy) NSString* message;
@property(copy) NSURL* directoryURL;
@property(copy) NSString* nameFieldStringValue;
@property BOOL canCreateDirectories;
@property(readonly, copy) NSURL* URL;
- (NSModalResponse)runModal;
@end
@interface NSOpenPanel : NSSavePanel
+ (NSOpenPanel*)openPanel;
@property BOOL canChooseFiles;
@property BOOL canChooseDirectories;
@property BOOL allowsMultipleSelection;
@end

#endif // MACSTUB_COCOA_H_
