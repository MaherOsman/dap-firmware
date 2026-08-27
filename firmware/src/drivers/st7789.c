#include "st7789.h"

static void cmd(st7789_t *d, uint8_t c)
{
    d->bus->set_dc(d->bus->ctx, false);
    d->bus->write(d->bus->ctx, &c, 1);
    d->bus->set_dc(d->bus->ctx, true);
}

static void data(st7789_t *d, const uint8_t *p, size_t n)
{
    d->bus->set_dc(d->bus->ctx, true);
    d->bus->write(d->bus->ctx, p, n);
}

static void cmd8(st7789_t *d, uint8_t c, uint8_t arg)
{
    cmd(d, c);
    data(d, &arg, 1);
}

uint16_t st7789_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

void st7789_set_rotation(st7789_t *d, uint8_t rotation)
{
    uint8_t madctl;
    d->rotation = (uint8_t)(rotation & 3);

    /* The panel is a 240x240 window into a 240x320 controller. In the two
     * rotations where the origin lands at the far end of frame memory we
     * have to shift by 320-240 = 80. */
    switch (d->rotation) {
    case 0:
        madctl = MADCTL_RGB;
        d->x_off = 0; d->y_off = 0;
        break;
    case 1:
        madctl = MADCTL_MX | MADCTL_MV | MADCTL_RGB;
        d->x_off = 0; d->y_off = 0;
        break;
    case 2:
        madctl = MADCTL_MX | MADCTL_MY | MADCTL_RGB;
        d->x_off = 0; d->y_off = 80;
        break;
    default: /* 3 */
        madctl = MADCTL_MV | MADCTL_MY | MADCTL_RGB;
        d->x_off = 80; d->y_off = 0;
        break;
    }
    d->width  = ST7789_WIDTH;
    d->height = ST7789_HEIGHT;

    d->bus->set_cs(d->bus->ctx, true);
    cmd8(d, ST7789_MADCTL, madctl);
    d->bus->set_cs(d->bus->ctx, false);
}

void st7789_init(st7789_t *d, const st7789_bus_t *bus, uint8_t rotation)
{
    d->bus = bus;

    if (bus->set_reset) {
        bus->set_reset(bus->ctx, true);
        bus->delay_ms(bus->ctx, 10);
        bus->set_reset(bus->ctx, false);
        bus->delay_ms(bus->ctx, 120);
    }

    bus->set_cs(bus->ctx, true);

    cmd(d, ST7789_SWRESET);
    bus->delay_ms(bus->ctx, 150);

    cmd(d, ST7789_SLPOUT);
    bus->delay_ms(bus->ctx, 120);

    /* 16 bits/pixel, RGB565. 0x55 = 65k colours for both RGB and MCU. */
    cmd8(d, ST7789_COLMOD, 0x55);
    bus->delay_ms(bus->ctx, 10);

    /* Adafruit's 240x240 breakout ships with the panel inverted; without
     * INVON everything comes up as a photographic negative. This surprises
     * everyone exactly once. */
    cmd(d, ST7789_INVON);
    cmd(d, ST7789_NORON);
    bus->delay_ms(bus->ctx, 10);

    cmd(d, ST7789_DISPON);
    bus->delay_ms(bus->ctx, 10);

    bus->set_cs(bus->ctx, false);

    st7789_set_rotation(d, rotation);
}

void st7789_set_window(st7789_t *d, uint16_t x0, uint16_t y0, uint16_t x1,
                       uint16_t y1)
{
    uint16_t xs = (uint16_t)(x0 + d->x_off);
    uint16_t xe = (uint16_t)(x1 + d->x_off);
    uint16_t ys = (uint16_t)(y0 + d->y_off);
    uint16_t ye = (uint16_t)(y1 + d->y_off);

    uint8_t buf[4];

    cmd(d, ST7789_CASET);
    buf[0] = (uint8_t)(xs >> 8); buf[1] = (uint8_t)xs;
    buf[2] = (uint8_t)(xe >> 8); buf[3] = (uint8_t)xe;
    data(d, buf, 4);

    cmd(d, ST7789_RASET);
    buf[0] = (uint8_t)(ys >> 8); buf[1] = (uint8_t)ys;
    buf[2] = (uint8_t)(ye >> 8); buf[3] = (uint8_t)ye;
    data(d, buf, 4);

    cmd(d, ST7789_RAMWR);
}

void st7789_write_pixels(st7789_t *d, const uint16_t *px, size_t count)
{
    /* The panel takes RGB565 MSB-first. Chunk the byte-swap so this needs
     * no allocation and stays friendly to a DMA-backed write later. */
    uint8_t chunk[64];
    size_t i = 0;
    d->bus->set_dc(d->bus->ctx, true);
    while (i < count) {
        size_t n = count - i;
        if (n > sizeof(chunk) / 2) {
            n = sizeof(chunk) / 2;
        }
        for (size_t k = 0; k < n; k++) {
            chunk[2 * k]     = (uint8_t)(px[i + k] >> 8);
            chunk[2 * k + 1] = (uint8_t)(px[i + k]);
        }
        d->bus->write(d->bus->ctx, chunk, n * 2);
        i += n;
    }
}

void st7789_fill_rect(st7789_t *d, uint16_t x, uint16_t y, uint16_t w,
                      uint16_t h, uint16_t color)
{
    if (w == 0 || h == 0 || x >= d->width || y >= d->height) {
        return;
    }
    if (x + w > d->width) {
        w = (uint16_t)(d->width - x);
    }
    if (y + h > d->height) {
        h = (uint16_t)(d->height - y);
    }

    d->bus->set_cs(d->bus->ctx, true);
    st7789_set_window(d, x, y, (uint16_t)(x + w - 1), (uint16_t)(y + h - 1));

    uint8_t line[64];
    for (size_t k = 0; k < sizeof(line) / 2; k++) {
        line[2 * k]     = (uint8_t)(color >> 8);
        line[2 * k + 1] = (uint8_t)(color);
    }
    size_t remaining = (size_t)w * h;
    d->bus->set_dc(d->bus->ctx, true);
    while (remaining) {
        size_t n = remaining > sizeof(line) / 2 ? sizeof(line) / 2 : remaining;
        d->bus->write(d->bus->ctx, line, n * 2);
        remaining -= n;
    }
    d->bus->set_cs(d->bus->ctx, false);
}

void st7789_fill_screen(st7789_t *d, uint16_t color)
{
    st7789_fill_rect(d, 0, 0, d->width, d->height, color);
}
