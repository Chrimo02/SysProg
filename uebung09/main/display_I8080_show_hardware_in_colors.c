/**
 * display_I8080.c
 * ILI9341 2.4" TFT über 8-Bit Intel-8080-Interface
 * ESP32-S3-DevKitC-1 | ESP-IDF v5.x
 *
 * Verdrahtung:
 *   Shield      ESP32-S3
 *   ───────────────────────
 *   VCC    →   3V3
 *   GND    →   GND
 *   5V     →   5V       (Backlight-Versorgung!)
 *   LCD_RST→   GPIO46
 *   LCD_CS →   GPIO45
 *   LCD_RS →   GPIO9    (Data/Command)
 *   LCD_WR →   GPIO8    (Write-Strobe)
 *   LCD_RD →   GPIO7    (permanent HIGH, kein Lesen)
 *   LCD_D0 →   GPIO10
 *   LCD_D1 →   GPIO11
 *   LCD_D2 →   GPIO12
 *   LCD_D3 →   GPIO13
 *   LCD_D4 →   GPIO14
 *   LCD_D5 →   GPIO21
 *   LCD_D6 →   GPIO47
 *   LCD_D7 →   GPIO48
 *
 * CMakeLists.txt (main/):
 *   idf_component_register(
 *       SRCS "display_I8080.c"
 *       INCLUDE_DIRS "."
 *       REQUIRES esp_lcd driver
 *   )
 *
 * Keine externen Managed Components nötig!
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "font_8x16.h"   // 8×16 VGA Bitmap-Font, ASCII 32–126

static const char *TAG = "ILI9341";

// ── Pin-Definitionen ──────────────────────────────────────────────
#define LCD_RST     46
#define LCD_CS      45
#define LCD_RS       9   // Data/Command (DC)
#define LCD_WR       8   // Write-Strobe
#define LCD_RD       7   // Read (permanent HIGH)

#define LCD_D0      10
#define LCD_D1      11
#define LCD_D2      12
#define LCD_D3      13
#define LCD_D4      14
#define LCD_D5      21
#define LCD_D6      47
#define LCD_D7      48

// ── Display-Auflösung ─────────────────────────────────────────────
#define LCD_H_RES   240
#define LCD_V_RES   320

// ── ILI9341 Befehlskonstanten ─────────────────────────────────────
#define ILI9341_SWRESET  0x01
#define ILI9341_SLPOUT   0x11
#define ILI9341_COLMOD   0x3A
#define ILI9341_MADCTL   0x36
#define ILI9341_CASET    0x2A
#define ILI9341_RASET    0x2B
#define ILI9341_RAMWR    0x2C
#define ILI9341_DISPON   0x29

// ── RGB565 Farben (BGR-kompensiert für ILI9341 BGR-Modus) ─────────
// Im BGR-Modus des ILI9341 sind die Rot- und Blau-Bits vertauscht.
// Wir schreiben die Konstanten daher mit getauschten R/B-Werten,
// sodass auf dem Display die erwartete Farbe erscheint.
#define COLOR_WHITE  0xFFFF   // R=31 G=63 B=31 → bleibt weiß
#define COLOR_BLACK  0x0000   // → bleibt schwarz
#define COLOR_RED    0x001F   // BGR: B=0  G=0  R=31 → erscheint rot
#define COLOR_GREEN  0x07E0   // BGR: B=0  G=63 R=0  → bleibt grün
#define COLOR_BLUE   0xF800   // BGR: B=31 G=0  R=0  → erscheint blau
#define COLOR_YELLOW 0x07FF   // → erscheint gelb
#define COLOR_CYAN   0xFFE0   // → erscheint cyan

static esp_lcd_panel_io_handle_t io_handle = NULL;

// ── Adressfenster setzen (x0,y0) → exklusiv (x1,y1) ─────────────
static void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t col[4] = {
        (x0 >> 8) & 0xFF, x0 & 0xFF,
        ((x1 - 1) >> 8) & 0xFF, (x1 - 1) & 0xFF,
    };
    esp_lcd_panel_io_tx_param(io_handle, ILI9341_CASET, col, 4);

    uint8_t row[4] = {
        (y0 >> 8) & 0xFF, y0 & 0xFF,
        ((y1 - 1) >> 8) & 0xFF, (y1 - 1) & 0xFF,
    };
    esp_lcd_panel_io_tx_param(io_handle, ILI9341_RASET, row, 4);
}

// ── Bildschirm mit einer Farbe füllen ─────────────────────────────
static void lcd_fill(uint16_t color)
{
    uint32_t n = LCD_H_RES * LCD_V_RES;
    uint16_t *buf = heap_caps_malloc(n * 2, MALLOC_CAP_DMA);
    if (!buf) { ESP_LOGE(TAG, "OOM"); return; }
    for (uint32_t i = 0; i < n; i++) buf[i] = color;
    lcd_set_window(0, 0, LCD_H_RES, LCD_V_RES);
    esp_lcd_panel_io_tx_color(io_handle, ILI9341_RAMWR, buf, n * 2);
    free(buf);
}

// ── Einzelnes Zeichen zeichnen ────────────────────────────────────
// x, y = obere linke Ecke in Pixeln
// fg   = Vordergrundfarbe (RGB565)
// bg   = Hintergrundfarbe (RGB565)
static void lcd_draw_char(uint16_t x, uint16_t y, char c,
                          uint16_t fg, uint16_t bg)
{
    if (c < FONT_FIRST || c > FONT_LAST) c = '?';

    const uint8_t *glyph = font_8x16[c - FONT_FIRST];

    // Puffer für ein Zeichen: 8×16 Pixel
    uint16_t buf[FONT_W * FONT_H];

    for (int row = 0; row < FONT_H; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < FONT_W; col++) {
            // Bit7 = linke Spalte
            buf[row * FONT_W + col] = (bits & (0x80 >> col)) ? fg : bg;
        }
    }

    lcd_set_window(x, y, x + FONT_W, y + FONT_H);
    esp_lcd_panel_io_tx_color(io_handle, ILI9341_RAMWR,
                              buf, sizeof(buf));
}

// ── String ab Position (x, y) ausgeben ───────────────────────────
// Zeilenumbruch bei '\n' oder wenn der Rand erreicht wird
static void lcd_draw_string(uint16_t x, uint16_t y, const char *str,
                             uint16_t fg, uint16_t bg)
{
    uint16_t cx = x;
    uint16_t cy = y;

    while (*str) {
        if (*str == '\n') {
            cx = x;
            cy += FONT_H;
        } else {
            if (cx + FONT_W > LCD_H_RES) {   // Zeilenumbruch am Rand
                cx = x;
                cy += FONT_H;
            }
            lcd_draw_char(cx, cy, *str, fg, bg);
            cx += FONT_W;
        }
        str++;
    }
}

// ── ILI9341 Initialisierungssequenz ───────────────────────────────
static void ili9341_init(void)
{
    gpio_set_level(LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    esp_lcd_panel_io_tx_param(io_handle, ILI9341_SWRESET, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(100));

    esp_lcd_panel_io_tx_param(io_handle, ILI9341_SLPOUT, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(120));

    uint8_t colmod = 0x55;   // RGB565
    esp_lcd_panel_io_tx_param(io_handle, ILI9341_COLMOD, &colmod, 1);

    uint8_t madctl = 0x48;   // MX | BGR → Portrait, 0,0 oben links
    esp_lcd_panel_io_tx_param(io_handle, ILI9341_MADCTL, &madctl, 1);

    esp_lcd_panel_io_tx_param(io_handle, ILI9341_DISPON, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(20));

    ESP_LOGI(TAG, "ILI9341 initialisiert");
}

// ── I8080-Bus und Panel-IO einrichten ─────────────────────────────
static void lcd_bus_init(void)
{
    gpio_set_direction(LCD_RD, GPIO_MODE_OUTPUT);
    gpio_set_level(LCD_RD, 1);

    gpio_set_direction(LCD_RST, GPIO_MODE_OUTPUT);
    gpio_set_level(LCD_RST, 1);

    esp_lcd_i80_bus_handle_t i80_bus = NULL;
    esp_lcd_i80_bus_config_t bus_cfg = {
        .clk_src        = LCD_CLK_SRC_DEFAULT,
        .wr_gpio_num    = LCD_WR,
        .dc_gpio_num    = LCD_RS,
        .data_gpio_nums = {
            LCD_D0, LCD_D1, LCD_D2, LCD_D3,
            LCD_D4, LCD_D5, LCD_D6, LCD_D7,
        },
        .bus_width           = 8,
        .max_transfer_bytes  = LCD_H_RES * LCD_V_RES * 2 + 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_i80_bus(&bus_cfg, &i80_bus));
    ESP_LOGI(TAG, "I8080-Bus initialisiert");

    esp_lcd_panel_io_i80_config_t io_cfg = {
        .cs_gpio_num       = LCD_CS,
        .pclk_hz           = 20 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits      = 8,
        .lcd_param_bits    = 8,
        .dc_levels = {
            .dc_idle_level  = 0,
            .dc_cmd_level   = 0,
            .dc_dummy_level = 0,
            .dc_data_level  = 1,
        },
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i80(i80_bus, &io_cfg, &io_handle));
    ESP_LOGI(TAG, "Panel-IO erstellt");
}

// ── Einstiegspunkt ────────────────────────────────────────────────
void app_main(void)
{
    ESP_LOGI(TAG, "Starte ILI9341 Display");

    lcd_bus_init();
    ili9341_init();

    // Schwarzer Hintergrund
    lcd_fill(COLOR_BLACK);

    // Hello World
    lcd_draw_string(10, 10,  "Hello, World!",  COLOR_WHITE,  COLOR_BLACK);

    // Weitere Zeilen
    lcd_draw_string(10, 40,  "ESP32-S3",       COLOR_YELLOW, COLOR_BLACK);
    lcd_draw_string(10, 60,  "ILI9341 I8080",  COLOR_CYAN,   COLOR_BLACK);
    lcd_draw_string(10, 80,  "RGB565 Font",    COLOR_GREEN,  COLOR_BLACK);

    // Mehrzeiliger Text mit \n
    lcd_draw_string(10, 120,
        "Zeile 1\nZeile 2\nZeile 3",
        COLOR_RED, COLOR_BLACK);

    ESP_LOGI(TAG, "Text gezeichnet");
}