// DS18B20-Temperatursensor, 1-Wire auf GPIO 48 (Bit-Bang via GPIO).
// Kein externer Component — IDF5 GPIO + ets_delay_us reichen aus.
// Die Messung (12-Bit, ~750 ms) laeuft in einem periodischen esp_timer
// (one-shot, alle 10 s rearmed). temperature_get() liefert den Cache
// sofort ohne Bus-I/O, atomar wie volume_get().
//
// 1-Wire-Timings gemaess Maxim DS18B20 Datasheet Rev. 6 (Abb. 1):
//   Reset pulse:  >= 480 µs low; Sample nach 15-60 µs (Presence: 60-240 µs)
//   Write-1 slot: 1-15 µs low, dann release (gesamt >= 60 µs)
//   Write-0 slot: 60-120 µs low
//   Read slot:    1-15 µs low, dann sample innerhalb 15 µs nach Release
//
// Aufloesung: 12 Bit (default). Conversion time: max 750 ms.
// CRC: Maxim DOW CRC (x^8 + x^5 + x^4 + 1).
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rom/ets_sys.h"

#include "temperature.h"

static const char *TAG = "temperature";

// Messung alle 10 s; Pause zwischen Warmup-Delay und naechster Messung.
#define MEASURE_INTERVAL_US  (10 * 1000 * 1000)
// Wandlungszeit 12-Bit DS18B20 max 750 ms.
#define CONVERT_WAIT_MS      800

// Gecachter Wert in milli-Celsius; INT32_MIN = ungueltig.
static atomic_int  s_milli_celsius;
static atomic_bool s_valid;
static esp_timer_handle_t s_timer;

// --- 1-Wire low-level (GPIO Bit-Bang, critical sections per task) ---

static inline void ow_low(void)
{
    gpio_set_direction(TEMP_ONEWIRE_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(TEMP_ONEWIRE_GPIO, 0);
}

static inline void ow_release(void)
{
    // Pullup zieht die Leitung auf High; nur Richtung aendern.
    gpio_set_direction(TEMP_ONEWIRE_GPIO, GPIO_MODE_INPUT);
}

static inline int ow_read_bit(void)
{
    return gpio_get_level(TEMP_ONEWIRE_GPIO);
}

// Gibt true zurueck, wenn ein Presence-Pulse erkannt wurde.
static bool ow_reset(void)
{
    ow_low();
    ets_delay_us(480);
    ow_release();
    ets_delay_us(70);
    int presence = !ow_read_bit();
    ets_delay_us(410);
    return presence;
}

static void ow_write_bit(int bit)
{
    ow_low();
    if (bit) {
        ets_delay_us(6);
        ow_release();
        ets_delay_us(64);
    } else {
        ets_delay_us(60);
        ow_release();
        ets_delay_us(10);
    }
}

static int ow_read_bit_slot(void)
{
    ow_low();
    ets_delay_us(6);
    ow_release();
    ets_delay_us(9);
    int bit = ow_read_bit();
    ets_delay_us(55);
    return bit;
}

static void ow_write_byte(uint8_t byte)
{
    for (int i = 0; i < 8; i++) {
        ow_write_bit(byte & 0x01);
        byte >>= 1;
    }
}

static uint8_t ow_read_byte(void)
{
    uint8_t byte = 0;
    for (int i = 0; i < 8; i++) {
        byte |= (ow_read_bit_slot() << i);
    }
    return byte;
}

// Maxim DOW CRC (Poly x^8 + x^5 + x^4 + 1).
static uint8_t ow_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t byte = data[i];
        for (int j = 0; j < 8; j++) {
            uint8_t mix = (crc ^ byte) & 0x01;
            crc >>= 1;
            if (mix) {
                crc ^= 0x8C;
            }
            byte >>= 1;
        }
    }
    return crc;
}

// --- DS18B20 high-level ---

// DS18B20-Befehle
#define DS18B20_CMD_SKIP_ROM      0xCC
#define DS18B20_CMD_CONVERT_T     0x44
#define DS18B20_CMD_READ_SCRATCH  0xBE

