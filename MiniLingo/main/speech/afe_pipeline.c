// AFE-Pipeline (Spec Section 5, angepasst an die esp-sr-2.x-API:
// afe_config_init() + esp_afe_handle_from_config() statt des
// V1-Makros AFE_CONFIG_DEFAULT aus der Spec).
//
// afe_feed_task:  I2S0 (audio_in) -> AFE
// afe_fetch_task: AFE -> WakeNet-Ergebnis -> wake_word_on_detected(),
//                 danach Frames parallel an recorder (Phase 3, Spec
//                 Section 9.1) und MultiNet/commands (Phase 4).
// Beide auf Core 1, Prio 5 (Task-Tabelle Spec Section 8).
#include <stdatomic.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_afe_sr_models.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_wn_models.h"
#include "sdkconfig.h"

#include "drivers/audio_in.h"
#include "network/cloud.h"
#include "commands.h"
#include "recorder.h"
#include "wake_word.h"
#include "afe_pipeline.h"

static const char *TAG = "afe_pipeline";

static const esp_afe_sr_iface_t *s_afe_handle;
static esp_afe_sr_data_t *s_afe_data;
// Touch-Trigger (Mic-Button): gesetzt aus dem LVGL-Task, konsumiert
// vom afe_fetch_task mit dem naechsten AFE-Frame (~32 ms Latenz).
static atomic_bool s_listen_request;

// Charakter-Wechsel ohne Reboot (Phase B2):
static srmodel_list_t *s_models;       // fuer create_from_config beim Reconfigure
static int s_character_idx = -1;       // aktuell geladenes Wakeword (0/1/2)
// -1 = keine Anforderung, sonst Ziel-Charakter. Gesetzt aus beliebigem
// Task (afe_pipeline_request_wakeword), konsumiert im afe_fetch_task.
static atomic_int s_reconfig_char;
// Handshake zwischen fetch- (Reconfigure) und feed-Task: feed parkt,
// damit waehrend destroy/create kein feed() auf s_afe_data laeuft.
static atomic_bool s_feed_park;
static atomic_bool s_feed_parked;

void afe_pipeline_request_listening(void)
{
    atomic_store(&s_listen_request, true);
}

void afe_pipeline_request_wakeword(int character_idx)
{
    atomic_store(&s_reconfig_char, character_idx);
}

// AFE mit dem Wakeword des Charakters bauen (aus init + reconfigure).
// Setzt s_afe_handle/s_afe_data/s_character_idx; wendet Threshold an.
static esp_err_t create_afe(int character_idx)
{
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

    const char *kw = (character_idx == 1)   ? "alexa"
                     : (character_idx == 2) ? "jarvis"
                                            : "mycroft";
    char *wn = esp_srmodel_filter(s_models, ESP_WN_PREFIX, kw);
    cfg->wakenet_model_name = wn;
    cfg->wakenet_model_name_2 = NULL;
    ESP_LOGI(TAG, "wakenet model: %s (character %d)",
             wn ? wn : "(none)", character_idx);

    s_afe_handle = esp_afe_handle_from_config(cfg);
    s_afe_data = s_afe_handle->create_from_config(cfg);
    afe_config_free(cfg);
    if (!s_afe_data) {
        ESP_LOGE(TAG, "AFE create_from_config failed");
        return ESP_FAIL;
    }

    // Kconfig-Threshold (0-100) auf WakeNet-Range 0.0-1.0 mappen;
    // Index 1 = erstes Wake Word. 0 = Model-Default belassen.
#if CONFIG_WAKE_WORD_THRESHOLD > 0
    s_afe_handle->set_wakenet_threshold(s_afe_data, 1,
                                        CONFIG_WAKE_WORD_THRESHOLD / 100.0f);
#endif
    s_character_idx = character_idx;
    return ESP_OK;
}

