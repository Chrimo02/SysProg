// FreeRTOS-Kernheader: liefert Typen wie TickType_t, Makros (pdMS_TO_TICKS) und
// die Basis-Konfiguration des RTOS, das auf dem ESP32 als Scheduler läuft.
#include "freertos/FreeRTOS.h"
// FreeRTOS Task-API: vTaskDelay, xTaskCreate etc. – wird hier indirekt durch
// NimBLE/MCPWM benötigt (Tasks für BLE-Host).
#include "freertos/task.h"
// ESP-IDF Logging-Framework: stellt ESP_LOGI/W/E-Makros bereit, die über UART
// (Serial Monitor) ausgegeben werden.
#include "esp_log.h"
// Treiber für das MCPWM-Peripheral des ESP32 (Motor Control PWM).
// "prelude" bündelt alle Sub-Header (timer, operator, comparator, generator),
// mit denen ein präzises Hardware-PWM-Signal erzeugt wird – ideal für Servos.
#include "driver/mcpwm_prelude.h"
// NimBLE Portierungsschicht: nimble_port_init/run – initialisiert den BLE-Stack
// und seine internen Queues auf dem ESP32 (Bluetooth-Controller läuft im ROM).
#include "nimble/nimble_port.h"
// NimBLE-Anbindung an FreeRTOS: erzeugt die Tasks, in denen der BLE-Host läuft.
#include "nimble/nimble_port_freertos.h"
// BLE-Host-API: GAP/GATT-Funktionen, ble_hs_cfg (Konfigurationsstruct), Callbacks.
#include "host/ble_hs.h"
// NimBLE-Hilfsfunktionen, u.a. ble_hs_util_ensure_addr (sichert eine BLE-Adresse).
#include "host/util/util.h"
// GAP-Service (Generic Access Profile) – stellt Standard-Charakteristika wie
// "Device Name" und "Appearance" bereit, die jeder BLE-Peripheral haben sollte.
#include "services/gap/ble_svc_gap.h"
// GATT-Service (Generic Attribute Profile) – stellt den GATT-Server bereit,
// über den eigene Services/Charakteristika veröffentlicht werden.
#include "services/gatt/ble_svc_gatt.h"
// Non-Volatile Storage: BLE-Stack speichert hier Bonding-Keys / Kalibrierdaten
// des Funk-Controllers. Muss vor BLE-Init initialisiert sein.
#include "nvs_flash.h"

// Tag-String für ESP_LOG*: erscheint als Präfix in jeder Logzeile.
static const char *TAG = "servo_ble";

// Pulslänge in Mikrosekunden für die untere Endlage des Servos (~ -90°).
// Standard-RC-Servos erwarten 1 ms ... 2 ms Puls innerhalb eines 20-ms-Rahmens.
#define SERVO_MIN_PULSEWIDTH_US      1000
// Pulslänge in Mikrosekunden für die obere Endlage (~ +90°).
#define SERVO_MAX_PULSEWIDTH_US      2000
// Logische Mindestwinkel-Grenze in Grad, auf die clamped wird.
#define SERVO_MIN_DEGREE             -90
// Logische Maximalwinkel-Grenze in Grad.
#define SERVO_MAX_DEGREE             90
// GPIO-Pin des ESP32, an dem die Signalleitung des Servos hängt.
#define SERVO_PULSE_GPIO             2
// MCPWM-Zeitbasis: 1 MHz → 1 Tick = 1 µs. So lassen sich Pulsbreiten in µs
// direkt als Tick-Werte verwenden.
#define SERVO_TIMEBASE_RESOLUTION_HZ 1000000
// Periodendauer in Ticks. Bei 1 MHz Auflösung = 20000 µs = 20 ms = 50 Hz,
// die typische Wiederholrate für analoge Servos.
#define SERVO_TIMEBASE_PERIOD        20000

// Gerätename, unter dem der ESP32 im BLE-Advertising sichtbar ist (Scanner-Apps).
#define BLE_DEVICE_NAME "ESP32-Servo"
// 16-bit UUID des eigenen GATT-Service (frei gewählt, ohne offizielle Registrierung).
#define SERVICE_UUID    0x00FF
// 16-bit UUID der Charakteristik, in die der Client Steuerbefehle schreibt.
#define CHAR_UUID       0xFF01

