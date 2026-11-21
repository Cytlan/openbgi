/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Metal.h - a stand-in for the part of Metal the macOS back end uses
 * (src/host/macos/present.m), for `make check-macos`; never used by the
 * real build.
 */
#ifndef MACSTUB_METAL_H_
#define MACSTUB_METAL_H_

#pragma GCC system_header

#import <Cocoa/Cocoa.h>

typedef NSUInteger MTLPixelFormat;
enum { MTLPixelFormatBGRA8Unorm = 80 };
typedef NSUInteger MTLSamplerMinMagFilter;
enum { MTLSamplerMinMagFilterNearest = 0, MTLSamplerMinMagFilterLinear = 1 };
typedef NSUInteger MTLSamplerAddressMode;
enum { MTLSamplerAddressModeClampToEdge = 0 };
typedef NSUInteger MTLTextureUsage;
enum { MTLTextureUsageShaderRead = 1 };
typedef NSUInteger MTLLoadAction;
enum { MTLLoadActionClear = 2 };
typedef NSUInteger MTLStoreAction;
enum { MTLStoreActionStore = 1 };
typedef NSUInteger MTLPrimitiveType;
enum { MTLPrimitiveTypeTriangle = 3 };

typedef struct { double red, green, blue, alpha; } MTLClearColor;
static inline MTLClearColor MTLClearColorMake(double r, double g, double b, double a) { MTLClearColor c = {r, g, b, a}; return c; }
typedef struct { double originX, originY, width, height, znear, zfar; } MTLViewport;
typedef struct { NSUInteger x, y, z; } MTLOrigin;
typedef struct { NSUInteger width, height, depth; } MTLSize;
typedef struct { MTLOrigin origin; MTLSize size; } MTLRegion;
static inline MTLRegion MTLRegionMake2D(NSUInteger x, NSUInteger y, NSUInteger w, NSUInteger h) { MTLRegion r = {{x, y, 0}, {w, h, 1}}; return r; }

@protocol MTLFunction <NSObject>
@end
@protocol MTLLibrary <NSObject>
- (id<MTLFunction>)newFunctionWithName:(NSString*)name;
@end
@protocol MTLRenderPipelineState <NSObject>
@end
@protocol MTLSamplerState <NSObject>
@end
@protocol MTLTexture <NSObject>
- (void)replaceRegion:(MTLRegion)region mipmapLevel:(NSUInteger)level withBytes:(const void*)bytes bytesPerRow:(NSUInteger)rowBytes;
@end
@protocol MTLDrawable <NSObject>
@end

@interface MTLRenderPipelineColorAttachmentDescriptor : NSObject
@property MTLPixelFormat pixelFormat;
@end
@interface MTLRenderPipelineColorAttachmentDescriptorArray : NSObject
- (MTLRenderPipelineColorAttachmentDescriptor*)objectAtIndexedSubscript:(NSUInteger)i;
@end
@interface MTLRenderPipelineDescriptor : NSObject
@property(strong) id<MTLFunction> vertexFunction;
@property(strong) id<MTLFunction> fragmentFunction;
@property(readonly) MTLRenderPipelineColorAttachmentDescriptorArray* colorAttachments;
@end
@interface MTLSamplerDescriptor : NSObject
@property MTLSamplerMinMagFilter minFilter;
@property MTLSamplerMinMagFilter magFilter;
@property MTLSamplerAddressMode sAddressMode;
@property MTLSamplerAddressMode tAddressMode;
@end
@interface MTLTextureDescriptor : NSObject
+ (MTLTextureDescriptor*)texture2DDescriptorWithPixelFormat:(MTLPixelFormat)format width:(NSUInteger)w height:(NSUInteger)h mipmapped:(BOOL)mip;
@property MTLTextureUsage usage;
@end
@interface MTLRenderPassColorAttachmentDescriptor : NSObject
@property(strong) id<MTLTexture> texture;
@property MTLLoadAction loadAction;
@property MTLStoreAction storeAction;
@property MTLClearColor clearColor;
@end
@interface MTLRenderPassColorAttachmentDescriptorArray : NSObject
- (MTLRenderPassColorAttachmentDescriptor*)objectAtIndexedSubscript:(NSUInteger)i;
@end
@interface MTLRenderPassDescriptor : NSObject
+ (MTLRenderPassDescriptor*)renderPassDescriptor;
@property(readonly) MTLRenderPassColorAttachmentDescriptorArray* colorAttachments;
@end

@protocol MTLRenderCommandEncoder <NSObject>
- (void)setViewport:(MTLViewport)viewport;
- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)state;
- (void)setFragmentTexture:(id<MTLTexture>)texture atIndex:(NSUInteger)i;
- (void)setFragmentSamplerState:(id<MTLSamplerState>)sampler atIndex:(NSUInteger)i;
- (void)setFragmentBytes:(const void*)bytes length:(NSUInteger)n atIndex:(NSUInteger)i;
- (void)drawPrimitives:(MTLPrimitiveType)type vertexStart:(NSUInteger)start vertexCount:(NSUInteger)count;
- (void)endEncoding;
@end
@protocol MTLCommandBuffer <NSObject>
- (id<MTLRenderCommandEncoder>)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor*)d;
- (void)presentDrawable:(id<MTLDrawable>)drawable;
- (void)commit;
@end
@protocol MTLCommandQueue <NSObject>
- (id<MTLCommandBuffer>)commandBuffer;
@end
@protocol MTLDevice <NSObject>
- (id<MTLCommandQueue>)newCommandQueue;
- (id<MTLLibrary>)newLibraryWithSource:(NSString*)source options:(id)options error:(NSError**)error;
- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor*)d error:(NSError**)error;
- (id<MTLSamplerState>)newSamplerStateWithDescriptor:(MTLSamplerDescriptor*)d;
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor*)d;
@end
id<MTLDevice> MTLCreateSystemDefaultDevice(void);

#endif // MACSTUB_METAL_H_
