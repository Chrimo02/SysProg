#include "freertos/FreeRTOS.h"              // RTOS-Basis: Typen (TickType_t), Makros, Scheduler-Config.
#include "freertos/task.h"                  // Task-API (xTaskCreate/vTaskDelay) – von NimBLE/MCPWM genutzt.
#include "esp_log.h"                         // ESP_LOGI/W/E – schreibt Logzeilen über UART (Serial Monitor).
#include "driver/mcpwm_prelude.h"            // MCPWM-Treiber (Timer/Operator/Comparator/Generator) für HW-PWM.
#include "nimble/nimble_port.h"              // nimble_port_init/run – initialisiert/treibt den BLE-Stack.
#include "nimble/nimble_port_freertos.h"     // legt die FreeRTOS-Tasks an, in denen der BLE-Host läuft.
#include "host/ble_hs.h"                     // BLE-Host-API: GAP/GATT, ble_hs_cfg, Callback-Typen.
#include "host/util/util.h"                  // Host-Helfer, u.a. ble_hs_util_ensure_addr (sichert BLE-Adresse).
#include "services/gap/ble_svc_gap.h"        // GAP-Service: Standard-Charakteristika "Device Name"/"Appearance".
#include "services/gatt/ble_svc_gatt.h"      // GATT-Service: stellt den GATT-Server bereit.
#include "nvs_flash.h"                        // NVS: persistenter Speicher, den der BLE-Controller braucht.

static const char *TAG = "servo_ble";        // Präfix, das jede ESP_LOG*-Zeile vorangestellt bekommt.

#define SERVO_MIN_PULSEWIDTH_US      1000    // Puls (µs) für untere Endlage (~ -90°); RC-Servo erwartet 1..2 ms.
#define SERVO_MAX_PULSEWIDTH_US      2000    // Puls (µs) für obere Endlage (~ +90°).
#define SERVO_MIN_DEGREE             -90     // untere Winkelgrenze (Grad), auf die geclamped wird.
#define SERVO_MAX_DEGREE             90      // obere Winkelgrenze (Grad).
#define SERVO_PULSE_GPIO             4       // GPIO, der getoggelt wird (Signalleitung des Servos hängt hier).
#define SERVO_TIMEBASE_RESOLUTION_HZ 1000000 // 1 MHz Zähltakt → 1 Tick = 1 µs, Pulsbreiten direkt als Ticks.
#define SERVO_TIMEBASE_PERIOD        20000   // Periode in Ticks = 20 ms = 50 Hz (Servo-Wiederholrate).

#define BLE_DEVICE_NAME "ESP32-Servo"        // Name, unter dem der ESP im Advertising/Scanner-Apps erscheint.
#define SERVICE_UUID    0x00FF               // 16-bit UUID des eigenen GATT-Service (frei gewählt).
#define CHAR_UUID       0xFF01               // 16-bit UUID der Charakteristik, in die der Client schreibt.

static mcpwm_cmpr_handle_t comparator = NULL; // Handle: über dessen Compare-Wert wird die Pulsbreite gesetzt.
static int current_angle = 0;                 // zuletzt gesetzter Winkel, Bezug für relative Befehle ('L'/'R').

static inline uint32_t angle_to_compare(int angle)
{
    return (angle - SERVO_MIN_DEGREE) * (SERVO_MAX_PULSEWIDTH_US - SERVO_MIN_PULSEWIDTH_US)  // Winkel → Pulsbreite (µs):
           / (SERVO_MAX_DEGREE - SERVO_MIN_DEGREE) + SERVO_MIN_PULSEWIDTH_US;                // lineare Abbildung [-90..90]→[1000..2000].
}

static void set_servo_angle(int angle)
{
    if (angle < SERVO_MIN_DEGREE) angle = SERVO_MIN_DEGREE;                 // nach unten begrenzen.
    if (angle > SERVO_MAX_DEGREE) angle = SERVO_MAX_DEGREE;                 // nach oben begrenzen.
    current_angle = angle;                                                 // neuen Winkel merken (für 'L'/'R').
    mcpwm_comparator_set_compare_value(comparator, angle_to_compare(angle)); // Compare-Wert schreiben → neue Pulsbreite ab nächster Periode.
    ESP_LOGI(TAG, "Servo -> %d°", angle);                                  // Winkel über UART loggen.
}

