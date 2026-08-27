# dap-firmware — portable core

Hardware-independent firmware for an STM32H7-based digital audio player.
Everything in `src/core/` and `src/drivers/` compiles and is unit-tested with
a plain PC compiler, so the logic can be finished and debugged **before** the
Nucleo-H753ZI arrives.

```
make          # build + run all tests
make preview  # render real 240x240 screens to build/preview/*.ppm
make asan     # same tests, with AddressSanitizer + UBSan
make clean
```

Requires only `gcc` (or `clang`) and `make`. On Windows, use the MSYS2 MinGW64
shell or WSL — see the setup guide.

## Layout

| Path | What it is | Depends on hardware? |
|---|---|---|
| `src/core/ringbuf.*` | SPSC lock-free ring buffer — the SD-reader → I2S-DMA handoff | no |
| `src/core/wav.*` | Streaming RIFF/WAVE parser (PCM 8/16/24/32, float, EXTENSIBLE) | no |
| `src/core/audio.*` | Sample unpacking to Q1.31, fixed-point log volume, peak meter | no |
| `src/core/encoder.*` | PEC11R quadrature decode + button debounce/long-press | no |
| `src/core/player.*` | Playback state machine, preroll and refill policy | no |
| `src/core/font.*` | 1-bit proportional bitmap fonts + ellipsis truncation | no |
| `src/core/font_data.c` | Roboto 11/14/18 baked to C arrays (~6 KB flash) | no |
| `src/core/gfx.*` | RGB565 framebuffer, rects, rounded rects, text | no |
| `src/core/theme.*` | Dark / Warm / iPod themes, ported from the simulator | no |
| `src/ui/screen_library.*` | Artist→album→track browser, drawn to a framebuffer | no |
| `src/drivers/st7789.*` | 240x240 TFT driver over an injected bus struct | via `st7789_bus_t` |
| `src/platform/` | STM32 glue goes here — the only folder that includes the HAL | yes |
| `tools/fontgen.py` | Regenerates `font_data.c` from a TTF | no |
| `tools/preview.c` | Renders real screens to images so they can be eyeballed | no |
| `tests/` | ~20,600 assertions, no external test framework | no |

## The preview loop

`make preview` composes real screens into a 240x240 RGB565 framebuffer and
writes them out as images. Those are the exact pixels the ST7789 will receive
— so UI work can be finished and judged before the panel exists.

## The porting contract

`st7789_t` takes a `st7789_bus_t` of five function pointers. On the host the
tests plug in a recorder that asserts on the exact bytes emitted; on the
Nucleo you plug in `HAL_SPI_Transmit`, `HAL_GPIO_WritePin`, and `HAL_Delay`.
The driver itself never changes. Add the same shape of bus struct for the SD
card and the I2S output as those get written.

Two things to define in the STM32 build that the host build leaves as no-ops:

```c
#define RB_PUBLISH_BARRIER() __DMB()   /* before ringbuf.h */
```

...and put audio DMA buffers in a non-cached region (or clean/invalidate
around every transfer). See the setup guide's cache-coherency section — on an
H7 this is the difference between working audio and intermittent noise.
