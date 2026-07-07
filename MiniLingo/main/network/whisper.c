// Groq Whisper STT (Spec Section 9.3, Phase 5): Multipart-Upload des
// Recording-Buffers per esp_http_client_open/_write, Response mit
// cJSON parsen. TLS-Zertifikate kommen aus dem esp_crt_bundle.
#include <ctype.h>                 // [C] Header-Include | [Logik] fuer isspace() beim Trimmen des Transkripts
#include <stdio.h>                 // [C] Header-Include | [Logik] fuer snprintf() beim Aufbau von Header-/Body-Strings
#include <string.h>                // [C] Header-Include | [Logik] fuer strlen()/strcmp-artige Stringoperationen

#include "cJSON.h"                 // [C] Header-Include | [HW] JSON-Parser fuer die Whisper-API-Antwort
#include "esp_crt_bundle.h"        // [C] Header-Include | [HW] liefert esp_crt_bundle_attach fuer TLS-Zertifikatspruefung
#include "esp_http_client.h"       // [C] Header-Include | [HW] ESP-IDF HTTP(S)-Client-API fuer den Whisper-Request
#include "esp_log.h"               // [C] Header-Include | [Logik] Logging-Makros ESP_LOGx
#include "sdkconfig.h"             // [C] Header-Include | [HW] projektspezifische menuconfig-Einstellungen

#include "config.h"                // [C] Header-Include | [Logik] liefert config_groq_key()/config_whisper_language() aus der Laufzeit-Konfiguration
#include "http_util.h"             // [C] Header-Include | [HW] gemeinsame HTTP-Hilfsfunktionen (Retry/Backoff, Body lesen)
#include "whisper.h"               // [C] Header-Include | [Logik] eigene Schnittstellendeklaration, Konsistenzpruefung durch Compiler

static const char *TAG = "whisper";  // [C] static = Datei-lokale (interne) Linkage | [Logik] Log-Tag zur Zuordnung der Ausgaben

#define WHISPER_URL   "https://api.groq.com/openai/v1/audio/transcriptions"  // [C] Praeprozessor-Konstante (Textersetzung) | [HW] HTTPS-Endpunkt der Groq-Whisper-Transkriptions-API
// whisper-large-v3-turbo: schneller als large-v3 bei praktisch
// identischer Qualitaet (Spec Section 9.3).
#define WHISPER_MODEL "whisper-large-v3-turbo"  // [C] Praeprozessor-Konstante | [HW] Modellname wird als Multipart-Feld "model" mitgeschickt
#define BOUNDARY      "----esp32minilingo"      // [C] Praeprozessor-Konstante | [HW] Trennstring fuer multipart/form-data-Upload

// Whisper-Antworten sind kurz (max 8 s Sprache); 2 KB reichen fuer
// das JSON inkl. Transkript locker.
#define RESPONSE_MAX  2048         // [C] Praeprozessor-Konstante | [HW] Groesse des Stack-Puffers fuer den HTTP-Response-Body

#define TIMEOUT_MS    20000        // [C] Praeprozessor-Konstante | [HW] Socket-/Request-Timeout fuer esp_http_client in Millisekunden

static const char *PART_FMT =                                          // [C] static = Datei-lokale Linkage, Zeiger auf String-Literal | [HW] Format-String fuer die multipart/form-data-Praeambel
    "--" BOUNDARY "\r\n"                                                // [C] String-Literal-Konkatenation durch Compiler | [HW] Boundary-Marker leitet einen neuen Multipart-Teil ein
    "Content-Disposition: form-data; name=\"model\"\r\n\r\n"            // [HW] HTTP-Header des Teils: benanntes Formularfeld "model"
    WHISPER_MODEL "\r\n"                                                // [C] Makro-Expansion zu Modellname | [HW] Feldwert des "model"-Parts
    "--" BOUNDARY "\r\n"                                                // [HW] naechster Multipart-Teil beginnt
    "Content-Disposition: form-data; name=\"response_format\"\r\n\r\n"  // [HW] Formularfeld "response_format"
    "json\r\n"                                                         // [HW] fordert JSON-formatierte Antwort statt Klartext an
    "--" BOUNDARY "\r\n"                                                // [HW] naechster Multipart-Teil beginnt
    "Content-Disposition: form-data; name=\"language\"\r\n\r\n"         // [HW] Formularfeld "language" fuer Spracherkennungs-Hinweis
    "%s\r\n"                                                            // [C] printf-Platzhalter, wird per snprintf durch die konfigurierte Sprache ersetzt
    "--" BOUNDARY "\r\n"                                                // [HW] letzter Multipart-Teil beginnt (die Audiodatei)
    "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n"  // [HW] Formularfeld "file" mit Dateinamen fuer den Upload
    "Content-Type: audio/wav\r\n\r\n";                                  // [HW] MIME-Type der angehaengten Audiodaten

