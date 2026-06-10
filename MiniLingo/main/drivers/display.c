// ILI9341-Treiber (abgeleitet aus display_test.c) + LVGL-v9-Anbindung.
#include "display.h"

#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"

static const char *TAG = "display";

#define LCD_HOST    SPI2_HOST

#define PIN_NUM_SCK   GPIO_NUM_12
#define PIN_NUM_MOSI  GPIO_NUM_11
#define PIN_NUM_MISO  GPIO_NUM_13
#define PIN_NUM_CS    GPIO_NUM_10
#define PIN_NUM_DC    GPIO_NUM_8
#define PIN_NUM_RST   GPIO_NUM_9

#define LCD_PIXEL_CLOCK_HZ (20 * 1000 * 1000)

// LVGL rendert in zwei Teilpuffern von je 40 Zeilen (Partial-Mode).
#define LVGL_BUF_LINES 40

static lv_display_t *s_disp;
static esp_lcd_panel_handle_t s_panel;

static bool notify_flush_ready(esp_lcd_panel_io_handle_t io,
                               esp_lcd_panel_io_event_data_t *edata,
                               void *user_ctx)
{
    lv_display_flush_ready(s_disp);
    return false;
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    // ILI9341 erwartet RGB565 big-endian; LVGL v9 hat kein
    // LV_COLOR_16_SWAP mehr, daher Swap hier im Flush-Pfad.
    lv_draw_sw_rgb565_swap(px_map, lv_area_get_size(area));
    esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1, px_map);
    // lv_display_flush_ready() kommt asynchron aus notify_flush_ready().
}

esp_err_t display_init(void)
{
    ESP_LOGI(TAG, "Initializing SPI bus");
    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_NUM_SCK,
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = DISPLAY_H_RES * LVGL_BUF_LINES * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO),
                        TAG, "spi_bus_initialize failed");

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
        .on_color_trans_done = notify_flush_ready,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST,
                                                 &io_config, &io_handle),
                        TAG, "panel io failed");

    ESP_LOGI(TAG, "Installing ILI9341 panel driver");
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_NUM_RST,
        .color_space = ESP_LCD_COLOR_SPACE_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_ili9341(io_handle, &panel_config, &s_panel),
                        TAG, "panel create failed");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init failed");
    // Das Panel ist ab Werk horizontal gespiegelt (Text erschien
    // spiegelverkehrt); MADCTL-MX korrigiert das.
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, true, false), TAG, "mirror failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "disp on failed");

    ESP_LOGI(TAG, "Registering LVGL display");
    s_disp = lv_display_create(DISPLAY_H_RES, DISPLAY_V_RES);
    lv_display_set_flush_cb(s_disp, flush_cb);

    size_t buf_size = DISPLAY_H_RES * LVGL_BUF_LINES * sizeof(uint16_t);
    void *buf1 = heap_caps_malloc(buf_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    void *buf2 = heap_caps_malloc(buf_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!buf1 || !buf2) {
        ESP_LOGE(TAG, "Failed to allocate LVGL draw buffers");
        return ESP_ERR_NO_MEM;
    }
    lv_display_set_buffers(s_disp, buf1, buf2, buf_size,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    return ESP_OK;
}

lv_display_t *display_get_lv_display(void)
{
    return s_disp;
}
