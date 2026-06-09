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

#include <stdio.h>                      // Standard-Ein-/Ausgabe: fopen, fprintf, fread, fclose
#include <string.h>                     // String-Funktionen (z.B. memset/strlen, hier indirekt genutzt)
#include <stdlib.h>                     // Speicherverwaltung: malloc/free, Konvertierungen
#include "freertos/FreeRTOS.h"          // FreeRTOS-Basis: Datentypen, pdMS_TO_TICKS
#include "freertos/task.h"              // FreeRTOS-Tasks: vTaskDelay (Verzögerungen)
#include "driver/gpio.h"                // GPIO-Treiber: Pins als Ein-/Ausgang schalten
#include "driver/sdspi_host.h"          // SD-Karte über SPI ansprechen (SDSPI-Host)
#include "driver/spi_common.h"          // Gemeinsame SPI-Bus-Konfiguration
#include "esp_vfs_fat.h"                // FAT-Dateisystem ins VFS einhängen (mounten)
#include "sdmmc_cmd.h"                  // SD-Karten-Kommandos / Karteninfos ausgeben
#include "esp_lcd_panel_io.h"           // LCD-Panel-IO-Schicht (Befehle/Daten ans Display)
#include "esp_log.h"                    // Logging-Makros (ESP_LOGI/E) über UART
#include "esp_heap_caps.h"              // Speicher mit Eigenschaften anfordern (z.B. DMA-fähig)
#include "font_8x16.h"                  // Eigene Schriftart 8x16 Pixel für Textausgabe

static const char *TAG = "SDTEST";      // Tag-String, der in jeder Log-Zeile vorangestellt wird

// Display Pins (wie gehabt)
#define LCD_RST  46                     // Reset-Pin des Displays
#define LCD_CS   45                     // Chip-Select (Display auswählen)
#define LCD_RS    9                     // Register-Select / Data-Command (Befehl vs. Daten)
#define LCD_WR    8                     // Write-Strobe (Daten in Display schreiben)
#define LCD_RD    7                     // Read-Strobe (nicht genutzt, fest auf HIGH)
#define LCD_D0   10                     // Datenbus Bit 0
#define LCD_D1   11                     // Datenbus Bit 1
#define LCD_D2   12                     // Datenbus Bit 2
#define LCD_D3   13                     // Datenbus Bit 3
#define LCD_D4   14                     // Datenbus Bit 4
#define LCD_D5   21                     // Datenbus Bit 5
#define LCD_D6   47                     // Datenbus Bit 6
#define LCD_D7   48                     // Datenbus Bit 7

// SD-Karten Pins
#define SD_SCK   3                      // SPI-Takt (Clock) für die SD-Karte
#define SD_MOSI  4                      // Master Out Slave In (Daten zur Karte)
#define SD_MISO  5                      // Master In Slave Out (Daten von der Karte)
#define SD_CS    6                      // Chip-Select der SD-Karte

#define MOUNT_POINT "/sdcard"           // Pfad, unter dem die Karte im Dateisystem erscheint
#define TEST_FILE   MOUNT_POINT"/hello.txt"  // Vollständiger Pfad zur Testdatei (/sdcard/hello.txt)

#define LCD_W   240                     // Display-Breite in Pixel
#define LCD_H   320                     // Display-Höhe in Pixel

// Farben (BGR-kompensiert)
#define COLOR_BLACK   0x0000            // Schwarz (RGB565)
#define COLOR_WHITE   0xFFFF            // Weiß
#define COLOR_RED     0x001F            // Rot (durch BGR vertauscht im 565-Format)
#define COLOR_GREEN   0x07E0            // Grün
#define COLOR_YELLOW  0x07FF            // Gelb
#define COLOR_CYAN    0xFFE0            // Cyan
#define COLOR_GRAY    0x8410            // Grau

static esp_lcd_panel_io_handle_t io_handle = NULL;  // Handle für die LCD-IO-Schicht (global genutzt)

// ================================================================
// DISPLAY (kompakt)
// ================================================================

