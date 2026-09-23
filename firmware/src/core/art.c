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
    return job->work->u.acc[oy % ART_ACC_ROWS];
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

    memset(work->u.acc, 0, sizeof(work->u.acc));

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

static art_result_t prog_dc_only(const art_src_t *src, uint16_t *out,
                                 int size, art_work_t *work, art_info_t *info)
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
    memset(work->u.acc, 0, sizeof(work->u.acc));
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

/* =====================================================================
 * Progressive JPEGs, sharp: stream every pass into screen-sized totals.
 *
 * The IDCT is linear, and so is area-averaging down to the screen. So the
 * contribution of each coefficient, whenever its pass happens to deliver
 * it, can be added straight into the output pixels it lands on — no
 * per-block storage, no matter how the encoder ordered its passes.
 *
 * A block is reconstructed at k x k (k = 2, 4 or 8) using only its lowest
 * k x k coefficients: the same reduced IDCT libjpeg uses for scaled
 * decoding. Luma goes into an int16 total per output pixel (the output
 * buffer itself, converted in place at the end); chroma keeps its DC
 * only, per chroma block, and is interpolated at the end.
 * ===================================================================== */

#define PS_FALLBACK  ((art_result_t)100)   /* internal: use prog_dc_only */

typedef struct {
    uint32_t maxcode[17];
    uint32_t mincode[17];
    uint16_t valptr[17];
    uint8_t  vals[256];
    bool     present;
} hf_t;

#define PS_MAP_MAX (2 * ART_MAX_SIZE + 2)

typedef struct {
    uint8_t  inbuf[DC_INBUF];
    hf_t     dc[4], ac[4];
    uint16_t qt[4][64];                     /* zigzag order, as sent */
    uint16_t map[2][PS_MAP_MAX];            /* plane coord -> total index */
    uint8_t  cnt[2][ART_MAX_SIZE];          /* plane coords per total */
    int16_t  sx0[ART_MAX_SIZE], sy0[ART_MAX_SIZE];   /* chroma sampling */
    uint8_t  sxw[ART_MAX_SIZE], syw[ART_MAX_SIZE];
} ps_pool_t;

typedef char ps_pool_fits[(sizeof(ps_pool_t) <= ART_POOL_BYTES) ? 1 : -1];

typedef struct {
    uint8_t  id, h, v, tq, td, ta;
    int      bw, bh;            /* block grid (non-interleaved scans) */
    int32_t  pred;
    bool     dc_done;
    uint64_t coded;             /* AC coefficients first-coded (zigzag bits) */
} ps_comp_t;

/* One resolution level being accumulated into: luma at the screen size,
 * chroma at half of it. Each block is reconstructed at k x k samples from
 * its lowest k x k coefficients (libjpeg's reduced IDCT); every sample is
 * added into the total it lands on. */
typedef struct {
    int16_t *acc[2];            /* totals; chroma has Cb and Cr */
    int      n;                 /* totals per side */
    int      k;
    const int16_t *basis;
    int      side, x0, y0;      /* crop, in plane samples */
    uint16_t *map;
    uint8_t  *cnt;
    uint64_t need;              /* AC coefficients worth decoding */
} ps_plane_t;

/* Reduced-IDCT basis, 4096 = 1.0: B[u][i] = C(u) cos((2i+1)u pi / 2k),
 * C(0) = 1/sqrt(2). A block's k x k sample (i, j) is
 * (1/4) sum F(v,u) B[u][i] B[v][j] over u, v < k. (Averaging a full 8x8
 * IDCT exactly instead was measured: 0.1 dB sharper for 8x the reading.) */
static const int16_t BASIS2[2 * 2] = { 2896, 2896, 2896, -2896 };
static const int16_t BASIS4[4 * 4] = {
    2896, 2896, 2896, 2896,  3784, 1567, -1567, -3784,
    2896, -2896, -2896, 2896,  1567, -3784, 3784, -1567 };
