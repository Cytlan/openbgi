/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * QuartzCore.h - a stand-in for the layers the macOS back end uses
 * (src/host/macos/), for `make check-macos`; never used by the real build.
 */
#ifndef MACSTUB_QUARTZCORE_H_
#define MACSTUB_QUARTZCORE_H_

#pragma GCC system_header

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>

typedef NSString* CALayerContentsGravity;
typedef NSString* CALayerContentsFilter;
extern CALayerContentsGravity const kCAGravityResize;
extern CALayerContentsFilter const kCAFilterNearest;

@interface CALayer : NSObject
+ (instancetype)layer;
@property CGFloat contentsScale;
@property(strong) id contents;
@property(copy) CALayerContentsGravity contentsGravity;
@property(copy) CALayerContentsFilter magnificationFilter;
@property(getter=isOpaque) BOOL opaque;
@end

@protocol CAMetalDrawable <MTLDrawable>
@property(readonly) id<MTLTexture> texture;
@end

@interface CAMetalLayer : CALayer
@property(strong) id<MTLDevice> device;
@property MTLPixelFormat pixelFormat;
@property BOOL framebufferOnly;
@property CGSize drawableSize;
@property BOOL displaySyncEnabled;
@property CGColorSpaceRef colorspace;
- (id<CAMetalDrawable>)nextDrawable;
@end

#endif // MACSTUB_QUARTZCORE_H_