static void servo_init(void)
{
    mcpwm_timer_handle_t timer = NULL;                                      // Handle der Zeitbasis (Zähler).
    mcpwm_timer_config_t timer_config = {
        .group_id = 0,                                  // MCPWM-Gruppe 0 (ESP32 hat zwei Gruppen).
        .clk_src = MCPWM_TIMER_CLK_SRC_DEFAULT,         // Default-Taktquelle (APB 80 MHz).
        .resolution_hz = SERVO_TIMEBASE_RESOLUTION_HZ,  // Prescaler ergibt 1 MHz Zählfrequenz.
        .period_ticks = SERVO_TIMEBASE_PERIOD,          // Zähler läuft 0..19999 → 20 ms.
        .count_mode = MCPWM_TIMER_COUNT_MODE_UP,        // aufwärts zählen (Sägezahn).
    };
    ESP_ERROR_CHECK(mcpwm_new_timer(&timer_config, &timer));               // Timer anlegen; bei Fehler abbrechen.

    mcpwm_oper_handle_t oper = NULL;                                        // Operator verbindet Timer mit Comparator/Generator.
    mcpwm_operator_config_t operator_config = { .group_id = 0 };            // gleiche Gruppe wie der Timer.
    ESP_ERROR_CHECK(mcpwm_new_operator(&operator_config, &oper));          // Operator anlegen.
    ESP_ERROR_CHECK(mcpwm_operator_connect_timer(oper, timer));            // Operator an Timer binden.

    mcpwm_comparator_config_t comparator_config = {
        .flags.update_cmp_on_tez = true,    // Compare-Wert erst bei Timer-Zero übernehmen → glitch-freie Pulsbreite.
    };
    ESP_ERROR_CHECK(mcpwm_new_comparator(oper, &comparator_config, &comparator)); // Comparator anlegen (vergleicht Zählerstand).

    mcpwm_gen_handle_t generator = NULL;                                    // Generator: schaltet tatsächlich den GPIO.
    mcpwm_generator_config_t generator_config = { .gen_gpio_num = SERVO_PULSE_GPIO }; // Ausgabe auf SERVO_PULSE_GPIO.
    ESP_ERROR_CHECK(mcpwm_new_generator(oper, &generator_config, &generator)); // Generator anlegen.

    ESP_ERROR_CHECK(mcpwm_comparator_set_compare_value(comparator, angle_to_compare(0))); // Startpuls für 0° (Mittelstellung).
    ESP_ERROR_CHECK(mcpwm_generator_set_action_on_timer_event(generator,                  // bei Timer-Zero (Periodenanfang):
        MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, MCPWM_TIMER_EVENT_EMPTY, MCPWM_GEN_ACTION_HIGH))); // GPIO auf HIGH.
    ESP_ERROR_CHECK(mcpwm_generator_set_action_on_compare_event(generator,                // bei Erreichen des Compare-Werts:
        MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, comparator, MCPWM_GEN_ACTION_LOW))); // GPIO auf LOW → Pulslänge = Compare-µs.

    ESP_ERROR_CHECK(mcpwm_timer_enable(timer));                            // Timer scharfschalten.
    ESP_ERROR_CHECK(mcpwm_timer_start_stop(timer, MCPWM_TIMER_START_NO_STOP)); // dauerhaft starten – PWM läuft in HW, ohne CPU.
}

static int gatt_write_cb(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;                // nur Schreibzugriffe behandeln.

    uint8_t cmd = 0;                                                       // Puffer für das empfangene Steuerbyte.
    os_mbuf_copydata(ctxt->om, 0, 1, &cmd);                               // 1 Byte ab Offset 0 aus dem mbuf lesen → cmd.
    ESP_LOGI(TAG, "Empfangen: %c (0x%02X)", cmd, cmd);                    // Byte als ASCII und Hex loggen.

    switch (cmd) {                                                         // Single-Byte-Protokoll auswerten:
        case 'L': set_servo_angle(current_angle - 30); break;            // 30° nach links.
        case 'R': set_servo_angle(current_angle + 30); break;            // 30° nach rechts.
        case 'C': set_servo_angle(0);                  break;            // zurück in die Mitte.
        default:
            ESP_LOGW(TAG, "Unbekannter Befehl: 0x%02X", cmd);            // unbekanntes Byte: nur warnen.
    }
    return 0;                                                             // 0 = Erfolg an den NimBLE-Host melden.
}

