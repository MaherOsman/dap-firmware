/*
 * Verifies the ST7789 driver against a mock SPI bus. This is the closest you
 * can get to bringing up a display before the display exists: every command
 * byte, every window coordinate, and the rotation offsets are checked here,
 * so when the panel arrives the remaining unknowns are wiring and clock
 * speed, not driver logic.
 */
#include "test.h"
#include "../src/drivers/st7789.h"

#define LOG_MAX 8192

typedef struct {
    uint8_t bytes[LOG_MAX];
    bool    is_cmd[LOG_MAX];
    size_t  n;           /* bytes captured (capped at LOG_MAX) */
    size_t  total;       /* bytes written, uncapped */
    bool    dc_data;
    bool    cs;
    int     resets;
    uint32_t delay_total;
} mock_t;

static void m_write(void *ctx, const uint8_t *d, size_t len)
{
    mock_t *m = ctx;
    m->total += len;
    for (size_t i = 0; i < len && m->n < LOG_MAX; i++) {
        m->bytes[m->n]  = d[i];
        m->is_cmd[m->n] = !m->dc_data;
        m->n++;
    }
}
static void m_dc(void *ctx, bool data)   { ((mock_t *)ctx)->dc_data = data; }
static void m_cs(void *ctx, bool sel)    { ((mock_t *)ctx)->cs = sel; }
static void m_rst(void *ctx, bool a)     { if (a) ((mock_t *)ctx)->resets++; }
static void m_delay(void *ctx, uint32_t ms) { ((mock_t *)ctx)->delay_total += ms; }

static mock_t mock;
static st7789_bus_t bus;
static st7789_t dev;

static void setup(uint8_t rotation)
{
    memset(&mock, 0, sizeof(mock));
    bus.write     = m_write;
    bus.set_dc    = m_dc;
    bus.set_cs    = m_cs;
    bus.set_reset = m_rst;
    bus.delay_ms  = m_delay;
    bus.ctx       = &mock;
    st7789_init(&dev, &bus, rotation);
}

/* Find the index of the Nth occurrence of a command byte. */
static long find_cmd(uint8_t c, int nth)
{
    int seen = 0;
    for (size_t i = 0; i < mock.n; i++) {
        if (mock.is_cmd[i] && mock.bytes[i] == c) {
            if (++seen > nth) {
                return (long)i;
            }
        }
    }
    return -1;
}

TEST(init_emits_the_required_sequence)
{
    setup(0);
    CHECK_EQ(mock.resets, 1);
    long swreset = find_cmd(ST7789_SWRESET, 0);
    long slpout  = find_cmd(ST7789_SLPOUT, 0);
    long colmod  = find_cmd(ST7789_COLMOD, 0);
    long dispon  = find_cmd(ST7789_DISPON, 0);
    CHECK(swreset >= 0 && slpout > swreset && colmod > slpout &&
          dispon > colmod);
    /* 16 bpp */
    CHECK_EQ(mock.bytes[colmod + 1], 0x55);
    /* Enough settle time after reset+sleep-out or the panel ignores us. */
    CHECK(mock.delay_total >= 250);
}

TEST(init_turns_inversion_on)
{
    setup(0);
    /* Without INVON this panel displays a negative image. */
    CHECK(find_cmd(ST7789_INVON, 0) >= 0);
}

TEST(rotation_offsets_are_right)
{
    setup(0);
    CHECK_EQ(dev.x_off, 0);
    CHECK_EQ(dev.y_off, 0);

    st7789_set_rotation(&dev, 2);
    CHECK_EQ(dev.y_off, 80); /* 320 - 240 */
    CHECK_EQ(dev.x_off, 0);

    st7789_set_rotation(&dev, 3);
    CHECK_EQ(dev.x_off, 80);
    CHECK_EQ(dev.y_off, 0);

    st7789_set_rotation(&dev, 1);
    CHECK_EQ(dev.x_off, 0);
    CHECK_EQ(dev.y_off, 0);
}