static const int16_t BASIS8[8 * 8] = {
    2896, 2896, 2896, 2896, 2896, 2896, 2896, 2896,
    4017, 3406, 2276, 799, -799, -2276, -3406, -4017,
    3784, 1567, -1567, -3784, -3784, -1567, 1567, 3784,
    3406, -799, -4017, -2276, 2276, 4017, 799, -3406,
    2896, -2896, -2896, 2896, 2896, -2896, -2896, 2896,
    2276, -4017, 799, 3406, -3406, -799, 4017, -2276,
    1567, -3784, 3784, -1567, -1567, 3784, -3784, 1567,
    799, -2276, 3406, -4017, 4017, -3406, 2276, -799 };

/* Zigzag position -> natural (row * 8 + col). */
static const uint8_t ZZ_NATURAL[64] = {
     0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63 };

typedef struct {
    dc_in_t    in;
    ps_pool_t *pp;
    int        size;
    ps_plane_t pl[2];           /* 0 luma, 1 chroma */
    int        width, height, ncomp, hmax, vmax, mcux, mcuy;
    ps_comp_t  comp[DC_MAX_COMP];
    uint32_t   restart;
    int        pending;         /* marker already read, or -1 */
    bool       adobe_rgb;
} ps_t;

static void hf_build(hf_t *h, const uint8_t counts[16])
{
    uint32_t code = 0;
    int l, k = 0;
    for (l = 1; l <= 16; l++) {
        h->valptr[l] = (uint16_t)k;
        h->mincode[l] = code;
        code += counts[l - 1];
        k += counts[l - 1];
        h->maxcode[l] = counts[l - 1] ? code : 0u;
        code <<= 1;
    }
    h->present = true;
}

static int hf_decode(dc_in_t *in, const hf_t *h)
{
    uint32_t code = 0;
    int l;
    for (l = 1; l <= 16; l++) {
        code = (code << 1) | dc_getbits(in, 1);
        if (h->maxcode[l] != 0u && code < h->maxcode[l]) {
            uint32_t idx = h->valptr[l] + (code - h->mincode[l]);
            return (idx < 256u) ? h->vals[idx] : -1;
        }
    }
    return -1;
}

static int32_t extend(uint32_t v, int s)
{
    int32_t x = (int32_t)v;
    return (s > 0 && x < (1 << (s - 1))) ? x + 1 - (1 << s) : x;
}

/* One dequantised coefficient of block (bx, by) of component ci. */
static void ps_add(ps_t *P, int ci, int bx, int by, int nat, int32_t f)
{
    ps_plane_t *pl = &P->pl[ci == 0 ? 0 : 1];
    int16_t *acc;
    int row = nat >> 3, col = nat & 7, i, j, k = pl->k;
    const int16_t *bu, *bv;

    if (f == 0 || row >= k || col >= k) return;
    if (bx >= P->comp[ci].bw || by >= P->comp[ci].bh) return;   /* padding */
    acc = pl->acc[ci == 0 ? 0 : ci - 1];
    bu = pl->basis + col * k;
    bv = pl->basis + row * k;
    for (j = 0; j < k; j++) {
        int cy = by * k + j - pl->y0;
        int64_t fv;
        int16_t *arow;
        if (cy < 0 || cy >= pl->side) continue;
        fv = (int64_t)f * bv[j];
        arow = acc + (size_t)pl->map[cy] * (size_t)pl->n;
        for (i = 0; i < k; i++) {
            int cx = bx * k + i - pl->x0;
            if (cx < 0 || cx >= pl->side) continue;
            /* 4 x the sample: (1/4) f Bu Bv / 2^24, times 4 */
            arow[pl->map[cx]] = (int16_t)(arow[pl->map[cx]] +
                (int32_t)((fv * bu[i] + (1 << 23)) >> 24));
        }
    }
}

/* The next marker: one already met inside entropy data, or the next
 * FF xx in the stream (skipping whatever precedes it — which is how a
 * scan that is not needed gets passed over). */
