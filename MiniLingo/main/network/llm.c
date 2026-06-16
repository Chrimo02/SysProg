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

#include "config.h"
#include "http_util.h"
#include "state.h"
#include "llm.h"

static const char *TAG = "llm";

#define LLM_URL "https://api.cerebras.ai/v1/chat/completions"

// Erzwingt knappe Antworten ohne Markdown — Sternchen klingen im TTS
// (Phase 7) schrecklich. (Uebernommen aus der Gemini-Integration.)
#define SYSTEM_INSTRUCTION \
    "Du bist ein kompakter Sprachassistent namens MiniLingo. Halte " \
    "Antworten unter 3 Saetzen und ohne Markdown-Formatierung. Du " \
    "steuerst einen Kuechen-Timer und die Lautstaerke: Wenn der Nutzer " \
    "einen Timer stellen, abbrechen, die Restzeit erfragen oder die " \
    "Lautstaerke aendern will, rufe das passende Tool auf statt zu " \
    "antworten. Wandle Minuten in Sekunden um."

// Antwortlaenge steuert primaer der System-Prompt (<3 Saetze);
// das Token-Limit ist nur die Notbremse (~10 s TTS-Audio, Spec
// Section 9.4). 300 statt 200, weil gpt-oss-120b ein Reasoning-Model
// ist und die Reasoning-Tokens GEGEN dieses Budget zaehlen (mit
// reasoning_effort=low gemessen ~11 Tokens, aber mit Luft).
#define MAX_COMPLETION_TOKENS 300
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

// Ein Tool-Schema (OpenAI-Format) an das tools-Array haengen. params ist
// ein vorgefertigtes JSON-Schema-Objekt (Besitz geht ans Tool ueber) oder
// NULL fuer parameterlose Tools.
static void add_tool(cJSON *tools, const char *name, const char *desc,
                     cJSON *params)
{
    cJSON *tool = cJSON_CreateObject();
    cJSON_AddStringToObject(tool, "type", "function");
    cJSON *func = cJSON_AddObjectToObject(tool, "function");
    cJSON_AddStringToObject(func, "name", name);
    cJSON_AddStringToObject(func, "description", desc);
    if (!params) {
        params = cJSON_CreateObject();
        cJSON_AddStringToObject(params, "type", "object");
        cJSON_AddObjectToObject(params, "properties");
    }
    cJSON_AddItemToObject(func, "parameters", params);
    cJSON_AddItemToArray(tools, tool);
}

// Schema-Objekt fuer ein Tool mit genau einem Integer-Pflichtparameter.
static cJSON *int_param_schema(const char *param, const char *param_desc)
{
    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "type", "object");
    cJSON *props = cJSON_AddObjectToObject(params, "properties");
    cJSON *p = cJSON_AddObjectToObject(props, param);
    cJSON_AddStringToObject(p, "type", "integer");
    cJSON_AddStringToObject(p, "description", param_desc);
    cJSON *required = cJSON_AddArrayToObject(params, "required");
    cJSON_AddItemToArray(required, cJSON_CreateString(param));
    return params;
}

// Tool-Definitionen (Phase 10): freie Timer-/Volume-Werte, die die
// lokalen MultiNet-Commands nicht abdecken. Die Ausfuehrung passiert in
// cloud.c (run_tool), nicht hier.
static void add_tools(cJSON *root)
{
    cJSON *tools = cJSON_AddArrayToObject(root, "tools");
    add_tool(tools, "set_timer",
             "Stellt einen Kuechen-Timer auf die angegebene Dauer.",
             int_param_schema("seconds",
                              "Dauer in Sekunden (1 bis 86400)."));
    add_tool(tools, "set_volume",
             "Setzt die Lautstaerke auf den angegebenen Prozentwert.",
             int_param_schema("percent", "Lautstaerke 0 bis 100."));
    add_tool(tools, "cancel_timer",
             "Bricht den laufenden Kuechen-Timer ab.", NULL);
    add_tool(tools, "get_time_left",
             "Gibt die Restzeit des laufenden Timers zurueck.", NULL);
}