// Handle auf den MCPWM-Comparator. Über dessen Compare-Wert wird die Pulsbreite
// (= Servo-Winkel) zur Laufzeit verändert. static = nur in dieser Datei sichtbar.
static mcpwm_cmpr_handle_t comparator = NULL;
// Aktuell zuletzt gesetzter Winkel; dient als Bezug für relative Bewegungen ('L'/'R').
static int current_angle = 0;

// Wandelt einen Winkel in Grad in einen Compare-Tick-Wert um (= Pulsbreite in µs).
// inline: Compiler darf den Aufruf inlinen, da kurz und häufig.
static inline uint32_t angle_to_compare(int angle)
{
    // Lineare Abbildung [SERVO_MIN_DEGREE..SERVO_MAX_DEGREE] → [MIN_PW..MAX_PW].
    // (angle - MIN_DEG) verschiebt auf 0..180, danach Skalierung auf 1000..2000.
    return (angle - SERVO_MIN_DEGREE) * (SERVO_MAX_PULSEWIDTH_US - SERVO_MIN_PULSEWIDTH_US)
           / (SERVO_MAX_DEGREE - SERVO_MIN_DEGREE) + SERVO_MIN_PULSEWIDTH_US;
}

// Setzt den Servo auf einen bestimmten Winkel (mit Begrenzung auf gültigen Bereich).
static void set_servo_angle(int angle)
{
    // Clamping nach unten: kein Wert kleiner als -90°.
    if (angle < SERVO_MIN_DEGREE) angle = SERVO_MIN_DEGREE;
    // Clamping nach oben: kein Wert größer als +90°.
    if (angle > SERVO_MAX_DEGREE) angle = SERVO_MAX_DEGREE;
    // Aktuellen Winkel merken (für nächste relative Bewegung).
    current_angle = angle;
    // Compare-Wert des MCPWM neu setzen: ändert ab dem nächsten Periodenstart
    // die Pulsbreite und damit die Servo-Position. Hardware erledigt das Timing.
    mcpwm_comparator_set_compare_value(comparator, angle_to_compare(angle));
    // Info-Log über UART: "°" wird als UTF-8 ausgegeben.
    ESP_LOGI(TAG, "Servo -> %d°", angle);
}

