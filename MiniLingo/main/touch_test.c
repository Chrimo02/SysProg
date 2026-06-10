#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "touch_test";

#define TOUCH_HOST    SPI2_HOST

#define PIN_NUM_SCK   GPIO_NUM_12
#define PIN_NUM_MOSI  GPIO_NUM_11
#define PIN_NUM_MISO  GPIO_NUM_13
#define PIN_NUM_T_CS  GPIO_NUM_14

// XPT2046 control byte commands (12-bit mode, single-ended, power on)
#define CMD_READ_X  0x90
#define CMD_READ_Y  0xD0
#define CMD_READ_Z1 0xB0
#define CMD_READ_Z2 0xC0

static spi_device_handle_t touch_spi;

static uint16_t xpt2046_read(uint8_t cmd)
{
    uint8_t tx[3] = {cmd, 0x00, 0x00};
    uint8_t rx[3] = {0};

    spi_transaction_t t = {
        .length = 24,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    ESP_ERROR_CHECK(spi_device_polling_transmit(touch_spi, &t));

    // 12-bit result in bits [14:3] of the 16-bit reply
    return ((rx[1] << 8) | rx[2]) >> 3;
}

void app_main(void)
{
    ESP_LOGI(TAG, "Initializing SPI bus");
    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_NUM_SCK,
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 32,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(TOUCH_HOST, &buscfg, SPI_DMA_DISABLED));

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 2 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = PIN_NUM_T_CS,
        .queue_size = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(TOUCH_HOST, &devcfg, &touch_spi));

    ESP_LOGI(TAG, "Polling XPT2046 touch values...");

    while (1) {
        uint16_t x  = xpt2046_read(CMD_READ_X);
        uint16_t y  = xpt2046_read(CMD_READ_Y);
        uint16_t z1 = xpt2046_read(CMD_READ_Z1);
        uint16_t z2 = xpt2046_read(CMD_READ_Z2);

        // Z1+4095-Z2 is small (large) when pressed (released); use Z1 as a simple touch indicator
        if (z1 > 100) {
            ESP_LOGI(TAG, "Touch: X=%4u Y=%4u Z1=%4u Z2=%4u", x, y, z1, z2);
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
