// WiFi-STA-Connect mit Kconfig-Credentials (Spec Section 10/12, Phase 2).
// Reconnect-Strategie: bei Disconnect einfach erneut verbinden — das
// Geraet ist stationaer, die Verbindung soll dauerhaft stehen.
#include <string.h>
#include <stdatomic.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h" //
#include "esp_log.h"
#include "esp_netif.h" // = Netzwerk-Interface von ESP-IDF (WiFi, Ethernet, etc.)
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
 // esp_event_base_t = Typ für Event-Base (z.B. WIFI_EVENT, IP_EVENT)
 // void *event_data = Pointer auf die Event-Daten (z.B. IP-Adresse, Disconnect-Reason, etc.)
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect(); // WiFi-STA-Interface gestartet -> Verbindung zum Access-Point herstellen
    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) { // WiFi-STA-Interface getrennt -> Reconnect starten
        wifi_event_sta_disconnected_t *dis = event_data; // Pointer auf die Disconnect-Event-Daten (enthält u.a. den Disconnect-Reason)
        bool was_connected = atomic_exchange(&s_connected, false); // atomar den aktuellen WiFi-Status auf "nicht verbunden" setzen und den vorherigen Status zurückgeben (true = verbunden, false = nicht verbunden)
        if (was_connected && s_status_cb) {
            s_status_cb(false); // wenn vorher verbunden war und callback gesetzt ist, diesen callback aufrufen um UI zu informieren, dass WiFi getrennt ist
        }
        ESP_LOGW(TAG, "disconnected (reason %d), reconnecting in %d ms",
                 dis->reason, WIFI_RECONNECT_DELAY_MS);

        // Nicht im Event-Loop-Task blockieren — Reconnect verzoegert
        // ueber One-Shot-Timer.
        esp_timer_start_once(s_reconnect_timer,
                             (uint64_t)WIFI_RECONNECT_DELAY_MS * 1000); // Timer starten, der nach WIFI_RECONNECT_DELAY_MS Millisekunden den reconnect_timer_cb aufruft, um die Verbindung erneut herzustellen
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = event_data; // Pointer auf die IP-Event-Daten (enthält u.a. die zugewiesene IP-Adresse)
        ESP_LOGI(TAG, "connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));
        atomic_store(&s_connected, true); // atomar den aktuellen WiFi-Status auf "verbunden" setzen
        if (s_status_cb) {
            s_status_cb(true); // wenn callback gesetzt ist, diesen aufrufen um UI zu informieren, dass WiFi verbunden ist
        }
    }
}

esp_err_t wifi_init(wifi_status_cb_t status_cb)
{
    s_status_cb = status_cb; // speichert den Callback, der aufgerufen wird, wenn sich der WiFi-Status ändert (verbunden/verbunden)

    // Args für den Reconnect-Timer vorbereiten:
    const esp_timer_create_args_t timer_args = {
        .callback = reconnect_timer_cb,
        .name = "wifi_reconnect",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_reconnect_timer)); //timer erstellen und handle in s_reconnect_timer speichern -> wird später für reconnect verwendet

    ESP_ERROR_CHECK(esp_netif_init()); // Netzwerk-Stack initialisieren
    ESP_ERROR_CHECK(esp_event_loop_create_default()); // Default-Event-Loop erstellen (für WiFi-Events, IP-Events, etc.)
    esp_netif_create_default_wifi_sta(); // Default-WiFi-STA-Interface erstellen (für Station-Modus, d.h. Client)

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT(); // WiFi-Konfiguration initialisieren (Standardwerte aus sdkconfig.h)
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg)); // Initialisiert das WiFi-Modul mit der Konfiguration init_cfg (Treiber laden, Hardware vorbereiten, etc.)

    // Event-Handler für WiFi- und IP-Events registrieren (für Connect/Disconnect/IP-Adress-Zuweisung)
    // &wifi_event_handler = Zeiger auf die Funktion, die aufgerufen wird, wenn ein WiFi- oder IP-Event auftritt -> implementierung siehe oben
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    // SSID/Passwort kommen seit dem Provisioning aus dem NVS-Cache
    // (config.c), nicht mehr aus den Kconfig-Makros. -> können dadurch zur Laufzeit geändert werden, ohne dass ein Rebuild nötig ist.
    wifi_config_t wifi_cfg = {
        .sta = { //.sta = Station-Mode-Konfiguration (Client-Modus)
            .threshold.authmode = WIFI_AUTH_WPA2_PSK, // nur WPA2-PSK erlauben (keine offenen Netzwerke, keine WEP, etc.)
        },
    };

    // strlcpy = safer version of strcpy -> verhindert buffer overflow, indem es die maximale Länge des Ziel-Strings angibt -> kopiert die SSID und das Passwort aus der Config in die WiFi-Konfiguration
    // (char *) cast, weil wifi_cfg.sta.ssid eigentlich typ uint8_t[] ist, aber strlcpy erwartet char* als ZIel-Parameter -> kein Problem, weil uint8_t und char beide 1 Byte groß sind
    strlcpy((char *)wifi_cfg.sta.ssid, config_wifi_ssid(), 
            sizeof(wifi_cfg.sta.ssid)); // kopiert die SSID aus der Config in die WiFi-Konfiguration
    strlcpy((char *)wifi_cfg.sta.password, config_wifi_password(),
            sizeof(wifi_cfg.sta.password)); // kopiert das Passwort aus der Config in die WiFi-Konfiguration
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA)); // WiFi-Modus auf Station (Client) setzen
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg)); // WiFi-Konfiguration für das Station-Interface setzen (SSID, Passwort, Auth-Mode, etc.)
    ESP_ERROR_CHECK(esp_wifi_start()); // WiFi starten (Verbindung zum Access-Point herstellen, DHCP-Client starten, etc.)

    ESP_LOGI(TAG, "STA started, connecting to \"%s\"", config_wifi_ssid());
    return ESP_OK;
}

// Gibt den aktuellen WiFi-Status zurück (true = verbunden, false = getrennt). Wird von der UI-Task alle 50 ms abgefragt, um das WiFi-Icon in der Statusbar zu aktualisieren.
bool wifi_is_connected(void)
{
    return atomic_load(&s_connected);
}
