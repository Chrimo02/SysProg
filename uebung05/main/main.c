#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "rom/ets_sys.h"

#define OW_PIN GPIO_NUM_4   // 1-Wire Bus läuft auf GPIO 4
#define TAG "DS18X20"

// 1-Wire Kommandos laut DS18x20 Datenblatt
#define CMD_READ_ROM        0x33  // ROM-Code (Seriennummer) des Sensors lesen
#define CMD_SKIP_ROM        0xCC  // ROM-Adressierung überspringen (nur 1 Sensor am Bus)
#define CMD_CONVERT_T       0x44  // Temperaturmessung starten
#define CMD_READ_SCRATCHPAD 0xBE  // Messergebnis aus internem RAM lesen

// Wrapper um ets_delay_us für lesbareren Code
static inline void delay_us(uint32_t us) { ets_delay_us(us); }

// Bus auf LOW ziehen: Pin als Output konfigurieren und 0 ausgeben
static void ow_low(void) {
    gpio_set_direction(OW_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(OW_PIN, 0);
}

// Bus freigeben: Pin als Input → externer Pull-up zieht Leitung auf HIGH
static void ow_release(void) {
    gpio_set_direction(OW_PIN, GPIO_MODE_INPUT);
}

// Aktuellen Pegel auf dem Bus lesen
static int ow_read(void) {
    return gpio_get_level(OW_PIN);
}

// Reset-Sequenz: Master signalisiert Busreset, Sensor antwortet mit Presence Pulse
// Gibt 1 zurück wenn ein Sensor gefunden wurde, sonst 0
static int ow_reset(void) {
    int presence;
    ow_low();           // Bus für 480µs auf LOW halten → Reset Signal
    delay_us(480);
    ow_release();       // Bus loslassen
    delay_us(70);       // Warten bis Sensor seinen Presence Pulse beginnt
    presence = !ow_read(); // LOW auf dem Bus = Sensor meldet sich (negiert weil active-low)
    delay_us(410);      // Rest des Zeitfensters abwarten (gesamt ~480µs nach Release)
    return presence;
}

// Ein einzelnes Bit senden
// 1-Bit: kurzer LOW-Impuls (6µs) → Sensor sampelt HIGH → logisch 1
// 0-Bit: langer LOW-Impuls (60µs) → Sensor sampelt LOW → logisch 0
static void ow_write_bit(int bit) {
    if (bit) {
        ow_low();
        delay_us(6);    // Kurz LOW: Sensor erkennt als 1
        ow_release();
        delay_us(64);   // Recovery-Zeit bis nächstes Bit
    } else {
        ow_low();
        delay_us(60);   // Lang LOW: Sensor erkennt als 0
        ow_release();
        delay_us(10);   // Recovery-Zeit
    }
}

// Ein einzelnes Bit lesen
// Master zieht kurz LOW → gibt frei → Sensor hält oder lässt los → Master sampelt
static int ow_read_bit(void) {
    int bit;
    ow_low();
    delay_us(6);        // Zeitfenster öffnen (Master initiiert)
    ow_release();
    delay_us(9);        // Warten bis Sensor seinen Pegel stabilisiert hat
    bit = ow_read();    // Pegel lesen: HIGH=1, LOW=0
    delay_us(55);       // Rest des 70µs Bit-Zeitfensters abwarten
    return bit;
}

// Ein Byte senden: 8 Bits einzeln, LSB (niedrigstes Bit) zuerst
static void ow_write_byte(uint8_t data) {
    for (int i = 0; i < 8; i++) {
        ow_write_bit(data & 1); // Aktuelles LSB senden
        data >>= 1;             // Nächstes Bit nach rechts schieben
    }
}

// Ein Byte empfangen: 8 Bits einzeln, LSB zuerst, zu einem Byte zusammensetzen
static uint8_t ow_read_byte(void) {
    uint8_t data = 0;
    for (int i = 0; i < 8; i++) {
        data >>= 1;                     // Platz für neues Bit schaffen (rechts reinrotieren)
        if (ow_read_bit()) data |= 0x80; // Empfangenes Bit an MSB-Position setzen
    }
    return data;
}

void app_main(void) {
    uint8_t rom[8];        // 64-bit ROM-Code: [Family(1)] [Serial(6)] [CRC(1)]
    uint8_t scratchpad[9]; // Internes Sensor-RAM: [Temp-LSB][Temp-MSB][Config...]

    // GPIO initialisieren: Reset-Zustand herstellen, dann als Input (Bus ist idle HIGH)
    gpio_reset_pin(OW_PIN);
    gpio_set_direction(OW_PIN, GPIO_MODE_INPUT);

    ESP_LOGI(TAG, "Starting DS18x20 example");

    // Aufgabe d) ROM Code auslesen (einmalig beim Start)
    if (!ow_reset()) {          // Prüfen ob Sensor am Bus vorhanden
        ESP_LOGE(TAG, "No device found!");
        return;
    }

    ow_write_byte(CMD_READ_ROM); // Sensor auffordern seinen ROM-Code zu senden
    for (int i = 0; i < 8; i++) {
        rom[i] = ow_read_byte(); // 8 Bytes = 64 Bit ROM-Code einlesen
    }

    printf("ROM Code: ");
    for (int i = 0; i < 8; i++) {
        printf("%02X", rom[i]);  // Hex-Ausgabe, z.B. "28FF1A2B3C4D5E6F"
    }
    printf("\n");

    // Aufgabe e) Temperatur dauerhaft auslesen
    while (1) {
        // Schritt 1: Temperaturmessung anstoßen (dauert bis zu 750ms)
        if (!ow_reset()) continue;         // Reset + Presence Check
        ow_write_byte(CMD_SKIP_ROM);       // ROM-Adressierung überspringen (nur 1 Sensor)
        ow_write_byte(CMD_CONVERT_T);      // Messbefehl senden
        vTaskDelay(pdMS_TO_TICKS(750));    // Warten bis Messung abgeschlossen (12-bit = 750ms)

        // Schritt 2: Messergebnis aus dem Scratchpad-RAM lesen
        if (!ow_reset()) continue;
        ow_write_byte(CMD_SKIP_ROM);
        ow_write_byte(CMD_READ_SCRATCHPAD);
        for (int i = 0; i < 9; i++) {
            scratchpad[i] = ow_read_byte(); // 9 Bytes lesen (Byte 8 = CRC)
        }

        // Temperatur berechnen: Byte 0 = LSB, Byte 1 = MSB → 16-bit signed (Q12.4 Format)
        // Die unteren 4 Bits sind Nachkommastellen → Division durch 16
        int16_t raw = (scratchpad[1] << 8) | scratchpad[0];
        float temp = (float)raw / 16.0f;
        printf("Temperature: %.2f °C\n", temp);

        vTaskDelay(pdMS_TO_TICKS(2000)); // 2 Sekunden bis zur nächsten Messung
    }
}
