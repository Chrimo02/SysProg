#pragma once

#include <stdbool.h>

#include "esp_err.h"

// Laufzeit-Konfiguration in NVS (Namespace "minilingo"): SSID, Passwort
// und alle API-Keys/Modelle kommen seit dem Provisioning-Portal aus dem
// NVS statt aus den CONFIG_*-Makros. config_init() lädt jeden Wert beim
// Boot in einen statischen Cache; fehlt ein Key im NVS, wird der bisherige
// Kconfig-Default als Fallback genutzt (Verhalten ohne Provisioning bleibt
// identisch). Die Getter geben Pointer auf die Cache-Buffer zurück und
// sind nach config_init() aus jedem Task lesbar (read-only).

// NVS-Key-Namen (max. 15 Zeichen, NVS-Limit).
#define CFG_KEY_WIFI_SSID    "wifi_ssid"
#define CFG_KEY_WIFI_PASS    "wifi_pass"
#define CFG_KEY_GROQ_KEY     "groq_key"
#define CFG_KEY_CEREBRAS_KEY "cerebras_key"
#define CFG_KEY_GEMINI_KEY   "gemini_key"
#define CFG_KEY_LLM_MODEL    "llm_model"
#define CFG_KEY_WHISPER_LANG "whisper_lang"
#define CFG_KEY_GEMINI_VOICE "gemini_voice"
#define CFG_KEY_CHARACTER    "character"

esp_err_t config_init(void);

const char *config_wifi_ssid(void);
const char *config_wifi_password(void);
const char *config_groq_key(void);
const char *config_cerebras_key(void);
const char *config_gemini_key(void);
const char *config_llm_model(void);
const char *config_whisper_language(void);
const char *config_gemini_voice(void);
int config_character_index(void);

// true, wenn beim Boot keine SSID provisioniert war (NVS leer) und noch
// der Kconfig-Platzhalter aktiv ist — der Erststart kann damit direkt das
// Setup-Portal öffnen.
bool config_wifi_unprovisioned(void);

// Schreibt einen String-Wert nach NVS (Key = CFG_KEY_*) und committet;
// aktualisiert auch den Laufzeit-Cache. Vom Provisioning-Portal genutzt.
esp_err_t config_set_str(const char *key, const char *value);
esp_err_t config_set_character(int index);
