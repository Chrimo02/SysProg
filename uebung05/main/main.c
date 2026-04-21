#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "rom/ets_sys.h"

#define OW_PIN GPIO_NUM_4
#define TAG "DS18X20"

// Commands
#define CMD_READ_ROM        0x33
#define CMD_SKIP_ROM        0xCC
#define CMD_CONVERT_T       0x44
#define CMD_READ_SCRATCHPAD 0xBE

// Timing
static inline void delay_us(uint32_t us) { ets_delay_us(us); }

static void ow_low(void) {
    gpio_set_direction(OW_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(OW_PIN, 0);
}

static void ow_release(void) {
    gpio_set_direction(OW_PIN, GPIO_MODE_INPUT);
}

static int ow_read(void) {
    return gpio_get_level(OW_PIN);
}

static int ow_reset(void) {
    int presence;
    ow_low();
    delay_us(480);
    ow_release();
    delay_us(70);
    presence = !ow_read();
    delay_us(410);
    return presence;
}

static void ow_write_bit(int bit) {
    if (bit) {
        ow_low();
        delay_us(6);
        ow_release();
        delay_us(64);
    } else {
        ow_low();
        delay_us(60);
        ow_release();
        delay_us(10);
    }
}

static int ow_read_bit(void) {
    int bit;
    ow_low();
    delay_us(6);
    ow_release();
    delay_us(9);
    bit = ow_read();
    delay_us(55);
    return bit;
}

static void ow_write_byte(uint8_t data) {
    for (int i = 0; i < 8; i++) {
        ow_write_bit(data & 1);
        data >>= 1;
    }
}

static uint8_t ow_read_byte(void) {
    uint8_t data = 0;
    for (int i = 0; i < 8; i++) {
        data >>= 1;
        if (ow_read_bit()) data |= 0x80;
    }
    return data;
}

void app_main(void) {
    uint8_t rom[8];
    uint8_t scratchpad[9];
    
    // GPIO init
    gpio_reset_pin(OW_PIN);
    gpio_set_direction(OW_PIN, GPIO_MODE_INPUT);
    
    ESP_LOGI(TAG, "Starting DS18x20 example");
    
    // Aufgabe d) ROM Code auslesen
    if (!ow_reset()) {
        ESP_LOGE(TAG, "No device found!");
        return;
    }
    
    ow_write_byte(CMD_READ_ROM);
    for (int i = 0; i < 8; i++) {
        rom[i] = ow_read_byte();
    }
    
    printf("ROM Code: ");
    for (int i = 0; i < 8; i++) {
        printf("%02X", rom[i]);
    }
    printf("\n");
    
    // Aufgabe e) Temperatur auslesen
    while (1) {
        // CONVERT T
        if (!ow_reset()) continue;
        ow_write_byte(CMD_SKIP_ROM);
        ow_write_byte(CMD_CONVERT_T);
        vTaskDelay(pdMS_TO_TICKS(750));
        
        // READ SCRATCHPAD
        if (!ow_reset()) continue;
        ow_write_byte(CMD_SKIP_ROM);
        ow_write_byte(CMD_READ_SCRATCHPAD);
        for (int i = 0; i < 9; i++) {
            scratchpad[i] = ow_read_byte();
        }
        
        // Temperatur berechnen
        int16_t raw = (scratchpad[1] << 8) | scratchpad[0];
        float temp = (float)raw / 16.0f;
        printf("Temperature: %.2f °C\n", temp);
        
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}