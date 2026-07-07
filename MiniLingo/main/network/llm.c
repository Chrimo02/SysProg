// LLM-Antworten via Cerebras (seit 2026-06-12, ersetzt Gemini 2.5
// Flash aus Spec Section 9.4): OpenAI-kompatible Chat-Completions,
// Free Tier 1 Mio. Token/Tag statt 250 Requests/Tag bei Gemini.
// JSON-Request per cJSON (System-Prompt + History-Ringpuffer +
// neuer User-Turn), Antwort aus choices[0].message.content.
// TLS-Zertifikate aus dem esp_crt_bundle.
#include <ctype.h>                     // [C] Header-Include | isspace() fuer Trimmen der LLM-Textantwort
#include <stdio.h>                     // [C] Header-Include | snprintf() fuer sichere String-Formatierung
#include <string.h>                    // [C] Header-Include | strlen() fuer Laengenberechnungen

#include "cJSON.h"                     // [C] Header-Include | JSON-Bibliothek zum Bauen/Parsen der Request-/Response-Payloads
#include "esp_crt_bundle.h"            // [C] Header-Include | [HW] eingebettetes TLS-Zertifikatsbuendel fuer HTTPS-Verbindungen
#include "esp_http_client.h"           // [C] Header-Include | [HW] ESP-IDF HTTP(S)-Client-API
#include "esp_log.h"                   // [C] Header-Include | Logging-Makros (ESP_LOGE/ESP_LOGI)
#include "sdkconfig.h"                 // [C] Header-Include | Projekt-Konfiguration aus menuconfig

#include "config.h"                    // [C] Header-Include | Zugriff auf gespeicherte Konfiguration (API-Key, Modellname)
#include "http_util.h"                 // [C] Header-Include | gemeinsame HTTP-Hilfsfunktionen (Retry/Backoff, Read/Write)
#include "state.h"                     // [C] Header-Include | Zugriff auf die Konversations-History (chat_history_*)
#include "llm.h"                       // [C] Header-Include | eigene Modul-Schnittstelle (Deklarationen zu diesem .c)

static const char *TAG = "llm";        // [C] static = Datei-lokale (interne) Linkage | [Logik] Log-Tag zur Zuordnung der Ausgaben dieses Moduls

#define LLM_URL "https://api.cerebras.ai/v1/chat/completions"  // [C] Praeprozessor-Konstante | [HW] HTTPS-Endpoint der Cerebras Chat-Completions-API

// System-Prompt = Persona (Phase B, per llm_set_system_prompt zur
// Laufzeit umschaltbar) + TOOL_INSTRUCTIONS. Die Tool-Hinweise MUESSEN
// in jeder Persona erhalten bleiben, sonst brechen die Phase-10-Tools.
// Knappe Antworten ohne Markdown — Sternchen klingen im TTS schrecklich.
#define DEFAULT_PERSONA \
    "You are a compact voice assistant named MiniLingo."      // [C] Praeprozessor-Makro expandiert zu String-Literal | [Logik] Default-Persona, falls keine gesetzt wurde

#define TOOL_INSTRUCTIONS \
    " Keep replies under 3 sentences and without markdown formatting. You " \
    "control a kitchen timer and the volume: if the user wants to set or " \
    "cancel a timer, ask for the remaining time, or change the volume, " \
    "call the matching tool instead of replying in text. Convert minutes " \
    "to seconds."                                             // [C] mehrzeiliges String-Literal-Makro (Backslash-Fortsetzung) | [Logik] Anweisungen fuer Tool-Nutzung, an jeden System-Prompt angehaengt

