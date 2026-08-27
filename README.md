# dap-firmware

Firmware for a personal hi-fi digital audio player, built on an STM32H753
(NUCLEO-H753ZI for development, Custom Motherboard as the eventual target).

## Layout

| Folder | What it is | Status |
|---|---|---|
| `firmware/` | Portable core: ring buffer, WAV parser, fixed-point audio, encoder decode, player state machine, bitmap fonts, RGB565 framebuffer, themes, screens. No HAL — compiles and is unit-tested on a PC. | active |
| `cube/` | The STM32CubeIDE project. Pin configuration lives in the `.ioc`. This is the only place the ST HAL appears. | active |
| `sim/` | SDL2 desktop simulator (320x240) built before the hardware was ordered — themes, artist/album/track browser, now-playing screen. | **archived** — kept as a visual reference for the UI, no longer developed |

## Hardware

| Part | Role |
|---|---|
| NUCLEO-H753ZI | Cortex-M7 @ 480 MHz, onboard ST-LINK |
| PCM5102 | I2S DAC, line level |
| ST7789 1.3" 240x240 | SPI colour display, onboard microSD slot |
| PEC11R | Rotary encoder with pushbutton |
| PAM8302 | Mono class-D amp (bring-up only; the real build wants stereo) |

## Working on the portable core

```
cd firmware
make          # build + run all tests (~20,600 assertions)
make preview  # render real 240x240 screens to build/preview/*.ppm
make asan     # same tests under AddressSanitizer + UBSan
```

Needs only `gcc` and `make`. On Windows use MSYS2 MinGW64 or WSL.

## The design rule

**Firmware logic is hardware-independent and unit-tested on a PC.** Hardware
access is injected as a struct of function pointers (see `st7789_bus_t`): tests
plug in a mock recorder, the STM32 build plugs in HAL calls, and the driver
itself never changes.

Screens draw into an RGB565 framebuffer rather than straight to SPI. On hardware
that buffer goes out in one DMA transfer; on a PC it's written as an image. So
`make preview` shows the exact pixels the panel will receive — UI work is
finishable without the panel.

Only `firmware/src/platform/` and `cube/` are allowed to include the ST HAL.

## Regenerating from CubeMX

CubeMX rewrites everything outside `/* USER CODE BEGIN X */ ... /* USER CODE END X */`
markers. **Commit before every regeneration.**
