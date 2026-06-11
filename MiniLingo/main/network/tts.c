// Gemini TTS (Spec Section 9.5, Phase 7): responseModalities=AUDIO,
// Response-JSON in PSRAM puffern, inlineData.data per
// mbedtls_base64_decode in den PCM-Buffer dekodieren. Die Sample-Rate
// kommt aus dem mimeType ("audio/L16;codec=pcm;rate=24000").
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "mbedtls/base64.h"
#include "sdkconfig.h"

#include "http_util.h"
#include "tts.h"

static const char *TAG = "tts";

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

static char *s_resp;     // Response-JSON (PSRAM)
static int16_t *s_pcm;   // dekodiertes PCM (PSRAM)

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
    cJSON_AddStringToObject(voice, "voiceName", CONFIG_GEMINI_VOICE_NAME);

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

esp_err_t tts_synthesize(const char *text, const int16_t **pcm,
                         size_t *samples, uint32_t *sample_rate)
{
    if (strlen(CONFIG_GEMINI_API_KEY) == 0) {
        ESP_LOGE(TAG, "CONFIG_GEMINI_API_KEY not set (menuconfig)");
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
                               CONFIG_GEMINI_API_KEY);
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
        // Limit vom generischen Fehler unterscheidet. Retry/Backoff
        // kommt in Phase 8.
        ESP_LOGE(TAG, "HTTP %d: %.256s", status, s_resp);
        return status == 429 ? ESP_ERR_NOT_ALLOWED : ESP_FAIL;
    }

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