// Sprach-Anweisung (Stand 2026-06-30): das LLM spiegelt sonst die Sprache
// des transkribierten Prompts — spricht der User Deutsch (oder steht
// whisper_lang im NVS noch auf "de"), antwortet es Deutsch. Default ist
// Englisch; Deutsch nur, wenn der User es im Turn ausdruecklich verlangt.
// Wird ans Ende des System-Prompts gehaengt (Recency = hoehere Prioritaet).
#define LANGUAGE_INSTRUCTIONS \
    " Always answer in English, regardless of the language of the user's " \
    "message, unless the user explicitly asks you to use another language."  // [C] String-Literal-Makro | [Logik] erzwingt Standard-Antwortsprache Englisch

// Aktive Persona (Pointer auf ein statisches String-Literal aus main.c
// oder das Default). Pointer-Tausch ist atomar genug: gesetzt aus dem
// LVGL-Task, gelesen im network_task — schlimmstenfalls nutzt ein
// gerade laufender Request noch die vorherige Persona.
static const char *s_persona = DEFAULT_PERSONA;  // [C] static = Datei-lokale globale Variable, Pointer auf konstanten String | [Logik] haelt aktuell aktive Persona zwischen Tasks

// Setzt die aktive Persona zur Laufzeit (z. B. bei Charakterwechsel per
// Voice-Command). NULL bedeutet: zurueck zur Default-Persona.
void llm_set_system_prompt(const char *persona)
{
    s_persona = persona ? persona : DEFAULT_PERSONA;  // [C] Ternaerer Operator: Pointer-Zuweisung ohne Kopie des Strings | [Logik] NULL-Check faellt auf Default zurueck
}

// Antwortlaenge steuert primaer der System-Prompt (<3 Saetze);
// das Token-Limit ist nur die Notbremse (~10 s TTS-Audio, Spec
// Section 9.4). 300 statt 200, weil gpt-oss-120b ein Reasoning-Model
// ist und die Reasoning-Tokens GEGEN dieses Budget zaehlen (mit
// reasoning_effort=low gemessen ~11 Tokens, aber mit Luft).
#define MAX_COMPLETION_TOKENS 300      // [C] Praeprozessor-Konstante | [HW] JSON-Feld "max_completion_tokens" im Request an die Cerebras-API
#define TEMPERATURE           0.7      // [C] Praeprozessor-Konstante (double) | [HW] JSON-Feld "temperature" steuert Zufaelligkeit der LLM-Antwort

// Antwort-JSON bleibt klein (200 Output-Tokens + Usage-Metadaten).
#define RESPONSE_MAX 4096              // [C] Praeprozessor-Konstante | [Logik] Groesse des Stack-Puffers fuer die HTTP-Response
#define TIMEOUT_MS   20000             // [C] Praeprozessor-Konstante | [HW] esp_http_client Socket-Timeout in Millisekunden

// Haengt eine Chat-Nachricht (role/content) an das messages-Array des
// Request-JSON an. Wird fuer System-Prompt, History und neuen User-Turn
// benutzt, um das OpenAI-Chat-Format aufzubauen.
static void add_message(cJSON *messages, const char *role,
                        const char *content)
{
    cJSON *msg = cJSON_CreateObject();               // [C] cJSON_CreateObject() liefert Heap-allozierten JSON-Objekt-Pointer | [HW] entspricht einem Element im "messages"-Array der Chat-API
    cJSON_AddStringToObject(msg, "role", role);       // [HW] JSON-Feld "role" (system/user/assistant) laut OpenAI-Chat-Format
    cJSON_AddStringToObject(msg, "content", content); // [HW] JSON-Feld "content" = eigentlicher Nachrichtentext
    cJSON_AddItemToArray(messages, msg);              // [C] uebergibt Besitz des msg-Objekts an das Array (kein separates Free noetig) | [Logik] Nachricht wird Teil der Konversations-Historie im Request
}

