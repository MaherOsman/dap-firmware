/* screen_now_playing.c — art-first playback screen, drawn to a framebuffer. */

#include "screen_now_playing.h"

#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------- layout */

#define MARGIN        10
#define ART_SIZE      104
#define ART_Y         (LIB_HEADER_H + 6)

#define TITLE_Y       (ART_Y + ART_SIZE + 6)
#define ARTIST_Y      (TITLE_Y + 20)

#define BAR_Y         (ARTIST_Y + 18)      /* centre line of the scrubber */
#define TIMES_Y       (BAR_Y + 6)

#define CTL_ROW_Y     (SCREEN_H - 30)
#define CTL_ROW_H     24
#define CTL_ICON      11                   /* nominal icon half-extent */

#define DASH          "-"

static const char *or_dash(const char *s)
{
    return (s != NULL && s[0] != '\0') ? s : DASH;
}

/* --------------------------------------------------------------- helpers */

bool np_ctl_enabled(const np_state_t *s, np_ctl_t c)
{
    uint32_t mask;

    if (s == NULL || c >= NP_CTL_COUNT) return false;
    mask = (s->enabled != 0u) ? s->enabled : (uint32_t)NP_CTL_DEFAULT;
    return (mask & NP_BIT(c)) != 0u;
}

np_ctl_t np_focus_move(np_state_t *s, int delta)
{
    int cur, step, i;

    if (s == NULL) return NP_CTL_PLAY;

    cur = (int)s->focus;
    if (cur < 0 || cur >= (int)NP_CTL_COUNT) cur = 0;

    /* If focus is sitting on something disabled (the mask changed under us),
     * pull it to the nearest enabled control before moving. */
    if (!np_ctl_enabled(s, (np_ctl_t)cur)) {
        for (i = 0; i < (int)NP_CTL_COUNT; i++) {
            if (np_ctl_enabled(s, (np_ctl_t)i)) { cur = i; break; }
        }
        if (!np_ctl_enabled(s, (np_ctl_t)cur)) return s->focus;  /* none */
    }

    step = (delta > 0) ? 1 : -1;
    while (delta != 0) {
        int next = cur + step;

        while (next >= 0 && next < (int)NP_CTL_COUNT &&
               !np_ctl_enabled(s, (np_ctl_t)next)) {
            next += step;
        }
        /* Clamp at the ends rather than wrapping — a three-control row that
         * wraps feels like it slipped rather than stopped. */
        if (next < 0 || next >= (int)NP_CTL_COUNT) break;

        cur = next;
        delta -= step;
    }

    s->focus = (np_ctl_t)cur;
    return s->focus;
}

void np_fmt_time(char *buf, uint32_t ms)
{
    uint32_t total = ms / 1000u;
    uint32_t mins = total / 60u;
    uint32_t secs = total % 60u;

    if (mins > 99u) { mins = 99u; secs = 59u; }
    (void)snprintf(buf, 8, "%lu:%02lu", (unsigned long)mins,
                   (unsigned long)secs);
}

uint32_t np_progress_permille(const np_state_t *s)
{
    uint64_t p;

    if (s == NULL || s->duration_ms == 0u) return 0u;
    if (s->elapsed_ms >= s->duration_ms) return 1000u;

    p = ((uint64_t)s->elapsed_ms * 1000u) / s->duration_ms;
    return (uint32_t)p;
}

/* ----------------------------------------------------------------- icons */

/* Right-pointing triangle with its tip at (x + w, y + h/2). */
static void icon_triangle(gfx_t *g, int x, int y, int w, int h, uint16_t c)
{
    int i;
    for (i = 0; i < w; i++) {
        int col_h = h - (2 * h * i) / (2 * w);
        if (col_h <= 0) break;
        gfx_vline(g, x + i, y + (h - col_h) / 2, col_h, c);
    }
}

static void icon_triangle_left(gfx_t *g, int x, int y, int w, int h,
                               uint16_t c)
{
    int i;
    for (i = 0; i < w; i++) {
        int col_h = h - (2 * h * (w - 1 - i)) / (2 * w);
        if (col_h <= 0) continue;
        gfx_vline(g, x + i, y + (h - col_h) / 2, col_h, c);
    }
}

static void icon_play(gfx_t *g, int cx, int cy, uint16_t c)
{
    icon_triangle(g, cx - 5, cy - 7, 12, 14, c);
}

