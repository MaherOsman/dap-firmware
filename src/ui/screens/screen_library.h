#ifndef SCREEN_LIBRARY_H
#define SCREEN_LIBRARY_H

#include "../themes/theme.h"

#define LIBRARY_ROW_H  18

/* Which drill-down level is currently shown */
typedef enum {
    LIB_LEVEL_ARTIST = 0,
    LIB_LEVEL_ALBUM  = 1,
    LIB_LEVEL_TRACK  = 2
} LibLevel;

/* One displayable row at the current level */
typedef struct {
    const char *text;        /* display string                          */
    int         has_sub;     /* 1 = artist or album (shows › chevron)   */
    int         is_current;  /* 1 = contains / is the playing track     */
} LibRow;

void screen_library_init   (void);
void screen_library_destroy(void);

/* header   – shown in the top bar (e.g. "Library", artist name, album name)
   level    – affects the navigation hint text                              */
void screen_library_draw(void         *renderer,
                          const Theme  *theme,
                          const LibRow *rows,
                          int           row_count,
                          int           selected,
                          int           scroll_top,
                          const char   *header,
                          LibLevel      level,
                          int           screen_w,
                          int           screen_h);

#endif /* SCREEN_LIBRARY_H */
