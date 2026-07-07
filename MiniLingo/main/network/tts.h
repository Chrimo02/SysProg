#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// TTS: Antwort-Text -> roher PCM-Buffer (16 bit LE, mono). Primaer ein
// lokaler Piper-Server im LAN (rohe WAV-Bytes), Gemini nur als Fallback
// (Spec Section 9.5, Phase 7).

// Allokiert Response- und PCM-Buffer im PSRAM (Spec Section 13:
// TTS-Response bis ~1 MB, immer PSRAM).
esp_err_t tts_init(void);

// Setzt die aktive Gemini-Stimme (Fallback-Pfad; Pointer auf ein statisches
// String-Literal aus main.c, analog zu llm_set_system_prompt). Faellt ohne
// Aufruf auf config_gemini_voice() zurueck.
void tts_set_voice(const char *voice);

// Setzt die Stimme fuer den lokalen Piper-Server (z. B. "jarvis",
// "mycroft", "alexa"). Faellt ohne Aufruf auf TTS_LOCAL_VOICE zurueck.
void tts_set_local_voice(const char *voice);

// Synthetisiert text. *pcm zeigt auf den internen PSRAM-Buffer und
// bleibt bis zum naechsten Aufruf gueltig — es gibt genau einen
// Aufrufer (network_task), der die Wiedergabe abwartet, bevor der
// naechste Query startet. Aufruf nur bei geschlossener anderer
// TLS-Verbindung (sequenziell im network_task).
// Rueckgabe ESP_ERR_NOT_ALLOWED = HTTP 429, das Free-Tier-Tageslimit
// (10 Requests/Tag, Stand 2026-06-12) ist erschoepft.
esp_err_t tts_synthesize(const char *text, const int16_t **pcm,
                         size_t *samples, uint32_t *sample_rate);
