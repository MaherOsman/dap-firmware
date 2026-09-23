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

/* =====================================================================
 * Progressive JPEGs: decode the first scan only.
 *
 * A progressive JPEG stores the picture as a series of passes. The first
 * is (nearly always) the DC pass: one value per 8x8 block, for every
 * colour component — which is exactly an 1/8-size version of the image,
 * the same thing TJpgDec produces at its 1/8 descale. Decoding just that
 * pass needs no coefficient storage at all and reads only the start of the
 * file (34 KB of a 1.28 MB cover, in the one that prompted this).
 *
 * The cost is resolution: a 1200 px cover gives 150 px, which then has to
 * be enlarged. That enlargement is bilinear, so it reads as slightly soft
 * rather than blocky. Covers of 1408 px and up come out at full sharpness.
 * ===================================================================== */

#define DC_INBUF      512
#define DC_MAX_W8     1024      /* 1/8-size width limit: 8192 px covers */
#define DC_MAX_COMP   3

typedef struct {
    uint32_t maxcode[17];       /* one past the last code of each length; 0 = none */
    uint32_t mincode[17];
    uint8_t  valptr[17];
    uint8_t  vals[16];
    bool     present;
} dc_huff_t;

typedef struct {
    uint8_t  id, h, v, tq, td;
    int32_t  pred;
} dc_comp_t;

typedef struct {
    const art_src_t *src;
    uint8_t  *buf;
    size_t    len, pos;
    bool      eof;
    uint32_t  bits;             /* MSB-aligned bit buffer */
    int       nbits;
    bool      marker;           /* ran into a marker inside entropy data */
    uint8_t   marker_id;
} dc_in_t;

static int dc_byte(dc_in_t *in)
{
    if (in->pos >= in->len) {
        in->len = in->src->read(in->src->ctx, in->buf, DC_INBUF);
        in->pos = 0;
        if (in->len == 0) { in->eof = true; return -1; }
    }
    return in->buf[in->pos++];
}

static bool dc_skip(dc_in_t *in, size_t n)
{
    size_t have = in->len - in->pos;
    if (n <= have) { in->pos += n; return true; }
    n -= have;
    in->pos = in->len;
    return in->src->read(in->src->ctx, NULL, n) == n;
}

static int dc_u16(dc_in_t *in)
{
    int a = dc_byte(in), b = dc_byte(in);
    return (a < 0 || b < 0) ? -1 : ((a << 8) | b);
}

static void dc_fill(dc_in_t *in)
{
    while (in->nbits <= 24) {
        int b = 0;
        if (!in->marker) {
            b = dc_byte(in);
            if (b < 0) {
                b = 0;
            } else if (b == 0xFF) {
                int b2 = dc_byte(in);
                while (b2 == 0xFF) b2 = dc_byte(in);   /* fill bytes */
                if (b2 != 0x00) {                      /* a real marker */
                    in->marker = true;
                    in->marker_id = (uint8_t)(b2 < 0 ? 0 : b2);
                    b = 0;
                }
            }
        }
        in->bits |= (uint32_t)b << (24 - in->nbits);
        in->nbits += 8;
    }
}

static uint32_t dc_getbits(dc_in_t *in, int n)
{
    uint32_t v;
    if (n == 0) return 0;
    if (in->nbits < n) dc_fill(in);
    v = in->bits >> (32 - n);
    in->bits <<= n;
    in->nbits -= n;
    return v;
}

static int dc_decode_sym(dc_in_t *in, const dc_huff_t *h)
{
    uint32_t code = 0;
    int l;
    for (l = 1; l <= 16; l++) {
        code = (code << 1) | dc_getbits(in, 1);
        if (h->maxcode[l] != 0u && code < h->maxcode[l]) {
            uint32_t idx = h->valptr[l] + (code - h->mincode[l]);
            return (idx < 16u) ? h->vals[idx] : -1;
        }
    }
    return -1;
}