static void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)  // legt Zeichenbereich fest
{
    uint8_t col[4] = { x0>>8, x0&0xFF, (x1-1)>>8, (x1-1)&0xFF };  // Spaltenbereich als High-/Low-Bytes (Start..Ende)
    uint8_t row[4] = { y0>>8, y0&0xFF, (y1-1)>>8, (y1-1)&0xFF };  // Zeilenbereich als High-/Low-Bytes (Start..Ende)
    esp_lcd_panel_io_tx_param(io_handle, 0x2A, col, 4);          // Befehl 0x2A: Spaltenadresse setzen (CASET)
    esp_lcd_panel_io_tx_param(io_handle, 0x2B, row, 4);          // Befehl 0x2B: Zeilenadresse setzen (RASET)
}

static void lcd_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)  // füllt Rechteck mit Farbe
{
    uint32_t n = w * h;                                          // Anzahl der Pixel im Rechteck
    uint16_t *buf = heap_caps_malloc(n * 2, MALLOC_CAP_DMA);     // DMA-fähigen Puffer anfordern (2 Byte je Pixel)
    if (!buf) return;                                           // bei fehlgeschlagener Allokation abbrechen
    for (uint32_t i = 0; i < n; i++) buf[i] = color;            // Puffer komplett mit der Farbe füllen
    lcd_set_window(x, y, x+w, y+h);                            // Zeichenfenster auf das Rechteck setzen
    esp_lcd_panel_io_tx_color(io_handle, 0x2C, buf, n*2);       // Befehl 0x2C: Pixeldaten senden (RAM-Write)
    free(buf);                                                  // Puffer wieder freigeben
}

static void lcd_fill(uint16_t color) { lcd_fill_rect(0, 0, LCD_W, LCD_H, color); }  // füllt das ganze Display

static void lcd_draw_char(uint16_t x, uint16_t y, char c, uint16_t fg, uint16_t bg)  // zeichnet ein Zeichen
{
    if (c < FONT_FIRST || c > FONT_LAST) c = '?';              // unbekannte Zeichen durch '?' ersetzen
    const uint8_t *g = font_8x16[c - FONT_FIRST];             // Zeiger auf die Bitmap (Glyph) des Zeichens
    uint16_t buf[FONT_W * FONT_H];                            // Pixelpuffer für ein Zeichen
    for (int r = 0; r < FONT_H; r++)                          // über alle Zeilen des Zeichens
        for (int b = 0; b < FONT_W; b++)                      // über alle Spalten (Bits) der Zeile
            buf[r*FONT_W+b] = (g[r] & (0x80>>b)) ? fg : bg;   // Bit gesetzt? -> Vordergrund, sonst Hintergrund
    lcd_set_window(x, y, x+FONT_W, y+FONT_H);                // Fenster auf die Zeichengröße setzen
    esp_lcd_panel_io_tx_color(io_handle, 0x2C, buf, sizeof(buf));  // Pixeldaten des Zeichens senden
}

static void lcd_print(uint16_t x, uint16_t y, const char *str, uint16_t fg, uint16_t bg)  // gibt einen Text aus
{
    uint16_t cx = x, cy = y;                                  // aktuelle Cursorposition (x, y)
    while (*str) {                                            // bis zum String-Ende ('\0')
        if (*str == '\n') { cx = x; cy += FONT_H; }          // Zeilenumbruch: zurück nach links, eine Zeile tiefer
        else {                                               // normales Zeichen
            if (cx + FONT_W > LCD_W) { cx = x; cy += FONT_H; }  // automatischer Umbruch am rechten Rand
            lcd_draw_char(cx, cy, *str, fg, bg);             // Zeichen an aktueller Position zeichnen
            cx += FONT_W;                                    // Cursor um eine Zeichenbreite nach rechts
        }
        str++;                                               // zum nächsten Zeichen weitergehen
    }
}

