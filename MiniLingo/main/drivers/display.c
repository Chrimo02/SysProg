// ILI9341-Treiber (abgeleitet aus display_test.c) + LVGL-v9-Anbindung.
#include "display.h"

#include "driver/spi_master.h" // SPI-Treiber (esp-idf) fuer den ILI9341
#include "esp_check.h" // ESP-IDF-Fehlerbehandlung
#include "esp_heap_caps.h" // Heap-Capabilities (DMA-fähiger Speicher für LVGL-Puffer) (DMA = Direct Memory Access)
#include "esp_lcd_ili9341.h" // ILI9341-spezifische Funktionen (esp-idf)
#include "esp_lcd_panel_io.h" // LCD-Panel-IO (esp-idf)
#include "esp_lcd_panel_ops.h" // LCD-Panel-Operationen (esp-idf)
#include "esp_lcd_panel_vendor.h" // LCD-Panel-Herstellerfunktionen (esp-idf)
#include "esp_log.h" // ESP-IDF-Logging (esp_log.h)

static const char *TAG = "display";

#define LCD_HOST    SPI2_HOST // SPI2_HOST = SPI2-Bus (ESP32 hat 3 SPI-Controller: SPI1, SPI2, SPI3)

#define PIN_NUM_SCK   GPIO_NUM_40 // SCK = Serial Clock (SPI-Takt)
#define PIN_NUM_MOSI  GPIO_NUM_41 // MOSI = Master Out Slave In (SPI-Datenleitung vom ESP32 zum Display)
#define PIN_NUM_MISO  GPIO_NUM_39 // MISO = Master In Slave Out (SPI-Datenleitung vom Display zum ESP32)
#define PIN_NUM_CS    GPIO_NUM_1 // CS = Chip Select

// DC-Pin ist paralleles Kontrollsignal für das Display
// DC=0 (low) -> Commands -> Display interpretiert die Daten die über SPI kommen als Befehle
// DC=1 (high) -> Data -> Display interpretiert die Daten die über SPI kommen als Pixel-Daten
#define PIN_NUM_DC    GPIO_NUM_42 // DC = Data/Command --> quasi hardwareseitige erweiterung des SPI-Protokolls speziell für Displays
#define PIN_NUM_RST   GPIO_NUM_2 // RST = Reset

#define LCD_PIXEL_CLOCK_HZ (20 * 1000 * 1000) // 20Mhz Taktfrequenz für MOSI Datenleitung (20Mhz = maximale Taktfrequenz für ILI9341 laut Datenblatt)

// LVGL rendert in zwei Teilpuffern von je 40 Zeilen (Partial-Mode).
#define LVGL_BUF_LINES 40 // Anzahl der Zeilen, die in einem LVGL-Puffer gespeichert werden. Partial-Mode bedeutet, dass das Display in mehreren Durchgängen aktualisiert wird, anstatt den gesamten Bildschirm auf einmal zu rendern.

static lv_display_t *s_disp;  // LVGL-Display Handle (Puffer, Rendering, Flush-Callback)
static esp_lcd_panel_handle_t s_panel; // Hardware-Panel Handle (ILI9341-Kommunikation über SPI)

// Callback-Funktion, die aufgerufen wird, wenn der Flush-Vorgang abgeschlossen ist.
static bool notify_flush_ready(esp_lcd_panel_io_handle_t io,
                               esp_lcd_panel_io_event_data_t *edata,
                               void *user_ctx)
{
    lv_display_flush_ready(s_disp);
    return false;
}

// Flush-Callback-Funktion, die von LVGL aufgerufen wird, wenn ein Bereich des Displays aktualisiert werden muss.
static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    // ILI9341 erwartet RGB565 big-endian; LVGL v9 hat kein
    // LV_COLOR_16_SWAP mehr, daher Swap hier im Flush-Pfad.
    lv_draw_sw_rgb565_swap(px_map, lv_area_get_size(area)); // Tauscht die Endianness der RGB565-Pixel-Daten, die von LVGL bereitgestellt werden, bevor sie an das ILI9341-Display gesendet werden. Dies ist notwendig, da das ILI9341-Display die Daten in einer anderen Byte-Reihenfolge erwartet als LVGL sie liefert.
    esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1, px_map); // Zeichnet die Bitmap-Daten (px_map) auf dem ILI9341-Display im angegebenen Bereich (area). Die Koordinaten werden um 1 erhöht, da das ILI9341-Panel die Endkoordinaten exklusiv behandelt (d.h. die Pixel an den Endkoordinaten werden nicht gezeichnet).
    // lv_display_flush_ready() kommt asynchron aus notify_flush_ready().
}

