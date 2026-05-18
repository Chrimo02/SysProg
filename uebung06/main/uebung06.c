#include <stdio.h>
#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "EEPROM";

// ── Hardware-Konfiguration ─────────────────────────────────────────────────
#define I2C_MASTER_NUM      I2C_NUM_0   // Welcher I2C-Bus des ESP32 genutzt wird
#define I2C_MASTER_SDA_IO   8           // GPIO8 = SDA
#define I2C_MASTER_SCL_IO   9           // GPIO9 = SCL
#define I2C_MASTER_FREQ_HZ  100000      // 100 kHz = Standard Mode

// ── AT24C08 Konfiguration ──────────────────────────────────────────────────
// A2 = GND → obere Adressbits = 000
// Device-Adresse (7-Bit, ohne R/W): 0b1010000 = 0x50
#define EEPROM_ADDR         0x50

// Timeouts
#define I2C_TIMEOUT_MS      1000
#define EEPROM_WRITE_DELAY_MS 10   // AT24C08 braucht max. 5ms nach einem Write


// ── I2C Bus initialisieren ─────────────────────────────────────────────────
static void i2c_master_init(void)
{
    i2c_config_t conf = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = I2C_MASTER_SDA_IO,
        .scl_io_num       = I2C_MASTER_SCL_IO,
        .sda_pullup_en    = GPIO_PULLUP_DISABLE,  // Externe 4.7kΩ vorhanden
        .scl_pullup_en    = GPIO_PULLUP_DISABLE,  // Externe 4.7kΩ vorhanden
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };
    ESP_ERROR_CHECK(i2c_param_config(I2C_MASTER_NUM, &conf));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_MASTER_NUM, I2C_MODE_MASTER, 0, 0, 0));
    ESP_LOGI(TAG, "I2C initialisiert (SDA=GPIO%d, SCL=GPIO%d, %d Hz)",
             I2C_MASTER_SDA_IO, I2C_MASTER_SCL_IO, I2C_MASTER_FREQ_HZ);
}


// ── Ein einzelnes Byte ins EEPROM schreiben ────────────────────────────────
//
// Ablauf auf dem Bus:
//   START → [EEPROM_ADDR | W] → ACK → [word_addr] → ACK → [data] → ACK → STOP
//
// Der AT24C08 hat 1024 Byte, also 10-Bit-Adressen.
// Die oberen 2 Bit der Wortadresse stecken in den Bits P1:P0 der Device-Adresse.
// Beispiel: Wortadresse 0x1FF → P1=1, P0=1 → Device-Addr = 0x50 | (0x1FF >> 8) = 0x51
//           Wortadresse 0x000 → P1=0, P0=0 → Device-Addr = 0x50
//
static esp_err_t eeprom_byte_write(uint16_t word_addr, uint8_t data)
{
    // Obere 2 Bit der Wortadresse in die Device-Adresse einbauen
    uint8_t dev_addr = EEPROM_ADDR | ((word_addr >> 8) & 0x03);
    // Untere 8 Bit der Wortadresse als Adress-Byte
    uint8_t addr_low = word_addr & 0xFF;

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (dev_addr << 1) | I2C_MASTER_WRITE, true); // true = ACK prüfen
    i2c_master_write_byte(cmd, addr_low, true);
    i2c_master_write_byte(cmd, data, true);
    i2c_master_stop(cmd);

    esp_err_t ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);

    if (ret == ESP_OK) {
        // Warten bis der interne Schreibzyklus fertig ist (max. 5ms laut Datasheet)
        vTaskDelay(pdMS_TO_TICKS(EEPROM_WRITE_DELAY_MS));
        ESP_LOGI(TAG, "Write: Adresse 0x%03X ← 0x%02X ('%c')", word_addr, data, data);
    } else {
        ESP_LOGE(TAG, "Write FEHLER an Adresse 0x%03X: %s", word_addr, esp_err_to_name(ret));
    }
    return ret;
}


// ── Ein einzelnes Byte aus dem EEPROM lesen (Random Read) ─────────────────
//
// Ablauf auf dem Bus (Random Read = Dummy Write + Current Address Read):
//   START → [EEPROM_ADDR | W] → ACK → [word_addr] → ACK
//   START → [EEPROM_ADDR | R] → ACK → ← [data] → NACK → STOP
//
static esp_err_t eeprom_byte_read(uint16_t word_addr, uint8_t *data)
{
    uint8_t dev_addr = EEPROM_ADDR | ((word_addr >> 8) & 0x03);
    uint8_t addr_low = word_addr & 0xFF;

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();

    // Dummy Write: Nur um die interne Adresse des EEPROMs zu setzen
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (dev_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, addr_low, true);

    // Repeated Start + Lesen
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (dev_addr << 1) | I2C_MASTER_READ, true);
    i2c_master_read_byte(cmd, data, I2C_MASTER_NACK); // NACK = letztes Byte, danach STOP
    i2c_master_stop(cmd);

    esp_err_t ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Read:  Adresse 0x%03X → 0x%02X ('%c')", word_addr, *data, *data);
    } else {
        ESP_LOGE(TAG, "Read FEHLER an Adresse 0x%03X: %s", word_addr, esp_err_to_name(ret));
    }
    return ret;
}


// ── Hauptprogramm ──────────────────────────────────────────────────────────
void app_main(void)
{
    i2c_master_init();

    // Kurz warten bis alles stabil
    vTaskDelay(pdMS_TO_TICKS(100));

    // Test-Daten: Wir schreiben ein paar Bytes und lesen sie zurück
    typedef struct {
        uint16_t addr;
        uint8_t  value;
    } test_entry_t;

    test_entry_t tests[] = {
        { 0x000, 'H' },
        { 0x001, 'e' },
        { 0x002, 'l' },
        { 0x003, 'l' },
        { 0x004, 'o' },
    };
    int num_tests = sizeof(tests) / sizeof(tests[0]);

    // ── Schreiben ──────────────────────────────────────────────────────────
    ESP_LOGI(TAG, "=== Schreibe %d Bytes ins EEPROM ===", num_tests);
    for (int i = 0; i < num_tests; i++) {
        eeprom_byte_write(tests[i].addr, tests[i].value);
    }

    // ── Lesen ──────────────────────────────────────────────────────────────
    ESP_LOGI(TAG, "=== Lese %d Bytes aus dem EEPROM ===", num_tests);
    uint8_t read_val;
    for (int i = 0; i < num_tests; i++) {
        if (eeprom_byte_read(tests[i].addr, &read_val) == ESP_OK) {
            // Vergleich: stimmt das Gelesene mit dem Geschriebenen überein?
            if (read_val == tests[i].value) {
                ESP_LOGI(TAG, "✓ Adresse 0x%03X: OK (0x%02X)", tests[i].addr, read_val);
            } else {
                ESP_LOGE(TAG, "✗ Adresse 0x%03X: MISMATCH (erwartet 0x%02X, gelesen 0x%02X)",
                         tests[i].addr, tests[i].value, read_val);
            }
        }
    }

    ESP_LOGI(TAG, "=== Fertig ===");
}