static void lcd_bus_init(void)                                // initialisiert den parallelen I80-Bus zum Display
{
    gpio_set_direction(LCD_RD,  GPIO_MODE_OUTPUT); gpio_set_level(LCD_RD,  1);  // RD als Ausgang, dauerhaft HIGH (kein Lesen)
    gpio_set_direction(LCD_RST, GPIO_MODE_OUTPUT); gpio_set_level(LCD_RST, 1);  // RST als Ausgang, zunächst HIGH (kein Reset)

    esp_lcd_i80_bus_handle_t bus = NULL;                     // Handle für den I80-Bus
    esp_lcd_i80_bus_config_t bus_cfg = {                     // Konfiguration des parallelen Busses
        .clk_src        = LCD_CLK_SRC_DEFAULT,               // Standard-Taktquelle verwenden
        .wr_gpio_num    = LCD_WR,                            // Write-Pin festlegen
        .dc_gpio_num    = LCD_RS,                            // Data/Command-Pin festlegen
        .data_gpio_nums = { LCD_D0, LCD_D1, LCD_D2, LCD_D3,  // die 8 Datenpins zuordnen
                            LCD_D4, LCD_D5, LCD_D6, LCD_D7 },
        .bus_width          = 8,                             // 8-Bit-Parallelbus
        .max_transfer_bytes = LCD_W * LCD_H * 2 + 8,         // größter Transfer = ganzer Bildschirm + Reserve
    };
    ESP_ERROR_CHECK(esp_lcd_new_i80_bus(&bus_cfg, &bus));    // Bus anlegen, bei Fehler Programm abbrechen

    esp_lcd_panel_io_i80_config_t io_cfg = {                 // Konfiguration der IO-Schicht auf dem Bus
        .cs_gpio_num       = LCD_CS,                         // Chip-Select-Pin des Displays
        .pclk_hz           = 20 * 1000 * 1000,               // Pixeltakt 20 MHz
        .trans_queue_depth = 10,                             // Warteschlangentiefe für Transfers
        .lcd_cmd_bits      = 8,                              // Befehle sind 8 Bit breit
        .lcd_param_bits    = 8,                              // Parameter sind 8 Bit breit
        .dc_levels = { .dc_cmd_level = 0, .dc_data_level = 1 },  // DC-Pegel: 0 = Befehl, 1 = Daten
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i80(bus, &io_cfg, &io_handle));  // IO-Handle anlegen, sonst Abbruch
}

static void ili9341_init(void)                               // Init-Sequenz für den ILI9341-Displaycontroller
{
    gpio_set_level(LCD_RST, 0); vTaskDelay(pdMS_TO_TICKS(20));   // Hardware-Reset: RST LOW, 20 ms warten
    gpio_set_level(LCD_RST, 1); vTaskDelay(pdMS_TO_TICKS(120));  // RST HIGH, 120 ms Hochlaufzeit abwarten
    esp_lcd_panel_io_tx_param(io_handle, 0x01, NULL, 0);     // Befehl 0x01: Software-Reset
    vTaskDelay(pdMS_TO_TICKS(100));                          // nach Reset 100 ms warten
    esp_lcd_panel_io_tx_param(io_handle, 0x11, NULL, 0);     // Befehl 0x11: Sleep Out (Display aufwecken)
    vTaskDelay(pdMS_TO_TICKS(120));                          // 120 ms warten bis stabil
    uint8_t p;                                               // Hilfsvariable für Befehlsparameter
    p = 0x55; esp_lcd_panel_io_tx_param(io_handle, 0x3A, &p, 1);  // Befehl 0x3A: Pixelformat 16 Bit (RGB565)
    p = 0x48; esp_lcd_panel_io_tx_param(io_handle, 0x36, &p, 1);  // Befehl 0x36: Memory Access (Orientierung/BGR)
    esp_lcd_panel_io_tx_param(io_handle, 0x29, NULL, 0);     // Befehl 0x29: Display ON (Anzeige einschalten)
    vTaskDelay(pdMS_TO_TICKS(20));                           // kurz warten bis Anzeige aktiv
}

// ================================================================
// SD-KARTE
// ================================================================

static sdmmc_card_t *card = NULL;                            // globale Struktur, die die gemountete Karte beschreibt

