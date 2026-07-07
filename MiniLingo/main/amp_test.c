#include <stdio.h>                     // [C] Standard-I/O-Header (hier ungenutzt eingebunden, üblich für printf)
#include <math.h>                      // [C] Mathe-Header für sinf() zur Sinuston-Erzeugung
#include "freertos/FreeRTOS.h"         // [HW] FreeRTOS-Basistypen/Makros, u.a. portMAX_DELAY, pdMS_TO_TICKS
#include "freertos/task.h"             // [Logik] Task-Funktionen wie vTaskDelay
#include "driver/i2s_std.h"            // [HW] ESP-IDF I2S-Standard-Modus-Treiber (Philips-I2S-Frames) für den Verstärker-Bus
#include "esp_log.h"                   // [Logik] ESP_LOGI/ESP_LOGE-Logging über UART-Konsole

static const char *TAG = "amp_test";   // [C] static = Datei-lokale Linkage | [Logik] Log-Tag zur Zuordnung der Ausgaben

#define I2S_BCLK_PIN GPIO_NUM_15       // [C] Präprozessor-Konstante | [HW] GPIO15 = Bit-Clock (BCLK) für den MAX98357A-Verstärker
#define I2S_LRC_PIN  GPIO_NUM_16       // [HW] GPIO16 = Word-Select/LRCLK, wählt L/R-Kanal im I2S-Frame
#define I2S_DIN_PIN  GPIO_NUM_17       // [HW] GPIO17 = Data-Out vom ESP32 zum Verstärker (Din am MAX98357A)

#define SAMPLE_RATE   44100            // [HW] Abtastrate 44,1 kHz für die Testton-Ausgabe
#define TONE_FREQ_HZ  1000             // [HW] Zielfrequenz des erzeugten Sinustons: 1 kHz
#define BUF_SAMPLES   256              // [Logik] Größe des Ringpuffers für einen Sinusschwingungs-Ausschnitt

static i2s_chan_handle_t tx_chan;      // [C] static-globale Variable | [HW] opaker Handle auf den I2S-TX-Kanal (Sender)

// Initialisiert den I2S1-TX-Kanal für den MAX98357A-Verstärker: legt den
// Kanal an, konfiguriert Takt/Slot/GPIOs im Philips-Standardmodus und
// aktiviert die Peripherie. Muss vor der ersten Sample-Ausgabe laufen.
static void amp_init(void)             // [C] static = interne Linkage; void = keine Parameter, kein Rückgabewert
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);  // [HW] Kanalkonfig: I2S-Port 1, ESP32 als Clock-Master
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_chan, NULL));  // [C] Makro prüft Rückgabewert, bricht bei Fehler ab | [HW] legt Kanal an: TX-Handle gesetzt, RX=NULL (nur Senden)

    i2s_std_config_t std_cfg = {       // [C] Designated Initializer (Feld-für-Feld-Initialisierung per Name)
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),  // [HW] Taktkonfig: leitet MCLK/BCLK-Teiler aus 44,1 kHz Sample-Rate ab
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),  // [HW] Slot-Konfig im Philips-I2S-Format, 16-bit-Samples, mono
        .gpio_cfg = {                   // [HW] Pin-Zuordnung des I2S-Busses
            .mclk = I2S_GPIO_UNUSED,   // [HW] kein Master-Clock-Pin nötig (MAX98357A braucht kein MCLK)
            .bclk = I2S_BCLK_PIN,      // [HW] Bit-Clock auf GPIO15
            .ws   = I2S_LRC_PIN,       // [HW] Word-Select (LRCLK) auf GPIO16
            .dout = I2S_DIN_PIN,       // [HW] Datenausgang zum Verstärker auf GPIO17
            .din  = I2S_GPIO_UNUSED,   // [HW] kein Dateneingang (reiner Sendekanal)
            .invert_flags = {          // [HW] Polaritäts-Invertierung der Taktleitungen
                .mclk_inv = false,     // [C] bool-Literal false | [HW] MCLK nicht invertiert
                .bclk_inv = false,     // [HW] BCLK nicht invertiert
                .ws_inv   = false,     // [HW] WS nicht invertiert
            },
        },
    };
    // MAX98357A plays the left channel summed mono regardless of slot mask
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;  // [C] Feldzugriff per '.' auf verschachteltes Struct | [HW] Slot-Maske auf linken Kanal gesetzt (Verstärker mischt ohnehin mono zusammen)

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_chan, &std_cfg));  // [HW] schreibt Clock/Slot/GPIO-Konfig in die I2S-Hardware-Register
    ESP_ERROR_CHECK(i2s_channel_enable(tx_chan));  // [HW] startet DMA + I2S-Peripherie; ab hier läuft der BCLK und Samples können gesendet werden
}