static int ps_marker(ps_t *P)
{
    int m;
    if (P->pending >= 0) { m = P->pending; P->pending = -1; return m; }
    if (P->in.marker) { P->in.marker = false; return P->in.marker_id; }
    for (;;) {
        m = dc_byte(&P->in);
        if (m < 0) return -1;
        if (m != 0xFF) continue;
        do { m = dc_byte(&P->in); } while (m == 0xFF);
        if (m < 0) return -1;
        if (m == 0x00 || (m >= 0xD0 && m <= 0xD7)) continue;   /* in data */
        return m;
    }
}

static bool ps_restart_due(const ps_t *P, uint32_t n)
{
    return P->restart != 0u && n != 0u && (n % P->restart) == 0u;
}

/* Passes send values with their low `al` bits cut off. The true value is
 * then somewhere in a range 2^al wide; reconstructing at the bottom of
 * that range biases every value one way (measured: a visible brightness
 * shift). These put it in the middle, in dequantised units so the half
 * step is not lost to integer rounding.
 *
 * DC: the cut is an arithmetic shift, so the range is [v*2^al, +2^al-1].
 * Each refinement bit then picks a half; the corrections telescope to the
 * exact value once the last refinement (al = 0) is in. */
static int32_t dc_first(int32_t v, int al, int32_t q)
{
    return v * (1 << al) * q + (((1 << al) - 1) * q) / 2;
}

static int32_t dc_refine(uint32_t bit, int al, int32_t q)
{
    return (int32_t)(bit << al) * q - ((1 << al) * q) / 2;
}

static art_result_t ps_dc_block(ps_t *P, int ci, int bx, int by, int ah, int al)
{
    ps_comp_t *c = &P->comp[ci];
    int32_t q = (int32_t)P->pp->qt[c->tq][0];
    if (ah == 0) {
        int s = hf_decode(&P->in, &P->pp->dc[c->td]);
        if (s < 0 || s > 15) return ART_ERR_FORMAT;
        c->pred += extend(dc_getbits(&P->in, s), s);
        ps_add(P, ci, bx, by, 0, dc_first(c->pred, al, q));
    } else {
        ps_add(P, ci, bx, by, 0, dc_refine(dc_getbits(&P->in, 1), al, q));
    }
    return ART_OK;
}

static art_result_t ps_dc_scan(ps_t *P, const int *sel, int ns, int ah, int al)
{
    uint32_t n = 0;
    int i, mx, my, b;
    art_result_t r;

    for (i = 0; i < ns; i++) P->comp[sel[i]].pred = 0;

    if (ns == 1) {                             /* one component alone */
        ps_comp_t *c = &P->comp[sel[0]];
        int bx, by;
        for (by = 0; by < c->bh; by++) {
            for (bx = 0; bx < c->bw; bx++, n++) {
                if (ps_restart_due(P, n)) {
                    if (!dc_restart(&P->in)) return ART_ERR_FORMAT;
                    c->pred = 0;
                }
                r = ps_dc_block(P, sel[0], bx, by, ah, al);
                if (r != ART_OK) return r;
            }
            if (P->in.eof) return ART_ERR_READ;
        }
        return ART_OK;
    }

    for (my = 0; my < P->mcuy; my++) {         /* interleaved, by MCU */
        for (mx = 0; mx < P->mcux; mx++, n++) {
            if (ps_restart_due(P, n)) {
                if (!dc_restart(&P->in)) return ART_ERR_FORMAT;
                for (i = 0; i < ns; i++) P->comp[sel[i]].pred = 0;
            }
            for (i = 0; i < ns; i++) {
                ps_comp_t *c = &P->comp[sel[i]];
                for (b = 0; b < c->h * c->v; b++) {
                    r = ps_dc_block(P, sel[i], mx * c->h + b % c->h,
                                    my * c->v + b / c->h, ah, al);
                    if (r != ART_OK) return r;
                }
            }
        }
        if (P->in.eof) return ART_ERR_READ;
    }
    return ART_OK;
}

