/* test_art_find — locating album art in folders, FLAC and ID3 tags. */

#include "test.h"

#include <stdlib.h>

#include "../src/core/art_find.h"
#include "art_fixtures.h"

/* ---------------------------------------------------- RAM filesystem */

#define MAX_FILES  16
#define FILE_CAP   16384

typedef struct {
    char     path[ART_PATH_MAX + 1];
    uint8_t  data[FILE_CAP];
    uint32_t len;
} ram_file_t;

typedef struct {
    ram_file_t *f;
    uint32_t    pos;
} ram_fh_t;

static ram_file_t g_files[MAX_FILES];
static int        g_nfiles;
static ram_fh_t   g_fh[8];
static int        g_open_count;   /* leak check: opens minus closes */

static void fs_reset(void)
{
    memset(g_files, 0, sizeof(g_files));
    g_nfiles = 0;
    g_open_count = 0;
}

static ram_file_t *fs_add(const char *path)
{
    ram_file_t *f = &g_files[g_nfiles++];
    strcpy(f->path, path);
    f->len = 0;
    return f;
}

static void put(ram_file_t *f, const void *p, uint32_t n)
{
    memcpy(f->data + f->len, p, n);
    f->len += n;
}
static void put8(ram_file_t *f, uint32_t v) { uint8_t b = (uint8_t)v; put(f, &b, 1); }
static void put24(ram_file_t *f, uint32_t v) { put8(f, v >> 16); put8(f, v >> 8); put8(f, v); }
static void put32(ram_file_t *f, uint32_t v) { put8(f, v >> 24); put24(f, v); }
static void put_ss(ram_file_t *f, uint32_t v)   /* ID3 syncsafe */
{
    put8(f, (v >> 21) & 0x7F); put8(f, (v >> 14) & 0x7F);
    put8(f, (v >> 7) & 0x7F);  put8(f, v & 0x7F);
}
static void put_str(ram_file_t *f, const char *s) { put(f, s, (uint32_t)strlen(s)); }

static int r_open(void *c, const char *path, int mode, void **fh)
{
    int i, k;
    (void)c;
    if (mode != LIB_IO_READ) return -1;
    for (i = 0; i < g_nfiles; i++) {
        if (strcmp(g_files[i].path, path) == 0) {
            for (k = 0; k < 8; k++) {
                if (g_fh[k].f == NULL) {
                    g_fh[k].f = &g_files[i];
                    g_fh[k].pos = 0;
                    *fh = &g_fh[k];
                    g_open_count++;
                    return 0;
                }
            }
        }
    }
    return -1;
}

static int r_read(void *c, void *fh, void *dst, uint32_t len, uint32_t *got)
{
    ram_fh_t *h = (ram_fh_t *)fh;
    uint32_t left = (h->pos < h->f->len) ? h->f->len - h->pos : 0;
    (void)c;
    if (len > left) len = left;
    memcpy(dst, h->f->data + h->pos, len);
    h->pos += len;
    *got = len;
    return 0;
}

static int r_seek(void *c, void *fh, uint32_t off)
{
    ram_fh_t *h = (ram_fh_t *)fh;
    (void)c;
    if (off > h->f->len) return -1;
    h->pos = off;
    return 0;
}

static int r_close(void *c, void *fh)
{
    (void)c;
    ((ram_fh_t *)fh)->f = NULL;
    g_open_count--;
    return 0;
}

static lib_io_t make_io(void)
{
    lib_io_t io;
    memset(&io, 0, sizeof(io));
    io.open = r_open; io.read = r_read; io.seek = r_seek; io.close = r_close;
    return io;
}

/* -------------------------------------------------- file builders */

static const uint8_t PNG_BYTES[] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A,
                                     0x0A, 0, 0, 0, 13, 'I', 'H', 'D', 'R' };
static const uint8_t AUDIO_JUNK[] = { 0xFF, 0xFB, 0x90, 0x44, 0, 0, 0, 0 };

static void add_image(const char *path, const uint8_t *data, uint32_t n)
{
    put(fs_add(path), data, n);
}

