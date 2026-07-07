// Laufzeit-Konfiguration aus NVS (Namespace "minilingo"). Ersetzt die
// CONFIG_*-Makros als Quelle für SSID/Passwort/Keys/Modelle: das
// Provisioning-Portal (network/provisioning.c) schreibt die Werte ins
// NVS, hier werden sie beim Boot in einen Cache geladen. Fehlt ein Key,
// gilt der Kconfig-Default — ohne Provisioning ändert sich also nichts.
//
// Sicherheitshinweis: Keys liegen im Klartext im NVS (wie zuvor im
// sdkconfig). Für dieses Hobby-Setup akzeptabel.
#include <stdio.h>                    // [C] snprintf für sicheres Kopieren in feste Buffer

#include <string.h>                   // [C] strcmp für Key-Vergleich

#include "esp_log.h"                  // [Logik] ESP_LOGI/ESP_LOGW-Logging über UART-Konsole
#include "nvs.h"                      // [HW] NVS-Flash-API (nvs_open/nvs_get_str/nvs_set_str/...)
#include "sdkconfig.h"                // [C] generierte Kconfig-Makros (CONFIG_WIFI_SSID etc.) als Fallback-Quelle

#include "config.h"                   // [C] eigener Header: deklariert config_init/Getter (Prototyp-Abgleich)

static const char *TAG = "config";    // [C] static = Datei-lokale Linkage | [Logik] Log-Tag zur Zuordnung der Ausgaben

#define NVS_NAMESPACE "minilingo"     // [C] Präprozessor-Konstante | [HW] Namespace-Name im NVS-Flash-Bereich

// Ein String-Eintrag: NVS-Key, Cache-Buffer, Größe, Kconfig-Fallback.
// Bündelt alles, was find_entry()/config_init() brauchen, um generisch
// über alle konfigurierten String-Werte iterieren zu können.
typedef struct {
    const char *key;                  // [C] Zeiger auf den NVS-Schlüsselstring (CFG_KEY_*)
    char *buf;                        // [C] Zeiger auf den zugehörigen statischen Cache-Buffer
    size_t size;                      // [C] size_t = vorzeichenlose Größe | [Logik] Kapazität des Cache-Buffers in Byte
    const char *fallback;             // [C] Zeiger auf den Kconfig-Default-String
} cfg_entry_t;

static char s_wifi_ssid[33];          // [C] statisches Array fester Größe | [HW] max. SSID-Länge 32 Zeichen + Nullterminator
static char s_wifi_pass[65];          // [HW] max. WPA2-Passwortlänge 64 Zeichen + Nullterminator
static char s_groq_key[128];          // [Logik] Cache-Buffer für den Groq-API-Key
static char s_cerebras_key[128];      // [Logik] Cache-Buffer für den Cerebras-API-Key
static char s_gemini_key[128];        // [Logik] Cache-Buffer für den Gemini-API-Key
static char s_llm_model[32];          // [Logik] Cache-Buffer für den LLM-Modellnamen
static char s_whisper_lang[8];        // [Logik] Cache-Buffer für den Whisper-Sprachcode (z.B. "de")
static char s_gemini_voice[24];       // [Logik] Cache-Buffer für den Namen der Gemini-TTS-Stimme
static int s_character;               // [Logik] aktuell gewählter Charakter-Index (kein String, daher separat von s_entries)
static bool s_wifi_unprovisioned;     // [C] bool-Flag | [Logik] merkt sich, ob beim Boot noch der SSID-Platzhalter aktiv war

// Tabelle aller String-Konfigwerte: verknüpft NVS-Key, Cache-Buffer und
// Kconfig-Fallback, damit config_init()/find_entry() generisch (per
// Schleife) statt mit Copy-Paste-Code für jeden Wert arbeiten können.
static cfg_entry_t s_entries[] = {                                            // [C] Array-Initialisierung von Struct-Literalen
    { CFG_KEY_WIFI_SSID,    s_wifi_ssid,    sizeof(s_wifi_ssid),    CONFIG_WIFI_SSID },        // [C] sizeof liefert Array-Größe zur Compile-Zeit
    { CFG_KEY_WIFI_PASS,    s_wifi_pass,    sizeof(s_wifi_pass),    CONFIG_WIFI_PASSWORD },
    { CFG_KEY_GROQ_KEY,     s_groq_key,     sizeof(s_groq_key),     CONFIG_GROQ_API_KEY },
    { CFG_KEY_CEREBRAS_KEY, s_cerebras_key, sizeof(s_cerebras_key), CONFIG_CEREBRAS_API_KEY },
    { CFG_KEY_GEMINI_KEY,   s_gemini_key,   sizeof(s_gemini_key),   CONFIG_GEMINI_API_KEY },
    { CFG_KEY_LLM_MODEL,    s_llm_model,    sizeof(s_llm_model),    CONFIG_LLM_MODEL },
    { CFG_KEY_WHISPER_LANG, s_whisper_lang, sizeof(s_whisper_lang), CONFIG_WHISPER_LANGUAGE },
    { CFG_KEY_GEMINI_VOICE, s_gemini_voice, sizeof(s_gemini_voice), CONFIG_GEMINI_VOICE_NAME },
};

