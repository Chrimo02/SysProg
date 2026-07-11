// MAX98357A-Verstaerker an I2S1 TX, abgeleitet aus amp_test.c (Phase 0).
// Lautstaerke laeuft softwareseitig als Q15-Multiplikation ueber die
// Samples (Spec Section 12, Phase 4) — der MAX98357A hat keinen
// digitalen Gain-Eingang.
#include <stdatomic.h>

#include "freertos/FreeRTOS.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"

#include "audio_out.h"

static const char *TAG = "audio_out";

#define I2S_BCLK_PIN GPIO_NUM_15 // BCLK = Bit Clock, Takt fuer die einzelnen Bits
#define I2S_LRC_PIN  GPIO_NUM_16 // LRC = Left/Right Clock, Takt fuer den linken oder rechten Kanal
#define I2S_DIN_PIN  GPIO_NUM_17 // DIN = Data In, Pin fuer die Audiodaten

#define DEFAULT_RATE 16000 // Standard-Sample-Rate fuer die Wiedergabe (16 kHz, mono, 16 bit)

static i2s_chan_handle_t s_tx_chan; // Handle fuer den I2S-Transmitter-Kanal
static uint32_t s_rate; // Aktuelle Sample-Rate, initialisiert auf DEFAULT_RATE
static atomic_int_least32_t s_gain_q15 = AUDIO_OUT_GAIN_UNITY; //"int_least32_t" -> int mit min 32-bit -> AUDIO_OUT_GAIN_UNITY = 32768 (Q15) -> 1.0f -> bedeutet, dass die Lautstaerke unveraendert bleibt (1.0f = 100% Lautstaerke)

// Initialisiert den I2S1 TX-Kanal fuer den MAX98357A-Verstaerker. Konfiguriert die Pins, den Takt und das Datenformat. Setzt die Standard-Sample-Rate auf DEFAULT_RATE.
esp_err_t audio_out_init(void)
{
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER); // Konfiguriert den I2S-Kanal 1 als Master fuer die Wiedergabe von Audiodaten an den MAX98357A-Verstaerker
    // Ohne auto_clear loopt nach dem letzten i2s_channel_write der
    // DMA-Buffer-Inhalt endlos weiter -> "Knattern" nach jedem
    // Playback. auto_clear nullt gesendete Deskriptoren.
    chan_cfg.auto_clear = true; // Automatisch den DMA-Buffer leeren, nachdem Daten gesendet wurden
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx_chan, NULL),
                        TAG, "i2s_new_channel");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(DEFAULT_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO), // Datenformat: 16 Bit, Mono (nur linker Kanal wird verwendet)
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED, // MCLK (Master Clock) wird nicht verwendet, da der MAX98357A keinen MCLK benötigt
            .bclk = I2S_BCLK_PIN, // BCLK-Pin für den Bit Clock
            .ws   = I2S_LRC_PIN, // WS-Pin für Left/Right
            .dout = I2S_DIN_PIN, // .dout aus sicht des ESP32, also Daten vom ESP32 zum MAX98357A -> DIN-Pin des MAX98357A
            .din  = I2S_GPIO_UNUSED, // .din aus sicht des ESP32, also Daten vom MAX98357A zum ESP32 -> nicht verwendet, da der MAX98357A nur Audioausgabe hat
        },
    };
    // MAX98357A spielt den linken Kanal (SD-Pin offen = links/mono)
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx_chan, &std_cfg),
                        TAG, "i2s_channel_init_std_mode"); // Initialisiert den I2S-Kanal im Standardmodus mit der angegebenen Konfiguration.
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx_chan),
                        TAG, "i2s_channel_enable"); // Aktiviert den I2S-Kanal, sodass er Audiodaten senden kann.
    s_rate = DEFAULT_RATE; // Setzt die aktuelle Sample-Rate auf DEFAULT_RATE

    ESP_LOGI(TAG, "I2S1 TX up (%d Hz, 16 bit mono)", DEFAULT_RATE);
    return ESP_OK; 
}

// Setzt die Sample-Rate für die Audioausgabe. Wenn die neue Sample-Rate von der aktuellen abweicht, wird der I2S-Kanal deaktiviert, die Taktkonfiguration neu gesetzt und der Kanal wieder aktiviert.
esp_err_t audio_out_set_rate(uint32_t sample_rate)
{
    if (sample_rate == s_rate) {
        return ESP_OK;
    }
    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate); // I2S-Taktkonfiguration für die neue Sample-Rate erstellen
    ESP_RETURN_ON_ERROR(i2s_channel_disable(s_tx_chan), TAG, "disable"); // I2S-Kanal deaktivieren
    ESP_RETURN_ON_ERROR(i2s_channel_reconfig_std_clock(s_tx_chan, &clk_cfg), 
                        TAG, "reconfig_std_clock"); // I2S-Kanal mit neuer Taktkonfiguration reinitialisieren
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx_chan), TAG, "enable"); // I2S-Kanal wieder aktivieren
    s_rate = sample_rate; // Aktuelle Sample-Rate auf die neue Sample-Rate setzen
    ESP_LOGI(TAG, "sample rate -> %u Hz", (unsigned)sample_rate);
    return ESP_OK;
}

esp_err_t audio_out_write(int16_t *samples, size_t count)
{
    // 1. Lade aktuellen Lautstärke-Wert (threadsafe, atomar)
    int32_t gain = atomic_load(&s_gain_q15);
    
    // 2. Wenn Lautstärke ≠ normal (1.0):
    if (gain != AUDIO_OUT_GAIN_UNITY) {
        for (size_t i = 0; i < count; i++) {
            // Multipliziere Sample mit Gain, dann Rückkonvertierung (>> 15)
            // erst 16-bit sample zu 32-bit erweitern, damit multiplikation nicht überläuft. Danach wieder auf 16 bit zurückkonvertieren mit >> 15
            samples[i] = (int16_t)(((int32_t)samples[i] * gain) >> 15);
        }
    }
    
    // 3. Schreibe alle Samples zum I2S (Audio-Ausgang)
    size_t written = 0;  // i2s_channel_write meldet hier die tatsaechlich geschriebenen Bytes zurueck
    return i2s_channel_write(s_tx_chan, samples, count * sizeof(int16_t),
                             &written, portMAX_DELAY);
}


// threadsafe setter für gain
void audio_out_set_gain_q15(int32_t gain)
{
    atomic_store(&s_gain_q15, gain);
}

// threadsafe getter für gain
int32_t audio_out_get_gain_q15(void)
{
    return atomic_load(&s_gain_q15);
}