esp_err_t display_init(void)
{
    ESP_LOGI(TAG, "Initializing SPI bus");
    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_NUM_SCK,
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .quadwp_io_num = -1, // Quad-SPI Write Protect Pin (nicht verwendet, daher -1)
        .quadhd_io_num = -1, // Quad-SPI Hold Pin (nicht verwendet, daher -1)
        //Quad-SPI = eine Erweiterung des SPI-Protokolls, die es ermöglicht, vier Datenleitungen gleichzeitig zu verwenden, um die Datenübertragungsrate zu erhöhen. Da der ILI9341 jedoch nur eine Datenleitung (MOSI) verwendet, sind diese Pins nicht erforderlich.
        .max_transfer_sz = DISPLAY_H_RES * LVGL_BUF_LINES * sizeof(uint16_t), // Maximale Transfergröße pro SPI-Transfer.
        // 320 (DISPLAY_H_RES = Breite) * 40 (LVGL_BUF_LINES) * 2 (RGB565 size) = 25600 Bytes
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO),
                        TAG, "spi_bus_initialize failed");

    ESP_LOGI(TAG, "Installing panel IO");
    esp_lcd_panel_io_handle_t io_handle = NULL; // Handle für die Panel-IO-Struktur, die die Kommunikation zwischen dem ESP32 und dem ILI9341-Display über SPI verwaltet.
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = PIN_NUM_DC,
        .cs_gpio_num = PIN_NUM_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ, // Pixel-Taktfrequenz für die SPI-Kommunikation mit dem ILI9341-Display.
        .lcd_cmd_bits = 8, // Anzahl der Bits für Befehle, die an das ILI9341-Display gesendet werden. 8 Bits = 1 Byte pro Befehl.
        .lcd_param_bits = 8, // Anzahl der Bits für Parameter, die an das ILI9341-Display gesendet werden. 8 Bits = 1 Byte pro Parameter.
        .spi_mode = 0, // SPI-Modus 0 (CPOL=0, CPHA=0) für die Kommunikation mit dem ILI9341-Display. Dies bedeutet, dass der Clock-Pin (SCK) im Ruhezustand low ist und die Daten auf der steigenden Flanke des Clocks gelesen werden.
        .trans_queue_depth = 10, // Maximale Anzahl von SPI-Transaktionen, die gleichzeitig in der Warteschlange stehen können. Dies ermöglicht eine effizientere Nutzung des SPI-Busses, da mehrere Transaktionen gleichzeitig vorbereitet werden können.
        .on_color_trans_done = notify_flush_ready, // Callback-Funktion, die aufgerufen wird, wenn eine Farbtransaktion abgeschlossen ist. Dies signalisiert LVGL, dass der Flush-Vorgang abgeschlossen ist und das Display bereit ist, weitere Daten zu empfangen.
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST,
                                                 &io_config, &io_handle),
                        TAG, "panel io failed");

    ESP_LOGI(TAG, "Installing ILI9341 panel driver");
    // esp_lcd_panel_dev_config_t ist eine Struktur, die die Konfiguration des ILI9341-Panels enthält. Sie wird verwendet, um das Panel zu initialisieren und zu konfigurieren.
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_NUM_RST,
        // Dieses Panel ist BGR (MADCTL-BGR-Bit): mit RGB waren Rot und
        // Blau vertauscht (User-Bubble orange statt blau, Listening-
        // Button blau statt rot). Unabhaengig vom Byte-Swap im
        // Flush-Callback, der nur die Endianness korrigiert.
        .color_space = ESP_LCD_COLOR_SPACE_BGR,
        .bits_per_pixel = 16,
    };
    // esp_lcd_new_panel_ili9341() erstellt ein neues ILI9341-Panel-Handle, das die Kommunikation mit dem ILI9341-Display über SPI verwaltet. Es verwendet das zuvor erstellte Panel-IO-Handle (io_handle) und die Panel-Konfiguration (panel_config).
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_ili9341(io_handle, &panel_config, &s_panel),
                        TAG, "panel create failed");

    // Reset und Initialisierung des ILI9341-Panels. Dies stellt sicher, dass das Display in einem bekannten Zustand ist und bereit ist, Daten zu empfangen.
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset failed");

    // Initialisierung des ILI9341-Panels mit den Standardbefehlen. Dies konfiguriert das Display für die Verwendung mit dem ESP32 und LVGL.
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init failed");

    // Das Panel ist ab Werk horizontal gespiegelt (Text erschien
    // spiegelverkehrt); MADCTL-MX korrigiert das.
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, true, false), TAG, "mirror failed");

    // Das Panel ist ab Werk vertikal gespiegelt (Text erschien
    // spiegelverkehrt); MADCTL-MY korrigiert das.
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "disp on failed");

    ESP_LOGI(TAG, "Registering LVGL display");

    // LVGL-Display erstellen und konfigurieren. Dies erstellt ein neues LVGL-Display-Handle (s_disp) mit der angegebenen horizontalen und vertikalen Auflösung (DISPLAY_H_RES und DISPLAY_V_RES). Das Display-Handle wird verwendet, um die Kommunikation zwischen LVGL und dem ILI9341-Display zu verwalten.
    s_disp = lv_display_create(DISPLAY_H_RES, DISPLAY_V_RES);
    lv_display_set_flush_cb(s_disp, flush_cb);

    size_t buf_size = DISPLAY_H_RES * LVGL_BUF_LINES * sizeof(uint16_t); // LVGL-Puffergröße in bytes
    void *buf1 = heap_caps_malloc(buf_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);  // Speicher für 40 Display-Zeilen: DMA ermöglicht Hardware-Direktzugriff für schnelle SPI-Übertragung, interner RAM schneller als PSRAM

    void *buf2 = heap_caps_malloc(buf_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!buf1 || !buf2) {
        ESP_LOGE(TAG, "Failed to allocate LVGL draw buffers");
        return ESP_ERR_NO_MEM;
    }

    // buf1 -> LVGL rendert nächste Szene hier
    // buf2 -> Hardware zeigt gerade diesen Puffer auf Display an
    // -> kein Flimmern, keine Verzögerungen
    lv_display_set_buffers(s_disp, buf1, buf2, buf_size,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    return ESP_OK;
}

// Liefert das LVGL-Display-Handle zurück, das für die Kommunikation zwischen LVGL und dem ILI9341-Display verwendet wird. Dies ermöglicht anderen Teilen des Programms, auf das Display zuzugreifen und es zu steuern.
lv_display_t *display_get_lv_display(void)
{
    return s_disp;
}
