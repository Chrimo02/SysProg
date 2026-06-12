// Gemini 2.5 Flash (Spec Section 9.4, Phase 6): JSON-Request per cJSON
// (systemInstruction + History-Ringpuffer + generationConfig), Antwort
// aus candidates[0].content.parts[].text. TLS-Zertifikate aus dem
// esp_crt_bundle.
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
#include "gemini.h"

static const char *TAG = "gemini";

// Modellname als Konstante fuehren — Gemini-Models aendern sich
// (Spec Section 13).
#define GEMINI_MODEL "gemini-2.5-flash"
#define GEMINI_URL \
    "https://generativelanguage.googleapis.com/v1beta/models/" \
    GEMINI_MODEL ":generateContent"

// Erzwingt knappe Antworten ohne Markdown — Sternchen klingen im TTS
// (Phase 7) schrecklich.
#define SYSTEM_INSTRUCTION \
    "Du bist ein kompakter Sprachassistent namens MiniLingo. Halte " \
    "Antworten unter 3 Saetzen und ohne Markdown-Formatierung."

// Hard-Limit ~10 s TTS-Audio (Spec Section 9.4).
#define MAX_OUTPUT_TOKENS 200
#define TEMPERATURE       0.7

// Antwort-JSON bleibt klein: Thinking aus, 200 Output-Tokens.
#define RESPONSE_MAX 4096
#define TIMEOUT_MS   20000

static void add_content(cJSON *contents, const char *role,
                        const char *text)
{
    cJSON *turn = cJSON_CreateObject();
    cJSON_AddStringToObject(turn, "role", role);
    cJSON *part = cJSON_CreateObject();
    cJSON_AddStringToObject(part, "text", text);
    cJSON_AddItemToArray(cJSON_AddArrayToObject(turn, "parts"), part);
    cJSON_AddItemToArray(contents, turn);
}

// Request-Body nach Spec Section 9.4; Rueckgabe via cJSON_free().
static char *build_request(const char *user_text)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }

    cJSON *sys = cJSON_AddObjectToObject(root, "systemInstruction");
    cJSON *sys_part = cJSON_CreateObject();
    cJSON_AddStringToObject(sys_part, "text", SYSTEM_INSTRUCTION);
    cJSON_AddItemToArray(cJSON_AddArrayToObject(sys, "parts"), sys_part);

    cJSON *contents = cJSON_AddArrayToObject(root, "contents");
    for (size_t i = 0; i < chat_history_count(); i++) {
        chat_role_t role;
        const char *text = chat_history_get(i, &role);
        add_content(contents,
                    role == CHAT_ROLE_MODEL ? "model" : "user", text);
    }
    add_content(contents, "user", user_text);

    cJSON *gen = cJSON_AddObjectToObject(root, "generationConfig");
    cJSON_AddNumberToObject(gen, "maxOutputTokens", MAX_OUTPUT_TOKENS);
    cJSON_AddNumberToObject(gen, "temperature", TEMPERATURE);
    // 2.5 Flash "denkt" per Default (dynamic thinking) — das kostet
    // Latenz und wuerde das 200-Token-Budget auffressen (Antwort kaeme
    // leer mit finishReason=MAX_TOKENS zurueck). Abschalten.
    cJSON *think = cJSON_AddObjectToObject(gen, "thinkingConfig");
    cJSON_AddNumberToObject(think, "thinkingBudget", 0);

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
    // Glied (z.B. keine parts bei finishReason=SAFETY), wird die
    // Schleife einfach nicht betreten.
    esp_err_t ret = ESP_FAIL;
    const cJSON *cand = cJSON_GetArrayItem(
        cJSON_GetObjectItem(root, "candidates"), 0);
    const cJSON *parts = cJSON_GetObjectItem(
        cJSON_GetObjectItem(cand, "content"), "parts");
    const cJSON *part;
    cJSON_ArrayForEach(part, parts) {
        const cJSON *text = cJSON_GetObjectItem(part, "text");
        if (!cJSON_IsString(text)) {
            continue;
        }
        snprintf(reply, reply_len, "%s", text->valuestring);
        // Gemini haengt typisch ein '\n' an.
        size_t len = strlen(reply);
        while (len > 0 && isspace((unsigned char)reply[len - 1])) {
            reply[--len] = '\0';
        }
        if (len > 0) {
            ret = ESP_OK;
            break;
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
    if (strlen(CONFIG_GEMINI_API_KEY) == 0) {
        ESP_LOGE(TAG, "CONFIG_GEMINI_API_KEY not set (menuconfig)");
        return ESP_ERR_INVALID_STATE;
    }

    char *body = build_request(user_text);
    if (!body) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t cfg = {
        .url = GEMINI_URL,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = TIMEOUT_MS,
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
        // andere 4xx (z.B. 400, kaputter Request) sind permanent.
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

esp_err_t gemini_generate(const char *user_text, char *reply,
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