/* First pass of an AC band for one component. Every symbol is decoded
 * (the bitstream leaves no choice); only coefficients that can show at
 * the plane's resolution are added. */
static art_result_t ps_ac_first(ps_t *P, int ci, int ss, int se, int al)
{
    ps_comp_t *c = &P->comp[ci];
    const hf_t *h = &P->pp->ac[c->ta];
    uint64_t need = P->pl[ci == 0 ? 0 : 1].need;
    uint32_t n = 0, eobrun = 0;
    int bx, by;

    for (by = 0; by < c->bh; by++) {
        for (bx = 0; bx < c->bw; bx++, n++) {
            int z;
            if (ps_restart_due(P, n)) {
                if (!dc_restart(&P->in)) return ART_ERR_FORMAT;
                eobrun = 0;
            }
            if (eobrun > 0) { eobrun--; continue; }
            for (z = ss; z <= se; ) {
                int rs = hf_decode(&P->in, h), r, sz;
                if (rs < 0) return ART_ERR_FORMAT;
                r = rs >> 4;
                sz = rs & 15;
                if (sz == 0) {
                    if (r < 15) {                /* end-of-band run */
                        eobrun = (1u << r) - 1u;
                        if (r > 0) eobrun += dc_getbits(&P->in, r);
                        break;
                    }
                    z += 16;                     /* ZRL */
                    continue;
                }
                z += r;
                if (z > 63) return ART_ERR_FORMAT;
                {
                    /* AC: magnitude cut, sign kept: |v| stands for
                     * [|v|*2^al, +2^al-1]. */
                    int32_t v = extend(dc_getbits(&P->in, sz), sz);
                    int32_t q = (int32_t)P->pp->qt[c->tq][z];
                    int32_t mag = (v > 0 ? v : -v) * (1 << al) * q +
                                  (((1 << al) - 1) * q) / 2;
                    if ((need >> z) & 1u) {
                        ps_add(P, ci, bx, by, ZZ_NATURAL[z], v > 0 ? mag : -mag);
                    }
                }
                z++;
            }
        }
        if (P->in.eof) return ART_ERR_READ;
    }
    return ART_OK;
}

static bool ps_done(const ps_t *P)
{
    int i;
    for (i = 0; i < P->ncomp; i++) {
        uint64_t need = P->pl[i == 0 ? 0 : 1].need;
        if (!P->comp[i].dc_done) return false;
        if ((need & ~P->comp[i].coded) != 0u) return false;
    }
    return true;
}

/* Sets up one plane: pick k so the crop is at least `target` samples but
 * under twice that, then map plane samples onto totals. */
static void ps_plane(ps_plane_t *pl, int pw, int ph, int target,
                     uint16_t *map, uint8_t *cnt)
{
    int shorter = (pw < ph) ? pw : ph, sc = 0, i, sw, sh;
    uint64_t need = 0;

    while (sc < 3 && (shorter >> (sc + 1)) >= target) sc++;
    pl->k = 8 >> sc;
    pl->basis = (pl->k == 1) ? BASIS8 : (pl->k == 2) ? BASIS2
              : (pl->k == 4) ? BASIS4 : BASIS8;
    sw = (pw * pl->k + 7) / 8;
    sh = (ph * pl->k + 7) / 8;
    pl->side = (sw < sh) ? sw : sh;
    pl->x0 = (sw - pl->side) / 2;
    pl->y0 = (sh - pl->side) / 2;
    pl->n = (pl->side < target) ? pl->side : target;
    pl->map = map;
    pl->cnt = cnt;
    memset(cnt, 0, ART_MAX_SIZE);
    for (i = 0; i < pl->side; i++) {
        int o = i * pl->n / pl->side;
        map[i] = (uint16_t)o;
        cnt[o]++;
    }
    for (i = 1; i < 64; i++) {
        if ((ZZ_NATURAL[i] & 7) < pl->k && (ZZ_NATURAL[i] >> 3) < pl->k) {
            need |= (uint64_t)1 << i;
        }
    }
    pl->need = need;
}

