// LLM-Antworten via Cerebras (seit 2026-06-12, ersetzt Gemini 2.5
// Flash aus Spec Section 9.4): OpenAI-kompatible Chat-Completions,
// Free Tier 1 Mio. Token/Tag statt 250 Requests/Tag bei Gemini.
// JSON-Request per cJSON (System-Prompt + History-Ringpuffer +
// neuer User-Turn), Antwort aus choices[0].message.content.
// TLS-Zertifikate aus dem esp_crt_bundle.
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "http_util.h"
#include "state.h"
#include "llm.h"

static const char *TAG = "llm";

#define LLM_URL "https://api.cerebras.ai/v1/chat/completions"

// Erzwingt knappe Antworten ohne Markdown — Sternchen klingen im TTS
// (Phase 7) schrecklich. (Uebernommen aus der Gemini-Integration.)
#define SYSTEM_INSTRUCTION \
    "Du bist ein kompakter Sprachassistent namens MiniLingo. Halte " \
    "Antworten unter 3 Saetzen und ohne Markdown-Formatierung."

// Hard-Limit ~10 s TTS-Audio (Spec Section 9.4).
#define MAX_COMPLETION_TOKENS 200
#define TEMPERATURE           0.7

// Antwort-JSON bleibt klein (200 Output-Tokens + Usage-Metadaten).
#define RESPONSE_MAX 4096
#define TIMEOUT_MS   20000

static void add_message(cJSON *messages, const char *role,
                        const char *content)
{
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", role);
    cJSON_AddStringToObject(msg, "content", content);
    cJSON_AddItemToArray(messages, msg);
}

// Request-Body (OpenAI-Chat-Format); Rueckgabe via cJSON_free().
static char *build_request(const char *user_text)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }

    cJSON_AddStringToObject(root, "model", CONFIG_LLM_MODEL);

    cJSON *messages = cJSON_AddArrayToObject(root, "messages");
    add_message(messages, "system", SYSTEM_INSTRUCTION);
    for (size_t i = 0; i < chat_history_count(); i++) {
        chat_role_t role;
        const char *text = chat_history_get(i, &role);
        add_message(messages,
                    role == CHAT_ROLE_MODEL ? "assistant" : "user", text);
    }
    add_message(messages, "user", user_text);

    cJSON_AddNumberToObject(root, "max_completion_tokens",
                            MAX_COMPLETION_TOKENS);
    cJSON_AddNumberToObject(root, "temperature", TEMPERATURE);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return body;
}

static esp_err_t parse_reply(const char *json, char *reply,
                             size_t reply_len)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGE(TAG, "response is not valid JSON");
        return ESP_FAIL;
    }

    // cJSON_GetObjectItem/GetArrayItem sind NULL-tolerant — fehlt ein
    // Glied, faellt das Ergebnis einfach auf NULL durch.
    esp_err_t ret = ESP_FAIL;
    const cJSON *msg = cJSON_GetObjectItem(
        cJSON_GetArrayItem(cJSON_GetObjectItem(root, "choices"), 0),
        "message");
    const cJSON *content = cJSON_GetObjectItem(msg, "content");
    if (cJSON_IsString(content)) {
        snprintf(reply, reply_len, "%s", content->valuestring);
        size_t len = strlen(reply);
        while (len > 0 && isspace((unsigned char)reply[len - 1])) {
            reply[--len] = '\0';
        }
        if (len > 0) {
            ret = ESP_OK;
        }
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "no text in response: %.256s", json);
    }
    cJSON_Delete(root);
    return ret;
}

static esp_err_t generate_once(const char *user_text, char *reply,
                               size_t reply_len)
{
    if (strlen(CONFIG_CEREBRAS_API_KEY) == 0) {
        ESP_LOGE(TAG, "CONFIG_CEREBRAS_API_KEY not set (menuconfig)");
        return ESP_ERR_INVALID_STATE;
    }

    char *body = build_request(user_text);
    if (!body) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t cfg = {
        .url = LLM_URL,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = TIMEOUT_MS,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        cJSON_free(body);
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Authorization",
                               "Bearer " CONFIG_CEREBRAS_API_KEY);
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

    char resp[RESPONSE_MAX];
    int status = -1;
    if (err == ESP_OK) {
        if (esp_http_client_fetch_headers(client) < 0) {
            ESP_LOGE(TAG, "no response headers");
            err = ESP_FAIL;
        } else {
            status = esp_http_client_get_status_code(client);
            http_read_body(client, resp, sizeof(resp));
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        return err;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "HTTP %d: %.256s", status, resp);
        // 429 (Rate-Limit) und 5xx sind transient -> Retry im Wrapper;
        // andere 4xx (z.B. 401, ungueltiger Key) sind permanent.
        return (status == 429 || status >= 500) ? ESP_FAIL
                                                : ESP_ERR_INVALID_RESPONSE;
    }

    err = parse_reply(resp, reply, reply_len);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "reply in %u ms: \"%s\"",
                 (unsigned)(esp_log_timestamp() - t0), reply);
    }
    return err;
}

esp_err_t llm_generate(const char *user_text, char *reply,
                       size_t reply_len)
{
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < HTTP_MAX_ATTEMPTS; attempt++) {
        if (attempt > 0) {
            http_backoff_delay(attempt, TAG);
        }
        err = generate_once(user_text, reply, reply_len);
        if (!http_err_is_transient(err)) {
            break;
        }
    }
    return err;
}
