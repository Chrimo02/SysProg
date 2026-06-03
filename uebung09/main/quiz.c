/**
 * Quiz mit physischen Tastern
 * ILI9341 2.4" TFT + 2 Push-Buttons | ESP32-S3 | ESP-IDF v5.x
 *
 * Verdrahtung Display (I8080):
 *   VCC    → 3V3
 *   GND    → GND
 *   5V     → 5V        (Backlight)
 *   LCD_RST→ GPIO46
 *   LCD_CS → GPIO45
 *   LCD_RS → GPIO9
 *   LCD_WR → GPIO8
 *   LCD_RD → GPIO7
 *   LCD_D0 → GPIO10
 *   LCD_D1 → GPIO11
 *   LCD_D2 → GPIO12
 *   LCD_D3 → GPIO13
 *   LCD_D4 → GPIO14
 *   LCD_D5 → GPIO21
 *   LCD_D6 → GPIO47
 *   LCD_D7 → GPIO48
 *
 * Verdrahtung Taster:
 *   Taster JA   zwischen GPIO1  und GND
 *   Taster NEIN zwischen GPIO2  und GND
 *   (interne Pull-Ups werden aktiviert, also kein externer Widerstand nötig)
 *
 * Alle Touch-Brücken können entfernt werden!
 */

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "font_8x16.h"

static const char *TAG = "QUIZ";

// Display Pins
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

// Button Pins (aktiv LOW, interne Pull-Ups)
#define BTN_JA_GPIO    1
#define BTN_NEIN_GPIO  2

#define LCD_W   240
#define LCD_H   320

// Farben (BGR-kompensiert)
#define COLOR_BLACK   0x0000
#define COLOR_WHITE   0xFFFF
#define COLOR_RED     0x001F
#define COLOR_GREEN   0x07E0
#define COLOR_YELLOW  0x07FF
#define COLOR_GRAY    0x8410

// Button-Layout auf dem Display
#define UI_JA_X     20
#define UI_JA_Y    230
#define UI_JA_W     90
#define UI_JA_H     50
#define UI_NEIN_X  130
#define UI_NEIN_Y  230
#define UI_NEIN_W   90
#define UI_NEIN_H   50

static esp_lcd_panel_io_handle_t io_handle = NULL;

// ================================================================
// DISPLAY
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
        else { lcd_draw_char(cx, cy, *str, fg, bg); cx += FONT_W; }
        str++;
    }
}

static void lcd_print_centered(uint16_t rx, uint16_t ry, uint16_t rw, uint16_t rh,
                                const char *str, uint16_t fg, uint16_t bg)
{
    uint16_t sw = strlen(str) * FONT_W;
    uint16_t x = rx + (rw > sw ? (rw-sw)/2 : 0);
    uint16_t y = ry + (rh > FONT_H ? (rh-FONT_H)/2 : 0);
    lcd_print(x, y, str, fg, bg);
}

static void draw_button(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                         const char *label, uint16_t bg, uint16_t fg)
{
    lcd_fill_rect(x, y, w, h, bg);
    lcd_fill_rect(x,     y,     w, 2, COLOR_WHITE);
    lcd_fill_rect(x,     y+h-2, w, 2, COLOR_WHITE);
    lcd_fill_rect(x,     y,     2, h, COLOR_WHITE);
    lcd_fill_rect(x+w-2, y,     2, h, COLOR_WHITE);
    lcd_print_centered(x, y, w, h, label, fg, bg);
}

// ================================================================
// BUTTONS
// ================================================================

static void buttons_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << BTN_JA_GPIO) | (1ULL << BTN_NEIN_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    ESP_LOGI(TAG, "Taster initialisiert (GPIO%d=JA, GPIO%d=NEIN)",
             BTN_JA_GPIO, BTN_NEIN_GPIO);
}

