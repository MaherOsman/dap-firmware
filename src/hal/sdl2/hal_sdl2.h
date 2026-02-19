// src/hal/sdl2/hal_sdl2.h
#pragma once

#include <stdint.h>

#ifndef DAP_SCREEN_WIDTH
#define DAP_SCREEN_WIDTH  320
#endif
#ifndef DAP_SCREEN_HEIGHT
#define DAP_SCREEN_HEIGHT 240
#endif

int  hal_init(void);
void hal_clear_screen(uint8_t r, uint8_t g, uint8_t b);
void hal_present(void);
void hal_quit(void);

// SDL2-specific — only use in simulator code
struct SDL_Renderer;
struct SDL_Renderer *hal_get_renderer(void);