// Request-Body (OpenAI-Chat-Format); Rueckgabe via cJSON_free().
static char *build_request(const char *user_text)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }

    cJSON_AddStringToObject(root, "model", config_llm_model());

    cJSON *messages = cJSON_AddArrayToObject(root, "messages");
    add_message(messages, "system", SYSTEM_INSTRUCTION);
    for (size_t i = 0; i < chat_history_count(); i++) {
        chat_role_t role;
        const char *text = chat_history_get(i, &role);
        add_message(messages,
                    role == CHAT_ROLE_MODEL ? "assistant" : "user", text);
    }
    add_message(messages, "user", user_text);

    add_tools(root);

    cJSON_AddNumberToObject(root, "max_completion_tokens",
                            MAX_COMPLETION_TOKENS);
    cJSON_AddNumberToObject(root, "temperature", TEMPERATURE);
    // gpt-oss-120b: Reasoning kurz halten (Latenz + Token-Budget;
    // verifiziert per curl 2026-06-12: ~11 Reasoning-Tokens, Antwort
    // in 24 ms Serverzeit). zai-glm-4.7 akzeptiert den Parameter,
    // ignoriert ihn aber weitgehend.
    cJSON_AddStringToObject(root, "reasoning_effort", "low");

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return body;
}

static esp_err_t parse_reply(const char *json, llm_result_t *out)
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

    // Tool-Call hat Vorrang: bei einem set_timer/... ist content leer.
    const cJSON *tool_calls = cJSON_GetObjectItem(msg, "tool_calls");
    const cJSON *call = cJSON_GetArrayItem(tool_calls, 0);
    if (call) {
        const cJSON *func = cJSON_GetObjectItem(call, "function");
        const cJSON *name = cJSON_GetObjectItem(func, "name");
        const cJSON *args = cJSON_GetObjectItem(func, "arguments");
        if (cJSON_IsString(name)) {
            out->type = LLM_RESULT_TOOL_CALL;
            snprintf(out->tool_name, sizeof(out->tool_name), "%s",
                     name->valuestring);
            // arguments ist im OpenAI-Format ein JSON-String (kann bei
            // parameterlosen Tools fehlen) — als Text uebernehmen,
            // run_tool parst ihn separat.
            snprintf(out->tool_args, sizeof(out->tool_args), "%s",
                     cJSON_IsString(args) ? args->valuestring : "{}");
            ret = ESP_OK;
        }
    }

    if (ret != ESP_OK) {
        const cJSON *content = cJSON_GetObjectItem(msg, "content");
        if (cJSON_IsString(content)) {
            out->type = LLM_RESULT_TEXT;
            snprintf(out->text, sizeof(out->text), "%s",
                     content->valuestring);
            size_t len = strlen(out->text);
            while (len > 0 && isspace((unsigned char)out->text[len - 1])) {
                out->text[--len] = '\0';
            }
            if (len > 0) {
                ret = ESP_OK;
            }
        }
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "no text/tool in response: %.256s", json);
    }
    cJSON_Delete(root);
    return ret;
}

static esp_err_t generate_once(const char *user_text, llm_result_t *out)
{
    if (strlen(config_cerebras_key()) == 0) {
        ESP_LOGE(TAG, "Cerebras API key not set (Setup-Portal/menuconfig)");
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

    char auth[160];
    snprintf(auth, sizeof(auth), "Bearer %s", config_cerebras_key());
    esp_http_client_set_header(client, "Authorization", auth);
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

    err = parse_reply(resp, out);
    if (err == ESP_OK) {
        unsigned ms = (unsigned)(esp_log_timestamp() - t0);
        if (out->type == LLM_RESULT_TOOL_CALL) {
            ESP_LOGI(TAG, "tool call in %u ms: %s(%s)", ms,
                     out->tool_name, out->tool_args);
        } else {
            ESP_LOGI(TAG, "reply in %u ms: \"%s\"", ms, out->text);
        }
    }
    return err;
}

esp_err_t llm_generate(const char *user_text, llm_result_t *out)
{
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < HTTP_MAX_ATTEMPTS; attempt++) {
        if (attempt > 0) {
            http_backoff_delay(attempt, TAG);
        }
        err = generate_once(user_text, out);
        if (!http_err_is_transient(err)) {
            break;
        }
    }
    return err;
}