static esp_err_t sd_mount(void)                              // initialisiert SPI und mountet die SD-Karte
{
    ESP_LOGI(TAG, "SPI-Bus für SD initialisieren...");      // Info-Log: SPI-Initialisierung startet

    spi_bus_config_t bus_cfg = {                            // Konfiguration des SPI-Busses für die Karte
        .mosi_io_num     = SD_MOSI,                         // MOSI-Pin (Daten zur Karte)
        .miso_io_num     = SD_MISO,                         // MISO-Pin (Daten von der Karte)
        .sclk_io_num     = SD_SCK,                          // SCK-Pin (Takt)
        .quadwp_io_num   = -1,                              // kein WP-Pin (nur bei Quad-SPI nötig)
        .quadhd_io_num   = -1,                              // kein HD-Pin (nur bei Quad-SPI nötig)
        .max_transfer_sz = 4000,                            // maximale Transfergröße in Bytes
    };
    esp_err_t ret = spi_bus_initialize(SPI3_HOST, &bus_cfg, SDSPI_DEFAULT_DMA);  // SPI3-Bus mit DMA initialisieren
    if (ret != ESP_OK) {                                    // Fehler bei der Bus-Initialisierung?
        ESP_LOGE(TAG, "SPI-Bus init fehlgeschlagen: %s", esp_err_to_name(ret));  // Fehlertext loggen
        return ret;                                         // mit Fehlercode zurückkehren
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();               // Standard-Hostkonfiguration für SD-über-SPI
    host.slot = SPI3_HOST;                                  // Host an SPI3-Bus binden

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();  // Standard-Slot-/Geräteconfig
    slot_config.gpio_cs = SD_CS;                            // Chip-Select-Pin der Karte setzen
    slot_config.host_id = SPI3_HOST;                        // Slot an SPI3-Host binden

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {       // Optionen fürs Einhängen des Dateisystems
        .format_if_mount_failed = false,                   // NICHT automatisch formatieren wenn Mount scheitert
        .max_files              = 5,                        // höchstens 5 gleichzeitig geöffnete Dateien
        .allocation_unit_size   = 16 * 1024,               // Cluster-/Zuordnungsgröße (16 KB)
    };

    ESP_LOGI(TAG, "SD-Karte mounten...");                  // Info-Log: Mount-Vorgang startet
    ret = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot_config,  // Karte einhängen unter /sdcard
                                   &mount_config, &card);   // füllt das globale card-Handle
    if (ret != ESP_OK) {                                    // Mount fehlgeschlagen?
        ESP_LOGE(TAG, "Mount fehlgeschlagen: %s", esp_err_to_name(ret));  // Fehlertext loggen
        return ret;                                         // mit Fehlercode zurückkehren
    }

    ESP_LOGI(TAG, "SD-Karte gemountet:");                  // Info-Log: Mount erfolgreich
    sdmmc_card_print_info(stdout, card);                   // Karteninfos (Name, Größe, Typ) ausgeben
    return ESP_OK;                                         // Erfolg melden
}

static esp_err_t sd_write_test(void)                       // schreibt eine Testdatei auf die Karte
{
    ESP_LOGI(TAG, "Schreibe %s ...", TEST_FILE);          // Info-Log: welche Datei geschrieben wird
    FILE *f = fopen(TEST_FILE, "w");                       // Datei zum Schreiben öffnen (legt sie neu an)
    if (!f) {                                              // Öffnen fehlgeschlagen?
        ESP_LOGE(TAG, "fopen (write) fehlgeschlagen");    // Fehler loggen
        return ESP_FAIL;                                  // mit Fehler zurückkehren
    }
    fprintf(f, "Hallo von Christians\nESP32-S3!\n");      // erste Zeilen in die Datei schreiben
    fprintf(f, "SPI funktioniert :)\n");                  // weitere Zeile schreiben
    fclose(f);                                            // Datei schließen (Puffer auf Karte schreiben)
    ESP_LOGI(TAG, "Schreiben fertig");                    // Info-Log: Schreiben abgeschlossen
    return ESP_OK;                                        // Erfolg melden
}

