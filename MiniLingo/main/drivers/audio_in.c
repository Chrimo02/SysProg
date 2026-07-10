// INMP441-Mikrofon an I2S0 RX, abgeleitet aus mic_test.c (Phase 0).
// Liefert 16-bit-Samples fuer die AFE-Pipeline (esp-sr erwartet
// 16 kHz / 16 bit / mono).
#include "freertos/FreeRTOS.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "audio_in.h"

static const char *TAG = "audio_in";


 // ich hab beim mic den L/R Pin zu GND verbunden, wodurch das Mic auf dem linken Kanal sendet
#define I2S_WS_PIN   GPIO_NUM_4 // Word Select (L/R) Pin, auch LRCK oder LRC genannt. 0 = links, 1 = rechts. Das INMP441 sendet auf dem linken Kanal, daher ist der Pin auf GND gelegt.
#define I2S_SCK_PIN  GPIO_NUM_5 // Serial Clock Pin, auch BCLK genannt. Taktet die Datenübertragung.
#define I2S_SD_PIN   GPIO_NUM_6 // Serial Data Pin -> überträgt Audiodaten vom mic zum esp

#define SAMPLE_RATE  16000 // 16 kHz, mono, 16 bit. Das INMP441 liefert 24 bit links-buendig im 32-bit-Frame. Wir wandeln auf 16 bit um (siehe SAMPLE_SHIFT unten).

// Der INMP441 liefert 24 bit links-buendig im 32-bit-Frame. Fuer die
// 16-bit-Ausgabe waere >>16 die neutrale Wandlung; das Mic ist aber
// leise, daher >>14 (= +12 dB) mit Saettigung. Bei Uebersteuerung im
// Wake-Word-Test ggf. auf 15 oder 16 erhoehen.
#define SAMPLE_SHIFT 14

static i2s_chan_handle_t s_rx_chan; // Handle für den I2S RX-Kanal (Empfang von Audiodaten vom Mikrofon)
static int32_t *s_raw_buf; // Scratch-Buffer fuer die 32-bit-Rohdaten vom I2S. Wird auf die groesste angefragte Chunk-Groesse vergrößert, um wiederholte Allokationen zu vermeiden.
static size_t s_raw_buf_samples; // Anzahl der Samples im Scratch-Buffer s_raw_buf. Wird auf die groesste angefragte Chunk-Groesse vergrößert, um wiederholte Allokationen zu vermeiden.

// Initialisiert den I2S0 RX-Kanal für das INMP441-Mikrofon. Konfiguriert die Pins, den Takt und die Datenformatierung. Liefert 16-bit-Samples für die AFE-Pipeline.
esp_err_t audio_in_init(void)
{
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);  // Konfiguriert den I2S-Kanal 0 als Master für den Empfang von Audiodaten vom Mikrofon
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &s_rx_chan),
                        TAG, "i2s_new_channel"); // Erstellt einen neuen I2S-Kanal mit der angegebenen Konfiguration. Der RX-Kanal wird in s_rx_chan gespeichert.

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE), // I2S-Takt für die gewünschte Sample-Rate (16 kHz)
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO), // Datenformat: 32-bit pro Sample, Mono-Modus (nur linker Kanal wird verwendet)
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED, // Master Clock Pin wird nicht verwendet
            .bclk = I2S_SCK_PIN, // Serial Clock Pin (BCLK) für die Datenübertragung
            .ws   = I2S_WS_PIN, // Word Select Pin (L/R) für die Kanalwahl (links/rechts)
            .dout = I2S_GPIO_UNUSED, // Data Out Pin wird nicht verwendet, da wir nur empfangen
            .din  = I2S_SD_PIN, // Data In Pin für den Empfang von Audiodaten vom Mikrofon
        },
    };
    // INMP441 sendet auf dem linken Kanal (L/R an GND)
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx_chan, &std_cfg),
                        TAG, "i2s_channel_init_std_mode"); // Initialisiert den I2S-Kanal im Standardmodus mit der angegebenen Konfiguration.
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx_chan),
                        TAG, "i2s_channel_enable"); // Aktiviert den I2S-Kanal, sodass er Audiodaten empfangen kann.

    ESP_LOGI(TAG, "I2S0 RX up (%d Hz, 32->16 bit shift %d)",
             SAMPLE_RATE, SAMPLE_SHIFT);
    return ESP_OK;
}

// function, die Audiodaten vom I2S0 RX-Kanal liest und in den Zielpuffer "dest" schreibt.
// "samples" = Anzahl der Samples, die gelesen werden sollen  
esp_err_t audio_in_read(int16_t *dest, size_t samples)
{
    if (s_raw_buf_samples < samples) { // Scratch-Buffer vergrößern, wenn die angefragte Chunk-Größe größer ist als der aktuelle Buffer
        heap_caps_free(s_raw_buf);
        s_raw_buf = heap_caps_malloc(samples * sizeof(int32_t),
                                     MALLOC_CAP_INTERNAL); // Scratch-Buffer im internen RAM allokieren, um die 32-bit-Rohdaten vom I2S zu speichern
        if (!s_raw_buf) {
            s_raw_buf_samples = 0;
            return ESP_ERR_NO_MEM;
        }
        s_raw_buf_samples = samples; // Scratch-Buffer-Größe aktualisieren
    }

    size_t bytes_read = 0;
    esp_err_t err = i2s_channel_read(s_rx_chan, s_raw_buf,
                                     samples * sizeof(int32_t),
                                     &bytes_read, portMAX_DELAY); // Liest die angeforderte Anzahl von Samples vom I2S-Kanal in den Scratch-Buffer. Wartet unbegrenzt, bis die Daten verfügbar sind.
    if (err != ESP_OK) {
        return err;
    }


    size_t count = bytes_read / sizeof(int32_t); // Anzahl der tatsächlich gelesenen Samples berechnen
    for (size_t i = 0; i < count; i++) { // Wandelt die 32-bit-Rohdaten in 16-bit-Samples um, indem sie um SAMPLE_SHIFT Bits nach rechts verschoben werden. Sättigung wird berücksichtigt, um Übersteuerung zu vermeiden.
        int32_t v = s_raw_buf[i] >> SAMPLE_SHIFT; // >>14 bedeutet: nimm 32-bit wert, teile durch 2^14 = 16384, um auf 16-bit zu skalieren. Das entspricht einer Verstärkung von +12 dB.
        if (v > INT16_MAX) { // Übersteuerung vermeiden
            v = INT16_MAX;
        } else if (v < INT16_MIN) {
            v = INT16_MIN;
        }
        dest[i] = (int16_t)v;
    }
    return ESP_OK;
}
