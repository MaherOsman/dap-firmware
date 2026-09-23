/* platform_art.c — see platform_art.h. */

#include "platform_art.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "main.h"
#include "art.h"
#include "art_find.h"
#include "screen_now_playing.h"
#include "platform_audio.h"
#include "platform_libio.h"

/* Uninitialised, in D2 SRAM. The linker script's .ram_d2 section is NOLOAD,
 * so start-up neither copies nor zeroes it — plat_art_init() does. If that
 * section is ever missing from the linker script, these land after .bss in
 * AXI SRAM, overflow it, and the link fails: loud, not silent. */
#define IN_D2 __attribute__((section(".ram_d2"), aligned(32)))

static art_work_t g_work IN_D2;
static uint16_t   g_img[NP_ART_LARGE * NP_ART_LARGE] IN_D2;

static bool     g_ready;
static bool     g_key_valid;   /* g_album/g_size describe g_img */
static uint32_t g_album;
static int      g_size;
static bool     g_have;        /* ...and it decoded successfully */

void plat_art_init(void)
{
    /* The D2 SRAM blocks have their own clock enables. Nothing else in the
     * project uses this memory yet, so turn them on before touching it. */
    __HAL_RCC_D2SRAM1_CLK_ENABLE();
    __HAL_RCC_D2SRAM2_CLK_ENABLE();
    __HAL_RCC_D2SRAM3_CLK_ENABLE();

    memset(&g_work, 0, sizeof(g_work));
    memset(g_img, 0, sizeof(g_img));
    g_key_valid = false;
    g_have = false;
    g_ready = true;
}

void plat_art_forget(void)
{
    g_key_valid = false;
    g_have = false;
}

static void yield_to_audio(void *ctx)
{
    (void)ctx;
    plat_audio_service();
}

const uint16_t *plat_art_for(uint32_t album, const char *track_path, int size)
{
    const lib_io_t *io = plat_libio_shared();
    art_ref_t ref;
    art_reader_t rd;
    art_src_t src;
    art_info_t info;
    art_result_t r;
    uint32_t t0;

    if (!g_ready || track_path == NULL || track_path[0] == '\0' ||
        size < 1 || size > NP_ART_LARGE || io == NULL) {
        return NULL;
    }

    if (g_key_valid && album == g_album && size == g_size) {
        return g_have ? g_img : NULL;
    }

    /* Claim the key before trying, so a failure is remembered too and an
     * album without art is not re-searched on every repaint. */
    g_key_valid = true;
    g_album = album;
    g_size = size;
    g_have = false;

    t0 = HAL_GetTick();
    if (!art_find(io, track_path, &ref)) {
        if (ref.fmt == ART_FMT_NONE) {
            printf("art: none found for %s\r\n", track_path);
        } else {
            printf("art: %s %s not supported yet: %s\r\n",
                   art_from_name(ref.from), art_fmt_name(ref.fmt), ref.path);
        }
        return NULL;
    }

    if (!art_open(io, &ref, &rd, &src, yield_to_audio, NULL)) {
        printf("art: could not open %s\r\n", ref.path);
        return NULL;
    }
    r = art_decode_jpeg(&src, g_img, size, &g_work, &info);
    art_close(&rd);

    if (r != ART_OK) {
        printf("art: decode failed (%s): %s\r\n", art_result_name(r),
               ref.path);
        return NULL;
    }

    printf("art: %s, %ux%u -> %d px (1/%u) in %lu ms: %s\r\n",
           art_from_name(ref.from), (unsigned)info.src_w,
           (unsigned)info.src_h, size, 1u << info.scale,
           (unsigned long)(HAL_GetTick() - t0), ref.path);
    g_have = true;
    return g_img;
}
