#include <stdio.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "esp_log.h"

static const char *TAG = "amp_test";

#define I2S_BCLK_PIN GPIO_NUM_15
#define I2S_LRC_PIN  GPIO_NUM_16
#define I2S_DIN_PIN  GPIO_NUM_17

#define SAMPLE_RATE   44100
#define TONE_FREQ_HZ  1000
#define BUF_SAMPLES   256

static i2s_chan_handle_t tx_chan;

static void amp_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_chan, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCLK_PIN,
            .ws   = I2S_LRC_PIN,
            .dout = I2S_DIN_PIN,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    // MAX98357A plays the left channel summed mono regardless of slot mask
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_chan));
}

void app_main(void)
{
    amp_init();

    static int16_t samples[BUF_SAMPLES];
    const float amplitude = 8000.0f;
    const float phase_inc = 2.0f * (float)M_PI * TONE_FREQ_HZ / SAMPLE_RATE;
    float phase = 0.0f;

    for (int i = 0; i < BUF_SAMPLES; i++) {
        samples[i] = (int16_t)(amplitude * sinf(phase));
        phase += phase_inc;
        if (phase > 2.0f * (float)M_PI) {
            phase -= 2.0f * (float)M_PI;
        }
    }

    ESP_LOGI(TAG, "Playing %d Hz tone...", TONE_FREQ_HZ);

    while (1) {
        size_t bytes_written = 0;
        esp_err_t ret = i2s_channel_write(tx_chan, samples, sizeof(samples), &bytes_written, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "i2s_channel_write failed: %s", esp_err_to_name(ret));
        }
    }
}
