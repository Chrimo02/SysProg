#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"
#include "esp_log.h"

static const char *TAG = "display_test";

#define LCD_HOST    SPI2_HOST

#define PIN_NUM_SCK   GPIO_NUM_12
#define PIN_NUM_MOSI  GPIO_NUM_11
#define PIN_NUM_MISO  GPIO_NUM_13
#define PIN_NUM_CS    GPIO_NUM_10
#define PIN_NUM_DC    GPIO_NUM_8
#define PIN_NUM_RST   GPIO_NUM_9

#define LCD_H_RES   240
#define LCD_V_RES   320
#define LCD_PIXEL_CLOCK_HZ (20 * 1000 * 1000)

// RGB565 colors
#define COLOR_RED   0xF800
#define COLOR_GREEN 0x07E0
#define COLOR_BLUE  0x001F
#define COLOR_WHITE 0xFFFF

static void fill_screen(esp_lcd_panel_handle_t panel, uint16_t color)
{
    static uint16_t line[LCD_H_RES];
    for (int i = 0; i < LCD_H_RES; i++) {
        line[i] = color;
    }
    for (int y = 0; y < LCD_V_RES; y++) {
        esp_lcd_panel_draw_bitmap(panel, 0, y, LCD_H_RES, y + 1, line);
    }
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
        .max_transfer_sz = LCD_H_RES * 80 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    ESP_LOGI(TAG, "Installing panel IO");
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = PIN_NUM_DC,
        .cs_gpio_num = PIN_NUM_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle));

    ESP_LOGI(TAG, "Installing ILI9341 panel driver");
    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_NUM_RST,
        .color_space = ESP_LCD_COLOR_SPACE_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io_handle, &panel_config, &panel_handle));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    const uint16_t colors[] = {COLOR_RED, COLOR_GREEN, COLOR_BLUE, COLOR_WHITE};
    const char *names[] = {"Rot", "Gruen", "Blau", "Weiss"};

    while (1) {
        for (int i = 0; i < 4; i++) {
            ESP_LOGI(TAG, "Fuelle Bildschirm: %s", names[i]);
            fill_screen(panel_handle, colors[i]);
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }
}
