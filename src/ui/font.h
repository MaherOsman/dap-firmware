#pragma once
#include <stdint.h>

typedef enum {
    FONT_SMALL  = 0,  // 12px — metadata, labels
    FONT_MEDIUM = 1,  // 16px — list items
    FONT_LARGE  = 2,  // 22px — titles, now playing
    FONT_COUNT  = 3
} FontSize;

typedef struct {
    uint8_t r, g, b, a;
} Color;

// Common colors
#define COLOR_WHITE   (Color){255, 255, 255, 255}
#define COLOR_BLACK   (Color){0,   0,   0,   255}
#define COLOR_GRAY    (Color){150, 150, 150, 255}
#define COLOR_ACCENT  (Color){255, 255, 255, 255}

int  font_init(const char *font_path);
void font_quit(void);
void font_draw(FontSize size, const char *text, int x, int y, Color color);
void font_draw_centered(FontSize size, const char *text, int y, Color color);
void font_measure(FontSize size, const char *text, int *w, int *h);