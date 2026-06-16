// WLAN-Setup-Portal (SoftAP + HTTP). Trigger: Tap aufs WiFi-Icon
// (ui_status_bar) -> provisioning_start(). Das Gerät bleibt im APSTA-
// Modus (die bestehende STA-Verbindung läuft weiter), spannt zusätzlich
// den AP "MiniLingo-Setup" auf und serviert ein Formular. POST /save
// schreibt die Werte ins NVS (config.c) und rebootet.
//
// Sicherheit: WPA2-geschützter AP, Werte im Klartext im NVS — für dieses
// Hobby-Setup akzeptabel (Kconfig war auch Klartext). Kein TLS auf dem
// lokalen Portal.
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"

#include "config.h"
#include "provisioning.h"

static const char *TAG = "provision";

#define AP_SSID     "MiniLingo-Setup"
#define AP_PASS     "minilingo"  // >= 8 Zeichen für WPA2
#define AP_MAX_CONN 2

static atomic_bool s_active;
static httpd_handle_t s_server;
static esp_netif_t *s_ap_netif;

// --- URL-Decoding (application/x-www-form-urlencoded) -----------------

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(char *s)
{
    char *d = s;
    while (*s) {
        if (*s == '%' && hexval(s[1]) >= 0 && hexval(s[2]) >= 0) {
            *d++ = (char)(hexval(s[1]) * 16 + hexval(s[2]));
            s += 3;
        } else if (*s == '+') {
            *d++ = ' ';
            s++;
        } else {
            *d++ = *s++;
        }
    }
    *d = '\0';
}

// Ein Formularfeld aus dem Body ziehen, decoden und ins NVS schreiben.
// skip_if_empty: leere Geheimfelder (Passwort/Keys) lassen den alten
// Wert stehen.
static void apply_field(const char *body, const char *field,
                        const char *cfg_key, bool skip_if_empty)
{
    char val[160];
    if (httpd_query_key_value(body, field, val, sizeof(val)) != ESP_OK) {
        return;
    }
    url_decode(val);
    if (skip_if_empty && val[0] == '\0') {
        return;
    }
    config_set_str(cfg_key, val);
}

// --- HTTP-Handler -----------------------------------------------------

static esp_err_t get_handler(httpd_req_t *req)
{
    static char html[2048];
    snprintf(html, sizeof(html),
        "<!DOCTYPE html><html><head><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>MiniLingo Setup</title>"
        "<style>body{font-family:sans-serif;max-width:480px;margin:1em auto;"
        "padding:0 1em}label{display:block;margin:.6em 0 .2em;font-weight:bold}"
        "input{width:100%%;padding:.5em;box-sizing:border-box}"
        "button{margin-top:1em;padding:.7em 1.4em;font-size:1em}"
        "small{color:#666}</style></head><body>"
        "<h2>MiniLingo Setup</h2>"
        "<form method=post action=/save>"
        "<label>WLAN-SSID</label><input name=ssid value='%s'>"
        "<label>WLAN-Passwort</label>"
        "<input name=pass type=password placeholder='unveraendert lassen'>"
        "<label>Cerebras API-Key (LLM)</label>"
        "<input name=cerebras type=password placeholder='unveraendert lassen'>"
        "<label>Groq API-Key (Whisper)</label>"
        "<input name=groq type=password placeholder='unveraendert lassen'>"
        "<label>Gemini API-Key (TTS)</label>"
        "<input name=gemini type=password placeholder='unveraendert lassen'>"
        "<label>LLM-Modell</label><input name=model value='%s'>"
        "<label>Whisper-Sprache</label><input name=lang value='%s'>"
        "<label>Gemini-Voice</label><input name=voice value='%s'>"
        "<button type=submit>Speichern &amp; Neustart</button>"
        "<p><small>Geheimfelder leer lassen, um den bisherigen Wert zu "
        "behalten.</small></p>"
        "</form></body></html>",
        config_wifi_ssid(), config_llm_model(), config_whisper_language(),
        config_gemini_voice());

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static void reboot_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

static esp_err_t save_handler(httpd_req_t *req)
{
    char body[1024];
    int len = req->content_len;
    if (len <= 0 || len >= (int)sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form");
        return ESP_FAIL;
    }
    int got = httpd_req_recv(req, body, len);
    if (got <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
        return ESP_FAIL;
    }
    body[got] = '\0';

    // Nicht-Geheim: immer übernehmen (Felder sind vorbefüllt). Geheim:
    // nur wenn ausgefüllt.
    apply_field(body, "ssid",     CFG_KEY_WIFI_SSID,    false);
    apply_field(body, "model",    CFG_KEY_LLM_MODEL,    false);
    apply_field(body, "lang",     CFG_KEY_WHISPER_LANG, false);
    apply_field(body, "voice",    CFG_KEY_GEMINI_VOICE, false);
    apply_field(body, "pass",     CFG_KEY_WIFI_PASS,    true);
    apply_field(body, "cerebras", CFG_KEY_CEREBRAS_KEY, true);
    apply_field(body, "groq",     CFG_KEY_GROQ_KEY,     true);
    apply_field(body, "gemini",   CFG_KEY_GEMINI_KEY,   true);

    ESP_LOGI(TAG, "config saved, rebooting");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req,
        "<html><body style='font-family:sans-serif'>"
        "<h2>Gespeichert</h2><p>MiniLingo startet neu...</p>"
        "</body></html>");

    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

// --- Start ------------------------------------------------------------

static void start_softap(void)
{
    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid = AP_SSID,
            .ssid_len = strlen(AP_SSID),
            .password = AP_PASS,
            .max_connection = AP_MAX_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    // STA bleibt aktiv (APSTA), damit eine bestehende Verbindung nicht
    // abreißt; der AP kommt zusätzlich dazu.
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_LOGI(TAG, "SoftAP \"%s\" up — http://192.168.4.1", AP_SSID);
}

static void start_httpd(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return;
    }
    httpd_uri_t get_uri = {
        .uri = "/", .method = HTTP_GET, .handler = get_handler };
    httpd_uri_t save_uri = {
        .uri = "/save", .method = HTTP_POST, .handler = save_handler };
    httpd_register_uri_handler(s_server, &get_uri);
    httpd_register_uri_handler(s_server, &save_uri);
}

void provisioning_start(void)
{
    if (atomic_exchange(&s_active, true)) {
        return;  // schon aktiv
    }
    start_softap();
    start_httpd();
}

bool provisioning_is_active(void)
{
    return atomic_load(&s_active);
}