static const char *EPILOGUE = "\r\n--" BOUNDARY "--\r\n";  // [C] static const Zeiger auf String-Literal | [HW] schliessender Boundary-Marker (mit "--" Suffix) beendet den Multipart-Body

// Parst die JSON-Antwort der Whisper-API und extrahiert das Feld
// "text" (das eigentliche Transkript). Entfernt fuehrende Leerzeichen
// und schreibt das Ergebnis nul-terminiert nach text/text_len.
static esp_err_t parse_transcript(const char *json, char *text,   // [C] static = nur in dieser Datei sichtbare Hilfsfunktion
                                  size_t text_len)
{
    cJSON *root = cJSON_Parse(json);           // [C] Zeiger auf geparstes JSON-Objekt (Heap-Allokation durch cJSON) | [HW] JSON-Antwort-Body wird geparst
    if (!root) {                               // [C] Nullzeiger-Pruefung, implizite Bool-Konvertierung | [Logik] Parsing fehlgeschlagen -> kein gueltiges JSON
        ESP_LOGE(TAG, "response is not valid JSON");  // [Logik] Fehlerprotokollierung
        return ESP_FAIL;                       // [Logik] Abbruch mit generischem Fehlercode
    }

    esp_err_t ret = ESP_FAIL;                              // [C] Initialisierung mit Default-Fehlerwert | [Logik] wird nur bei Erfolg auf ESP_OK gesetzt
    const cJSON *field = cJSON_GetObjectItem(root, "text"); // [C] Zeiger auf JSON-Unterobjekt/-Feld | [HW] sucht das "text"-Feld der Whisper-JSON-Antwort
    if (cJSON_IsString(field)) {                            // [Logik] prueft Feldtyp, bevor auf valuestring zugegriffen wird
        // Whisper liefert oft ein fuehrendes Leerzeichen mit.
        const char *src = field->valuestring;               // [C] Struct-Zeiger-Zugriff auf den String-Wert des JSON-Feldes
        while (*src && isspace((unsigned char)*src)) {       // [C] Pointer-Dereferenzierung + Cast zu unsigned char (korrektes isspace-Verhalten) | [Logik] ueberspringt fuehrende Whitespace-Zeichen
            src++;                                           // [C] Pointer-Arithmetik: naechstes Zeichen im String
        }
        if (*src) {                                          // [C] Dereferenzierung | [Logik] pruefen ob nach dem Trimmen noch Inhalt uebrig ist
            snprintf(text, text_len, "%s", src);              // [C] Funktionsaufruf mit variabler Formatierung | [Logik] kopiert getrimmten String sicher (laengenbegrenzt) in den Ausgabepuffer
            ret = ESP_OK;                                     // [Logik] Erfolg: nicht-leeres Transkript vorhanden
        } else {
            ESP_LOGW(TAG, "empty transcript");                // [Logik] Warnung: Whisper lieferte nur Leerzeichen/nichts
        }
    } else {
        ESP_LOGE(TAG, "no \"text\" field in response: %.256s", json);  // [Logik] Fehlerfall: Antwortstruktur unerwartet, Auszug wird geloggt
    }
    cJSON_Delete(root);       // [C] Funktionsaufruf gibt Heap-Speicher des JSON-Baums frei | [Logik] verhindert Memory-Leak
    return ret;               // [Logik] ESP_OK nur bei erfolgreichem, nicht-leerem Transkript
}

