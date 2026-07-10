// network_task (Spec Section 8): fuehrt die Cloud-Requests sequenziell
// aus — nie zwei TLS-Verbindungen parallel (Heap, Spec Section 13).
// Phase 5: Whisper-Transkription; Phase 6: Gemini-Antwort; Phase 7:
// Gemini-TTS + Wiedergabe (der Task blockiert bis zum Wiedergabe-Ende,
// damit busy/speaking den SPEAKING-State der Spec abbilden).
#include <stdatomic.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "local/canned_audio.h"
#include "local/timer.h"
#include "local/volume.h"
#include "network/wifi.h"
#include "state.h"
#include "whisper.h"
#include "llm.h"
#include "tts.h"
#include "cloud.h"

static const char *TAG = "cloud";

// Transkript-Maximum: 8 s Sprache sind << 512 Zeichen.
#define TRANSCRIPT_MAX 512
// Gemini-Antwort: maxOutputTokens=200 bleibt unter 1 KB Text.
#define REPLY_MAX 1024

// speak_only = true: kein Whisper/LLM, nur speak_reply(text).
// text wird bei cloud_speak() per Wert reinkopiert.
#define SPEAK_TEXT_MAX 80

// cloud_job_t ist ein Job-Paket, das an die Cloud gesendet wird
typedef struct {
    const uint8_t *wav; 
    size_t bytes;
    bool speak_only;
    char text[SPEAK_TEXT_MAX];
} cloud_job_t;

static QueueHandle_t s_queue;
static cloud_event_cb_t s_cb;
// busy von submit (afe_fetch_task) bis nach dem Ergebnis-Callback
// (network_task); gelesen auch vom afe_fetch_task (Wake-Word-Gate).
static atomic_bool s_busy;
// speaking = TTS-Wiedergabe laeuft (Teilintervall von busy); gelesen
// vom afe_fetch_task (MultiNet-"stop"-Session) und von main.c.
static atomic_bool s_speaking;
// Laufzeit-Schalter (Phase 10b): TTS-Vorlesen an/aus ueber den Button.
// CONFIG_TTS_SPEAK_REPLIES gibt nur noch den Startwert vor.
static atomic_bool s_tts_enabled;
// PSRAM-Buffer erst beim ersten Vorlesen allokieren (lazy); nur im
// network_task gelesen/geschrieben, daher kein atomic noetig.
static bool s_tts_inited;

// Antwort vorlesen (Phase 7): TTS-Request (Retry/Backoff macht tts.c),
// dann blockierende Wiedergabe ueber den Player-Task — "stop"
// (MultiNet) bricht sie per canned_audio_stop() ab. Ein TTS-Fehler ist
// nicht fatal: die Antwort steht bereits als Bubble auf dem Display.
static void speak_reply(const char *reply)
{
    if (!s_tts_inited) {
        // Lazy: 2,5 MB PSRAM erst jetzt holen (Toggle war an).
        if (tts_init() != ESP_OK) {
            s_cb(CLOUD_EVT_ERROR, "TTS init failed");
            return;
        }
        s_tts_inited = true;
    }

    const int16_t *pcm;
    size_t samples;
    uint32_t rate;

    esp_err_t err = tts_synthesize(reply, &pcm, &samples, &rate);
    if (err != ESP_OK) {
        // ESP_ERR_NOT_ALLOWED = HTTP 429: das Free-Tier-Tageslimit
        // (10 TTS-Requests) ist erschoepft.
        s_cb(CLOUD_EVT_ERROR, err == ESP_ERR_NOT_ALLOWED
                                  ? "TTS daily limit"
                                  : "Speech failed");
        return;
    }

    atomic_store(&s_speaking, true);
    s_cb(CLOUD_EVT_SPEAKING, "");
    canned_audio_play_pcm(pcm, samples, rate);
    atomic_store(&s_speaking, false);
}

