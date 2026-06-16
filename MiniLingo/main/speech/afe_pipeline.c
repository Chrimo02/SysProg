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
// Charakter-Switcher (Phase B): aktiver Charakter (0..2) und das Mapping
// WakeNet-Model-Index (1-basiert) -> Charakter-Index. s_num_wakenets =
// tatsaechlich geladene Wakenets.
static atomic_int s_active_character;
static int s_char_for_model[1 + 3];  // Index 0 ungenutzt
static int s_num_wakenets;

void afe_pipeline_request_listening(void)
{
    atomic_store(&s_listen_request, true);
}

void afe_pipeline_set_active_character(int idx)
{
    atomic_store(&s_active_character, idx);
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
                // Charakter-Filter (Phase B): nur das Wakeword des
                // aktiven Charakters startet eine Session. Touch umgeht
                // den Filter (nutzt den aktiven Charakter).
                int midx = res->wakenet_model_index;
                int ch = (midx >= 1 && midx <= s_num_wakenets)
                             ? s_char_for_model[midx]
                             : -1;
                if (!touch && ch != atomic_load(&s_active_character)) {
                    ESP_LOGI(TAG, "wake ignored: model %d (char %d) != "
                                  "active %d", midx, ch,
                             atomic_load(&s_active_character));
                    continue;
                }
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

esp_err_t afe_pipeline_init(srmodel_list_t *models)
{
    if (!models || models->num <= 0) {
        ESP_LOGE(TAG, "no models in partition 'model' — flashed?");
        return ESP_FAIL;
    }

    // "M" = 1 Mic-Kanal, 16 kHz. WakeNet-Model kommt automatisch aus
    // der Model-Partition (einziges einkompiliertes WakeNet), VAD ist im
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

    // Phase B: drei Wakenets fuer die drei Charaktere. Die Reihenfolge
    // MUSS zur character_t-Tabelle in main.c passen (0=Mycroft, 1=Alexa,
    // 2=Jarvis). Zwei Modelle gehen ueber die Config (Model-Index 1/2),
    // das dritte kommt nach create per add_wakenet_model() dazu (Index 3).
    char *wn_mycroft = esp_srmodel_filter(models, ESP_WN_PREFIX, "mycroft");
    char *wn_alexa = esp_srmodel_filter(models, ESP_WN_PREFIX, "alexa");
    char *wn_jarvis = esp_srmodel_filter(models, ESP_WN_PREFIX, "jarvis");
    cfg->wakenet_model_name = wn_mycroft;
    cfg->wakenet_model_name_2 = wn_alexa;
    ESP_LOGI(TAG, "wakenets: 1=%s 2=%s 3(add)=%s",
             wn_mycroft ? wn_mycroft : "(none)",
             wn_alexa ? wn_alexa : "(none)",
             wn_jarvis ? wn_jarvis : "(none)");

    s_afe_handle = esp_afe_handle_from_config(cfg);
    s_afe_data = s_afe_handle->create_from_config(cfg);
    afe_config_free(cfg);
    if (!s_afe_data) {
        ESP_LOGE(TAG, "AFE create_from_config failed");
        return ESP_FAIL;
    }

    // Drittes Wakenet (Jarvis) zur Laufzeit ergaenzen -> Model-Index 3.
    // wn_jarvis zeigt in die srmodel-Liste (lebt programmweit), bleibt
    // also nach afe_config_free gueltig.
    s_num_wakenets = 2;
    if (wn_jarvis) {
        int n = s_afe_handle->add_wakenet_model(s_afe_data, wn_jarvis);
        ESP_LOGI(TAG, "add_wakenet_model(jarvis) -> %d wakenet(s)", n);
        if (n > 0) {
            s_num_wakenets = n;
        }
    }
    // Model-Index (1-basiert) -> Charakter-Index. Am Boot-Log
    // gegenpruefen, falls esp-sr die Indizes anders vergibt.
    s_char_for_model[1] = 0;  // Mycroft
    s_char_for_model[2] = 1;  // Alexa
    s_char_for_model[3] = 2;  // Jarvis

    // Kconfig-Threshold (0-100) auf WakeNet-Range 0.0-1.0 mappen;
    // Index 1 = erstes Wake Word. 0 = Model-Default belassen
    // (siehe Model-Info-String im Boot-Log).
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
