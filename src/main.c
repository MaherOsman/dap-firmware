// src/main.c
#include <stdio.h>
#include <SDL2/SDL.h>
#include "hal/sdl2/hal_sdl2.h"

int main(void) {
    if (hal_init() != 0) {
        fprintf(stderr, "Failed to initialize HAL\n");
        return 1;
    }

    printf("DAP Simulator running at %dx%d\n", DAP_SCREEN_WIDTH, DAP_SCREEN_HEIGHT);

    int running = 1;
    SDL_Event event;

    while (running) {
        // Handle input
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) running = 0;
            if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_q) running = 0;
        }

        // Draw — black screen for now
        hal_clear_screen(0, 0, 0);
        hal_present();

        SDL_Delay(16); // ~60fps
    }

    hal_quit();
    return 0;
}