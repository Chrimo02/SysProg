// Laufzeit-Konfiguration aus NVS (Namespace "minilingo"). Ersetzt die
// CONFIG_*-Makros als Quelle für SSID/Passwort/Keys/Modelle: das
// Provisioning-Portal (network/provisioning.c) schreibt die Werte ins
// NVS, hier werden sie beim Boot in einen Cache geladen. Fehlt ein Key,
// gilt der Kconfig-Default — ohne Provisioning ändert sich also nichts.
//
// Sicherheitshinweis: Keys liegen im Klartext im NVS (wie zuvor im
// sdkconfig). Für dieses Hobby-Setup akzeptabel.
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h" // NVS-API von Espressif -> Non-Volatile Storage (=Flash)
#include "sdkconfig.h"

#include "config.h"

static const char *TAG = "config";

#define NVS_NAMESPACE "minilingo" // NVS-Namespace -> Name der Partition im Flash, in der die Konfiguration gespeichert wird

// Ein String-Eintrag: NVS-Key, Cache-Buffer, Größe, Kconfig-Fallback.
typedef struct {
    const char *key;
    char *buf;
    size_t size;
    const char *fallback;
} cfg_entry_t;

static char s_wifi_ssid[33];
static char s_wifi_pass[65];
static char s_groq_key[128];
static char s_cerebras_key[128];
static char s_gemini_key[128];
static char s_llm_model[32];
static char s_whisper_lang[8];
static char s_gemini_voice[24];
static int s_character;
static bool s_wifi_unprovisioned;

static cfg_entry_t s_entries[] = {
    { CFG_KEY_WIFI_SSID,    s_wifi_ssid,    sizeof(s_wifi_ssid),    CONFIG_WIFI_SSID },
    { CFG_KEY_WIFI_PASS,    s_wifi_pass,    sizeof(s_wifi_pass),    CONFIG_WIFI_PASSWORD },
    { CFG_KEY_GROQ_KEY,     s_groq_key,     sizeof(s_groq_key),     CONFIG_GROQ_API_KEY },
    { CFG_KEY_CEREBRAS_KEY, s_cerebras_key, sizeof(s_cerebras_key), CONFIG_CEREBRAS_API_KEY },
    { CFG_KEY_GEMINI_KEY,   s_gemini_key,   sizeof(s_gemini_key),   CONFIG_GEMINI_API_KEY },
    { CFG_KEY_LLM_MODEL,    s_llm_model,    sizeof(s_llm_model),    CONFIG_LLM_MODEL },
    { CFG_KEY_WHISPER_LANG, s_whisper_lang, sizeof(s_whisper_lang), CONFIG_WHISPER_LANGUAGE },
    { CFG_KEY_GEMINI_VOICE, s_gemini_voice, sizeof(s_gemini_voice), CONFIG_GEMINI_VOICE_NAME },
};

// Hilfsfunktion: sucht einen Eintrag in s_entries[] nach Key ab und gibt Pointer auf den Eintrag zurück (oder NULL, wenn nicht gefunden).
static cfg_entry_t *find_entry(const char *key)
{
    for (size_t i = 0; i < sizeof(s_entries) / sizeof(s_entries[0]); i++) {
        if (strcmp(s_entries[i].key, key) == 0) {
            return &s_entries[i];
        }
    }
    return NULL;
}

esp_err_t config_init(void)
{
    nvs_handle_t nvs; // NVS-handle variable, wird in nvs_open() gesetzt, sodass sie auf die geöffnete NVS-Partition zeigt
    // NVS ist in app_main schon initialisiert; nur öffnen.
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs); //NVS_NAMESPACE = name der partition im flash NVS_READONLY = nur lesen, &nvs = output-parameter -> nvs wird innerhalb von nvs_open gesetzt, sodass nvs danach auf die geöffnete NVS-Partition zeigt
    bool have_nvs = (err == ESP_OK); // have_nvs = true, wenn nvs_open() erfolgreich (NVS_Partition existiert und konnte geöffnet werden) -> sonst false

    // Nur Warnung bei Fehlern, die nicht ESP_ERR_NVS_NOT_FOUND sind
    // (Erste Inbetriebnahme hat leeren NVS, das ist kein Problem)
    if (!have_nvs && err != ESP_ERR_NVS_NOT_FOUND) { 
        ESP_LOGW(TAG, "nvs_open failed: %s", esp_err_to_name(err));
    }

    // for-schleife um alle Systemkonfigurationseinträge aus dem NVS zu laden, damit sie im RAM verfügbar sind für andere Tasks.
    for (size_t i = 0; i < sizeof(s_entries) / sizeof(s_entries[0]); i++) {
        cfg_entry_t *e = &s_entries[i]; // e = pointer auf aktuelles cfg_entry_t struct -> enthält key, buf, size und fallback für den aktuellen Eintrag
        if (have_nvs &&
            nvs_get_str(nvs, e->key, e->buf, &len) == ESP_OK) {
            continue;  // aus NVS geladen
        }
        // Fallback: Kconfig-Default.
        snprintf(e->buf, e->size, "%s", e->fallback);
    }


    // erstmal default character-idnex auf 0, damit falls nix im nvs ist einer ausgewählt ist
    s_character = 0;
    if (have_nvs) {
        int32_t v = 0;
        if (nvs_get_i32(nvs, CFG_KEY_CHARACTER, &v) == ESP_OK) { //character-idx ist im nvs als 32-bit int gespeichert, wird gelessen und falls es dieses lesen geklappt hat wird der index zu normalem int gecastet und in s_character gespeichert -> ausgewählter character bleibt über reboot erhalten
            s_character = (int)v;
        }
    }

    if (have_nvs) {
        nvs_close(nvs); // schließt handle auf NVS-Partition, damit andere Tasks/Threads darauf zugreifen können -> vglbar mit fclose(f) um File handle zu schließen
    }

    // "Unprovisioniert" = SSID ist noch der Kconfig-Default-Platzhalter
    // "yourwifi" (echte Kconfig-Creds oder NVS-Werte zählen als
    // provisioniert -> kein Auto-Portal beim Boot).
    s_wifi_unprovisioned = (strcmp(s_wifi_ssid, "yourwifi") == 0); // flag setzen, wenn SSID noch der Kconfig-Default ist -> dann beim Boot das Setup-Portal öffnen

    ESP_LOGI(TAG, "loaded (ssid=\"%s\", model=\"%s\", lang=\"%s\", "
                  "voice=\"%s\", character=%d)",
             s_wifi_ssid, s_llm_model, s_whisper_lang, s_gemini_voice,
             s_character);
    return ESP_OK;
}

