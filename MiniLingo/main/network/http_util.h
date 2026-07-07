#pragma once

#include <stdbool.h>                // [C] Header-Include | [Logik] liefert den Typ bool für Rückgabewerte
#include <stddef.h>                 // [C] Header-Include | [Logik] liefert size_t für Längenangaben

#include "esp_err.h"                // [HW] ESP-IDF-Fehlercode-Typ esp_err_t und Standardwerte wie ESP_OK
#include "esp_http_client.h"        // [HW] ESP-IDF-HTTP-Client-API (Handle-Typ, Read/Write-Funktionen)

// Gemeinsame Bausteine fuer die Cloud-Requests (whisper.c, gemini.c):
// alle nutzen das Muster esp_http_client_open + _write + _read.

// esp_http_client_write schreibt ggf. weniger als angefordert.
// Deklaration von http_write_all: schreibt einen kompletten Puffer über einen
// bereits geöffneten HTTP-Client-Handle, indem sie kurze Teilschreiben in
// einer Schleife wiederholt, bis alle Bytes gesendet sind oder ein Fehler auftritt.
esp_err_t http_write_all(esp_http_client_handle_t client,   // [C] Rückgabetyp esp_err_t | [HW] Handle auf die offene HTTP-Verbindung
                         const char *data, size_t len);     // [C] const-Zeiger auf zu sendende Bytes und deren Länge

// Liest den Response-Body (bis buf_len-1 Bytes) und nul-terminiert.
// Deklaration von http_read_body: liest den kompletten Antwort-Body eines
// HTTP-Requests in einen bereitgestellten Puffer und terminiert ihn als
// C-String, damit er direkt mit String-/JSON-Funktionen weiterverarbeitet werden kann.
int http_read_body(esp_http_client_handle_t client, char *buf,   // [C] Rückgabetyp int (gelesene Byteanzahl) | [HW] Ziel-Client und Zielpuffer
                   size_t buf_len);                              // [C] Größe des Zielpuffers inklusive Platz für Null-Terminator

// Retry-Logik (Phase 8): jeder Cloud-Request wird bei transienten
// Fehlern bis zu HTTP_MAX_ATTEMPTS-mal versucht, mit exponentiellem
// Backoff (1 s, 2 s) dazwischen. Transient = Verbindungs-/Serverfehler,
// HTTP 429 (Rate-Limit) und 5xx sowie kaputte Antworten (beobachtet
// 2026-06-12: TTS-Preview liefert sporadisch 500 INTERNAL bzw.
// finishReason=OTHER ohne Audio). Permanent = ESP_ERR_INVALID_STATE
// (kein API-Key), ESP_ERR_INVALID_RESPONSE (4xx ausser 429) und
// ESP_ERR_NOT_ALLOWED (TTS-Tageslimit — Warten hilft erst morgen).
#define HTTP_MAX_ATTEMPTS 3    // [C] Präprozessor-Konstante (Textersetzung) | [Logik] Obergrenze der Versuche pro Request

// Deklaration von http_err_is_transient: prüft, ob ein esp_err_t-Fehlercode
// einen vorübergehenden (wiederholbaren) Fehler darstellt oder einen
// dauerhaften, bei dem ein erneuter Versuch keinen Sinn hätte.
bool http_err_is_transient(esp_err_t err);   // [C] Rückgabetyp bool | [HW] err = zu prüfender esp_err_t-Fehlercode

// Blockiert 1 s << (attempt-1); attempt zaehlt ab 1.
// Deklaration von http_backoff_delay: verzögert den nächsten Retry-Versuch
// um eine exponentiell wachsende Wartezeit und protokolliert dies, damit
// aufeinanderfolgende Fehlversuche den Server/das Netz nicht überlasten.
void http_backoff_delay(int attempt, const char *tag);   // [C] kein Rückgabewert | [Logik] attempt = aktuelle Versuchsnummer, tag = Log-Tag des Aufrufers
