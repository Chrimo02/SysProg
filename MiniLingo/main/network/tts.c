// TTS: primaer ein lokaler Piper-Server im LAN (HTTP POST -> rohe
// WAV-Bytes, 44-Byte-Header ueberspringen, PCM direkt in die I2S-Pipeline),
// Gemini nur noch als Fallback wenn der lokale Server fehlschlaegt oder das
// 3-s-Timeout reisst.
//
// Gemini-Fallback (Spec Section 9.5, Phase 7): responseModalities=AUDIO,
// Response-JSON in PSRAM puffern, inlineData.data per
// mbedtls_base64_decode in den PCM-Buffer dekodieren. Die Sample-Rate
// kommt aus dem mimeType ("audio/L16;codec=pcm;rate=24000").
#include <ctype.h>          // [C] Header-Include | [Logik] tolower() fuer Gross-/Kleinschreibung bei der Spracherkennung
#include <stdbool.h>        // [C] Header-Include | [C] liefert den Typ bool/true/false
#include <stdlib.h>         // [C] Header-Include | [Logik] atoi() zum Parsen der Sample-Rate aus dem mimeType-String
#include <string.h>         // [C] Header-Include | [Logik] strlen/strstr/strncmp/memcmp fuer String- und Buffer-Vergleiche

#include "cJSON.h"              // [C] Header-Include | [HW] JSON-Bibliothek zum Bauen/Parsen der TTS-Request/Response-Payloads
#include "esp_crt_bundle.h"     // [C] Header-Include | [HW] liefert das eingebettete CA-Zertifikatsbuendel fuer TLS gegen Gemini
#include "esp_heap_caps.h"      // [C] Header-Include | [HW] heap_caps_malloc() fuer gezielte PSRAM-Allokation
#include "esp_http_client.h"    // [C] Header-Include | [HW] ESP-IDF HTTP(S)-Client-API fuer beide TTS-Backends
#include "esp_log.h"            // [C] Header-Include | [Logik] ESP_LOGx-Makros fuer Diagnose-Ausgaben
#include "mbedtls/base64.h"     // [C] Header-Include | [HW] Base64-Decoder fuer die inlineData.data aus der Gemini-Antwort
#include "sdkconfig.h"          // [C] Header-Include | [HW] Projekt-Konfiguration aus menuconfig

#include "config.h"       // [C] Header-Include | [Logik] Zugriff auf gespeicherte Konfiguration (API-Key, Default-Stimme)
#include "http_util.h"    // [C] Header-Include | [HW] gemeinsame HTTP-Hilfsfunktionen (Write/Read/Backoff/Retry-Klassifikation)
#include "tts.h"          // [C] Header-Include | [Logik] eigene Modul-Schnittstelle (Deklarationen dieser .c-Datei)

static const char *TAG = "tts";   // [C] static = Datei-lokale (interne) Linkage | [Logik] Log-Tag zur Zuordnung aller Ausgaben dieses Moduls

// Lokaler Piper-TTS-Server (primaere Quelle, kein TLS): liefert rohe
// WAV-Bytes (PCM, mono, 16-bit). Bei Fehler/Timeout faellt tts_synthesize
// auf Gemini zurueck.
#define TTS_LOCAL_URL        "http://192.168.3.143:5050/tts"   // [C] Praeprozessor-Konstante (Textersetzung) | [HW] Klartext-HTTP (kein TLS) im LAN, feste IP:Port des Piper-Servers
#define TTS_LOCAL_VOICE      "jarvis"   // [C] Praeprozessor-Konstante | [Logik] Fallback-Default, per-character ueberschreibbar
#define TTS_LOCAL_TIMEOUT_MS 3000       // [C] Praeprozessor-Konstante | [HW] Socket-/Request-Timeout in ms, danach Fallback auf Gemini
#define WAV_HEADER_BYTES     44         // [C] Praeprozessor-Konstante | [HW] kanonischer RIFF/WAVE-Header-Groesse in Byte
#define LANG_DETECT_MAX      512        // [C] Praeprozessor-Konstante | [Logik] genug Text fuer die Stopwort-Heuristik

// Preview-Modellname als Konstante (Spec Section 13: kann sich aendern).
#define TTS_MODEL "gemini-2.5-flash-preview-tts"   // [C] Praeprozessor-Konstante | [HW] Modell-Bezeichner im Gemini-API-Pfad
#define TTS_URL \
    "https://generativelanguage.googleapis.com/v1beta/models/" \
    TTS_MODEL ":generateContent"
// [C] Makro-Expansion: TTS_URL wird durch Verkettung mehrerer String-Literale inkl. TTS_MODEL zur vollstaendigen URL zusammengesetzt | [HW] HTTPS-Endpoint der Gemini generateContent-API

// ~10 s Audio (maxOutputTokens=200) sind 480 KB PCM = 640 KB Base64 +
// JSON-Overhead (Spec Section 13). Etwas Luft fuer laengere Antworten.
#define RESPONSE_MAX (1536 * 1024)   // [C] Praeprozessor-Konstante, Ausdruck wird zur Compile-Zeit ausgewertet | [HW] maximale Groesse des HTTP-Response-Puffers in Byte
#define PCM_MAX      (1024 * 1024)   // [C] Praeprozessor-Konstante | [HW] maximale Groesse des dekodierten PCM-Puffers in Byte
#define DEFAULT_RATE 24000           // [C] Praeprozessor-Konstante | [HW] Default-Sample-Rate falls mimeType keine rate=... enthaelt

