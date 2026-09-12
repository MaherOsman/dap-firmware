/* platform_libio.c — FatFs implementation of lib_io_t and lib_dir_t. */

#include "platform_libio.h"

#include <string.h>

#include "ff.h"

/* Pools rather than malloc. The builder needs at most two files open at once
 * (temp + index) and one directory per nested level. */
#define PLAT_MAX_FILES  3u
#define PLAT_MAX_DIRS   (LIB_BUILD_MAX_DEPTH + 1u)

typedef struct {
    FIL fp;
    int in_use;
} file_slot_t;

typedef struct {
    DIR dp;
    int in_use;
} dir_slot_t;

static file_slot_t s_files[PLAT_MAX_FILES];
static dir_slot_t  s_dirs[PLAT_MAX_DIRS];
static FILINFO     s_fno;

static int      s_last_res;
static unsigned s_peak_files;
static unsigned s_peak_dirs;

static void note(FRESULT r)
{
    s_last_res = (int)r;
}

static unsigned files_open(void)
{
    unsigned i, n = 0;
    for (i = 0; i < PLAT_MAX_FILES; i++) if (s_files[i].in_use) n++;
    return n;
}

static unsigned dirs_open(void)
{
    unsigned i, n = 0;
    for (i = 0; i < PLAT_MAX_DIRS; i++) if (s_dirs[i].in_use) n++;
    return n;
}

/* ------------------------------------------------------------------ io */

static int p_open(void *ctx, const char *path, int mode, void **fh)
{
    file_slot_t *s = NULL;
    unsigned i;
    BYTE flags;
    FRESULT r;

    (void)ctx;

    for (i = 0; i < PLAT_MAX_FILES; i++) {
        if (!s_files[i].in_use) { s = &s_files[i]; break; }
    }
    if (!s) {
        s_last_res = -1;
        return -1;
    }

    flags = (mode == LIB_IO_WRITE)
          ? (BYTE)(FA_CREATE_ALWAYS | FA_WRITE)
          : (BYTE)FA_READ;

    r = f_open(&s->fp, path, flags);
    note(r);
    if (r != FR_OK) return -1;

    s->in_use = 1;
    if (files_open() > s_peak_files) s_peak_files = files_open();
    *fh = s;
    return 0;
}

static int p_read(void *ctx, void *fh, void *dst, uint32_t len, uint32_t *got)
{
    file_slot_t *s = (file_slot_t *)fh;
    UINT br = 0;
    FRESULT r;

    (void)ctx;
    r = f_read(&s->fp, dst, (UINT)len, &br);
    note(r);
    if (r != FR_OK) return -1;
    *got = (uint32_t)br;
    return 0;
}

static int p_write(void *ctx, void *fh, const void *src, uint32_t len)
{
    file_slot_t *s = (file_slot_t *)fh;
    UINT bw = 0;
    FRESULT r;

    (void)ctx;
    r = f_write(&s->fp, src, (UINT)len, &bw);
    note(r);
    /* A short write means the card is full. FatFs reports FR_OK for it, so
     * checking bw is the only way to catch it. */
    if (r != FR_OK || bw != len) return -1;
    return 0;
}

static int p_seek(void *ctx, void *fh, uint32_t off)
{
    file_slot_t *s = (file_slot_t *)fh;
    FRESULT r;

    (void)ctx;
    r = f_lseek(&s->fp, (FSIZE_t)off);
    note(r);
    if (r != FR_OK) return -1;
    if (f_tell(&s->fp) != (FSIZE_t)off) return -1;
    return 0;
}

static int p_close(void *ctx, void *fh)
{
    file_slot_t *s = (file_slot_t *)fh;
    FRESULT r;

    (void)ctx;
    r = f_close(&s->fp);
    note(r);
    s->in_use = 0;
    return (r == FR_OK) ? 0 : -1;
}

static int p_unlink(void *ctx, const char *path)
{
    FRESULT r;
    (void)ctx;
    r = f_unlink(path);
    note(r);
    return (r == FR_OK) ? 0 : -1;
}

/* ----------------------------------------------------------- directory */

