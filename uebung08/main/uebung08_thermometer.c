// FreeRTOS-Kernheader: Typen, Makros (pdMS_TO_TICKS) und Konfiguration des
// RTOS, das den ESP32 als Scheduler trägt.
#include "freertos/FreeRTOS.h"
// FreeRTOS Task-API: xTaskCreate, vTaskDelay – für den BLE-Host- und den
// Notify-Task in dieser Datei.
#include "freertos/task.h"
// ESP-IDF Logging-Framework: ESP_LOGI/W/E-Makros, Ausgabe via UART.
#include "esp_log.h"
// Non-Volatile Storage: BLE-Controller speichert hier Calibration/Bonding-Keys;
// muss vor dem BLE-Init initialisiert werden.
#include "nvs_flash.h"
// GPIO-Treiber des ESP32: gpio_set_direction/level – Steuerung der OneWire-Leitung.
#include "driver/gpio.h"
// ROM-Funktion ets_delay_us: präzise µs-Busy-Wait. Wichtig für das streng
// getimete OneWire-Protokoll (Software-Bitbanging).
#include "rom/ets_sys.h"
// NimBLE-Portierungsschicht: initialisiert den BLE-Stack auf dem ESP32.
#include "nimble/nimble_port.h"
// NimBLE-Anbindung an FreeRTOS: Tasks, in denen der BLE-Host läuft.
#include "nimble/nimble_port_freertos.h"
// BLE-Host-API: GAP-/GATT-Funktionen, ble_hs_cfg, Callbacks.
#include "host/ble_hs.h"
// NimBLE-Hilfsfunktionen, u.a. ble_hs_util_ensure_addr.
#include "host/util/util.h"
// GAP-Standardservice (Generic Access Profile): Device Name, Appearance, …
#include "services/gap/ble_svc_gap.h"
// GATT-Standardservice (Generic Attribute Profile): GATT-Server.
#include "services/gatt/ble_svc_gatt.h"

// Log-Tag, erscheint als Präfix in jeder Logzeile.
static const char *TAG = "thermo_ble";

// ─── 1-WIRE / DS18B20 ───────────────────────────────────────────────────────

// GPIO-Pin, an dem die DQ-Leitung des DS18B20 hängt (mit 4,7 kΩ Pull-up nach VCC).
#define OW_PIN GPIO_NUM_5

// OneWire-ROM-Befehl: alle Slaves ansprechen (nur einer am Bus → keine Adressierung nötig).
#define CMD_SKIP_ROM        0xCC
// DS18B20-Funktion: Temperaturmessung starten.
#define CMD_CONVERT_T       0x44
// DS18B20-Funktion: 9 Byte Scratchpad lesen (enthält Temperaturregister).
#define CMD_READ_SCRATCHPAD 0xBE

// Kurzer Wrapper um ets_delay_us, damit der Code besser lesbar ist.
static inline void delay_us(uint32_t us) { ets_delay_us(us); }

// OneWire-Leitung aktiv auf LOW ziehen: Pin als Output und Level 0 setzen.
static void ow_low(void)     { gpio_set_direction(OW_PIN, GPIO_MODE_OUTPUT); gpio_set_level(OW_PIN, 0); }
// Leitung loslassen: Pin als Input → Pull-up zieht auf HIGH (Open-Drain-Idee).
static void ow_release(void) { gpio_set_direction(OW_PIN, GPIO_MODE_INPUT); }
// Aktuellen Pegel der Leitung lesen (0 = jemand zieht LOW, 1 = idle HIGH).
static int  ow_read(void)    { return gpio_get_level(OW_PIN); }

// OneWire-Reset-Puls + Presence-Detection.
static int ow_reset(void) {
    ow_low(); delay_us(480);                // ≥480 µs LOW = Reset-Puls.
    ow_release(); delay_us(70);             // 70 µs warten; DS18B20 antwortet danach mit LOW.
    int presence = !ow_read();              // LOW gelesen → Sensor präsent.
    delay_us(410);                          // Restlichen Slot abwarten (Gesamtsumme ≥480 µs).
    return presence;
}

// Ein Bit auf den OneWire-Bus schreiben (timing-kritisch, daher Busy-Waits).
static void ow_write_bit(int bit) {
    if (bit) { ow_low(); delay_us(6);  ow_release(); delay_us(64); }   // Eine 1: kurzer LOW-Puls.
    else      { ow_low(); delay_us(60); ow_release(); delay_us(10); }  // Eine 0: langer LOW-Puls.
}

