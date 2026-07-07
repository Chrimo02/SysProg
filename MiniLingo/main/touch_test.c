#include <stdio.h>                     // [C] Standard-I/O-Header (hier ungenutzt eingebunden)
#include "freertos/FreeRTOS.h"         // [HW] FreeRTOS-Basistypen/Makros
#include "freertos/task.h"             // [Logik] Task-Funktionen wie vTaskDelay, pdMS_TO_TICKS
#include "driver/spi_master.h"         // [HW] ESP-IDF SPI-Master-Treiber für den Touch-Controller-Bus
#include "driver/gpio.h"               // [HW] GPIO-Treiber-Header (GPIO_NUM_x-Konstanten)
#include "esp_log.h"                   // [Logik] ESP_LOGI/ESP_LOGE-Logging über UART-Konsole

static const char *TAG = "touch_test"; // [C] static = Datei-lokale Linkage | [Logik] Log-Tag zur Zuordnung der Ausgaben

#define TOUCH_HOST    SPI2_HOST        // [C] Präprozessor-Konstante | [HW] verwendeter SPI-Host-Peripherie-Index (SPI2, gemeinsam mit dem Display genutzt)

#define PIN_NUM_SCK   GPIO_NUM_12      // [HW] GPIO12 = SPI-Clock (SCK) zum XPT2046
#define PIN_NUM_MOSI  GPIO_NUM_11      // [HW] GPIO11 = Master-Out-Slave-In (Kommando zum Touch-Controller)
#define PIN_NUM_MISO  GPIO_NUM_13      // [HW] GPIO13 = Master-In-Slave-Out (Messwert vom Touch-Controller)
#define PIN_NUM_T_CS  GPIO_NUM_14      // [HW] GPIO14 = Chip-Select speziell für den Touch-Controller

// XPT2046 control byte commands (12-bit mode, single-ended, power on)
#define CMD_READ_X  0x90               // [C] Hex-Literal | [HW] Kontrollbyte zum Auslesen der X-Koordinate
#define CMD_READ_Y  0xD0               // [HW] Kontrollbyte zum Auslesen der Y-Koordinate
#define CMD_READ_Z1 0xB0               // [HW] Kontrollbyte zum Auslesen des Druck-Messwerts Z1
#define CMD_READ_Z2 0xC0               // [HW] Kontrollbyte zum Auslesen des Druck-Messwerts Z2

static spi_device_handle_t touch_spi;  // [C] static-globale Variable | [HW] Handle auf das SPI-Gerät (Touch-Controller) auf dem Bus

// Führt eine SPI-Transaktion mit dem XPT2046 aus: sendet ein 8-bit-Kommando
// und liest die 12-bit-Antwort (Koordinate oder Druckwert) zurück. Wird für
// jede der vier Messgrößen (X, Y, Z1, Z2) separat aufgerufen.
static uint16_t xpt2046_read(uint8_t cmd)  // [C] static = interne Linkage; cmd = 8-bit-Kontrollbyte, Rückgabe 16-bit-Rohwert
{
    uint8_t tx[3] = {cmd, 0x00, 0x00};  // [C] Array-Initialisierung mit Aggregat-Initializer | [HW] 3-Byte-Sendepuffer: Kommandobyte + 2 Dummy-Bytes zum Takten der Antwort
    uint8_t rx[3] = {0};                // [C] Rest-Initialisierung auf 0 (übrige Elemente automatisch genullt) | [HW] Empfangspuffer für die Antwort

    spi_transaction_t t = {             // [C] Designated Initializer (Feld-für-Feld-Initialisierung per Name) | [HW] SPI-Transaktionsbeschreibung
        .length = 24,                   // [HW] Übertragungslänge in Bit (3 Byte = 24 Bit, vollduplex)
        .tx_buffer = tx,                // [C] Array-Name zerfällt zu Zeiger | [HW] Sendepuffer für die Transaktion
        .rx_buffer = rx,                // [HW] Empfangspuffer für die Transaktion
    };
    ESP_ERROR_CHECK(spi_device_polling_transmit(touch_spi, &t));  // [C] Makro prüft Rückgabewert, bricht bei Fehler ab | [HW] blockierende (Polling-)SPI-Transaktion ohne Interrupt-Overhead, passend für kleine Transfers

    // 12-bit result in bits [14:3] of the 16-bit reply
    return ((rx[1] << 8) | rx[2]) >> 3;  // [C] Bitshift und bitweises ODER kombinieren zwei Bytes zu 16 bit, dann Rechtsshift | [HW] extrahiert den 12-bit-ADC-Wert aus der XPT2046-Antwort
}