static bool dc_restart(dc_in_t *in)
{
    in->bits = 0;
    in->nbits = 0;
    if (in->marker) {
        in->marker = false;
        return in->marker_id >= 0xD0 && in->marker_id <= 0xD7;
    }
    for (;;) {                               /* hunt for FF Dn */
        int b = dc_byte(in);
        if (b < 0) return false;
        if (b != 0xFF) continue;
        do { b = dc_byte(in); } while (b == 0xFF);
        if (b >= 0xD0 && b <= 0xD7) return true;
        if (b != 0x00) return false;
    }
}

static int clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

/* A quantised DC value to its block's mean sample. F(0,0) is 8x the block
 * mean, level-shifted by 128. */
static int dc_level(int32_t dc, uint16_t q)
{
    int32_t f = dc * (int32_t)q;
    int32_t m = (f >= 0) ? (f + 4) / 8 : -((-f + 4) / 8);
    return clamp255((int)m + 128);
}

/* Bilinear enlargement, one source row at a time. `prev` and `cur` are
 * cropped rows (side pixels, RGB888); cy is cur's row in the crop. */
typedef struct {
    int oy;                     /* next output row to write */
    uint32_t step;              /* source pixels per output pixel, 16.16 */
} dc_up_t;

static uint32_t dc_src_pos(const dc_up_t *u, int o)
{
    int32_t f = (int32_t)((uint32_t)o * u->step + u->step / 2u) - 32768;
    return (f < 0) ? 0u : (uint32_t)f;
}

static void dc_up_rows(art_job_t *job, dc_up_t *u, const uint8_t *prev,
                       const uint8_t *cur, int cy, bool final)
{
    int side = job->side, n = job->size;

    while (u->oy < n) {
        uint32_t fy = dc_src_pos(u, u->oy);
        int y0 = (int)(fy >> 16), y1 = y0 + 1;
        uint32_t wy = (fy >> 8) & 0xFFu;
        const uint8_t *r0, *r1;
        uint16_t *o = job->out + (size_t)u->oy * (size_t)n;
        int ox;

        if (y1 > side - 1) y1 = side - 1;
        if (y0 > side - 1) y0 = side - 1;
        if (!final && y1 > cy) return;       /* needs a row not yet here */
        r0 = (y0 == cy || prev == NULL) ? cur : prev;
        r1 = (y1 == cy || prev == NULL) ? cur : prev;
        if (final) { r0 = cur; r1 = cur; }

        for (ox = 0; ox < n; ox++) {
            uint32_t fx = dc_src_pos(u, ox);
            int x0 = (int)(fx >> 16), x1 = x0 + 1;
            uint32_t wx = (fx >> 8) & 0xFFu;
            uint32_t c[3];
            int k;
            if (x1 > side - 1) x1 = side - 1;
            if (x0 > side - 1) x0 = side - 1;
            for (k = 0; k < 3; k++) {
                uint32_t a = r0[x0 * 3 + k] * (256u - wx) + r0[x1 * 3 + k] * wx;
                uint32_t b = r1[x0 * 3 + k] * (256u - wx) + r1[x1 * 3 + k] * wx;
                c[k] = (a * (256u - wy) + b * wy + 32768u) >> 16;
            }
            o[ox] = to565(c[0], c[1], c[2], ox, u->oy);
        }
        u->oy++;
    }
}

