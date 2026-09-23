/*
 * platform_art — album art on the device: find it on the card, decode it,
 * keep the result.
 *
 * One cover is cached at a time, keyed by album and size. Asking again for
 * the same album at the same size is free, so the now-playing screen can ask
 * on every repaint. A new album — or switching between the Standard and
 * Art-only layouts, which want different sizes — triggers one decode.
 *
 * The decode keeps the audio fed: the decoder's yield hook is
 * plat_audio_service(), called after every block of the image, the same
 * trick the banded panel push uses.
 *
 * The workspace and the cached image (~146 KB) live in the D2 SRAM, via the
 * .ram_d2 section in the linker script. AXI SRAM is nearly full with the
 * framebuffer and audio buffers.
 */
#ifndef PLATFORM_ART_H
#define PLATFORM_ART_H

#include <stdint.h>

/* Call once at start-up, before the first plat_art_for(). */
void plat_art_init(void);

/*
 * The cover for `album` at `size` x `size`, decoding it if it is not the
 * one already cached. `track_path` is any track of that album — folder art
 * is looked for beside it, embedded art inside it.
 *
 * Returns NULL when the album has no art the decoder can use (the reason
 * goes to the serial log once, not on every call). The pointer stays valid
 * until the next call with a different album or size.
 */
const uint16_t *plat_art_for(uint32_t album, const char *track_path, int size);

/* Drops the cache. Album numbers change on a rescan. */
void plat_art_forget(void);

#endif /* PLATFORM_ART_H */