static void icon_pause(gfx_t *g, int cx, int cy, uint16_t c)
{
    gfx_fill_rect(g, cx - 5, cy - 7, 4, 14, c);
    gfx_fill_rect(g, cx + 2, cy - 7, 4, 14, c);
}

static void icon_prev(gfx_t *g, int cx, int cy, uint16_t c)
{
    gfx_fill_rect(g, cx - 7, cy - 6, 2, 12, c);
    icon_triangle_left(g, cx - 4, cy - 6, 10, 12, c);
}

static void icon_next(gfx_t *g, int cx, int cy, uint16_t c)
{
    icon_triangle(g, cx - 6, cy - 6, 10, 12, c);
    gfx_fill_rect(g, cx + 5, cy - 6, 2, 12, c);
}

/* Speaker cone plus two arcs, drawn geometrically because the baked fonts
 * are ASCII-only and have no symbol glyphs. */
static void icon_volume(gfx_t *g, int cx, int cy, uint16_t c)
{
    gfx_fill_rect(g, cx - 7, cy - 3, 4, 6, c);
    icon_triangle_left(g, cx - 3, cy - 7, 5, 14, c);
    gfx_vline(g, cx + 4, cy - 3, 6, c);
    gfx_vline(g, cx + 6, cy - 5, 10, c);
}

static void icon_info(gfx_t *g, int cx, int cy, uint16_t c)
{
    gfx_rect(g, cx - 6, cy - 7, 13, 15, c);
    gfx_fill_rect(g, cx - 1, cy - 4, 2, 2, c);   /* the dot */
    gfx_fill_rect(g, cx - 1, cy - 1, 2, 6, c);   /* the stem */
}

/* ------------------------------------------------------------- top bar */

static void draw_topbar(gfx_t *g, const theme_t *t, const char *text)
{
    int y;

    gfx_hline(g, 0, t->topbar_height - 1, SCREEN_W, t->surface_alt);
    y = (t->topbar_height - font_sm.height) / 2;

    /* The simulator marquee-scrolled this using a per-frame tick. Here the
     * screen only redraws on input, so a frame-counted scroll would sit
     * still — ellipsis until there is a redraw timer to drive it. */
    gfx_clip(g, MARGIN, 0, SCREEN_W - MARGIN * 2, t->topbar_height);
    (void)gfx_text_ellipsis(g, &font_sm, or_dash(text), MARGIN, y,
                            SCREEN_W - MARGIN * 2, t->text_secondary);
    gfx_clip_reset(g);
}

/* ------------------------------------------------------------ album art */

static void draw_album_art(gfx_t *g, const theme_t *t)
{
    int x = (SCREEN_W - ART_SIZE) / 2;
    int cx = x + ART_SIZE / 2;
    int cy = ART_Y + ART_SIZE / 2;
    int r;

    gfx_fill_rect(g, x, ART_Y, ART_SIZE, ART_SIZE, t->art_placeholder_bg);
    gfx_rect(g, x, ART_Y, ART_SIZE, ART_SIZE, t->surface_alt);

    /* Concentric record grooves, matching the simulator's placeholder.
     * Drawn as a midpoint circle per radius — no floating point, no libm. */
    for (r = ART_SIZE / 2 - 10; r > 6; r -= 8) {
        int px = r, py = 0, err = 1 - r;
        while (px >= py) {
            gfx_pixel(g, cx + px, cy + py, t->art_placeholder_fg);
            gfx_pixel(g, cx + py, cy + px, t->art_placeholder_fg);
            gfx_pixel(g, cx - py, cy + px, t->art_placeholder_fg);
            gfx_pixel(g, cx - px, cy + py, t->art_placeholder_fg);
            gfx_pixel(g, cx - px, cy - py, t->art_placeholder_fg);
            gfx_pixel(g, cx - py, cy - px, t->art_placeholder_fg);
            gfx_pixel(g, cx + py, cy - px, t->art_placeholder_fg);
            gfx_pixel(g, cx + px, cy - py, t->art_placeholder_fg);
            py++;
            if (err < 0) {
                err += 2 * py + 1;
            } else {
                px--;
                err += 2 * (py - px) + 1;
            }
        }
    }

    gfx_fill_round_rect(g, cx - 3, cy - 3, 6, 6, 3, t->accent);
}

/* ------------------------------------------------------------- scrubber */

