#pragma once                          // [C] Include-Guard-Ersatz: verhindert Mehrfach-Einbindung dieses Headers

#include <stddef.h>                    // [C] Standard-Header für size_t
#include <stdint.h>                    // [C] Standard-Header für feste Integer-Breiten wie int16_t/int32_t

#include "esp_err.h"                   // [HW] ESP-IDF-Header: definiert esp_err_t und Fehlercodes (z.B. ESP_OK)

// Software-Gain in Q15: 0 = stumm, 32768 = 1.0 (unveraendert).
#define AUDIO_OUT_GAIN_UNITY 32768     // [C] Präprozessor-Konstante | [Logik] Q15-Festkommawert für Verstärkungsfaktor 1.0 (kein Gain)

// MAX98357A an I2S1 TX (Pins aus amp_test.c, Phase 0). Startet mit
// 16 kHz / 16 bit / mono; die Rate wechselt audio_out_set_rate().
esp_err_t audio_out_init(void);        // [C] Funktionsdeklaration/Prototyp; void = keine Parameter, Rückgabe esp_err_t

// Stellt die Sample-Rate um (no-op, wenn schon aktiv). Nicht waehrend
// eines laufenden audio_out_write() aufrufen — es gibt genau einen
// Player (canned_audio-Task), der beides sequenziell macht.
esp_err_t audio_out_set_rate(uint32_t sample_rate);  // [C] Parameter uint32_t = vorzeichenlose 32-bit-Rate in Hz

// Schreibt Samples blockierend nach I2S1 und wendet dabei den
// aktuellen Gain an (veraendert den uebergebenen Buffer!).
esp_err_t audio_out_write(int16_t *samples, size_t count);  // [C] Zeiger-Parameter samples (In-/Out-Buffer), count = Anzahl Samples

// Gain thread-safe setzen/lesen (atomar) — Owner der Lautstaerke-Logik
// ist local/volume.c, hier liegt nur der rohe Multiplikator.
void audio_out_set_gain_q15(int32_t gain);      // [C] Rückgabetyp void = kein Wert | [Logik] setzt den Q15-Gain-Multiplikator atomar
int32_t audio_out_get_gain_q15(void);           // [Logik] liest den aktuell gesetzten Q15-Gain-Multiplikator atomar