/* A FLAC file with STREAMINFO and one PICTURE per (type, image). */
static uint32_t add_flac(const char *path, int npics, const uint32_t *types,
                         const uint8_t *const *imgs, const uint32_t *lens)
{
    ram_file_t *f = fs_add(path);
    uint32_t data_off = 0;
    int i;

    put_str(f, "fLaC");
    put8(f, 0x00); put24(f, 34);                   /* STREAMINFO */
    { uint8_t z[34] = {0}; put(f, z, 34); }
    put8(f, 0x04); put24(f, 12);                   /* a VORBIS_COMMENT-ish */
    { uint8_t z[12] = {0}; put(f, z, 12); }

    for (i = 0; i < npics; i++) {
        const char *mime = "image/jpeg";
        const char *desc = "cover";
        uint32_t body = 4 + 4 + (uint32_t)strlen(mime) + 4 +
                        (uint32_t)strlen(desc) + 16 + 4 + lens[i];
        put8(f, (i == npics - 1) ? 0x86 : 0x06);   /* last flag on the end */
        put24(f, body);
        put32(f, types[i]);
        put32(f, (uint32_t)strlen(mime)); put_str(f, mime);
        put32(f, (uint32_t)strlen(desc)); put_str(f, desc);
        put32(f, 400); put32(f, 400); put32(f, 24); put32(f, 0);
        put32(f, lens[i]);
        if (types[i] == 3u || data_off == 0) data_off = f->len;
        put(f, imgs[i], lens[i]);
    }
    put(f, AUDIO_JUNK, sizeof(AUDIO_JUNK));
    return data_off;
}

/* An MP3 with an ID3v2.<major> tag holding one picture frame. */
static uint32_t add_mp3(const char *path, int major, uint32_t pic_type,
                        const uint8_t *img, uint32_t n, int utf16_desc)
{
    ram_file_t *f = fs_add(path);
    uint32_t frame_len, data_off;
    uint8_t desc16[] = { 0xFF, 0xFE, 'c', 0, 'v', 0, 0, 0 };  /* BOM "cv" */

    put_str(f, "ID3"); put8(f, (uint32_t)major); put8(f, 0); put8(f, 0);
    put_ss(f, 0);                                  /* patched below */

    /* a text frame first, so the walker has to step over something */
    if (major == 2) {
        put_str(f, "TT2"); put24(f, 4); put8(f, 0); put_str(f, "abc");
    } else {
        put_str(f, "TIT2");
        if (major == 4) put_ss(f, 4); else put32(f, 4);
        put8(f, 0); put8(f, 0);
        put8(f, 0); put_str(f, "abc");
    }

    if (major == 2) {
        frame_len = 1 + 3 + 1 + 6 + n;
        put_str(f, "PIC"); put24(f, frame_len);
        put8(f, 0); put_str(f, "JPG"); put8(f, pic_type);
        put_str(f, "front"); put8(f, 0);
    } else {
        uint32_t desc_len = utf16_desc ? (uint32_t)sizeof(desc16) : 6u;
        frame_len = 1 + 11 + 1 + desc_len + n;
        put_str(f, "APIC");
        if (major == 4) put_ss(f, frame_len); else put32(f, frame_len);
        put8(f, 0); put8(f, 0);
        put8(f, utf16_desc ? 1 : 0);
        put_str(f, "image/jpeg"); put8(f, 0);
        put8(f, pic_type);
        if (utf16_desc) put(f, desc16, sizeof(desc16));
        else { put_str(f, "front"); put8(f, 0); }
    }
    data_off = f->len;
    put(f, img, n);

    { uint8_t z[32] = {0}; put(f, z, 32); }        /* padding */
    {   /* patch the tag size */
        uint32_t size = f->len - 10;
        f->data[6] = (uint8_t)((size >> 21) & 0x7F);
        f->data[7] = (uint8_t)((size >> 14) & 0x7F);
        f->data[8] = (uint8_t)((size >> 7) & 0x7F);
        f->data[9] = (uint8_t)(size & 0x7F);
    }
    put(f, AUDIO_JUNK, sizeof(AUDIO_JUNK));
    return data_off;
}

static void add_plain_audio(const char *path)
{
    put(fs_add(path), AUDIO_JUNK, sizeof(AUDIO_JUNK));
}

/* ================================================================== */