// Sucht in s_entries den Eintrag mit passendem NVS-Key (linearer Scan).
// Wird von config_set_str() gebraucht, um Key -> Cache-Buffer aufzulösen.
static cfg_entry_t *find_entry(const char *key)
{
    for (size_t i = 0; i < sizeof(s_entries) / sizeof(s_entries[0]); i++) {  // [C] sizeof(array)/sizeof(element) = Elementanzahl zur Compile-Zeit
        if (strcmp(s_entries[i].key, key) == 0) {                           // [C] strcmp == 0 bedeutet Stringgleichheit
            return &s_entries[i];                                          // [C] '&' Adressoperator: Zeiger auf gefundenes Element zurückgeben
        }
    }
    return NULL;                                                           // [Logik] kein passender Key gefunden
}

// Initialisiert die gesamte Laufzeit-Konfiguration: öffnet NVS read-only,
// lädt jeden Eintrag aus s_entries (oder dessen Kconfig-Fallback) sowie
// den Charakter-Index, und bestimmt, ob das Gerät unprovisioniert ist.
// Muss einmalig beim Boot laufen, bevor die Getter benutzt werden.
esp_err_t config_init(void)
{
    nvs_handle_t nvs;                                        // [HW] opaker Handle auf den geöffneten NVS-Namespace
    // NVS ist in app_main schon initialisiert; nur öffnen.
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);  // [HW] öffnet Namespace "minilingo" nur lesend
    bool have_nvs = (err == ESP_OK);                          // [C] Bedingung als bool zwischenspeichern | [Logik] merkt, ob NVS-Zugriff möglich ist
    if (!have_nvs && err != ESP_ERR_NVS_NOT_FOUND) {          // [C] '!' logisches NICHT | [Logik] echter Fehler (nicht nur "Namespace fehlt noch")
        ESP_LOGW(TAG, "nvs_open failed: %s", esp_err_to_name(err));  // [Logik] Warnung loggen, aber weiterlaufen (Fallback greift)
    }

    for (size_t i = 0; i < sizeof(s_entries) / sizeof(s_entries[0]); i++) {  // [C] Schleife über alle String-Konfigeinträge
        cfg_entry_t *e = &s_entries[i];                       // [C] Zeiger auf aktuellen Eintrag zur Vereinfachung
        size_t len = e->size;                                 // [C] Feldzugriff per '->' auf Zeiger-Struct | [HW] NVS braucht die Puffergröße als In/Out-Parameter
        if (have_nvs &&
            nvs_get_str(nvs, e->key, e->buf, &len) == ESP_OK) {  // [HW] liest String-Wert aus NVS direkt in den Cache-Buffer
            continue;  // aus NVS geladen                     // [C] continue überspringt den Fallback-Zweig für diesen Eintrag
        }
        // Fallback: Kconfig-Default.
        snprintf(e->buf, e->size, "%s", e->fallback);         // [C] snprintf begrenzt auf Puffergröße, verhindert Overflow | [Logik] Default übernehmen, wenn NVS-Wert fehlt
    }

    s_character = 0;                                          // [Logik] Default-Charakter, falls NVS keinen Wert liefert
    if (have_nvs) {
        int32_t v = 0;                                        // [C] 32-bit-Zwischenvariable für NVS-Lesefunktion
        if (nvs_get_i32(nvs, CFG_KEY_CHARACTER, &v) == ESP_OK) {  // [HW] liest den gespeicherten Charakter-Index aus NVS
            s_character = (int)v;                             // [C] expliziter Cast int32_t -> int
        }
    }

    if (have_nvs) {
        nvs_close(nvs);                                       // [HW] gibt den NVS-Handle wieder frei
    }

    // "Unprovisioniert" = SSID ist noch der Kconfig-Default-Platzhalter
    // "yourwifi" (echte Kconfig-Creds oder NVS-Werte zählen als
    // provisioniert -> kein Auto-Portal beim Boot).
    s_wifi_unprovisioned = (strcmp(s_wifi_ssid, "yourwifi") == 0);  // [Logik] Heuristik: unveränderter Platzhalter erkennt fehlendes Provisioning

    ESP_LOGI(TAG, "loaded (ssid=\"%s\", model=\"%s\", lang=\"%s\", "  // [C] mehrzeiliges String-Literal wird vom Compiler verkettet
                  "voice=\"%s\", character=%d)",
             s_wifi_ssid, s_llm_model, s_whisper_lang, s_gemini_voice,
             s_character);
    return ESP_OK;                                            // [Logik] Initialisierung immer erfolgreich (Fallbacks fangen NVS-Probleme ab)
}

