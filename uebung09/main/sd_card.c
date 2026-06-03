/**
 * SD-Karten Test über SPI
 * Schreibt eine Datei, liest sie zurück, zeigt Inhalt auf dem Display
 *
 * Verdrahtung Display: wie bisher (I8080)
 * Verdrahtung SD-Karte (4 neue Jumper):
 *   SD_SCK → GPIO3
 *   SD_DI  → GPIO4   (MOSI)
 *   SD_DO  → GPIO5   (MISO)
 *   SD_SS  → GPIO6   (CS)
 *
 * Außerdem: microSD-Karte FAT32-formatiert in den Slot stecken
 *
 * CMakeLists.txt:
 *   idf_component_register(
 *       SRCS "display_I8080.c"
 *       INCLUDE_DIRS "."
 *       REQUIRES esp_lcd driver fatfs sdmmc esp_driver_sdspi
 *   )
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "font_8x16.h"

static const char *TAG = "SDTEST";

// Display Pins (wie gehabt)
#define LCD_RST  46
#define LCD_CS   45
#define LCD_RS    9
#define LCD_WR    8
#define LCD_RD    7
#define LCD_D0   10
#define LCD_D1   11
#define LCD_D2   12
#define LCD_D3   13
#define LCD_D4   14
#define LCD_D5   21
#define LCD_D6   47
#define LCD_D7   48

// SD-Karten Pins
#define SD_SCK   3
#define SD_MOSI  4
#define SD_MISO  5
#define SD_CS    6

#define MOUNT_POINT "/sdcard"
#define TEST_FILE   MOUNT_POINT"/hello.txt"

#define LCD_W   240
#define LCD_H   320

// Farben (BGR-kompensiert)
#define COLOR_BLACK   0x0000
#define COLOR_WHITE   0xFFFF
#define COLOR_RED     0x001F
#define COLOR_GREEN   0x07E0
#define COLOR_YELLOW  0x07FF
#define COLOR_CYAN    0xFFE0
#define COLOR_GRAY    0x8410

static esp_lcd_panel_io_handle_t io_handle = NULL;

// ================================================================
// DISPLAY (kompakt)
// ================================================================

static void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t col[4] = { x0>>8, x0&0xFF, (x1-1)>>8, (x1-1)&0xFF };
    uint8_t row[4] = { y0>>8, y0&0xFF, (y1-1)>>8, (y1-1)&0xFF };
    esp_lcd_panel_io_tx_param(io_handle, 0x2A, col, 4);
    esp_lcd_panel_io_tx_param(io_handle, 0x2B, row, 4);
}

static void lcd_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    uint32_t n = w * h;
    uint16_t *buf = heap_caps_malloc(n * 2, MALLOC_CAP_DMA);
    if (!buf) return;
    for (uint32_t i = 0; i < n; i++) buf[i] = color;
    lcd_set_window(x, y, x+w, y+h);
    esp_lcd_panel_io_tx_color(io_handle, 0x2C, buf, n*2);
    free(buf);
}

static void lcd_fill(uint16_t color) { lcd_fill_rect(0, 0, LCD_W, LCD_H, color); }

static void lcd_draw_char(uint16_t x, uint16_t y, char c, uint16_t fg, uint16_t bg)
{
    if (c < FONT_FIRST || c > FONT_LAST) c = '?';
    const uint8_t *g = font_8x16[c - FONT_FIRST];
    uint16_t buf[FONT_W * FONT_H];
    for (int r = 0; r < FONT_H; r++)
        for (int b = 0; b < FONT_W; b++)
            buf[r*FONT_W+b] = (g[r] & (0x80>>b)) ? fg : bg;
    lcd_set_window(x, y, x+FONT_W, y+FONT_H);
    esp_lcd_panel_io_tx_color(io_handle, 0x2C, buf, sizeof(buf));
}

static void lcd_print(uint16_t x, uint16_t y, const char *str, uint16_t fg, uint16_t bg)
{
    uint16_t cx = x, cy = y;
    while (*str) {
        if (*str == '\n') { cx = x; cy += FONT_H; }
        else {
            if (cx + FONT_W > LCD_W) { cx = x; cy += FONT_H; }
            lcd_draw_char(cx, cy, *str, fg, bg);
            cx += FONT_W;
        }
        str++;
    }
}

static void lcd_bus_init(void)
{
    gpio_set_direction(LCD_RD,  GPIO_MODE_OUTPUT); gpio_set_level(LCD_RD,  1);
    gpio_set_direction(LCD_RST, GPIO_MODE_OUTPUT); gpio_set_level(LCD_RST, 1);

    esp_lcd_i80_bus_handle_t bus = NULL;
    esp_lcd_i80_bus_config_t bus_cfg = {
        .clk_src        = LCD_CLK_SRC_DEFAULT,
        .wr_gpio_num    = LCD_WR,
        .dc_gpio_num    = LCD_RS,
        .data_gpio_nums = { LCD_D0, LCD_D1, LCD_D2, LCD_D3,
                            LCD_D4, LCD_D5, LCD_D6, LCD_D7 },
        .bus_width          = 8,
        .max_transfer_bytes = LCD_W * LCD_H * 2 + 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_i80_bus(&bus_cfg, &bus));

    esp_lcd_panel_io_i80_config_t io_cfg = {
        .cs_gpio_num       = LCD_CS,
        .pclk_hz           = 20 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits      = 8,
        .lcd_param_bits    = 8,
        .dc_levels = { .dc_cmd_level = 0, .dc_data_level = 1 },
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i80(bus, &io_cfg, &io_handle));
}

static void ili9341_init(void)
{
    gpio_set_level(LCD_RST, 0); vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(LCD_RST, 1); vTaskDelay(pdMS_TO_TICKS(120));
    esp_lcd_panel_io_tx_param(io_handle, 0x01, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_lcd_panel_io_tx_param(io_handle, 0x11, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(120));
    uint8_t p;
    p = 0x55; esp_lcd_panel_io_tx_param(io_handle, 0x3A, &p, 1);
    p = 0x48; esp_lcd_panel_io_tx_param(io_handle, 0x36, &p, 1);
    esp_lcd_panel_io_tx_param(io_handle, 0x29, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
}

// ================================================================
// SD-KARTE
// ================================================================

static sdmmc_card_t *card = NULL;

static esp_err_t sd_mount(void)
{
    ESP_LOGI(TAG, "SPI-Bus für SD initialisieren...");

    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = SD_MOSI,
        .miso_io_num     = SD_MISO,
        .sclk_io_num     = SD_SCK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 4000,
    };
    esp_err_t ret = spi_bus_initialize(SPI3_HOST, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI-Bus init fehlgeschlagen: %s", esp_err_to_name(ret));
        return ret;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI3_HOST;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = SD_CS;
    slot_config.host_id = SPI3_HOST;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files              = 5,
        .allocation_unit_size   = 16 * 1024,
    };

    ESP_LOGI(TAG, "SD-Karte mounten...");
    ret = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot_config,
                                   &mount_config, &card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Mount fehlgeschlagen: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "SD-Karte gemountet:");
    sdmmc_card_print_info(stdout, card);
    return ESP_OK;
}

static esp_err_t sd_write_test(void)
{
    ESP_LOGI(TAG, "Schreibe %s ...", TEST_FILE);
    FILE *f = fopen(TEST_FILE, "w");
    if (!f) {
        ESP_LOGE(TAG, "fopen (write) fehlgeschlagen");
        return ESP_FAIL;
    }
    fprintf(f, "Hallo von Christians\nESP32-S3!\n");
    fprintf(f, "SPI funktioniert :)\n");
    fclose(f);
    ESP_LOGI(TAG, "Schreiben fertig");
    return ESP_OK;
}

static esp_err_t sd_read_test(char *out_buf, size_t out_size)
{
    ESP_LOGI(TAG, "Lese %s ...", TEST_FILE);
    FILE *f = fopen(TEST_FILE, "r");
    if (!f) {
        ESP_LOGE(TAG, "fopen (read) fehlgeschlagen");
        return ESP_FAIL;
    }
    size_t n = fread(out_buf, 1, out_size - 1, f);
    out_buf[n] = '\0';
    fclose(f);
    ESP_LOGI(TAG, "Inhalt (%d Bytes):\n%s", (int)n, out_buf);
    return ESP_OK;
}

// ================================================================
// MAIN
// ================================================================

void app_main(void)
{
    ESP_LOGI(TAG, "SD-Karten Test startet");

    // 1) Display init
    lcd_bus_init();
    ili9341_init();
    lcd_fill(COLOR_BLACK);

    lcd_print(10, 10, "SD-Karten Test", COLOR_YELLOW, COLOR_BLACK);
    lcd_print(10, 35, "Mounte SD...",   COLOR_WHITE,  COLOR_BLACK);

    // 2) SD mounten
    if (sd_mount() != ESP_OK) {
        lcd_print(10, 60, "MOUNT FEHLER!",  COLOR_RED, COLOR_BLACK);
        lcd_print(10, 80, "Karte gesteckt?", COLOR_GRAY, COLOR_BLACK);
        lcd_print(10, 98, "FAT32?",          COLOR_GRAY, COLOR_BLACK);
        return;
    }
    lcd_print(10, 60, "OK gemountet",  COLOR_GREEN, COLOR_BLACK);

    // 3) Datei schreiben
    lcd_print(10, 85, "Schreibe Datei...", COLOR_WHITE, COLOR_BLACK);
    if (sd_write_test() != ESP_OK) {
        lcd_print(10, 105, "SCHREIB-FEHLER!", COLOR_RED, COLOR_BLACK);
        return;
    }
    lcd_print(10, 105, "OK geschrieben", COLOR_GREEN, COLOR_BLACK);

    // 4) Datei lesen
    lcd_print(10, 130, "Lese Datei...", COLOR_WHITE, COLOR_BLACK);
    char buf[256];
    if (sd_read_test(buf, sizeof(buf)) != ESP_OK) {
        lcd_print(10, 150, "LESE-FEHLER!", COLOR_RED, COLOR_BLACK);
        return;
    }
    lcd_print(10, 150, "OK gelesen", COLOR_GREEN, COLOR_BLACK);

    // 5) Inhalt anzeigen
    lcd_print(10, 180, "Inhalt:",  COLOR_CYAN, COLOR_BLACK);
    lcd_fill_rect(10, 200, LCD_W-20, 2, COLOR_GRAY);
    lcd_print(10, 210, buf, COLOR_WHITE, COLOR_BLACK);

    // 6) Unmount
    esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);
    ESP_LOGI(TAG, "Unmount fertig");
}