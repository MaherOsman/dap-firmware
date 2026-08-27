/*
 * preview — render real screens at true 240x240 and write them out as PPM.
 *
 * This is the payoff of drawing into a framebuffer instead of straight to
 * SPI: the pixels produced here are bit-for-bit the pixels the ST7789 will
 * receive. If it looks right in the preview, it looks right on the panel.
 *
 *   make preview      builds and runs this, writing PPMs to build/preview/
 */
#include <stdio.h>
#include <string.h>

#include "../src/core/gfx.h"
#include "../src/core/theme.h"
#include "../src/ui/screen_library.h"

static uint16_t fb[SCREEN_W * SCREEN_H];
static gfx_t g;

static void write_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "cannot write %s\n", path);
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", SCREEN_W, SCREEN_H);
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
        uint16_t c = fb[i];
        /* Expand RGB565 back to 8-bit, replicating high bits into the low
         * ones so pure white stays pure white. */
        uint8_t r = (uint8_t)(((c >> 11) & 0x1F) * 255 / 31);
        uint8_t gg = (uint8_t)(((c >> 5) & 0x3F) * 255 / 63);
        uint8_t b = (uint8_t)((c & 0x1F) * 255 / 31);
        fputc(r, f);
        fputc(gg, f);
        fputc(b, f);
    }
    fclose(f);
    printf("  wrote %s\n", path);
}

/* A believable classical library, since that's what the simulator was
 * pointed at. Long names on purpose — truncation is the common case. */
static const char *ARTISTS[] = {
    "Claude Debussy", "Frederic Chopin", "Erik Satie",
    "Maurice Ravel", "Johann Sebastian Bach", "Sergei Rachmaninoff",
    "Ludwig van Beethoven", "Franz Liszt", "Robert Schumann",
    "Pyotr Ilyich Tchaikovsky", "Antonin Dvorak", "Edvard Grieg",
    "Gustav Mahler", "Jean Sibelius", "Bela Bartok",
};
#define N_ARTISTS ((int)(sizeof(ARTISTS) / sizeof(ARTISTS[0])))

static const char *TRACKS[] = {
    "Clair de Lune",
    "Reverie",
    "Arabesque No. 1 in E major",
    "Prelude a l'apres-midi d'un faune",
    "Suite Bergamasque - IV. Passepied",
    "La Fille aux Cheveux de Lin",
};
#define N_TRACKS ((int)(sizeof(TRACKS) / sizeof(TRACKS[0])))

static void render_artist_level(const theme_t *t, const char *out)
{
    lib_row_t rows[N_ARTISTS];
    for (int i = 0; i < N_ARTISTS; i++) {
        rows[i].text       = ARTISTS[i];
        rows[i].has_sub    = true;
        rows[i].is_current = (i == 0);
    }
    int selected = 4;
    int top = 0;
    lib_clamp_scroll(selected, N_ARTISTS, &top);
    screen_library_draw(&g, t, rows, N_ARTISTS, selected, top, "Library",
                        LIB_LEVEL_ARTIST);
    write_ppm(out);
}

static void render_track_level(const theme_t *t, const char *out)
{
    lib_row_t rows[N_TRACKS];
    for (int i = 0; i < N_TRACKS; i++) {
        rows[i].text       = TRACKS[i];
        rows[i].has_sub    = false;
        rows[i].is_current = (i == 0);
    }
    int selected = 2;
    int top = 0;
    lib_clamp_scroll(selected, N_TRACKS, &top);
    screen_library_draw(&g, t, rows, N_TRACKS, selected, top,
                        "Suite Bergamasque", LIB_LEVEL_TRACK);
    write_ppm(out);
}

/* A type specimen, so the three sizes can be judged at real scale. */
static void render_font_specimen(const theme_t *t, const char *out)
{
    gfx_clip_reset(&g);
    gfx_clear(&g, t->bg);

    int y = 8;
    gfx_text(&g, &font_sm, "Roboto 11  --  labels, hints", 8, y,
             t->text_secondary);
    y += font_sm.height + 6;
    gfx_text(&g, &font_md, "Roboto 14  --  list rows", 8, y, t->text_primary);
    y += font_md.height + 6;
    gfx_text(&g, &font_lg, "Roboto 18", 8, y, t->text_primary);
    y += font_lg.height + 10;

    gfx_hline(&g, 8, y, SCREEN_W - 16, t->surface_alt);
    y += 8;

    gfx_text(&g, &font_sm, "ABCDEFGHIJKLMNOPQRSTUVWXYZ", 8, y,
             t->text_secondary);
    y += font_sm.height + 2;
    gfx_text(&g, &font_sm, "abcdefghijklmnopqrstuvwxyz", 8, y,
             t->text_secondary);
    y += font_sm.height + 2;
    gfx_text(&g, &font_sm, "0123456789 !?.,:;'\"-()[]/&", 8, y,
             t->text_secondary);
    y += font_sm.height + 10;

    /* Truncation, at the exact width a library row gets. */
    gfx_text(&g, &font_sm, "truncation at 200px:", 8, y, t->text_inactive);
    y += font_sm.height + 3;
    gfx_text_ellipsis(&g, &font_sm,
                      "Symphony No. 9 in D minor, Op. 125 - IV. Presto", 8, y,
                      200, t->accent);
    y += font_sm.height + 10;

    /* The scrubber pill, at theme geometry. */
    gfx_text(&g, &font_sm, "scrubber:", 8, y, t->text_inactive);
    y += font_sm.height + 4;
    gfx_fill_rect(&g, 8, y + 6, SCREEN_W - 16, t->bar_height, t->accent_dim);
    gfx_fill_rect(&g, 8, y + 6, 90, t->bar_height, t->accent);
    gfx_fill_round_rect(&g, 96, y, t->pill_width, t->pill_height,
                        t->pill_radius, t->accent);

    write_ppm(out);
}

int main(void)
{
    gfx_init(&g, fb, SCREEN_W, SCREEN_H);

    printf("Rendering 240x240 previews...\n");

    render_artist_level(&THEME_DARK, "build/preview/library_dark.ppm");
    render_artist_level(&THEME_IPOD, "build/preview/library_ipod.ppm");
    render_artist_level(&THEME_WARM, "build/preview/library_warm.ppm");
    render_track_level(&THEME_DARK, "build/preview/tracks_dark.ppm");
    render_font_specimen(&THEME_DARK, "build/preview/specimen_dark.ppm");
    render_font_specimen(&THEME_IPOD, "build/preview/specimen_ipod.ppm");

    return 0;
}
