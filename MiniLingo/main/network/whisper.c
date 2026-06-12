// Groq Whisper STT (Spec Section 9.3, Phase 5): Multipart-Upload des
// Recording-Buffers per esp_http_client_open/_write, Response mit
// cJSON parsen. TLS-Zertifikate kommen aus dem esp_crt_bundle.
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "http_util.h"
#include "whisper.h"

static const char *TAG = "whisper";

#define WHISPER_URL   "https://api.groq.com/openai/v1/audio/transcriptions"
// whisper-large-v3-turbo: schneller als large-v3 bei praktisch
// identischer Qualitaet (Spec Section 9.3).
#define WHISPER_MODEL "whisper-large-v3-turbo"
#define BOUNDARY      "----esp32minilingo"

// Whisper-Antworten sind kurz (max 8 s Sprache); 2 KB reichen fuer
// das JSON inkl. Transkript locker.
#define RESPONSE_MAX  2048

#define TIMEOUT_MS    20000

static const char *PART_FMT =
    "--" BOUNDARY "\r\n"
    "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
    WHISPER_MODEL "\r\n"
    "--" BOUNDARY "\r\n"
    "Content-Disposition: form-data; name=\"response_format\"\r\n\r\n"
    "json\r\n"
    "--" BOUNDARY "\r\n"
    "Content-Disposition: form-data; name=\"language\"\r\n\r\n"
    "%s\r\n"
    "--" BOUNDARY "\r\n"
    "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n"
    "Content-Type: audio/wav\r\n\r\n";

static const char *EPILOGUE = "\r\n--" BOUNDARY "--\r\n";

static esp_err_t parse_transcript(const char *json, char *text,
                                  size_t text_len)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGE(TAG, "response is not valid JSON");
        return ESP_FAIL;
    }

    esp_err_t ret = ESP_FAIL;
    const cJSON *field = cJSON_GetObjectItem(root, "text");
    if (cJSON_IsString(field)) {
        // Whisper liefert oft ein fuehrendes Leerzeichen mit.
        const char *src = field->valuestring;
        while (*src && isspace((unsigned char)*src)) {
            src++;
        }
        if (*src) {
            snprintf(text, text_len, "%s", src);
            ret = ESP_OK;
        } else {
            ESP_LOGW(TAG, "empty transcript");
        }
    } else {
        ESP_LOGE(TAG, "no \"text\" field in response: %.256s", json);
    }
    cJSON_Delete(root);
    return ret;
}

static esp_err_t transcribe_once(const uint8_t *wav, size_t wav_bytes,
                                 char *text, size_t text_len)
{
    if (strlen(CONFIG_GROQ_API_KEY) == 0) {
        ESP_LOGE(TAG, "CONFIG_GROQ_API_KEY not set (menuconfig)");
        return ESP_ERR_INVALID_STATE;
    }

    char prologue[512];
    int prologue_len = snprintf(prologue, sizeof(prologue), PART_FMT,
                                CONFIG_WHISPER_LANGUAGE);
    if (prologue_len <= 0 || prologue_len >= (int)sizeof(prologue)) {
        return ESP_FAIL;
    }

    esp_http_client_config_t cfg = {
        .url = WHISPER_URL,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = TIMEOUT_MS,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Authorization",
                               "Bearer " CONFIG_GROQ_API_KEY);
    esp_http_client_set_header(client, "Content-Type",
                               "multipart/form-data; boundary=" BOUNDARY);

    size_t content_len =
        (size_t)prologue_len + wav_bytes + strlen(EPILOGUE);
    esp_err_t err = esp_http_client_open(client, (int)content_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "connect failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    int64_t t0 = esp_log_timestamp();
    err = http_write_all(client, prologue, (size_t)prologue_len);
    if (err == ESP_OK) {
        err = http_write_all(client, (const char *)wav, wav_bytes);
    }
    if (err == ESP_OK) {
        err = http_write_all(client, EPILOGUE, strlen(EPILOGUE));
    }

    char body[RESPONSE_MAX];
    int status = -1;
    if (err == ESP_OK) {
        if (esp_http_client_fetch_headers(client) < 0) {
            ESP_LOGE(TAG, "no response headers");
            err = ESP_FAIL;
        } else {
            status = esp_http_client_get_status_code(client);
            http_read_body(client, body, sizeof(body));
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        return err;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "HTTP %d: %.256s", status, body);
        // 429 (Rate-Limit) und 5xx sind transient -> Retry im Wrapper;
        // andere 4xx (z.B. 401, ungueltiger Key) sind permanent.
        return (status == 429 || status >= 500) ? ESP_FAIL
                                                : ESP_ERR_INVALID_RESPONSE;
    }

    err = parse_transcript(body, text, text_len);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "transcribed %u KB in %u ms: \"%s\"",
                 (unsigned)(wav_bytes / 1024),
                 (unsigned)(esp_log_timestamp() - t0), text);
    }
    return err;
}

esp_err_t whisper_transcribe(const uint8_t *wav, size_t wav_bytes,
                             char *text, size_t text_len)
{
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < HTTP_MAX_ATTEMPTS; attempt++) {
        if (attempt > 0) {
            http_backoff_delay(attempt, TAG);
        }
        err = transcribe_once(wav, wav_bytes, text, text_len);
        if (!http_err_is_transient(err)) {
            break;
        }
    }
    return err;
}
