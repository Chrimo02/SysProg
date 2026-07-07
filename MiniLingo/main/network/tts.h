#pragma once

#include <stddef.h>     // [C] Header-Include | [Logik] liefert size_t fuer Sample-Zaehlung
#include <stdint.h>     // [C] Header-Include | [Logik] liefert uint32_t/int16_t fuer PCM-Daten und Sample-Rate

#include "esp_err.h"    // [C] Header-Include | [HW] esp_err_t-Fehlercode-Typ des ESP-IDF

// TTS: Antwort-Text -> roher PCM-Buffer (16 bit LE, mono). Primaer ein
// lokaler Piper-Server im LAN (rohe WAV-Bytes), Gemini nur als Fallback
// (Spec Section 9.5, Phase 7).

// Initialisiert das TTS-Modul: allokiert Response- und PCM-Buffer im
// PSRAM (Spec Section 13: TTS-Response bis ~1 MB, immer PSRAM). Muss
// einmalig vor dem ersten tts_synthesize() aufgerufen werden.
esp_err_t tts_init(void);   // [C] Funktionsdeklaration, Rueckgabetyp esp_err_t | [HW] reserviert PSRAM-Puffer fuer TTS-Antworten

// Setzt die aktive Gemini-Stimme (Fallback-Pfad; Pointer auf ein statisches
// String-Literal aus main.c, analog zu llm_set_system_prompt). Faellt ohne
// Aufruf auf config_gemini_voice() zurueck.
void tts_set_voice(const char *voice);   // [C] Parameter const char* = schreibgeschuetzter Zeiger auf String-Literal | [Logik] setzt modul-internen Zustand fuer Gemini-Stimme

// Setzt die Stimme fuer den lokalen Piper-Server (z. B. "jarvis",
// "mycroft", "alexa"). Faellt ohne Aufruf auf TTS_LOCAL_VOICE zurueck.
void tts_set_local_voice(const char *voice);   // [C] const char*-Parameter, kein Kopieren des Strings | [Logik] setzt modul-internen Zustand fuer lokale Piper-Stimme

// Synthetisiert text. *pcm zeigt auf den internen PSRAM-Buffer und
// bleibt bis zum naechsten Aufruf gueltig — es gibt genau einen
// Aufrufer (network_task), der die Wiedergabe abwartet, bevor der
// naechste Query startet. Aufruf nur bei geschlossener anderer
// TLS-Verbindung (sequenziell im network_task).
// Rueckgabe ESP_ERR_NOT_ALLOWED = HTTP 429, das Free-Tier-Tageslimit
// (10 Requests/Tag, Stand 2026-06-12) ist erschoepft.
esp_err_t tts_synthesize(const char *text, const int16_t **pcm,
                         size_t *samples, uint32_t *sample_rate);
// [C] Ausgabeparameter als Zeiger-auf-Zeiger (**pcm) und Zeiger (*samples, *sample_rate), damit die Funktion mehrere Werte zurueckgeben kann
// [HW] *pcm zeigt nach Erfolg auf rohe 16-bit-PCM-Samples im PSRAM-Puffer, sample_rate stammt aus WAV-Header bzw. Gemini-mimeType
// [Logik] Kernfunktion: waehlt lokal (Piper) oder Gemini-Fallback und liefert Fehlercode inkl. Sonderfall ESP_ERR_NOT_ALLOWED fuer Tageslimit
