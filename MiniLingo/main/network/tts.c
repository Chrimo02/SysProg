// TTS: primaer ein lokaler Piper-Server im LAN (HTTP POST -> rohe
// WAV-Bytes, 44-Byte-Header ueberspringen, PCM direkt in die I2S-Pipeline),
// Gemini nur noch als Fallback wenn der lokale Server fehlschlaegt oder das
// 3-s-Timeout reisst.
//
// Gemini-Fallback (Spec Section 9.5, Phase 7): responseModalities=AUDIO,
// Response-JSON in PSRAM puffern, inlineData.data per
// mbedtls_base64_decode in den PCM-Buffer dekodieren. Die Sample-Rate
// kommt aus dem mimeType ("audio/L16;codec=pcm;rate=24000").
#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "mbedtls/base64.h"
#include "sdkconfig.h"

#include "config.h"
#include "http_util.h"
#include "tts.h"

static const char *TAG = "tts";

// Lokaler Piper-TTS-Server (primaere Quelle, kein TLS): liefert rohe
// WAV-Bytes (PCM, mono, 16-bit). Bei Fehler/Timeout faellt tts_synthesize
// auf Gemini zurueck.
#define TTS_LOCAL_URL        "http://192.168.3.143:5050/tts"
#define TTS_LOCAL_VOICE      "jarvis"   // Fallback-Default, per-character ueberschreibbar
#define TTS_LOCAL_TIMEOUT_MS 3000       // danach Fallback auf Gemini
#define WAV_HEADER_BYTES     44         // kanonischer RIFF/WAVE-Header
#define LANG_DETECT_MAX      512        // genug Text fuer die Stopwort-Heuristik

// Preview-Modellname als Konstante (Spec Section 13: kann sich aendern).
#define TTS_MODEL "gemini-2.5-flash-preview-tts"
#define TTS_URL \
    "https://generativelanguage.googleapis.com/v1beta/models/" \
    TTS_MODEL ":generateContent"

// ~10 s Audio (maxOutputTokens=200) sind 480 KB PCM = 640 KB Base64 +
// JSON-Overhead (Spec Section 13). Etwas Luft fuer laengere Antworten.
#define RESPONSE_MAX (1536 * 1024)
#define PCM_MAX      (1024 * 1024)
#define DEFAULT_RATE 24000

// TTS-Generierung braucht laenger als der Text-Endpoint.
#define TIMEOUT_MS 30000

static char *s_resp;     // Response-Puffer (PSRAM): Gemini-JSON oder lokale WAV-Bytes
static int16_t *s_pcm;   // dekodiertes Gemini-PCM (PSRAM)
static const char *s_voice;        // NULL = config_gemini_voice() als Fallback
static const char *s_local_voice;  // NULL = TTS_LOCAL_VOICE als Fallback