// Initialisiert die komplette MCPWM-Kette: Timer → Operator → Comparator → Generator.
static void servo_init(void)
{
    // Handle für den MCPWM-Timer (Zeitbasis/Zähler).
    mcpwm_timer_handle_t timer = NULL;
    // Konfiguration der Zeitbasis:
    mcpwm_timer_config_t timer_config = {
        .group_id = 0,                                  // MCPWM-Gruppe 0 (ESP32 hat zwei Gruppen).
        .clk_src = MCPWM_TIMER_CLK_SRC_DEFAULT,         // Default-Taktquelle (APB-Clock 80 MHz).
        .resolution_hz = SERVO_TIMEBASE_RESOLUTION_HZ,  // Prescaler ergibt 1 MHz Zählerfrequenz.
        .period_ticks = SERVO_TIMEBASE_PERIOD,          // Zähler läuft 0..19999 → 20 ms Periode.
        .count_mode = MCPWM_TIMER_COUNT_MODE_UP,        // Aufwärts zählen (Sägezahn).
    };
    // Timer-Instanz im Treiber anlegen; ESP_ERROR_CHECK bricht bei Fehler ab.
    ESP_ERROR_CHECK(mcpwm_new_timer(&timer_config, &timer));

    // Operator: verbindet einen Timer mit Comparatoren/Generatoren.
    mcpwm_oper_handle_t oper = NULL;
    mcpwm_operator_config_t operator_config = { .group_id = 0 }; // gleiche Gruppe wie Timer.
    ESP_ERROR_CHECK(mcpwm_new_operator(&operator_config, &oper));
    // Operator an den eben erzeugten Timer binden.
    ESP_ERROR_CHECK(mcpwm_operator_connect_timer(oper, timer));

    // Comparator: vergleicht Zählerstand mit einem Compare-Wert → erzeugt Events,
    // an denen der Generator den Pin umschalten kann.
    mcpwm_comparator_config_t comparator_config = {
        .flags.update_cmp_on_tez = true,    // Compare-Wert erst bei Timer-Zero (TEZ) übernehmen
                                            // → glitch-freies Update der Pulsbreite.
    };
    ESP_ERROR_CHECK(mcpwm_new_comparator(oper, &comparator_config, &comparator));

    // Generator: das Stück Hardware, das tatsächlich den GPIO toggelt.
    mcpwm_gen_handle_t generator = NULL;
    mcpwm_generator_config_t generator_config = { .gen_gpio_num = SERVO_PULSE_GPIO };
    ESP_ERROR_CHECK(mcpwm_new_generator(oper, &generator_config, &generator));

    // Initiale Pulsbreite für Winkel 0° setzen (Servo geht in Mittelstellung).
    ESP_ERROR_CHECK(mcpwm_comparator_set_compare_value(comparator, angle_to_compare(0)));
    // Bei Timer-Zero (Anfang der 20-ms-Periode): GPIO auf HIGH.
    ESP_ERROR_CHECK(mcpwm_generator_set_action_on_timer_event(generator,
        MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, MCPWM_TIMER_EVENT_EMPTY, MCPWM_GEN_ACTION_HIGH)));
    // Bei Erreichen des Compare-Werts: GPIO auf LOW → Pulslänge = Compare-Wert in µs.
    ESP_ERROR_CHECK(mcpwm_generator_set_action_on_compare_event(generator,
        MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, comparator, MCPWM_GEN_ACTION_LOW)));

    // Timer aktivieren (Konfiguration scharfschalten).
    ESP_ERROR_CHECK(mcpwm_timer_enable(timer));
    // Timer dauerhaft starten – PWM läuft jetzt komplett in Hardware ohne CPU-Last.
    ESP_ERROR_CHECK(mcpwm_timer_start_stop(timer, MCPWM_TIMER_START_NO_STOP));
}

// GATT-Callback: wird vom NimBLE-Host aufgerufen, sobald ein Client auf unsere
// Charakteristik zugreift (Read/Write). Hier nur Write relevant.
static int gatt_write_cb(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    // Nur Schreibzugriffe auf die Charakteristik behandeln – alles andere ignorieren.
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;

    // Puffer für das erste empfangene Byte (Steuerzeichen).
    uint8_t cmd = 0;
    // Daten aus dem mbuf (NimBLEs verkettetem Puffer) extrahieren: 1 Byte ab Offset 0.
    os_mbuf_copydata(ctxt->om, 0, 1, &cmd);
    // Empfangenes Byte loggen – sowohl als ASCII-Zeichen als auch hex.
    ESP_LOGI(TAG, "Empfangen: %c (0x%02X)", cmd, cmd);

    // Befehlsauswertung: einfaches Single-Byte-Protokoll.
    switch (cmd) {
        case 'L': set_servo_angle(current_angle - 30); break;   // 30° nach links.
        case 'R': set_servo_angle(current_angle + 30); break;   // 30° nach rechts.
        case 'C': set_servo_angle(0);                  break;   // zurück in die Mitte.
        default:
            // Unbekanntes Byte: nur warnen, kein Fehler – Verbindung bleibt aktiv.
            ESP_LOGW(TAG, "Unbekannter Befehl: 0x%02X", cmd);
    }
    // 0 = Erfolg an den NimBLE-Host zurückmelden.
    return 0;
}