static int p_opendir(void *ctx, const char *path, void **dh)
{
    dir_slot_t *s = NULL;
    unsigned i;
    FRESULT r;

    (void)ctx;

    for (i = 0; i < PLAT_MAX_DIRS; i++) {
        if (!s_dirs[i].in_use) { s = &s_dirs[i]; break; }
    }
    if (!s) {
        s_last_res = -1;
        return -1;
    }

    r = f_opendir(&s->dp, path);
    note(r);
    if (r != FR_OK) return -1;

    s->in_use = 1;
    if (dirs_open() > s_peak_dirs) s_peak_dirs = dirs_open();
    *dh = s;
    return 0;
}

static int p_readdir(void *ctx, void *dh, lib_dirent_t *out, int *done)
{
    dir_slot_t *s = (dir_slot_t *)dh;
    FRESULT r;
    const char *name;

    (void)ctx;

    r = f_readdir(&s->dp, &s_fno);
    note(r);
    if (r != FR_OK) return -1;

    if (s_fno.fname[0] == '\0') {
        *done = 1;
        return 0;
    }

#if _USE_LFN
    /* fname holds the long name when LFN is enabled; altname is the 8.3 form.
     * Fall back to altname for entries that have no long name. */
    name = (s_fno.fname[0] != '\0') ? s_fno.fname : s_fno.altname;
#else
    name = s_fno.fname;
#endif

    strncpy(out->name, name, LIB_FILENAME_MAX - 1u);
    out->name[LIB_FILENAME_MAX - 1u] = '\0';
    out->is_dir = (s_fno.fattrib & AM_DIR) ? 1 : 0;
    out->size = (uint32_t)s_fno.fsize;
    *done = 0;
    return 0;
}

static int p_closedir(void *ctx, void *dh)
{
    dir_slot_t *s = (dir_slot_t *)dh;
    FRESULT r;

    (void)ctx;
    r = f_closedir(&s->dp);
    note(r);
    s->in_use = 0;
    return (r == FR_OK) ? 0 : -1;
}

/* ------------------------------------------------------------------ api */

void plat_libio_init(lib_io_t *io, lib_dir_t *dir)
{
    memset(s_files, 0, sizeof(s_files));
    memset(s_dirs, 0, sizeof(s_dirs));
    s_last_res = 0;
    s_peak_files = 0;
    s_peak_dirs = 0;

    if (io) {
        memset(io, 0, sizeof(*io));
        io->ctx = 0;
        io->open = p_open;
        io->read = p_read;
        io->write = p_write;
        io->seek = p_seek;
        io->close = p_close;
        io->unlink = p_unlink;
    }
    if (dir) {
        memset(dir, 0, sizeof(*dir));
        dir->ctx = 0;
        dir->opendir = p_opendir;
        dir->readdir = p_readdir;
        dir->closedir = p_closedir;
    }
}

int plat_libio_last_result(void)
{
    return s_last_res;
}

const char *plat_libio_result_name(int fres)
{
    switch (fres) {
    case FR_OK:                  return "OK";
    case FR_DISK_ERR:            return "DISK_ERR";
    case FR_INT_ERR:             return "INT_ERR";
    case FR_NOT_READY:           return "NOT_READY";
    case FR_NO_FILE:             return "NO_FILE";
    case FR_NO_PATH:             return "NO_PATH";
    case FR_INVALID_NAME:        return "INVALID_NAME";
    case FR_DENIED:              return "DENIED";
    case FR_EXIST:               return "EXIST";
    case FR_INVALID_OBJECT:      return "INVALID_OBJECT";
    case FR_WRITE_PROTECTED:     return "WRITE_PROTECTED";
    case FR_INVALID_DRIVE:       return "INVALID_DRIVE";
    case FR_NOT_ENABLED:         return "NOT_ENABLED";
    case FR_NO_FILESYSTEM:       return "NO_FILESYSTEM";
    case FR_MKFS_ABORTED:        return "MKFS_ABORTED";
    case FR_TIMEOUT:             return "TIMEOUT";
    case FR_LOCKED:              return "LOCKED";
    case FR_NOT_ENOUGH_CORE:     return "NOT_ENOUGH_CORE";
    case FR_TOO_MANY_OPEN_FILES: return "TOO_MANY_OPEN_FILES";
    case FR_INVALID_PARAMETER:   return "INVALID_PARAMETER";
    case -1:                     return "HANDLE_POOL_EMPTY";
    default:                     return "?";
    }
}

unsigned plat_libio_peak_files(void) { return s_peak_files; }
unsigned plat_libio_peak_dirs(void)  { return s_peak_dirs; }
