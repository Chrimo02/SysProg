#include <stdio.h>                         // [C] Standard-I/O-Header (hier ungenutzt eingebunden)
#include "freertos/FreeRTOS.h"             // [HW] FreeRTOS-Basistypen/Makros
#include "freertos/task.h"                 // [Logik] Task-Funktionen wie vTaskDelay, pdMS_TO_TICKS
#include "driver/spi_master.h"             // [HW] ESP-IDF SPI-Master-Treiber für den Display-Bus
#include "driver/gpio.h"                   // [HW] GPIO-Treiber-Header (GPIO_NUM_x-Konstanten)
#include "esp_lcd_panel_io.h"              // [HW] Abstraktion für die Panel-IO-Schicht (SPI-Kommandos ans Display)
#include "esp_lcd_panel_vendor.h"          // [HW] Vendor-spezifische Panel-Treiber-Deklarationen
#include "esp_lcd_panel_ops.h"             // [HW] generische Panel-Operationen (reset, init, draw_bitmap, ...)
#include "esp_lcd_ili9341.h"               // [HW] ILI9341-spezifischer LCD-Controller-Treiber
#include "esp_log.h"                       // [Logik] ESP_LOGI/ESP_LOGE-Logging über UART-Konsole

static const char *TAG = "display_test";   // [C] static = Datei-lokale Linkage | [Logik] Log-Tag zur Zuordnung der Ausgaben

#define LCD_HOST    SPI2_HOST              // [C] Präprozessor-Konstante | [HW] verwendeter SPI-Host-Peripherie-Index (SPI2)

#define PIN_NUM_SCK   GPIO_NUM_12          // [HW] GPIO12 = SPI-Clock (SCK) zum Display
#define PIN_NUM_MOSI  GPIO_NUM_11          // [HW] GPIO11 = Master-Out-Slave-In (Daten zum Display)
#define PIN_NUM_MISO  GPIO_NUM_13          // [HW] GPIO13 = Master-In-Slave-Out (hier ungenutzt, da Display i.d.R. nicht zurückliest)
#define PIN_NUM_CS    GPIO_NUM_10          // [HW] GPIO10 = Chip-Select für das Display
#define PIN_NUM_DC    GPIO_NUM_8           // [HW] GPIO8 = Data/Command-Leitung (unterscheidet Befehl von Pixel-Daten)
#define PIN_NUM_RST   GPIO_NUM_9           // [HW] GPIO9 = Hardware-Reset-Leitung des Displays

#define LCD_H_RES   240                    // [HW] horizontale Auflösung des ILI9341 in Pixeln
#define LCD_V_RES   320                    // [HW] vertikale Auflösung des ILI9341 in Pixeln
#define LCD_PIXEL_CLOCK_HZ (20 * 1000 * 1000)  // [C] Konstantenausdruck, zur Compile-Zeit berechnet | [HW] SPI-Taktfrequenz 20 MHz für Pixel-Übertragung

// RGB565 colors
#define COLOR_RED   0xF800                 // [C] Hex-Literal | [HW] RGB565-kodierte Farbe Rot (5 Bit R, 6 Bit G, 5 Bit B)
#define COLOR_GREEN 0x07E0                 // [HW] RGB565-kodierte Farbe Grün
#define COLOR_BLUE  0x001F                 // [HW] RGB565-kodierte Farbe Blau
#define COLOR_WHITE 0xFFFF                 // [HW] RGB565-kodierte Farbe Weiß (alle Bits gesetzt)

// Füllt den gesamten Bildschirm zeilenweise mit einer einzigen Farbe, indem
// eine vorbereitete Pixelzeile per DMA/SPI wiederholt an den Panel-Treiber
// übergeben wird. Dient hier als einfacher Sichtprüfungstest des Displays.
static void fill_screen(esp_lcd_panel_handle_t panel, uint16_t color)  // [C] Zeiger-Parameter panel (opaker Handle), color = 16-bit RGB565-Wert
{
    static uint16_t line[LCD_H_RES];       // [C] static = liegt im BSS statt auf dem Stack | [Logik] Zeilenpuffer für eine komplette Bildschirmbreite
    for (int i = 0; i < LCD_H_RES; i++) {  // [C] for-Schleife über alle Spalten der Zeile
        line[i] = color;                   // [Logik] jedes Pixel der Zeile auf die Zielfarbe setzen
    }
    for (int y = 0; y < LCD_V_RES; y++) {  // [C] for-Schleife über alle Bildzeilen
        esp_lcd_panel_draw_bitmap(panel, 0, y, LCD_H_RES, y + 1, line);  // [HW] überträgt die vorbereitete Zeile per SPI an Display-Zeile y (Koordinaten x0,y0,x1,y1)
    }
}

