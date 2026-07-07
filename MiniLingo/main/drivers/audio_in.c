// INMP441-Mikrofon an I2S0 RX, abgeleitet aus mic_test.c (Phase 0).
// Liefert 16-bit-Samples für die AFE-Pipeline (esp-sr erwartet
// 16 kHz / 16 bit / mono).
#include "freertos/FreeRTOS.h"      // [C] Header-Include | [HW] FreeRTOS-Basistypen/Makros, u.a. portMAX_DELAY für blockierendes Warten
#include "driver/i2s_std.h"         // [HW] ESP-IDF I2S-Standard-Modus-Treiber (Philips-I2S-Frames) für den Mikrofon-Bus
#include "esp_check.h"              // [C] stellt ESP_RETURN_ON_ERROR-Makro bereit: Fehlerprüfung mit early return + Logging
#include "esp_heap_caps.h"          // [Logik] Heap-Allokator mit Capabilities (z.B. internes RAM statt PSRAM)
#include "esp_log.h"                // [Logik] ESP_LOGI/ESP_LOGE-Logging über UART-Konsole

#include "audio_in.h"               // [C] eigener Header: deklariert audio_in_init/audio_in_read (Prototyp-Abgleich)

static const char *TAG = "audio_in";  // [C] static = Datei-lokale (interne) Linkage | [Logik] Log-Tag zur Zuordnung der Ausgaben

#define I2S_WS_PIN   GPIO_NUM_4     // [C] Präprozessor-Konstante (Textersetzung) | [HW] GPIO4 = Word-Select/LRCLK, wählt L/R-Kanal im I2S-Frame
#define I2S_SCK_PIN  GPIO_NUM_5     // [HW] GPIO5 = Bit-Clock (SCK/BCLK): taktet jedes einzelne Bit auf der I2S-Datenleitung
#define I2S_SD_PIN   GPIO_NUM_6     // [HW] GPIO6 = Serial-Data (SD/DIN): serielle PCM-Bits vom INMP441 ins ESP32

#define SAMPLE_RATE  16000         // [HW] 16 kHz Abtastrate, von esp-sr (Wake-Word/AFE) vorgeschrieben

// Der INMP441 liefert 24 bit links-buendig im 32-bit-Frame. Fuer die
// 16-bit-Ausgabe waere >>16 die neutrale Wandlung; das Mic ist aber
// leise, daher >>14 (= +12 dB) mit Saettigung. Bei Uebersteuerung im
// Wake-Word-Test ggf. auf 15 oder 16 erhoehen.
#define SAMPLE_SHIFT 14            // [HW] Rechtsshift-Betrag 32->16 bit; jedes Bit weniger Shift = +6 dB Verstärkung des Mic-Signals

static i2s_chan_handle_t s_rx_chan;   // [C] static-globale Variable, persistiert zwischen Aufrufen | [HW] opaker Handle auf den I2S-RX-Kanal
static int32_t *s_raw_buf;            // [C] Zeiger auf Heap-Puffer für 32-bit-Rohsamples; NULL bis erste Allokation
static size_t s_raw_buf_samples;     // [C] size_t = vorzeichenlose Größe | [Logik] aktuelle Kapazität des Puffers in Samples

// Initialisiert den I2S0-RX-Kanal für das INMP441-Mikrofon: legt den Kanal
// an, konfiguriert Takt/Slot/GPIOs im Philips-Standardmodus und aktiviert
// die Peripherie. Muss einmalig vor dem ersten audio_in_read() laufen.
esp_err_t audio_in_init(void)        // [C] Rückgabetyp esp_err_t (Fehlercode); void = keine Parameter
{
    i2s_chan_config_t chan_cfg =                                   // [HW] Struct für allgemeine I2S-Kanal-Parameter (DMA, Rolle)
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);    // [HW] Makro füllt Defaults: I2S-Port 0, ESP32 als Clock-Master (erzeugt BCLK/WS selbst)
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &s_rx_chan),  // [C] Makro-Expansion mit early return bei Fehler | [HW] legt Kanal an: TX=NULL (nur RX)
                        TAG, "i2s_new_channel");                       // [Logik] Tag + Meldung für den Fehlerfall

    i2s_std_config_t std_cfg = {                                   // [C] Designated Initializer (Feld-für-Feld-Initialisierung per Name)
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),        // [HW] Taktkonfig: leitet MCLK/BCLK-Teiler aus 16 kHz Sample-Rate ab
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(           // [HW] Slot-Konfig im Philips-I2S-Format (Daten 1 BCLK nach WS-Flanke verschoben)
            I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),        // [HW] 32-bit-Slot-Breite passend zum INMP441-Frame, mono (ein Kanal)
        .gpio_cfg = {                                              // [HW] Pin-Zuordnung des I2S-Busses
            .mclk = I2S_GPIO_UNUSED,                               // [HW] kein Master-Clock-Pin nötig (INMP441 braucht kein MCLK)
            .bclk = I2S_SCK_PIN,                                   // [HW] Bit-Clock auf GPIO5
            .ws   = I2S_WS_PIN,                                    // [HW] Word-Select (LRCLK) auf GPIO4
            .dout = I2S_GPIO_UNUSED,                               // [HW] kein Datenausgang (reiner Empfangskanal)
            .din  = I2S_SD_PIN,                                    // [HW] Dateneingang (Mic-Samples) auf GPIO6
        },
    };
    // INMP441 sendet auf dem linken Kanal (L/R an GND)
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;               // [C] Feldzugriff per '.' auf verschachteltes Struct | [HW] nur linken Slot einlesen (Mic-Pin L/R auf GND = links)

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx_chan, &std_cfg),  // [HW] schreibt Clock/Slot/GPIO-Konfig in die I2S-Hardware-Register
                        TAG, "i2s_channel_init_std_mode");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx_chan),           // [HW] startet DMA + I2S-Peripherie; ab hier läuft der BCLK und Samples fließen
                        TAG, "i2s_channel_enable");

    ESP_LOGI(TAG, "I2S0 RX up (%d Hz, 32->16 bit shift %d)",     // [C] %d = Format-Spezifizierer für Dezimalausgabe (variadisches Makro)
             SAMPLE_RATE, SAMPLE_SHIFT);
    return ESP_OK;                                               // [Logik] 0/ESP_OK signalisiert erfolgreiche Initialisierung nach außen
}