/* Geometry once SOF2 is known; PS_FALLBACK for anything this path does
 * not handle (the first-pass path then gets the file). */
static art_result_t ps_setup(ps_t *P, int16_t *out, art_chroma_t *ch)
{
    int shorter = (P->width < P->height) ? P->width : P->height, sc = 0, i;

    while (sc < 3 && (shorter >> (sc + 1)) >= P->size) sc++;
    if (sc == 3 || shorter < P->size) return PS_FALLBACK;   /* DC enough / tiny */
    if (P->adobe_rgb) return PS_FALLBACK;
    if (P->comp[0].h != P->hmax || P->comp[0].v != P->vmax) return PS_FALLBACK;

    for (i = 0; i < P->ncomp; i++) {
        ps_comp_t *c = &P->comp[i];
        int cw = (P->width * c->h + P->hmax - 1) / P->hmax;
        int chh = (P->height * c->v + P->vmax - 1) / P->vmax;
        c->bw = (cw + 7) / 8;
        c->bh = (chh + 7) / 8;
    }

    ps_plane(&P->pl[0], P->width, P->height, P->size,
             P->pp->map[0], P->pp->cnt[0]);
    if (P->pl[0].side > PS_MAP_MAX) return PS_FALLBACK;
    P->pl[0].acc[0] = out;

    if (P->ncomp == 3) {
        int cw = (P->width * P->comp[1].h + P->hmax - 1) / P->hmax;
        int chh = (P->height * P->comp[1].v + P->vmax - 1) / P->vmax;
        int target = (P->size + 1) / 2;
        if (P->comp[1].h != P->comp[2].h || P->comp[1].v != P->comp[2].v) {
            return PS_FALLBACK;
        }
        if (target > ART_CHROMA_MAX) target = ART_CHROMA_MAX;
        ps_plane(&P->pl[1], cw, chh, target, P->pp->map[1], P->pp->cnt[1]);
        if (P->pl[1].side > PS_MAP_MAX) return PS_FALLBACK;
        P->pl[1].acc[0] = ch->sum[0];
        P->pl[1].acc[1] = ch->sum[1];
    }
    return ART_OK;
}

/* Where each output pixel's centre falls on the chroma totals grid:
 * a cell and a 0..255 weight toward the next one. */
static void ps_chroma_axis(int n_out, int n_in, int16_t *c0, uint8_t *w)
{
    int o;
    for (o = 0; o < n_out; o++) {
        int32_t g = (int32_t)(((int64_t)(2 * o + 1) * n_in * 32768) / n_out) - 32768;
        int gi;
        if (g < 0) g = 0;
        gi = g >> 16;
        if (gi >= n_in - 1) { gi = n_in - 1; g = gi << 16; }
        c0[o] = (int16_t)gi;
        w[o] = (uint8_t)((g >> 8) & 0xFF);
    }
}

/* Average of one chroma total, in levels * 256 (level-shifted). */
static int32_t ps_chroma_at(const ps_t *P, int slot, int x, int y)
{
    const ps_plane_t *pl = &P->pl[1];
    int c = 4 * pl->cnt[x] * pl->cnt[y];
    int32_t t = pl->acc[slot][y * pl->n + x];
    return (c > 0) ? (t * 256) / c : 0;
}