// Wartet auf einen Tastendruck mit Entprellung.
// Liefert "JA" oder "NEIN" zurück.
static const char *button_wait_for_press(void)
{
    while (1) {
        bool ja_down   = (gpio_get_level(BTN_JA_GPIO)   == 0);
        bool nein_down = (gpio_get_level(BTN_NEIN_GPIO) == 0);

        if (ja_down || nein_down) {
            // Entprellung: 30ms warten, dann nochmal prüfen
            vTaskDelay(pdMS_TO_TICKS(30));
            ja_down   = (gpio_get_level(BTN_JA_GPIO)   == 0);
            nein_down = (gpio_get_level(BTN_NEIN_GPIO) == 0);
            if (ja_down)   return "JA";
            if (nein_down) return "NEIN";
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// Wartet bis beide Tasten losgelassen sind
static void button_wait_for_release(void)
{
    while (gpio_get_level(BTN_JA_GPIO) == 0 ||
           gpio_get_level(BTN_NEIN_GPIO) == 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    vTaskDelay(pdMS_TO_TICKS(50));  // Entprellung
}

// ================================================================
// DISPLAY INIT
// ================================================================

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
    ESP_LOGI(TAG, "Display-Bus initialisiert");
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
    ESP_LOGI(TAG, "ILI9341 bereit");
}

// ================================================================
// QUIZ
// ================================================================

static void draw_dialog(const char *frage)
{
    lcd_fill(COLOR_BLACK);
    lcd_print_centered(0, 10, LCD_W, FONT_H, "Quiz!", COLOR_YELLOW, COLOR_BLACK);
    lcd_fill_rect(10, 30, LCD_W-20, 2, COLOR_GRAY);
    lcd_print(10, 50, frage, COLOR_WHITE, COLOR_BLACK);
    draw_button(UI_JA_X,   UI_JA_Y,   UI_JA_W,   UI_JA_H,   "JA",   COLOR_GREEN, COLOR_BLACK);
    draw_button(UI_NEIN_X, UI_NEIN_Y, UI_NEIN_W, UI_NEIN_H, "NEIN", COLOR_RED,   COLOR_WHITE);
}

static void draw_ergebnis(bool korrekt, int punkte, int total)
{
    lcd_fill(COLOR_BLACK);
    if (korrekt) {
        lcd_print_centered(0, 80,  LCD_W, FONT_H, "RICHTIG!", COLOR_GREEN, COLOR_BLACK);
        lcd_print_centered(0, 110, LCD_W, FONT_H, ":)",       COLOR_GREEN, COLOR_BLACK);
    } else {
        lcd_print_centered(0, 80,  LCD_W, FONT_H, "FALSCH!",  COLOR_RED, COLOR_BLACK);
        lcd_print_centered(0, 110, LCD_W, FONT_H, ":(",       COLOR_RED, COLOR_BLACK);
    }

    char score[32];
    snprintf(score, sizeof(score), "%d / %d Punkte", punkte, total);
    lcd_print_centered(0, 160, LCD_W, FONT_H, score, COLOR_WHITE, COLOR_BLACK);

    lcd_print_centered(0, 220, LCD_W, FONT_H, "Druecke eine Taste", COLOR_GRAY, COLOR_BLACK);
    lcd_print_centered(0, 238, LCD_W, FONT_H, "fuer naechste Frage", COLOR_GRAY, COLOR_BLACK);
}

typedef struct { const char *frage; const char *antwort; } Quiz_t;

static const Quiz_t fragen[] = {
    { "Ist die Erde\nrund?",                        "JA"   },
    { "Ist 7 eine\ngerade Zahl?",                   "NEIN" },
    { "Hat ein Dreieck\n3 Ecken?",                  "JA"   },
    { "Ist Paris\nHauptstadt von\nDeutschland?",    "NEIN" },
    { "Ist H2O die\nFormel von Wasser?",            "JA"   },
};
#define FRAGEN_ANZ (sizeof(fragen)/sizeof(fragen[0]))

// ================================================================
// MAIN
// ================================================================

void app_main(void)
{
    ESP_LOGI(TAG, "Start Quiz");
    lcd_bus_init();
    ili9341_init();
    buttons_init();

    int idx = 0, punkte = 0;

    while (1) {
        const Quiz_t *q = &fragen[idx % FRAGEN_ANZ];
        draw_dialog(q->frage);

        const char *antwort = button_wait_for_press();
        ESP_LOGI(TAG, "Gedrueckt: %s", antwort);

        // Button visuell hervorheben
        if (strcmp(antwort, "JA") == 0) {
            draw_button(UI_JA_X, UI_JA_Y, UI_JA_W, UI_JA_H,
                        "JA", COLOR_WHITE, COLOR_BLACK);
        } else {
            draw_button(UI_NEIN_X, UI_NEIN_Y, UI_NEIN_W, UI_NEIN_H,
                        "NEIN", COLOR_WHITE, COLOR_BLACK);
        }

        bool korrekt = (strcmp(antwort, q->antwort) == 0);
        if (korrekt) punkte++;
        ESP_LOGI(TAG, "%s | %s | %d/%d",
                 antwort, korrekt ? "RICHTIG" : "FALSCH", punkte, idx+1);

        vTaskDelay(pdMS_TO_TICKS(500));
        button_wait_for_release();

        draw_ergebnis(korrekt, punkte, idx + 1);

        button_wait_for_press();
        button_wait_for_release();

        idx++;
    }
}