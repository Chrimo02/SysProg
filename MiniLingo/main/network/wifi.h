#pragma once

#include <stdbool.h>
#include "esp_err.h"

// Callback bei Aenderung des Verbindungsstatus (z.B. fuer das
// WiFi-Icon in der Status-Bar). Laeuft im Event-Task-Kontext —
// LVGL-Calls darin muessen ueber lv_async_call o.ae. entkoppelt werden.
typedef void (*wifi_status_cb_t)(bool connected);

// Startet WiFi-STA mit den Kconfig-Credentials (CONFIG_WIFI_SSID /
// CONFIG_WIFI_PASSWORD) und reconnected automatisch bei Disconnect.
// Blockiert nicht; der Status kommt asynchron ueber den Callback.
// Erwartet initialisiertes NVS (nvs_flash_init in app_main).
esp_err_t wifi_init(wifi_status_cb_t status_cb);

bool wifi_is_connected(void);