esp_err_t tts_init(void)
{
    s_resp = heap_caps_malloc(RESPONSE_MAX, MALLOC_CAP_SPIRAM);
    s_pcm = heap_caps_malloc(PCM_MAX, MALLOC_CAP_SPIRAM);
    if (!s_resp || !s_pcm) {
        ESP_LOGE(TAG, "PSRAM buffer alloc failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void tts_set_voice(const char *voice)
{
    s_voice = voice;
}

void tts_set_local_voice(const char *voice)
{
    s_local_voice = voice;
}

// Sehr einfache Spracherkennung (kein ML, kein Netz): Stopwort-Score
// Deutsch vs. Englisch plus Umlaute/sz. Reicht, um die richtige
// Piper-Stimme zu waehlen — die lokalen Voices sind einsprachige
// Modelle, englischer Text mit der (oft eingestellten) Deutsch-Stimme
// (oder umgekehrt) klingt deshalb sehr falsch ausgesprochen.
static bool text_is_german(const char *text)
{
    static const char *de_words[] = {
        " der ", " die ", " das ", " und ", " ist ", " nicht ", " ich ",
        " du ", " wir ", " mit ", " fuer ", " ein ", " eine ", " auf ",
        " sind ", " war ", " hat ", " kann ", " auch ", " sehr ",
        " schon ", " noch ", " mal ", " gerne ", " bitte ", " ihnen ",
    };
    static const char *en_words[] = {
        " the ", " is ", " and ", " you ", " are ", " what ", " this ",
        " that ", " with ", " for ", " have ", " can ", " will ",
        " your ", " it's ", " of ", " to ", " a ", " i'm ", " sir ",
    };

    char buf[LANG_DETECT_MAX + 2];
    size_t len = strlen(text);
    if (len > sizeof(buf) - 3) {
        len = sizeof(buf) - 3;
    }
    buf[0] = ' ';
    for (size_t i = 0; i < len; i++) {
        buf[i + 1] = (char)tolower((unsigned char)text[i]);
    }
    buf[len + 1] = ' ';
    buf[len + 2] = '\0';

    int de_score = 0, en_score = 0;
    for (size_t i = 0; i < sizeof(de_words) / sizeof(de_words[0]); i++) {
        if (strstr(buf, de_words[i])) {
            de_score++;
        }
    }
    for (size_t i = 0; i < sizeof(en_words) / sizeof(en_words[0]); i++) {
        if (strstr(buf, en_words[i])) {
            en_score++;
        }
    }
    // UTF-8-Praefixbyte von ae/oe/ue/sz (0xC3 0x84-0xBF) ist ein starkes
    // Deutsch-Signal und faengt auch kurze Antworten ohne Stopwoerter ab.
    for (size_t i = 0; i < len; i++) {
        if ((unsigned char)text[i] == 0xC3) {
            de_score += 2;
        }
    }
    return de_score >= en_score;
}

// Request-Body fuer den lokalen Piper-Server: {"text":...,"voice":...}.
// Rueckgabe via cJSON_free().
static char *build_local_request(const char *text, const char *voice)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    cJSON_AddStringToObject(root, "text", text);
    cJSON_AddStringToObject(root, "voice", voice);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return body;
}

// HTTP POST an den lokalen Piper-Server. Antwort sind rohe WAV-Bytes; der
// 44-Byte-Header wird uebersprungen, die Sample-Rate aus dem Header gelesen
// und *pcm zeigt auf die rohen PCM-Frames in s_resp (gueltig bis zum
// naechsten Aufruf, genau ein Aufrufer wartet die Wiedergabe ab).
static esp_err_t synthesize_local(const char *text, const int16_t **pcm,
                                  size_t *samples, uint32_t *sample_rate)
{
    // Die lokalen Voices sind einsprachig: bei Deutsch immer "de" senden,
    // unabhaengig von der per-character Stimme — sonst spricht z. B.
    // "jarvis" (Englisch-Modell) deutschen Text in falscher Phonetik.
    const char *voice = text_is_german(text)
                             ? "de"
                             : (s_local_voice ? s_local_voice : TTS_LOCAL_VOICE);
    char *body = build_local_request(text, voice);
    if (!body) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t cfg = {
        .url = TTS_LOCAL_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = TTS_LOCAL_TIMEOUT_MS,
        .buffer_size = 8192,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        cJSON_free(body);
        return ESP_FAIL;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");

    esp_err_t err = esp_http_client_open(client, (int)strlen(body));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "local connect failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        cJSON_free(body);
        return err;
    }

    int64_t t0 = esp_log_timestamp();
    err = http_write_all(client, body, strlen(body));
    cJSON_free(body);

    int status = -1;
    int body_len = 0;
    if (err == ESP_OK) {
        if (esp_http_client_fetch_headers(client) < 0) {
            err = ESP_FAIL;
        } else {
            status = esp_http_client_get_status_code(client);
            body_len = http_read_body(client, s_resp, RESPONSE_MAX);
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "local request failed: %s", esp_err_to_name(err));
        return err;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "local HTTP %d", status);
        return ESP_FAIL;
    }
    if (body_len <= WAV_HEADER_BYTES) {
        ESP_LOGW(TAG, "local response too short (%d B)", body_len);
        return ESP_FAIL;
    }
    if (memcmp(s_resp, "RIFF", 4) != 0 || memcmp(s_resp + 8, "WAVE", 4) != 0) {
        ESP_LOGW(TAG, "local response is not a WAV");
        return ESP_FAIL;
    }

    // Sample-Rate aus dem WAV-Header (Offset 24, uint32 LE) — Piper-Voices
    // laufen z. B. mit 22050 Hz; nicht hart kodieren.
    const uint8_t *hdr = (const uint8_t *)s_resp;
    *sample_rate = (uint32_t)hdr[24] | ((uint32_t)hdr[25] << 8) |
                   ((uint32_t)hdr[26] << 16) | ((uint32_t)hdr[27] << 24);
    // Offset 44 ist gerade -> 2-Byte-Ausrichtung bleibt erhalten, kein Kopieren.
    *pcm = (const int16_t *)(s_resp + WAV_HEADER_BYTES);
    *samples = (size_t)(body_len - WAV_HEADER_BYTES) / sizeof(int16_t);
    ESP_LOGI(TAG, "local audio (voice=%s) in %u ms: %.1f s PCM @ %u Hz",
             voice, (unsigned)(esp_log_timestamp() - t0),
             (float)*samples / (float)*sample_rate, (unsigned)*sample_rate);
    return ESP_OK;
}

// Request-Body nach Spec Section 9.5; Rueckgabe via cJSON_free().
static char *build_request(const char *text)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }

    cJSON *turn = cJSON_CreateObject();
    cJSON *part = cJSON_CreateObject();
    cJSON_AddStringToObject(part, "text", text);
    cJSON_AddItemToArray(cJSON_AddArrayToObject(turn, "parts"), part);
    cJSON_AddItemToArray(cJSON_AddArrayToObject(root, "contents"), turn);

    cJSON *gen = cJSON_AddObjectToObject(root, "generationConfig");
    cJSON *mod = cJSON_AddArrayToObject(gen, "responseModalities");
    cJSON_AddItemToArray(mod, cJSON_CreateString("AUDIO"));
    cJSON *voice = cJSON_AddObjectToObject(
        cJSON_AddObjectToObject(
            cJSON_AddObjectToObject(gen, "speechConfig"), "voiceConfig"),
        "prebuiltVoiceConfig");
    cJSON_AddStringToObject(voice, "voiceName",
                            s_voice ? s_voice : config_gemini_voice());

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return body;
}

