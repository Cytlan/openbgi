/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/gl.c - test of the picture shown through OpenGL (src/host/os_gl.c)
 *              both ways it has: through OpenGL and through OpenGL ES;
 *              built as test_gl by `make test`
 *
 * The presenter is run on a context without a window, which EGL makes
 * (libEGL is loaded at run time: Mesa has one that draws on the processor
 * where there is no graphics processor), once with OpenGL 3.3 and once
 * with OpenGL ES 3.0.  What each draws is read back and compared with the
 * other's, pixel by pixel.  Where the system has no EGL, or EGL makes one
 * of the two contexts only, the test says so and passes: the build that
 * has no OpenGL (GL=0, and every platform but Linux) says the same.
 *
 * The groups, in the order they run (each for both, then compared):
 *
 *   nearest  - a picture of 32 bits made twice as large by the nearest
 *              pixel, with black around it: every pixel is the picture's
 *              own (red and blue where they belong, the top row at the
 *              top), the border black
 *   rules    - bilinear and bicubic, larger and smaller (the smaller from
 *              the texture's smaller copies)
 *   parts    - a picture written in parts, from a surface with a pitch
 *              wider than its rows and from a place inside it
 *   sixteen  - a picture of 15 bits (RGB555)
 *   chains   - the six Anime4K modes with the tree's shader files (when
 *              the test is run from the tree), the smaller networks, and
 *              the larger ones of the first
 *   layer    - a layer over the picture and the border (OsGl_Layer): an
 *              opaque part hides them, a half clear one is mixed with
 *              them, a clear one - whatever its colours - shows them; a
 *              part of it written anew; a layer that is not the target's
 *              size is not drawn, nor one taken away
 */
#include "host/os_gl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"

#if defined(BGI_GL) && defined(__linux__) && !defined(__ANDROID__)

#include <dlfcn.h>

// ---- EGL as far as it is used (no header of it is included) -------------------------------------

typedef void* EglDisplay_t;
typedef void* EglConfig_t;
typedef void* EglContext_t;
typedef void* EglSurface_t;

#define EGL_ALPHA_SIZE                  0x3021
#define EGL_BLUE_SIZE                   0x3022
#define EGL_GREEN_SIZE                  0x3023
#define EGL_RED_SIZE                    0x3024
#define EGL_SURFACE_TYPE                0x3033
#define EGL_NONE                        0x3038
#define EGL_RENDERABLE_TYPE             0x3040
#define EGL_HEIGHT                      0x3056
#define EGL_WIDTH                       0x3057
#define EGL_EXTENSIONS                  0x3055
#define EGL_PBUFFER_BIT                 0x0001
#define EGL_OPENGL_BIT                  0x0008
#define EGL_OPENGL_ES3_BIT              0x0040
#define EGL_CONTEXT_MAJOR_VERSION       0x3098
#define EGL_CONTEXT_MINOR_VERSION       0x30FB
#define EGL_CONTEXT_OPENGL_PROFILE_MASK 0x30FD
#define EGL_CONTEXT_CORE_PROFILE_BIT    1
#define EGL_OPENGL_ES_API               0x30A0
#define EGL_OPENGL_API                  0x30A2
#define EGL_PLATFORM_SURFACELESS_MESA   0x31DD

static struct
{
	EglDisplay_t (*GetDisplay)(void*);
	unsigned (*Initialize)(EglDisplay_t, int*, int*);
	unsigned (*Terminate)(EglDisplay_t);
	const char* (*QueryString)(EglDisplay_t, int);
	unsigned (*BindAPI)(unsigned);
	unsigned (*ChooseConfig)(EglDisplay_t, const int*, EglConfig_t*, int, int*);
	EglContext_t (*CreateContext)(EglDisplay_t, EglConfig_t, EglContext_t, const int*);
	unsigned (*DestroyContext)(EglDisplay_t, EglContext_t);
	EglSurface_t (*CreatePbufferSurface)(EglDisplay_t, EglConfig_t, const int*);
	unsigned (*DestroySurface)(EglDisplay_t, EglSurface_t);
	unsigned (*MakeCurrent)(EglDisplay_t, EglSurface_t, EglSurface_t, EglContext_t);
	OsGlProc_t (*GetProcAddress)(const char*);
} egl;