// Fuehrt einen einzelnen (nicht wiederholten) HTTPS-Request gegen die
// Groq-Whisper-API aus: baut Multipart-Body, sendet Header+Audio+
// Epilog, liest die Antwort und parst das Transkript. Wird vom
// Retry-Wrapper whisper_transcribe() aufgerufen.
static esp_err_t transcribe_once(const uint8_t *wav, size_t wav_bytes,  // [C] static Hilfsfunktion, nur intern sichtbar
                                 char *text, size_t text_len)
{
    if (strlen(config_groq_key()) == 0) {                 // [C] Funktionsaufruf liefert Zeiger, strlen darauf | [Logik] pruefen ob ueberhaupt ein API-Key konfiguriert ist
        ESP_LOGE(TAG, "Groq API key not set (Setup-Portal/menuconfig)");  // [Logik] Fehlermeldung mit Hinweis auf Konfigurationsquelle
        return ESP_ERR_INVALID_STATE;                     // [Logik] permanenter Fehler, kein Retry sinnvoll (kein Netzwerkproblem)
    }

    char prologue[512];                                    // [C] Stack-Array fester Groesse fuer den Multipart-Header-Teil
    int prologue_len = snprintf(prologue, sizeof(prologue), PART_FMT,  // [C] snprintf liefert die (potentielle) Ausgabelaenge als int zurueck | [HW] fuellt PART_FMT-Template mit der konfigurierten Sprache
                                config_whisper_language());
    if (prologue_len <= 0 || prologue_len >= (int)sizeof(prologue)) {  // [C] Cast zu int fuer Vergleich mit sizeof (size_t) | [Logik] Formatierungsfehler oder Truncation erkennen
        return ESP_FAIL;                                    // [Logik] Abbruch, da der Header sonst unvollstaendig waere
    }

    esp_http_client_config_t cfg = {              // [C] Struct-Initialisierung mit Designated Initializers
        .url = WHISPER_URL,                       // [HW] Ziel-URL des HTTPS-Requests
        .method = HTTP_METHOD_POST,               // [HW] HTTP-Methode POST fuer den Datei-Upload
        .crt_bundle_attach = esp_crt_bundle_attach,  // [C] Funktionszeiger-Zuweisung | [HW] bindet das eingebettete CA-Zertifikatsbuendel fuer TLS-Verifizierung ein
        .timeout_ms = TIMEOUT_MS,                 // [HW] Timeout fuer Verbindungsaufbau/Antwort in ms
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);  // [C] Handle (opaker Zeiger) auf den initialisierten HTTP-Client | [HW] legt Client anhand der Konfiguration an
    if (!client) {                                 // [C] Nullzeiger-Pruefung | [Logik] Init fehlgeschlagen (z.B. OOM)
        return ESP_FAIL;                           // [Logik] Abbruch ohne Cleanup, da kein Handle existiert
    }

    char auth[160];                                // [C] Stack-Puffer fuer den Authorization-Header-String
    snprintf(auth, sizeof(auth), "Bearer %s", config_groq_key());  // [C] Stringformatierung mit Laengenbegrenzung | [HW] Bearer-Token-Authentifizierung gemaess Groq-API
    esp_http_client_set_header(client, "Authorization", auth);     // [HW] setzt HTTP-Header "Authorization" fuer den Request
    esp_http_client_set_header(client, "Content-Type",             // [HW] setzt HTTP-Header "Content-Type"
                               "multipart/form-data; boundary=" BOUNDARY);  // [C] Makro-Expansion von BOUNDARY in den Header-Wert | [HW] kennzeichnet multipart/form-data-Body mit Boundary-String

    size_t content_len =                                    // [C] Summenbildung mehrerer Groessen als size_t
        (size_t)prologue_len + wav_bytes + strlen(EPILOGUE);  // [C] Cast von int zu size_t | [HW] Gesamtlaenge des Request-Bodys (Content-Length) aus Praeambel + Audiodaten + Epilog
    esp_err_t err = esp_http_client_open(client, (int)content_len);  // [C] Cast zu int fuer die API-Signatur | [HW] oeffnet die TLS-Verbindung und kuendigt die Body-Laenge an
    if (err != ESP_OK) {                                     // [Logik] Verbindungsaufbau fehlgeschlagen
        ESP_LOGE(TAG, "connect failed: %s", esp_err_to_name(err));  // [Logik] Fehlercode in lesbaren Namen umwandeln und loggen
        esp_http_client_cleanup(client);                     // [HW] gibt Client-Ressourcen frei, da kein weiterer Request folgt
        return err;                                          // [Logik] Fehler an Aufrufer weiterreichen
    }

    int64_t t0 = esp_log_timestamp();                        // [C] 64-Bit Ganzzahl fuer Zeitstempel | [Logik] Startzeit zur spaeteren Dauermessung des Requests
    err = http_write_all(client, prologue, (size_t)prologue_len);  // [C] Cast von int zu size_t | [HW] sendet den Multipart-Header (Model/Format/Sprache) ueber die offene HTTPS-Verbindung
    if (err == ESP_OK) {                                     // [Logik] nur weiterschreiben, wenn vorheriger Schritt erfolgreich war
        err = http_write_all(client, (const char *)wav, wav_bytes);  // [C] Cast von uint8_t* zu const char* | [HW] sendet die eigentlichen WAV-Audiodaten als Body-Teil
    }
    if (err == ESP_OK) {                                     // [Logik] nur senden, wenn Audio-Upload erfolgreich war
        err = http_write_all(client, EPILOGUE, strlen(EPILOGUE));  // [HW] sendet den abschliessenden Boundary-Marker, der den Multipart-Body beendet
    }

    char body[RESPONSE_MAX];                                 // [C] Stack-Puffer fuer den kompletten Response-Body
    int status = -1;                                         // [C] Initialisierung mit ungueltigem Statuswert | [Logik] Sentinel, falls kein Statuscode gelesen wird
    if (err == ESP_OK) {                                      // [Logik] Header/Response nur lesen, wenn der Upload fehlerfrei war
        if (esp_http_client_fetch_headers(client) < 0) {       // [C] Vergleich mit negativem Wert (Fehlercode) | [HW] liest die HTTP-Response-Header ein
            ESP_LOGE(TAG, "no response headers");              // [Logik] keine Header erhalten -> Fehlerfall
            err = ESP_FAIL;                                    // [Logik] Fehlerstatus setzen fuer nachfolgende Pruefung
        } else {
            status = esp_http_client_get_status_code(client);  // [HW] liest den HTTP-Statuscode (z.B. 200, 401, 429) aus der Antwort
            http_read_body(client, body, sizeof(body));         // [HW] liest den Response-Body (JSON) in den lokalen Puffer
        }
    }
    esp_http_client_close(client);       // [HW] schliesst die TLS-/HTTP-Verbindung
    esp_http_client_cleanup(client);     // [HW] gibt das Client-Handle und zugehoerige Ressourcen frei

    if (err != ESP_OK) {                 // [Logik] Uebertragungsfehler (Schreiben/Lesen) -> direkt mit Fehler zurueck
        return err;                      // [Logik] Fehlercode an Aufrufer (Retry-Wrapper) weiterreichen
    }
    if (status != 200) {                 // [C] Ganzzahlvergleich | [Logik] kein HTTP-Erfolgsstatus
        ESP_LOGE(TAG, "HTTP %d: %.256s", status, body);  // [Logik] Statuscode und (gekuerzten) Body zur Diagnose loggen
        // 429 (Rate-Limit) und 5xx sind transient -> Retry im Wrapper;
        // andere 4xx (z.B. 401, ungueltiger Key) sind permanent.
        return (status == 429 || status >= 500) ? ESP_FAIL   // [C] Ternaerer Operator | [Logik] transiente Serverfehler signalisieren, damit der Wrapper erneut versucht
                                                : ESP_ERR_INVALID_RESPONSE;  // [Logik] permanenter Client-/Auth-Fehler, kein Retry sinnvoll
    }

    err = parse_transcript(body, text, text_len);  // [C] Funktionsaufruf, Rueckgabewert wird ueberschrieben | [Logik] extrahiert das Transkript aus dem JSON-Body
    if (err == ESP_OK) {                           // [Logik] nur bei erfolgreichem Parsing zusaetzlich loggen
        ESP_LOGI(TAG, "transcribed %u KB in %u ms: \"%s\"",       // [Logik] Erfolgsmeldung mit Groesse/Dauer/Ergebnis
                 (unsigned)(wav_bytes / 1024),                    // [C] Cast zu unsigned, Ganzzahldivision fuer KB-Angabe
                 (unsigned)(esp_log_timestamp() - t0), text);     // [C] Cast zu unsigned, Zeitdifferenz seit t0 | [Logik] Dauer des gesamten Requests in ms
    }
    return err;   // [Logik] ESP_OK nur bei erfolgreichem Parsing eines nicht-leeren Transkripts
}