// Wakeword zur Laufzeit umstellen. NUR aus dem afe_fetch_task am
// Schleifenkopf aufrufen (zwischen zwei fetch-Calls) — dann ist weder
// fetch noch (nach dem Park-Handshake) feed in einem AFE-Call.
static void reconfigure_wakeword(int character_idx)
{
    if (character_idx == s_character_idx) {
        return;  // schon aktiv
    }
    int prev = s_character_idx;
    ESP_LOGI(TAG, "reconfigure wakenet: character %d -> %d",
             prev, character_idx);

    // Feed-Task parken und warten, bis er wirklich aus feed() raus ist.
    atomic_store(&s_feed_park, true);
    while (!atomic_load(&s_feed_parked)) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    s_afe_handle->destroy(s_afe_data);
    s_afe_data = NULL;
    esp_err_t err = create_afe(character_idx);
    if (err != ESP_OK && prev >= 0) {
        ESP_LOGE(TAG, "reconfigure failed, restoring character %d", prev);
        create_afe(prev);  // best effort: alte Pipeline zurueckholen
    }

    atomic_store(&s_feed_park, false);
    ESP_LOGI(TAG, "wakenet reconfigured, free internal: %u B (largest block %u B)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

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
        // Park-Handshake fuer den Wakeword-Reconfigure (Phase B2): der
        // fetch-Task setzt s_feed_park, wir bestaetigen mit s_feed_parked
        // und ruehren s_afe_data nicht an, bis er wieder freigibt.
        if (atomic_load(&s_feed_park)) {
            atomic_store(&s_feed_parked, true);
            while (atomic_load(&s_feed_park)) {
                vTaskDelay(pdMS_TO_TICKS(5));
            }
            atomic_store(&s_feed_parked, false);
            continue;
        }
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
    // MultiNet-Session, die nur fuer den "stop"-Command waehrend der
    // TTS-Wiedergabe laeuft (Phase 7, Spec Section 9.6: WakeNet aus,
    // MultiNet an). Nur in diesem Task benutzt.
    bool speak_session = false;

    while (1) {
        // Charakter-Wechsel (Phase B2): Wakeword hier umstellen, zwischen
        // zwei fetch-Calls — nie mitten in fetch_with_delay, und das
        // vorige res ist ausgewertet (kein Use-after-free auf res->data).
        int reconfig = atomic_exchange(&s_reconfig_char, -1);
        if (reconfig >= 0) {
            reconfigure_wakeword(reconfig);
        }

        afe_fetch_result_t *res =
            s_afe_handle->fetch_with_delay(s_afe_data, portMAX_DELAY);
        if (!res || res->ret_value == ESP_FAIL) {
            continue;
        }

        // SPEAKING: MultiNet mitlaufen lassen, damit "stop" die
        // Wiedergabe abbrechen kann. Laeuft die Session in ihren
        // 6-s-Timeout oder matcht ein False-Positive, wird sie hier
        // einfach neu gestartet, solange die Wiedergabe dauert; nach
        // dem Wiedergabe-Ende wird sie beendet. (Eine normale
        // Wake-Word-Session kann hier nicht aktiv sein — waehrend der
        // Cloud-Pipeline startet keine, s.u.)
        bool speaking = cloud_is_speaking();
        if (speaking && !commands_session_active()) {
            commands_session_start();
            speak_session = true;
        } else if (!speaking && speak_session) {
            commands_session_stop();
            speak_session = false;
        }

        // Solange die Cloud-Pipeline laeuft, kein neues Wake Word
        // und kein Touch-Trigger annehmen: recorder_start() wuerde
        // den WAV-Buffer ueberschreiben, den der Whisper-Upload
        // gerade liest. Ein Touch waehrend einer laufenden Session
        // wird konsumiert und ignoriert (no-op, Session laeuft schon).
        bool touch = atomic_exchange(&s_listen_request, false);
        if ((res->wakeup_state == WAKENET_DETECTED || touch)
            && !recorder_is_active() && !cloud_is_busy()
            && !commands_session_active()) {
            if (touch && res->wakeup_state != WAKENET_DETECTED) {
                wake_word_on_touch();
            } else {
                wake_word_on_detected(res->wake_word_index);
            }
            // Capture ab dem naechsten Frame — res->data dieses Frames
            // enthaelt noch das Ende des Wake Words selbst (bzw. beim
            // Touch das Tipp-Geraeusch).
            recorder_start();
            commands_session_start();
            continue;
        }

        if (recorder_is_active() || commands_session_active()) {
            // Die MultiNet-Session laeuft bewusst WEITER, wenn der
            // Recorder schon bei 1,5 s Stille stoppt: MultiNets
            // Endpoint braucht bei langen Phrasen ("set timer five
            // minutes") laenger als das Capture — sonst wird die
            // Erkennung kurz vor dem Match abgeschnitten. Session-Ende
            // ist erst Match oder MultiNet-Timeout (6 s); ab Phase 5
            // darf der Cloud-Upload deshalb erst nach dem
            // Session-Ende starten.
            //
            // Der vad_cache geht weder an MultiNet noch (seit Phase 8)
            // an den Recorder: er ueberlappt mit bereits gelieferten
            // Frames statt nur aelteres Audio nachzuliefern — der
            // MultiNet-Decoder hoerte verdoppelte Phrasenanfaenge
            // ("SfPSfT" = "set set", Session 6), und im WAV war die
            // Doppelung hoerbar (Whisper: "Set Set Timer", Session 7).
            // Das Capture beginnt ohnehin direkt nach dem Wake Word,
            // lange bevor der Query-Satz anfaengt — der Sprachanfang
            // ist also auch ohne vad_cache vollstaendig.
            //
            // Bei einem MultiNet-Match bricht der Callback (main.c) die
            // Aufnahme per recorder_abort() ab — recorder_feed dahinter
            // ist dann ein no-op (ebenso nach Capture-Ende).
            commands_feed(res->data, (size_t)res->data_size / sizeof(int16_t));
            recorder_feed(res->data, (size_t)res->data_size,
                          res->vad_state == VAD_SPEECH);
        }
    }
}

esp_err_t afe_pipeline_init(srmodel_list_t *models, int character_idx)
{
    if (!models || models->num <= 0) {
        ESP_LOGE(TAG, "no models in partition 'model' — flashed?");
        return ESP_FAIL;
    }
    s_models = models;
    atomic_store(&s_reconfig_char, -1);

    // "M" = 1 Mic-Kanal, 16 kHz, VAD im AFE_TYPE_SR-Default, kein AEC.
    // Nur das EINE Wakeword des aktiven Charakters laden (0=Mycroft,
    // 1=Alexa, 2=Jarvis). Drei Wakenets gleichzeitig sprengen den
    // internen RAM; der Wechsel tauscht das Modell zur Laufzeit
    // (afe_pipeline_request_wakeword -> reconfigure_wakeword).
    esp_err_t err = create_afe(character_idx);
    if (err != ESP_OK) {
        return err;
    }

    BaseType_t ok;
    ok = xTaskCreatePinnedToCore(afe_feed_task, "afe_feed", 4096, NULL, 5,
                                 NULL, 1);
    assert(ok == pdPASS);
    ok = xTaskCreatePinnedToCore(afe_fetch_task, "afe_fetch", 8192, NULL, 5,
                                 NULL, 1);
    assert(ok == pdPASS);

    ESP_LOGI(TAG, "AFE pipeline up (character %d), free internal: %u B "
                  "(largest block %u B)", character_idx,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    return ESP_OK;
}