static const char* const kEglNames[] = {"eglGetDisplay", "eglInitialize", "eglTerminate", "eglQueryString", "eglBindAPI", "eglChooseConfig",
	"eglCreateContext", "eglDestroyContext", "eglCreatePbufferSurface", "eglDestroySurface", "eglMakeCurrent", "eglGetProcAddress"};

static void* gLib;
static EglDisplay_t gDpy;

// libEGL and a display that needs no window system; 0 when the system has none
static int EglOpen(void)
{
	void* found[sizeof kEglNames / sizeof kEglNames[0]];
	const char* ext;
	size_t i;
	int major = 0, minor = 0;
	if(sizeof egl != sizeof found || !(gLib = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL)))
		return 0;
	for(i = 0; i < sizeof kEglNames / sizeof kEglNames[0]; i++)
		if(!(found[i] = dlsym(gLib, kEglNames[i])))
			return 0;
	memcpy(&egl, found, sizeof egl);
	// Mesa's display without a surface of the system's, where it has one; else the system's own
	ext = egl.QueryString(NULL, EGL_EXTENSIONS);
	if(ext && strstr(ext, "EGL_MESA_platform_surfaceless"))
	{
		EglDisplay_t (*platform)(unsigned, void*, const void*) = NULL;
		void* sym = dlsym(gLib, "eglGetPlatformDisplay");
		memcpy(&platform, &sym, sizeof platform);
		if(platform)
			gDpy = platform(EGL_PLATFORM_SURFACELESS_MESA, NULL, NULL);
	}
	if(!gDpy)
		gDpy = egl.GetDisplay(NULL);
	return gDpy && egl.Initialize(gDpy, &major, &minor);
}

// ---- a context of one of the two, and what it drew ----------------------------------------------

#define GL_RGBA          0x1908
#define GL_UNSIGNED_BYTE 0x1401

static EglContext_t gCtx;
static EglSurface_t gSurf;
static void (*gReadPixels)(int, int, int, int, unsigned, unsigned, void*);
static void (*gFinish)(void);

static OsGlProc_t Load(const char* name)
{
	return egl.GetProcAddress(name);
}

// a context of OpenGL 3.3 (es 0) or OpenGL ES 3.0 (es 1) on a surface of w x h in memory, current; 0 when EGL makes none
static int ContextOpen(int es, int w, int h)
{
	int want[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, 0, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
		EGL_ALPHA_SIZE, 8, EGL_NONE};
	int size[] = {EGL_WIDTH, 0, EGL_HEIGHT, 0, EGL_NONE};
	static const int kCore[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3, EGL_CONTEXT_OPENGL_PROFILE_MASK,
		EGL_CONTEXT_CORE_PROFILE_BIT, EGL_NONE};
	static const int kEs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
	EglConfig_t config = NULL;
	OsGlProc_t p;
	int n = 0;
	want[3] = es ? EGL_OPENGL_ES3_BIT : EGL_OPENGL_BIT;
	size[1] = w;
	size[3] = h;
	if(!egl.BindAPI(es ? EGL_OPENGL_ES_API : EGL_OPENGL_API) || !egl.ChooseConfig(gDpy, want, &config, 1, &n) || n < 1)
		return 0;
	gCtx = egl.CreateContext(gDpy, config, NULL, es ? kEs : kCore);
	gSurf = gCtx ? egl.CreatePbufferSurface(gDpy, config, size) : NULL;
	if(!gCtx || !gSurf || !egl.MakeCurrent(gDpy, gSurf, gSurf, gCtx))
		return 0;
	p = Load("glReadPixels");
	memcpy(&gReadPixels, &p, sizeof gReadPixels);
	p = Load("glFinish");
	memcpy(&gFinish, &p, sizeof gFinish);
	return gReadPixels && gFinish;
}