// Extrahiert inlineData aus dem Response-JSON und dekodiert nach s_pcm.
static esp_err_t parse_audio(const char *json, size_t *samples,
                             uint32_t *sample_rate)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGE(TAG, "response is not valid JSON");
        return ESP_FAIL;
    }

    esp_err_t ret = ESP_FAIL;
    const cJSON *cand = cJSON_GetArrayItem(
        cJSON_GetObjectItem(root, "candidates"), 0);
    const cJSON *parts = cJSON_GetObjectItem(
        cJSON_GetObjectItem(cand, "content"), "parts");
    const cJSON *part;
    cJSON_ArrayForEach(part, parts) {
        const cJSON *inline_data = cJSON_GetObjectItem(part, "inlineData");
        const cJSON *mime = cJSON_GetObjectItem(inline_data, "mimeType");
        const cJSON *data = cJSON_GetObjectItem(inline_data, "data");
        if (!cJSON_IsString(mime) || !cJSON_IsString(data)) {
            continue;
        }
        if (strncmp(mime->valuestring, "audio/L16", 9) != 0) {
            ESP_LOGW(TAG, "unexpected mimeType: %s", mime->valuestring);
            continue;
        }
        const char *rate_str = strstr(mime->valuestring, "rate=");
        *sample_rate = rate_str ? (uint32_t)atoi(rate_str + 5)
                                : DEFAULT_RATE;

        size_t pcm_bytes = 0;
        int err = mbedtls_base64_decode(
            (unsigned char *)s_pcm, PCM_MAX, &pcm_bytes,
            (const unsigned char *)data->valuestring,
            strlen(data->valuestring));
        if (err != 0) {
            // BUFFER_TOO_SMALL hiesse: Antwort laenger als PCM_MAX.
            ESP_LOGE(TAG, "base64 decode failed (-0x%04x)", -err);
            break;
        }
        *samples = pcm_bytes / sizeof(int16_t);
        ret = ESP_OK;
        break;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "no audio in response: %.256s", json);
    }
    cJSON_Delete(root);
    return ret;
}

