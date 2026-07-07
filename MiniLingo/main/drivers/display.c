// ILI9341-Treiber (abgeleitet aus display_test.c) + LVGL-v9-Anbindung.
#include "display.h"                    // [C] eigener Header: deklariert display_init/display_get_lv_display (Prototyp-Abgleich)

#include "driver/spi_master.h"         // [HW] ESP-IDF SPI-Master-Treiber für den Display-Bus
#include "esp_check.h"                 // [C] stellt ESP_RETURN_ON_ERROR-Makro bereit: Fehlerprüfung mit early return + Logging
#include "esp_heap_caps.h"             // [Logik] Heap-Allokator mit Capabilities (z.B. DMA-fähiger interner RAM)
#include "esp_lcd_ili9341.h"           // [HW] Vendor-Treiber für das ILI9341-Panel (Init-Sequenz, Register)
#include "esp_lcd_panel_io.h"          // [HW] generische LCD-Panel-IO-Schicht (SPI-Transaktionen zum Panel)
#include "esp_lcd_panel_ops.h"         // [HW] generische Panel-Operationen (reset/init/mirror/disp_on_off/draw_bitmap)
#include "esp_lcd_panel_vendor.h"      // [HW] Vendor-übergreifende Panel-Konfigurationsstrukturen
#include "esp_log.h"                   // [Logik] ESP_LOGI/ESP_LOGE-Logging über UART-Konsole

static const char *TAG = "display";    // [C] static = Datei-lokale (interne) Linkage | [Logik] Log-Tag zur Zuordnung der Ausgaben

#define LCD_HOST    SPI2_HOST          // [C] Präprozessor-Konstante (Textersetzung) | [HW] verwendetes SPI-Peripherie-Modul (SPI2)

#define PIN_NUM_SCK   GPIO_NUM_40      // [HW] GPIO40 = SPI-Clock (SCK) des Display-Busses
#define PIN_NUM_MOSI  GPIO_NUM_41      // [HW] GPIO41 = Master-Out-Slave-In (Daten ESP32 -> Display)
#define PIN_NUM_MISO  GPIO_NUM_39      // [HW] GPIO39 = Master-In-Slave-Out (ungenutzt beim reinen Display, aber Bus-Pin)
#define PIN_NUM_CS    GPIO_NUM_1       // [HW] GPIO1 = Chip-Select für das ILI9341-Panel
#define PIN_NUM_DC    GPIO_NUM_42      // [HW] GPIO42 = Data/Command-Leitung (unterscheidet SPI-Befehl von Pixeldaten)
#define PIN_NUM_RST   GPIO_NUM_2       // [HW] GPIO2 = Hardware-Reset-Pin des Panels

#define LCD_PIXEL_CLOCK_HZ (20 * 1000 * 1000)  // [HW] SPI-Taktfrequenz 20 MHz für die Pixel-Übertragung

// LVGL rendert in zwei Teilpuffern von je 40 Zeilen (Partial-Mode).
#define LVGL_BUF_LINES 40              // [HW] Anzahl Bildzeilen pro Draw-Buffer (Kompromiss zwischen RAM-Bedarf und Flush-Häufigkeit)

static lv_display_t *s_disp;           // [C] static-globale Variable, persistiert zwischen Aufrufen | [HW] Handle auf das registrierte LVGL-Display-Objekt
static esp_lcd_panel_handle_t s_panel; // [C] Zeiger auf internen Panel-State | [HW] opaker Handle auf das initialisierte ILI9341-Panel

// SPI-IO-Fertigstellungs-Callback: wird von der Panel-IO-Schicht aus einem
// Interrupt-Kontext aufgerufen, sobald eine Farbübertragung (DMA) fertig ist,
// und meldet LVGL, dass der Flush-Vorgang abgeschlossen ist.
static bool notify_flush_ready(esp_lcd_panel_io_handle_t io,   // [C] static = interne Linkage, Rückgabetyp bool
                               esp_lcd_panel_io_event_data_t *edata,  // [C] Zeiger auf Event-Metadaten (hier ungenutzt)
                               void *user_ctx)                 // [C] generischer Kontext-Zeiger (hier ungenutzt)
{
    lv_display_flush_ready(s_disp);    // [HW] meldet LVGL: DMA-Übertragung an das Display ist fertig, nächster Flush kann starten
    return false;                      // [C] Rückgabewert false = kein High-Priority-Task-Wakeup nötig (ESP-IDF-ISR-Konvention)
}