static void ContextClose(void)
{
	OsGl_Stop();
	if(gDpy)
		egl.MakeCurrent(gDpy, NULL, NULL, NULL);
	if(gSurf)
		egl.DestroySurface(gDpy, gSurf);
	if(gCtx)
		egl.DestroyContext(gDpy, gCtx);
	gSurf = NULL;
	gCtx = NULL;
}

// ---- the pictures -------------------------------------------------------------------------------

#define PIC_W 96
#define PIC_H 72
#define OUT_W 240 // the surface drawn into
#define OUT_H 160
#define SHOTS 15 // what a run draws, one after the other
#define LAYER 14 // .. the one with the layer last

// a pixel of the test's picture: colours that differ in every channel, an edge, and noise
static uint32_t Pixel(int x, int y)
{
	uint32_t r = (uint32_t)(x * 255 / (PIC_W - 1)), g = (uint32_t)(y * 255 / (PIC_H - 1)), b = (uint32_t)((x * 7 + y * 13) & 255);
	if(x >= 40 && x < 56 && y >= 20 && y < 50)
	{ // a block of one colour with a hard edge: red alone, so that red and blue cannot change places unseen
		r = 240;
		g = 16;
		b = 32;
	}
	if(((x * 31 + y * 17) % 23) == 0)
		b ^= 0x80;
	return r << 16 | g << 8 | b;
}

static uint32_t gPic32[PIC_H][PIC_W + 5]; // (a pitch wider than the rows)
static uint16_t gPic16[PIC_H][PIC_W + 3];

static void MakePictures(void)
{
	int x, y;
	for(y = 0; y < PIC_H; y++)
		for(x = 0; x < PIC_W + 5; x++)
		{
			uint32_t v = x < PIC_W ? Pixel(x, y) : 0x00ff00ffu; // (what lies beyond a row must never be seen)
			gPic32[y][x] = v;
			if(x < PIC_W + 3)
				gPic16[y][x] = (uint16_t)(x < PIC_W ? (v >> 19 & 31) << 10 | (v >> 11 & 31) << 5 | (v >> 3 & 31) : 0x7c1f);
		}
}

static void Surface32(OsSurface_t* s)
{
	s->pixels = gPic32;
	s->pitch = (int)sizeof gPic32[0];
	s->width = PIC_W;
	s->height = PIC_H;
	s->bpp = 32;
}

// what the surface holds, as bytes of red, green, blue, alpha, the top row first; free() it
static uint8_t* Shot(void)
{
	uint8_t* raw = (uint8_t*)malloc((size_t)OUT_W * OUT_H * 4);
	uint8_t* out = (uint8_t*)malloc((size_t)OUT_W * OUT_H * 4);
	int y;
	if(!raw || !out)
	{
		free(raw);
		free(out);
		return NULL;
	}
	gFinish();
	gReadPixels(0, 0, OUT_W, OUT_H, GL_RGBA, GL_UNSIGNED_BYTE, raw);
	for(y = 0; y < OUT_H; y++) // (OpenGL's first row is the bottom one)
		memcpy(out + (size_t)y * OUT_W * 4, raw + (size_t)(OUT_H - 1 - y) * OUT_W * 4, (size_t)OUT_W * 4);
	free(raw);
	return out;
}

static uint32_t At(const uint8_t* shot, int x, int y)
{
	const uint8_t* p = shot + ((size_t)y * OUT_W + (size_t)x) * 4;
	return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
}

// ---- the shader files, as the build packs them (tools/mkpack.py), from the tree --------------------

static unsigned char* gPack;
static unsigned gPackSize;

