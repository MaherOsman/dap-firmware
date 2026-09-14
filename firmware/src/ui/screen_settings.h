/*
 * screen_settings — a list of settings, each a label and a cycling value.
 *
 * Built as a table rather than as hardcoded rows, because the sidebar this
 * is heading towards will have several destinations that are all "a list of
 * things you can change". Adding "Album art: on/off" later should be one
 * table entry, not new drawing code.
 *
 * Changes are applied and saved the moment they are made — there is no save
 * button and no unsaved state to lose. Long-press just leaves.
 *
 * Owns no globals, calls no platform, draws into a gfx_t. The router maps
 * item ids to config fields; this file never knows what a theme is.
 */
#ifndef SCREEN_SETTINGS_H
#define SCREEN_SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

#include "../core/gfx.h"
#include "../core/theme.h"

typedef enum {
    SET_ID_THEME = 0,
    SET_ID_REPEAT,
    SET_ID_RESCAN,
    SET_ID_COUNT
} set_id_t;

#define SET_MAX_VALUES 8

typedef struct {
    set_id_t    id;
    const char *label;
    /* NULL for an action row (drawn with a chevron, activating it does
     * something rather than cycling a value). */
    const char *const *values;
    uint8_t     value_count;
    uint8_t     value;          /* index into values */
} set_item_t;

typedef struct {
    set_item_t items[SET_ID_COUNT];
    uint8_t    count;
    int        sel;
    int        top;
} settings_t;

/* Builds the table. `theme`, `repeat` are current values; they are clamped
 * to the available choices. */
void settings_init(settings_t *s, uint8_t theme, uint8_t repeat);

int  settings_count(const settings_t *s);
int  settings_selected(const settings_t *s);
int  settings_scroll_top(const settings_t *s);

void settings_move(settings_t *s, int delta);

/*
 * Activate the selected row. A value row cycles to its next value and
 * returns its id — the caller applies and saves it. An action row returns
 * its id without changing anything.
 *
 * Returns SET_ID_COUNT when there is nothing to activate.
 */
set_id_t settings_activate(settings_t *s);

/* Current value index of an item, or 0 if absent. */
uint8_t settings_value(const settings_t *s, set_id_t id);

/* Keeps the table in step when a value changes elsewhere. */
void settings_set_value(settings_t *s, set_id_t id, uint8_t value);

void screen_settings_draw(gfx_t *g, const theme_t *t, const settings_t *s);

#endif /* SCREEN_SETTINGS_H */