// TTS-Generierung braucht laenger als der Text-Endpoint.
#define TIMEOUT_MS 30000   // [C] Praeprozessor-Konstante | [HW] HTTP-Timeout in ms fuer den Gemini-Request

static char *s_resp;     // [C] static = Datei-lokale Linkage, globaler Zeiger | [HW] Response-Puffer (PSRAM): Gemini-JSON oder lokale WAV-Bytes
static int16_t *s_pcm;   // [C] static globaler Zeiger auf int16_t-Array | [HW] dekodiertes Gemini-PCM (PSRAM)
static const char *s_voice;        // [C] static globaler Zeiger, const char* | [Logik] NULL = config_gemini_voice() als Fallback
static const char *s_local_voice;  // [C] static globaler Zeiger, const char* | [Logik] NULL = TTS_LOCAL_VOICE als Fallback

// Initialisiert das TTS-Modul: reserviert die beiden PSRAM-Puffer fuer
// HTTP-Response und dekodiertes PCM. Muss vor dem ersten tts_synthesize()
// aufgerufen werden, sonst zeigen s_resp/s_pcm ins Leere.
esp_err_t tts_init(void)   // [C] Rueckgabetyp esp_err_t (Fehlercode); void = keine Parameter
{
    s_resp = heap_caps_malloc(RESPONSE_MAX, MALLOC_CAP_SPIRAM);   // [C] Zuweisung an globalen Zeiger, Rueckgabe von malloc-artiger Funktion | [HW] Allokation explizit im externen PSRAM statt internem RAM
    s_pcm = heap_caps_malloc(PCM_MAX, MALLOC_CAP_SPIRAM);         // [C] Zuweisung an globalen Zeiger | [HW] PCM-Puffer ebenfalls im PSRAM (interner RAM ist zu klein/knapp)
    if (!s_resp || !s_pcm) {                                     // [C] Zeiger-Nullpruefung ueber Negation | [Logik] Fehlerfall: mindestens eine Allokation ist fehlgeschlagen
        ESP_LOGE(TAG, "PSRAM buffer alloc failed");               // [Logik] Fehler-Log vor Abbruch
        return ESP_ERR_NO_MEM;                                   // [Logik] Fehlercode fuer Speichermangel an Aufrufer zurueckgeben
    }
    return ESP_OK;                                               // [Logik] Erfolg
}

// Setzt die modul-interne Gemini-Stimme (Fallback-Pfad). Der uebergebene
// Zeiger wird nur gespeichert, nicht kopiert — der Aufrufer muss ein
// langlebiges/statisches String-Literal uebergeben.
void tts_set_voice(const char *voice)   // [C] void-Rueckgabe, Parameter const char* = schreibgeschuetzter String-Zeiger
{
    s_voice = voice;   // [C] einfache Zeiger-Zuweisung, kein strcpy | [Logik] merkt sich die aktive Gemini-Stimme fuer folgende Requests
}

// Setzt die modul-interne Stimme fuer den lokalen Piper-Server. Analog zu
// tts_set_voice, aber fuer den primaeren (lokalen) TTS-Pfad.
void tts_set_local_voice(const char *voice)   // [C] void-Rueckgabe, Parameter const char*
{
    s_local_voice = voice;   // [C] Zeiger-Zuweisung | [Logik] merkt sich die aktive lokale Piper-Stimme
}