static void PackAdd(const char* name)
{
	char path[256];
	FILE* f;
	long size;
	unsigned char* grown;
	size_t n = strlen(name);
	snprintf(path, sizeof path, "shaders/anime4k/%s", name);
	f = fopen(path, "rb");
	if(!f)
		return;
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);
	grown = (unsigned char*)realloc(gPack, gPackSize + 1 + n + 9 + (size_t)size);
	if(grown && size > 0)
	{
		unsigned char* at = grown + gPackSize;
		unsigned k;
		gPack = grown;
		*at++ = (unsigned char)n;
		memcpy(at, name, n);
		at += n;
		*at++ = 0; // stored as it is
		for(k = 0; k < 2; k++, at += 4)
		{
			at[0] = (unsigned char)size;
			at[1] = (unsigned char)(size >> 8);
			at[2] = (unsigned char)(size >> 16);
			at[3] = (unsigned char)(size >> 24);
		}
		if(fread(at, 1, (size_t)size, f) == (size_t)size)
		{
			gPackSize += (unsigned)(1 + n + 9 + (size_t)size);
			gPack[8]++;
		}
	}
	fclose(f);
}

// 1 when all fourteen files were found
static int PackMake(void)
{
	static const char* const kFiles[] = {"Anime4K_Clamp_Highlights.glsl", "Anime4K_AutoDownscalePre_x2.glsl", "Anime4K_AutoDownscalePre_x4.glsl",
		"Anime4K_Restore_CNN_S.glsl", "Anime4K_Restore_CNN_M.glsl", "Anime4K_Restore_CNN_VL.glsl", "Anime4K_Restore_CNN_Soft_S.glsl",
		"Anime4K_Restore_CNN_Soft_M.glsl", "Anime4K_Restore_CNN_Soft_VL.glsl", "Anime4K_Upscale_CNN_x2_S.glsl", "Anime4K_Upscale_CNN_x2_M.glsl",
		"Anime4K_Upscale_CNN_x2_VL.glsl", "Anime4K_Upscale_Denoise_CNN_x2_M.glsl", "Anime4K_Upscale_Denoise_CNN_x2_VL.glsl"};
	size_t i;
	gPack = (unsigned char*)calloc(1, 12);
	if(!gPack)
		return 0;
	memcpy(gPack, "OBGIPACK", 8);
	gPackSize = 12;
	for(i = 0; i < sizeof kFiles / sizeof kFiles[0]; i++)
		PackAdd(kFiles[i]);
	return gPack[8] == sizeof kFiles / sizeof kFiles[0];
}

// ---- a run --------------------------------------------------------------------------------------

static const char* const kShotNames[SHOTS] = {"nearest x2", "bilinear larger", "bicubic larger", "bilinear smaller", "bicubic smaller",
	"written in parts", "15 bits", "Anime4K A", "Anime4K B", "Anime4K C", "Anime4K A+A", "Anime4K B+B", "Anime4K C+A", "Anime4K A, high", "layer"};

// ---- the layer ----------------------------------------------------------------------------------

#define LAYER_PITCH ((OUT_W + 3) * 4)          // (a pitch wider than the rows)
static uint8_t gLayerPix[OUT_H * LAYER_PITCH]; // blue, green, red, alpha
#define OPAQUE_X 10                            // an opaque block, over the border and the picture's corner,
#define OPAQUE_Y 4
#define OPAQUE_W 40
#define OPAQUE_H 30
#define HALF_X   150 // .. and a half clear white one inside the picture
#define HALF_Y   100
#define HALF_W   50
#define HALF_H   40

static void LayerBlock(int x0, int y0, int w, int h, int b, int g, int r, int a)
{
	int x, y;
	for(y = y0; y < y0 + h; y++)
		for(x = x0; x < x0 + w; x++)
		{
			uint8_t* p = gLayerPix + (size_t)y * LAYER_PITCH + (size_t)x * 4;
			p[0] = (uint8_t)b;
			p[1] = (uint8_t)g;
			p[2] = (uint8_t)r;
			p[3] = (uint8_t)a;
		}
}

