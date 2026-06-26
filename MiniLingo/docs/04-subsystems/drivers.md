# Subsystem: Drivers

Hardware abstraction for the four peripherals. Each driver was first validated
by a standalone Phase-0 test program (`main/*_test.c`) and then distilled into
the driver used by the app. Files: [drivers/](../../main/drivers/).

## Pin map

| Peripheral | Part | Bus | Pins | Test program |
|---|---|---|---|---|
| Microphone | INMP441 | I2S0 RX | WS=4, SCK=5, SD=6 (L/R→GND) | [mic_test.c](../../main/mic_test.c) |
| Amplifier | MAX98357A | I2S1 TX | BCLK=15, LRC=16, DIN=17 (SD→3V3, GAIN open) | [amp_test.c](../../main/amp_test.c) |
| Display | ILI9341 | SPI2 | SCK=40, MOSI=41, MISO=39, CS=1, DC=42, RST=2 | [display_test.c](../../main/display_test.c) |
| Touch | XPT2046 | SPI2 (shared) | CS=38, SCK/MOSI/MISO shared with display, IRQ unused | [touch_test.c](../../main/touch_test.c) |

Mic and amp sit on the left header (GPIO4-17); display and touch were moved to
the right header (GPIO1/2/38-42) to keep the breadboard wiring on two sides
instead of one crowded edge. Display and touch share SPI2 with separate
chip-selects. **GPIO35/36/37 are off-limits** on this N16R8 board even though
they're broken out on the header — the chip uses them internally for the
octal PSRAM, and GPIO0/19/20/43/44 are reserved for boot-strapping/USB/UART.
Full wiring tables (with notes like the series-speaker arrangement) are in the
archived [COMPONENT_TESTS-de.md](../archive/COMPONENT_TESTS-de.md).

## Microphone — audio_in

[audio_in.c](../../main/drivers/audio_in.c). INMP441 on **I2S0** in master-RX,
Philips format, 16 kHz, mono, left slot.

- The INMP441 delivers 24 valid bits left-justified in a 32-bit frame.
  `audio_in_read()` reads 32-bit samples into a scratch buffer and converts to
  16-bit with `>> SAMPLE_SHIFT` where **`SAMPLE_SHIFT = 14`** (not the neutral
  16) — that's **+12 dB** with saturation, because the mic is quiet. Tune this
  (15 or 16) if the wake word over-saturates.
- The scratch buffer (internal RAM) grows to the largest requested chunk
  (effectively the AFE feed-chunk size) and is reused.

## Amplifier — audio_out

[audio_out.c](../../main/drivers/audio_out.c). MAX98357A on **I2S1** in
master-TX, 16-bit, mono, left slot. Default rate 16 kHz, reconfigurable.

- **`auto_clear = true`** on the channel. Without it, the DMA loops the last
  buffer's contents forever after the final write → a "rattle" after every
  playback. `auto_clear` zeroes sent descriptors.
- **Software volume**: the MAX98357A has no digital gain input, so
  `audio_out_write()` multiplies each sample by a Q15 gain in place
  (`(sample * gain_q15) >> 15`) before writing. Gain is an atomic, so it can be
  changed from any task. Unity gain = `AUDIO_OUT_GAIN_UNITY`.
- **`audio_out_set_rate()`** disables, reconfigures the clock, and re-enables
  the channel — used to switch between 16 kHz canned audio and the TTS rate
  (~22 kHz from the local Piper WAV header, 24 kHz from the Gemini fallback).
  Only the caller (the player task) uses this, so there's no locking.

> **Do not enlarge the I2S-DMA buffer** to smooth playback — it starved the TLS
> hardware-AES of internal RAM and broke cloud uploads. See
> [07-lessons-learned.md](../07-lessons-learned.md).

## Display — display

[display.c](../../main/drivers/display.c). ILI9341 (240×320) on **SPI2** at
20 MHz via the `espressif/esp_lcd_ili9341` managed component, bound to LVGL v9.

- LVGL renders into two partial buffers of 40 lines each; `flush_cb` pushes the
  area to the panel via `esp_lcd_panel_draw_bitmap`. Flush completion is
  signalled asynchronously from `notify_flush_ready`.
- **Byte swap**: the ILI9341 expects big-endian RGB565. LVGL v9 dropped
  `LV_COLOR_16_SWAP`, so the swap happens in the flush path via
  `lv_draw_sw_rgb565_swap()`.
- **The panel is BGR**, not RGB — `display.c` uses the BGR color space.
  With RGB, red and blue were swapped (it went unnoticed at first because grey
  and green survive the R/B swap nearly unchanged).

## Touch — touch

[touch.c](../../main/drivers/touch.c). XPT2046 on the **same SPI2 bus** as the
display, its own chip-select (GPIO14), as an LVGL input device. Polled (IRQ pin
not connected). Resistive touch is coarse, which is why on-screen targets like
the volume slider get an extended click area; no calibration screen was needed.

## Design notes & gotchas

- The drivers were derived from the Phase-0 tests, which means the test
  programs are still the simplest reference for each peripheral in isolation.
- A wiring short between amp BCLK (GPIO15) and mic SD (GPIO6) once made the mic
  read constant full-scale once I2S1 started clocking. If "mic reads constant
  max", suspect wiring and check with a GPIO toggle. Full story in
  [07-lessons-learned.md](../07-lessons-learned.md).
