/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * present.m - the main window's picture through Metal (macos_internal.h)
 *
 * The picture is a texture of its own size, which the engine's blits
 * fill a rectangle at a time (MacPresent_Upload).  MacPresent_Draw
 * clears the window's layer and draws the texture over a rectangle of
 * it, scaled by the graphics processor: by the nearest pixel, by the
 * four around a point (bilinear) or by sixteen (bicubic, the
 * Catmull-Rom curve), as the caller says.
 *
 * The two shader functions are compiled from the text below when the
 * first window is made: a Mac without Xcode has no compiler for them
 * at build time.
 */
#import "macos_internal.h"

static id<MTLDevice> gDevice;
static id<MTLCommandQueue> gCommands;
static id<MTLRenderPipelineState> gPipeline;
static id<MTLSamplerState> gSamplerLinear, gSamplerNearest;
static id<MTLTexture> gTexture; // the picture
static int gTexW, gTexH;
static CAMetalLayer* gLayer;
static uint32_t* gRows; // pixels made ready for the texture
static size_t gRowsCap;

/* One triangle that covers the viewport, its texture coordinates running
 * over the picture; the fragments take the picture's colour at theirs. */
static NSString* const kShaders = @"#include <metal_stdlib>\n"
								   "using namespace metal;\n"
								   "struct VOut { float4 pos [[position]]; float2 uv; };\n"
								   "struct Uniforms { float2 size; int cubic; int pad; };\n"
								   "vertex VOut vmain(uint vid [[vertex_id]])\n"
								   "{\n"
								   "    VOut o;\n"
								   "    float2 p = float2(float((vid << 1) & 2), float(vid & 2));\n"
								   "    o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);\n"
								   "    o.uv = float2(p.x, 1.0 - p.y);\n"
								   "    return o;\n"
								   "}\n"
								   "static float4 weights(float t)\n"
								   "{\n"
								   "    return float4(((-0.5 * t + 1.0) * t - 0.5) * t, (1.5 * t - 2.5) * t * t + 1.0,\n"
								   "                  ((-1.5 * t + 2.0) * t + 0.5) * t, (0.5 * t - 0.5) * t * t);\n"
								   "}\n"
								   "fragment float4 fmain(VOut in [[stage_in]], texture2d<float> tex [[texture(0)]],\n"
								   "                      sampler smp [[sampler(0)]], constant Uniforms& u [[buffer(0)]])\n"
								   "{\n"
								   "    float3 c;\n"
								   "    if(u.cubic != 0)\n"
								   "    {\n"
								   "        float2 pos = in.uv * u.size - 0.5;\n"
								   "        float2 ip = floor(pos);\n"
								   "        float2 f = pos - ip;\n"
								   "        float4 wx = weights(f.x);\n"
								   "        float4 wy = weights(f.y);\n"
								   "        float2 last = u.size - 1.0;\n"
								   "        c = float3(0.0);\n"
								   "        for(int j = 0; j < 4; j++)\n"
								   "        {\n"
								   "            float3 row = float3(0.0);\n"
								   "            for(int i = 0; i < 4; i++)\n"
								   "            {\n"
								   "                float2 q = clamp(ip + float2(float(i - 1), float(j - 1)), float2(0.0), last);\n"
								   "                row += tex.read(uint2(q)).rgb * wx[i];\n"
								   "            }\n"
								   "            c += row * wy[j];\n"
								   "        }\n"
								   "        c = clamp(c, 0.0, 1.0);\n"
								   "    }\n"
								   "    else\n"
								   "        c = tex.sample(smp, in.uv).rgb;\n"
								   "    return float4(c, 1.0);\n"
								   "}\n";

// what the fragment function is told (the shader's Uniforms)
typedef struct Uniforms
{
	float size[2]; // the picture's size in pixels
	int32_t cubic; // 1: the sixteen pixels around the point
	int32_t pad;
} Uniforms_t;

static void Complain(const char* what, NSError* error)
{
	NSString* text = error ? error.localizedDescription : nil;
	fprintf(stderr, "openbgi: Metal: %s%s%s\n", what, text ? ": " : "", text ? text.UTF8String : "");
}

