// WiFi-STA-Connect mit Kconfig-Credentials (Spec Section 10/12, Phase 2).
// Reconnect-Strategie: bei Disconnect einfach erneut verbinden — das
// Geraet ist stationaer, die Verbindung soll dauerhaft stehen.
#include <string.h>              // [C] Header-Include | [Logik] strlcpy() fuer sicheres Kopieren von SSID/Passwort
#include <stdatomic.h>           // [C] Header-Include | [C] atomische Typen/Operationen (atomic_bool, atomic_load/store/exchange)

#include "freertos/FreeRTOS.h"   // [C] Header-Include | [HW] FreeRTOS-Basistypen/Makros fuer die darunterliegenden Header
#include "freertos/task.h"       // [C] Header-Include | [HW] FreeRTOS-Task-API (hier indirekt fuer Timer-/Event-Infrastruktur benoetigt)

#include "esp_event.h"           // [C] Header-Include | [HW] ESP-IDF Event-Loop-API (esp_event_handler_instance_register etc.)
#include "esp_log.h"             // [C] Header-Include | [Logik] Logging-Makros ESP_LOGI/ESP_LOGW
#include "esp_netif.h"           // [C] Header-Include | [HW] Netzwerk-Interface-Abstraktion (esp_netif_init, esp_netif_create_default_wifi_sta)
#include "esp_timer.h"           // [C] Header-Include | [HW] High-Resolution-Timer-API fuer den verzoegerten Reconnect
#include "esp_wifi.h"            // [C] Header-Include | [HW] Kern-API des WiFi-Treibers (esp_wifi_init/connect/set_mode/set_config/start)
#include "sdkconfig.h"           // [C] Header-Include | [HW] generierte Kconfig-Makros (CONFIG_*) aus menuconfig

#include "config.h"              // [C] Header-Include | [Logik] Zugriff auf per NVS gespeicherte WiFi-Credentials (config_wifi_ssid/password)
#include "wifi.h"                // [C] Header-Include | [Logik] eigene oeffentliche Deklarationen (wifi_init, wifi_is_connected, Callback-Typ)

#define WIFI_RECONNECT_DELAY_MS 5000  // [C] Praeprozessor-Konstante (Textersetzung) | [Logik] Wartezeit vor automatischem Reconnect-Versuch nach Disconnect

static const char *TAG = "wifi";  // [C] static = Datei-lokale (interne) Linkage | [Logik] Log-Tag zur Zuordnung der Ausgaben dieses Moduls

static wifi_status_cb_t s_status_cb;      // [C] static Datei-globale Variable, Funktionszeiger-Typ | [Logik] vom Anwendungscode registrierter Callback fuer Statusaenderungen
static atomic_bool s_connected;           // [C] atomarer Bool-Typ | [Logik] threadsicherer Cache des aktuellen Verbindungszustands (Zugriff aus mehreren Tasks moeglich)
static esp_timer_handle_t s_reconnect_timer;  // [C] static Handle-Variable (opaquer Zeiger) | [HW] Referenz auf den One-Shot-Timer fuer den verzoegerten Reconnect

// Timer-Callback des One-Shot-Reconnect-Timers: wird ausgeloest, nachdem
// nach einem Disconnect WIFI_RECONNECT_DELAY_MS verstrichen sind, und stoesst
// einen neuen Verbindungsversuch an.
static void reconnect_timer_cb(void *arg)  // [C] static = interne Linkage | [C] Signatur passt zur esp_timer-Callback-Typdefinition (void*-Argument) | [HW] wird vom esp_timer-Subsystem im Timer-Task aufgerufen
{
    esp_wifi_connect();  // [HW] stoesst einen neuen WiFi-Verbindungsversuch (STA) an
}