const char *config_wifi_ssid(void) { return s_wifi_ssid; }               // [C] einzeiliger Getter, gibt Zeiger auf statischen Cache zurück
const char *config_wifi_password(void) { return s_wifi_pass; }           // [Logik] liefert aktuelles WLAN-Passwort für die Verbindung
const char *config_groq_key(void) { return s_groq_key; }                 // [Logik] liefert Groq-API-Key für HTTP-Requests
const char *config_cerebras_key(void) { return s_cerebras_key; }         // [Logik] liefert Cerebras-API-Key für HTTP-Requests
const char *config_gemini_key(void) { return s_gemini_key; }             // [Logik] liefert Gemini-API-Key für HTTP-Requests
const char *config_llm_model(void) { return s_llm_model; }               // [Logik] liefert Namen des zu verwendenden LLM-Modells
const char *config_whisper_language(void) { return s_whisper_lang; }     // [Logik] liefert Zielsprache für die Whisper-Transkription
const char *config_gemini_voice(void) { return s_gemini_voice; }         // [Logik] liefert Namen der Gemini-TTS-Stimme
int config_character_index(void) { return s_character; }                 // [Logik] liefert Index des aktuell aktiven Charakters
bool config_wifi_unprovisioned(void) { return s_wifi_unprovisioned; }    // [Logik] liefert, ob beim Boot noch kein echtes WLAN provisioniert war

// Schreibt einen einzelnen String-Konfigwert persistent nach NVS und
// aktualisiert danach den Laufzeit-Cache, damit Getter sofort den neuen
// Wert liefern. Wird vom Provisioning-Portal beim Speichern der Formulardaten genutzt.
esp_err_t config_set_str(const char *key, const char *value)
{
    cfg_entry_t *e = find_entry(key);                         // [Logik] Key auf zugehörigen Cache-Eintrag auflösen
    if (!e) {                                                  // [C] '!' prüft auf NULL-Zeiger
        ESP_LOGE(TAG, "unknown config key: %s", key);         // [Logik] unbekannter Key ist ein Programmierfehler des Aufrufers
        return ESP_ERR_INVALID_ARG;                            // [Logik] Fehlercode signalisiert ungültigen Key
    }

    nvs_handle_t nvs;                                          // [HW] Handle für den schreibbaren NVS-Zugriff
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);  // [HW] Namespace read-write öffnen
    if (err != ESP_OK) {                                       // [C] Ungleichheitsvergleich als Fehlerprüfung
        return err;                                            // [Logik] Öffnen fehlgeschlagen, nichts geschrieben
    }
    err = nvs_set_str(nvs, key, value);                        // [HW] String-Wert unter key im NVS ablegen (noch nicht persistent)
    if (err == ESP_OK) {
        err = nvs_commit(nvs);                                 // [HW] erst commit schreibt die Änderung dauerhaft in den Flash
    }
    nvs_close(nvs);                                            // [HW] Handle in jedem Fall freigeben
    if (err == ESP_OK) {
        snprintf(e->buf, e->size, "%s", value);  // Cache nachziehen  // [C] Puffergrößen-begrenztes Kopieren | [Logik] Getter liefern ab sofort den neuen Wert
    }
    return err;                                                // [Logik] Erfolg/Fehler an den Aufrufer durchreichen
}

// Schreibt den gewählten Charakter-Index persistent nach NVS und
// aktualisiert den Cache. Getrennt von config_set_str(), weil der Wert
// ein Integer und kein String ist.
esp_err_t config_set_character(int index)
{
    nvs_handle_t nvs;                                          // [HW] Handle für den schreibbaren NVS-Zugriff
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);  // [HW] Namespace read-write öffnen
    if (err != ESP_OK) {
        return err;                                            // [Logik] Öffnen fehlgeschlagen, nichts geschrieben
    }
    err = nvs_set_i32(nvs, CFG_KEY_CHARACTER, index);          // [HW] Integer-Wert unter dem Charakter-Key ablegen
    if (err == ESP_OK) {
        err = nvs_commit(nvs);                                 // [HW] persistiert die Änderung im Flash
    }
    nvs_close(nvs);                                            // [HW] Handle in jedem Fall freigeben
    if (err == ESP_OK) {
        s_character = index;                                   // [Logik] Cache nachziehen, damit Getter sofort den neuen Index liefert
    }
    return err;                                                // [Logik] Erfolg/Fehler an den Aufrufer durchreichen
}