static void ps_output(ps_t *P)
{
    const ps_plane_t *L = &P->pl[0], *C = &P->pl[1];
    int ox, oy, n = P->size;

    if (P->ncomp == 3) {
        ps_chroma_axis(n, C->n, P->pp->sx0, P->pp->sxw);
        ps_chroma_axis(n, C->n, P->pp->sy0, P->pp->syw);
    }

    for (oy = 0; oy < n; oy++) {
        for (ox = 0; ox < n; ox++) {
            int16_t *a = &L->acc[0][oy * n + ox];
            int c4 = 4 * L->cnt[ox] * L->cnt[oy];
            int yv = clamp255((*a >= 0 ? (*a + c4 / 2) / c4
                                       : -((-*a + c4 / 2) / c4)) + 128);
            int r, g, b;

            if (P->ncomp == 3) {
                int x0 = P->pp->sx0[ox], y0 = P->pp->sy0[oy];
                int x1 = (x0 + 1 < C->n) ? x0 + 1 : x0;
                int y1 = (y0 + 1 < C->n) ? y0 + 1 : y0;
                int32_t wx = P->pp->sxw[ox], wy = P->pp->syw[oy], cc[2];
                int sl;
                for (sl = 0; sl < 2; sl++) {
                    int32_t t0 = ps_chroma_at(P, sl, x0, y0) * (256 - wx) +
                                 ps_chroma_at(P, sl, x1, y0) * wx;
                    int32_t t1 = ps_chroma_at(P, sl, x0, y1) * (256 - wx) +
                                 ps_chroma_at(P, sl, x1, y1) * wx;
                    int64_t q = (int64_t)t0 * (256 - wy) + (int64_t)t1 * wy;
                    cc[sl] = (int32_t)((q >= 0 ? q + (1 << 23) : q - (1 << 23)) / (1 << 24));
                }
                r = clamp255(yv + ((91881 * cc[1] + 32768) >> 16));
                g = clamp255(yv - ((22554 * cc[0] + 46802 * cc[1] + 32768) >> 16));
                b = clamp255(yv + ((116130 * cc[0] + 32768) >> 16));
            } else {
                r = g = b = yv;
            }
            /* In place: this pixel's total has been read and nothing reads
             * it again. */
            ((uint16_t *)L->acc[0])[oy * n + ox] =
                to565((uint32_t)r, (uint32_t)g, (uint32_t)b, ox, oy);
        }
    }
}

