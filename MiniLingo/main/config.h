#pragma once                          // [C] Include-Guard-Ersatz: verhindert Mehrfach-Einbindung dieses Headers

#include <stdbool.h>                   // [C] Standard-Header für bool/true/false

#include "esp_err.h"                   // [HW] ESP-IDF-Header: definiert esp_err_t und Fehlercodes (z.B. ESP_OK)

// Laufzeit-Konfiguration in NVS (Namespace "minilingo"): SSID, Passwort
// und alle API-Keys/Modelle kommen seit dem Provisioning-Portal aus dem
// NVS statt aus den CONFIG_*-Makros. config_init() lädt jeden Wert beim
// Boot in einen statischen Cache; fehlt ein Key im NVS, wird der bisherige
// Kconfig-Default als Fallback genutzt (Verhalten ohne Provisioning bleibt
// identisch). Die Getter geben Pointer auf die Cache-Buffer zurück und
// sind nach config_init() aus jedem Task lesbar (read-only).

// NVS-Key-Namen (max. 15 Zeichen, NVS-Limit).
#define CFG_KEY_WIFI_SSID    "wifi_ssid"     // [C] Präprozessor-Konstante | [HW] NVS-Schlüssel für die WLAN-SSID
#define CFG_KEY_WIFI_PASS    "wifi_pass"     // [HW] NVS-Schlüssel für das WLAN-Passwort
#define CFG_KEY_GROQ_KEY     "groq_key"      // [HW] NVS-Schlüssel für den Groq-API-Key (Whisper-Transkription)
#define CFG_KEY_CEREBRAS_KEY "cerebras_key"  // [HW] NVS-Schlüssel für den Cerebras-API-Key (LLM-Backend)
#define CFG_KEY_GEMINI_KEY   "gemini_key"    // [HW] NVS-Schlüssel für den Gemini-API-Key (LLM/TTS-Backend)
#define CFG_KEY_LLM_MODEL    "llm_model"     // [Logik] NVS-Schlüssel für das ausgewählte LLM-Modell
#define CFG_KEY_WHISPER_LANG "whisper_lang"  // [Logik] NVS-Schlüssel für die Whisper-Zielsprache
#define CFG_KEY_GEMINI_VOICE "gemini_voice"  // [Logik] NVS-Schlüssel für die Gemini-TTS-Stimme
#define CFG_KEY_CHARACTER    "character"     // [Logik] NVS-Schlüssel für den ausgewählten Charakter-Index

// Lädt alle Konfigurationswerte beim Boot aus NVS in den statischen Cache;
// fehlende Keys werden mit den Kconfig-Defaults befüllt. Muss vor dem
// ersten Getter-Aufruf laufen (z.B. in app_main).
esp_err_t config_init(void);                          // [C] Rückgabetyp esp_err_t; void = keine Parameter

const char *config_wifi_ssid(void);                   // [C] Getter, gibt const-Zeiger auf Cache-Buffer zurück
const char *config_wifi_password(void);               // [Logik] liefert das aktuell gecachte WLAN-Passwort
const char *config_groq_key(void);                    // [Logik] liefert den aktuell gecachten Groq-API-Key
const char *config_cerebras_key(void);                // [Logik] liefert den aktuell gecachten Cerebras-API-Key
const char *config_gemini_key(void);                  // [Logik] liefert den aktuell gecachten Gemini-API-Key
const char *config_llm_model(void);                   // [Logik] liefert den Namen des konfigurierten LLM-Modells
const char *config_whisper_language(void);            // [Logik] liefert die konfigurierte Whisper-Zielsprache
const char *config_gemini_voice(void);                // [Logik] liefert die konfigurierte Gemini-TTS-Stimme
int config_character_index(void);                     // [Logik] liefert den Index des aktuell gewählten Charakters

// true, wenn beim Boot keine SSID provisioniert war (NVS leer) und noch
// der Kconfig-Platzhalter aktiv ist — der Erststart kann damit direkt das
// Setup-Portal öffnen.
bool config_wifi_unprovisioned(void);                 // [C] Rückgabetyp bool | [Logik] Entscheidungsgrundlage für Auto-Start des Setup-Portals

// Schreibt einen String-Wert nach NVS (Key = CFG_KEY_*) und committet;
// aktualisiert auch den Laufzeit-Cache. Vom Provisioning-Portal genutzt.
esp_err_t config_set_str(const char *key, const char *value);  // [C] key/value als const char* (nur lesender Zugriff für den Aufrufer)
esp_err_t config_set_character(int index);                     // [Logik] schreibt den Charakter-Index persistent nach NVS und aktualisiert den Cache
