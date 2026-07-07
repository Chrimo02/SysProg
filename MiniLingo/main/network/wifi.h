#pragma once

#include <stdbool.h>            // [C] Header-Include | [Logik] Standard-Bool-Typ (bool/true/false) fuer die API unten
#include "esp_err.h"            // [C] Header-Include | [HW] esp_err_t-Fehlercode-Typ der ESP-IDF-APIs

// Callback bei Aenderung des Verbindungsstatus (z.B. fuer das
// WiFi-Icon in der Status-Bar). Laeuft im Event-Task-Kontext —
// LVGL-Calls darin muessen ueber lv_async_call o.ae. entkoppelt werden.
typedef void (*wifi_status_cb_t)(bool connected);  // [C] typedef auf Funktionszeigertyp: Signatur fuer Status-Callbacks | [HW] Parameter connected spiegelt WiFi-Verbindungszustand | [Logik] Entkopplung: WiFi-Modul meldet Zustand, ohne UI-Code zu kennen

// Startet WiFi-STA mit den Kconfig-Credentials (CONFIG_WIFI_SSID /
// CONFIG_WIFI_PASSWORD) und reconnected automatisch bei Disconnect.
// Blockiert nicht; der Status kommt asynchron ueber den Callback.
// Erwartet initialisiertes NVS (nvs_flash_init in app_main).
esp_err_t wifi_init(wifi_status_cb_t status_cb);  // [C] Funktionsdeklaration, Parameter ist Funktionszeiger | [HW] initialisiert WiFi-Stack im Stationsmodus (STA) | [Logik] Rueckgabewert signalisiert nur Init-Erfolg, nicht den Verbindungsstatus

// Liefert den aktuellen (thread-sicher gecachten) WiFi-Verbindungsstatus.
// Kann aus beliebigem Task-Kontext abgefragt werden, ohne zu blockieren.
bool wifi_is_connected(void);  // [C] Funktionsdeklaration ohne Parameter (void) | [Logik] Abfrage des zuletzt vom Event-Handler gesetzten Verbindungszustands