static art_result_t prog_sharp(const art_src_t *src, uint16_t *out, int size,
                               art_work_t *work, art_info_t *info)
{
    static ps_t P;               /* a few hundred bytes; off the stack */
    bool have_sof = false;
    int i;

    memset(&P, 0, sizeof(P));
    P.pp = (ps_pool_t *)(void *)work->pool;
    memset(P.pp, 0, sizeof(*P.pp));
    P.size = size;
    P.pending = -1;
    P.in.src = src;
    P.in.buf = P.pp->inbuf;

    if (dc_byte(&P.in) != 0xFF || dc_byte(&P.in) != 0xD8) {
        return P.in.eof ? ART_ERR_READ : ART_ERR_FORMAT;
    }

    for (;;) {
        int m = ps_marker(&P), len;
        if (m < 0) goto out_of_data;
        if (m == 0xD9) break;                   /* EOI */
        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) continue;
        len = dc_u16(&P.in);
        if (len < 2) { if (P.in.eof) goto out_of_data; return ART_ERR_FORMAT; }
        len -= 2;

        if (m == 0xDB) {                                   /* DQT */
            while (len > 0) {
                int pq_tq = dc_byte(&P.in), t = pq_tq & 3, n16 = pq_tq >> 4, z;
                if (pq_tq < 0) goto out_of_data;
                for (z = 0; z < 64; z++) {
                    int q = n16 ? dc_u16(&P.in) : dc_byte(&P.in);
                    if (q < 0) goto out_of_data;
                    P.pp->qt[t][z] = (uint16_t)q;
                }
                len -= 1 + (n16 ? 128 : 64);
            }
        } else if (m == 0xC4) {                            /* DHT */
            while (len > 0) {
                int tc_th = dc_byte(&P.in), total = 0, l;
                uint8_t counts[16];
                hf_t *h;
                for (l = 0; l < 16; l++) {
                    int c = dc_byte(&P.in);
                    if (c < 0) goto out_of_data;
                    counts[l] = (uint8_t)c;
                    total += c;
                }
                if (tc_th < 0) goto out_of_data;
                if (total > 256) return ART_ERR_FORMAT;
                h = ((tc_th >> 4) == 0) ? &P.pp->dc[tc_th & 3] : &P.pp->ac[tc_th & 3];
                memset(h, 0, sizeof(*h));
                for (l = 0; l < total; l++) {
                    int v = dc_byte(&P.in);
                    if (v < 0) goto out_of_data;
                    h->vals[l] = (uint8_t)v;
                }
                hf_build(h, counts);
                len -= 17 + total;
            }
        } else if (m == 0xC2) {                            /* SOF2 */
            int p = dc_byte(&P.in);
            art_result_t r;
            P.height = dc_u16(&P.in);
            P.width = dc_u16(&P.in);
            P.ncomp = dc_byte(&P.in);
            if (p != 8 || (P.ncomp != 1 && P.ncomp != 3)) return PS_FALLBACK;
            P.hmax = P.vmax = 1;
            for (i = 0; i < P.ncomp; i++) {
                int id = dc_byte(&P.in), hv = dc_byte(&P.in), tq = dc_byte(&P.in);
                if (tq < 0) goto out_of_data;
                P.comp[i].id = (uint8_t)id;
                P.comp[i].h = (uint8_t)(hv >> 4);
                P.comp[i].v = (uint8_t)(hv & 15);
                P.comp[i].tq = (uint8_t)(tq & 3);
                if (P.comp[i].h < 1 || P.comp[i].h > 4 || P.comp[i].v < 1 ||
                    P.comp[i].v > 4) {
                    return ART_ERR_FORMAT;
                }
            }
            if (P.ncomp == 1) P.comp[0].h = P.comp[0].v = 1;
            for (i = 0; i < P.ncomp; i++) {
                if (P.comp[i].h > P.hmax) P.hmax = P.comp[i].h;
                if (P.comp[i].v > P.vmax) P.vmax = P.comp[i].v;
            }
            if (len != 6 + 3 * P.ncomp) return ART_ERR_FORMAT;
            if (info != NULL) {
                info->src_w = (uint16_t)P.width;
                info->src_h = (uint16_t)P.height;
            }
            if (P.width < 1 || P.height < 1) return ART_ERR_FORMAT;
            P.mcux = (P.width + 8 * P.hmax - 1) / (8 * P.hmax);
            P.mcuy = (P.height + 8 * P.vmax - 1) / (8 * P.vmax);
            memset(out, 0, (size_t)size * (size_t)size * sizeof(uint16_t));
            memset(&work->u.chroma, 0, sizeof(work->u.chroma));
            r = ps_setup(&P, (int16_t *)(void *)out, &work->u.chroma);
            if (r != ART_OK) return r;
            if (info != NULL) {
                info->scale = (uint8_t)((P.pl[0].k == 2) ? 2 :
                                        (P.pl[0].k == 4) ? 1 : 0);
            }
            have_sof = true;
        } else if ((m >= 0xC0 && m <= 0xCF) && m != 0xC4 && m != 0xC8 &&
                   m != 0xCC) {
            return ART_ERR_UNSUPPORTED;
        } else if (m == 0xDD) {                            /* DRI */
            int ri = dc_u16(&P.in);
            if (ri < 0) goto out_of_data;
            P.restart = (uint32_t)ri;
            if (len > 2 && !dc_skip(&P.in, (size_t)(len - 2))) goto out_of_data;
        } else if (m == 0xEE && len >= 12) {               /* Adobe */
            uint8_t a[12];
            for (i = 0; i < 12; i++) {
                int b = dc_byte(&P.in);
                if (b < 0) goto out_of_data;
                a[i] = (uint8_t)b;
            }
            if (memcmp(a, "Adobe", 5) == 0 && a[11] == 0u) P.adobe_rgb = true;
            if (!dc_skip(&P.in, (size_t)(len - 12))) goto out_of_data;
        } else if (m == 0xDA) {                            /* SOS */
            int ns = dc_byte(&P.in), sel[DC_MAX_COMP], ss, se, ahal, ah, al;
            art_result_t r = ART_OK;

            if (!have_sof) return ART_ERR_FORMAT;
            if (P.adobe_rgb) return PS_FALLBACK;   /* APP14 can follow SOF */
            if (ns < 1 || ns > P.ncomp) return ART_ERR_FORMAT;
            for (i = 0; i < ns; i++) {
                int cs = dc_byte(&P.in), t = dc_byte(&P.in), c;
                if (t < 0) goto out_of_data;
                sel[i] = -1;
                for (c = 0; c < P.ncomp; c++) {
                    if (P.comp[c].id == cs) {
                        sel[i] = c;
                        P.comp[c].td = (uint8_t)((t >> 4) & 3);
                        P.comp[c].ta = (uint8_t)(t & 3);
                    }
                }
                if (sel[i] < 0) return ART_ERR_FORMAT;
            }
            ss = dc_byte(&P.in);
            se = dc_byte(&P.in);
            ahal = dc_byte(&P.in);
            if (ahal < 0) goto out_of_data;
            ah = ahal >> 4;
            al = ahal & 15;

            if (ss == 0) {                           /* DC: first or refine */
                for (i = 0; i < ns; i++) {
                    if (ah == 0 && !P.pp->dc[P.comp[sel[i]].td].present) {
                        return ART_ERR_FORMAT;
                    }
                }
                r = ps_dc_scan(&P, sel, ns, ah, al);
                if (r == ART_ERR_READ) goto out_of_data;
                if (r != ART_OK) return r;
                if (ah == 0) for (i = 0; i < ns; i++) P.comp[sel[i]].dc_done = true;
            } else if (ns == 1 && ah == 0 && ss <= se && se <= 63) {
                /* AC first pass. Refinement passes (ah > 0) cannot be
                 * decoded without per-coefficient history, and only add
                 * low bits: they are passed over. */
                int ci = sel[0], z;
                uint64_t band = 0;
                for (z = ss; z <= se; z++) band |= (uint64_t)1 << z;
                if ((band & P.pl[ci == 0 ? 0 : 1].need) != 0u) {
                    if (!P.pp->ac[P.comp[ci].ta].present) return ART_ERR_FORMAT;
                    r = ps_ac_first(&P, ci, ss, se, al);
                    if (r == ART_ERR_READ) goto out_of_data;
                    if (r != ART_OK) return r;
                }
                P.comp[ci].coded |= band;
            }
            P.in.bits = 0;                          /* leave the scan */
            P.in.nbits = 0;
            if (ps_done(&P)) break;                 /* everything needed */
            if (src->yield != NULL) src->yield(src->ctx);
        } else if (len > 0 && !dc_skip(&P.in, (size_t)len)) {
            goto out_of_data;
        }
    }
    goto finish;