static esp_err_t sd_read_test(char *out_buf, size_t out_size)  // liest die Testdatei in einen Puffer
{
    ESP_LOGI(TAG, "Lese %s ...", TEST_FILE);              // Info-Log: welche Datei gelesen wird
    FILE *f = fopen(TEST_FILE, "r");                       // Datei zum Lesen öffnen
    if (!f) {                                              // Öffnen fehlgeschlagen?
        ESP_LOGE(TAG, "fopen (read) fehlgeschlagen");     // Fehler loggen
        return ESP_FAIL;                                  // mit Fehler zurückkehren
    }
    size_t n = fread(out_buf, 1, out_size - 1, f);        // bis zu (Puffergröße-1) Bytes lesen, n = gelesene Bytes
    out_buf[n] = '\0';                                    // String mit Nullterminator abschließen
    fclose(f);                                            // Datei schließen
    ESP_LOGI(TAG, "Inhalt (%d Bytes):\n%s", (int)n, out_buf);  // gelesene Bytezahl und Inhalt loggen
    return ESP_OK;                                        // Erfolg melden
}

// ================================================================
// MAIN
// ================================================================

void app_main(void)                                        // Einstiegspunkt der Anwendung (von ESP-IDF aufgerufen)
{
    ESP_LOGI(TAG, "SD-Karten Test startet");              // Info-Log: Programmstart

    // 1) Display init
    lcd_bus_init();                                        // parallelen Bus zum Display einrichten
    ili9341_init();                                        // Displaycontroller initialisieren
    lcd_fill(COLOR_BLACK);                                 // Bildschirm schwarz löschen

    lcd_print(10, 10, "SD-Karten Test", COLOR_YELLOW, COLOR_BLACK);  // Überschrift anzeigen
    lcd_print(10, 35, "Mounte SD...",   COLOR_WHITE,  COLOR_BLACK);  // Statushinweis: Mount läuft

    // 2) SD mounten
    if (sd_mount() != ESP_OK) {                            // Karte einhängen; bei Fehler:
        lcd_print(10, 60, "MOUNT FEHLER!",  COLOR_RED, COLOR_BLACK);   // Fehler auf Display anzeigen
        lcd_print(10, 80, "Karte gesteckt?", COLOR_GRAY, COLOR_BLACK); // möglicher Grund 1
        lcd_print(10, 98, "FAT32?",          COLOR_GRAY, COLOR_BLACK); // möglicher Grund 2
        return;                                            // Programm abbrechen (kein Sinn weiterzumachen)
    }
    lcd_print(10, 60, "OK gemountet",  COLOR_GREEN, COLOR_BLACK);  // Erfolgsmeldung anzeigen

    // 3) Datei schreiben
    lcd_print(10, 85, "Schreibe Datei...", COLOR_WHITE, COLOR_BLACK);  // Statushinweis: Schreiben läuft
    if (sd_write_test() != ESP_OK) {                      // Datei schreiben; bei Fehler:
        lcd_print(10, 105, "SCHREIB-FEHLER!", COLOR_RED, COLOR_BLACK); // Fehler anzeigen
        return;                                            // abbrechen
    }
    lcd_print(10, 105, "OK geschrieben", COLOR_GREEN, COLOR_BLACK);  // Erfolgsmeldung anzeigen

    // 4) Datei lesen
    lcd_print(10, 130, "Lese Datei...", COLOR_WHITE, COLOR_BLACK);  // Statushinweis: Lesen läuft
    char buf[256];                                         // Puffer für den Dateiinhalt
    if (sd_read_test(buf, sizeof(buf)) != ESP_OK) {       // Datei lesen; bei Fehler:
        lcd_print(10, 150, "LESE-FEHLER!", COLOR_RED, COLOR_BLACK);  // Fehler anzeigen
        return;                                            // abbrechen
    }
    lcd_print(10, 150, "OK gelesen", COLOR_GREEN, COLOR_BLACK);  // Erfolgsmeldung anzeigen

    // 5) Inhalt anzeigen
    lcd_print(10, 180, "Inhalt:",  COLOR_CYAN, COLOR_BLACK);     // Beschriftung über dem Inhalt
    lcd_fill_rect(10, 200, LCD_W-20, 2, COLOR_GRAY);            // dünne graue Trennlinie zeichnen
    lcd_print(10, 210, buf, COLOR_WHITE, COLOR_BLACK);         // gelesenen Dateiinhalt anzeigen

    // 6) Unmount
    esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);        // Karte sauber aushängen (Daten sichern)
    ESP_LOGI(TAG, "Unmount fertig");                      // Info-Log: fertig
}
