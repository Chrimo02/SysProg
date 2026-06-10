// AFE-Pipeline (Spec Section 5, angepasst an die esp-sr-2.x-API:
// afe_config_init() + esp_afe_handle_from_config() statt des
// V1-Makros AFE_CONFIG_DEFAULT aus der Spec).
//
// afe_feed_task:  I2S0 (audio_in) -> AFE
// afe_fetch_task: AFE -> WakeNet-Ergebnis -> wake_word_on_detected(),
//                 danach Frames parallel an recorder (Phase 3, Spec
//                 Section 9.1) und MultiNet/commands (Phase 4).
// Beide auf Core 1, Prio 5 (Task-Tabelle Spec Section 8).
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_afe_sr_models.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "drivers/audio_in.h"
#include "commands.h"
#include "recorder.h"
#include "wake_word.h"
#include "afe_pipeline.h"

static const char *TAG = "afe_pipeline";

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

    int chunks = 0;

    while (1) {
        if (audio_in_read(buf, (size_t)chunk * channels) == ESP_OK) {
            // Mic-Pegel-Diagnose alle ~3s (Chunk = 32 ms), analog zu
            // mic_test.c. Debug-Level: fuer Gain-Tuning per
            // CONFIG_LOG_MAXIMUM_LEVEL_DEBUG aktivieren. Referenz vom
            // Wake-Word-Test 2026-06-10: Ruhe rms 10-30, "Alexa" in
            // normaler Lautstaerke rms 250-400 — detektiert zuverlaessig.
            if (++chunks % 100 == 0) {
                int64_t sum_sq = 0;
                int16_t peak = 0;
                for (int i = 0; i < chunk * channels; i++) {
                    sum_sq += (int32_t)buf[i] * buf[i];
                    int16_t a = buf[i] < 0 ? -buf[i] : buf[i];
                    if (a > peak) {
                        peak = a;
                    }
                }
                ESP_LOGD(TAG, "mic rms %.0f peak %d",
                         __builtin_sqrt((double)sum_sq / (chunk * channels)),
                         peak);
            }
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

        if (res->wakeup_state == WAKENET_DETECTED && !recorder_is_active()) {
            wake_word_on_detected(res->wake_word_index);
            // Capture ab dem naechsten Frame — res->data dieses Frames
            // enthaelt noch das Ende des Wake Words selbst.
            recorder_start();
            commands_session_start();
            continue;
        }

        if (recorder_is_active()) {
            // vad_cache enthaelt vom VAD zurueckgehaltene Samples, die
            // zeitlich VOR res->data liegen — zuerst anhaengen, damit
            // der Sprachanfang nicht abgeschnitten wird. MultiNet
            // bekommt dieselben Frames wie der Recorder.
            if (res->vad_cache_size > 0) {
                recorder_feed(res->vad_cache, (size_t)res->vad_cache_size,
                              true);
                commands_feed(res->vad_cache,
                              (size_t)res->vad_cache_size / sizeof(int16_t));
            }
            // Bei einem MultiNet-Match bricht der Callback (main.c) die
            // Aufnahme per recorder_abort() ab — recorder_feed dahinter
            // ist dann ein no-op.
            commands_feed(res->data, (size_t)res->data_size / sizeof(int16_t));
            recorder_feed(res->data, (size_t)res->data_size,
                          res->vad_state == VAD_SPEECH);
            if (!recorder_is_active()) {
                // Capture-Ende (Stille/Limit/Abort) beendet auch das
                // MultiNet-Fenster ("force timeout", Spec Section 9.1).
                commands_session_stop();
            }
        }
    }
}

esp_err_t afe_pipeline_init(srmodel_list_t *models)
{
    if (!models || models->num <= 0) {
        ESP_LOGE(TAG, "no models in partition 'model' — flashed?");
        return ESP_FAIL;
    }

    // "M" = 1 Mic-Kanal, 16 kHz. WakeNet-Model kommt automatisch aus
    // der Model-Partition (einziges WakeNet: wn9_alexa), VAD ist im
    // AFE_TYPE_SR-Default aktiv, AEC bleibt aus (Phase 8).
    afe_config_t *cfg =
        afe_config_init("M", models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
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
    // Index 1 = erstes Wake Word. 0 = Model-Default belassen
    // (wn9_alexa: 0.64 — siehe Model-Info-String im Boot-Log).
#if CONFIG_WAKE_WORD_THRESHOLD > 0
    s_afe_handle->set_wakenet_threshold(s_afe_data, 1,
                                        CONFIG_WAKE_WORD_THRESHOLD / 100.0f);
#endif

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
