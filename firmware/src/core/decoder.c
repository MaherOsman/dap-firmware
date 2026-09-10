#include "decoder.h"
#include <string.h>

#define MAX_DECODERS 8
static const decoder_vtable_t *g_reg[MAX_DECODERS];
static size_t g_count;

void decoder_register(const decoder_vtable_t *vt)
{
    if (vt && g_count < MAX_DECODERS) g_reg[g_count++] = vt;
}

void decoder_registry_clear(void) { g_count = 0; }
size_t decoder_registry_count(void) { return g_count; }

const decoder_vtable_t *decoder_find(const uint8_t *header, size_t len)
{
    for (size_t i = 0; i < g_count; i++)
        if (g_reg[i]->probe && g_reg[i]->probe(header, len)) return g_reg[i];
    return NULL;
}

bool decoder_open(decoder_t *d, decoder_io_t io, decoder_info_t *out)
{
    if (!d || !io.read || !io.seek) return false;

    uint8_t hdr[DECODER_MAX_HEADER];
    size_t n = io.read(io.ctx, hdr, sizeof hdr);

    const decoder_vtable_t *vt = decoder_find(hdr, n);
    if (!vt) return false;

    /* The probe consumed bytes; rewind so open() sees the file from byte 0. */
    if (!io.seek(io.ctx, 0)) return false;

    memset(d, 0, sizeof *d);
    d->vt = vt;
    d->io = io;

    if (!vt->open(d, &d->info)) return false;
    d->is_open = true;
    if (out) *out = d->info;
    return true;
}

size_t decoder_decode(decoder_t *d, int32_t *dst, size_t frames)
{
    if (!d || !d->is_open || !dst || frames == 0) return 0;
    return d->vt->decode(d, dst, frames);
}

bool decoder_seek_frame(decoder_t *d, uint32_t frame)
{
    if (!d || !d->is_open || !d->vt->seek_frame) return false;
    return d->vt->seek_frame(d, frame);
}

void decoder_close(decoder_t *d)
{
    if (!d) return;
    if (d->is_open && d->vt->close) d->vt->close(d);
    d->is_open = false;
}

const char *decoder_name(const decoder_t *d)
{
    return (d && d->vt) ? d->vt->name : "none";
}
