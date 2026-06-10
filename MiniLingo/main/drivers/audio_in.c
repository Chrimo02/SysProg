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

#define I2S_WS_PIN   GPIO_NUM_4
#define I2S_SCK_PIN  GPIO_NUM_5
#define I2S_SD_PIN   GPIO_NUM_6

#define SAMPLE_RATE  16000

// Der INMP441 liefert 24 bit links-buendig im 32-bit-Frame. Fuer die
// 16-bit-Ausgabe waere >>16 die neutrale Wandlung; das Mic ist aber
// leise, daher >>14 (= +12 dB) mit Saettigung. Bei Uebersteuerung im
// Wake-Word-Test ggf. auf 15 oder 16 erhoehen.
#define SAMPLE_SHIFT 14

static i2s_chan_handle_t s_rx_chan;
static int32_t *s_raw_buf;
static size_t s_raw_buf_samples;

esp_err_t audio_in_init(void)
{
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &s_rx_chan),
                        TAG, "i2s_new_channel");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_SCK_PIN,
            .ws   = I2S_WS_PIN,
            .dout = I2S_GPIO_UNUSED,
            .din  = I2S_SD_PIN,
        },
    };
    // INMP441 sendet auf dem linken Kanal (L/R an GND)
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx_chan, &std_cfg),
                        TAG, "i2s_channel_init_std_mode");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx_chan),
                        TAG, "i2s_channel_enable");

    ESP_LOGI(TAG, "I2S0 RX up (%d Hz, 32->16 bit shift %d)",
             SAMPLE_RATE, SAMPLE_SHIFT);
    return ESP_OK;
}

esp_err_t audio_in_read(int16_t *dest, size_t samples)
{
    if (s_raw_buf_samples < samples) {
        // Scratch-Buffer fuer die 32-bit-Rohdaten; waechst auf die
        // groesste angefragte Chunk-Groesse (praktisch konstant: die
        // AFE-Feed-Chunk-Groesse).
        heap_caps_free(s_raw_buf);
        s_raw_buf = heap_caps_malloc(samples * sizeof(int32_t),
                                     MALLOC_CAP_INTERNAL);
        if (!s_raw_buf) {
            s_raw_buf_samples = 0;
            return ESP_ERR_NO_MEM;
        }
        s_raw_buf_samples = samples;
    }

    size_t bytes_read = 0;
    esp_err_t err = i2s_channel_read(s_rx_chan, s_raw_buf,
                                     samples * sizeof(int32_t),
                                     &bytes_read, portMAX_DELAY);
    if (err != ESP_OK) {
        return err;
    }

    size_t count = bytes_read / sizeof(int32_t);
    for (size_t i = 0; i < count; i++) {
        int32_t v = s_raw_buf[i] >> SAMPLE_SHIFT;
        if (v > INT16_MAX) {
            v = INT16_MAX;
        } else if (v < INT16_MIN) {
            v = INT16_MIN;
        }
        dest[i] = (int16_t)v;
    }
    return ESP_OK;
}
