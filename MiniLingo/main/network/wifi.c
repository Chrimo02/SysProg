// WiFi-STA-Connect mit Kconfig-Credentials (Spec Section 10/12, Phase 2).
// Reconnect-Strategie: bei Disconnect einfach erneut verbinden — das
// Geraet ist stationaer, die Verbindung soll dauerhaft stehen.
#include <string.h>
#include <stdatomic.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "sdkconfig.h"

#include "config.h"
#include "wifi.h"

#define WIFI_RECONNECT_DELAY_MS 5000

static const char *TAG = "wifi";

static wifi_status_cb_t s_status_cb;
static atomic_bool s_connected;
static esp_timer_handle_t s_reconnect_timer;

static void reconnect_timer_cb(void *arg)
{
    esp_wifi_connect();
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *dis = event_data;
        bool was_connected = atomic_exchange(&s_connected, false);
        if (was_connected && s_status_cb) {
            s_status_cb(false);
        }
        ESP_LOGW(TAG, "disconnected (reason %d), reconnecting in %d ms",
                 dis->reason, WIFI_RECONNECT_DELAY_MS);
        // Nicht im Event-Loop-Task blockieren — Reconnect verzoegert
        // ueber One-Shot-Timer.
        esp_timer_start_once(s_reconnect_timer,
                             (uint64_t)WIFI_RECONNECT_DELAY_MS * 1000);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = event_data;
        ESP_LOGI(TAG, "connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));
        atomic_store(&s_connected, true);
        if (s_status_cb) {
            s_status_cb(true);
        }
    }
}

esp_err_t wifi_init(wifi_status_cb_t status_cb)
{
    s_status_cb = status_cb;

    const esp_timer_create_args_t timer_args = {
        .callback = reconnect_timer_cb,
        .name = "wifi_reconnect",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_reconnect_timer));

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    // SSID/Passwort kommen seit dem Provisioning aus dem NVS-Cache
    // (config.c), nicht mehr aus den Kconfig-Makros.
    wifi_config_t wifi_cfg = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strlcpy((char *)wifi_cfg.sta.ssid, config_wifi_ssid(),
            sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, config_wifi_password(),
            sizeof(wifi_cfg.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "STA started, connecting to \"%s\"", config_wifi_ssid());
    return ESP_OK;
}

bool wifi_is_connected(void)
{
    return atomic_load(&s_connected);
}
