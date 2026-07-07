#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// INMP441 an I2S0 RX (Spec Section 3: SCK=5, WS=4, SD=6),
// 16 kHz mono. Abgeleitet aus mic_test.c (Phase 0).
esp_err_t audio_in_init(void);

// Liest blockierend `samples` Samples vom Mikrofon und konvertiert
// sie nach 16-bit signed (Format, das die AFE-Pipeline erwartet).
esp_err_t audio_in_read(int16_t *dest, size_t samples);