// Haupteinstiegspunkt des Testprogramms: erzeugt einen 1-kHz-Sinuston als
// Sample-Puffer und gibt ihn endlos über I2S an den Verstärker aus, um die
// Audio-Ausgabe-Hardware isoliert zu prüfen.
void app_main(void)                    // [C] ESP-IDF-Einstiegspunkt, wird vom Startup-Code aufgerufen
{
    amp_init();                        // [Logik] I2S-TX-Kanal einmalig initialisieren

    static int16_t samples[BUF_SAMPLES];         // [C] static = liegt im BSS, nicht auf dem Stack | [Logik] Puffer für einen vorgerechneten Sinusabschnitt
    const float amplitude = 8000.0f;             // [C] const float, 'f'-Suffix = float-Literal | [Logik] Lautstärke-Skalierung (weit unter INT16_MAX zur Vermeidung von Clipping)
    const float phase_inc = 2.0f * (float)M_PI * TONE_FREQ_HZ / SAMPLE_RATE;  // [C] Cast von double (M_PI) auf float | [Logik] Phasenzuwachs pro Sample für 1 kHz bei 44,1 kHz
    float phase = 0.0f;                          // [Logik] laufende Phase des Sinusgenerators, startet bei 0

    for (int i = 0; i < BUF_SAMPLES; i++) {      // [C] for-Schleife über den gesamten Sample-Puffer
        samples[i] = (int16_t)(amplitude * sinf(phase));  // [C] Cast auf int16_t | [HW] Sinuswert skaliert und quantisiert als PCM-Sample
        phase += phase_inc;                      // [C] Kombinierte Zuweisung += | [Logik] Phase für nächstes Sample fortschreiben
        if (phase > 2.0f * (float)M_PI) {        // [Logik] Phase eine volle Periode (2π) überschritten?
            phase -= 2.0f * (float)M_PI;         // [Logik] Phase in den Bereich [0, 2π) zurückfalten, verhindert numerisches Wegdriften
        }
    }

    ESP_LOGI(TAG, "Playing %d Hz tone...", TONE_FREQ_HZ);  // [Logik] Statusmeldung, dass die Endlosschleife beginnt

    while (1) {                                  // [C] Endlosschleife (1 ist immer wahr) | [Logik] Dauerausgabe des Testtons
        size_t bytes_written = 0;                // [C] Ausgabe-Variable, wird per Zeiger von i2s_channel_write gefüllt
        esp_err_t ret = i2s_channel_write(tx_chan, samples, sizeof(samples), &bytes_written, portMAX_DELAY);  // [HW] blockierender DMA-Write des Sample-Puffers auf den I2S-TX-Kanal | [C] '&' Adressoperator, portMAX_DELAY = unbegrenzt warten
        if (ret != ESP_OK) {                     // [C] Ungleichheitsoperator | [Logik] Fehlerfall abfangen
            ESP_LOGE(TAG, "i2s_channel_write failed: %s", esp_err_to_name(ret));  // [Logik] Fehlercode als lesbaren String loggen
        }
    }
}
