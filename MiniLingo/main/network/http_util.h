#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "esp_http_client.h"

// Gemeinsame Bausteine fuer die Cloud-Requests (whisper.c, gemini.c):
// alle nutzen das Muster esp_http_client_open + _write + _read.

// esp_http_client_write schreibt ggf. weniger als angefordert.
esp_err_t http_write_all(esp_http_client_handle_t client,
                         const char *data, size_t len);

// Liest den Response-Body (bis buf_len-1 Bytes) und nul-terminiert.
int http_read_body(esp_http_client_handle_t client, char *buf,
                   size_t buf_len);

// Retry-Logik (Phase 8): jeder Cloud-Request wird bei transienten
// Fehlern bis zu HTTP_MAX_ATTEMPTS-mal versucht, mit exponentiellem
// Backoff (1 s, 2 s) dazwischen. Transient = Verbindungs-/Serverfehler,
// HTTP 429 (Rate-Limit) und 5xx sowie kaputte Antworten (beobachtet
// 2026-06-12: TTS-Preview liefert sporadisch 500 INTERNAL bzw.
// finishReason=OTHER ohne Audio). Permanent = ESP_ERR_INVALID_STATE
// (kein API-Key), ESP_ERR_INVALID_RESPONSE (4xx ausser 429) und
// ESP_ERR_NOT_ALLOWED (TTS-Tageslimit — Warten hilft erst morgen).
#define HTTP_MAX_ATTEMPTS 3

bool http_err_is_transient(esp_err_t err);

// Blockiert 1 s << (attempt-1); attempt zaehlt ab 1.
void http_backoff_delay(int attempt, const char *tag);
