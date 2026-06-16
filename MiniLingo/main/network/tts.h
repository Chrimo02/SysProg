#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// Gemini TTS (Spec Section 9.5, Phase 7): Antwort-Text -> roher
// PCM-Buffer (16 bit LE, mono, typ. 24 kHz). Seit dem geraeteseitigen
// Streaming wird das Audio chunkweise dekodiert und sofort ausgegeben,
// statt erst die ganze Antwort zu puffern (Latenz + Limit fuer lange
// Antworten).

// Allokiert die kleinen Scratch-Puffer im PSRAM (Lesepuffer, Base64-
// Akku, PCM-Decode) — kein 1,5-MB-Response-Puffer mehr.
esp_err_t tts_init(void);

// Callback pro dekodiertem PCM-Chunk (16 bit LE mono). rate ist bei
// jedem Aufruf gleich; der erste Aufruf markiert den Wiedergabe-Start.
// Der Callback darf den Buffer veraendern (z.B. Gain). Rueckgabe != ESP_OK
// bricht das Streaming ab (z.B. "stop").
typedef esp_err_t (*tts_pcm_cb_t)(int16_t *pcm, size_t samples,
                                  uint32_t rate, void *ctx);

// Synthetisiert text und streamt das Audio chunkweise an cb. Blockiert
// bis Wiedergabe-Ende/Abbruch/Fehler; nur aus dem network_task (TLS
// sequenziell). Retry (transiente Fehler) nur, solange noch KEIN Chunk
// ausgegeben wurde. Rueckgabe ESP_ERR_NOT_ALLOWED = HTTP 429 (Free-Tier-
// Tageslimit).
esp_err_t tts_synthesize_stream(const char *text, tts_pcm_cb_t cb,
                                void *ctx);