// Startet eine Temperaturwandlung (Skip ROM: genau 1 Sensor am Bus).
static esp_err_t ds_convert(void)
{
    if (!ow_reset()) {
        return ESP_ERR_NOT_FOUND;
    }
    ow_write_byte(DS18B20_CMD_SKIP_ROM);
    ow_write_byte(DS18B20_CMD_CONVERT_T);
    return ESP_OK;
}

// Liest das 9-Byte-Scratchpad und gibt die Temperatur in Grad zurueck.
// Prueft CRC byte 8 ueber bytes 0-7.
static esp_err_t ds_read_temp(float *out_temp)
{
    if (!ow_reset()) {
        return ESP_ERR_NOT_FOUND;
    }
    ow_write_byte(DS18B20_CMD_SKIP_ROM);
    ow_write_byte(DS18B20_CMD_READ_SCRATCH);

    uint8_t sp[9];
    for (int i = 0; i < 9; i++) {
        sp[i] = ow_read_byte();
    }
    if (ow_crc8(sp, 8) != sp[8]) {
        ESP_LOGW(TAG, "CRC mismatch");
        return ESP_ERR_INVALID_CRC;
    }
    int16_t raw = (int16_t)((sp[1] << 8) | sp[0]);
    *out_temp = (float)raw / 16.0f;
    return ESP_OK;
}

// --- Timer-Callback ---

static void measure_cb(void *arg)
{
    esp_err_t err = ds_convert();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "convert failed: %s", esp_err_to_name(err));
        atomic_store(&s_valid, false);
        esp_timer_start_once(s_timer, MEASURE_INTERVAL_US);
        return;
    }
    // 12-Bit-Wandlung abwarten — wir laufen im esp_timer-Task.
    vTaskDelay(pdMS_TO_TICKS(CONVERT_WAIT_MS));

    float temp;
    err = ds_read_temp(&temp);
    if (err != ESP_OK) {
        atomic_store(&s_valid, false);
        esp_timer_start_once(s_timer, MEASURE_INTERVAL_US);
        return;
    }
    atomic_store(&s_milli_celsius, (int)(temp * 1000.0f));
    atomic_store(&s_valid, true);
    ESP_LOGI(TAG, "%.1f degC", temp);
    esp_timer_start_once(s_timer, MEASURE_INTERVAL_US);
}

// --- Public API ---

esp_err_t temperature_init(void)
{
    atomic_store(&s_valid, false);

    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << TEMP_ONEWIRE_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE, // externer 4,7k Pullup
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    // Pruefen, ob ein Sensor vorhanden ist.
    if (!ow_reset()) {
        ESP_LOGW(TAG, "no DS18B20 presence on GPIO %d", TEMP_ONEWIRE_GPIO);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "DS18B20 detected on GPIO %d", TEMP_ONEWIRE_GPIO);

    // Erste Messung synchron im Init: Wandlung auslösen, kurz warten,
    // sofort lesen — so ist der Wert beim ersten User-Command bereits
    // gueltig (kein "Sensor not ready" nach dem Boot/Charakter-Wechsel).
    err = ds_convert();
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(CONVERT_WAIT_MS));
        float temp;
        if (ds_read_temp(&temp) == ESP_OK) {
            atomic_store(&s_milli_celsius, (int)(temp * 1000.0f));
            atomic_store(&s_valid, true);
            ESP_LOGI(TAG, "initial: %.1f degC", temp);
        }
    }

    esp_timer_create_args_t ta = {
        .callback = measure_cb,
        .name     = "temp_measure",
    };
    err = esp_timer_create(&ta, &s_timer);
    if (err != ESP_OK) {
        return err;
    }
    return esp_timer_start_once(s_timer, MEASURE_INTERVAL_US);
}

float temperature_get(void)
{
    return (float)atomic_load(&s_milli_celsius) / 1000.0f;
}

bool temperature_valid(void)
{
    return atomic_load(&s_valid);
}