// Ein Bit vom OneWire-Bus lesen: Master initiiert Slot, Slave hält LOW/lässt los.
static int ow_read_bit(void) {
    ow_low(); delay_us(6);                  // Slot starten (kurzer LOW-Puls).
    ow_release(); delay_us(9);              // Sample-Zeitpunkt abwarten (~15 µs nach Slot-Beginn).
    int bit = ow_read();                    // jetzt sampeln.
    delay_us(55);                           // Rest des 60-µs-Slots verstreichen lassen.
    return bit;
}

// Ein Byte schreiben: LSB zuerst (OneWire-Konvention).
static void ow_write_byte(uint8_t data) {
    for (int i = 0; i < 8; i++) { ow_write_bit(data & 1); data >>= 1; }
}

// Ein Byte lesen: LSB zuerst, dann ins MSB schieben.
static uint8_t ow_read_byte(void) {
    uint8_t data = 0;
    for (int i = 0; i < 8; i++) { data >>= 1; if (ow_read_bit()) data |= 0x80; }
    return data;
}

// Komplette Temperaturmessung am DS18B20.
static float read_temperature(void) {
    uint8_t scratchpad[9];                  // Sensor liefert 9 Bytes (inkl. CRC).

    if (!ow_reset()) return -999.0f;        // Kein Sensor gefunden → Fehlerwert.
    ow_write_byte(CMD_SKIP_ROM);            // ROM-Adressierung überspringen (Single-Drop-Bus).
    ow_write_byte(CMD_CONVERT_T);           // Wandlung starten.
    vTaskDelay(pdMS_TO_TICKS(750));         // 12-bit-Wandlung dauert bis ~750 ms.

    if (!ow_reset()) return -999.0f;        // Neuer Reset vor dem Lesen.
    ow_write_byte(CMD_SKIP_ROM);
    ow_write_byte(CMD_READ_SCRATCHPAD);     // Scratchpad anfordern.
    for (int i = 0; i < 9; i++) scratchpad[i] = ow_read_byte();

    // Temperaturregister = Byte 0 (LSB) + Byte 1 (MSB) als 16-bit signed.
    int16_t raw = (scratchpad[1] << 8) | scratchpad[0];
    // 1 LSB = 1/16 °C (12-bit-Auflösung).
    return (float)raw / 16.0f;
}

// ─── BLE ────────────────────────────────────────────────────────────────────

// Anzeigename im BLE-Advertising.
#define BLE_DEVICE_NAME "ESP32-Thermo"
// 16-bit UUID des eigenen Service (frei gewählt).
#define SERVICE_UUID    0x00FF
// 16-bit UUID der Temperatur-Charakteristik.
#define CHAR_UUID       0xFF02

// Vom Stack vergebenes Attribut-Handle der Charakteristik (für Notifications gebraucht).
static uint16_t temp_char_handle = 0;
// Handle der aktuell offenen Verbindung; NONE = nicht verbunden.
static uint16_t conn_handle_global = BLE_HS_CONN_HANDLE_NONE;

// Vorwärtsdeklaration, weil ble_gap_event_cb advertise() bei Disconnect aufruft.
static void ble_advertise(void);

// FreeRTOS-Task: sendet alle 2 s die aktuelle Temperatur als Notify an den Client.
static void temp_notify_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(2000));                                // 2-Sekunden-Takt.
        if (conn_handle_global == BLE_HS_CONN_HANDLE_NONE) continue;    // kein Client → nichts zu tun.
        if (temp_char_handle == 0) continue;                            // Charakteristik noch nicht registriert.

        float temp = read_temperature();                                // OneWire-Messung.
        ESP_LOGI(TAG, "Temperatur: %.2f °C", temp);

        char buf[16];
        snprintf(buf, sizeof(buf), "%.2f C", temp);                     // als ASCII-String formatieren.
        // mbuf = NimBLEs verketteter Puffer; ble_hs_mbuf_from_flat kopiert den String hinein.
        struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, strlen(buf));
        // Notification an den verbundenen Client – ohne Bestätigung, geringe Latenz.
        ble_gatts_notify_custom(conn_handle_global, temp_char_handle, om);
    }
}

// GATT-Callback: feuert bei klassischem Read-Zugriff auf die Charakteristik.
static int gatt_read_cb(uint16_t conn_handle, uint16_t attr_handle,
                        struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    float temp = read_temperature();                            // Live-Messung beim Read.
    ESP_LOGI(TAG, "Temperatur gelesen: %.2f °C", temp);
    char buf[16];
    snprintf(buf, sizeof(buf), "%.2f C", temp);
    // Antwortdaten in den vom Stack bereitgestellten mbuf anhängen.
    os_mbuf_append(ctxt->om, buf, strlen(buf));
    return 0;                                                   // Erfolg an NimBLE zurückmelden.
}

