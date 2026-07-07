#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// Schickt einen fertigen WAV-Buffer (Header + PCM, wie ihn der
// Recorder liefert) an Groq Whisper (Spec Section 9.3) und schreibt
// das Transkript nach text (nul-terminiert, bei Bedarf gekuerzt).
// Blockiert fuer die Dauer des HTTPS-Requests — nur aus dem
// network_task (cloud.c) aufrufen, nie parallel zu anderen
// TLS-Verbindungen (Heap, Spec Section 13).
// ESP_OK nur, wenn ein nicht-leeres Transkript ankam.
esp_err_t whisper_transcribe(const uint8_t *wav, size_t wav_bytes,
                             char *text, size_t text_len);