// Ein Tool-Schema (OpenAI-Format) an das tools-Array haengen. params ist
// ein vorgefertigtes JSON-Schema-Objekt (Besitz geht ans Tool ueber) oder
// NULL fuer parameterlose Tools.
static void add_tool(cJSON *tools, const char *name, const char *desc,
                     cJSON *params)
{
    cJSON *tool = cJSON_CreateObject();                          // [C] neues JSON-Objekt fuer einen Tool-Eintrag
    cJSON_AddStringToObject(tool, "type", "function");            // [HW] OpenAI-Tool-Format verlangt "type": "function"
    cJSON *func = cJSON_AddObjectToObject(tool, "function");       // [C] verschachteltes Objekt, Rueckgabe-Pointer zeigt hinein in tool | [HW] JSON-Unterobjekt "function" mit Name/Beschreibung/Parametern
    cJSON_AddStringToObject(func, "name", name);                   // [HW] Funktionsname, den das LLM beim Tool-Call zurueckliefert
    cJSON_AddStringToObject(func, "description", desc);            // [HW] Beschreibung, anhand derer das LLM entscheidet, wann das Tool passt
    if (!params) {                                                 // [C] Pointer-Null-Check | [Logik] kein Schema uebergeben -> parameterloses Tool
        params = cJSON_CreateObject();                             // [C] Ersatzobjekt fuer leeres Schema anlegen
        cJSON_AddStringToObject(params, "type", "object");          // [HW] JSON-Schema-Konvention: "type": "object" auch ohne Properties
        cJSON_AddObjectToObject(params, "properties");              // [HW] leeres "properties"-Objekt, da keine Parameter erwartet werden
    }
    cJSON_AddItemToObject(func, "parameters", params);              // [C] uebergibt Besitz von params an func | [HW] JSON-Feld "parameters" = Argument-Schema fuer das Tool
    cJSON_AddItemToArray(tools, tool);                              // [C] Besitz von tool geht ans tools-Array ueber | [Logik] Tool ist damit Teil der im Request beworbenen Tool-Liste
}

// Schema-Objekt fuer ein Tool mit genau einem Integer-Pflichtparameter.
static cJSON *int_param_schema(const char *param, const char *param_desc)
{
    cJSON *params = cJSON_CreateObject();                                  // [C] neues JSON-Objekt fuer das Parameter-Schema
    cJSON_AddStringToObject(params, "type", "object");                      // [HW] JSON-Schema: Wurzel-Typ des Parameterobjekts
    cJSON *props = cJSON_AddObjectToObject(params, "properties");           // [C] verschachteltes Objekt fuer die einzelnen Properties | [HW] JSON-Schema-Feld "properties"
    cJSON *p = cJSON_AddObjectToObject(props, param);                      // [C] Property-Objekt unter dem uebergebenen Parameternamen anlegen
    cJSON_AddStringToObject(p, "type", "integer");                         // [HW] JSON-Schema-Typ: erzwingt Integer-Argument vom LLM
    cJSON_AddStringToObject(p, "description", param_desc);                 // [HW] Beschreibung des Parameters fuer das LLM
    cJSON *required = cJSON_AddArrayToObject(params, "required");          // [C] neues Array fuer Pflichtfeld-Namen | [HW] JSON-Schema-Feld "required"
    cJSON_AddItemToArray(required, cJSON_CreateString(param));             // [C] String-Item mit Parametername ins Array einfuegen | [Logik] macht den Parameter zur Pflichtangabe
    return params;                                                        // [C] Rueckgabe des Pointers; Besitz geht an den Aufrufer (spaeter an add_tool) ueber
}