// what the picture drawn twice as large at (24, 8) shows at a place of the surface, without a layer
static uint32_t Under(int x, int y)
{
	return x >= 24 && x < 24 + PIC_W * 2 && y >= 8 && y < 8 + PIC_H * 2 ? Pixel((x - 24) / 2, (y - 8) / 2) : 0;
}

static int Near(uint32_t a, uint32_t b)
{
	int k;
	for(k = 0; k < 24; k += 8)
	{
		int d = (int)(a >> k & 255) - (int)(b >> k & 255);
		if(d < -1 || d > 1)
			return 0;
	}
	return 1;
}

static void RunLayer(uint8_t** shot)
{
	OsSurface_t s;
	uint8_t* now;
	int x, y, wrong = 0;
	Surface32(&s);
	OsGl_Blit(&s, 0, 0, PIC_W, PIC_H, 0, 0, PIC_W, PIC_H);
	// clear everywhere, in a colour that must never be seen; the two blocks
	LayerBlock(0, 0, OUT_W + 3, OUT_H, 255, 0, 255, 0);
	LayerBlock(OPAQUE_X, OPAQUE_Y, OPAQUE_W, OPAQUE_H, 0x20, 0x40, 0xe0, 255);
	LayerBlock(HALF_X, HALF_Y, HALF_W, HALF_H, 255, 255, 255, 128);
	OsGl_Layer(gLayerPix, OUT_W, OUT_H, LAYER_PITCH, 0, 0, OUT_W, OUT_H);
	CHECK(OsGl_Draw(OUT_W, OUT_H, 24, 8, PIC_W * 2, PIC_H * 2, OPENBGI_SCALE_NEAREST));
	shot[LAYER] = now = Shot();
	for(y = 0; now && y < OUT_H; y++)
		for(x = 0; x < OUT_W; x++)
		{
			uint32_t under = Under(x, y), want = under, got = At(now, x, y);
			if(x >= OPAQUE_X && x < OPAQUE_X + OPAQUE_W && y >= OPAQUE_Y && y < OPAQUE_Y + OPAQUE_H)
				want = 0xe04020;
			else if(x >= HALF_X && x < HALF_X + HALF_W && y >= HALF_Y && y < HALF_Y + HALF_H)
			{ // white at 128 of 255 over the picture
				int k;
				want = 0;
				for(k = 0; k < 24; k += 8)
					want |= (uint32_t)((255 * 128 + (int)(under >> k & 255) * 127 + 127) / 255) << k;
			}
			if(!Near(got, want))
				wrong++;
		}
	if(wrong)
		printf("  %d pixels under the layer are not what they should be\n", wrong);
	CHECK(now && wrong == 0);
	// the opaque block's left half cleared, and only that part written anew
	LayerBlock(OPAQUE_X, OPAQUE_Y, OPAQUE_W / 2, OPAQUE_H, 0, 255, 0, 0);
	LayerBlock(HALF_X, HALF_Y, HALF_W, HALF_H, 0, 0, 0, 0); // (not handed over: still seen)
	OsGl_Layer(gLayerPix, OUT_W, OUT_H, LAYER_PITCH, OPAQUE_X, OPAQUE_Y, OPAQUE_W / 2, OPAQUE_H);
	CHECK(OsGl_Draw(OUT_W, OUT_H, 24, 8, PIC_W * 2, PIC_H * 2, OPENBGI_SCALE_NEAREST));
	now = Shot();
	if(now)
	{
		CHECK(At(now, OPAQUE_X + 5, OPAQUE_Y + 10) == Under(OPAQUE_X + 5, OPAQUE_Y + 10));
		CHECK(At(now, OPAQUE_X + OPAQUE_W / 2 - 1, OPAQUE_Y + OPAQUE_H - 1) == Under(OPAQUE_X + OPAQUE_W / 2 - 1, OPAQUE_Y + OPAQUE_H - 1));
		CHECK(At(now, OPAQUE_X + OPAQUE_W / 2, OPAQUE_Y + 10) == 0xe04020);
		CHECK(At(now, HALF_X + 10, HALF_Y + 10) != Under(HALF_X + 10, HALF_Y + 10));
	}
	free(now);
	// a target of another size than the layer: the picture alone
	CHECK(OsGl_Draw(OUT_W, OUT_H - 20, 24, 8, PIC_W * 2, PIC_H * 2, OPENBGI_SCALE_NEAREST));
	CHECK(OsGl_Draw(OUT_W, OUT_H, 24, 8, PIC_W * 2, PIC_H * 2, OPENBGI_SCALE_NEAREST)); // (and with it again)
	now = Shot();
	CHECK(now && At(now, OPAQUE_X + OPAQUE_W / 2, OPAQUE_Y + 10) == 0xe04020);
	free(now);
	// a layer of another size: all of it written, and not drawn on this target
	OsGl_Layer(gLayerPix, OUT_W - 8, OUT_H, LAYER_PITCH, 0, 0, 1, 1);
	CHECK(OsGl_Draw(OUT_W, OUT_H, 24, 8, PIC_W * 2, PIC_H * 2, OPENBGI_SCALE_NEAREST));
	now = Shot();
	CHECK(now && At(now, OPAQUE_X + OPAQUE_W / 2, OPAQUE_Y + 10) == Under(OPAQUE_X + OPAQUE_W / 2, OPAQUE_Y + 10));
	free(now);
	// the layer over black alone (no place for the picture)
	OsGl_Layer(gLayerPix, OUT_W, OUT_H, LAYER_PITCH, 0, 0, OUT_W, OUT_H);
	CHECK(OsGl_Draw(OUT_W, OUT_H, 0, 0, 0, 0, OPENBGI_SCALE_NEAREST));
	now = Shot();
	CHECK(now && At(now, OPAQUE_X + OPAQUE_W / 2, OPAQUE_Y + 10) == 0xe04020 && At(now, 120, 80) == 0);
	free(now);
	// no layer any more
	OsGl_Layer(NULL, 0, 0, 0, 0, 0, 0, 0);
	CHECK(OsGl_Draw(OUT_W, OUT_H, 24, 8, PIC_W * 2, PIC_H * 2, OPENBGI_SCALE_NEAREST));
	now = Shot();
	CHECK(now && At(now, OPAQUE_X + OPAQUE_W / 2, OPAQUE_Y + 10) == Under(OPAQUE_X + OPAQUE_W / 2, OPAQUE_Y + 10));
	free(now);
}

