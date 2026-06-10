// AFE-Pipeline (Spec Section 5, angepasst an die esp-sr-2.x-API:
// afe_config_init() + esp_afe_handle_from_config() statt des
// V1-Makros AFE_CONFIG_DEFAULT aus der Spec).
//
// afe_feed_task:  I2S0 (audio_in) -> AFE
// afe_fetch_task: AFE -> WakeNet-Ergebnis -> wake_word_on_detected()
// Beide auf Core 1, Prio 5 (Task-Tabelle Spec Section 8).
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_afe_sr_models.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "model_path.h"
#include "sdkconfig.h"

#include "drivers/audio_in.h"
#include "wake_word.h"
#include "afe_pipeline.h"

static const char *TAG = "afe_pipeline";

static srmodel_list_t *s_models;
static const esp_afe_sr_iface_t *s_afe_handle;
static esp_afe_sr_data_t *s_afe_data;

static void afe_feed_task(void *arg)
{
    int chunk = s_afe_handle->get_feed_chunksize(s_afe_data);
    int channels = s_afe_handle->get_feed_channel_num(s_afe_data);
    ESP_LOGI(TAG, "feed task: chunk %d samples, %d channel(s)",
             chunk, channels);

    int16_t *buf = heap_caps_malloc(chunk * channels * sizeof(int16_t),
                                    MALLOC_CAP_INTERNAL);
    assert(buf);

    while (1) {
        if (audio_in_read(buf, (size_t)chunk * channels) == ESP_OK) {
            s_afe_handle->feed(s_afe_data, buf);
        }
    }
}

static void afe_fetch_task(void *arg)
{
    while (1) {
        afe_fetch_result_t *res =
            s_afe_handle->fetch_with_delay(s_afe_data, portMAX_DELAY);
        if (!res || res->ret_value == ESP_FAIL) {
            continue;
        }

        if (res->wakeup_state == WAKENET_DETECTED) {
            wake_word_on_detected(res->wake_word_index);
        }

        // Phase 3: hier res->data in den Recording-Buffer schreiben
        // und res->vad_state fuer das Capture-Ende auswerten.
    }
}

esp_err_t afe_pipeline_init(void)
{
    s_models = esp_srmodel_init("model");
    if (!s_models || s_models->num <= 0) {
        ESP_LOGE(TAG, "no models in partition 'model' — flashed?");
        return ESP_FAIL;
    }

    // "M" = 1 Mic-Kanal, 16 kHz. WakeNet-Model kommt automatisch aus
    // der Model-Partition (einziges WakeNet: wn9_alexa), VAD ist im
    // AFE_TYPE_SR-Default aktiv, AEC bleibt aus (Phase 8).
    afe_config_t *cfg =
        afe_config_init("M", s_models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (!cfg) {
        ESP_LOGE(TAG, "afe_config_init failed");
        return ESP_FAIL;
    }
    cfg->aec_init = false;
    cfg->wakenet_init = true;
    cfg->vad_init = true;
    cfg->afe_perferred_core = 1;
    cfg->afe_perferred_priority = 5;
    ESP_LOGI(TAG, "wakenet model: %s",
             cfg->wakenet_model_name ? cfg->wakenet_model_name : "(none)");

    s_afe_handle = esp_afe_handle_from_config(cfg);
    s_afe_data = s_afe_handle->create_from_config(cfg);
    afe_config_free(cfg);
    if (!s_afe_data) {
        ESP_LOGE(TAG, "AFE create_from_config failed");
        return ESP_FAIL;
    }

    // Kconfig-Threshold (0-100) auf WakeNet-Range 0.0-1.0 mappen;
    // Index 1 = erstes Wake Word.
    s_afe_handle->set_wakenet_threshold(s_afe_data, 1,
                                        CONFIG_WAKE_WORD_THRESHOLD / 100.0f);

    BaseType_t ok;
    ok = xTaskCreatePinnedToCore(afe_feed_task, "afe_feed", 4096, NULL, 5,
                                 NULL, 1);
    assert(ok == pdPASS);
    ok = xTaskCreatePinnedToCore(afe_fetch_task, "afe_fetch", 8192, NULL, 5,
                                 NULL, 1);
    assert(ok == pdPASS);

    ESP_LOGI(TAG, "AFE pipeline up, listening for wake word");
    return ESP_OK;
}
