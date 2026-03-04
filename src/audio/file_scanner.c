#include "file_scanner.h"

#include <dirent.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>  /* strcasecmp */

/* ─────────────────────────────────────────────
   Helpers
───────────────────────────────────────────── */
static int is_audio_ext(const char *name)
{
    const char *ext = strrchr(name, '.');
    if (!ext) return 0;
    return (strcasecmp(ext, ".flac") == 0 ||
            strcasecmp(ext, ".mp3")  == 0 ||
            strcasecmp(ext, ".wav")  == 0 ||
            strcasecmp(ext, ".aiff") == 0 ||
            strcasecmp(ext, ".ogg")  == 0);
}

/* qsort comparator for char arrays of MAX_PATH_LEN */
static int cmp_path(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

/* ─────────────────────────────────────────────
   Recursive directory walk
   We use stat() as a fallback for filesystems
   (e.g. NTFS via WSL) that return DT_UNKNOWN.
───────────────────────────────────────────── */
static void scan_recursive(const char *dir, TrackList *out, int depth)
{
    if (depth > 4 || out->count >= MAX_TRACKS) return;

    DIR *d = opendir(dir);
    if (!d) return;

    /* Collect entries so we can sort within each directory */
    char subdirs[64][MAX_PATH_LEN];
    int  nsubdirs = 0;
    int  track_start = out->count;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;   /* skip hidden / . / .. */

        char path[MAX_PATH_LEN];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);

        /* Resolve type, falling back to stat() for DT_UNKNOWN */
        int is_dir = 0, is_reg = 0;
        if (ent->d_type == DT_DIR) {
            is_dir = 1;
        } else if (ent->d_type == DT_REG) {
            is_reg = 1;
        } else {
            struct stat st;
            if (stat(path, &st) == 0) {
                is_dir = S_ISDIR(st.st_mode);
                is_reg = S_ISREG(st.st_mode);
            }
        }

        if (is_dir && nsubdirs < 64) {
            snprintf(subdirs[nsubdirs++], MAX_PATH_LEN, "%s", path);
        } else if (is_reg && is_audio_ext(ent->d_name) && out->count < MAX_TRACKS) {
            snprintf(out->paths[out->count++], MAX_PATH_LEN, "%s", path);
        }
    }
    closedir(d);

    /* Sort the tracks added from this directory alphabetically */
    if (out->count > track_start)
        qsort(out->paths[track_start],
              out->count - track_start,
              MAX_PATH_LEN,
              cmp_path);

    /* Sort and recurse into subdirectories */
    qsort(subdirs, nsubdirs, MAX_PATH_LEN, cmp_path);
    for (int i = 0; i < nsubdirs; i++)
        scan_recursive(subdirs[i], out, depth + 1);
}

/* ─────────────────────────────────────────────
   Public API
───────────────────────────────────────────── */
int file_scanner_scan(const char *dir, TrackList *out)
{
    out->count = 0;
    scan_recursive(dir, out, 0);
    return out->count;
}
