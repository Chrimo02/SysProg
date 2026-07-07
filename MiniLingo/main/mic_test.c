#include <stdio.h>                     // [C] Standard-I/O-Header (hier ungenutzt eingebunden)
#include <math.h>                      // [C] Mathe-Header für sqrt() zur RMS-Berechnung
#include <inttypes.h>                  // [C] Header für portable Format-Makros wie PRId32
#include "freertos/FreeRTOS.h"         // [HW] FreeRTOS-Basistypen/Makros, u.a. portMAX_DELAY, pdMS_TO_TICKS
#include "freertos/task.h"             // [Logik] Task-Funktionen wie vTaskDelay
#include "driver/i2s_std.h"            // [HW] ESP-IDF I2S-Standard-Modus-Treiber (Philips-I2S-Frames) für den Mikrofon-Bus
#include "esp_log.h"                   // [Logik] ESP_LOGI/ESP_LOGE-Logging über UART-Konsole

static const char *TAG = "mic_test";   // [C] static = Datei-lokale Linkage | [Logik] Log-Tag zur Zuordnung der Ausgaben

#define I2S_WS_PIN   GPIO_NUM_4        // [C] Präprozessor-Konstante | [HW] GPIO4 = Word-Select/LRCLK, wählt L/R-Kanal im I2S-Frame
#define I2S_SCK_PIN  GPIO_NUM_5        // [HW] GPIO5 = Bit-Clock (SCK/BCLK): taktet jedes einzelne Bit auf der I2S-Datenleitung
#define I2S_SD_PIN   GPIO_NUM_6        // [HW] GPIO6 = Serial-Data (SD/DIN): serielle PCM-Bits vom INMP441 ins ESP32

#define SAMPLE_RATE     16000          // [HW] Abtastrate 16 kHz für die Mikrofonaufnahme
#define SAMPLES_PER_READ 512           // [Logik] Anzahl Samples, die pro Schleifendurchlauf gelesen werden

static i2s_chan_handle_t rx_chan;      // [C] static-globale Variable | [HW] opaker Handle auf den I2S-RX-Kanal (Empfänger)

// Initialisiert den I2S0-RX-Kanal für das INMP441-Mikrofon: legt den Kanal
// an, konfiguriert Takt/Slot/GPIOs im Philips-Standardmodus und aktiviert
// die Peripherie. Muss vor dem ersten Lesezyklus laufen.
static void mic_init(void)             // [C] static = interne Linkage; void = keine Parameter, kein Rückgabewert
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);  // [HW] Kanalkonfig: I2S-Port 0, ESP32 als Clock-Master
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx_chan));  // [C] Makro prüft Rückgabewert, bricht bei Fehler ab | [HW] legt Kanal an: TX=NULL (nur Empfangen)

    i2s_std_config_t std_cfg = {       // [C] Designated Initializer (Feld-für-Feld-Initialisierung per Name)
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),  // [HW] Taktkonfig: leitet MCLK/BCLK-Teiler aus 16 kHz Sample-Rate ab
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),  // [HW] Slot-Konfig im Philips-I2S-Format, 32-bit-Slot passend zum INMP441-Frame, mono
        .gpio_cfg = {                   // [HW] Pin-Zuordnung des I2S-Busses
            .mclk = I2S_GPIO_UNUSED,   // [HW] kein Master-Clock-Pin nötig (INMP441 braucht kein MCLK)
            .bclk = I2S_SCK_PIN,       // [HW] Bit-Clock auf GPIO5
            .ws   = I2S_WS_PIN,        // [HW] Word-Select (LRCLK) auf GPIO4
            .dout = I2S_GPIO_UNUSED,   // [HW] kein Datenausgang (reiner Empfangskanal)
            .din  = I2S_SD_PIN,        // [HW] Dateneingang (Mic-Samples) auf GPIO6
            .invert_flags = {          // [HW] Polaritäts-Invertierung der Taktleitungen
                .mclk_inv = false,     // [C] bool-Literal false | [HW] MCLK nicht invertiert
                .bclk_inv = false,     // [HW] BCLK nicht invertiert
                .ws_inv   = false,     // [HW] WS nicht invertiert
            },
        },
    };
    // INMP441 sends data on the left channel when L/R is tied to GND
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;  // [C] Feldzugriff per '.' auf verschachteltes Struct | [HW] nur linken Slot einlesen (Mic-Pin L/R auf GND = links)

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_chan, &std_cfg));  // [HW] schreibt Clock/Slot/GPIO-Konfig in die I2S-Hardware-Register
    ESP_ERROR_CHECK(i2s_channel_enable(rx_chan));  // [HW] startet DMA + I2S-Peripherie; ab hier läuft der BCLK und Samples fließen
}

