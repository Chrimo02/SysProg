#pragma once                          // [C] Include-Guard-Ersatz: verhindert Mehrfach-Einbindung dieses Headers

#include <stddef.h>                    // [C] Standard-Header für size_t
#include <stdint.h>                    // [C] Standard-Header für feste Integer-Breiten wie int16_t
#include "esp_err.h"                   // [HW] ESP-IDF-Header: definiert esp_err_t und Fehlercodes (z.B. ESP_OK)

// INMP441 an I2S0 RX (Spec Section 3: SCK=5, WS=4, SD=6),
// 16 kHz mono. Abgeleitet aus mic_test.c (Phase 0).
esp_err_t audio_in_init(void);         // [C] Funktionsdeklaration/Prototyp; void = keine Parameter, Rückgabe esp_err_t

// Liest blockierend `samples` Samples vom Mikrofon und konvertiert
// sie nach 16-bit signed (Format, das die AFE-Pipeline erwartet).
esp_err_t audio_in_read(int16_t *dest, size_t samples);  // [C] dest = Ausgabe-Zeiger, samples = angeforderte Sample-Anzahl