static void draw_scrubber(gfx_t *g, const theme_t *t, const np_state_t *s)
{
    int bar_x = MARGIN;
    int bar_w = SCREEN_W - MARGIN * 2;
    int bar_h = (int)t->bar_height;
    int bar_top = BAR_Y - bar_h / 2;
    int filled = (int)(((uint32_t)bar_w * np_progress_permille(s)) / 1000u);
    char buf[8];

    gfx_fill_rect(g, bar_x, bar_top, bar_w, bar_h, t->accent_dim);
    if (filled > 0) {
        gfx_fill_rect(g, bar_x, bar_top, filled, bar_h, t->accent);
    }

    gfx_fill_round_rect(g, bar_x + filled - (int)t->pill_width / 2,
                        BAR_Y - (int)t->pill_height / 2,
                        (int)t->pill_width, (int)t->pill_height,
                        (int)t->pill_radius, t->accent);

    np_fmt_time(buf, s->elapsed_ms);
    (void)gfx_text(g, &font_sm, buf, bar_x, TIMES_Y, t->text_inactive);

    np_fmt_time(buf, s->duration_ms);
    (void)gfx_text_right(g, &font_sm, buf, bar_x + bar_w, TIMES_Y,
                         t->text_inactive);
}

/* ------------------------------------------------------------- controls */

/* Lays the enabled controls out evenly and calls back per control with its
 * cell centre. Used for drawing; keeping it in one place means the focus
 * ring and the icons can never disagree about where a control is. */
static int control_cells(const np_state_t *s, np_ctl_t *order, int *cx)
{
    int n = 0, i;
    int cell;

    for (i = 0; i < (int)NP_CTL_COUNT; i++) {
        if (np_ctl_enabled(s, (np_ctl_t)i)) {
            order[n] = (np_ctl_t)i;
            n++;
        }
    }
    if (n == 0) return 0;

    cell = SCREEN_W / n;
    for (i = 0; i < n; i++) {
        cx[i] = cell * i + cell / 2;
    }
    return n;
}

static void draw_controls(gfx_t *g, const theme_t *t, const np_state_t *s)
{
    np_ctl_t order[NP_CTL_COUNT];
    int cx[NP_CTL_COUNT];
    int n, i, cell;

    n = control_cells(s, order, cx);
    if (n == 0) return;
    cell = SCREEN_W / n;

    gfx_hline(g, 0, CTL_ROW_Y - 6, SCREEN_W, t->surface_alt);

    for (i = 0; i < n; i++) {
        bool focused = (order[i] == s->focus);
        uint16_t col = focused ? t->accent : t->icon_inactive;
        int mid = CTL_ROW_Y + CTL_ROW_H / 2;

        if (focused) {
            /* The ring, not a fill: the icon stays legible and the row does
             * not flash a block as focus moves. */
            int rw = cell - 8;
            if (rw > 44) rw = 44;
            gfx_fill_round_rect(g, cx[i] - rw / 2, CTL_ROW_Y, rw, CTL_ROW_H,
                                4, t->surface);
            gfx_rect(g, cx[i] - rw / 2, CTL_ROW_Y, rw, CTL_ROW_H, t->accent);
        }

        switch (order[i]) {
        case NP_CTL_PREV: icon_prev(g, cx[i], mid, col);   break;
        case NP_CTL_NEXT: icon_next(g, cx[i], mid, col);   break;
        case NP_CTL_VOL:  icon_volume(g, cx[i], mid, col); break;
        case NP_CTL_INFO: icon_info(g, cx[i], mid, col);   break;
        case NP_CTL_PLAY:
        default:
            if (s->is_playing) icon_pause(g, cx[i], mid, col);
            else               icon_play(g, cx[i], mid, col);
            break;
        }
    }
}

/* ------------------------------------------------------------- overlay */

void screen_volume_overlay_draw(gfx_t *g, const theme_t *t, int volume_pct)
{
    const int OVL_W = 180;
    const int OVL_H = 22;
    const int OVL_X = (SCREEN_W - OVL_W) / 2;
    const int OVL_Y = 34;
    const int BAR_X = OVL_X + 34;
    const int BAR_W = OVL_W - 34 - 40;
    const int BAR_H = 4;
    const int BY    = OVL_Y + (OVL_H - BAR_H) / 2;
    char pct[8];
    int fill;

    if (volume_pct < 0)   volume_pct = 0;
    if (volume_pct > 100) volume_pct = 100;

    gfx_fill_round_rect(g, OVL_X, OVL_Y, OVL_W, OVL_H, 4, t->surface);
    gfx_rect(g, OVL_X, OVL_Y, OVL_W, OVL_H, t->surface_alt);

    (void)gfx_text(g, &font_sm, "VOL", OVL_X + 6,
                   OVL_Y + (OVL_H - font_sm.height) / 2, t->text_secondary);

    gfx_fill_rect(g, BAR_X, BY, BAR_W, BAR_H, t->accent_dim);
    fill = BAR_W * volume_pct / 100;
    if (fill > 0) {
        gfx_fill_rect(g, BAR_X, BY, fill, BAR_H, t->accent);
    }

    (void)snprintf(pct, sizeof(pct), "%d%%", volume_pct);
    (void)gfx_text_right(g, &font_sm, pct, OVL_X + OVL_W - 6,
                         OVL_Y + (OVL_H - font_sm.height) / 2,
                         t->text_primary);
}