// Haupteinstiegspunkt des Testprogramms: liest fortlaufend Rohsamples vom
// INMP441 und gibt RMS- und Peak-Pegel aus, um Mikrofonverkabelung und
// Signalqualität isoliert zu prüfen.
void app_main(void)                    // [C] ESP-IDF-Einstiegspunkt, wird vom Startup-Code aufgerufen
{
    mic_init();                        // [Logik] I2S-RX-Kanal einmalig initialisieren

    static int32_t samples[SAMPLES_PER_READ];  // [C] static = liegt im BSS statt auf dem Stack | [Logik] Puffer für einen Lesezyklus Rohsamples

    while (1) {                        // [C] Endlosschleife | [Logik] Dauerhafte Pegelmessung
        size_t bytes_read = 0;         // [C] Ausgabe-Variable, wird per Zeiger von i2s_channel_read gefüllt
        esp_err_t ret = i2s_channel_read(rx_chan, samples, sizeof(samples), &bytes_read, portMAX_DELAY);  // [HW] blockierender DMA-Read vom I2S-RX-Kanal in den Rohpuffer | [C] '&' Adressoperator, portMAX_DELAY = unbegrenzt warten
        if (ret != ESP_OK) {           // [C] Ungleichheitsoperator | [Logik] Fehlerfall abfangen
            ESP_LOGE(TAG, "i2s_channel_read failed: %s", esp_err_to_name(ret));  // [Logik] Fehlercode als lesbaren String loggen
            continue;                  // [C] überspringt Rest der Schleifeniteration | [Logik] bei Lesefehler keine Auswertung, direkt nächster Versuch
        }

        size_t count = bytes_read / sizeof(int32_t);  // [C] Integer-Division: gelesene Bytes in Sample-Anzahl umrechnen
        int64_t sum_sq = 0;            // [C] 64-bit-Akkumulator, verhindert Overflow bei Quadratsummen | [Logik] Summe der quadrierten Samples für RMS
        int32_t peak = 0;              // [Logik] höchster bisher gesehener Betrag (Spitzenpegel)

        for (size_t i = 0; i < count; i++) {  // [C] for-Schleife über alle gelesenen Samples
            // INMP441 outputs 24-bit data left-justified in 32-bit frame
            int32_t sample = samples[i] >> 8;  // [C] arithmetischer Rechtsshift (Vorzeichen bleibt erhalten) | [HW] 24-bit-Rohwert aus dem 32-bit-Frame extrahieren
            sum_sq += (int64_t)sample * (int64_t)sample;  // [C] explizite Casts vor der Multiplikation verhindern 32-bit-Overflow | [Logik] Quadrat des Samples aufsummieren
            int32_t abs_sample = sample < 0 ? -sample : sample;  // [C] ternärer Operator berechnet den Betrag | [Logik] Vorzeichen entfernen für Peak-Vergleich
            if (abs_sample > peak) {   // [Logik] neuer Spitzenwert gefunden?
                peak = abs_sample;     // [Logik] Peak aktualisieren
            }
        }

        double rms = sqrt((double)sum_sq / count);  // [C] Cast auf double vor der Division für Fließkomma-Genauigkeit | [Logik] quadratischer Mittelwert (RMS) als Lautstärkemaß
        ESP_LOGI(TAG, "RMS: %8.1f  Peak: %8" PRId32, rms, peak);  // [C] Format-String-Verkettung mit Makro PRId32 für portablen int32_t-Formatspezifizierer | [Logik] Pegelwerte ausgeben

        vTaskDelay(pdMS_TO_TICKS(100));  // [HW] FreeRTOS-Task-Delay | [Logik] 100 ms Pause zwischen Messungen, damit die Logs lesbar bleiben
    }
}