static const struct ble_gatt_svc_def gatt_services[] = {                  // GATT-Tabelle; NimBLE liest sie einmal beim Start.
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,                // Primary Service (im Discovery sichtbar).
        .uuid = BLE_UUID16_DECLARE(SERVICE_UUID),         // 16-bit UUID des Service.
        .characteristics = (struct ble_gatt_chr_def[]) {  // Liste der Charakteristika:
            {
                .uuid = BLE_UUID16_DECLARE(CHAR_UUID),    // UUID der Steuer-Charakteristik.
                .access_cb = gatt_write_cb,               // Callback bei Zugriff.
                .flags = BLE_GATT_CHR_F_WRITE             // Write mit Response erlaubt
                       | BLE_GATT_CHR_F_WRITE_NO_RSP,     // und Write ohne Response (latenzärmer).
            },
            { 0 }                                         // Nullterminator der Charakteristik-Liste.
        },
    },
    { 0 }                                                 // Nullterminator der Service-Liste.
};

static void ble_advertise(void)
{
    struct ble_hs_adv_fields fields = {0};                                // Advertising-Felder, alles auf 0.
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;     // discoverable + kein klassisches BT.
    fields.name = (uint8_t *)BLE_DEVICE_NAME;                            // Gerätename ins Advertising schreiben.
    fields.name_len = strlen(BLE_DEVICE_NAME);                           // Länge des Namens.
    fields.name_is_complete = 1;                                         // Name vollständig (nicht gekürzt).
    ble_gap_adv_set_fields(&fields);                                     // Felder → Stack baut das Payload.

    struct ble_gap_adv_params adv_params = {0};                          // Wie geworben wird:
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;    // verbindbar (undirected connectable).
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;    // allgemein auffindbar.
    ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,         // Advertising starten: Public-Adresse,
                      &adv_params, NULL, NULL);                          // kein Peer, unbegrenzt lang.
}

static void ble_on_sync(void)                                            // feuert, wenn Host+Controller synchron sind.
{
    ble_hs_util_ensure_addr(0);                                          // gültige BLE-Adresse sicherstellen.
    ble_advertise();                                                     // mit dem Werben beginnen.
    ESP_LOGI(TAG, "BLE advertising als '%s'", BLE_DEVICE_NAME);          // Status loggen.
}

static void nimble_host_task(void *param)
{
    nimble_port_run();              // blockiert und verarbeitet BLE-Events.
    nimble_port_freertos_deinit();  // Aufräumen, wenn nimble_port_run() zurückkehrt.
}

void app_main(void)                                                      // Einsprung nach dem Boot.
{
    esp_err_t ret = nvs_flash_init();                                   // NVS öffnen (BLE-Controller braucht ihn).
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());                            // Partition leeren …
        ret = nvs_flash_init();                                        // … und neu anlegen.
    }
    ESP_ERROR_CHECK(ret);                                              // jeden anderen Fehler hart abbrechen.

    servo_init();           // MCPWM konfigurieren und PWM starten.
    set_servo_angle(0);     // initial in Mittelstellung fahren.

    nimble_port_init();             // BLE-Stack initialisieren.
    ble_svc_gap_init();             // Standard-GAP-Service (Name/Appearance) registrieren.
    ble_svc_gatt_init();            // Standard-GATT-Service registrieren.
    ble_gatts_count_cfg(gatt_services); // Ressourcenbedarf der eigenen Services berechnen.
    ble_gatts_add_svcs(gatt_services);  // eigene Services registrieren.
    ble_hs_cfg.sync_cb = ble_on_sync;   // Sync-Callback setzen → startet das Advertising.

    nimble_port_freertos_init(nimble_host_task); // BLE-Host-Task starten.
}