// Haupteinstiegspunkt des Testprogramms: initialisiert SPI-Bus, Panel-IO und
// ILI9341-Treiber und lässt den Bildschirm dauerhaft in vier Farben
// durchlaufen, um Display-Verkabelung und -Ansteuerung zu prüfen.
void app_main(void)                        // [C] ESP-IDF-Einstiegspunkt, wird vom Startup-Code aufgerufen
{
    ESP_LOGI(TAG, "Initializing SPI bus");
    spi_bus_config_t buscfg = {            // [C] Designated Initializer (Feld-für-Feld-Initialisierung per Name) | [HW] SPI-Bus-Konfigurationsstruktur
        .sclk_io_num = PIN_NUM_SCK,        // [HW] Clock-Pin des SPI-Busses
        .mosi_io_num = PIN_NUM_MOSI,       // [HW] MOSI-Pin (Daten zum Slave)
        .miso_io_num = PIN_NUM_MISO,       // [HW] MISO-Pin (Daten vom Slave, hier kaum genutzt)
        .quadwp_io_num = -1,               // [C] negativer Wert als Sentinel | [HW] kein Quad-Write-Protect-Pin (nicht im Quad-SPI-Modus)
        .quadhd_io_num = -1,               // [HW] kein Quad-Hold-Pin (nicht im Quad-SPI-Modus)
        .max_transfer_sz = LCD_H_RES * 80 * sizeof(uint16_t),  // [C] sizeof liefert Typgröße zur Compile-Zeit | [HW] maximale Transfergröße pro SPI-Transaktion (80 Zeilen Puffer)
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));  // [C] Makro prüft Rückgabewert, bricht bei Fehler ab | [HW] initialisiert SPI-Host mit automatischer DMA-Kanalwahl

    ESP_LOGI(TAG, "Installing panel IO");
    esp_lcd_panel_io_handle_t io_handle = NULL;  // [C] Zeiger-Initialisierung auf NULL | [HW] wird von der folgenden Funktion mit dem IO-Handle befüllt
    esp_lcd_panel_io_spi_config_t io_config = {  // [HW] Konfiguration der SPI-Panel-IO-Schicht
        .dc_gpio_num = PIN_NUM_DC,         // [HW] Data/Command-Pin für die IO-Schicht
        .cs_gpio_num = PIN_NUM_CS,         // [HW] Chip-Select-Pin für die IO-Schicht
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,     // [HW] Pixel-/SPI-Taktfrequenz für Übertragungen
        .lcd_cmd_bits = 8,                 // [HW] Breite der Kommando-Übertragung in Bit (ILI9341-Standard: 8 bit)
        .lcd_param_bits = 8,               // [HW] Breite der Parameter-Übertragung in Bit
        .spi_mode = 0,                     // [HW] SPI-Modus 0 (CPOL=0, CPHA=0), vom ILI9341 erwartet
        .trans_queue_depth = 10,           // [Logik] Tiefe der Transaktions-Warteschlange für gepufferte SPI-Übertragungen
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle));  // [C] expliziter Cast auf den erwarteten Bus-Handle-Typ | [HW] legt die Panel-IO-Instanz auf dem SPI-Bus an

    ESP_LOGI(TAG, "Installing ILI9341 panel driver");
    esp_lcd_panel_handle_t panel_handle = NULL;  // [C] Zeiger-Initialisierung auf NULL, wird gleich befüllt
    esp_lcd_panel_dev_config_t panel_config = {  // [HW] Geräte-Konfiguration des LCD-Panels
        .reset_gpio_num = PIN_NUM_RST,     // [HW] Reset-Pin für einen Hardware-Reset des Panels
        .color_space = ESP_LCD_COLOR_SPACE_RGB,  // [HW] Farbraum-Reihenfolge RGB (statt BGR)
        .bits_per_pixel = 16,              // [HW] Farbtiefe 16 bit = RGB565-Format
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io_handle, &panel_config, &panel_handle));  // [HW] erzeugt die ILI9341-Treiberinstanz gebunden an die vorherige IO-Schicht

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));       // [HW] löst Hardware-Reset über den Reset-Pin aus
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));        // [HW] sendet die ILI9341-Init-Sequenz (Timing, Pixelformat etc.)
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));  // [C] bool-Argument true | [HW] schaltet die Anzeige des Panels ein

    const uint16_t colors[] = {COLOR_RED, COLOR_GREEN, COLOR_BLUE, COLOR_WHITE};  // [C] Array-Initialisierung mit Aggregat-Initializer | [Logik] Farbreihenfolge für den Testdurchlauf
    const char *names[] = {"Rot", "Gruen", "Blau", "Weiss"};  // [C] Array von String-Literalen (const char*) | [Logik] passende Anzeigenamen für die Log-Ausgabe

    while (1) {                            // [C] Endlosschleife | [Logik] wiederholter Farbdurchlauf zur Dauerprüfung
        for (int i = 0; i < 4; i++) {       // [C] for-Schleife über alle vier Testfarben
            ESP_LOGI(TAG, "Fuelle Bildschirm: %s", names[i]);  // [Logik] Statusmeldung, welche Farbe gerade angezeigt wird
            fill_screen(panel_handle, colors[i]);              // [Logik] Bildschirm mit aktueller Testfarbe füllen
            vTaskDelay(pdMS_TO_TICKS(2000));                    // [HW] FreeRTOS-Task-Delay | [Logik] 2 Sekunden warten, damit die Farbe sichtbar bleibt
        }
    }
}
