// XPT2046-Treiber (abgeleitet aus touch_test.c) als LVGL-Input-Device.
#include "touch.h"                      // [C] eigener Header: deklariert touch_init/touch_get_lv_indev (Prototyp-Abgleich)

#include "display.h"                   // [HW] liefert DISPLAY_H_RES/V_RES für die Koordinaten-Skalierung
#include "driver/gpio.h"               // [HW] GPIO-Treiber (Pin-Konstanten wie GPIO_NUM_x)
#include "driver/spi_master.h"         // [HW] ESP-IDF SPI-Master-Treiber, hier für den Touch-Bus (gleicher Bus wie Display)
#include "esp_check.h"                 // [C] stellt ESP_RETURN_ON_ERROR-Makro bereit: Fehlerprüfung mit early return + Logging
#include "esp_log.h"                   // [Logik] ESP_LOGI/ESP_LOGE-Logging über UART-Konsole

static const char *TAG = "touch";      // [C] static = Datei-lokale (interne) Linkage | [Logik] Log-Tag zur Zuordnung der Ausgaben

#define TOUCH_HOST    SPI2_HOST        // [C] Präprozessor-Konstante (Textersetzung) | [HW] gleicher SPI-Bus wie das Display (Bus-Sharing)
#define PIN_NUM_T_CS  GPIO_NUM_38      // [HW] GPIO38 = eigenes Chip-Select für den XPT2046 (Bus geteilt, CS separat)

// XPT2046 control bytes (12-bit mode, single-ended, power on).
// Achtung: laut Datenblatt liest 0x90 die Y- und 0xD0 die X-Position —
// die Benennung hier stammt aus touch_test.c und ist vertauscht,
// TOUCH_SWAP_XY=1 unten gleicht das aus.
#define CMD_READ_X  0x90               // [HW] XPT2046-Kommandobyte: Kanalauswahl + 12-bit-Modus + Single-Ended + Power-On
#define CMD_READ_Y  0xD0               // [HW] XPT2046-Kommandobyte für die andere Achse (siehe Hinweis oben zur Vertauschung)
#define CMD_READ_Z1 0xB0               // [HW] XPT2046-Kommandobyte zum Auslesen des Druckdrucks (Z1, Andruckstärke)

#define TOUCH_PRESS_THRESHOLD 100  // Z1-Rohwert, ab dem "gedrueckt" gilt
#define TOUCH_POLL_PERIOD_MS  50   // [HW] Abtastintervall des LVGL-Input-Timers in Millisekunden

// Hardcodierte Kalibrierung (Spec Phase 1): Rohwert-Spannen der vier
// Eckpunkte. Werte sind typische Defaults fuer ILI9341/XPT2046-Module
// und muessen am Geraet verifiziert werden — beim ersten Druck pro
// Beruehrung werden Roh- und Pixelkoordinaten geloggt.
#define TOUCH_RAW_X_MIN 200            // [HW] kleinster erwarteter ADC-Rohwert der X-Achse (linker Rand)
#define TOUCH_RAW_X_MAX 3900           // [HW] größter erwarteter ADC-Rohwert der X-Achse (rechter Rand)
#define TOUCH_RAW_Y_MIN 240            // [HW] kleinster erwarteter ADC-Rohwert der Y-Achse (oberer Rand)
#define TOUCH_RAW_Y_MAX 3800           // [HW] größter erwarteter ADC-Rohwert der Y-Achse (unterer Rand)
#define TOUCH_SWAP_XY   1              // [Logik] gleicht die vertauschte X/Y-Benennung der Kommandobytes aus (Bedingung, s.u.)
#define TOUCH_INVERT_X  0              // [Logik] X-Achse nicht invertieren (Panel-Ausrichtung passt bereits)
#define TOUCH_INVERT_Y  1              // [Logik] Y-Achse invertieren (Panel-Ausrichtung erfordert Spiegelung)

static spi_device_handle_t s_touch_spi;  // [C] static-globale Variable | [HW] opaker Handle auf das am SPI-Bus registrierte XPT2046-Gerät
static lv_indev_t *s_indev;              // [C] Zeiger auf das registrierte LVGL-Input-Device, persistiert zwischen Aufrufen

