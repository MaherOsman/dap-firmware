/*
 * st7789 — driver for the Adafruit 1.3" 240x240 IPS TFT.
 *
 * The driver never touches an STM32 register. All hardware access goes
 * through a st7789_bus_t of function pointers, so:
 *   - on the Nucleo you plug in HAL_SPI_Transmit / HAL_GPIO_WritePin,
 *   - in the test suite you plug in a recorder and assert on the exact
 *     command bytes and window coordinates the driver emitted.
 *
 * That second point matters more than it sounds: display bring-up failures
 * are almost always "wrong init sequence" or "wrong column offset", and both
 * are things you can get right before the board arrives.
 *
 * Note on this specific panel: the 240x240 ST7789 has no offset in its
 * default rotation, but rotations 2 and 3 need a 80-pixel row offset because
 * the controller's frame memory is 240x320. Getting this wrong shows up as
 * a picture shifted off the bottom of the screen.
 */
#ifndef ST7789_H
#define ST7789_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ST7789_WIDTH  240
#define ST7789_HEIGHT 240

/* Commands used by this driver. */
enum {
    ST7789_SWRESET = 0x01,
    ST7789_SLPOUT  = 0x11,
    ST7789_NORON   = 0x13,
    ST7789_INVON   = 0x21,
    ST7789_DISPON  = 0x29,
    ST7789_CASET   = 0x2A,
    ST7789_RASET   = 0x2B,
    ST7789_RAMWR   = 0x2C,
    ST7789_MADCTL  = 0x36,
    ST7789_COLMOD  = 0x3A
};

/* MADCTL bits */
enum {
    MADCTL_MY  = 0x80,
    MADCTL_MX  = 0x40,
    MADCTL_MV  = 0x20,
    MADCTL_ML  = 0x10,
    MADCTL_RGB = 0x00,
    MADCTL_BGR = 0x08
};

typedef struct {
    /* Blocking write of `len` bytes to the panel. */
    void (*write)(void *ctx, const uint8_t *data, size_t len);
    /* Data/Command pin: true == data, false == command. */
    void (*set_dc)(void *ctx, bool data);
    /* Chip select, active low; the driver passes true to assert. */
    void (*set_cs)(void *ctx, bool selected);
    /* Hardware reset pin, or NULL if the panel's RST is tied high. */
    void (*set_reset)(void *ctx, bool asserted);
    /* Millisecond delay. */
    void (*delay_ms)(void *ctx, uint32_t ms);
    void *ctx;
} st7789_bus_t;

typedef struct {
    const st7789_bus_t *bus;
    uint8_t  rotation;   /* 0..3 */
    uint16_t x_off;
    uint16_t y_off;
    uint16_t width;
    uint16_t height;
} st7789_t;

void st7789_init(st7789_t *d, const st7789_bus_t *bus, uint8_t rotation);
void st7789_set_rotation(st7789_t *d, uint8_t rotation);

/* Set the drawing window and leave the panel ready to receive pixels. */
void st7789_set_window(st7789_t *d, uint16_t x0, uint16_t y0, uint16_t x1,
                       uint16_t y1);

/* Push RGB565 pixels (big-endian on the wire, as the panel expects). */
void st7789_write_pixels(st7789_t *d, const uint16_t *px, size_t count);
void st7789_fill_rect(st7789_t *d, uint16_t x, uint16_t y, uint16_t w,
                      uint16_t h, uint16_t color);
void st7789_fill_screen(st7789_t *d, uint16_t color);

/* Colour helper: 8-8-8 in, RGB565 out. */
uint16_t st7789_rgb(uint8_t r, uint8_t g, uint8_t b);

#endif /* ST7789_H */
