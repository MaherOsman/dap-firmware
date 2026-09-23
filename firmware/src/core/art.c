/* art.c — JPEG cover to square RGB565. See art.h for the approach. */

#include "art.h"

#include <string.h>

#include "../../third_party/tjpgd/tjpgd.h"

/* The biggest ratio between the cropped square and the output that the
 * 16-bit accumulators can average without overflowing: a box of up to
 * 16 x 16 source pixels, 256 * 255 < 65536. Covers this large (over 20,000
 * pixels on a side even after 1/8 descale) are not real album art. */
#define ART_MAX_RATIO   16

typedef struct {
    const art_src_t *src;
    art_work_t      *work;
    uint16_t        *out;
    int              size;     /* output side */

    int  sw, sh;               /* decoded (descaled) image size */
    int  side;                 /* cropped square side, in decoded pixels */
    int  x0, y0;               /* crop origin, in decoded pixels */
    bool enlarge;              /* side < size: repeat pixels, no averaging */

    int  flushed;              /* output rows [0, flushed) are final */
    art_result_t err;          /* set when the output callback bails */
} art_job_t;

/* RGB565 keeps only 32 levels of red and blue, which turns the smooth
 * gradients common in cover art into visible stripes. A 4x4 ordered dither
 * adds a position-dependent nudge below the bits being dropped, so the
 * stripes become a fine even texture the eye averages back into a gradient.
 * Deterministic, so a redraw never shimmers. */
static const uint8_t BAYER4[4][4] = {
    {  0,  8,  2, 10 },
    { 12,  4, 14,  6 },
    {  3, 11,  1,  9 },
    { 15,  7, 13,  5 },
};

