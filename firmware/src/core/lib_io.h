/* lib_io.h — injected file access for the library index.
 *
 * The index reader/builder never touch FatFs or stdio. The STM32 build plugs
 * in f_open/f_read/f_lseek from src/platform/; host tests plug in a RAM or
 * stdio backend. Same rule as st7789_bus_t.
 *
 * Contract:
 *   - every call returns 0 on success, negative on failure
 *   - offsets are absolute byte offsets from the start of the file
 *   - read() sets *got to the number of bytes actually read; a short read is
 *     NOT an error, callers check *got themselves (EOF is detected this way)
 *   - open() for write must create-or-truncate
 */
#ifndef LIB_IO_H
#define LIB_IO_H

#include <stdint.h>

#define LIB_IO_READ   0
#define LIB_IO_WRITE  1

typedef struct lib_io {
    void *ctx;

    /* mode is LIB_IO_READ or LIB_IO_WRITE. On success *fh receives an opaque
     * handle owned by the backend. */
    int (*open)(void *ctx, const char *path, int mode, void **fh);

    int (*read)(void *ctx, void *fh, void *dst, uint32_t len, uint32_t *got);

    /* May be NULL on a read-only backend; the builder requires it. */
    int (*write)(void *ctx, void *fh, const void *src, uint32_t len);

    int (*seek)(void *ctx, void *fh, uint32_t off);

    int (*close)(void *ctx, void *fh);

    /* Optional: remove a file. May be NULL. Used to drop the temp file after
     * a build. */
    int (*unlink)(void *ctx, const char *path);
} lib_io_t;

#endif /* LIB_IO_H */