// Haupteinstiegspunkt des Testprogramms: initialisiert den SPI-Bus und das
// XPT2046-Touch-Gerät und pollt dauerhaft X/Y/Z-Werte, um Touch-Verkabelung
// und Druckerkennung isoliert zu prüfen.
void app_main(void)                    // [C] ESP-IDF-Einstiegspunkt, wird vom Startup-Code aufgerufen
{
    ESP_LOGI(TAG, "Initializing SPI bus");
    spi_bus_config_t buscfg = {         // [C] Designated Initializer | [HW] SPI-Bus-Konfigurationsstruktur
        .sclk_io_num = PIN_NUM_SCK,     // [HW] Clock-Pin des SPI-Busses
        .mosi_io_num = PIN_NUM_MOSI,    // [HW] MOSI-Pin (Daten zum Slave)
        .miso_io_num = PIN_NUM_MISO,    // [HW] MISO-Pin (Daten vom Slave, hier aktiv genutzt für Messwerte)
        .quadwp_io_num = -1,            // [C] negativer Wert als Sentinel | [HW] kein Quad-Write-Protect-Pin (nicht im Quad-SPI-Modus)
        .quadhd_io_num = -1,            // [HW] kein Quad-Hold-Pin (nicht im Quad-SPI-Modus)
        .max_transfer_sz = 32,          // [HW] maximale Transfergröße pro SPI-Transaktion in Byte (ausreichend für 3-Byte-Kommandos)
    };
    ESP_ERROR_CHECK(spi_bus_initialize(TOUCH_HOST, &buscfg, SPI_DMA_DISABLED));  // [HW] initialisiert SPI-Host ohne DMA (für die kleinen Touch-Transfers unnötig)

    spi_device_interface_config_t devcfg = {  // [HW] Geräte-spezifische SPI-Konfiguration für den Touch-Controller
        .clock_speed_hz = 2 * 1000 * 1000,     // [C] Konstantenausdruck zur Compile-Zeit berechnet | [HW] SPI-Takt 2 MHz (XPT2046-verträglich, niedriger als Display-Takt)
        .mode = 0,                             // [HW] SPI-Modus 0 (CPOL=0, CPHA=0), vom XPT2046 erwartet
        .spics_io_num = PIN_NUM_T_CS,          // [HW] eigenes Chip-Select für den Touch-Controller auf dem gemeinsamen Bus
        .queue_size = 1,                       // [Logik] Tiefe der Transaktions-Warteschlange (hier nur 1, da synchron gepollt wird)
    };
    ESP_ERROR_CHECK(spi_bus_add_device(TOUCH_HOST, &devcfg, &touch_spi));  // [HW] registriert den Touch-Controller als Gerät auf dem SPI-Bus und liefert das Handle

    ESP_LOGI(TAG, "Polling XPT2046 touch values...");

    while (1) {                         // [C] Endlosschleife | [Logik] Dauerhaftes Auslesen der Touch-Werte
        uint16_t x  = xpt2046_read(CMD_READ_X);   // [Logik] X-Koordinate abfragen
        uint16_t y  = xpt2046_read(CMD_READ_Y);   // [Logik] Y-Koordinate abfragen
        uint16_t z1 = xpt2046_read(CMD_READ_Z1);  // [Logik] Druck-Messwert Z1 abfragen
        uint16_t z2 = xpt2046_read(CMD_READ_Z2);  // [Logik] Druck-Messwert Z2 abfragen

        // Z1+4095-Z2 is small (large) when pressed (released); use Z1 as a simple touch indicator
        if (z1 > 100) {                  // [Logik] einfache Schwellwert-Prüfung: Z1 hoch genug bedeutet, dass der Bildschirm gerade berührt wird
            ESP_LOGI(TAG, "Touch: X=%4u Y=%4u Z1=%4u Z2=%4u", x, y, z1, z2);  // [C] %4u = rechtsbündige vorzeichenlose Dezimalausgabe mit Mindestbreite | [Logik] gemessene Touch-Werte ausgeben
        }

        vTaskDelay(pdMS_TO_TICKS(50));    // [HW] FreeRTOS-Task-Delay | [Logik] 50 ms Pause zwischen den Polling-Zyklen
    }
}