// Zentraler Event-Handler fuer WiFi- und IP-Events: startet die Verbindung
// beim STA-Start, reagiert auf Disconnects mit verzoegertem Reconnect und
// meldet erfolgreiche IP-Zuweisung (DHCP) ueber den registrierten Callback.
// Laeuft im Kontext des Event-Loop-Tasks.
static void wifi_event_handler(void *arg, esp_event_base_t event_base,  // [C] static = interne Linkage | [HW] Standard-Signatur eines esp_event-Handlers (Callback-Funktionszeiger, wird bei Registrierung uebergeben)
                               int32_t event_id, void *event_data)      // [C] event_id unterscheidet konkretes Event innerhalb der Base; event_data zeigt auf ereignisspezifische Payload
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {  // [C] Vergleich von Event-Base und Event-ID | [HW] Event: WiFi-Treiber im STA-Modus gestartet
        esp_wifi_connect();  // [HW] erster Verbindungsaufbau, nachdem der STA-Treiber bereit ist
    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {  // [HW] Event: WiFi-Verbindung wurde getrennt (z.B. AP ausser Reichweite, Fehler)
        wifi_event_sta_disconnected_t *dis = event_data;  // [C] Cast des generischen void*-Pointers auf den konkreten Event-Struct-Typ | [HW] enthaelt u.a. den Disconnect-Grund (reason)
        bool was_connected = atomic_exchange(&s_connected, false);  // [C] atomares Lesen-und-Setzen in einem Schritt (verhindert Race Condition) | [Logik] merkt sich alten Zustand und setzt sofort auf "getrennt"
        if (was_connected && s_status_cb) {  // [C] Kurzschlussauswertung (&&): zweiter Ausdruck nur bei true des ersten geprueft | [Logik] Callback nur bei tatsaechlichem Zustandswechsel und wenn ueberhaupt registriert
            s_status_cb(false);  // [Logik] informiert Anwendung (z.B. UI) ueber Verbindungsverlust
        }
        ESP_LOGW(TAG, "disconnected (reason %d), reconnecting in %d ms",
                 dis->reason, WIFI_RECONNECT_DELAY_MS);  // [C] Zugriff auf Struct-Member ueber Pointer (->) | [Logik] Warn-Log mit Disconnect-Grund und geplanter Wartezeit
        // Nicht im Event-Loop-Task blockieren — Reconnect verzoegert
        // ueber One-Shot-Timer.
        esp_timer_start_once(s_reconnect_timer,
                             (uint64_t)WIFI_RECONNECT_DELAY_MS * 1000);  // [C] expliziter Cast auf uint64_t vor der Multiplikation (Ueberlauf/Typsicherheit) | [HW] startet den Timer einmalig, Zeitangabe in Mikrosekunden | [Logik] entkoppelt Reconnect-Verzoegerung vom Event-Handler, damit der Event-Loop-Task nicht blockiert
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {  // [HW] Event: DHCP hat dem STA-Interface eine IP-Adresse zugewiesen
        ip_event_got_ip_t *event = event_data;  // [C] Cast auf den IP-Event-Struct-Typ | [HW] enthaelt u.a. ip_info mit der zugewiesenen IPv4-Adresse
        ESP_LOGI(TAG, "connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));  // [C] IPSTR/IP2STR sind Makros, die zur Kompilierzeit String-Konkatenation bzw. Argument-Expansion durchfuehren | [HW] loggt die per DHCP erhaltene IP-Adresse
        atomic_store(&s_connected, true);  // [C] atomares Schreiben ohne Zwischenzustand | [Logik] Verbindungsstatus jetzt "verbunden"
        if (s_status_cb) {  // [Logik] Callback nur aufrufen, wenn Anwendung einen registriert hat
            s_status_cb(true);  // [Logik] informiert Anwendung (z.B. UI) ueber erfolgreiche Verbindung
        }
    }
}

// Initialisiert den WiFi-Stack im Stationsmodus: legt Netif/Event-Loop an,
// registriert den Event-Handler, uebernimmt SSID/Passwort aus dem
// NVS-Konfigurationscache und startet den Verbindungsaufbau. Muss einmalig
// nach nvs_flash_init() in app_main aufgerufen werden.
esp_err_t wifi_init(wifi_status_cb_t status_cb)  // [C] Rueckgabetyp esp_err_t (Fehlercode); Parameter ist Funktionszeiger auf Status-Callback
{
    s_status_cb = status_cb;  // [Logik] Callback fuer spaetere Statusmeldungen im Modul merken

    const esp_timer_create_args_t timer_args = {  // [C] const-Struct-Initialisierung mit designated initializers | [HW] Konfiguration des spaeteren One-Shot-Reconnect-Timers
        .callback = reconnect_timer_cb,  // [C] Zuweisung eines Funktionszeigers an das Callback-Feld der Struct
        .name = "wifi_reconnect",        // [Logik] Name des Timers, hilfreich fuer Debug-/Diagnoseausgaben
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_reconnect_timer));  // [C] Makro prueft Rueckgabewert und bricht bei Fehler mit Abort ab | [HW] erzeugt den Timer, Handle wird in s_reconnect_timer abgelegt

    ESP_ERROR_CHECK(esp_netif_init());  // [HW] initialisiert die Netif-Abstraktionsschicht (Voraussetzung fuer WiFi/IP-Stack)
    ESP_ERROR_CHECK(esp_event_loop_create_default());  // [HW] erstellt den Standard-Event-Loop, ueber den WiFi-/IP-Events zugestellt werden
    esp_netif_create_default_wifi_sta();  // [HW] legt das Standard-Netzwerkinterface fuer den WiFi-Stationsmodus an

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();  // [C] Makro-Expansion liefert vorbefuellte Default-Konfigurationsstruct | [HW] Grundeinstellungen fuer den WiFi-Treiber (Puffer, Task-Prioritaeten etc.)
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));  // [HW] initialisiert den WiFi-Treiber/Ressourcen mit der uebergebenen Konfiguration

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));  // [C] &wifi_event_handler = Adresse der Callback-Funktion (Funktionszeiger) | [HW] registriert den Handler fuer alle WiFi-Events (ESP_EVENT_ANY_ID)
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));  // [HW] registriert denselben Handler zusaetzlich fuer das IP-Event "IP per DHCP erhalten"

    // SSID/Passwort kommen seit dem Provisioning aus dem NVS-Cache
    // (config.c), nicht mehr aus den Kconfig-Makros.
    wifi_config_t wifi_cfg = {  // [C] Struct-Initialisierung, restliche Felder werden implizit auf 0 gesetzt | [HW] Konfigurationsstruct fuer STA-Modus (SSID, Passwort, Auth-Schwelle)
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,  // [C] verschachtelter designated initializer auf Struct-Member | [HW] fordert mindestens WPA2-PSK-Sicherheit vom Access Point
        },
    };
    strlcpy((char *)wifi_cfg.sta.ssid, config_wifi_ssid(),
            sizeof(wifi_cfg.sta.ssid));  // [C] Cast auf char* und sizeof des Zielarrays begrenzt die Kopie (verhindert Pufferueberlauf) | [Logik] SSID aus dem NVS-Konfigurationscache in die WiFi-Config uebernehmen
    strlcpy((char *)wifi_cfg.sta.password, config_wifi_password(),
            sizeof(wifi_cfg.sta.password));  // [C] gleiches Muster fuer das Passwort-Feld | [Logik] Passwort aus dem NVS-Konfigurationscache uebernehmen
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));  // [HW] versetzt den WiFi-Treiber explizit in den Stationsmodus (Client, kein Access Point)
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));  // [HW] uebertraegt SSID/Passwort/Auth-Konfiguration an das STA-Interface
    ESP_ERROR_CHECK(esp_wifi_start());  // [HW] startet den WiFi-Treiber; loest daraufhin WIFI_EVENT_STA_START aus

    ESP_LOGI(TAG, "STA started, connecting to \"%s\"", config_wifi_ssid());  // [Logik] Info-Log ueber gestarteten Verbindungsversuch samt Ziel-SSID
    return ESP_OK;  // [Logik] Erfolg der Initialisierung (nicht des eigentlichen Verbindungsaufbaus, der erfolgt asynchron)
}

// Gibt den zuletzt vom Event-Handler ermittelten Verbindungszustand zurueck.
// Threadsicher und nicht blockierend, kann von beliebigen Tasks (z.B. UI)
// jederzeit abgefragt werden.
bool wifi_is_connected(void)  // [C] Rueckgabetyp bool, keine Parameter (void)
{
    return atomic_load(&s_connected);  // [C] atomares Lesen ohne Locking | [Logik] liefert den aktuellen gecachten Verbindungsstatus
}