// Liest 'samples' Samples vom I2S-RX-Kanal, wandelt die 32-bit-Rohframes
// per Rechtsshift + Sättigung in 16-bit-PCM um und schreibt sie nach dest.
// Blockiert, bis genug Daten per DMA angekommen sind (portMAX_DELAY).
esp_err_t audio_in_read(int16_t *dest, size_t samples)          // [C] Zeiger-Parameter dest (Ausgabe-Array), samples = angeforderte Anzahl
{
    if (s_raw_buf_samples < samples) {                          // [C] Vergleichsoperator, Bedingung steuert Verzweigung | [Logik] Puffer zu klein für Anfrage?
        // Scratch-Buffer fuer die 32-bit-Rohdaten; waechst auf die
        // groesste angefragte Chunk-Groesse (praktisch konstant: die
        // AFE-Feed-Chunk-Groesse).
        heap_caps_free(s_raw_buf);                             // [C] free(NULL) ist laut Standard erlaubt -> erster Aufruf harmlos
        s_raw_buf = heap_caps_malloc(samples * sizeof(int32_t),  // [C] sizeof liefert Typgröße zur Compile-Zeit | [Logik] Neuallokation: samples * 4 Byte
                                     MALLOC_CAP_INTERNAL);      // [HW] Capability-Flag: internes DRAM (DMA-fähig, schneller als externes PSRAM)
        if (!s_raw_buf) {                                       // [C] '!' = logisches NICHT; prüft auf NULL-Zeiger (malloc fehlgeschlagen)
            s_raw_buf_samples = 0;                             // [Logik] Kapazität zurücksetzen, damit nächster Aufruf neu alloziert
            return ESP_ERR_NO_MEM;                             // [Logik] Fehlercode nach oben durchreichen
        }
        s_raw_buf_samples = samples;                          // [Logik] neue Kapazität für künftige Aufrufe merken
    }

    size_t bytes_read = 0;                                     // [C] Ausgabe-Variable, wird per Zeiger von i2s_channel_read gefüllt
    esp_err_t err = i2s_channel_read(s_rx_chan, s_raw_buf,     // [HW] blockierender DMA-Read vom I2S-RX-Kanal in den Rohpuffer
                                     samples * sizeof(int32_t),  // [HW] angeforderte Byte-Anzahl (32-bit-Frames vom INMP441)
                                     &bytes_read, portMAX_DELAY);  // [C] '&' = Adressoperator, Ausgabe per Zeiger | [HW] portMAX_DELAY = unbegrenzt auf DMA-Daten warten
    if (err != ESP_OK) {                                       // [C] '!=' Ungleichheitsoperator
        return err;                                            // [Logik] I2S-Fehler unverändert weiterreichen
    }

    size_t count = bytes_read / sizeof(int32_t);              // [C] Integer-Division: gelesene Bytes in Sample-Anzahl umrechnen
    for (size_t i = 0; i < count; i++) {                      // [C] for-Schleife über alle gelesenen Samples; i++ = Post-Inkrement
        int32_t v = s_raw_buf[i] >> SAMPLE_SHIFT;             // [C] arithmetischer Rechtsshift (Vorzeichen bleibt erhalten) | [HW] 32-bit-Rohwert auf 16-bit-Bereich skalieren (+Verstärkung)
        if (v > INT16_MAX) {                                  // [Logik] Übersteuerung nach oben? (Sättigungsprüfung, +32767)
            v = INT16_MAX;                                    // [Logik] auf Maximalwert klemmen, verhindert Wrap-Around-Knacken im Audio
        } else if (v < INT16_MIN) {                           // [Logik] Übersteuerung nach unten? (-32768)
            v = INT16_MIN;                                    // [Logik] auf Minimalwert klemmen
        }
        dest[i] = (int16_t)v;                                 // [C] explizite Typumwandlung (Cast) 32->16 bit; Schreibzugriff ins Ziel-Array
    }
    return ESP_OK;                                            // [Logik] Erfolg; dest enthält jetzt 'count' fertige 16-bit-Samples
}
