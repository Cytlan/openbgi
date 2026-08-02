#include <SDL2/SDL.h>
#include "os.h"
#include "version.h"
#include "engine.h"

Engine_t* osEngine = NULL;

int OS_Init(Engine_t* engine)
{
	osEngine = engine;

    SDL_SetHint(SDL_HINT_SHUTDOWN_DBUS_ON_QUIT, "1");
	if(SDL_Init(SDL_INIT_VIDEO) < 0)
	{
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow(
        VERSION_STRING,             // Window title
        SDL_WINDOWPOS_CENTERED,     // Initial x position
        SDL_WINDOWPOS_CENTERED,     // Initial y position
        800,                        // Width in pixels
        600,                        // Height in pixels
        SDL_WINDOW_SHOWN            // Flags (make it visible)
    );

    if(window == NULL)
    {
        SDL_Quit();
        return 1;
    }
    engine->window = window;
    return 0;
}

int OS_Poll()
{
	SDL_Event event;
	while(SDL_PollEvent(&event))
	{
        if(event.type == SDL_QUIT)
        {
        	osEngine->isRunning = 0;
        }
    }
}

int OS_Quit()
{
	SDL_DestroyWindow(osEngine->window);
    SDL_Quit();
    return 0;
}
