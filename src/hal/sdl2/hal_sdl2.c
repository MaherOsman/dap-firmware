// src/hal/sdl2/hal_sdl2.c
// SDL2 hardware abstraction layer — PC simulator backend

#include "hal_sdl2.h"
#include <SDL2/SDL.h>
#include <stdio.h>

static SDL_Window   *window   = NULL;
static SDL_Renderer *renderer = NULL;

int hal_init(void) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init error: %s\n", SDL_GetError());
        return -1;
    }

    window = SDL_CreateWindow(
        "DAP Simulator",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        DAP_SCREEN_WIDTH,
        DAP_SCREEN_HEIGHT,
        SDL_WINDOW_SHOWN
    );

    if (!window) {
        fprintf(stderr, "SDL_CreateWindow error: %s\n", SDL_GetError());
        return -1;
    }

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) {
        fprintf(stderr, "SDL_CreateRenderer error: %s\n", SDL_GetError());
        return -1;
    }

    return 0;
}

void hal_clear_screen(uint8_t r, uint8_t g, uint8_t b) {
    SDL_SetRenderDrawColor(renderer, r, g, b, 255);
    SDL_RenderClear(renderer);
}

void hal_present(void) {
    SDL_RenderPresent(renderer);
}

void hal_quit(void) {
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window)   SDL_DestroyWindow(window);
    SDL_Quit();
}

SDL_Renderer *hal_get_renderer(void) {
    return renderer;
}