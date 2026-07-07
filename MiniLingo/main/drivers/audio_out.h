#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// Software-Gain in Q15: 0 = stumm, 32768 = 1.0 (unveraendert).
#define AUDIO_OUT_GAIN_UNITY 32768

// MAX98357A an I2S1 TX (Pins aus amp_test.c, Phase 0). Startet mit
// 16 kHz / 16 bit / mono; die Rate wechselt audio_out_set_rate().
esp_err_t audio_out_init(void);

// Stellt die Sample-Rate um (no-op, wenn schon aktiv). Nicht waehrend
// eines laufenden audio_out_write() aufrufen — es gibt genau einen
// Player (canned_audio-Task), der beides sequenziell macht.
esp_err_t audio_out_set_rate(uint32_t sample_rate);

// Schreibt Samples blockierend nach I2S1 und wendet dabei den
// aktuellen Gain an (veraendert den uebergebenen Buffer!).
esp_err_t audio_out_write(int16_t *samples, size_t count);

// Gain thread-safe setzen/lesen (atomar) — Owner der Lautstaerke-Logik
// ist local/volume.c, hier liegt nur der rohe Multiplikator.
void audio_out_set_gain_q15(int32_t gain);
int32_t audio_out_get_gain_q15(void);