art_result_t art_decode_jpeg_progressive(const art_src_t *src, uint16_t *out,
                                         int size, art_work_t *work,
                                         art_info_t *info)
{
    /* Everything lives in the TJpgDec pool, unused on this path. */
    uint8_t *pool = work != NULL ? work->pool : NULL;
    dc_in_t in;
    dc_huff_t huff[4];
    uint16_t q0[4] = { 1, 1, 1, 1 };
    dc_comp_t comp[DC_MAX_COMP];
    int ncomp = 0, width = 0, height = 0, hmax = 1, vmax = 1;
    uint32_t restart = 0;
    bool adobe_rgb = false, have_sof = false;
    art_job_t job;
    dc_up_t up;
    uint8_t *band, *rowa, *rowb;
    int sw, sh, mcux, mcuy, my, mx, i, al = 0;
    size_t band_bytes;

    if (info != NULL) memset(info, 0, sizeof(*info));
    if (src == NULL || src->read == NULL || out == NULL || work == NULL ||
        size < 1 || size > ART_MAX_SIZE) {
        return ART_ERR_ARG;
    }

    memset(&in, 0, sizeof(in));
    memset(huff, 0, sizeof(huff));
    memset(comp, 0, sizeof(comp));
    in.src = src;
    in.buf = pool;

    /* ---- headers, up to the first scan ---- */
    if (dc_byte(&in) != 0xFF || dc_byte(&in) != 0xD8) {
        return in.eof ? ART_ERR_READ : ART_ERR_FORMAT;
    }
    for (;;) {
        int m, len;
        do { m = dc_byte(&in); } while (m >= 0 && m != 0xFF);
        do { m = dc_byte(&in); } while (m == 0xFF);
        if (m < 0) return ART_ERR_READ;
        if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) continue;
        if (m == 0xD9) return ART_ERR_FORMAT;
        len = dc_u16(&in);
        if (len < 2) return in.eof ? ART_ERR_READ : ART_ERR_FORMAT;
        len -= 2;

        if (m == 0xDB) {                                   /* DQT */
            while (len > 0) {
                int pq_tq = dc_byte(&in), t = pq_tq & 3, n16 = pq_tq >> 4;
                int first = n16 ? dc_u16(&in) : dc_byte(&in);
                if (pq_tq < 0 || first < 0) return ART_ERR_READ;
                q0[t] = (uint16_t)first;       /* zigzag 0 is the DC step */
                if (!dc_skip(&in, (size_t)(n16 ? 126 : 63))) return ART_ERR_READ;
                len -= 1 + (n16 ? 128 : 64);
            }
        } else if (m == 0xC4) {                            /* DHT */
            while (len > 0) {
                int tc_th = dc_byte(&in), total = 0, l;
                uint8_t counts[16];
                for (l = 0; l < 16; l++) {
                    int c = dc_byte(&in);
                    if (c < 0) return ART_ERR_READ;
                    counts[l] = (uint8_t)c;
                    total += c;
                }
                if (tc_th < 0) return ART_ERR_READ;
                len -= 17 + total;
                if ((tc_th >> 4) == 0) {                   /* DC table */
                    dc_huff_t *h = &huff[tc_th & 3];
                    uint32_t code = 0;
                    int k = 0;
                    if (total > 16) return ART_ERR_FORMAT;
                    memset(h, 0, sizeof(*h));
                    for (l = 0; l < total; l++) {
                        int v = dc_byte(&in);
                        if (v < 0) return ART_ERR_READ;
                        h->vals[l] = (uint8_t)v;
                    }
                    for (l = 1; l <= 16; l++) {
                        h->valptr[l] = (uint8_t)k;
                        h->mincode[l] = code;
                        code += counts[l - 1];
                        k += counts[l - 1];
                        h->maxcode[l] = counts[l - 1] ? code : 0u;
                        code <<= 1;
                    }
                    h->present = true;
                } else if (!dc_skip(&in, (size_t)total)) {
                    return ART_ERR_READ;
                }
            }
        } else if (m == 0xC2) {                            /* SOF2 */
            int p = dc_byte(&in);
            height = dc_u16(&in);
            width = dc_u16(&in);
            ncomp = dc_byte(&in);
            if (p != 8) return ART_ERR_UNSUPPORTED;
            if (ncomp != 1 && ncomp != 3) return ART_ERR_UNSUPPORTED;
            for (i = 0; i < ncomp; i++) {
                int id = dc_byte(&in), hv = dc_byte(&in), tq = dc_byte(&in);
                if (tq < 0) return ART_ERR_READ;
                comp[i].id = (uint8_t)id;
                comp[i].h = (uint8_t)(hv >> 4);
                comp[i].v = (uint8_t)(hv & 15);
                comp[i].tq = (uint8_t)(tq & 3);
                if (comp[i].h < 1 || comp[i].h > 4 || comp[i].v < 1 ||
                    comp[i].v > 4) {
                    return ART_ERR_FORMAT;
                }
            }
            if (ncomp == 1) comp[0].h = comp[0].v = 1;  /* see T.81 A.2.2 */
            for (i = 0; i < ncomp; i++) {
                if (comp[i].h > hmax) hmax = comp[i].h;
                if (comp[i].v > vmax) vmax = comp[i].v;
            }
            len -= 6 + 3 * ncomp;
            if (len != 0) return ART_ERR_FORMAT;
            have_sof = true;
        } else if ((m >= 0xC0 && m <= 0xCF) && m != 0xC4 && m != 0xC8 &&
                   m != 0xCC) {
            return ART_ERR_UNSUPPORTED;          /* baseline etc: wrong path */
        } else if (m == 0xDD) {                            /* DRI */
            int ri = dc_u16(&in);
            if (ri < 0) return ART_ERR_READ;
            restart = (uint32_t)ri;
            len -= 2;
            if (len > 0 && !dc_skip(&in, (size_t)len)) return ART_ERR_READ;
        } else if (m == 0xEE && len >= 12) {               /* Adobe */
            uint8_t a[12];
            for (i = 0; i < 12; i++) {
                int b = dc_byte(&in);
                if (b < 0) return ART_ERR_READ;
                a[i] = (uint8_t)b;
            }
            if (memcmp(a, "Adobe", 5) == 0 && a[11] == 0u) adobe_rgb = true;
            if (!dc_skip(&in, (size_t)(len - 12))) return ART_ERR_READ;
        } else if (m == 0xDA) {                            /* SOS */
            int ns = dc_byte(&in), ss, se, ahal;
            if (!have_sof) return ART_ERR_FORMAT;
            if (ns != ncomp) return ART_ERR_UNSUPPORTED;   /* not interleaved */
            for (i = 0; i < ns; i++) {
                int cs = dc_byte(&in), t = dc_byte(&in), k;
                if (t < 0) return ART_ERR_READ;
                for (k = 0; k < ncomp; k++) {
                    if (comp[k].id == cs) comp[k].td = (uint8_t)((t >> 4) & 3);
                }
            }
            ss = dc_byte(&in);
            se = dc_byte(&in);
            ahal = dc_byte(&in);
            if (ahal < 0) return ART_ERR_READ;
            if (ss != 0 || se != 0 || (ahal >> 4) != 0) {
                return ART_ERR_UNSUPPORTED;   /* first scan is not DC-first */
            }
            al = ahal & 15;
            break;
        } else if (len > 0 && !dc_skip(&in, (size_t)len)) {
            return ART_ERR_READ;
        }
    }

    for (i = 0; i < ncomp; i++) {
        if (!huff[comp[i].td].present) return ART_ERR_FORMAT;
    }
    if (info != NULL) {
        info->src_w = (uint16_t)width;
        info->src_h = (uint16_t)height;
        info->scale = 3;
    }
    if (width < 1 || height < 1) return ART_ERR_FORMAT;

    /* ---- geometry: the 1/8-size image, then the crop, as the baseline
     * path does it ---- */
    sw = (width + 7) / 8;
    sh = (height + 7) / 8;
    mcux = (width + 8 * hmax - 1) / (8 * hmax);
    mcuy = (height + 8 * vmax - 1) / (8 * vmax);
    if (sw > DC_MAX_W8) return ART_ERR_TOO_BIG;

    memset(&job, 0, sizeof(job));
    job.src = src;
    job.work = work;
    job.out = out;
    job.size = size;
    job.sw = sw;
    job.sh = sh;
    job.side = (sw < sh) ? sw : sh;
    job.x0 = (sw - job.side) / 2;
    job.y0 = (sh - job.side) / 2;
    job.enlarge = (job.side < size);
    if (job.side > size * ART_MAX_RATIO) return ART_ERR_TOO_BIG;

    band_bytes = (size_t)vmax * (size_t)(mcux * hmax) * 3u;
    if (DC_INBUF + band_bytes + 2u * (size_t)size * 3u > sizeof(work->pool)) {
        return ART_ERR_TOO_BIG;
    }
    band = pool + DC_INBUF;
    rowa = band + band_bytes;
    rowb = rowa + (size_t)size * 3u;
    memset(work->acc, 0, sizeof(work->acc));
    memset(&up, 0, sizeof(up));
    up.step = (uint32_t)(((uint32_t)job.side << 16) / (uint32_t)size);

    /* ---- the DC scan ---- */
    {
        uint32_t mcus = 0;
        const uint8_t *prev = NULL;
        uint8_t *cur = rowa;
        int cy_seen = -1;

        for (my = 0; my < mcuy; my++) {
            for (mx = 0; mx < mcux; mx++) {
                int vals[DC_MAX_COMP][16];
                int px, py;

                if (restart != 0u && mcus != 0u && (mcus % restart) == 0u) {
                    if (!dc_restart(&in)) return ART_ERR_FORMAT;
                    for (i = 0; i < ncomp; i++) comp[i].pred = 0;
                }
                mcus++;

                for (i = 0; i < ncomp; i++) {
                    int b, nb = comp[i].h * comp[i].v;
                    for (b = 0; b < nb; b++) {
                        int s = dc_decode_sym(&in, &huff[comp[i].td]);
                        int32_t diff = 0;
                        if (s < 0 || s > 15) return ART_ERR_FORMAT;
                        if (s > 0) {
                            diff = (int32_t)dc_getbits(&in, s);
                            if (diff < (1 << (s - 1))) diff += 1 - (1 << s);
                        }
                        comp[i].pred += diff;
                        vals[i][b] = dc_level(comp[i].pred * (1 << al),
                                              q0[comp[i].tq]);
                    }
                }

                for (py = 0; py < vmax; py++) {
                    for (px = 0; px < hmax; px++) {
                        int x = mx * hmax + px, rgb[3], k;
                        uint8_t *d = band + ((size_t)py * (size_t)(mcux * hmax) +
                                             (size_t)x) * 3u;
                        for (k = 0; k < ncomp; k++) {
                            int bx = px * comp[k].h / hmax;
                            int by = py * comp[k].v / vmax;
                            rgb[k] = vals[k][by * comp[k].h + bx];
                        }
                        if (ncomp == 1) {
                            d[0] = d[1] = d[2] = (uint8_t)rgb[0];
                        } else if (adobe_rgb) {
                            d[0] = (uint8_t)rgb[0];
                            d[1] = (uint8_t)rgb[1];
                            d[2] = (uint8_t)rgb[2];
                        } else {
                            /* JFIF YCbCr -> RGB, 16.16 fixed point */
                            int yy = rgb[0], cb = rgb[1] - 128, cr = rgb[2] - 128;
                            d[0] = (uint8_t)clamp255(yy + ((91881 * cr + 32768) >> 16));
                            d[1] = (uint8_t)clamp255(yy - ((22554 * cb + 46802 * cr + 32768) >> 16));
                            d[2] = (uint8_t)clamp255(yy + ((116130 * cb + 32768) >> 16));
                        }
                    }
                }
            }
            if (in.eof) return ART_ERR_READ;

            /* This band of rows is done: hand each on. */
            for (i = 0; i < vmax; i++) {
                int y = my * vmax + i;
                const uint8_t *row = band + (size_t)i * (size_t)(mcux * hmax) * 3u;
                if (y >= sh) break;
                if (!job.enlarge) {
                    JRECT r;
                    r.left = 0;
                    r.right = (uint16_t)(sw - 1);
                    r.top = (uint16_t)y;
                    r.bottom = (uint16_t)y;
                    /* One row per call, so the band being wider than sw
                     * (MCU padding) does not matter: only `sw` pixels of
                     * it are read. */
                    if (!shrink_block(&job, row, &r)) return job.err;
                } else {
                    int cy = y - job.y0;
                    if (cy < 0 || cy >= job.side) continue;
                    memcpy(cur, row + (size_t)job.x0 * 3u, (size_t)job.side * 3u);
                    dc_up_rows(&job, &up, prev, cur, cy, false);
                    prev = cur;
                    cur = (cur == rowa) ? rowb : rowa;
                    cy_seen = cy;
                }
            }
            if (src->yield != NULL) src->yield(src->ctx);
        }

        if (job.enlarge) {
            if (cy_seen < 0) return ART_ERR_FORMAT;
            dc_up_rows(&job, &up, NULL, prev, cy_seen, true);
        } else {
            while (job.flushed < size) {
                flush_row(&job, job.flushed);
                job.flushed++;
            }
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