out_of_data:
    /* The file ended early. If every component's first pass is in, the
     * picture is whole, just less detailed where the rest was missing:
     * show it. Otherwise it is an error. */
    if (!have_sof) return ART_ERR_READ;

finish:
    if (!have_sof) return ART_ERR_FORMAT;
    for (i = 0; i < P.ncomp; i++) if (!P.comp[i].dc_done) return ART_ERR_READ;
    ps_output(&P);
    return ART_OK;
}

art_result_t art_decode_jpeg_progressive(const art_src_t *src, uint16_t *out,
                                         int size, art_work_t *work,
                                         art_info_t *info)
{
    art_result_t r;

    if (info != NULL) memset(info, 0, sizeof(*info));
    if (src == NULL || src->read == NULL || out == NULL || work == NULL ||
        size < 1 || size > ART_MAX_SIZE) {
        return ART_ERR_ARG;
    }
    r = prog_sharp(src, out, size, work, info);
    if (r != PS_FALLBACK) return r;

    /* Big covers (1/8 is already sharp), tiny ones, odd layouts: the
     * first-pass path, from the top of the file. */
    if (src->rewind == NULL || !src->rewind(src->ctx)) return ART_ERR_UNSUPPORTED;
    return prog_dc_only(src, out, size, work, info);
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