static esp_err_t synthesize_once(const char *text, const int16_t **pcm,
                                 size_t *samples, uint32_t *sample_rate)
{
    if (strlen(config_gemini_key()) == 0) {
        ESP_LOGE(TAG, "Gemini API key not set (Setup-Portal/menuconfig)");
        return ESP_ERR_INVALID_STATE;
    }

    char *body = build_request(text);
    if (!body) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t cfg = {
        .url = TTS_URL,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = TIMEOUT_MS,
        // Response bis >1 MB: groesserer Empfangs-Buffer = weniger
        // Read-Aufrufe (Default 512 Byte).
        .buffer_size = 8192,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        cJSON_free(body);
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "x-goog-api-key",
                               config_gemini_key());
    esp_http_client_set_header(client, "Content-Type",
                               "application/json");

    esp_err_t err = esp_http_client_open(client, (int)strlen(body));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "connect failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        cJSON_free(body);
        return err;
    }

    int64_t t0 = esp_log_timestamp();
    err = http_write_all(client, body, strlen(body));
    cJSON_free(body);

    int status = -1;
    int body_len = 0;
    if (err == ESP_OK) {
        if (esp_http_client_fetch_headers(client) < 0) {
            ESP_LOGE(TAG, "no response headers");
            err = ESP_FAIL;
        } else {
            status = esp_http_client_get_status_code(client);
            body_len = http_read_body(client, s_resp, RESPONSE_MAX);
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        return err;
    }
    if (status != 200) {
        // 429 = Free-Tier-Limit (10 Requests/Tag fuer die TTS-Preview,
        // verifiziert 2026-06-12) — eigene Kennung, damit die UI das
        // Limit vom generischen Fehler unterscheidet und der Wrapper
        // nicht sinnlos wiederholt. 5xx ist transient (die Preview
        // wirft sporadisch 500 INTERNAL), restliche 4xx permanent.
        ESP_LOGE(TAG, "HTTP %d: %.256s", status, s_resp);
        if (status == 429) {
            return ESP_ERR_NOT_ALLOWED;
        }
        return status >= 500 ? ESP_FAIL : ESP_ERR_INVALID_RESPONSE;
    }

    // parse_audio scheitert auch bei finishReason=OTHER ohne Audio —
    // ebenfalls transient (beobachtet 2026-06-12), der Wrapper
    // wiederholt das mit.
    err = parse_audio(s_resp, samples, sample_rate);
    if (err == ESP_OK) {
        *pcm = s_pcm;
        ESP_LOGI(TAG, "audio in %u ms: %u KB response -> %.1f s PCM @ %u Hz",
                 (unsigned)(esp_log_timestamp() - t0),
                 (unsigned)(body_len / 1024),
                 (float)*samples / (float)*sample_rate,
                 (unsigned)*sample_rate);
    }
    return err;
}

esp_err_t tts_synthesize(const char *text, const int16_t **pcm,
                         size_t *samples, uint32_t *sample_rate)
{
    // Primaer: lokaler Piper-Server (ein Versuch, kein Backoff — bei
    // Fehler/Timeout sofort Fallback).
    if (synthesize_local(text, pcm, samples, sample_rate) == ESP_OK) {
        ESP_LOGI(TAG, "[TTS] Using local");
        return ESP_OK;
    }
    ESP_LOGW(TAG, "[TTS] Fallback to Gemini");

    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < HTTP_MAX_ATTEMPTS; attempt++) {
        if (attempt > 0) {
            http_backoff_delay(attempt, TAG);
        }
        err = synthesize_once(text, pcm, samples, sample_rate);
        if (!http_err_is_transient(err)) {
            break;
        }
    }
    return err;
}
