#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include "renderer.h"
#include "engine.h"
#include "spng.h"

Renderer_t* Renderer_Init(Engine_t* engine)
{
	Renderer_t* renderer = (Renderer_t*)malloc(sizeof(Renderer_t));
	if(renderer == NULL)
		return NULL;
	renderer->engine = engine;
	for(int i = 0; i < RENDERER_MAX_BITMAPS; i++)
		renderer->bitmaps[i] = NULL;
	for(int i = 0; i < RENDERER_MAX_SCREENS; i++)
		renderer->screens[i] = NULL;
	renderer->activeScreen = 0;
	renderer->allocatedScreens = 0;
	return renderer;
}

uint32_t Renderer_CreateScreen(Renderer_t* renderer, int width, int height)
{
	Screen_t* screen = (Screen_t*)malloc(sizeof(Screen_t));
    if(!screen)
    	return 0;
    uint8_t* bitmap = (uint8_t*)malloc(width * height * 4);
    if(!bitmap)
    {
    	free(screen);
    	return 0;
    }
	SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormatFrom(
	    bitmap,
	    width,
	    height,
	    32,
	    width * 4,
	    SDL_PIXELFORMAT_RGBA32
	);
    screen->width = width;
    screen->height = height;
    screen->x = 0;
    screen->y = 0;
    screen->bitmap = bitmap;
    screen->surface = surface;
    renderer->screens[renderer->allocatedScreens] = screen;
    uint32_t id = 0xC0000000 + renderer->allocatedScreens;
    renderer->activeScreen = renderer->allocatedScreens;
    renderer->allocatedScreens++;
    printf("[Renderer]: Created screen object (0x%08X) width size %dx%d\n", id, width, height);
    return id;
}

void Renderer_DestroyScreen(Renderer_t* renderer, uint32_t handle)
{
	uint32_t id = 0x0000001F & handle;
	if(renderer->screens[id] == NULL)
		return;
	if(renderer->screens[id]->surface != NULL)
		SDL_FreeSurface(renderer->screens[id]->surface);
	if(renderer->screens[id]->bitmap != NULL)
		free(renderer->screens[id]->bitmap);
	free(renderer->screens[id]);
	renderer->screens[id] = NULL;
	renderer->allocatedScreens--;
}

void Renderer_DrawBitmapToScreen(Renderer_t* renderer, uint32_t bitmapId, int screenId)
{
	printf("[Renderer]: Start draw\n");
	Screen_t* screen = renderer->screens[screenId];
	Bitmap_t* bitmap = renderer->bitmaps[bitmapId];
	if(!screen)
	{
		printf("[Renderer]: Warning: Attempting to draw to invalid screen object (%d)\n", screenId);
		return;
	}
	if(!bitmap)
	{
		printf("[Renderer]: Warning: Attempting to draw invalid bitmap (%d)\n", bitmapId);
		return;
	}
	int x = 0;
	int y = 0;
	for(int y = 0; y < bitmap->height; y++)
	{
		if(y >= screen->height)
			break;
		for(int x = 0; x < bitmap->width; x++)
		{
			if(x >= screen->width)
				break;
			screen->bitmap[(y * screen->width * 4) + (x * 4) + 0] = bitmap->bitmap[(y * bitmap->width * 4) + (x * 4) + 0];
			screen->bitmap[(y * screen->width * 4) + (x * 4) + 1] = bitmap->bitmap[(y * bitmap->width * 4) + (x * 4) + 1];
			screen->bitmap[(y * screen->width * 4) + (x * 4) + 2] = bitmap->bitmap[(y * bitmap->width * 4) + (x * 4) + 2];
			screen->bitmap[(y * screen->width * 4) + (x * 4) + 3] = bitmap->bitmap[(y * bitmap->width * 4) + (x * 4) + 3];
		}
	}
	printf("[Renderer]: End draw\n");
}