// Tool-Definitionen (Phase 10): freie Timer-/Volume-Werte, die die
// lokalen MultiNet-Commands nicht abdecken. Die Ausfuehrung passiert in
// cloud.c (run_tool), nicht hier.
static void add_tools(cJSON *root)
{
    cJSON *tools = cJSON_AddArrayToObject(root, "tools");   // [C] neues Array am Request-Root anhaengen | [HW] JSON-Feld "tools" im OpenAI-Chat-Request
    add_tool(tools, "set_timer",                            // [Logik] Tool zum Setzen eines Kuechen-Timers
             "Sets a kitchen timer for the given duration.",
             int_param_schema("seconds",
                              "Duration in seconds (1 to 86400)."));
    add_tool(tools, "set_volume",                           // [Logik] Tool zum Setzen der Lautstaerke
             "Sets the volume to the given percentage.",
             int_param_schema("percent", "Volume 0 to 100."));
    add_tool(tools, "cancel_timer",                         // [Logik] Tool zum Abbrechen des laufenden Timers, keine Parameter
             "Cancels the running kitchen timer.", NULL);
    add_tool(tools, "get_time_left",                        // [Logik] Tool zum Abfragen der verbleibenden Timer-Zeit, keine Parameter
             "Returns the remaining time of the running timer.", NULL);
}

// Request-Body (OpenAI-Chat-Format); Rueckgabe via cJSON_free().
static char *build_request(const char *user_text)
{
    cJSON *root = cJSON_CreateObject();               // [C] Wurzel-JSON-Objekt des Request-Bodys anlegen
    if (!root) {                                       // [C] Null-Check bei Heap-Allokationsfehler | [Logik] ohne Root kann kein Request gebaut werden
        return NULL;                                   // [Logik] Fehlerfall an Aufrufer signalisieren
    }

    cJSON_AddStringToObject(root, "model", config_llm_model());  // [HW] JSON-Feld "model" bestimmt, welches LLM bei Cerebras antwortet | [Logik] Modellname kommt aus persistenter Konfiguration

    cJSON *messages = cJSON_AddArrayToObject(root, "messages");   // [C] neues Array am Root anhaengen | [HW] JSON-Feld "messages" enthaelt die gesamte Konversation
    // 768 statt 512: Persona (~250) + TOOL_INSTRUCTIONS (~280) +
    // LANGUAGE_INSTRUCTIONS (~135) sprengen 512 und wurden frueher
    // stillschweigend abgeschnitten.
    char system[768];                                             // [C] Stack-Array fuer den zusammengesetzten System-Prompt
    snprintf(system, sizeof(system), "%s%s%s",                    // [C] snprintf begrenzt Schreibmenge auf sizeof(system) -> kein Buffer-Overflow
             s_persona, TOOL_INSTRUCTIONS, LANGUAGE_INSTRUCTIONS); // [Logik] Persona + Tool-Hinweise + Sprachvorgabe zu einem System-Prompt verketten
    add_message(messages, "system", system);                      // [HW] System-Nachricht als erstes Element in "messages"
    for (size_t i = 0; i < chat_history_count(); i++) {           // [C] size_t = vorzeichenloser Index-Typ, Schleife ueber alle History-Eintraege | [Logik] bisherige Konversation in den Request uebernehmen
        chat_role_t role;                                         // [C] Enum-Variable zur Aufnahme der Rolle des Eintrags
        const char *text = chat_history_get(i, &role);            // [C] Pointer auf History-Text; role wird per Out-Parameter (Adress-Operator) befuellt
        add_message(messages,
                    role == CHAT_ROLE_MODEL ? "assistant" : "user", text);  // [C] Ternaerer Operator waehlt JSON-Rolle passend zum internen Enum-Wert | [HW] OpenAI kennt nur "user"/"assistant"/"system"
    }
    add_message(messages, "user", user_text);                     // [HW] aktueller User-Turn als letzte Nachricht im Array

    add_tools(root);                                              // [Logik] Tool-Definitionen an den Request anhaengen, damit das LLM sie nutzen kann

    cJSON_AddNumberToObject(root, "max_completion_tokens",
                            MAX_COMPLETION_TOKENS);                // [HW] JSON-Feld begrenzt die maximale Tokenzahl der Antwort
    cJSON_AddNumberToObject(root, "temperature", TEMPERATURE);     // [HW] JSON-Feld steuert Sampling-Zufaelligkeit der Antwort
    // gpt-oss-120b: Reasoning kurz halten (Latenz + Token-Budget;
    // verifiziert per curl 2026-06-12: ~11 Reasoning-Tokens, Antwort
    // in 24 ms Serverzeit). zai-glm-4.7 akzeptiert den Parameter,
    // ignoriert ihn aber weitgehend.
    cJSON_AddStringToObject(root, "reasoning_effort", "low");      // [HW] modellspezifischer Cerebras-Parameter zur Reasoning-Tiefe

    char *body = cJSON_PrintUnformatted(root);                    // [C] serialisiert das JSON-Objekt in einen neu allozierten, kompakten String | [HW] das ist der eigentliche HTTP-Request-Body
    cJSON_Delete(root);                                           // [C] gibt den gesamten JSON-Baum (rekursiv) frei, body-String bleibt unabhaengig bestehen
    return body;                                                  // [Logik] Aufrufer muss body spaeter mit cJSON_free() freigeben
}