/* Everything the test draws, through the context that is current; the
 * pictures into shot[] (NULL for one that was not drawn).  `chains`: the
 * shader files are there. */
static void Run(uint8_t** shot, int chains, int es)
{
	OsSurface_t s;
	int n = 0, x, y, i;
	memset(shot, 0, SHOTS * sizeof *shot);
	CHECK(OsGl_Start(Load));
	if(!OsGl_On())
		return;
	CHECK((OsGl_FilterName(0) != NULL) == (chains != 0));
	CHECK(OsGl_FilterName(6) == NULL);

	// nearest, twice as large, at (24, 8): each pixel of the picture is four of the surface
	Surface32(&s);
	OsGl_Blit(&s, 0, 0, PIC_W, PIC_H, 0, 0, PIC_W, PIC_H);
	CHECK(OsGl_Draw(OUT_W, OUT_H, 24, 8, PIC_W * 2, PIC_H * 2, OPENBGI_SCALE_NEAREST));
	shot[n] = Shot();
	if(shot[n])
	{
		int wrong = 0, border = 0;
		for(y = 0; y < OUT_H; y++)
			for(x = 0; x < OUT_W; x++)
			{
				int in = x >= 24 && x < 24 + PIC_W * 2 && y >= 8 && y < 8 + PIC_H * 2;
				uint32_t want = in ? Pixel((x - 24) / 2, (y - 8) / 2) : 0;
				if(At(shot[n], x, y) != want)
				{
					if(in)
						wrong++;
					else
						border++;
				}
			}
		if(wrong || border)
			printf("  (%s) %d pixels of the picture and %d of the border are not what they should be; the pixel at its corner is %06x, not %06x\n",
				es ? "OpenGL ES" : "OpenGL", wrong, border, (unsigned)At(shot[n], 24, 8), (unsigned)Pixel(0, 0));
		CHECK(wrong == 0);
		CHECK(border == 0);
	}
	n++;

	// the rules, larger (a size that is no whole number of times the picture's) and smaller
	CHECK(OsGl_Draw(OUT_W, OUT_H, 10, 5, 213, 150, OPENBGI_SCALE_BILINEAR));
	shot[n++] = Shot();
	CHECK(OsGl_Draw(OUT_W, OUT_H, 10, 5, 213, 150, OPENBGI_SCALE_BICUBIC));
	shot[n++] = Shot();
	CHECK(OsGl_Draw(OUT_W, OUT_H, 100, 60, 40, 30, OPENBGI_SCALE_BILINEAR));
	shot[n++] = Shot();
	CHECK(OsGl_Draw(OUT_W, OUT_H, 100, 60, 40, 30, OPENBGI_SCALE_BICUBIC));
	shot[n++] = Shot();

	/* in parts: a picture of another size (it starts black), its right half
	 * from the picture's left one, then a block from inside the surface */
	OsGl_Blit(&s, 60, 0, 60, PIC_H, 0, 0, 120, PIC_H);
	OsGl_Blit(&s, 4, 10, 30, 40, 40, 20, 120, PIC_H);
	OsGl_Blit(&s, -10, -10, 20, 20, 0, 0, 120, PIC_H); // (cut at the picture's corner: the ten pixels inside it)
	CHECK(OsGl_Draw(OUT_W, OUT_H, 0, 0, 240, 144, OPENBGI_SCALE_NEAREST));
	shot[n] = Shot();
	if(shot[n])
	{
		CHECK(At(shot[n], 121, 1) == Pixel(0, 0)); // the right half begins with the picture's first column
		CHECK(At(shot[n], 239, 143) == Pixel(59, PIC_H - 1));
		CHECK(At(shot[n], 9, 21) == Pixel(40, 20)); // the block
		CHECK(At(shot[n], 67, 99) == Pixel(69, 59));
		CHECK(At(shot[n], 1, 1) == Pixel(10, 10)); // the corner
		CHECK(At(shot[n], 80, 60) == 0);           // never written: black
	}
	n++;

	// 15 bits: each colour's five bits spread over eight
	s.pixels = gPic16;
	s.pitch = (int)sizeof gPic16[0];
	s.bpp = 16;
	OsGl_Blit(&s, 0, 0, PIC_W, PIC_H, 0, 0, PIC_W, PIC_H);
	CHECK(OsGl_Draw(OUT_W, OUT_H, 24, 8, PIC_W * 2, PIC_H * 2, OPENBGI_SCALE_NEAREST));
	shot[n] = Shot();
	if(shot[n])
	{
		uint32_t v = Pixel(45, 30), got = At(shot[n], 24 + 90, 8 + 60);
		uint32_t r = v >> 19 & 31, g = v >> 11 & 31, b = v >> 3 & 31;
		CHECK(got == ((r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2)));
		CHECK(At(shot[n], 23, 8) == 0 && At(shot[n], 24 + PIC_W * 2, 8) == 0);
	}
	n++;

	// the chains: each mode's smaller networks, then the first one's larger
	Surface32(&s);
	OsGl_Blit(&s, 0, 0, PIC_W, PIC_H, 0, 0, PIC_W, PIC_H);
	for(i = 0; chains && i < 7; i++, n++)
	{
		int rule = i < 6 ? OPENBGI_SCALE_OWN + i : (OPENBGI_SCALE_OWN | OPENBGI_SCALE_HIGH);
		CHECK(OsGl_Draw(OUT_W, OUT_H, 24, 8, PIC_W * 2, PIC_H * 2, rule));
		shot[n] = Shot();
	}
	if(chains)
	{ // a chain that ran is no bicubic picture: what it made differs from that
		OsGl_Draw(OUT_W, OUT_H, 24, 8, PIC_W * 2, PIC_H * 2, OPENBGI_SCALE_BICUBIC);
		{
			uint8_t* cubic = Shot();
			CHECK(cubic && shot[7] && memcmp(cubic, shot[7], (size_t)OUT_W * OUT_H * 4) != 0);
			free(cubic);
		}
	}
	RunLayer(shot);
}