Bitmap_t* Renderer_ParsePng(uint8_t* file, size_t fileSize)
{
    spng_ctx *ctx = spng_ctx_new(0);
    if(!ctx)
    {
        printf("[Renderer]: Failed to create libspng context\n");
        return NULL;
    }

    int ret = spng_set_png_buffer(ctx, file, fileSize);
    if(ret)
    {
        printf("[Renderer]: Error setting PNG file: %s\n", spng_strerror(ret));
        spng_ctx_free(ctx);
        return NULL;
    }

    // Determine output buffer size
    size_t out_size;
    ret = spng_decoded_image_size(ctx, SPNG_FMT_RGBA8, &out_size);
    if(ret)
    {
        printf("Error calculating image size: %s\n", spng_strerror(ret));
        spng_ctx_free(ctx);
        return NULL;
    }

    unsigned char *out_buffer = malloc(out_size);
    if(!out_buffer)
    {
        printf("[Renderer]: Error: Memory allocation failed\n");
        spng_ctx_free(ctx);
        return NULL;
    }

    ret = spng_decode_image(ctx, out_buffer, out_size, SPNG_FMT_RGBA8, 0);
    if(ret)
    {
        printf("[Renderer]: Error decoding image: %s\n", spng_strerror(ret));
        free(out_buffer);
        spng_ctx_free(ctx);
        return NULL;
    }

    struct spng_ihdr ihdr;
    ret = spng_get_ihdr(ctx, &ihdr);
    if(ret)
    {
        printf("[Renderer]: Error getting image IHDR: %s\n", spng_strerror(ret));
        free(out_buffer);
        spng_ctx_free(ctx);
        return NULL;
    }

    printf("[Renderer]: Successfully decoded!\n");

    Bitmap_t* bitmap = (Bitmap_t*)malloc(sizeof(Bitmap_t));
    if(!bitmap)
    {
    	free(out_buffer);
    	return NULL;
    }
    bitmap->width = ihdr.width;
    bitmap->height = ihdr.height;
    bitmap->bitmap = out_buffer;

    spng_ctx_free(ctx);
    return bitmap;
}

void Renderer_LoadBitmap(Renderer_t* renderer, int slot, const char* filename, const char* archive)
{
	size_t fileSize;
	uint8_t* file = Engine_ReadFile(renderer->engine, archive, filename, &fileSize);
	if(file == NULL)
		return;

	Bitmap_t* bitmap = NULL;
	if(file[0] == 0x89 && file[1] == 'P' && file[2] == 'N' && file[3] == 'G')
	{
		// Parse PNG
		printf("[Renderer]: Parsing PNG\n");
		bitmap = Renderer_ParsePng(file, fileSize);
	}

	if(bitmap != NULL)
	{
		if(renderer->bitmaps[slot] != NULL)
		{
			printf("[Renderer]: Warning: Overwriting slot 0x%04X!\n", slot);
			if(renderer->bitmaps[slot]->bitmap != NULL)
				free(renderer->bitmaps[slot]->bitmap);
			renderer->bitmaps[slot]->bitmap = NULL;
			free(renderer->bitmaps[slot]);
		}
		renderer->bitmaps[slot] = bitmap;

		printf("[Renderer]: Added bitmap to slot 0x%04X (%dx%d)\n", slot, bitmap->width, bitmap->height);
	}

	free(file);
}

void Renderer_DrawScreen(Renderer_t* renderer)
{
	if(renderer->engine->window == NULL)
		return;
	//if(renderer->screens[renderer->activeScreen] == NULL)
	//	return;
	SDL_Surface* windowSurface = SDL_GetWindowSurface(renderer->engine->window);
	SDL_Rect* sourceRect = NULL;
	SDL_Rect destRect;
	destRect.w = 0;
	destRect.h = 0;
	uint32_t colour = SDL_MapRGB(windowSurface->format, 0, 0, 0);
	SDL_FillRect(windowSurface, NULL, colour);
	for(int i = 0; i < renderer->allocatedScreens; i++)
	{
		Screen_t* screen = renderer->screens[i];
		destRect.x = screen->x;
		destRect.y = screen->y;
		SDL_BlitSurface(screen->surface, sourceRect, windowSurface, &destRect);
	}
	SDL_UpdateWindowSurface(renderer->engine->window);
}

void Renderer_SetScreenParams(Renderer_t* renderer, uint32_t handle, int x, int y)
{
	uint32_t id = 0x0000001F & handle;
	if(renderer->screens[id] == NULL)
	{
		printf("[Renderer]: Warning: Attempting to set params on invalid screen object (%d)\n", id);
		return;
	}
	Screen_t* screen = renderer->screens[id];
	screen->x = x;
	screen->y = y;
}

void Renderer_Free(Renderer_t* renderer)
{
	if(renderer == NULL)
		return;
	for(int i = 0; i < RENDERER_MAX_BITMAPS; i++)
	{
		if(renderer->bitmaps[i] == NULL)
			continue;

		if(renderer->bitmaps[i]->bitmap != NULL)
			free(renderer->bitmaps[i]->bitmap);
		renderer->bitmaps[i]->bitmap = NULL;
		free(renderer->bitmaps[i]);
		renderer->bitmaps[i] = NULL;
	}
	for(int i = 0; i < RENDERER_MAX_SCREENS; i++)
	{
		if(renderer->screens[i] == NULL)
			continue;
		SDL_FreeSurface(renderer->screens[i]->surface);
		free(renderer->screens[i]->bitmap);
		free(renderer->screens[i]);
		renderer->screens[i] = NULL;
	}
	free(renderer);
}