/* ==================================================================== */

void screen_now_playing_draw(gfx_t *g, const theme_t *t, const np_state_t *s)
{
    if (g == NULL || t == NULL || s == NULL) return;

    gfx_clip_reset(g);
    gfx_clear(g, t->bg);

    draw_topbar(g, t, s->album);
    draw_album_art(g, t);

    (void)gfx_text_ellipsis(g, &font_lg, or_dash(s->title), MARGIN, TITLE_Y,
                            SCREEN_W - MARGIN * 2, t->text_primary);
    (void)gfx_text_ellipsis(g, &font_sm, or_dash(s->artist), MARGIN, ARTIST_Y,
                            SCREEN_W - MARGIN * 2, t->text_secondary);

    draw_scrubber(g, t, s);
    draw_controls(g, t, s);

    /* Drawn last so it sits over everything, and only while the mode is
     * active — an invisible mode is a mode you get stuck in. */
    if (s->vol_active) {
        screen_volume_overlay_draw(g, t, (int)s->volume_pct);
    }
}

/* ==================================================================== */

void screen_info_draw(gfx_t *g, const theme_t *t, const np_state_t *s)
{
    const int LABEL_X = 8;
    const int VALUE_X = 62;
    const int VALUE_W = SCREEN_W - VALUE_X - 8;
    const int ROW_H = font_sm.height + 5;
    int y;
    char buf[64];

    if (g == NULL || t == NULL || s == NULL) return;

    gfx_clip_reset(g);
    gfx_clear(g, t->bg);

    gfx_hline(g, 0, t->topbar_height - 1, SCREEN_W, t->surface_alt);
    (void)gfx_text_centered(g, &font_sm, "Track Info", 0,
                            (t->topbar_height - font_sm.height) / 2, SCREEN_W,
                            t->text_secondary);

    y = t->topbar_height + 6;

    #define ROW(lbl, val)                                                     \
        do {                                                                  \
            (void)gfx_text(g, &font_sm, (lbl), LABEL_X, y, t->text_secondary);\
            (void)gfx_text_ellipsis(g, &font_sm, (val), VALUE_X, y, VALUE_W,  \
                                    t->text_primary);                         \
            y += ROW_H;                                                       \
        } while (0)

    ROW("Track",  or_dash(s->title));
    ROW("Artist", or_dash(s->artist));
    ROW("Album",  or_dash(s->album));

    if (s->format != NULL && s->bit_depth > 0u && s->sample_rate_hz > 0u) {
        (void)snprintf(buf, sizeof(buf), "%s  %u-bit / %lu.%lu kHz",
                       s->format, (unsigned)s->bit_depth,
                       (unsigned long)(s->sample_rate_hz / 1000u),
                       (unsigned long)((s->sample_rate_hz % 1000u) / 100u));
    } else {
        (void)snprintf(buf, sizeof(buf), "%s", or_dash(s->format));
    }
    ROW("Format", buf);

    if (s->channels > 0u) {
        (void)snprintf(buf, sizeof(buf), "%u", (unsigned)s->channels);
        ROW("Channels", buf);
    }

    if (s->bitrate_kbps > 0u) {
        (void)snprintf(buf, sizeof(buf), "%lu kbps",
                       (unsigned long)s->bitrate_kbps);
    } else {
        (void)snprintf(buf, sizeof(buf), DASH);
    }
    ROW("Bitrate", buf);

    np_fmt_time(buf, s->duration_ms);
    ROW("Length", buf);

    /* The path is the thing you actually want when a track will not play,
     * so it gets a row even though it is usually too long to fit. */
    ROW("File", or_dash(s->path));

    #undef ROW

    (void)gfx_text_centered(g, &font_sm, "hold: back", 0,
                            SCREEN_H - font_sm.height - 4, SCREEN_W,
                            t->text_inactive);
}
