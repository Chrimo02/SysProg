// network_task (Spec Section 8): fuehrt die Cloud-Requests sequenziell
// aus — nie zwei TLS-Verbindungen parallel (Heap, Spec Section 13).
// Phase 5: Whisper-Transkription; Phase 6: Gemini-Antwort; TTS folgt
// in Phase 7.
#include <stdatomic.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "network/wifi.h"
#include "state.h"
#include "whisper.h"
#include "gemini.h"
#include "cloud.h"

static const char *TAG = "cloud";

// Transkript-Maximum: 8 s Sprache sind << 512 Zeichen.
#define TRANSCRIPT_MAX 512
// Gemini-Antwort: maxOutputTokens=200 bleibt unter 1 KB Text.
#define REPLY_MAX 1024

typedef struct {
    const uint8_t *wav;
    size_t bytes;
} cloud_job_t;

static QueueHandle_t s_queue;
static cloud_event_cb_t s_cb;
// busy von submit (afe_fetch_task) bis nach dem Ergebnis-Callback
// (network_task); gelesen auch vom afe_fetch_task (Wake-Word-Gate).
static atomic_bool s_busy;

static void network_task(void *arg)
{
    cloud_job_t job;
    static char text[TRANSCRIPT_MAX];
    static char reply[REPLY_MAX];

    while (1) {
        xQueueReceive(s_queue, &job, portMAX_DELAY);

        if (!wifi_is_connected()) {
            ESP_LOGW(TAG, "no WiFi, dropping query");
            s_cb(CLOUD_EVT_ERROR, "No WiFi connection");
        } else if (whisper_transcribe(job.wav, job.bytes, text,
                                      sizeof(text)) != ESP_OK) {
            s_cb(CLOUD_EVT_ERROR, "Transcription failed");
        } else {
            s_cb(CLOUD_EVT_TRANSCRIPT, text);
            // Gemini direkt im Anschluss — die Whisper-Verbindung ist
            // hier schon geschlossen (TLS strikt sequenziell).
            if (gemini_generate(text, reply, sizeof(reply)) == ESP_OK) {
                // History erst bei Erfolg fortschreiben: sie enthaelt
                // nur vollstaendige User/Model-Paare.
                chat_history_push(CHAT_ROLE_USER, text);
                chat_history_push(CHAT_ROLE_MODEL, reply);
                s_cb(CLOUD_EVT_REPLY, reply);
            } else {
                s_cb(CLOUD_EVT_ERROR, "Assistant failed");
            }
        }

        // Erst nach dem Callback freigeben: solange busy, startet kein
        // neues Wake Word / Capture (Recording-Buffer-Schutz).
        atomic_store(&s_busy, false);
    }
}

esp_err_t cloud_init(cloud_event_cb_t cb)
{
    esp_err_t err = chat_history_init();
    if (err != ESP_OK) {
        return err;
    }
    s_cb = cb;
    s_queue = xQueueCreate(1, sizeof(cloud_job_t));
    if (!s_queue) {
        return ESP_ERR_NO_MEM;
    }
    BaseType_t ok = xTaskCreatePinnedToCore(network_task, "network",
                                            16384, NULL, 3, NULL, 0);
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
}

void cloud_submit(const uint8_t *wav, size_t wav_bytes)
{
    if (atomic_exchange(&s_busy, true)) {
        ESP_LOGW(TAG, "pipeline busy, dropping query");
        return;
    }
    cloud_job_t job = { .wav = wav, .bytes = wav_bytes };
    xQueueSend(s_queue, &job, 0);
}

bool cloud_is_busy(void)
{
    return atomic_load(&s_busy);
}
