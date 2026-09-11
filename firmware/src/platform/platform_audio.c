#include "platform_audio.h"
#include "ringbuf.h"
#include "player.h"
#include "decoder.h"
#include "audio.h"
#include "ff.h"
#include <string.h>
#include <stdio.h>

/* ---- sizing -------------------------------------------------------------
 * DMA buffer: 1024 frames per half = 23 ms of audio at 44.1 kHz. The ISR has
 * that long to refill a half before it is played, which is enormous.
 * Ring: 64 KiB = 8192 frames = ~185 ms of slack against SD read jitter.
 * The decoder owns frame alignment now; this buffer just holds its output.
 */
#define HALF_FRAMES   1024u
#define HALF_SAMPLES  (HALF_FRAMES * 2u)
#define TOTAL_SAMPLES (HALF_SAMPLES * 2u)
#define RING_BYTES    65536u
#define UNPACK_SAMPLES 2048u

/* Large buffers must be static — they must not land on the stack. */
static int32_t  g_dma[TOTAL_SAMPLES];
static uint8_t  g_ring_store[RING_BYTES];
static int32_t  g_unpack[UNPACK_SAMPLES];

static ringbuf_t g_rb;
static player_t  g_pl;
static FIL       g_fil;
static decoder_t g_dec;
static SAI_HandleTypeDef *g_hsai;

static decoder_info_t g_info;      /* remaining in the data chunk */
static bool     g_file_open;
static bool     g_dma_running;
static uint32_t g_refill_max_ms;

/* Written by the ISR, drained by the main loop. */
static volatile uint32_t g_isr_frames;
static volatile uint32_t g_isr_underruns;

static uint8_t buffer_pct(void)
{
    return (uint8_t)((rb_used(&g_rb) * 100u) / RING_BYTES);
}

/* ---- consumer side: runs in the DMA interrupt --------------------------- */
static void fill_half(int32_t *half)
{
    const size_t want = HALF_SAMPLES * sizeof(int32_t);
    size_t got = rb_read(&g_rb, (uint8_t *)half, want);
    if (got < want) {
        /* Silence, never stale samples — a repeated fragment is far more
         * audible than a gap, and it hides the fault. */
        memset((uint8_t *)half + got, 0, want - got);
        g_isr_underruns++;
    }
    g_isr_frames += (uint32_t)(got / (2u * sizeof(int32_t)));
}

void HAL_SAI_TxHalfCpltCallback(SAI_HandleTypeDef *hsai)
{
    (void)hsai;
    fill_half(&g_dma[0]);
}

void HAL_SAI_TxCpltCallback(SAI_HandleTypeDef *hsai)
{
    (void)hsai;
    fill_half(&g_dma[HALF_SAMPLES]);
}

/* ---- producer side: runs in the main loop ------------------------------- */
static void refill_from_file(void)
{
    if (!g_file_open) { player_set_exhausted(&g_pl, true); return; }

    const size_t frames_room = rb_free(&g_rb) / (2u * sizeof(int32_t));
    if (frames_room < 256) return;

    size_t want = UNPACK_SAMPLES / 2u;      /* frames the scratch can hold */
    if (want > frames_room) want = frames_room;

    size_t got = decoder_decode(&g_dec, g_unpack, want);
    if (got == 0) { player_set_exhausted(&g_pl, true); return; }

    size_t samples = got * 2u;
    if (g_pl.volume < 100)
        audio_apply_gain(g_unpack, samples, audio_volume_q16(g_pl.volume));

    rb_write(&g_rb, (const uint8_t *)g_unpack, samples * sizeof(int32_t));
}

static void start_dma(void)
{
    /* Preload both halves so the very first bytes out are real audio. */
    fill_half(&g_dma[0]);
    fill_half(&g_dma[HALF_SAMPLES]);
    if (HAL_SAI_Transmit_DMA(g_hsai, (uint8_t *)g_dma, TOTAL_SAMPLES) == HAL_OK)
        g_dma_running = true;
    else
        printf("audio: DMA start failed\r\n");
}

void plat_audio_init(SAI_HandleTypeDef *hsai)