// Führt eine einzelne XPT2046-Kommandotransaktion aus (Kanal/Achse laut
// cmd) und liefert den zugehörigen 12-bit-ADC-Rohwert zurück. Wird für
// X-, Y- und Z1-Auslesungen gleichermaßen verwendet.
static uint16_t xpt2046_read(uint8_t cmd)  // [C] static = interne Linkage, Parameter uint8_t = Kommandobyte, Rückgabe 16-bit-Rohwert
{
    // 3-Byte-Transfer ueber die internen FIFO-Register (kein DMA noetig,
    // der Bus selbst laeuft mit DMA fuer das Display).
    spi_transaction_t t = {                              // [HW] Struct für eine einzelne SPI-Transaktion
        .length = 24,                                    // [C] Designated Initializer (Feld-für-Feld per Name) | [HW] 24 Bit = 3 Byte Gesamtlänge der Transaktion
        .flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA,  // [C] '|' = bitweises ODER zur Kombination von Flags | [HW] Daten direkt in tx_data/rx_data statt über DMA-Puffer
        .tx_data = {cmd, 0x00, 0x00},                     // [C] Array-Initialisierung | [HW] erstes Byte = Kommando, zwei Dummy-Bytes zum Auslesen der Antwort
    };
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_touch_spi, &t));  // [HW] blockierende Polling-Übertragung (kein Interrupt-Overhead, da nur 3 Byte) | [C] Makro prüft Fehlercode und bricht bei Fehler das Programm ab

    // 12-bit result in bits [14:3] of the 16-bit reply
    return ((t.rx_data[1] << 8) | t.rx_data[2]) >> 3;  // [C] Bit-Schiebeoperatoren '<<'/'>>' und bitweises ODER '|' setzen die zwei empfangenen Bytes zusammen | [HW] extrahiert den 12-bit-ADC-Wert aus den Antwortbytes
}

// Bildet einen kalibrierten Rohwert (raw_min..raw_max) linear auf einen
// Pixel-Bereich (0..res-1) ab, klemmt dabei Ausreißer und kann die Achse
// bei Bedarf invertieren. Gemeinsame Hilfsfunktion für X- und Y-Achse.
static int32_t map_axis(int32_t raw, int32_t raw_min, int32_t raw_max,  // [C] static = interne Linkage, alle Parameter als int32_t
                        int32_t res, bool invert)                       // [C] res = Zielauflösung, invert = Achse spiegeln?
{
    if (raw < raw_min) raw = raw_min;   // [C] Vergleichsoperator + Zuweisung ohne Block | [Logik] untere Sättigung: Rohwert nicht kleiner als Kalibrierminimum
    if (raw > raw_max) raw = raw_max;   // [Logik] obere Sättigung: Rohwert nicht größer als Kalibriermaximum
    int32_t v = (raw - raw_min) * (res - 1) / (raw_max - raw_min);  // [C] Integer-Arithmetik: erst Multiplikation dann Division (Rundungsfehler minimieren) | [Logik] lineare Interpolation Rohwert -> Pixelkoordinate
    return invert ? (res - 1 - v) : v;  // [C] ternärer Operator '?:' | [Logik] bei invert=true Achse spiegeln (Wert vom oberen Ende aus zählen)
}