static uint16_t to565(uint32_t r, uint32_t g, uint32_t b, int x, int y)
{
    uint32_t t = BAYER4[y & 3][x & 3];      /* 0..15 */
    r += t >> 1;                             /* red, blue lose 3 bits: 0..7 */
    g += t >> 2;                             /* green loses 2 bits:    0..3 */
    b += t >> 1;
    if (r > 255u) r = 255u;
    if (g > 255u) g = 255u;
    if (b > 255u) b = 255u;
    return (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
}

/* ------------------------------------------------------------ input */

static size_t infunc(JDEC *jd, uint8_t *buf, size_t len)
{
    art_job_t *job = (art_job_t *)jd->device;
    return job->src->read(job->src->ctx, buf, len);
}

/* ------------------------------------------------------ downscaling */

static art_acc_t *acc_row(art_job_t *job, int oy)
{
    return job->work->acc[oy % ART_ACC_ROWS];
}

/* Output row `oy` is final: average it into the output and clear its slot
 * for the row that will reuse it. */
static void flush_row(art_job_t *job, int oy)
{
    art_acc_t *a = acc_row(job, oy);
    uint16_t *o = job->out + (size_t)oy * (size_t)job->size;
    int x;

    for (x = 0; x < job->size; x++) {
        uint32_t n = a[x].n;
        if (n != 0u) {
            o[x] = to565((a[x].r + n / 2u) / n, (a[x].g + n / 2u) / n,
                         (a[x].b + n / 2u) / n, x, oy);
        } else {
            /* Cannot happen when shrinking — every output pixel owns at
             * least one source pixel — but never leave garbage behind. */
            o[x] = (x > 0) ? o[x - 1] : (oy > 0 ? o[x - job->size] : 0u);
        }
    }
    memset(a, 0, sizeof(art_acc_t) * (size_t)job->size);
}

/* Last output row whose every source row lies at or above crop row `cy`. */
static int rows_complete_through(const art_job_t *job, int cy)
{
    /* Output row oy takes crop rows cy with cy * size / side == oy, the
     * last of which is ((oy + 1) * side - 1) / size. Invert that. */
    return ((cy + 1) * job->size) / job->side - 1;
}

static bool shrink_block(art_job_t *job, const uint8_t *px, const JRECT *r)
{
    int bw = r->right - r->left + 1;
    int y;

    for (y = r->top; y <= r->bottom; y++) {
        int cy = y - job->y0;
        int oy, x;
        art_acc_t *a;
        const uint8_t *p;

        if (cy < 0 || cy >= job->side) continue;
        oy = cy * job->size / job->side;
        if (oy - job->flushed >= ART_ACC_ROWS) {
            job->err = ART_ERR_MEM;     /* band taller than the ring */
            return false;
        }
        a = acc_row(job, oy);
        p = px + (size_t)(y - r->top) * (size_t)bw * 3u;

        for (x = r->left; x <= r->right; x++, p += 3) {
            int cx = x - job->x0;
            art_acc_t *e;
            if (cx < 0 || cx >= job->side) continue;
            e = &a[cx * job->size / job->side];
            e->r = (uint16_t)(e->r + p[0]);
            e->g = (uint16_t)(e->g + p[1]);
            e->b = (uint16_t)(e->b + p[2]);
            e->n++;
        }
    }

    /* The right-most block of a band closes that band: every output row
     * whose source rows have all arrived can be written out. */
    if (r->right == job->sw - 1) {
        int cy = r->bottom - job->y0;
        int last;
        if (cy >= job->side) cy = job->side - 1;
        last = (cy >= 0) ? rows_complete_through(job, cy) : -1;
        if (last >= job->size) last = job->size - 1;
        while (job->flushed <= last) {
            flush_row(job, job->flushed);
            job->flushed++;
        }
    }
    return true;
}

/* ------------------------------------------------------- enlarging */

/* A cover smaller than the art square: each output pixel copies the source
 * pixel it falls on. Blocks arrive in any order across a band, so each one
 * writes exactly the output pixels whose source lies inside it. */
static void enlarge_block(art_job_t *job, const uint8_t *px, const JRECT *r)
{
    int bw = r->right - r->left + 1;
    int s = job->side, n = job->size;
    int ct = r->top - job->y0, cb = r->bottom - job->y0;
    int cl = r->left - job->x0, cr = r->right - job->x0;
    int oy0, oy1, ox0, ox1, oy, ox;

    if (cb < 0 || ct >= s || cr < 0 || cl >= s) return;
    if (ct < 0) ct = 0;
    if (cl < 0) cl = 0;
    if (cb >= s) cb = s - 1;
    if (cr >= s) cr = s - 1;

    /* Output rows oy with ct <= oy * s / n <= cb. */
    oy0 = (ct * n + s - 1) / s;
    oy1 = ((cb + 1) * n + s - 1) / s;
    ox0 = (cl * n + s - 1) / s;
    ox1 = ((cr + 1) * n + s - 1) / s;
    if (oy1 > n) oy1 = n;
    if (ox1 > n) ox1 = n;

    for (oy = oy0; oy < oy1; oy++) {
        int y = oy * s / n + job->y0;
        const uint8_t *row = px + (size_t)(y - r->top) * (size_t)bw * 3u;
        uint16_t *o = job->out + (size_t)oy * (size_t)n;
        for (ox = ox0; ox < ox1; ox++) {
            const uint8_t *p = row + (size_t)(ox * s / n + job->x0 - r->left) * 3u;
            o[ox] = to565(p[0], p[1], p[2], ox, oy);
        }
    }
}

/* ------------------------------------------------------------ output */

static int outfunc(JDEC *jd, void *bitmap, JRECT *rect)
{
    art_job_t *job = (art_job_t *)jd->device;
    bool ok = true;

    if (job->enlarge) enlarge_block(job, (const uint8_t *)bitmap, rect);
    else              ok = shrink_block(job, (const uint8_t *)bitmap, rect);

    if (job->src->yield != NULL) job->src->yield(job->src->ctx);
    return ok ? 1 : 0;
}

/* ------------------------------------------------------------- API */

static art_result_t map_jresult(JRESULT r)
{
    switch (r) {
    case JDR_OK:   return ART_OK;
    case JDR_INP:  return ART_ERR_READ;
    case JDR_MEM1:
    case JDR_MEM2: return ART_ERR_MEM;
    case JDR_PAR:  return ART_ERR_ARG;
    case JDR_FMT1: return ART_ERR_FORMAT;
    case JDR_FMT2:
    case JDR_FMT3: return ART_ERR_UNSUPPORTED;
    case JDR_INTR:
    default:       return ART_ERR_FORMAT;
    }
}

art_result_t art_decode_jpeg(const art_src_t *src, uint16_t *out, int size,
                             art_work_t *work, art_info_t *info)
{
    JDEC jd;
    art_job_t job;
    JRESULT jr;
    uint8_t scale;
    int shorter;

    if (info != NULL) memset(info, 0, sizeof(*info));
    if (src == NULL || src->read == NULL || out == NULL || work == NULL ||
        size < 1 || size > ART_MAX_SIZE) {
        return ART_ERR_ARG;
    }

    memset(&job, 0, sizeof(job));
    job.src = src;
    job.work = work;
    job.out = out;
    job.size = size;
    job.err = ART_OK;

    jr = jd_prepare(&jd, infunc, work->pool, sizeof(work->pool), &job);
    if (jr != JDR_OK) return map_jresult(jr);

    if (info != NULL) {
        info->src_w = jd.width;
        info->src_h = jd.height;
    }
    if (jd.width == 0u || jd.height == 0u) return ART_ERR_FORMAT;

    /* The biggest descale that still leaves at least `size` pixels on the
     * short side. Decoding less is the cheapest speed-up there is. */
    shorter = (jd.width < jd.height) ? jd.width : jd.height;
    scale = 0;
    while (scale < 3u && (shorter >> (scale + 1u)) >= size) scale++;

    job.sw = jd.width >> scale;
    job.sh = jd.height >> scale;
    job.side = (job.sw < job.sh) ? job.sw : job.sh;
    job.x0 = (job.sw - job.side) / 2;
    job.y0 = (job.sh - job.side) / 2;
    job.enlarge = (job.side < size);
    if (info != NULL) info->scale = scale;

    if (job.side < 1) return ART_ERR_FORMAT;
    if (job.side > size * ART_MAX_RATIO) return ART_ERR_TOO_BIG;

    memset(work->acc, 0, sizeof(work->acc));

    jr = jd_decomp(&jd, outfunc, scale);
    if (jr == JDR_INTR && job.err != ART_OK) return job.err;
    if (jr != JDR_OK) return map_jresult(jr);

    /* Anything the last band left pending. */
    if (!job.enlarge) {
        while (job.flushed < size) {
            flush_row(&job, job.flushed);
            job.flushed++;
        }
    }
    return ART_OK;
}

const char *art_result_name(art_result_t r)
{
    switch (r) {
    case ART_OK:              return "ok";
    case ART_ERR_ARG:         return "bad argument";
    case ART_ERR_READ:        return "read error";
    case ART_ERR_FORMAT:      return "not a valid JPEG";
    case ART_ERR_UNSUPPORTED: return "unsupported JPEG (progressive?)";
    case ART_ERR_TOO_BIG:     return "image too large";
    case ART_ERR_MEM:         return "out of workspace";
    default:                  return "?";
    }
}