// Sehr einfache Spracherkennung (kein ML, kein Netz): Stopwort-Score
// Deutsch vs. Englisch plus Umlaute/sz. Reicht, um die richtige
// Piper-Stimme zu waehlen — die lokalen Voices sind einsprachige
// Modelle, englischer Text mit der (oft eingestellten) Deutsch-Stimme
// (oder umgekehrt) klingt deshalb sehr falsch ausgesprochen.
static bool text_is_german(const char *text)   // [C] static = intere Sichtbarkeit, Rueckgabetyp bool
{
    static const char *de_words[] = {   // [C] static = einmalig angelegtes, Datei-lokales Array von String-Literal-Zeigern, ueberlebt Funktionsaufrufe (aber nur lesend genutzt)
        " der ", " die ", " das ", " und ", " ist ", " nicht ", " ich ",
        " du ", " wir ", " mit ", " fuer ", " ein ", " eine ", " auf ",
        " sind ", " war ", " hat ", " kann ", " auch ", " sehr ",
        " schon ", " noch ", " mal ", " gerne ", " bitte ", " ihnen ",
    };
    static const char *en_words[] = {   // [C] static const Array von Zeigern | [Logik] Stopwort-Liste fuer Englisch-Erkennung
        " the ", " is ", " and ", " you ", " are ", " what ", " this ",
        " that ", " with ", " for ", " have ", " can ", " will ",
        " your ", " it's ", " of ", " to ", " a ", " i'm ", " sir ",
    };

    char buf[LANG_DETECT_MAX + 2];   // [C] Stack-Array fester Groesse (Makro-Konstante + 2 fuer Rand-Leerzeichen/Nullterminator)
    size_t len = strlen(text);       // [C] size_t = vorzeichenloser Groessentyp | [Logik] Laenge des Eingabetexts ermitteln
    if (len > sizeof(buf) - 3) {      // [C] sizeof liefert Puffergroesse zur Compile-Zeit | [Logik] Kappen, wenn Text laenger als Puffer minus Rand-Bytes
        len = sizeof(buf) - 3;       // [Logik] Laenge auf maximal sicheren Wert begrenzen (Buffer-Overflow-Schutz)
    }
    buf[0] = ' ';   // [C] Array-Indexzugriff, Zeichen-Literal | [Logik] fuehrendes Leerzeichen, damit Stopwoerter am Textanfang matchen (" der " statt "der ")
    for (size_t i = 0; i < len; i++) {   // [C] klassische Zaehlschleife ueber size_t-Index
        buf[i + 1] = (char)tolower((unsigned char)text[i]);   // [C] expliziter Cast auf unsigned char vor tolower (Standard-Vorgabe), danach zurueck auf char | [Logik] Normalisierung auf Kleinschreibung fuer Wortvergleich
    }
    buf[len + 1] = ' ';    // [C] Array-Index | [Logik] abschliessendes Leerzeichen, damit Stopwoerter am Textende matchen
    buf[len + 2] = '\0';   // [C] Nullterminator setzt String-Ende | [Logik] macht buf zu einem gueltigen C-String fuer strstr

    int de_score = 0, en_score = 0;   // [C] zwei int-Variablen in einer Deklaration | [Logik] Zaehler fuer Treffer je Sprache
    for (size_t i = 0; i < sizeof(de_words) / sizeof(de_words[0]); i++) {   // [C] sizeof-Trick zur Ermittlung der Array-Elementanzahl
        if (strstr(buf, de_words[i])) {   // [C] strstr liefert Zeiger auf Fundstelle oder NULL, hier als Wahrheitswert genutzt | [Logik] Stopwort im normalisierten Text gefunden?
            de_score++;   // [C] Postfix-Inkrement | [Logik] Deutsch-Score erhoehen
        }
    }
    for (size_t i = 0; i < sizeof(en_words) / sizeof(en_words[0]); i++) {   // [C] Elementanzahl-Berechnung per sizeof
        if (strstr(buf, en_words[i])) {   // [Logik] Englisch-Stopwort gefunden?
            en_score++;   // [Logik] Englisch-Score erhoehen
        }
    }
    // UTF-8-Praefixbyte von ae/oe/ue/sz (0xC3 0x84-0xBF) ist ein starkes
    // Deutsch-Signal und faengt auch kurze Antworten ohne Stopwoerter ab.
    for (size_t i = 0; i < len; i++) {   // [C] Schleife ueber alle Rohbytes des Originaltexts
        if ((unsigned char)text[i] == 0xC3) {   // [C] Cast auf unsigned char verhindert Sign-Extension-Probleme beim Vergleich | [HW] 0xC3 ist das UTF-8-Leadbyte fuer den Bereich der deutschen Umlaute/ß
            de_score += 2;   // [C] Kombinierter Zuweisungsoperator += | [Logik] Umlaut wiegt staerker als ein einzelnes Stopwort
        }
    }
    return de_score >= en_score;   // [C] Vergleichsoperator liefert bool | [Logik] bei Gleichstand wird Deutsch bevorzugt
}

// Request-Body fuer den lokalen Piper-Server: {"text":...,"voice":...}.
// Rueckgabe via cJSON_free().
static char *build_local_request(const char *text, const char *voice)   // [C] static Funktion, Rueckgabe eines heap-allokierten char*
{
    cJSON *root = cJSON_CreateObject();   // [C] Zeiger auf neu erzeugtes cJSON-Objekt (Heap-Allokation durch die Bibliothek) | [HW] Wurzelobjekt des JSON-Request-Bodys
    if (!root) {   // [C] Nullpruefung per Negation | [Logik] Allokation fehlgeschlagen
        return NULL;   // [Logik] Fehlerfall signalisieren
    }
    cJSON_AddStringToObject(root, "text", text);     // [HW] fuegt Feld "text" mit dem zu sprechenden Text zum JSON hinzu
    cJSON_AddStringToObject(root, "voice", voice);   // [HW] fuegt Feld "voice" mit dem gewuenschten Piper-Stimmennamen hinzu
    char *body = cJSON_PrintUnformatted(root);       // [C] Serialisierung des JSON-Baums in einen neu allokierten String | [HW] kompaktes (nicht eingerücktes) JSON fuer den HTTP-Body
    cJSON_Delete(root);                              // [Logik] Baumstruktur wird nicht mehr gebraucht, Speicher freigeben (Body-String bleibt separat bestehen)
    return body;                                     // [Logik] Aufrufer erhaelt den fertigen JSON-String, muss ihn spaeter mit cJSON_free() freigeben
}

// HTTP POST an den lokalen Piper-Server. Antwort sind rohe WAV-Bytes; der
// 44-Byte-Header wird uebersprungen, die Sample-Rate aus dem Header gelesen
// und *pcm zeigt auf die rohen PCM-Frames in s_resp (gueltig bis zum
// naechsten Aufruf, genau ein Aufrufer wartet die Wiedergabe ab).
static esp_err_t synthesize_local(const char *text, const int16_t **pcm,
                                  size_t *samples, uint32_t *sample_rate)
