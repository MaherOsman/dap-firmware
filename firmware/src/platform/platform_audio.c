#include "platform_audio.h"
#include "ringbuf.h"
#include "player.h"
#include "wav.h"
#include "audio.h"
#include "ff.h"
#include <string.h>
#include <stdio.h>

/* ---- sizing -------------------------------------------------------------
 * DMA buffer: 1024 frames per half = 23 ms of audio at 44.1 kHz. The ISR has
 * that long to refill a half before it is played, which is enormous.
 * Ring: 64 KiB = 8192 frames = ~185 ms of slack against SD read jitter.
 * READ_BYTES is a multiple of 6 so every f_read lands on a frame boundary.
 */
#define HALF_FRAMES   1024u
#define HALF_SAMPLES  (HALF_FRAMES * 2u)
#define TOTAL_SAMPLES (HALF_SAMPLES * 2u)
#define RING_BYTES    65536u
#define READ_BYTES    6144u
#define UNPACK_SAMPLES (READ_BYTES / 3u)

/* Large buffers must be static — they must not land on the stack. */
static int32_t  g_dma[TOTAL_SAMPLES];
static uint8_t  g_ring_store[RING_BYTES];
static uint8_t  g_readbuf[READ_BYTES];
static int32_t  g_unpack[UNPACK_SAMPLES];

static ringbuf_t g_rb;
static player_t  g_pl;
static FIL       g_fil;
static wav_info_t g_info;
static SAI_HandleTypeDef *g_hsai;

static uint32_t g_bytes_left;      /* remaining in the data chunk */
static bool     g_file_open;
static bool     g_dma_running;

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
    if (!g_file_open || g_bytes_left == 0) {
        player_set_exhausted(&g_pl, true);
        return;
    }
    if (rb_free(&g_rb) < UNPACK_SAMPLES * sizeof(int32_t)) return;

    uint32_t want = READ_BYTES;
    if (want > g_bytes_left) want = g_bytes_left;
    want -= want % g_info.block_align;
    if (want == 0) { player_set_exhausted(&g_pl, true); return; }

    UINT br = 0;
    if (f_read(&g_fil, g_readbuf, want, &br) != FR_OK || br == 0) {
        player_set_exhausted(&g_pl, true);
        return;
    }
    g_bytes_left -= br;

    size_t samples = br / 3u;                 /* 24-bit: 3 bytes per sample */
    audio_unpack_s24(g_readbuf, g_unpack, samples);
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
    rb_init(&g_rb, g_ring_store, RING_BYTES);
    player_init(&g_pl);
    player_set_volume(&g_pl, 100);
}

int plat_audio_play(const char *path)
{
    plat_audio_stop();

    if (f_open(&g_fil, path, FA_READ) != FR_OK) {
        printf("audio: f_open failed\r\n");
        return -1;
    }
    g_file_open = true;

    UINT br = 0;
    if (f_read(&g_fil, g_readbuf, 4096, &br) != FR_OK) return -2;

    wav_err_t we = wav_parse(g_readbuf, br, &g_info);
    if (we != WAV_OK) {
        printf("audio: wav_parse: %s\r\n", wav_err_str(we));
        return -3;
    }
    printf("audio: %lu Hz, %u-bit, %u ch, %lu frames\r\n",
           (unsigned long)g_info.sample_rate, g_info.bits_per_sample,
           g_info.channels, (unsigned long)g_info.total_frames);

    /* The SAI is clocked for 44.1 kHz stereo 24-bit only, right now. */
    if (g_info.sample_rate != 44100 || g_info.channels != 2 ||
        g_info.bits_per_sample != 24) {
        printf("audio: unsupported format for this build\r\n");
        return -4;
    }

    if (f_lseek(&g_fil, g_info.data_offset) != FR_OK) return -5;
    g_bytes_left = g_info.data_bytes;

    rb_reset(&g_rb);
    g_isr_frames = 0;
    g_isr_underruns = 0;
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

    if (!g_pl.file_exhausted && player_needs_refill(&g_pl, pct))
        refill_from_file();

    if (!g_dma_running && player_output_enabled(&g_pl))
        start_dma();

    if (g_pl.file_exhausted && rb_is_empty(&g_rb) && g_dma_running) {
        plat_audio_stop();
        printf("audio: end of track, %lu underruns\r\n",
               (unsigned long)g_pl.underruns);
    }
}

void plat_audio_stop(void)
{
    if (g_dma_running) { HAL_SAI_DMAStop(g_hsai); g_dma_running = false; }
    if (g_file_open)   { f_close(&g_fil);         g_file_open = false; }
    player_stop(&g_pl);
    rb_reset(&g_rb);
}

bool plat_audio_is_active(void) { return g_pl.state != PLAYER_STOPPED; }
unsigned plat_audio_underruns(void) { return (unsigned)g_pl.underruns; }
unsigned plat_audio_position_ms(void) { return (unsigned)player_position_ms(&g_pl); }