// Zerlegt die JSON-Antwort der Cerebras-API in ein llm_result_t: entweder
// einen Tool-Call (choices[0].message.tool_calls) oder Klartext
// (choices[0].message.content). Wird nach jedem HTTP-200-Response aufgerufen.
static esp_err_t parse_reply(const char *json, llm_result_t *out)
{
    cJSON *root = cJSON_Parse(json);                    // [C] parst den JSON-String in einen Baum aus cJSON-Objekten (Heap-alloziert)
    if (!root) {                                        // [C] Null-Check: Parser liefert NULL bei ungueltigem JSON
        ESP_LOGE(TAG, "response is not valid JSON");     // [Logik] Fehlerfall protokollieren
        return ESP_FAIL;                                 // [Logik] Abbruch, kein gueltiges Ergebnis moeglich
    }

    // cJSON_GetObjectItem/GetArrayItem sind NULL-tolerant — fehlt ein
    // Glied, faellt das Ergebnis einfach auf NULL durch.
    esp_err_t ret = ESP_FAIL;                                              // [C] Initialisierung mit Fehlercode, wird bei Erfolg auf ESP_OK gesetzt
    const cJSON *msg = cJSON_GetObjectItem(                                // [C] verkettete Aufrufe (kein Zwischenspeichern) navigieren direkt zum Zielfeld
        cJSON_GetArrayItem(cJSON_GetObjectItem(root, "choices"), 0),       // [HW] choices[0] = erste (einzige angeforderte) Antwortvariante der Chat-API
        "message");                                                       // [HW] choices[0].message enthaelt Rolle/Inhalt der Modellantwort

    // Tool-Call hat Vorrang: bei einem set_timer/... ist content leer.
    const cJSON *tool_calls = cJSON_GetObjectItem(msg, "tool_calls");      // [HW] JSON-Feld "tool_calls" nur vorhanden, wenn das LLM ein Tool aufruft
    const cJSON *call = cJSON_GetArrayItem(tool_calls, 0);                 // [C] erstes Element des tool_calls-Arrays (NULL, falls Array fehlt/leer)
    if (call) {                                                            // [C] Pointer-Wahrheitswert: != NULL heisst, ein Tool-Call liegt vor | [Logik] Verzweigung Tool-Call vs. Text
        const cJSON *func = cJSON_GetObjectItem(call, "function");         // [HW] JSON-Unterobjekt "function" mit name/arguments
        const cJSON *name = cJSON_GetObjectItem(func, "name");             // [HW] JSON-Feld "name" = aufgerufene Tool-Funktion
        const cJSON *args = cJSON_GetObjectItem(func, "arguments");        // [HW] JSON-Feld "arguments" = Argumente als JSON-kodierter String
        if (cJSON_IsString(name)) {                                       // [C] Typpruefung vor Feldzugriff | [Logik] nur gueltig, wenn "name" tatsaechlich ein String ist
            out->type = LLM_RESULT_TOOL_CALL;                             // [C] Pfeil-Operator: Zugriff auf Struct-Feld ueber Pointer | [Logik] Diskriminator auf Tool-Call setzen
            snprintf(out->tool_name, sizeof(out->tool_name), "%s",
                     name->valuestring);                                  // [C] sizeof(out->tool_name) verhindert Overflow des Fixed-Size-Arrays | [Logik] Tool-Name in Ergebnis kopieren
            // arguments ist im OpenAI-Format ein JSON-String (kann bei
            // parameterlosen Tools fehlen) — als Text uebernehmen,
            // run_tool parst ihn separat.
            snprintf(out->tool_args, sizeof(out->tool_args), "%s",
                     cJSON_IsString(args) ? args->valuestring : "{}");    // [C] Ternaerer Operator liefert Fallback-JSON "{}" falls args fehlt | [HW] leeres JSON-Objekt als Default-Argumentliste
            ret = ESP_OK;                                                 // [Logik] Tool-Call erfolgreich extrahiert
        }
    }

    if (ret != ESP_OK) {                                                  // [Logik] noch kein Tool-Call gefunden -> auf Text-Antwort pruefen
        const cJSON *content = cJSON_GetObjectItem(msg, "content");        // [HW] JSON-Feld "content" = Klartext-Antwort des Modells
        if (cJSON_IsString(content)) {                                    // [C] Typpruefung, bevor auf valuestring zugegriffen wird
            out->type = LLM_RESULT_TEXT;                                  // [Logik] Diskriminator auf Text-Antwort setzen
            snprintf(out->text, sizeof(out->text), "%s",
                     content->valuestring);                               // [C] sizeof(out->text) begrenzt Kopie auf LLM_TEXT_MAX | [Logik] Antworttext in Ergebnis-Struct kopieren
            size_t len = strlen(out->text);                              // [C] strlen ermittelt aktuelle String-Laenge (ohne Nullterminator)
            while (len > 0 && isspace((unsigned char)out->text[len - 1])) {  // [C] Cast auf unsigned char vor isspace() (UB bei negativen char-Werten vermeiden) | [Logik] rueckwaerts Whitespace/Zeilenumbrueche vom Ende entfernen
                out->text[--len] = '\0';                                  // [C] Praedekrement verkuerzt len vor Nutzung, schreibt Nullterminator an neue Endposition
            }
            if (len > 0) {                                               // [Logik] nur als Erfolg werten, wenn nach dem Trimmen noch Inhalt uebrig ist
                ret = ESP_OK;
            }
        }
    }

    if (ret != ESP_OK) {                                                  // [Logik] weder Tool-Call noch nutzbarer Text gefunden
        ESP_LOGE(TAG, "no text/tool in response: %.256s", json);           // [C] Format-Praezision %.256s begrenzt geloggte Zeichen | [Logik] Diagnose-Log mit Rohantwort (gekuerzt)
    }
    cJSON_Delete(root);                                                  // [C] gibt den kompletten geparsten JSON-Baum wieder frei
    return ret;                                                          // [Logik] ESP_OK bei Erfolg, sonst Fehlercode
}