// [C] static Funktion, Ausgabeparameter als Zeiger (**pcm, *samples, *sample_rate)
{
    // Die lokalen Voices sind einsprachig: bei Deutsch immer "de" senden,
    // unabhaengig von der per-character Stimme — sonst spricht z. B.
    // "jarvis" (Englisch-Modell) deutschen Text in falscher Phonetik.
    const char *voice = text_is_german(text)
                             ? "de"
                             : (s_local_voice ? s_local_voice : TTS_LOCAL_VOICE);
    // [C] ternaerer Operator, verschachtelt | [Logik] Sprachheuristik entscheidet ueber Stimmennamen, mit Fallback-Kette auf modul-globale bzw. Default-Stimme
    char *body = build_local_request(text, voice);   // [Logik] JSON-Request-Body fuer den lokalen Server erzeugen
    if (!body) {   // [C] Nullpruefung | [Logik] Allokation fehlgeschlagen
        return ESP_ERR_NO_MEM;   // [Logik] Speichermangel an Aufrufer melden
    }

    esp_http_client_config_t cfg = {   // [C] Struct-Initialisierung mit designierten Initialisierern (.feld = wert)
        .url = TTS_LOCAL_URL,           // [HW] Ziel-URL des lokalen Piper-Servers (Klartext-HTTP)
        .method = HTTP_METHOD_POST,     // [HW] HTTP-Methode POST fuer die Synthese-Anfrage
        .timeout_ms = TTS_LOCAL_TIMEOUT_MS,   // [HW] Socket-Timeout in ms, begrenzt maximale Wartezeit auf den lokalen Server
        .buffer_size = 8192,             // [HW] Groesse des internen Empfangspuffers des HTTP-Clients
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);   // [C] opaker Handle-Typ (Zeiger) auf den initialisierten Client | [HW] legt Client-Instanz gemaess cfg an
    if (!client) {   // [C] Nullpruefung des Handles | [Logik] Initialisierung fehlgeschlagen
        cJSON_free(body);   // [Logik] zuvor allokierten Body wieder freigeben, um Leak zu vermeiden
        return ESP_FAIL;    // [Logik] generischer Fehler zurueckgeben
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");   // [HW] setzt HTTP-Header, signalisiert dem Server JSON-Payload

    esp_err_t err = esp_http_client_open(client, (int)strlen(body));   // [C] Cast von size_t auf int (Content-Length-Parametertyp) | [HW] oeffnet die TCP-Verbindung und kuendigt die Content-Length an
    if (err != ESP_OK) {   // [Logik] Verbindungsaufbau fehlgeschlagen (z. B. Server nicht erreichbar/Timeout)
        ESP_LOGW(TAG, "local connect failed: %s", esp_err_to_name(err));   // [Logik] Warnung loggen (kein Fehler-Log, da Fallback auf Gemini vorgesehen ist)
        esp_http_client_cleanup(client);   // [HW] Client-Ressourcen wieder freigeben
        cJSON_free(body);                  // [Logik] Body-Speicher freigeben
        return err;                        // [Logik] konkreten Fehlercode an Aufrufer weiterreichen
    }

    int64_t t0 = esp_log_timestamp();          // [C] 64-bit Ganzzahl fuer Zeitstempel | [Logik] Startzeit fuer die spaetere Laufzeitmessung merken
    err = http_write_all(client, body, strlen(body));   // [HW] schreibt den kompletten JSON-Body auf den Socket (kann in mehreren Chunks intern erfolgen)
    cJSON_free(body);   // [Logik] Body wird nach dem Senden nicht mehr gebraucht, Speicher freigeben

    int status = -1;      // [C] Initialisierung mit Sentinel-Wert -1 | [Logik] markiert "noch kein gueltiger HTTP-Status gelesen"
    int body_len = 0;     // [Logik] Initialisierung, falls Lesen der Antwort scheitert
    if (err == ESP_OK) {   // [Logik] nur weiterlesen, wenn das Schreiben erfolgreich war
        if (esp_http_client_fetch_headers(client) < 0) {   // [HW] liest die HTTP-Response-Header, negativer Rueckgabewert = Fehler
            err = ESP_FAIL;   // [Logik] Header-Empfang fehlgeschlagen, als Fehler markieren
        } else {
            status = esp_http_client_get_status_code(client);   // [HW] liest den numerischen HTTP-Statuscode (z. B. 200)
            body_len = http_read_body(client, s_resp, RESPONSE_MAX);   // [HW] liest den Response-Body (rohe WAV-Bytes) in den PSRAM-Puffer, begrenzt auf RESPONSE_MAX
        }
    }
    esp_http_client_close(client);      // [HW] schliesst die TCP-Verbindung
    esp_http_client_cleanup(client);    // [HW] gibt Client-internen Speicher frei

    if (err != ESP_OK) {   // [Logik] Sende- oder Header-Fehler aus obigem Block
        ESP_LOGW(TAG, "local request failed: %s", esp_err_to_name(err));   // [Logik] Warnung loggen
        return err;   // [Logik] Fehlercode an Aufrufer zurueckgeben, Aufrufer faellt auf Gemini zurueck
    }
    if (status != 200) {   // [Logik] jeder Nicht-200-Status gilt als Fehlerfall fuer den lokalen Pfad
        ESP_LOGW(TAG, "local HTTP %d", status);   // [Logik] Statuscode loggen
        return ESP_FAIL;   // [Logik] generischer Fehler, Fallback auf Gemini
    }
    if (body_len <= WAV_HEADER_BYTES) {   // [Logik] Antwort ist kuerzer/gleich dem WAV-Header -> keine echten PCM-Daten enthalten
        ESP_LOGW(TAG, "local response too short (%d B)", body_len);   // [Logik] Warnung mit tatsaechlicher Laenge loggen
        return ESP_FAIL;   // [Logik] als Fehler behandeln
    }
    if (memcmp(s_resp, "RIFF", 4) != 0 || memcmp(s_resp + 8, "WAVE", 4) != 0) {
        // [C] memcmp vergleicht Rohbytes, Zeigerarithmetik s_resp + 8 springt zum Format-Tag | [HW] prueft die kanonischen RIFF/WAVE-Magic-Bytes des WAV-Containers
        ESP_LOGW(TAG, "local response is not a WAV");   // [Logik] Antwort entspricht nicht dem erwarteten Format
        return ESP_FAIL;   // [Logik] Fehler zurueckgeben
    }

    // Sample-Rate aus dem WAV-Header (Offset 24, uint32 LE) — Piper-Voices
    // laufen z. B. mit 22050 Hz; nicht hart kodieren.
    const uint8_t *hdr = (const uint8_t *)s_resp;   // [C] expliziter Cast von char* auf uint8_t* fuer byteweisen Zugriff
    *sample_rate = (uint32_t)hdr[24] | ((uint32_t)hdr[25] << 8) |
                   ((uint32_t)hdr[26] << 16) | ((uint32_t)hdr[27] << 24);
    // [C] Bitweises Zusammensetzen eines Little-Endian uint32 aus vier Einzelbytes via Shift (<<) und Oder (|) | [HW] WAV-Header-Feld sampleRate bei Byte-Offset 24
    // Offset 44 ist gerade -> 2-Byte-Ausrichtung bleibt erhalten, kein Kopieren.
    *pcm = (const int16_t *)(s_resp + WAV_HEADER_BYTES);   // [C] Zeigerarithmetik + Cast auf int16_t* | [HW] *pcm zeigt direkt auf die PCM-Nutzdaten hinter dem WAV-Header, ohne Kopie
    *samples = (size_t)(body_len - WAV_HEADER_BYTES) / sizeof(int16_t);   // [C] Cast auf size_t, Division durch sizeof(int16_t)=2 | [Logik] Anzahl der 16-bit-Samples aus der Byteanzahl berechnen
    ESP_LOGI(TAG, "local audio (voice=%s) in %u ms: %.1f s PCM @ %u Hz",
             voice, (unsigned)(esp_log_timestamp() - t0),
             (float)*samples / (float)*sample_rate, (unsigned)*sample_rate);
    // [Logik] Erfolgsmeldung mit Stimme, Latenz, PCM-Dauer in Sekunden und Sample-Rate zu Diagnosezwecken
    return ESP_OK;   // [Logik] Erfolg
}

// Request-Body nach Spec Section 9.5; Rueckgabe via cJSON_free().
static char *build_request(const char *text)   // [C] static Funktion, Rueckgabe eines heap-allokierten JSON-Strings
{
    cJSON *root = cJSON_CreateObject();   // [HW] Wurzelobjekt des Gemini-generateContent-Request-Bodys
    if (!root) {   // [Logik] Allokation fehlgeschlagen
        return NULL;   // [Logik] Fehlerfall signalisieren
    }

    cJSON *turn = cJSON_CreateObject();   // [HW] ein "content"-Turn-Objekt der Gemini-Chat-API
    cJSON *part = cJSON_CreateObject();   // [HW] ein "parts"-Element mit dem eigentlichen Text
    cJSON_AddStringToObject(part, "text", text);   // [HW] fuegt den zu synthetisierenden Text als "text"-Feld hinzu
    cJSON_AddItemToArray(cJSON_AddArrayToObject(turn, "parts"), part);   // [C] verschachtelter Funktionsaufruf: Rueckgabewert von cJSON_AddArrayToObject wird direkt als Argument weiterverwendet | [HW] haengt part in das "parts"-Array des Turns
    cJSON_AddItemToArray(cJSON_AddArrayToObject(root, "contents"), turn);   // [HW] haengt den Turn in das "contents"-Array der Anfrage

    cJSON *gen = cJSON_AddObjectToObject(root, "generationConfig");   // [HW] "generationConfig"-Objekt steuert das Antwortformat
    cJSON *mod = cJSON_AddArrayToObject(gen, "responseModalities");   // [HW] Array "responseModalities", legt fest, dass Audio statt Text erwartet wird
    cJSON_AddItemToArray(mod, cJSON_CreateString("AUDIO"));   // [HW] fordert Modalitaet "AUDIO" an
    cJSON *voice = cJSON_AddObjectToObject(
        cJSON_AddObjectToObject(
            cJSON_AddObjectToObject(gen, "speechConfig"), "voiceConfig"),
        "prebuiltVoiceConfig");
    // [C] dreifach verschachtelte Funktionsaufrufe bauen die JSON-Hierarchie speechConfig -> voiceConfig -> prebuiltVoiceConfig in einem Ausdruck auf
    cJSON_AddStringToObject(voice, "voiceName",
                            s_voice ? s_voice : config_gemini_voice());
    // [C] ternaerer Operator waehlt zwischen gesetztem Wert und Konfigurationsdefault | [HW] "voiceName" legt die konkrete Gemini-Stimme fest

    char *body = cJSON_PrintUnformatted(root);   // [HW] Serialisierung zu kompaktem JSON-String fuer den HTTP-Body
    cJSON_Delete(root);   // [Logik] gesamten JSON-Baum (inkl. aller Unterobjekte) wieder freigeben
    return body;   // [Logik] fertigen Request-Body zurueckgeben
}

// Extrahiert inlineData aus dem Response-JSON und dekodiert nach s_pcm.
static esp_err_t parse_audio(const char *json, size_t *samples,
                             uint32_t *sample_rate)
// [C] static Funktion, Ausgabeparameter *samples/*sample_rate per Zeiger
{
    cJSON *root = cJSON_Parse(json);   // [HW] parst den JSON-Response-Text in einen cJSON-Baum
    if (!root) {   // [Logik] Parsing fehlgeschlagen -> kein gueltiges JSON
        ESP_LOGE(TAG, "response is not valid JSON");   // [Logik] Fehler loggen
        return ESP_FAIL;   // [Logik] Fehler zurueckgeben
    }

    esp_err_t ret = ESP_FAIL;   // [C] Initialisierung mit Default-Fehlerwert | [Logik] wird erst bei erfolgreichem Fund auf ESP_OK gesetzt
    const cJSON *cand = cJSON_GetArrayItem(
        cJSON_GetObjectItem(root, "candidates"), 0);
    // [C] verschachtelter Aufruf: erst Objektfeld "candidates" holen, dann dessen erstes Array-Element (Index 0) | [HW] Gemini liefert Antworten in candidates[0]
    const cJSON *parts = cJSON_GetObjectItem(
        cJSON_GetObjectItem(cand, "content"), "parts");
    // [HW] navigiert zu candidates[0].content.parts, dem Array der Antwort-Bestandteile
    const cJSON *part;   // [C] Deklaration der Schleifenvariable fuer das folgende Makro
    cJSON_ArrayForEach(part, parts) {   // [C] cJSON-Iterationsmakro, expandiert zu einer for-Schleife ueber alle Array-Elemente
        const cJSON *inline_data = cJSON_GetObjectItem(part, "inlineData");   // [HW] jedes part kann ein inlineData-Objekt mit Audio-Bytes enthalten
        const cJSON *mime = cJSON_GetObjectItem(inline_data, "mimeType");     // [HW] MIME-Typ-String, z. B. "audio/L16;codec=pcm;rate=24000"
        const cJSON *data = cJSON_GetObjectItem(inline_data, "data");         // [HW] Base64-kodierte Audio-Nutzdaten
        if (!cJSON_IsString(mime) || !cJSON_IsString(data)) {   // [C] Typ-Pruefungen der cJSON-Bibliothek, Kurzschlussauswertung mit || | [Logik] Feld fehlt oder hat falschen Typ -> dieses part ueberspringen
            continue;   // [C] Schleifensprung zur naechsten Iteration
        }
        if (strncmp(mime->valuestring, "audio/L16", 9) != 0) {   // [C] strncmp vergleicht nur die ersten 9 Zeichen | [HW] pruefen, ob es sich um das erwartete rohe L16-PCM-Format handelt
            ESP_LOGW(TAG, "unexpected mimeType: %s", mime->valuestring);   // [Logik] unerwarteten MIME-Typ loggen
            continue;   // [Logik] anderes Format ignorieren, naechstes part probieren
        }
        const char *rate_str = strstr(mime->valuestring, "rate=");   // [C] strstr sucht Teilstring, liefert Zeiger auf Fundstelle oder NULL | [HW] "rate=" Parameter im mimeType lokalisieren
        *sample_rate = rate_str ? (uint32_t)atoi(rate_str + 5)
                                : DEFAULT_RATE;
        // [C] ternaerer Operator; Zeigerarithmetik rate_str + 5 ueberspringt "rate=" | [HW] Sample-Rate aus dem mimeType parsen, sonst Default verwenden

        size_t pcm_bytes = 0;   // [Logik] Ausgabevariable fuer die Anzahl dekodierter Bytes
        int err = mbedtls_base64_decode(
            (unsigned char *)s_pcm, PCM_MAX, &pcm_bytes,
            (const unsigned char *)data->valuestring,
            strlen(data->valuestring));
        // [C] Casts zwischen char*/unsigned char* fuer die mbedtls-API-Signatur, &pcm_bytes uebergibt Ausgabeparameter per Adresse | [HW] dekodiert die Base64-Audio-Daten direkt in den PSRAM-PCM-Puffer, begrenzt auf PCM_MAX
        if (err != 0) {   // [Logik] mbedtls gibt 0 bei Erfolg, sonst negativen Fehlercode zurueck
            // BUFFER_TOO_SMALL hiesse: Antwort laenger als PCM_MAX.
            ESP_LOGE(TAG, "base64 decode failed (-0x%04x)", -err);   // [C] unaeres Minus dreht den negativen Fehlercode fuer die Hex-Ausgabe positiv | [Logik] Fehler mit Code loggen
            break;   // [C] Schleifenabbruch | [Logik] weiteres Suchen nach Audio-Daten abbrechen, ret bleibt ESP_FAIL
        }
        *samples = pcm_bytes / sizeof(int16_t);   // [C] Division durch sizeof(int16_t)=2 | [Logik] Byteanzahl in Sample-Anzahl umrechnen
        ret = ESP_OK;   // [Logik] Audio erfolgreich gefunden und dekodiert
        break;   // [Logik] weitere parts muessen nicht mehr geprueft werden
    }
    if (ret != ESP_OK) {   // [Logik] kein passendes inlineData-Audio in der gesamten Antwort gefunden
        ESP_LOGE(TAG, "no audio in response: %.256s", json);   // [C] Praezisions-Spezifizierer %.256s begrenzt die geloggte String-Laenge | [Logik] Rohantwort (gekuerzt) zur Fehlerdiagnose loggen
    }
    cJSON_Delete(root);   // [Logik] JSON-Baum wieder freigeben, unabhaengig von Erfolg/Misserfolg
    return ret;   // [Logik] Ergebnis an Aufrufer zurueckgeben
}

static esp_err_t synthesize_once(const char *text, const int16_t **pcm,
                                 size_t *samples, uint32_t *sample_rate)
// [C] static Funktion fuer einen einzelnen Gemini-Request-Versuch (ohne eigenes Retry, das macht der Aufrufer)
{
    if (strlen(config_gemini_key()) == 0) {   // [Logik] pruefen, ob ueberhaupt ein API-Key konfiguriert ist
        ESP_LOGE(TAG, "Gemini API key not set (Setup-Portal/menuconfig)");   // [Logik] fehlende Konfiguration loggen
        return ESP_ERR_INVALID_STATE;   // [Logik] klarer Fehlercode fuer fehlende Voraussetzung
    }

    char *body = build_request(text);   // [Logik] Gemini-JSON-Request-Body erzeugen
    if (!body) {   // [Logik] Allokation fehlgeschlagen
        return ESP_ERR_NO_MEM;   // [Logik] Speichermangel melden
    }

    esp_http_client_config_t cfg = {   // [C] Struct-Initialisierung mit designierten Initialisierern
        .url = TTS_URL,                          // [HW] Gemini generateContent-HTTPS-Endpoint
        .method = HTTP_METHOD_POST,              // [HW] POST-Request mit JSON-Body
        .crt_bundle_attach = esp_crt_bundle_attach,   // [C] Funktionszeiger als Struct-Feld | [HW] bindet das eingebettete CA-Zertifikatsbuendel fuer TLS-Zertifikatspruefung ein
        .timeout_ms = TIMEOUT_MS,                // [HW] grosszuegiges Timeout fuer die langsamere TTS-Generierung
        // Response bis >1 MB: groesserer Empfangs-Buffer = weniger
        // Read-Aufrufe (Default 512 Byte).
        .buffer_size = 8192,                     // [HW] interner Lesepuffer des HTTP-Clients, groesser als Default zur Reduktion der Read-Syscalls
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);   // [HW] legt den HTTPS-Client gemaess cfg an
    if (!client) {   // [Logik] Initialisierung fehlgeschlagen
        cJSON_free(body);   // [Logik] Body-Speicher wieder freigeben
        return ESP_FAIL;   // [Logik] generischer Fehler
    }

    esp_http_client_set_header(client, "x-goog-api-key",
                               config_gemini_key());
    // [HW] setzt den Gemini-API-Key als Authentifizierungs-Header
    esp_http_client_set_header(client, "Content-Type",
                               "application/json");
    // [HW] kennzeichnet den Body als JSON-Payload

    esp_err_t err = esp_http_client_open(client, (int)strlen(body));   // [C] Cast size_t -> int fuer Content-Length | [HW] TLS-Handshake + TCP-Verbindungsaufbau zum Gemini-Endpoint
    if (err != ESP_OK) {   // [Logik] Verbindungsaufbau (inkl. TLS) fehlgeschlagen
        ESP_LOGE(TAG, "connect failed: %s", esp_err_to_name(err));   // [Logik] Fehler mit lesbarem Namen loggen
        esp_http_client_cleanup(client);   // [HW] Client-Ressourcen freigeben
        cJSON_free(body);                  // [Logik] Body-Speicher freigeben
        return err;                        // [Logik] konkreten Fehlercode weitergeben
    }

    int64_t t0 = esp_log_timestamp();   // [Logik] Zeitstempel fuer Latenzmessung
    err = http_write_all(client, body, strlen(body));   // [HW] sendet den vollstaendigen JSON-Body ueber die (TLS-)Verbindung
    cJSON_free(body);   // [Logik] Body nach dem Senden freigeben

    int status = -1;      // [Logik] Sentinel-Wert, falls kein Status gelesen wird
    int body_len = 0;     // [Logik] Sentinel-Wert fuer noch nicht gelesene Antwortlaenge
    if (err == ESP_OK) {   // [Logik] nur bei erfolgreichem Senden weiterlesen
        if (esp_http_client_fetch_headers(client) < 0) {   // [HW] liest die HTTP-Response-Header
            ESP_LOGE(TAG, "no response headers");   // [Logik] Header-Empfang fehlgeschlagen
            err = ESP_FAIL;   // [Logik] als Fehler markieren
        } else {
            status = esp_http_client_get_status_code(client);   // [HW] HTTP-Statuscode auslesen
            body_len = http_read_body(client, s_resp, RESPONSE_MAX);   // [HW] Response-Body (JSON mit Base64-Audio) in den PSRAM-Puffer lesen, begrenzt auf RESPONSE_MAX
        }
    }
    esp_http_client_close(client);      // [HW] TLS/TCP-Verbindung schliessen
    esp_http_client_cleanup(client);    // [HW] Client-Objekt und dessen internen Speicher freigeben

    if (err != ESP_OK) {   // [Logik] Sende- oder Header-Fehler
        return err;   // [Logik] Fehlercode direkt durchreichen, der Aufrufer entscheidet ueber Retry
    }
    if (status != 200) {   // [Logik] HTTP-Fehlerstatus vom Gemini-Server
        // 429 = Free-Tier-Limit (10 Requests/Tag fuer die TTS-Preview,
        // verifiziert 2026-06-12) — eigene Kennung, damit die UI das
        // Limit vom generischen Fehler unterscheidet und der Wrapper
        // nicht sinnlos wiederholt. 5xx ist transient (die Preview
        // wirft sporadisch 500 INTERNAL), restliche 4xx permanent.
        ESP_LOGE(TAG, "HTTP %d: %.256s", status, s_resp);   // [C] %.256s begrenzt die geloggte Fehlerantwort auf 256 Zeichen | [Logik] Statuscode und Server-Fehlermeldung loggen
        if (status == 429) {   // [Logik] spezifischer Fall: Tageslimit erschoepft
            return ESP_ERR_NOT_ALLOWED;   // [Logik] eigener Fehlercode, damit die UI das Limit erkennt statt generisch zu retryen
        }
        return status >= 500 ? ESP_FAIL : ESP_ERR_INVALID_RESPONSE;
        // [C] ternaerer Operator | [Logik] 5xx gilt als transienter Serverfehler (ESP_FAIL, retry-faehig), sonstige 4xx als dauerhafter Client-/Request-Fehler
    }

    // parse_audio scheitert auch bei finishReason=OTHER ohne Audio —
    // ebenfalls transient (beobachtet 2026-06-12), der Wrapper
    // wiederholt das mit.
    err = parse_audio(s_resp, samples, sample_rate);   // [Logik] JSON-Antwort auspacken und Base64-Audio nach s_pcm dekodieren
    if (err == ESP_OK) {   // [Logik] Audio erfolgreich extrahiert
        *pcm = s_pcm;   // [C] Ausgabe-Zeiger auf den modul-globalen PCM-Puffer setzen | [Logik] Aufrufer erhaelt Zugriff auf die dekodierten Samples
        ESP_LOGI(TAG, "audio in %u ms: %u KB response -> %.1f s PCM @ %u Hz",
                 (unsigned)(esp_log_timestamp() - t0),
                 (unsigned)(body_len / 1024),
                 (float)*samples / (float)*sample_rate,
                 (unsigned)*sample_rate);
        // [Logik] Erfolgsmeldung mit Latenz, Response-Groesse und resultierender PCM-Dauer zu Diagnosezwecken
    }
    return err;   // [Logik] Ergebnis (Erfolg oder Fehler von parse_audio) zurueckgeben
}

esp_err_t tts_synthesize(const char *text, const int16_t **pcm,
                         size_t *samples, uint32_t *sample_rate)
// [C] oeffentliche Schnittstellenfunktion aus tts.h, Ausgabeparameter per Zeiger
{
    // Primaer: lokaler Piper-Server (ein Versuch, kein Backoff — bei
    // Fehler/Timeout sofort Fallback).
    if (synthesize_local(text, pcm, samples, sample_rate) == ESP_OK) {   // [Logik] erster Versuch ueber den lokalen Server, kein Retry noetig
        ESP_LOGI(TAG, "[TTS] Using local");   // [Logik] informiert, dass die lokale Quelle verwendet wurde
        return ESP_OK;   // [Logik] Erfolg, kein Gemini-Fallback noetig
    }
    ESP_LOGW(TAG, "[TTS] Fallback to Gemini");   // [Logik] lokaler Versuch fehlgeschlagen, Wechsel auf Cloud-Fallback ankuendigen

    esp_err_t err = ESP_FAIL;   // [C] Initialisierung mit Default-Fehlerwert fuer den Fall, dass die Schleife nie ESP_OK liefert
    for (int attempt = 0; attempt < HTTP_MAX_ATTEMPTS; attempt++) {   // [C] klassische Zaehlschleife ueber die maximale Anzahl Versuche
        if (attempt > 0) {   // [Logik] beim ersten Versuch keine Wartezeit, erst ab dem zweiten
            http_backoff_delay(attempt, TAG);   // [HW] exponentielles/gestuftes Backoff vor erneutem Request, um den Server nicht zu fluten
        }
        err = synthesize_once(text, pcm, samples, sample_rate);   // [Logik] einzelnen Gemini-Request-Versuch durchfuehren
        if (!http_err_is_transient(err)) {   // [C] Negation eines bool-Rueckgabewerts | [Logik] bei dauerhaftem Fehler (kein Retry sinnvoll) Schleife abbrechen
            break;   // [Logik] Abbruch bei permanentem Fehler oder Erfolg
        }
    }
    return err;   // [Logik] letztes Ergebnis (Erfolg oder finaler Fehler nach Ausschoepfen der Versuche) zurueckgeben
}
