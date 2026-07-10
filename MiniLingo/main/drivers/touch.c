// XPT2046-Treiber (abgeleitet aus touch_test.c) als LVGL-Input-Device.
#include "touch.h"

#include "display.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "touch";

#define TOUCH_HOST    SPI2_HOST // Display-Controller und Touch-Controller teilen sich den SPI-Bus (DMA), daher hier auch SPI2_HOST
#define PIN_NUM_T_CS  GPIO_NUM_38 // XPT2046 Chip Select (CS) Pin -> Separate CS-Leitungen für Display und Touch

// XPT2046 control bytes (12-bit mode, single-ended, power on).
// Achtung: laut Datenblatt liest 0x90 die Y- und 0xD0 die X-Position —
// die Benennung hier stammt aus touch_test.c und ist vertauscht,
// TOUCH_SWAP_XY=1 unten gleicht das aus.
#define CMD_READ_X  0x90 // X-Position lesen
#define CMD_READ_Y  0xD0 // Y-Position lesen
#define CMD_READ_Z1 0xB0 // Z1-Position lesen -> Z1 = Druckstärke

#define TOUCH_PRESS_THRESHOLD 100  // Z1-Rohwert, ab dem "gedrueckt" gilt
#define TOUCH_POLL_PERIOD_MS  50 // Zeit die angibt, wie oft der Touchscreen abgefragt wird (Polling-Intervall)

// Hardcodierte Kalibrierung (Spec Phase 1): Rohwert-Spannen der vier
// Eckpunkte. Werte sind typische Defaults fuer ILI9341/XPT2046-Module
// und muessen am Geraet verifiziert werden — beim ersten Druck pro
// Beruehrung werden Roh- und Pixelkoordinaten geloggt.
#define TOUCH_RAW_X_MIN 200
#define TOUCH_RAW_X_MAX 3900
#define TOUCH_RAW_Y_MIN 240
#define TOUCH_RAW_Y_MAX 3800
#define TOUCH_SWAP_XY   1
#define TOUCH_INVERT_X  0
#define TOUCH_INVERT_Y  1

static spi_device_handle_t s_touch_spi; // Handle für den SPI-Touch-Controller
static lv_indev_t *s_indev; // Handle für das LVGL-Input-Device

// function die den XPT2046 über SPI ausliest und den 12-bit Rohwert zurückgibt -> 
static uint16_t xpt2046_read(uint8_t cmd)
{
    // 3-Byte-Transfer ueber die internen FIFO-Register (kein DMA noetig,
    // der Bus selbst laeuft mit DMA fuer das Display).
    spi_transaction_t t = {
        .length = 24,
        .flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA,
        .tx_data = {cmd, 0x00, 0x00},
    };
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_touch_spi, &t));

    // 12-bit result in bits [14:3] of the 16-bit reply
    return ((t.rx_data[1] << 8) | t.rx_data[2]) >> 3;
}

// Mappt den Rohwert (raw) auf die Pixelkoordinate (0..res-1) unter
// Beruecksichtigung der Invertierung (invert). Rohwert wird auf die
// Spanne [raw_min, raw_max] begrenzt.
static int32_t map_axis(int32_t raw, int32_t raw_min, int32_t raw_max,
                        int32_t res, bool invert)
{
    if (raw < raw_min) raw = raw_min;
    if (raw > raw_max) raw = raw_max;
    int32_t v = (raw - raw_min) * (res - 1) / (raw_max - raw_min);
    return invert ? (res - 1 - v) : v;
}

// LVGL-Callback, der den Touchscreen ausliest und die Daten in das
// lv_indev_data_t-Struct schreibt. Wird periodisch vom LVGL-Timer
// aufgerufen (TOUCH_POLL_PERIOD_MS).
static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    static bool was_pressed = false;

    // Z1-Rohwert lesen, um zu entscheiden, ob der Touchscreen gedrueckt ist.
    uint16_t z1 = xpt2046_read(CMD_READ_Z1);
    if (z1 < TOUCH_PRESS_THRESHOLD) {
        data->state = LV_INDEV_STATE_RELEASED; // Touch nicht gedrueckt, LVGL-Status auf "released" setzen
        was_pressed = false; // Merker, dass der Touch nicht gedrueckt war, damit beim naechsten Druck ein Log ausgegeben wird
        return;
    }

    // Zwei Messungen mitteln, XPT2046-Rohwerte rauschen merklich.
    uint16_t raw_x = (xpt2046_read(CMD_READ_X) + xpt2046_read(CMD_READ_X)) / 2;
    uint16_t raw_y = (xpt2046_read(CMD_READ_Y) + xpt2046_read(CMD_READ_Y)) / 2;

#if TOUCH_SWAP_XY
    uint16_t tmp = raw_x;
    raw_x = raw_y;
    raw_y = tmp;
#endif

// Rohwerte auf Pixelkoordinaten mappen und in das lv_indev_data_t-Struct schreiben. Invertierung wird beruecksichtigt.
    data->point.x = map_axis(raw_x, TOUCH_RAW_X_MIN, TOUCH_RAW_X_MAX,
                             DISPLAY_H_RES, TOUCH_INVERT_X);
    data->point.y = map_axis(raw_y, TOUCH_RAW_Y_MIN, TOUCH_RAW_Y_MAX,
                             DISPLAY_V_RES, TOUCH_INVERT_Y);
    data->state = LV_INDEV_STATE_PRESSED;

    if (!was_pressed) {
        ESP_LOGI(TAG, "Touch: raw=(%u,%u) -> px=(%d,%d) z1=%u",
                 raw_x, raw_y, (int)data->point.x, (int)data->point.y, z1);
        was_pressed = true;
    }
}

// Initialisiert den XPT2046-Touchscreen und registriert ihn als LVGL-Input-Device. Der Touchscreen wird über SPI angesprochen, und die LVGL-Callback-Funktion touch_read_cb wird periodisch aufgerufen, um die Touchdaten auszulesen.
esp_err_t touch_init(void)
{
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 2 * 1000 * 1000, // 2 Mhz 
        .mode = 0, // SPI mode 0 -> clock low when idle, data sampled on rising edge
        .spics_io_num = PIN_NUM_T_CS, // Touchscreen Chip Select Pin
        .queue_size = 1, // SPI-Warteschlange -> Nur 1 Transaction: Input-Device braucht keine Queueing wie Display
    };
    // SPI-Bus-Device hinzufügen
    ESP_RETURN_ON_ERROR(spi_bus_add_device(TOUCH_HOST, &devcfg, &s_touch_spi),
                        TAG, "spi_bus_add_device failed");

    s_indev = lv_indev_create(); // LVGL-Input-Device erstellen
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER); // Input-Device als Zeigergerät konfigurieren
    lv_indev_set_read_cb(s_indev, touch_read_cb); // Callback-Funktion setzen
    lv_timer_set_period(lv_indev_get_read_timer(s_indev), TOUCH_POLL_PERIOD_MS); // Polling-Perioden setzen

    ESP_LOGI(TAG, "XPT2046 registered as LVGL pointer device");
    return ESP_OK;
}

// Gibt Handle auf das LVGL Input-Device (Touchscreen) zurück (andere Module brauchen das für Input-Handling)
lv_indev_t *touch_get_lv_indev(void)
lv_indev_t *touch_get_lv_indev(void)
{
    return s_indev;
}