// Fuehrt genau einen HTTPS-Request an die Cerebras-API aus (ohne Retry).
// Baut den Request-Body, oeffnet die TLS-Verbindung, sendet, liest die
// Antwort und parst sie. Wird von llm_generate() bei Bedarf mehrfach
// aufgerufen (Retry/Backoff dort).
static esp_err_t generate_once(const char *user_text, llm_result_t *out)
{
    if (strlen(config_cerebras_key()) == 0) {                             // [C] strlen auf konfigurierten API-Key | [Logik] fehlender Key ist ein dauerhafter Konfigurationsfehler
        ESP_LOGE(TAG, "Cerebras API key not set (Setup-Portal/menuconfig)");
        return ESP_ERR_INVALID_STATE;                                     // [Logik] kein Retry sinnvoll, da Ursache nicht transient ist
    }

    char *body = build_request(user_text);                                // [C] Pointer auf Heap-allozierten JSON-String, muss spaeter freigegeben werden
    if (!body) {                                                          // [C] Null-Check bei Allokationsfehler
        return ESP_ERR_NO_MEM;                                            // [Logik] Speicher konnte nicht bereitgestellt werden
    }

    esp_http_client_config_t cfg = {                                     // [C] Struct-Initialisierung per Designated Initializers
        .url = LLM_URL,                                                  // [HW] Ziel-URL des HTTPS-Requests
        .method = HTTP_METHOD_POST,                                      // [HW] POST fuer Chat-Completions-Aufruf
        .crt_bundle_attach = esp_crt_bundle_attach,                       // [C] Funktionszeiger auf das Zertifikatsbuendel-Handler | [HW] aktiviert TLS-Zertifikatspruefung gegen das eingebettete Buendel
        .timeout_ms = TIMEOUT_MS,                                        // [HW] Socket-/Request-Timeout in Millisekunden
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);        // [C] opakes Handle (Pointer) auf den initialisierten HTTP-Client
    if (!client) {                                                       // [C] Null-Check bei Initialisierungsfehler
        cJSON_free(body);                                                // [C] gibt den zuvor allozierten Request-Body-String frei | [Logik] Speicherleck vermeiden im Fehlerpfad
        return ESP_FAIL;
    }

    char auth[160];                                                     // [C] Stack-Puffer fuer den Authorization-Header
    snprintf(auth, sizeof(auth), "Bearer %s", config_cerebras_key());    // [HW] Bearer-Token-Schema fuer HTTP-Authentifizierung | [C] snprintf begrenzt auf Puffergroesse
    esp_http_client_set_header(client, "Authorization", auth);            // [HW] setzt den Authorization-Header fuer den Request
    esp_http_client_set_header(client, "Content-Type",
                               "application/json");                     // [HW] teilt dem Server mit, dass der Body JSON ist

    esp_err_t err = esp_http_client_open(client, (int)strlen(body));     // [C] Cast von size_t auf int (Content-Length erwartet int-Parameter) | [HW] oeffnet TLS-Verbindung und kuendigt Content-Length an
    if (err != ESP_OK) {                                                // [Logik] Verbindungsaufbau fehlgeschlagen
        ESP_LOGE(TAG, "connect failed: %s", esp_err_to_name(err));       // [C] esp_err_to_name() wandelt Fehlercode in lesbaren String
        esp_http_client_cleanup(client);                                // [C] gibt Client-Ressourcen frei
        cJSON_free(body);                                                // [Logik] Body-String im Fehlerpfad ebenfalls freigeben
        return err;
    }

    int64_t t0 = esp_log_timestamp();                                    // [C] 64-Bit-Zeitstempel als Startmarke | [Logik] fuer spaetere Latenzmessung des Requests
    err = http_write_all(client, body, strlen(body));                    // [HW] schreibt den gesamten Request-Body ueber die offene HTTPS-Verbindung
    cJSON_free(body);                                                    // [C] Body wird nach dem Senden nicht mehr gebraucht, sofort freigeben

    char resp[RESPONSE_MAX];                                             // [C] Stack-Puffer fuer die HTTP-Response
    int status = -1;                                                     // [C] Initialwert signalisiert "noch kein Status empfangen"
    if (err == ESP_OK) {                                                 // [Logik] nur weiterlesen, wenn Senden erfolgreich war
        if (esp_http_client_fetch_headers(client) < 0) {                // [HW] liest HTTP-Response-Header; negativer Rueckgabewert = Fehler
            ESP_LOGE(TAG, "no response headers");
            err = ESP_FAIL;
        } else {
            status = esp_http_client_get_status_code(client);            // [HW] liefert den HTTP-Statuscode (z. B. 200, 429, 500)
            http_read_body(client, resp, sizeof(resp));                  // [HW] liest den Response-Body in den Stack-Puffer, begrenzt auf dessen Groesse
        }
    }
    esp_http_client_close(client);                                      // [HW] schliesst die TLS-Verbindung
    esp_http_client_cleanup(client);                                    // [C] gibt das Client-Handle und zugehoerige Ressourcen frei

    if (err != ESP_OK) {                                                // [Logik] Kommunikationsfehler weiterreichen, kein Parsing moeglich
        return err;
    }
    if (status != 200) {                                                // [HW] nur HTTP 200 gilt als erfolgreiche Antwort
        ESP_LOGE(TAG, "HTTP %d: %.256s", status, resp);                  // [C] %.256s begrenzt geloggte Zeichenanzahl
        // 429 (Rate-Limit) und 5xx sind transient -> Retry im Wrapper;
        // andere 4xx (z.B. 401, ungueltiger Key) sind permanent.
        return (status == 429 || status >= 500) ? ESP_FAIL              // [C] Ternaerer Operator waehlt Fehlercode je nach Statusklasse | [Logik] unterscheidet transiente von permanenten Fehlern
                                                : ESP_ERR_INVALID_RESPONSE;
    }

    err = parse_reply(resp, out);                                       // [Logik] HTTP-200-Body in strukturiertes Ergebnis (Text oder Tool-Call) zerlegen
    if (err == ESP_OK) {                                                // [Logik] nur bei erfolgreichem Parsing Erfolgsmeldung loggen
        unsigned ms = (unsigned)(esp_log_timestamp() - t0);              // [C] Cast auf unsigned; Differenz zweier Zeitstempel ergibt Dauer in ms | [Logik] Latenzmessung fuer Log-Ausgabe
        if (out->type == LLM_RESULT_TOOL_CALL) {                        // [Logik] Verzweigung je nach Ergebnistyp fuer passendes Log-Format
            ESP_LOGI(TAG, "tool call in %u ms: %s(%s)", ms,
                     out->tool_name, out->tool_args);
        } else {
            ESP_LOGI(TAG, "reply in %u ms: \"%s\"", ms, out->text);
        }
    }
    return err;                                                         // [Logik] Ergebnis (ESP_OK oder Fehlercode) an llm_generate() zurueckgeben
}