// GATT-Service-Definition als statisches Array; NimBLE liest sie einmal beim Start.
static const struct ble_gatt_svc_def gatt_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,                  // Primary Service (im Discovery sichtbar).
        .uuid = BLE_UUID16_DECLARE(SERVICE_UUID),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = BLE_UUID16_DECLARE(CHAR_UUID),
                .access_cb = gatt_read_cb,                  // Callback bei Read.
                .val_handle = &temp_char_handle,            // Stack schreibt hier das Handle hinein.
                .flags = BLE_GATT_CHR_F_READ                // erlaubt Read
                       | BLE_GATT_CHR_F_NOTIFY,             // und Notifications (Server → Client push).
            },
            { 0 }                                           // Nullterminator der Charakteristik-Liste.
        },
    },
    { 0 }                                                   // Nullterminator der Service-Liste.
};

// GAP-Event-Callback: wird bei Verbindungs-Ereignissen vom NimBLE-Host aufgerufen.
static int ble_gap_event_cb(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {                                       // 0 = erfolgreich verbunden.
                ESP_LOGI(TAG, "Verbunden");
                conn_handle_global = event->connect.conn_handle;                    // Handle merken.
                // Notify-Task erst beim Connect starten – spart Ressourcen im idle.
                xTaskCreate(temp_notify_task, "temp_notify", 4096, NULL, 5, NULL);
            }
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "Getrennt");
            conn_handle_global = BLE_HS_CONN_HANDLE_NONE;                           // Verbindungs-Handle invalidieren.
            ble_advertise();                                                        // Werbung wieder aufnehmen.
            break;
        default:
            break;
    }
    return 0;
}

// Startet das BLE-Advertising, damit Smartphones den ESP32 finden.
static void ble_advertise(void)
{
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;   // discoverable, kein Classic-BT.
    fields.name = (uint8_t *)BLE_DEVICE_NAME;
    fields.name_len = strlen(BLE_DEVICE_NAME);
    fields.name_is_complete = 1;                                       // Name vollständig.
    ble_gap_adv_set_fields(&fields);

    struct ble_gap_adv_params adv_params = {0};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;                      // verbindbar (undirected).
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;                      // allgemein auffindbar.
    // Werbung dauerhaft starten; ble_gap_event_cb erhält Connect/Disconnect-Events.
    ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                      &adv_params, ble_gap_event_cb, NULL);
}

// Wird vom NimBLE-Host nach Sync mit dem Controller aufgerufen (Funk ist bereit).
static void ble_on_sync(void)
{
    ble_hs_util_ensure_addr(0);          // sicherstellen, dass eine BLE-Adresse vorhanden ist.
    ble_advertise();                     // Werbung starten.
    ESP_LOGI(TAG, "BLE advertising als '%s'", BLE_DEVICE_NAME);
}

// FreeRTOS-Task für die NimBLE-Event-Loop.
static void nimble_host_task(void *param)
{
    nimble_port_run();                   // verarbeitet BLE-Events (blockierend).
    nimble_port_freertos_deinit();       // Aufräumen, falls die Loop beendet wird.
}

// ─── MAIN ───────────────────────────────────────────────────────────────────

// Einsprungspunkt der Anwendung – wird von ESP-IDF nach dem Boot aufgerufen.
void app_main(void)
{
    // NVS-Partition öffnen; BLE-Controller braucht persistenten Speicher.
    esp_err_t ret = nvs_flash_init();
    // Falls Partition voll/inkompatibel: löschen und neu anlegen.
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // GPIO initialisieren
    gpio_reset_pin(OW_PIN);                          // alte Konfiguration (z.B. JTAG) entfernen.
    gpio_set_direction(OW_PIN, GPIO_MODE_INPUT);     // Idle-Zustand = Input (Pull-up hält HIGH).

    // NimBLE initialisieren
    nimble_port_init();                              // BLE-Stack initialisieren.
    ble_svc_gap_init();                              // GAP-Standardservice registrieren.
    ble_svc_gatt_init();                             // GATT-Standardservice registrieren.
    ble_gatts_count_cfg(gatt_services);              // Ressourcen für eigene Services kalkulieren.
    ble_gatts_add_svcs(gatt_services);               // eigene Services registrieren.
    ble_hs_cfg.sync_cb = ble_on_sync;                // Callback bei Stack-Sync setzen.

    // BLE-Host in eigenem FreeRTOS-Task starten.
    nimble_port_freertos_init(nimble_host_task);
}