TEST(window_coordinates_are_inclusive_and_offset)
{
    setup(0);
    mock.n = 0;
    st7789_set_window(&dev, 10, 20, 29, 49);

    long caset = find_cmd(ST7789_CASET, 0);
    CHECK(caset >= 0);
    CHECK_EQ(mock.bytes[caset + 1], 0);  CHECK_EQ(mock.bytes[caset + 2], 10);
    CHECK_EQ(mock.bytes[caset + 3], 0);  CHECK_EQ(mock.bytes[caset + 4], 29);

    long raset = find_cmd(ST7789_RASET, 0);
    CHECK_EQ(mock.bytes[raset + 1], 0);  CHECK_EQ(mock.bytes[raset + 2], 20);
    CHECK_EQ(mock.bytes[raset + 3], 0);  CHECK_EQ(mock.bytes[raset + 4], 49);

    /* RAMWR must be the last thing before pixel data. */
    CHECK(find_cmd(ST7789_RAMWR, 0) > raset);
}

TEST(rotation_2_shifts_rows_by_80)
{
    setup(2);
    mock.n = 0;
    st7789_set_window(&dev, 0, 0, 239, 239);
    long raset = find_cmd(ST7789_RASET, 0);
    CHECK_EQ(mock.bytes[raset + 2], 80);
    CHECK_EQ((mock.bytes[raset + 3] << 8) | mock.bytes[raset + 4], 319);
}

TEST(pixels_go_out_msb_first)
{
    setup(0);
    mock.n = 0;
    uint16_t px[2] = {0xF800, 0x001F}; /* red, blue */
    st7789_write_pixels(&dev, px, 2);
    CHECK_EQ(mock.n, 4);
    CHECK_EQ(mock.bytes[0], 0xF8); CHECK_EQ(mock.bytes[1], 0x00);
    CHECK_EQ(mock.bytes[2], 0x00); CHECK_EQ(mock.bytes[3], 0x1F);
}

TEST(fill_rect_writes_exactly_w_times_h_pixels)
{
    setup(0);
    mock.n = 0;
    st7789_fill_rect(&dev, 5, 5, 10, 4, 0x1234);

    long ramwr = find_cmd(ST7789_RAMWR, 0);
    CHECK(ramwr >= 0);
    size_t pixel_bytes = mock.n - (size_t)(ramwr + 1);
    CHECK_EQ(pixel_bytes, 10u * 4u * 2u);
    /* And the colour is right, MSB first. */
    CHECK_EQ(mock.bytes[ramwr + 1], 0x12);
    CHECK_EQ(mock.bytes[ramwr + 2], 0x34);
}

TEST(fill_rect_clips_to_the_panel)
{
    setup(0);
    mock.n = 0;
    st7789_fill_rect(&dev, 230, 230, 100, 100, 0);
    long ramwr = find_cmd(ST7789_RAMWR, 0);
    size_t pixel_bytes = mock.n - (size_t)(ramwr + 1);
    CHECK_EQ(pixel_bytes, 10u * 10u * 2u); /* clipped to 10x10, not 100x100 */

    mock.n = 0;
    st7789_fill_rect(&dev, 300, 300, 10, 10, 0); /* fully offscreen */
    CHECK_EQ(mock.n, 0);
}

TEST(full_screen_fill_is_240x240)
{
    setup(0);
    mock.n = 0;
    mock.total = 0;
    st7789_fill_screen(&dev, 0x0000);
    /* 11 bytes of CASET/RASET/RAMWR overhead, then a full frame.
     * 115200 bytes at 30 MHz SPI is ~31 ms — which is why the real firmware
     * will want DMA and partial redraws, not a full clear every frame. */
    CHECK_EQ(mock.total, 11u + 240u * 240u * 2u);
}

TEST(rgb565_packing)
{
    CHECK_EQ(st7789_rgb(255, 0, 0), 0xF800);
    CHECK_EQ(st7789_rgb(0, 255, 0), 0x07E0);
    CHECK_EQ(st7789_rgb(0, 0, 255), 0x001F);
    CHECK_EQ(st7789_rgb(255, 255, 255), 0xFFFF);
    CHECK_EQ(st7789_rgb(0, 0, 0), 0x0000);
}

int main(void)
{
    printf("st7789\n");
    RUN(init_emits_the_required_sequence);
    RUN(init_turns_inversion_on);
    RUN(rotation_offsets_are_right);
    RUN(window_coordinates_are_inclusive_and_offset);
    RUN(rotation_2_shifts_rows_by_80);
    RUN(pixels_go_out_msb_first);
    RUN(fill_rect_writes_exactly_w_times_h_pixels);
    RUN(fill_rect_clips_to_the_panel);
    RUN(full_screen_fill_is_240x240);
    RUN(rgb565_packing);
    return TEST_SUMMARY();
}