TEST(the_probe_tells_formats_apart_by_their_bytes)
{
    lib_io_t io = make_io();
    void *fh;

    fs_reset();
    add_image("/a.jpg", FIX_QUAD64, sizeof(FIX_QUAD64));
    add_image("/p.jpg", FIX_PROG, sizeof(FIX_PROG));
    add_image("/x.jpg", PNG_BYTES, sizeof(PNG_BYTES));   /* lying name */
    add_image("/j.bin", AUDIO_JUNK, sizeof(AUDIO_JUNK));

    CHECK_EQ(io.open(NULL, "/a.jpg", 0, &fh), 0);
    CHECK_EQ(art_probe(&io, fh, 0, 0), ART_FMT_JPEG);
    io.close(NULL, fh);
    CHECK_EQ(io.open(NULL, "/p.jpg", 0, &fh), 0);
    CHECK_EQ(art_probe(&io, fh, 0, 0), ART_FMT_JPEG_PROGRESSIVE);
    io.close(NULL, fh);
    CHECK_EQ(io.open(NULL, "/x.jpg", 0, &fh), 0);
    CHECK_EQ(art_probe(&io, fh, 0, 0), ART_FMT_PNG);
    io.close(NULL, fh);
    CHECK_EQ(io.open(NULL, "/j.bin", 0, &fh), 0);
    CHECK_EQ(art_probe(&io, fh, 0, 0), ART_FMT_UNKNOWN);
    io.close(NULL, fh);
    CHECK_EQ(g_open_count, 0);
}

TEST(a_folder_cover_is_found_next_to_the_track)
{
    lib_io_t io = make_io();
    art_ref_t r;

    fs_reset();
    add_plain_audio("/Music/A/Album/01 One.flac");
    add_image("/Music/A/Album/folder.jpg", FIX_QUAD64, sizeof(FIX_QUAD64));

    CHECK(art_find(&io, "/Music/A/Album/01 One.flac", &r));
    CHECK_EQ(r.fmt, ART_FMT_JPEG);
    CHECK_EQ(r.from, ART_FROM_FOLDER);
    CHECK(strcmp(r.path, "/Music/A/Album/folder.jpg") == 0);
    CHECK_EQ(r.offset, 0u);
    CHECK_EQ(g_open_count, 0);
}

TEST(cover_is_preferred_over_folder)
{
    lib_io_t io = make_io();
    art_ref_t r;

    fs_reset();
    add_plain_audio("/M/01.mp3");
    add_image("/M/folder.jpg", FIX_QUAD64, sizeof(FIX_QUAD64));
    add_image("/M/cover.jpg", FIX_QUAD400, sizeof(FIX_QUAD400));
    CHECK(art_find(&io, "/M/01.mp3", &r));
    CHECK(strcmp(r.path, "/M/cover.jpg") == 0);
}

TEST(a_disc_subfolder_looks_in_the_album_folder_too)
{
    lib_io_t io = make_io();
    art_ref_t r;

    fs_reset();
    add_plain_audio("/Music/DP/RAM/Disc 1/05 Instant Crush.flac");
    add_image("/Music/DP/RAM/cover.jpg", FIX_QUAD64, sizeof(FIX_QUAD64));
    CHECK(art_find(&io, "/Music/DP/RAM/Disc 1/05 Instant Crush.flac", &r));
    CHECK(strcmp(r.path, "/Music/DP/RAM/cover.jpg") == 0);

    /* ...but an ordinary subfolder does not climb into the artist folder */
    fs_reset();
    add_plain_audio("/Music/Artist/Album/01.flac");
    add_image("/Music/Artist/cover.jpg", FIX_QUAD64, sizeof(FIX_QUAD64));
    CHECK(!art_find(&io, "/Music/Artist/Album/01.flac", &r));
    CHECK_EQ(r.fmt, ART_FMT_NONE);

    /* CD2 and disk_3 spellings count */
    fs_reset();
    add_plain_audio("/X/CD2/01.flac");
    add_image("/X/cover.jpg", FIX_QUAD64, sizeof(FIX_QUAD64));
    CHECK(art_find(&io, "/X/CD2/01.flac", &r));
    fs_reset();
    add_plain_audio("/X/disk_3/01.flac");
    add_image("/X/front.jpg", FIX_QUAD64, sizeof(FIX_QUAD64));
    CHECK(art_find(&io, "/X/disk_3/01.flac", &r));
}

TEST(flac_embedded_art_is_found_and_the_front_cover_wins)
{
    lib_io_t io = make_io();
    art_ref_t r;
    uint32_t types[2] = { 4u, 3u };               /* back cover, then front */
    const uint8_t *imgs[2] = { FIX_QUAD64, FIX_QUAD400 };
    uint32_t lens[2] = { sizeof(FIX_QUAD64), sizeof(FIX_QUAD400) };
    uint32_t off;

    fs_reset();
    off = add_flac("/F/01.flac", 2, types, imgs, lens);
    CHECK(art_find(&io, "/F/01.flac", &r));
    CHECK_EQ(r.from, ART_FROM_FLAC);
    CHECK_EQ(r.fmt, ART_FMT_JPEG);
    CHECK_EQ(r.offset, off);
    CHECK_EQ(r.length, sizeof(FIX_QUAD400));
    CHECK_EQ(g_open_count, 0);
}