// Oeffentliche Einstiegsfunktion: schickt user_text ans LLM und fuellt
// out mit Text- oder Tool-Call-Ergebnis. Wiederholt generate_once() bei
// transienten Fehlern mit Backoff-Verzoegerung (siehe http_util.h).
esp_err_t llm_generate(const char *user_text, llm_result_t *out)
{
    esp_err_t err = ESP_FAIL;                                           // [C] Initialwert falls die Schleife nie ESP_OK liefert
    for (int attempt = 0; attempt < HTTP_MAX_ATTEMPTS; attempt++) {     // [C] klassische Zaehlschleife ueber die maximale Versuchsanzahl | [Logik] Retry-Mechanismus fuer transiente Netzwerkfehler
        if (attempt > 0) {                                              // [Logik] beim ersten Versuch keine Wartezeit
            http_backoff_delay(attempt, TAG);                          // [HW] wartet steigende Zeitspannen zwischen Wiederholungsversuchen (Exponential Backoff)
        }
        err = generate_once(user_text, out);                           // [Logik] eigentlicher Request-Versuch
        if (!http_err_is_transient(err)) {                             // [C] logische Negation | [Logik] bei permanentem Fehler oder Erfolg sofort abbrechen, kein weiterer Versuch
            break;
        }
    }
    return err;                                                        // [Logik] Endergebnis (Erfolg oder letzter Fehler) an den Aufrufer (network_task) zurueckgeben
}