{
    g_hsai = hsai;
    decoder_registry_clear();
    decoder_register(&decoder_wav_vt);
    decoder_register(&decoder_flac_vt);
    decoder_register(&decoder_wav_vt);
    decoder_register(&decoder_flac_vt);
    decoder_register(&decoder_mp3_vt);
    rb_init(&g_rb, g_ring_store, RING_BYTES);
    player_init(&g_pl);
    player_set_volume(&g_pl, 20);
}

static size_t fatfs_read(void *ctx, void *dst, size_t n)
{
    UINT br = 0;
    if (f_read((FIL *)ctx, dst, (UINT)n, &br) != FR_OK) return 0;
    return (size_t)br;
}

static bool fatfs_seek(void *ctx, uint32_t off)
{
    return f_lseek((FIL *)ctx, off) == FR_OK;
}

static uint32_t fatfs_size(void *ctx)
{
    return (uint32_t)f_size((FIL *)ctx);
}

int plat_audio_play(const char *path)
{
    plat_audio_stop();

    if (f_open(&g_fil, path, FA_READ) != FR_OK) {
        printf("audio: f_open failed\r\n");
        return -1;
    }
    g_file_open = true;

    decoder_io_t io = { fatfs_read, fatfs_seek, fatfs_size, &g_fil };
    if (!decoder_open(&g_dec, io, &g_info)) {
        printf("audio: no decoder for this file\r\n");
        return -2;
    }

    printf("audio: %s, %lu Hz, %u-bit, %u ch, %lu frames\r\n",
           decoder_name(&g_dec),
           (unsigned long)g_info.sample_rate, g_info.bits_per_sample,
           g_info.channels, (unsigned long)g_info.total_frames);

    if (g_info.sample_rate != 44100) {
        printf("audio: SAI is clocked for 44.1 kHz only\r\n");
        return -3;
    }

    rb_reset(&g_rb);
    g_isr_underruns = 0;
    g_refill_max_ms = 0;
    player_open(&g_pl, g_info.total_frames, g_info.sample_rate);
    return 0;
}

void plat_audio_service(void)
{
    if (g_pl.state == PLAYER_STOPPED) return;

    uint32_t f, u;
    __disable_irq();
    f = g_isr_frames;    g_isr_frames = 0;
    u = g_isr_underruns; g_isr_underruns = 0;
    __enable_irq();

    if (f) player_frames_consumed(&g_pl, f);
    while (u--) player_underrun(&g_pl);

    uint8_t pct = buffer_pct();
    player_tick(&g_pl, pct);

    if (!g_pl.file_exhausted && player_needs_refill(&g_pl, pct)) {
        uint32_t t0 = HAL_GetTick();
        refill_from_file();
        uint32_t dt = HAL_GetTick() - t0;
        if (dt > g_refill_max_ms) g_refill_max_ms = dt;
    }
    if (!g_dma_running && player_output_enabled(&g_pl))
        start_dma();

    if (g_pl.file_exhausted && rb_is_empty(&g_rb) && g_dma_running) {
        plat_audio_stop();
        printf("audio: end of track, %lu underruns, worst refill %lu ms\r\n",
               (unsigned long)g_pl.underruns,
               (unsigned long)g_refill_max_ms);
    }

    static uint32_t last_report;
    if (HAL_GetTick() - last_report > 2000) {
        last_report = HAL_GetTick();
        printf("audio: buf %u%%, underruns %lu, worst refill %lu ms\r\n",
               pct, (unsigned long)g_pl.underruns,
               (unsigned long)g_refill_max_ms);
    }
}

void plat_audio_stop(void)
{
    if (g_dma_running) { HAL_SAI_DMAStop(g_hsai); g_dma_running = false; }
    if (g_file_open)   { f_close(&g_fil);         g_file_open = false; }
    player_stop(&g_pl);
    rb_reset(&g_rb);
    decoder_close(&g_dec);
}

bool plat_audio_is_active(void) { return g_pl.state != PLAYER_STOPPED; }
unsigned plat_audio_underruns(void) { return (unsigned)g_pl.underruns; }
unsigned plat_audio_position_ms(void) { return (unsigned)player_position_ms(&g_pl); }
