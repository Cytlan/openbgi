#ifndef _RENDERER_H_
#define _RENDERER_H_

#include <stdint.h>
#include <SDL2/SDL.h>

#define RENDERER_MAX_BITMAPS 0x1000
#define RENDERER_MAX_SCREENS 0x10

typedef struct Screen
{
	int width;
	int height;
	int x;
	int y;
	uint8_t* bitmap;
	SDL_Surface* surface;
} Screen_t;

typedef struct Bitmap
{
	int width;
	int height;
	uint8_t* bitmap;
} Bitmap_t;

typedef struct Engine Engine_t;
typedef struct Renderer
{
	Engine_t* engine;
	Bitmap_t* bitmaps[RENDERER_MAX_BITMAPS];
	Screen_t* screens[RENDERER_MAX_SCREENS];
	int activeScreen;
	int allocatedScreens;
} Renderer_t;

Renderer_t* Renderer_Init(Engine_t* engine);
void Renderer_LoadBitmap(Renderer_t* renderer, int slot, const char* filename, const char* archive);
uint32_t Renderer_CreateScreen(Renderer_t* renderer, int width, int height);
void Renderer_DestroyScreen(Renderer_t* renderer, uint32_t handle);
void Renderer_DrawBitmapToScreen(Renderer_t* renderer, uint32_t bitmapId, int screenId);
void Renderer_DrawScreen(Renderer_t* renderer);
void Renderer_SetScreenParams(Renderer_t* renderer, uint32_t handle, int x, int y);
void Renderer_Free(Renderer_t* renderer);

#endif