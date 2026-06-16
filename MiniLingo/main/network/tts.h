#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// Gemini TTS (Spec Section 9.5, Phase 7): Antwort-Text -> roher
// PCM-Buffer (16 bit LE, mono, typ. 24 kHz).

// Allokiert Response- und PCM-Buffer im PSRAM (Spec Section 13:
// TTS-Response bis ~1 MB, immer PSRAM).
esp_err_t tts_init(void);

// Synthetisiert text. *pcm zeigt auf den internen PSRAM-Buffer und
// bleibt bis zum naechsten Aufruf gueltig — es gibt genau einen
// Aufrufer (network_task), der die Wiedergabe abwartet, bevor der
// naechste Query startet. Aufruf nur bei geschlossener anderer
// TLS-Verbindung (sequenziell im network_task).
// Rueckgabe ESP_ERR_NOT_ALLOWED = HTTP 429, das Free-Tier-Tageslimit
// (10 Requests/Tag, Stand 2026-06-12) ist erschoepft.
esp_err_t tts_synthesize(const char *text, const int16_t **pcm,
                         size_t *samples, uint32_t *sample_rate);
