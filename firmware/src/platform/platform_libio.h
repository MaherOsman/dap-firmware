/* platform_libio.h — FatFs backing for the library index seams.
 *
 * This is the only place the index code meets the HAL. Everything in
 * src/core/ stays hardware-free; this file hands it f_open/f_read/f_readdir.
 *
 * Not compiled by the host test build.
 */
#ifndef PLATFORM_LIBIO_H
#define PLATFORM_LIBIO_H

#include "lib_io.h"
#include "library_build.h"

/* Fills both seams. Safe to call more than once; it resets the handle pools,
 * so do not call it while a file or directory is open. */
void plat_libio_init(lib_io_t *io, lib_dir_t *dir);

/* Last FRESULT seen by any call, and its name. Silent failures are the
 * hazard here: when a build returns LIB_E_IO, print this to find out whether
 * it was NO_FILESYSTEM, DISK_ERR, NOT_ENOUGH_CORE or something else. */
int         plat_libio_last_result(void);
const char *plat_libio_result_name(int fres);

/* Handle-pool high-water marks, for checking the pool sizes are right. */
unsigned plat_libio_peak_files(void);
unsigned plat_libio_peak_dirs(void);

#endif /* PLATFORM_LIBIO_H */