// the device and what is made once for it; 0 without Metal
static int Setup(void)
{
	NSError* error = nil;
	id<MTLLibrary> library;
	MTLRenderPipelineDescriptor* pipeline;
	MTLSamplerDescriptor* sampler;
	if(gDevice)
		return 1;
	gDevice = MTLCreateSystemDefaultDevice();
	if(!gDevice)
	{
		Complain("no device", nil);
		return 0;
	}
	gCommands = [gDevice newCommandQueue];
	library = [gDevice newLibraryWithSource:kShaders options:nil error:&error];
	if(!library)
	{
		Complain("the shaders do not compile", error);
		gDevice = nil;
		return 0;
	}
	pipeline = [[MTLRenderPipelineDescriptor alloc] init];
	pipeline.vertexFunction = [library newFunctionWithName:@"vmain"];
	pipeline.fragmentFunction = [library newFunctionWithName:@"fmain"];
	pipeline.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
	gPipeline = [gDevice newRenderPipelineStateWithDescriptor:pipeline error:&error];
	if(!gPipeline)
	{
		Complain("no pipeline", error);
		gDevice = nil;
		return 0;
	}
	sampler = [[MTLSamplerDescriptor alloc] init];
	sampler.sAddressMode = MTLSamplerAddressModeClampToEdge;
	sampler.tAddressMode = MTLSamplerAddressModeClampToEdge;
	sampler.minFilter = MTLSamplerMinMagFilterLinear;
	sampler.magFilter = MTLSamplerMinMagFilterLinear;
	gSamplerLinear = [gDevice newSamplerStateWithDescriptor:sampler];
	sampler.minFilter = MTLSamplerMinMagFilterNearest;
	sampler.magFilter = MTLSamplerMinMagFilterNearest;
	gSamplerNearest = [gDevice newSamplerStateWithDescriptor:sampler];
	return 1;
}

/* The layer of the window's view: the device's, in the picture's pixel
 * format and sRGB (the colours the games were drawn in, which the system
 * matches to the display), shown as soon as drawn - the engine paces its
 * frames itself. */
int MacPresent_Attach(CAMetalLayer* layer)
{
	CGColorSpaceRef space;
	if(!layer || !Setup())
		return 0;
	gLayer = layer;
	layer.device = gDevice;
	layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
	layer.framebufferOnly = YES;
	layer.opaque = YES;
	layer.displaySyncEnabled = NO;
	space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
	layer.colorspace = space;
	CGColorSpaceRelease(space);
	return 1;
}

// the window is gone: the picture with it (the device stays for the next one)
void MacPresent_Detach(void)
{
	gLayer = nil;
	gTexture = nil;
	gTexW = gTexH = 0;
}

int MacPresent_Size(int* w, int* h)
{
	if(!gTexture)
		return 0;
	*w = gTexW;
	*h = gTexH;
	return 1;
}

// room for n pixels in gRows; 0 without memory
static int Rows(size_t n)
{
	if(n > gRowsCap)
	{
		uint32_t* p = (uint32_t*)realloc(gRows, n * sizeof *p);
		if(!p)
			return 0;
		gRows = p;
		gRowsCap = n;
	}
	return 1;
}

void MacPresent_Clear(void)
{
	size_t n = (size_t)gTexW * (size_t)gTexH;
	if(!gTexture || !Rows(n))
		return;
	memset(gRows, 0, n * sizeof *gRows);
	[gTexture replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)gTexW, (NSUInteger)gTexH)
				mipmapLevel:0
				  withBytes:gRows
				bytesPerRow:(NSUInteger)gTexW * 4];
}

// a black texture of w x h for the picture; 0 when the device gives none
static int NewTexture(int w, int h)
{
	MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
																				 width:(NSUInteger)w
																				height:(NSUInteger)h
																			 mipmapped:NO];
	d.usage = MTLTextureUsageShaderRead;
	gTexture = [gDevice newTextureWithDescriptor:d];
	gTexW = gTexture ? w : 0;
	gTexH = gTexture ? h : 0;
	if(!gTexture)
		return 0;
	MacPresent_Clear();
	return 1;
}

/* A rectangle of the surface into the picture.  A surface of another size
 * than the picture's is a new picture: the texture is made anew, black,
 * and the host is asked for all of it (the window's `paint`).  32-bit rows
 * go as they are; 16-bit ones (RGB555) are widened first. */
