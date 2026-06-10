#include <stdio.h>
#include <math.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "esp_log.h"

static const char *TAG = "mic_test";

#define I2S_WS_PIN   GPIO_NUM_4
#define I2S_SCK_PIN  GPIO_NUM_5
#define I2S_SD_PIN   GPIO_NUM_6

#define SAMPLE_RATE     16000
#define SAMPLES_PER_READ 512

static i2s_chan_handle_t rx_chan;

static void mic_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx_chan));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_SCK_PIN,
            .ws   = I2S_WS_PIN,
            .dout = I2S_GPIO_UNUSED,
            .din  = I2S_SD_PIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    // INMP441 sends data on the left channel when L/R is tied to GND
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_chan));
}

void app_main(void)
{
    mic_init();

    static int32_t samples[SAMPLES_PER_READ];

    while (1) {
        size_t bytes_read = 0;
        esp_err_t ret = i2s_channel_read(rx_chan, samples, sizeof(samples), &bytes_read, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "i2s_channel_read failed: %s", esp_err_to_name(ret));
            continue;
        }

        size_t count = bytes_read / sizeof(int32_t);
        int64_t sum_sq = 0;
        int32_t peak = 0;

        for (size_t i = 0; i < count; i++) {
            // INMP441 outputs 24-bit data left-justified in 32-bit frame
            int32_t sample = samples[i] >> 8;
            sum_sq += (int64_t)sample * (int64_t)sample;
            int32_t abs_sample = sample < 0 ? -sample : sample;
            if (abs_sample > peak) {
                peak = abs_sample;
            }
        }

        double rms = sqrt((double)sum_sq / count);
        ESP_LOGI(TAG, "RMS: %8.1f  Peak: %8" PRId32, rms, peak);

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