TEST(id3_art_is_found_in_every_tag_version)
{
    lib_io_t io = make_io();
    art_ref_t r;
    int major;

    for (major = 2; major <= 4; major++) {
        uint32_t off;
        fs_reset();
        off = add_mp3("/M/01.mp3", major, 3u, FIX_QUAD64, sizeof(FIX_QUAD64), 0);
        CHECK(art_find(&io, "/M/01.mp3", &r));
        CHECK_EQ(r.from, ART_FROM_ID3);
        CHECK_EQ(r.offset, off);
        CHECK_EQ(r.length, sizeof(FIX_QUAD64));
        CHECK_EQ(r.fmt, ART_FMT_JPEG);
    }
}

TEST(a_utf16_description_is_stepped_over_correctly)
{
    lib_io_t io = make_io();
    art_ref_t r;
    uint32_t off;

    fs_reset();
    off = add_mp3("/M/01.mp3", 3, 3u, FIX_QUAD64, sizeof(FIX_QUAD64), 1);
    CHECK(art_find(&io, "/M/01.mp3", &r));
    CHECK_EQ(r.offset, off);
    CHECK_EQ(r.fmt, ART_FMT_JPEG);
}

TEST(a_decodable_embedded_cover_beats_an_undecodable_folder_one)
{
    lib_io_t io = make_io();
    art_ref_t r;

    fs_reset();
    add_mp3("/M/01.mp3", 3, 3u, FIX_QUAD64, sizeof(FIX_QUAD64), 0);
    add_image("/M/cover.png", PNG_BYTES, sizeof(PNG_BYTES));
    CHECK(art_find(&io, "/M/01.mp3", &r));
    CHECK_EQ(r.from, ART_FROM_ID3);
}

TEST(undecodable_art_is_reported_for_the_log)
{
    lib_io_t io = make_io();
    art_ref_t r;

    fs_reset();
    add_plain_audio("/M/01.flac");
    add_image("/M/cover.png", PNG_BYTES, sizeof(PNG_BYTES));
    CHECK(!art_find(&io, "/M/01.flac", &r));
    CHECK_EQ(r.fmt, ART_FMT_PNG);
    CHECK(strcmp(r.path, "/M/cover.png") == 0);

    fs_reset();
    add_mp3("/M/01.mp3", 3, 3u, FIX_PROG, sizeof(FIX_PROG), 0);
    CHECK(!art_find(&io, "/M/01.mp3", &r));
    CHECK_EQ(r.fmt, ART_FMT_JPEG_PROGRESSIVE);
    CHECK_EQ(r.from, ART_FROM_ID3);

    fs_reset();
    add_plain_audio("/M/01.flac");
    CHECK(!art_find(&io, "/M/01.flac", &r));
    CHECK_EQ(r.fmt, ART_FMT_NONE);
    CHECK_EQ(g_open_count, 0);
}

TEST(found_art_decodes_through_the_reader)
{
    static art_work_t work;
    static uint16_t out[176 * 176];
    lib_io_t io = make_io();
    art_ref_t r;
    art_reader_t rd;
    art_src_t src;
    uint32_t types[1] = { 3u };
    const uint8_t *imgs[1] = { FIX_QUAD400 };
    uint32_t lens[1] = { sizeof(FIX_QUAD400) };
    uint16_t red;

    /* embedded: the reader must stop exactly at the picture's end, even
     * though audio follows it in the file */
    fs_reset();
    add_flac("/F/01.flac", 1, types, imgs, lens);
    CHECK(art_find(&io, "/F/01.flac", &r));
    CHECK(art_open(&io, &r, &rd, &src, NULL, NULL));
    CHECK_EQ(art_decode_jpeg(&src, out, 176, &work, NULL), ART_OK);
    art_close(&rd);
    red = out[44 * 176 + 44];
    CHECK(((red >> 11) & 0x1F) > 20 && ((red >> 5) & 0x3F) < 16);
    CHECK_EQ(g_open_count, 0);

    /* folder file, whole-file reference */
    fs_reset();
    add_plain_audio("/M/01.flac");
    add_image("/M/cover.jpg", FIX_QUAD400, sizeof(FIX_QUAD400));
    CHECK(art_find(&io, "/M/01.flac", &r));
    CHECK(art_open(&io, &r, &rd, &src, NULL, NULL));
    CHECK_EQ(art_decode_jpeg(&src, out, 176, &work, NULL), ART_OK);
    art_close(&rd);
    CHECK_EQ(g_open_count, 0);
}