int MacPresent_Upload(const OsSurface_t* s, int dstX, int dstY, int w, int h, int srcX, int srcY)
{
	MTLRegion region;
	if(!gDevice || !s || !s->pixels || s->width <= 0 || s->height <= 0)
		return 0;
	if(!gTexture || gTexW != s->width || gTexH != s->height)
	{
		if(!NewTexture(s->width, s->height))
			return 0;
		Mac_Post(MAC_EV_PAINT, 0, 0, 0, NULL);
	}
	// clipped to the surface and to the picture
	if(srcX < 0)
	{
		w += srcX;
		dstX -= srcX;
		srcX = 0;
	}
	if(srcY < 0)
	{
		h += srcY;
		dstY -= srcY;
		srcY = 0;
	}
	if(dstX < 0)
	{
		w += dstX;
		srcX -= dstX;
		dstX = 0;
	}
	if(dstY < 0)
	{
		h += dstY;
		srcY -= dstY;
		dstY = 0;
	}
	if(srcX + w > s->width)
		w = s->width - srcX;
	if(srcY + h > s->height)
		h = s->height - srcY;
	if(dstX + w > gTexW)
		w = gTexW - dstX;
	if(dstY + h > gTexH)
		h = gTexH - dstY;
	if(w <= 0 || h <= 0)
		return 0;
	region = MTLRegionMake2D((NSUInteger)dstX, (NSUInteger)dstY, (NSUInteger)w, (NSUInteger)h);
	if(s->bpp == 32 && s->pitch > 0)
	{
		const uint8_t* first = (const uint8_t*)s->pixels + (ptrdiff_t)srcY * s->pitch + (ptrdiff_t)srcX * 4;
		[gTexture replaceRegion:region mipmapLevel:0 withBytes:first bytesPerRow:(NSUInteger)s->pitch];
		return 1;
	}
	if(!Rows((size_t)w * (size_t)h))
		return 0;
	for(int y = 0; y < h; y++)
	{
		const uint8_t* row = (const uint8_t*)s->pixels + (ptrdiff_t)(srcY + y) * s->pitch;
		uint32_t* out = gRows + (size_t)y * (size_t)w;
		if(s->bpp == 32)
			memcpy(out, row + (size_t)srcX * 4, (size_t)w * 4);
		else
		{
			const uint16_t* in = (const uint16_t*)(const void*)row + srcX;
			for(int x = 0; x < w; x++)
			{
				uint32_t p = in[x], r = (p >> 10) & 31, g = (p >> 5) & 31, b = p & 31;
				out[x] = ((r << 3 | r >> 2) << 16) | ((g << 3 | g >> 2) << 8) | (b << 3 | b >> 2);
			}
		}
	}
	[gTexture replaceRegion:region mipmapLevel:0 withBytes:gRows bytesPerRow:(NSUInteger)w * 4];
	return 1;
}

/* The layer's next picture: black, and the texture over the rectangle
 * (the layer's pixels, from its top left) when there is one.  Nothing is
 * drawn when the layer has no picture to give (a window nobody sees). */
void MacPresent_Draw(double x, double y, double w, double h, int filter)
{
	id<CAMetalDrawable> drawable;
	id<MTLCommandBuffer> commands;
	id<MTLRenderCommandEncoder> pass;
	MTLRenderPassDescriptor* target;
	if(!gDevice || !gLayer)
		return;
	drawable = [gLayer nextDrawable];
	if(!drawable)
		return;
	target = [MTLRenderPassDescriptor renderPassDescriptor];
	target.colorAttachments[0].texture = drawable.texture;
	target.colorAttachments[0].loadAction = MTLLoadActionClear;
	target.colorAttachments[0].storeAction = MTLStoreActionStore;
	target.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
	commands = [gCommands commandBuffer];
	pass = [commands renderCommandEncoderWithDescriptor:target];
	if(gTexture && w >= 1.0 && h >= 1.0)
	{
		Uniforms_t u;
		MTLViewport viewport;
		u.size[0] = (float)gTexW;
		u.size[1] = (float)gTexH;
		u.cubic = filter == MAC_FILTER_BICUBIC;
		u.pad = 0;
		viewport.originX = x;
		viewport.originY = y;
		viewport.width = w;
		viewport.height = h;
		viewport.znear = 0.0;
		viewport.zfar = 1.0;
		[pass setViewport:viewport];
		[pass setRenderPipelineState:gPipeline];
		[pass setFragmentTexture:gTexture atIndex:0];
		[pass setFragmentSamplerState:(filter == MAC_FILTER_NEAREST ? gSamplerNearest : gSamplerLinear) atIndex:0];
		[pass setFragmentBytes:&u length:sizeof u atIndex:0];
		[pass drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
	}
	[pass endEncoding];
	[commands presentDrawable:drawable];
	[commands commit];
}