// GATT-Service-Definition als statisches Array. NimBLE liest diese Struktur
// einmal beim Start und baut daraus die Attribut-Tabelle des GATT-Servers auf.
static const struct ble_gatt_svc_def gatt_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,                // Primary Service (wird im Discovery sichtbar).
        .uuid = BLE_UUID16_DECLARE(SERVICE_UUID),         // 16-bit UUID des Service.
        .characteristics = (struct ble_gatt_chr_def[]) {  // Liste der Charakteristika:
            {
                .uuid = BLE_UUID16_DECLARE(CHAR_UUID),    // 16-bit UUID der Steuer-Charakteristik.
                .access_cb = gatt_write_cb,               // Callback bei Zugriff.
                .flags = BLE_GATT_CHR_F_WRITE             // erlaubt klassisches Write (mit Response)
                       | BLE_GATT_CHR_F_WRITE_NO_RSP,     // und Write ohne Response (latenzärmer).
            },
            { 0 }                                         // Nullterminator der Charakteristik-Liste.
        },
    },
    { 0 }                                                 // Nullterminator der Service-Liste.
};

// Startet das BLE-Advertising, sodass Smartphones den ESP32 finden können.
static void ble_advertise(void)
{
    // Struktur für die Advertising-Felder; alle Felder zunächst auf 0.
    struct ble_hs_adv_fields fields = {0};
    // Flags: allgemein discoverable + BR/EDR (klassisches Bluetooth) NICHT unterstützt.
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    // Gerätename in das Advertising-Paket aufnehmen.
    fields.name = (uint8_t *)BLE_DEVICE_NAME;
    fields.name_len = strlen(BLE_DEVICE_NAME);
    fields.name_is_complete = 1;                 // Name ist vollständig (nicht abgeschnitten).
    // Felder an den Stack übergeben – er baut daraus das Advertising-Payload.
    ble_gap_adv_set_fields(&fields);

    // Parameter, wie geworben werden soll:
    struct ble_gap_adv_params adv_params = {0};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;    // verbindbar (undirected connectable).
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;    // allgemein auffindbar.
    // Advertising starten: eigene Public-Adresse, kein Peer (broadcast), unbegrenzt lang.
    ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                      &adv_params, NULL, NULL);
}

// Wird vom NimBLE-Host aufgerufen, sobald Host und Controller synchronisiert sind
// (also Funk-Hardware ist bereit). Erst danach darf advertised werden.
static void ble_on_sync(void)
{
    // Stellt sicher, dass eine gültige BLE-Adresse vorhanden ist (public oder random).
    ble_hs_util_ensure_addr(0);
    // Loslegen mit dem Werben.
    ble_advertise();
    ESP_LOGI(TAG, "BLE advertising als '%s'", BLE_DEVICE_NAME);
}

// FreeRTOS-Task, der die NimBLE-Event-Loop ausführt – läuft, bis der Stack stoppt.
static void nimble_host_task(void *param)
{
    nimble_port_run();              // blockiert und verarbeitet BLE-Events.
    nimble_port_freertos_deinit();  // Aufräumen, wenn nimble_port_run() zurückkehrt.
}

// Einsprungspunkt der Anwendung – wird von ESP-IDF nach dem Boot aufgerufen.
void app_main(void)
{
    // NVS initialisieren
    // NVS-Partition öffnen; der BLE-Controller braucht persistenten Speicher.
    esp_err_t ret = nvs_flash_init();
    // Falls Partition voll/inkompatibel: löschen und neu anlegen.
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);   // jeden anderen Fehler hart abbrechen.

    // Servo initialisieren
    servo_init();           // MCPWM-Hardware konfigurieren und PWM starten.
    set_servo_angle(0);     // initial in Mittelstellung fahren.

    // NimBLE initialisieren
    nimble_port_init();             // BLE-Stack samt internen Strukturen initialisieren.
    ble_svc_gap_init();             // Standard-GAP-Service (Name, Appearance) registrieren.
    ble_svc_gatt_init();            // Standard-GATT-Service (Service Changed) registrieren.
    ble_gatts_count_cfg(gatt_services); // Ressourcenbedarf der eigenen Services kalkulieren.
    ble_gatts_add_svcs(gatt_services);  // Eigene Services tatsächlich registrieren.
    ble_hs_cfg.sync_cb = ble_on_sync;   // Callback setzen, der bei Sync feuert (→ Advertising).

    // BLE-Host in eigenem FreeRTOS-Task starten; nimble_host_task ist die Loop.
    nimble_port_freertos_init(nimble_host_task);
}
