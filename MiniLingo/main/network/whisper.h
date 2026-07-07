#pragma once

#include <stddef.h>                // [C] Header-Include | [HW] liefert size_t fuer Groessen-/Laengenangaben
#include <stdint.h>                // [C] Header-Include | [HW] liefert uint8_t fuer den rohen Byte-Buffer

#include "esp_err.h"               // [C] Header-Include | [HW] liefert esp_err_t und ESP_OK/ESP_FAIL Fehlercodes

// Schickt einen fertigen WAV-Buffer (Header + PCM, wie ihn der
// Recorder liefert) an Groq Whisper (Spec Section 9.3) und schreibt
// das Transkript nach text (nul-terminiert, bei Bedarf gekuerzt).
// Blockiert fuer die Dauer des HTTPS-Requests — nur aus dem
// network_task (cloud.c) aufrufen, nie parallel zu anderen
// TLS-Verbindungen (Heap, Spec Section 13).
// ESP_OK nur, wenn ein nicht-leeres Transkript ankam.
esp_err_t whisper_transcribe(const uint8_t *wav, size_t wav_bytes,   // [C] Funktionsdeklaration | [HW] wav = Zeiger auf WAV-Rohdaten, wav_bytes = deren Laenge
                             char *text, size_t text_len);           // [C] Out-Parameter per Zeiger | [Logik] text_len begrenzt die Schreiblaenge (Puffer-Overflow-Schutz)