// the largest difference of a colour between two pictures, and how many pixels differ by more than `allow`
static int Differ(const uint8_t* a, const uint8_t* b, int allow, int* count)
{
	int worst = 0, i, k;
	*count = 0;
	for(i = 0; i < OUT_W * OUT_H; i++)
	{
		int d = 0;
		for(k = 0; k < 3; k++)
		{
			int e = abs((int)a[i * 4 + k] - (int)b[i * 4 + k]);
			if(e > d)
				d = e;
		}
		if(d > worst)
			worst = d;
		if(d > allow)
			(*count)++;
	}
	return worst;
}

int main(void)
{
	uint8_t *gl[SHOTS], *es[SHOTS];
	int haveGl, haveEs, chains, i;
	memset(gl, 0, sizeof gl);
	memset(es, 0, sizeof es);
	setenv("BGI_GL", "force", 1); // (the context here is one that draws on the processor, as likely as not)
	if(!EglOpen())
	{
		printf("gl: no EGL here: not tested\n");
		return 0;
	}
	MakePictures();
	chains = PackMake();
	if(chains)
		OS_UseShaders(gPack, gPackSize);
	else
		printf("gl: the shader files are not here (shaders/anime4k/): the chains are not tested\n");
	haveGl = ContextOpen(0, OUT_W, OUT_H);
	if(haveGl)
		Run(gl, chains, 0);
	ContextClose();
	haveEs = ContextOpen(1, OUT_W, OUT_H);
	if(haveEs)
		Run(es, chains, 1);
	ContextClose();
	if(!haveGl || !haveEs)
		printf("gl: EGL makes no context of %s here: %s\n", haveGl ? "OpenGL ES 3.0" : haveEs ? "OpenGL 3.3"
																							  : "OpenGL 3.3 or OpenGL ES 3.0",
			haveGl || haveEs ? "the two are not compared" : "not tested");
	/* the two against each other: the same numbers either way but for the
	 * last bit of a rounding (and, in a chain, for what dozens of passes
	 * make of that) */
	for(i = 0; haveGl && haveEs && i < SHOTS; i++)
	{
		int allow = i < 7 || i == LAYER ? 1 : 3, count = 0, worst;
		if(!gl[i] && !es[i] && i >= 7 && i != LAYER && !chains)
			continue;
		CHECK(gl[i] && es[i]);
		if(!gl[i] || !es[i])
			continue;
		worst = Differ(gl[i], es[i], allow, &count);
		if(count)
			printf("  %s: %d pixels differ by more than %d between OpenGL and OpenGL ES (by %d at most)\n", kShotNames[i], count, allow, worst);
		CHECK(count == 0);
	}
	for(i = 0; i < SHOTS; i++)
	{
		free(gl[i]);
		free(es[i]);
	}
	free(gPack);
	if(gFails)
	{
		printf("gl: %d checks failed\n", gFails);
		return 1;
	}
	printf("gl: all tests passed\n");
	return 0;
}

#else

int main(void)
{
	(void)gFails;
	printf("gl: this build shows no picture through OpenGL: not tested\n");
	return 0;
}

#endif
