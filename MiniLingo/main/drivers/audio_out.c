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

#define I2S_BCLK_PIN GPIO_NUM_15
#define I2S_LRC_PIN  GPIO_NUM_16
#define I2S_DIN_PIN  GPIO_NUM_17

#define DEFAULT_RATE 16000

static i2s_chan_handle_t s_tx_chan;
static uint32_t s_rate;
static atomic_int_least32_t s_gain_q15 = AUDIO_OUT_GAIN_UNITY; //"int_least32_t" -> int mit min 32-bit

esp_err_t audio_out_init(void)
{
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    // Ohne auto_clear loopt nach dem letzten i2s_channel_write der
    // DMA-Buffer-Inhalt endlos weiter -> "Knattern" nach jedem
    // Playback. auto_clear nullt gesendete Deskriptoren.
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx_chan, NULL),
                        TAG, "i2s_new_channel");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(DEFAULT_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCLK_PIN,
            .ws   = I2S_LRC_PIN,
            .dout = I2S_DIN_PIN,
            .din  = I2S_GPIO_UNUSED,
        },
    };
    // MAX98357A spielt den linken Kanal (SD-Pin offen = links/mono)
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx_chan, &std_cfg),
                        TAG, "i2s_channel_init_std_mode");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx_chan),
                        TAG, "i2s_channel_enable");
    s_rate = DEFAULT_RATE;

    ESP_LOGI(TAG, "I2S1 TX up (%d Hz, 16 bit mono)", DEFAULT_RATE);
    return ESP_OK;
}

esp_err_t audio_out_set_rate(uint32_t sample_rate)
{
    if (sample_rate == s_rate) {
        return ESP_OK;
    }
    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    ESP_RETURN_ON_ERROR(i2s_channel_disable(s_tx_chan), TAG, "disable");
    ESP_RETURN_ON_ERROR(i2s_channel_reconfig_std_clock(s_tx_chan, &clk_cfg),
                        TAG, "reconfig_std_clock");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx_chan), TAG, "enable");
    s_rate = sample_rate;
    ESP_LOGI(TAG, "sample rate -> %u Hz", (unsigned)sample_rate);
    return ESP_OK;
}

esp_err_t audio_out_write(int16_t *samples, size_t count)
{
    int32_t gain = atomic_load(&s_gain_q15);
    if (gain != AUDIO_OUT_GAIN_UNITY) {
        for (size_t i = 0; i < count; i++) {
            samples[i] = (int16_t)(((int32_t)samples[i] * gain) >> 15);
        }
    }

    size_t written = 0;
    return i2s_channel_write(s_tx_chan, samples, count * sizeof(int16_t),
                             &written, portMAX_DELAY);
}

void audio_out_set_gain_q15(int32_t gain)
{
    atomic_store(&s_gain_q15, gain);
}

int32_t audio_out_get_gain_q15(void)
{
    return atomic_load(&s_gain_q15);
}
