/*
 * art — decode a JPEG album cover into a square RGB565 image of any size.
 *
 * The cover can be any size and shape; what comes out is exactly
 * `size` x `size` pixels, ready to blit:
 *
 *   1. TJpgDec shrinks it by 1/2, 1/4 or 1/8 while decoding — the biggest
 *      step that still leaves at least `size` pixels — so a 1400 px cover is
 *      never decoded at full resolution.
 *   2. A non-square cover is cropped to its centre square (covers are square
 *      to within a few pixels in practice; cropping beats letterboxing).
 *   3. That square is area-averaged down to `size` — every source pixel
 *      counts, which is what keeps fine detail from turning jagged. A cover
 *      smaller than `size` is enlarged by pixel repetition instead.
 *
 * It streams: TJpgDec hands over the picture a few rows at a time, and the
 * averaging works on those rows as they arrive. No full-size copy of the
 * image ever exists, so the whole decode fits in about 45 KB of working
 * memory (art_work_t, ~78 KB) plus the output buffer.
 *
 * It yields: `yield` is called after every block TJpgDec produces. On the
 * device that is plat_audio_service(), which is how a decode that takes a
 * few hundred milliseconds does not starve the audio ring — the same trick
 * the banded panel push uses.
 *
 * No FatFs, no HAL: bytes come in through `read`, so the tests feed it
 * JPEGs from memory and the device feeds it a file, or a span inside one
 * (art embedded in a FLAC or MP3).
 */
#ifndef ART_H
#define ART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ART_MAX_SIZE    256     /* largest output side supported */
#define ART_POOL_BYTES  12288   /* TJpgDec workspace (JD_FASTDECODE 2) */
#define ART_ACC_ROWS    20      /* output rows one decoded band can touch */

typedef enum {
    ART_OK = 0,
    ART_ERR_ARG,          /* bad parameters */
    ART_ERR_READ,         /* the byte source failed or ended early */
    ART_ERR_FORMAT,       /* not a JPEG, or a damaged one */
    ART_ERR_UNSUPPORTED,  /* a real JPEG TJpgDec cannot do — progressive,
                           * arithmetic-coded, 12-bit. Show the placeholder. */
    ART_ERR_TOO_BIG,      /* absurd dimensions */
    ART_ERR_MEM           /* the workspace was too small for this image */
} art_result_t;

typedef struct {
    /* Copy up to `len` bytes into `buf` and return how many were copied;
     * fewer than asked means end of data. When `buf` is NULL, skip `len`
     * bytes instead, returning how many were skipped. */
    size_t (*read)(void *ctx, uint8_t *buf, size_t len);
    /* Called often during a decode; may be NULL. */
    void   (*yield)(void *ctx);
    void   *ctx;
    /* Go back to the first byte. May be NULL; the progressive decoder uses
     * it to hand the rare file it cannot sharpen to its simpler path. */
    bool   (*rewind)(void *ctx);
} art_src_t;

/* One pending output pixel: sums of every source pixel landing in it. */
typedef struct {
    uint16_t r, g, b;
    uint16_t n;
} art_acc_t;

/* Colour totals for the progressive path: Cb and Cr at up to half of the
 * largest art size. */
#define ART_CHROMA_MAX  128
typedef struct {
    int16_t sum[2][ART_CHROMA_MAX * ART_CHROMA_MAX];
} art_chroma_t;

/* Everything a decode needs besides the output buffer. ~78 KB: make it
 * static on the device, not a stack variable. The baseline path uses
 * `acc`, the progressive path `chroma`; never both at once. */
typedef struct {
    _Alignas(8) uint8_t pool[ART_POOL_BYTES];
    union {
        art_acc_t    acc[ART_ACC_ROWS][ART_MAX_SIZE];
        art_chroma_t chroma;
    } u;
} art_work_t;

/* What the decode found, for logging. Any pointer may be NULL. */
typedef struct {
    uint16_t src_w, src_h;    /* the JPEG's own dimensions */
    uint8_t  scale;           /* TJpgDec descale used: 0..3 = 1/1..1/8 */
} art_info_t;

/*
 * Decode the JPEG from `src` into `out` (size*size RGB565, row-major).
 * On any failure `out` holds nothing useful — draw the placeholder.
 */
art_result_t art_decode_jpeg(const art_src_t *src, uint16_t *out, int size,
                             art_work_t *work, art_info_t *info);

/*
 * The same, for a progressive JPEG.
 *
 * A progressive file sends the picture as a series of passes, each adding
 * detail to the whole image. Holding the whole image's coefficients would
 * take far more RAM than the chip has, so this never does: every pass is
 * poured straight into screen-sized totals as it is decoded, and only the
 * detail the screen can actually show is decoded at all. Reading stops as
 * soon as it has arrived — typically a fifth to a quarter of the file.
 * Reading stops as soon as everything needed has arrived.
 *
 * Brightness is built at the screen's resolution and colour at half of it
 * (the same ratio the file stores), then interpolated. The low bits some
 * encoders send in late "refinement" passes are not waited for; the
 * values are reconstructed at the middle of the range they stand for.
 *
 * Covers of 1408 px and up, tiny covers, and unusual layouts use a simpler
 * first-pass-only decode (sharp for the big ones), which needs src->rewind.
 */
art_result_t art_decode_jpeg_progressive(const art_src_t *src, uint16_t *out,
                                         int size, art_work_t *work,
                                         art_info_t *info);

/* Short name for a result, for the serial log. */
const char *art_result_name(art_result_t r);

#endif /* ART_H */