// Getter functions für die Konfigurationseinträge. Geben Pointer auf die Cache-Buffer zurück, die nach config_init() aus jedem Task lesbar sind (read-only).
const char *config_wifi_ssid(void) { return s_wifi_ssid; }
const char *config_wifi_password(void) { return s_wifi_pass; }
const char *config_groq_key(void) { return s_groq_key; }
const char *config_cerebras_key(void) { return s_cerebras_key; }
const char *config_gemini_key(void) { return s_gemini_key; }
const char *config_llm_model(void) { return s_llm_model; }
const char *config_whisper_language(void) { return s_whisper_lang; }
const char *config_gemini_voice(void) { return s_gemini_voice; }
int config_character_index(void) { return s_character; }
bool config_wifi_unprovisioned(void) { return s_wifi_unprovisioned; }

// Setter function um neue Werte für die Konfigurationseinträge zu setzen.
esp_err_t config_set_str(const char *key, const char *value)
{
    cfg_entry_t *e = find_entry(key); // sucht den Eintrag in s_entries[] nach dem Key ab und gibt Pointer auf den Eintrag zurück (oder NULL, wenn nicht gefunden)
    if (!e) {
        ESP_LOGE(TAG, "unknown config key: %s", key);
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(nvs, key, value); // schreibt Wert in NVS RAM-Cache (noch nicht im Flash!)
    if (err == ESP_OK) {
        err = nvs_commit(nvs); // speichert NVS RAM-Cache persistent im Flash
    }
    nvs_close(nvs);
    if (err == ESP_OK) {
        snprintf(e->buf, e->size, "%s", value);  // aktualisiert den Application-Cache, (s_wifi_ssid, etc.) damit nächster zugriff den neuen Wert sieht
    }
    return err;
}

//esp_err_t ist ein typedef für int, das von Espressif verwendet wird, um Fehlercodes zurückzugeben. ESP_OK bedeutet Erfolg (0), andere Werte sind Fehlercodes.
// Diese functino setzt den Character-Index in Flash(NVS) und RAM (s_character)
// wird zb aufgerufen wenn user über UI-statusbar-klick den character wechselt
esp_err_t config_set_character(int index)
{
    nvs_handle_t nvs; //nvs-handle hier noch uninitialisiert, wird gleich in nvs_open() gesetzt

    //nvs_open öffnet NVS-Partition mit namen NVS_NAMESPACE im read/write Modus (NVS_READWRITE)
    // &nvs ist "Output-Parameter" -> nvs wird innerhalb von nvs_open gesetzt, sodass nvs danach auf die geöffnete NVS-Partition zeigt
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs); 
    if (err != ESP_OK) {
        return err;
    }

    // nvs=Handle auf NVS-Partition, CFG_KEY_CHARACTER=Key unter dem der Wert gespeichert wird, index=neuer Wert für den Charakter-Index
    err = nvs_set_i32(nvs, CFG_KEY_CHARACTER, index); //nvs_set_i32 schreibt zuerst nur in Cache/RAM, erst nvs_commit() schreibt in den Flash
    if (err == ESP_OK) {
        err = nvs_commit(nvs); // wenn write in RAM erfolgreich war (=kein err) -> schreibe Cache in Flash, damit der Wert auch nach einem Reboot erhalten bleibt
    }
    nvs_close(nvs); // schließt handle auf NVS-Partition, damit andere Tasks/Threads darauf zugreifen können -> vglbar mit fclose(f) um File handle zu schließen
    if (err == ESP_OK) {
        s_character = index; // s_character ist lokale Kopie des character-Index, die im RAM gehalten wird -> wird hier aktualisiert, damit der neue Wert sofort im laufenden Programm verfügbar ist, ohne dass man wieder aus dem Flash lesen muss
    }
    return err;
}
