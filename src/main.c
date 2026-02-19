#include <stdio.h>
#include <SDL2/SDL.h>
#include "hal/sdl2/hal_sdl2.h"
#include "ui/font.h"

int main(void) {
    if (hal_init() != 0) return 1;
    if (font_init("assets/fonts/Roboto-Regular.ttf") != 0) return 1;

    printf("DAP Simulator running at %dx%d\n", DAP_SCREEN_WIDTH, DAP_SCREEN_HEIGHT);

    int running = 1;
    SDL_Event event;

    while (running) {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) running = 0;
            if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_q) running = 0;
        }

        hal_clear_screen(0, 0, 0);

        // Test text rendering
        font_draw(FONT_LARGE,  "Now Playing",     10, 10,  COLOR_WHITE);
        font_draw(FONT_MEDIUM, "Paranoid Android", 10, 40,  COLOR_WHITE);
        font_draw(FONT_SMALL,  "Radiohead",        10, 65,  COLOR_GRAY);
        font_draw_centered(FONT_SMALL, "320 x 240 — DAP Simulator", 220, COLOR_GRAY);

        hal_present();
        SDL_Delay(16);
    }

    font_quit();
    hal_quit();
    return 0;
}