#ifndef FILE_SCANNER_H
#define FILE_SCANNER_H

#define MAX_TRACKS    512
#define MAX_PATH_LEN  512

typedef struct {
    char paths[MAX_TRACKS][MAX_PATH_LEN];
    int  count;
} TrackList;

/** Recursively scan dir (up to 4 levels deep) for audio files.
 *  Populates out->paths[] and out->count.
 *  Paths within each directory are returned in alphabetical order.
 *  Returns the number of tracks found.                             */
int file_scanner_scan(const char *dir, TrackList *out);

#endif /* FILE_SCANNER_H */