// LVGL-Flush-Callback: wird von LVGL aufgerufen, sobald ein Bildbereich
// fertig gerendert ist, und schiebt die Pixeldaten per SPI/DMA zum Panel.
static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)  // [C] static = interne Linkage, void = kein Rückgabewert
{
    // ILI9341 erwartet RGB565 big-endian; LVGL v9 hat kein
    // LV_COLOR_16_SWAP mehr, daher Swap hier im Flush-Pfad.
    lv_draw_sw_rgb565_swap(px_map, lv_area_get_size(area));  // [HW] tauscht Byte-Reihenfolge jedes RGB565-Pixels (Little- auf Big-Endian für das Panel)
    esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1,   // [HW] startet DMA-Übertragung des Pixelblocks an das Panel (Koordinaten aus area-Struct)
                              area->x2 + 1, area->y2 + 1, px_map);  // [C] '+1' weil area->x2/y2 inklusive sind, die Funktion aber ein exklusives Ende erwartet
    // lv_display_flush_ready() kommt asynchron aus notify_flush_ready().
}

// Initialisiert den SPI2-Bus, das ILI9341-Panel und registriert es als
// LVGL-Display mit Double-Buffering im Partial-Render-Modus. Muss einmalig
// nach lv_init() und vor touch_init() aufgerufen werden.
esp_err_t display_init(void)           // [C] Rückgabetyp esp_err_t (Fehlercode); void = keine Parameter
{
    ESP_LOGI(TAG, "Initializing SPI bus");  // [Logik] Fortschritts-Log für die Initialisierungsreihenfolge
    spi_bus_config_t buscfg = {             // [HW] Struct für die SPI-Bus-Pinbelegung und Transfergröße
        .sclk_io_num = PIN_NUM_SCK,          // [C] Designated Initializer (Feld-für-Feld per Name) | [HW] Clock-Pin des Busses
        .mosi_io_num = PIN_NUM_MOSI,         // [HW] MOSI-Pin des Busses
        .miso_io_num = PIN_NUM_MISO,         // [HW] MISO-Pin des Busses
        .quadwp_io_num = -1,                 // [C] -1 = Sentinel-Wert | [HW] kein Quad-Write-Protect-Pin (nicht im Quad-SPI-Modus)
        .quadhd_io_num = -1,                 // [HW] kein Quad-Hold-Pin (nicht im Quad-SPI-Modus)
        .max_transfer_sz = DISPLAY_H_RES * LVGL_BUF_LINES * sizeof(uint16_t),  // [C] sizeof liefert Typgröße zur Compile-Zeit | [HW] maximale Transfergröße = ein Teilpuffer (Zeilen * Breite * 2 Byte/Pixel)
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO),  // [C] Makro-Expansion mit early return bei Fehler | [HW] richtet den SPI2-Bus mit automatischer DMA-Kanalwahl ein
                        TAG, "spi_bus_initialize failed");

    ESP_LOGI(TAG, "Installing panel IO");   // [Logik] Fortschritts-Log
    esp_lcd_panel_io_handle_t io_handle = NULL;  // [C] Zeiger-Initialisierung auf NULL, wird von der folgenden Funktion gesetzt
    esp_lcd_panel_io_spi_config_t io_config = {  // [HW] Struct für die SPI-Panel-IO-Parameter (Pins, Takt, Timing)
        .dc_gpio_num = PIN_NUM_DC,               // [HW] Data/Command-Pin für die Panel-IO-Schicht
        .cs_gpio_num = PIN_NUM_CS,               // [HW] Chip-Select-Pin für die Panel-IO-Schicht
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,           // [HW] SPI-Taktfrequenz für die Pixel-Übertragung
        .lcd_cmd_bits = 8,                       // [HW] Befehle sind 8 Bit breit (ILI9341-Kommandoformat)
        .lcd_param_bits = 8,                     // [HW] Kommando-Parameter sind ebenfalls 8 Bit breit
        .spi_mode = 0,                           // [HW] SPI-Mode 0 (CPOL=0, CPHA=0), vom ILI9341 gefordert
        .trans_queue_depth = 10,                 // [HW] Anzahl gleichzeitig in der Warteschlange stehender SPI-Transaktionen
        .on_color_trans_done = notify_flush_ready,  // [C] Funktionszeiger als Callback-Registrierung | [HW] wird nach jeder abgeschlossenen Pixel-DMA-Übertragung aufgerufen
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST,  // [C] expliziter Cast des Bus-Handles auf den erwarteten Typ
                                                 &io_config, &io_handle),             // [HW] legt die SPI-Panel-IO-Instanz an
                        TAG, "panel io failed");

    ESP_LOGI(TAG, "Installing ILI9341 panel driver");  // [Logik] Fortschritts-Log
    esp_lcd_panel_dev_config_t panel_config = {        // [HW] Struct für die generische Panel-Gerätekonfiguration
        .reset_gpio_num = PIN_NUM_RST,                 // [HW] Reset-Pin, den der Treiber beim esp_lcd_panel_reset() ansteuert
        // Dieses Panel ist BGR (MADCTL-BGR-Bit): mit RGB waren Rot und
        // Blau vertauscht (User-Bubble orange statt blau, Listening-
        // Button blau statt rot). Unabhaengig vom Byte-Swap im
        // Flush-Callback, der nur die Endianness korrigiert.
        .color_space = ESP_LCD_COLOR_SPACE_BGR,        // [HW] setzt das MADCTL-BGR-Bit, damit Farbkanäle korrekt zugeordnet werden
        .bits_per_pixel = 16,                          // [HW] Farbtiefe 16 bit (RGB565-Format)
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_ili9341(io_handle, &panel_config, &s_panel),  // [HW] legt die ILI9341-Vendor-Treiberinstanz an
                        TAG, "panel create failed");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset failed");  // [HW] löst per RST-Pin einen Hardware-Reset des Panels aus
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init failed");    // [HW] spielt die ILI9341-Init-Registersequenz ein
    // Das Panel ist ab Werk horizontal gespiegelt (Text erschien
    // spiegelverkehrt); MADCTL-MX korrigiert das.
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, true, false), TAG, "mirror failed");  // [HW] setzt MADCTL-Spiegel-Bits (X gespiegelt, Y nicht)
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "disp on failed");   // [HW] schaltet die Panel-Ausgabe (Display ON-Kommando) ein

    ESP_LOGI(TAG, "Registering LVGL display");  // [Logik] Fortschritts-Log
    s_disp = lv_display_create(DISPLAY_H_RES, DISPLAY_V_RES);  // [HW] legt ein LVGL-Display-Objekt mit der Panel-Auflösung an
    lv_display_set_flush_cb(s_disp, flush_cb);                // [C] Funktionszeiger als Callback-Registrierung | [HW] registriert flush_cb als Ziel für gerenderte Pixelblöcke

    size_t buf_size = DISPLAY_H_RES * LVGL_BUF_LINES * sizeof(uint16_t);  // [C] size_t = vorzeichenlose Größe | [Logik] Bytegröße eines Teilpuffers berechnen
    void *buf1 = heap_caps_malloc(buf_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);  // [C] '|' = bitweises ODER zur Kombination von Capability-Flags | [HW] DMA-fähiger interner RAM für den ersten Draw-Buffer
    void *buf2 = heap_caps_malloc(buf_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);  // [HW] zweiter Draw-Buffer für Double-Buffering
    if (!buf1 || !buf2) {                        // [C] '!' = logisches NICHT, '||' = logisches ODER; prüft auf NULL-Zeiger (malloc fehlgeschlagen)
        ESP_LOGE(TAG, "Failed to allocate LVGL draw buffers");  // [Logik] Fehler-Log bei fehlgeschlagener Allokation
        return ESP_ERR_NO_MEM;                   // [Logik] Fehlercode nach oben durchreichen, Initialisierung abbrechen
    }
    lv_display_set_buffers(s_disp, buf1, buf2, buf_size,  // [HW] übergibt beide Puffer an LVGL für alternierendes Rendern
                           LV_DISPLAY_RENDER_MODE_PARTIAL);  // [HW] Partial-Mode: LVGL rendert nur Teilbereiche pro Flush statt des ganzen Schirms

    return ESP_OK;                               // [Logik] 0/ESP_OK signalisiert erfolgreiche Initialisierung nach außen
}

// Gibt das von display_init() registrierte LVGL-Display-Handle zurück, damit
// andere Module (z.B. UI-Code) Screens/Objekte darauf zeichnen können.
lv_display_t *display_get_lv_display(void)  // [C] Rückgabetyp Zeiger auf lv_display_t; void = keine Parameter
{
    return s_disp;                              // [Logik] gibt das gespeicherte Display-Handle nach außen weiter
}
