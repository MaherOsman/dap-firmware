#include "font.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include "../hal/sdl2/hal_sdl2.h"
#include <stdio.h>

static TTF_Font *fonts[FONT_COUNT] = {NULL};
static const int font_sizes[FONT_COUNT] = {12, 16, 22};

int font_init(const char *font_path) {
    if (TTF_Init() != 0) {
        fprintf(stderr, "TTF_Init error: %s\n", TTF_GetError());
        return -1;
    }

    for (int i = 0; i < FONT_COUNT; i++) {
        fonts[i] = TTF_OpenFont(font_path, font_sizes[i]);
        if (!fonts[i]) {
            fprintf(stderr, "Failed to load font size %d: %s\n", font_sizes[i], TTF_GetError());
            return -1;
        }
    }

    return 0;
}

void font_quit(void) {
    for (int i = 0; i < FONT_COUNT; i++) {
        if (fonts[i]) TTF_CloseFont(fonts[i]);
    }
    TTF_Quit();
}

void font_draw(FontSize size, const char *text, int x, int y, Color color) {
    SDL_Color sdl_color = {color.r, color.g, color.b, color.a};
    SDL_Surface *surface = TTF_RenderText_Blended(fonts[size], text, sdl_color);
    if (!surface) return;

    SDL_Renderer *renderer = hal_get_renderer();
    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_FreeSurface(surface);
    if (!texture) return;

    SDL_Rect dst = {x, y, 0, 0};
    SDL_QueryTexture(texture, NULL, NULL, &dst.w, &dst.h);
    SDL_RenderCopy(renderer, texture, NULL, &dst);
    SDL_DestroyTexture(texture);
}

void font_draw_centered(FontSize size, const char *text, int y, Color color) {
    int w, h;
    font_measure(size, text, &w, &h);
    font_draw(size, text, (DAP_SCREEN_WIDTH - w) / 2, y, color);
}

void font_measure(FontSize size, const char *text, int *w, int *h) {
    TTF_SizeText(fonts[size], text, w, h);
}