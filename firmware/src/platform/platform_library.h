/* platform_library.h — bring the on-card index up at boot.
 *
 * Wraps the scan/open decision so main.c needs one call:
 *
 *     dap_library_init(0);
 *
 * Opens /dap.idx if it is present and current; otherwise scans the card and
 * writes it. Everything it does is narrated over the VCP, because an index
 * that silently fails to build looks exactly like an empty card.
 */
#ifndef PLATFORM_LIBRARY_H
#define PLATFORM_LIBRARY_H

#include "library_index.h"

/* Bump this to force a rescan on the next boot without deleting the file:
 * the stored build_id is compared against it. */
#define DAP_INDEX_BUILD_ID  1u

/* Folder to scan. "/" walks the whole card. */
#define DAP_MUSIC_ROOT      "/"

/* force_rescan != 0 always rebuilds, even if a current index exists.
 * Returns LIB_OK when the library is open and usable. */
int dap_library_init(int force_rescan);

/* The open index, or NULL if it isn't open. Valid until the next init. */
libidx_t *dap_library(void);

#endif /* PLATFORM_LIBRARY_H */