// Oeffentliche Schnittstelle: transkribiert einen WAV-Buffer per Groq
// Whisper mit automatischem Retry bei transienten Fehlern (Rate-Limit,
// 5xx, Netzwerkfehler). Wird aus network_task (cloud.c) aufgerufen.
esp_err_t whisper_transcribe(const uint8_t *wav, size_t wav_bytes,  // [C] oeffentliche (nicht static) Funktion, Deklaration in whisper.h
                             char *text, size_t text_len)
{
    esp_err_t err = ESP_FAIL;                            // [C] Initialisierung mit Default-Fehlerwert, falls Schleife nie erfolgreich ist
    for (int attempt = 0; attempt < HTTP_MAX_ATTEMPTS; attempt++) {  // [C] Zaehlschleife ueber die maximale Anzahl Versuche | [Logik] Retry-Schleife fuer transiente Fehler
        if (attempt > 0) {                                // [Logik] beim ersten Versuch keine Wartezeit
            http_backoff_delay(attempt, TAG);             // [HW] wartet mit steigendem Backoff vor erneutem Request (schont Server/Netzwerk)
        }
        err = transcribe_once(wav, wav_bytes, text, text_len);  // [Logik] eigentlicher Transkriptionsversuch
        if (!http_err_is_transient(err)) {                // [C] Negation eines Funktionsergebnisses | [Logik] bei permanentem Fehler oder Erfolg Schleife abbrechen
            break;                                        // [Logik] kein weiterer Versuch noetig/sinnvoll
        }
    }
    return err;   // [Logik] Ergebnis des letzten Versuchs (Erfolg oder finaler Fehler)
}