// LVGL-Read-Callback für das Touch-Input-Device: fragt Druckstärke (Z1) und
// bei Berührung die X/Y-Rohwerte ab, rechnet sie in Pixelkoordinaten um und
// füllt die von LVGL erwartete data-Struktur. Wird zyklisch vom
// LVGL-Input-Timer (alle TOUCH_POLL_PERIOD_MS) aufgerufen.
static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)  // [C] static = interne Linkage, void = kein Rückgabewert; data = Ausgabe-Struct für LVGL
{
    static bool was_pressed = false;   // [C] static lokale Variable: behält Wert zwischen Aufrufen (Funktions-persistenter State) | [Logik] merkt sich, ob der vorherige Poll schon gedrückt war (für einmaliges Logging)

    uint16_t z1 = xpt2046_read(CMD_READ_Z1);   // [HW] liest die Andruckstärke vom XPT2046 aus
    if (z1 < TOUCH_PRESS_THRESHOLD) {          // [C] Vergleichsoperator | [Logik] Andruck unter Schwelle -> kein gültiger Touch
        data->state = LV_INDEV_STATE_RELEASED; // [C] Zeiger-Feldzugriff per '->' | [HW] meldet LVGL "losgelassen"
        was_pressed = false;                   // [Logik] State zurücksetzen für die nächste Berührung
        return;                                // [Logik] früher Ausstieg, keine weitere Koordinatenmessung nötig
    }

    // Zwei Messungen mitteln, XPT2046-Rohwerte rauschen merklich.
    uint16_t raw_x = (xpt2046_read(CMD_READ_X) + xpt2046_read(CMD_READ_X)) / 2;  // [HW] Doppelmessung + Mittelwertbildung gegen ADC-Rauschen
    uint16_t raw_y = (xpt2046_read(CMD_READ_Y) + xpt2046_read(CMD_READ_Y)) / 2;  // [HW] gleiches Vorgehen für die zweite Achse

#if TOUCH_SWAP_XY                       // [C] Präprozessor-Bedingung: Code nur einkompiliert, wenn TOUCH_SWAP_XY != 0
    uint16_t tmp = raw_x;               // [C] klassischer 3-Schritte-Tausch über Hilfsvariable
    raw_x = raw_y;                      // [Logik] gleicht die vertauschte Kommando-Benennung (0x90/0xD0) aus
    raw_y = tmp;
#endif

    data->point.x = map_axis(raw_x, TOUCH_RAW_X_MIN, TOUCH_RAW_X_MAX,  // [C] verschachtelter Struct-Feldzugriff '.point.x' | [HW] Rohwert -> Pixel-X über Kalibrierspanne
                             DISPLAY_H_RES, TOUCH_INVERT_X);
    data->point.y = map_axis(raw_y, TOUCH_RAW_Y_MIN, TOUCH_RAW_Y_MAX,  // [HW] Rohwert -> Pixel-Y über Kalibrierspanne
                             DISPLAY_V_RES, TOUCH_INVERT_Y);
    data->state = LV_INDEV_STATE_PRESSED;  // [HW] meldet LVGL "gedrückt" mit den berechneten Koordinaten

    if (!was_pressed) {                    // [C] '!' = logisches NICHT | [Logik] nur beim Übergang unpressed->pressed loggen (kein Log-Spam pro Poll)
        ESP_LOGI(TAG, "Touch: raw=(%u,%u) -> px=(%d,%d) z1=%u",  // [C] %u/%d Format-Spezifizierer für vorzeichenlose/vorzeichenbehaftete Werte
                 raw_x, raw_y, (int)data->point.x, (int)data->point.y, z1);  // [C] explizite Casts auf int für den %d-Formatspezifizierer
        was_pressed = true;                // [Logik] State merken, damit der nächste Poll bei weiterhin gedrückt nicht erneut loggt
    }
}

// Registriert den XPT2046 als LVGL-Pointer-Input-Device: fügt ihn als
// zusätzliches Gerät auf dem bereits von display_init() eingerichteten
// SPI2-Bus hinzu und richtet den zyklischen Polling-Timer ein.
esp_err_t touch_init(void)             // [C] Rückgabetyp esp_err_t (Fehlercode); void = keine Parameter
{
    spi_device_interface_config_t devcfg = {   // [HW] Struct für die Geräteparameter des XPT2046 auf dem SPI-Bus
        .clock_speed_hz = 2 * 1000 * 1000,      // [C] Designated Initializer | [HW] 2 MHz SPI-Takt (niedriger als Display, XPT2046 ist langsamer)
        .mode = 0,                              // [HW] SPI-Mode 0 (CPOL=0, CPHA=0), vom XPT2046 gefordert
        .spics_io_num = PIN_NUM_T_CS,            // [HW] eigenes Chip-Select-Pin für den Touch-Controller
        .queue_size = 1,                         // [HW] nur eine Transaktion gleichzeitig in der Warteschlange (Polling-Betrieb)
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(TOUCH_HOST, &devcfg, &s_touch_spi),  // [C] Makro-Expansion mit early return bei Fehler | [HW] hängt den XPT2046 als weiteres Gerät an den geteilten SPI2-Bus
                        TAG, "spi_bus_add_device failed");

    s_indev = lv_indev_create();                          // [HW] legt ein neues LVGL-Input-Device-Objekt an
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);    // [HW] deklariert das Gerät als Zeiger-/Touch-Input (liefert x/y + gedrückt/losgelassen)
    lv_indev_set_read_cb(s_indev, touch_read_cb);         // [C] Funktionszeiger als Callback-Registrierung | [HW] registriert touch_read_cb als Datenquelle für LVGL
    lv_timer_set_period(lv_indev_get_read_timer(s_indev), TOUCH_POLL_PERIOD_MS);  // [HW] stellt das Abfrageintervall des internen LVGL-Timers auf 50 ms

    ESP_LOGI(TAG, "XPT2046 registered as LVGL pointer device");  // [Logik] Bestätigungs-Log nach erfolgreicher Registrierung
    return ESP_OK;                                          // [Logik] 0/ESP_OK signalisiert erfolgreiche Initialisierung nach außen
}

// Gibt das von touch_init() registrierte LVGL-Input-Device-Handle zurück,
// z.B. um es einem bestimmten Screen/Group zuzuordnen.
lv_indev_t *touch_get_lv_indev(void)  // [C] Rückgabetyp Zeiger auf lv_indev_t; void = keine Parameter
{
    return s_indev;                   // [Logik] gibt das gespeicherte Input-Device-Handle nach außen weiter
}