TEST(damaged_containers_never_hang_or_crash)
{
    lib_io_t io = make_io();
    art_ref_t r;
    ram_file_t *f;
    int i;

    /* FLAC whose block lengths point past the end */
    fs_reset();
    f = fs_add("/F/01.flac");
    put_str(f, "fLaC");
    put8(f, 0x06); put24(f, 0xFFFFFF);
    CHECK(!art_find(&io, "/F/01.flac", &r));

    /* ID3 claiming a huge tag full of zero-size frames */
    fs_reset();
    f = fs_add("/M/01.mp3");
    put_str(f, "ID3"); put8(f, 3); put8(f, 0); put8(f, 0); put_ss(f, 0x0FFFFFFF);
    for (i = 0; i < 20; i++) { put_str(f, "TXXX"); put32(f, 0); put8(f, 0); put8(f, 0); }
    CHECK(!art_find(&io, "/M/01.mp3", &r));

    /* APIC whose size runs off the end of the file */
    fs_reset();
    f = fs_add("/M/02.mp3");
    put_str(f, "ID3"); put8(f, 3); put8(f, 0); put8(f, 0); put_ss(f, 100);
    put_str(f, "APIC"); put32(f, 5000); put8(f, 0); put8(f, 0);
    put8(f, 0); put_str(f, "image/jpeg"); put8(f, 0); put8(f, 3); put8(f, 0);
    put(f, FIX_QUAD64, 40);
    (void)art_find(&io, "/M/02.mp3", &r);   /* any answer, just no crash */

    /* a missing track */
    fs_reset();
    CHECK(!art_find(&io, "/nope/01.flac", &r));
    CHECK_EQ(r.fmt, ART_FMT_NONE);
    CHECK_EQ(g_open_count, 0);
}

TEST(bad_arguments_are_safe)
{
    lib_io_t io = make_io();
    art_ref_t r;
    art_reader_t rd;
    art_src_t src;

    fs_reset();
    CHECK(!art_find(NULL, "/a", &r));
    CHECK(!art_find(&io, NULL, &r));
    CHECK(!art_find(&io, "/a", NULL));
    CHECK(!art_find_embedded(&io, "/a", &r));
    memset(&r, 0, sizeof(r));
    CHECK(!art_open(&io, &r, &rd, &src, NULL, NULL));   /* fmt NONE */
    art_close(&rd);
    art_close(NULL);
    CHECK(art_fmt_name(ART_FMT_PNG)[0] != '?');
    CHECK(art_from_name(ART_FROM_ID3)[0] != '?');
}

TEST(later_candidates_can_be_asked_for_in_order)
{
    lib_io_t io = make_io();
    art_ref_t r;

    fs_reset();
    add_mp3("/M/01.mp3", 3, 3u, FIX_QUAD64, sizeof(FIX_QUAD64), 0);
    add_image("/M/folder.jpg", FIX_QUAD400, sizeof(FIX_QUAD400));
    add_image("/M/cover.jpg", FIX_QUAD64, sizeof(FIX_QUAD64));

    CHECK(art_find_nth(&io, "/M/01.mp3", 0, &r));
    CHECK(strcmp(r.path, "/M/cover.jpg") == 0);
    CHECK(art_find_nth(&io, "/M/01.mp3", 1, &r));
    CHECK(strcmp(r.path, "/M/folder.jpg") == 0);
    CHECK(art_find_nth(&io, "/M/01.mp3", 2, &r));
    CHECK_EQ(r.from, ART_FROM_ID3);
    CHECK(!art_find_nth(&io, "/M/01.mp3", 3, &r));
    CHECK_EQ(g_open_count, 0);
}

int main(void)
{
    printf("art_find\n");
    RUN(the_probe_tells_formats_apart_by_their_bytes);
    RUN(a_folder_cover_is_found_next_to_the_track);
    RUN(cover_is_preferred_over_folder);
    RUN(a_disc_subfolder_looks_in_the_album_folder_too);
    RUN(flac_embedded_art_is_found_and_the_front_cover_wins);
    RUN(id3_art_is_found_in_every_tag_version);
    RUN(a_utf16_description_is_stepped_over_correctly);
    RUN(a_decodable_embedded_cover_beats_an_undecodable_folder_one);
    RUN(undecodable_art_is_reported_for_the_log);
    RUN(found_art_decodes_through_the_reader);
    RUN(damaged_containers_never_hang_or_crash);
    RUN(bad_arguments_are_safe);
    RUN(later_candidates_can_be_asked_for_in_order);
    return TEST_SUMMARY();
}