static int arg_int(const cJSON *args, const char *key, int fallback)
{
    const cJSON *item = cJSON_GetObjectItem(args, key);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

// Tool-Call ausfuehren (Phase 10) und eine deterministische Bestaetigung
// formulieren — kein zweiter LLM-Call. Laeuft im network_task; die
// Aktionen sind aus jedem Task sicher: kitchen_timer_* nutzt esp_timer +
// atomics, volume_set ist atomar. Wortlaut wie format_command_bubble in
// main.c, damit Cloud- und lokale Aktionen gleich aussehen.
static void run_tool(const llm_result_t *r, char *confirm, size_t n)
{
    cJSON *args = cJSON_Parse(r->tool_args);  // NULL bei "{}"/Fehler: ok

    if (strcmp(r->tool_name, "set_timer") == 0) {
        int seconds = arg_int(args, "seconds", 0);
        if (seconds < 1) {
            seconds = 1;
        } else if (seconds > 86400) {
            seconds = 86400;
        }
        kitchen_timer_start(seconds);
        snprintf(confirm, n, "Timer set: %d:%02d", seconds / 60,
                 seconds % 60);
    } else if (strcmp(r->tool_name, "set_volume") == 0) {
        volume_set(arg_int(args, "percent", volume_get()));  // clampt selbst
        snprintf(confirm, n, "Volume: %d%%", volume_get());
    } else if (strcmp(r->tool_name, "cancel_timer") == 0) {
        snprintf(confirm, n, kitchen_timer_cancel() ? "Timer cancelled"
                                                    : "No timer running");
    } else if (strcmp(r->tool_name, "get_time_left") == 0) {
        int left = kitchen_timer_remaining_s();
        if (left >= 0) {
            snprintf(confirm, n, "Time left: %d:%02d", left / 60, left % 60);
        } else {
            snprintf(confirm, n, "No timer running");
        }
    } else {
        ESP_LOGW(TAG, "unknown tool: %s", r->tool_name);
        snprintf(confirm, n, "Unknown action");
    }

    cJSON_Delete(args);
}

static void network_task(void *arg)
{
    cloud_job_t job;
    static char text[TRANSCRIPT_MAX];
    static llm_result_t result;
    static char confirm[REPLY_MAX];

    while (1) {
        xQueueReceive(s_queue, &job, portMAX_DELAY);

        if (job.speak_only) {
            // Lokale TTS-Ansage (kein Whisper/LLM): nur sprechen, wenn
            // TTS an und WiFi vorhanden; sonst still verwerfen.
            if (!wifi_is_connected()) {
                ESP_LOGW(TAG, "speak_only: no WiFi, skipping TTS");
            } else if (atomic_load(&s_tts_enabled)) {
                speak_reply(job.text);
            }
            s_cb(CLOUD_EVT_DONE, "");
        } else if (!wifi_is_connected()) {
            ESP_LOGW(TAG, "no WiFi, dropping query");
            s_cb(CLOUD_EVT_ERROR, "No WiFi connection");
        } else if (whisper_transcribe(job.wav, job.bytes, text,
                                      sizeof(text)) != ESP_OK) {
            s_cb(CLOUD_EVT_ERROR, "Transcription failed");
        } else {
            s_cb(CLOUD_EVT_TRANSCRIPT, text);
            // LLM direkt im Anschluss — die Whisper-Verbindung ist
            // hier schon geschlossen (TLS strikt sequenziell).
            if (llm_generate(text, &result) != ESP_OK) {
                s_cb(CLOUD_EVT_ERROR, "Assistant failed");
            } else if (result.type == LLM_RESULT_TOOL_CALL) {
                // Phase 10: Tool lokal ausfuehren, Bestaetigung als
                // Reply-Bubble. History fuehrt den Bestaetigungstext als
                // Model-Turn (kein OpenAI-tool-Roundtrip) — Follow-ups
                // ("zwei Minuten kuerzer") haben damit Kontext.
                run_tool(&result, confirm, sizeof(confirm));
                chat_history_push(CHAT_ROLE_USER, text);
                chat_history_push(CHAT_ROLE_MODEL, confirm);
                s_cb(CLOUD_EVT_REPLY, confirm);
                s_cb(CLOUD_EVT_DONE, "");
            } else {
                // History erst bei Erfolg fortschreiben: sie enthaelt
                // nur vollstaendige User/Model-Paare.
                chat_history_push(CHAT_ROLE_USER, text);
                chat_history_push(CHAT_ROLE_MODEL, result.text);
                s_cb(CLOUD_EVT_REPLY, result.text);
                if (atomic_load(&s_tts_enabled)) {
                    speak_reply(result.text);
                }
                s_cb(CLOUD_EVT_DONE, "");
            }
        }

        // Erst nach dem Callback freigeben: solange busy, startet kein
        // neues Wake Word / Capture (Recording-Buffer-Schutz).
        atomic_store(&s_busy, false);
    }
}

esp_err_t cloud_init(cloud_event_cb_t cb) //cb = Pointer auf die Funktion on_cloud_event aus main.c, die aufgerufen wird, wenn ein Cloud-Event eintritt (zB Transkript, Antwort, Fehlerbeschreibung)
{
    esp_err_t err = chat_history_init();
    if (err != ESP_OK) {
        return err;
    }
    // Startwert aus dem Kconfig; tts_init() (2,5 MB PSRAM) passiert lazy
    // in speak_reply(), sobald der Toggle das erste Mal an ist.
#ifdef CONFIG_TTS_SPEAK_REPLIES
    atomic_store(&s_tts_enabled, true);
#else
    atomic_store(&s_tts_enabled, false);
#endif
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

bool cloud_is_speaking(void)
{
    return atomic_load(&s_speaking);
}

void cloud_set_tts_enabled(bool enabled)
{
    atomic_store(&s_tts_enabled, enabled);
}

bool cloud_tts_enabled(void)
{
    return atomic_load(&s_tts_enabled);
}

void cloud_speak(const char *text)
{
    // s_busy zeigt ob Pipeline gerade eine Anfrage bearbeitet. Wenn ja, wird die Anfrage verworfen, um die Pipeline nicht zu überlasten.
    if (atomic_exchange(&s_busy, true)) {
        ESP_LOGW(TAG, "cloud_speak: pipeline busy, dropping");
        return;
    }
    cloud_job_t job = { .speak_only = true }; //erstelle neuen job -> nur TTS weil text übergeben wird
    snprintf(job.text, sizeof(job.text), "%s", text); // übergebenen text in den job kopieren -> wird auf maximal 80 Zeichen gekürzt
    xQueueSend(s_queue, &job, 0); // Job in die Queue einfügen -> wird dann von network_task abgearbeitet
}
