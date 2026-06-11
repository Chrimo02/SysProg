// MiniLingo Phase 7: Gemini TTS + Wiedergabe.
// app_main initialisiert LVGL/Display/Touch/UI (Phase 1), NVS, WiFi-STA,
// die AFE-Pipeline mit WakeNet9-Mycroft (Phase 2; bis 2026-06-11
// Alexa), den Recording-Buffer (Phase 3), MultiNet7-EN mit 14 lokalen
// Commands, Kitchen-Timer, Lautstaerke und Canned-TTS aus SPIFFS
// (Phase 4) und den network_task, der Aufnahmen ohne lokalen
// Command-Match an Groq Whisper schickt (Phase 5, User-Bubble), das
// Transkript an Gemini weiterreicht (Phase 6, Assistant-Bubble) und
// die Antwort per Gemini-TTS vorliest (Phase 7, SPEAKING-State;
// "stop" bricht die Wiedergabe ab).
//
// Thread-Grenzen: WiFi-Events (Event-Loop-Task), Wake-Word-, Recorder-
// und Command-Events (afe_fetch_task, Core 1) sowie Timer-Done
// (esp_timer-Task) duerfen LVGL nicht direkt anfassen — sie setzen
// atomare Flags, die ein LVGL-Timer (app_event_timer_cb, laeuft im
// lvgl_task) abholt und in UI-Updates uebersetzt.
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "lvgl.h"

#include "debug/wav_dump.h"
#include "drivers/audio_in.h"
#include "drivers/audio_out.h"
#include "drivers/display.h"
#include "drivers/touch.h"
#include "local/canned_audio.h"
#include "local/timer.h"
#include "local/volume.h"
#include "network/cloud.h"
#include "network/wifi.h"
#include "speech/afe_pipeline.h"
#include "speech/commands.h"
#include "speech/recorder.h"
#include "speech/wake_word.h"
#include "ui/ui.h"

static const char *TAG = "main";

#define IDLE_STATE_TEXT  "Listening for Mycroft"
#define INITIAL_VOLUME   60

// -1 = keine Aenderung, 0 = getrennt, 1 = verbunden
static atomic_int s_wifi_event = -1;
static atomic_bool s_wake_event;
// 0 = kein Event, sonst command_id_t. s_cmd_arg traegt den Kontext fuer
// den Bubble-Text (Restzeit/Volume/Cancel-Erfolg) und wird VOR der ID
// geschrieben (einziger Writer: afe_fetch_task).
static atomic_int s_cmd_event;
static atomic_int s_cmd_arg;
static atomic_bool s_timer_done_event;
// Cloud-Pipeline (Phase 5-7): processing = Upload gestartet
// (afe_fetch_task), transcript/reply/speaking/done/error = Fortschritt
// (network_task). Die Textbuffer werden jeweils VOR dem Flag
// geschrieben; es ist nur ein Cloud-Query gleichzeitig in flight
// (cloud_is_busy-Gate).
static atomic_bool s_processing_event;
static atomic_bool s_transcript_event;
static atomic_bool s_reply_event;
static atomic_bool s_speaking_event;
static atomic_bool s_cloud_done_event;
static atomic_bool s_cloud_error_event;
static char s_transcript[512];
static char s_reply[1024];
static char s_cloud_error[48];

// Cloud-Pfad-Koordination, nur im afe_fetch_task: Upload erst, wenn
// BEIDES vorliegt — Capture-Ende (WAV fertig) und MultiNet-Timeout
// (kein lokaler Command; die MultiNet-Session lebt laenger als das
// Capture, siehe afe_pipeline.c).
static const uint8_t *s_pending_wav;
static size_t s_pending_wav_bytes;
static bool s_mn_timed_out;

// Nur im LVGL-Task benutzt (app_event_timer_cb / Status-Text-Logik).
static bool s_listening;
static bool s_processing;
static bool s_speaking;
static char s_state_text[24];

static uint32_t lvgl_tick_get_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void lvgl_task(void *arg)
{
    while (1) {
        uint32_t delay_ms = lv_timer_handler();
        if (delay_ms == LV_NO_TIMER_READY) {
            delay_ms = 20;
        }
        delay_ms = LV_CLAMP(1, delay_ms, 20);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

static void on_mic_button_clicked(void)
{
    // Laeuft im LVGL-Task-Kontext (Klick-Event). Startet eine
    // Listening-Session wie das Wake Word (Spec Section 2: Trigger =
    // "Wake Word | Touch"); das UI-Feedback (roter Button,
    // "Listening...") kommt ueber denselben Wake-Event-Pfad zurueck.
    // Waehrend laufender Session/Cloud-Pipeline wird der Klick in der
    // Pipeline ignoriert.
    afe_pipeline_request_listening();
}

static void on_wifi_status(bool connected)
{
    atomic_store(&s_wifi_event, connected ? 1 : 0);
}

// Laeuft (wie alle Recorder-/Command-Callbacks) im afe_fetch_task.
static void on_wake_word(void)
{
    // Neue Session: Cloud-Pfad-Zustand der vorigen zuruecksetzen.
    s_pending_wav = NULL;
    s_mn_timed_out = false;
    atomic_store(&s_wake_event, true);
}

// Cloud-Pfad starten, sobald WAV + MultiNet-Timeout beide da sind.
// Der Recording-Buffer bleibt waehrend des Uploads gueltig, weil
// afe_pipeline bei cloud_is_busy() kein neues Capture startet.
static void submit_cloud_query(void)
{
    cloud_submit(s_pending_wav, s_pending_wav_bytes);
    s_pending_wav = NULL;
    s_mn_timed_out = false;
    atomic_store(&s_processing_event, true);
}

static void on_recording_done(const uint8_t *wav, size_t wav_bytes,
                              uint32_t duration_ms,
                              recorder_stop_reason_t reason)
{
    wav_dump_trigger(wav, wav_bytes);
    s_pending_wav = wav;
    s_pending_wav_bytes = wav_bytes;
    if (s_mn_timed_out) {
        submit_cloud_query();
    }
}

// MultiNet-Timeout = kein lokaler Command. Meist liegt das WAV schon
// bereit (Capture stoppt nach 1,5 s Stille, MultiNet erst nach 6 s);
// redet der User aber laenger als 6 s, kommt das Capture-Ende spaeter.
static void on_command_timeout(void)
{
    s_mn_timed_out = true;
    if (s_pending_wav) {
        submit_cloud_query();
    }
}

// Laeuft im network_task (Core 0): Ergebnis der Cloud-Pipeline.
static void on_cloud_event(cloud_event_type_t type, const char *text)
{
    if (type == CLOUD_EVT_TRANSCRIPT) {
        snprintf(s_transcript, sizeof(s_transcript), "%s", text);
        atomic_store(&s_transcript_event, true);
    } else if (type == CLOUD_EVT_REPLY) {
        snprintf(s_reply, sizeof(s_reply), "%s", text);
        atomic_store(&s_reply_event, true);
    } else if (type == CLOUD_EVT_SPEAKING) {
        atomic_store(&s_speaking_event, true);
    } else if (type == CLOUD_EVT_DONE) {
        atomic_store(&s_cloud_done_event, true);
    } else {
        snprintf(s_cloud_error, sizeof(s_cloud_error), "%s", text);
        atomic_store(&s_cloud_error_event, true);
    }
}

static void on_timer_done(void)
{
    // esp_timer-Task; Done-/Alarm-Sound spielt timer.c selbst.
    atomic_store(&s_timer_done_event, true);
}

// Command-Dispatch (Spec Section 7 "Command-Handler"). Laeuft im
// afe_fetch_task: Aufnahme verwerfen (war ein lokaler Command, kein
// Cloud-Query), Handler ausfuehren, Canned-Response abspielen, Event
// fuer die UI setzen.
static void on_local_command(command_id_t id, const char *phrase)
{
    // Waehrend der TTS-Wiedergabe (Phase 7) laeuft MultiNet nur fuer
    // "stop" — alle anderen Matches sind dann mit hoher
    // Wahrscheinlichkeit False-Positives aus dem eigenen
    // Lautsprecher-Signal (Threshold 0.05, bekannt empfindlich).
    if (cloud_is_speaking() && id != CMD_STOP) {
        ESP_LOGW(TAG, "ignoring \"%s\" during TTS playback", phrase);
        return;
    }

    recorder_abort();
    // Falls das Capture schon fertig war (Stille vor dem Match):
    // Aufnahme verwerfen, sie war ein lokaler Command.
    s_pending_wav = NULL;
    int arg = 0;

    switch (id) {
    case CMD_TIMER_1MIN:
        kitchen_timer_start(60);
        canned_audio_play(CANNED_TIMER_SET_60S);
        break;
    case CMD_TIMER_5MIN:
        kitchen_timer_start(300);
        canned_audio_play(CANNED_TIMER_SET_300S);
        break;
    case CMD_TIMER_10MIN:
        kitchen_timer_start(600);
        canned_audio_play(CANNED_TIMER_SET_600S);
        break;
    case CMD_TIMER_15MIN:
        kitchen_timer_start(900);
        canned_audio_play(CANNED_TIMER_SET_900S);
        break;
    case CMD_TIMER_30MIN:
        kitchen_timer_start(1800);
        canned_audio_play(CANNED_TIMER_SET_1800S);
        break;
    case CMD_TIMER_30SEC:
        kitchen_timer_start(30);
        canned_audio_play(CANNED_TIMER_SET_30S);
        break;
    case CMD_TIMER_STOP:
    case CMD_TIMER_CANCEL:
        arg = kitchen_timer_cancel();
        canned_audio_play(arg ? CANNED_TIMER_CANCELLED : CANNED_NO_TIMER);
        break;
    case CMD_TIME_LEFT:
        arg = kitchen_timer_remaining_s();
        // Feste Phrasen koennen keine variable Restzeit ansagen — die
        // exakte Zeit zeigen Bubble + Status-Bar (Phase-4-Entscheidung).
        canned_audio_play(arg >= 0 ? CANNED_TIME_LEFT : CANNED_NO_TIMER);
        break;
    case CMD_STOP:
        // Bricht Canned- und TTS-Wiedergabe ab (Phase 4/7). Laeuft ein
        // Timer, gilt "stop" als "stop timer" — MultiNet feuert bei
        // "stop timer" oft schon nach "stop" (Diagnose 2026-06-11).
        // Ausnahme SPEAKING: da meint "stop" nur die Wiedergabe, der
        // Timer laeuft weiter.
        canned_audio_stop();
        if (!cloud_is_speaking() && kitchen_timer_cancel()) {
            arg = 1;
            canned_audio_play(CANNED_TIMER_CANCELLED);
        }
        break;
    case CMD_VOLUME_UP:
        volume_adjust(+VOLUME_STEP);
        arg = volume_get();
        canned_audio_play(CANNED_VOLUME_CHANGED);
        break;
    case CMD_VOLUME_DOWN:
        volume_adjust(-VOLUME_STEP);
        arg = volume_get();
        canned_audio_play(CANNED_VOLUME_CHANGED);
        break;
    case CMD_MUTE:
        volume_mute();
        canned_audio_play(CANNED_MUTED);  // bei Gain 0 bewusst stumm
        break;
    case CMD_UNMUTE:
        volume_unmute();
        arg = volume_get();
        canned_audio_play(CANNED_UNMUTED);
        break;
    }

    atomic_store(&s_cmd_arg, arg);
    atomic_store(&s_cmd_event, (int)id);
}

// Bubble-Text fuer ein Command-Event (laeuft im LVGL-Task).
static void format_command_bubble(command_id_t id, int arg, char *text,
                                  size_t len)
{
    switch (id) {
    case CMD_TIMER_1MIN:   snprintf(text, len, "Timer set: 1 min"); break;
    case CMD_TIMER_5MIN:   snprintf(text, len, "Timer set: 5 min"); break;
    case CMD_TIMER_10MIN:  snprintf(text, len, "Timer set: 10 min"); break;
    case CMD_TIMER_15MIN:  snprintf(text, len, "Timer set: 15 min"); break;
    case CMD_TIMER_30MIN:  snprintf(text, len, "Timer set: 30 min"); break;
    case CMD_TIMER_30SEC:  snprintf(text, len, "Timer set: 30 sec"); break;
    case CMD_TIMER_STOP:
    case CMD_TIMER_CANCEL:
        snprintf(text, len, arg ? "Timer cancelled" : "No timer running");
        break;
    case CMD_TIME_LEFT:
        if (arg >= 0) {
            snprintf(text, len, "Time left: %d:%02d", arg / 60, arg % 60);
        } else {
            snprintf(text, len, "No timer running");
        }
        break;
    case CMD_STOP:
        snprintf(text, len, arg ? "Timer cancelled" : "Stopped");
        break;
    case CMD_VOLUME_UP:
    case CMD_VOLUME_DOWN:
    case CMD_UNMUTE:      snprintf(text, len, "Volume: %d%%", arg); break;
    case CMD_MUTE:        snprintf(text, len, "Muted"); break;
    default:              snprintf(text, len, "Command %d", (int)id); break;
    }
}

static void mic_button_reset_cb(lv_timer_t *timer)
{
    ui_mic_button_set_state(UI_MIC_IDLE);
}

// Status-Text mittig in der Bar: Listening > Processing >
// Timer-Countdown > Idle (Spec Section 4: "Timer: 04:23" ersetzt den
// State-Text).
static void update_state_text(void)
{
    char text[24];
    int left = kitchen_timer_remaining_s();

    if (s_listening) {
        snprintf(text, sizeof(text), "Listening...");
    } else if (s_processing) {
        snprintf(text, sizeof(text), "Processing...");
    } else if (s_speaking) {
        snprintf(text, sizeof(text), "Speaking...");
    } else if (left >= 0) {
        snprintf(text, sizeof(text), "Timer: %02d:%02d", left / 60,
                 left % 60);
    } else {
        snprintf(text, sizeof(text), IDLE_STATE_TEXT);
    }

    if (strcmp(text, s_state_text) != 0) {
        strcpy(s_state_text, text);
        ui_status_bar_set_state_text(text);
    }
}

// LVGL-Timer: holt Events aus den anderen Task-Kontexten ab.
static void app_event_timer_cb(lv_timer_t *timer)
{
    int wifi = atomic_exchange(&s_wifi_event, -1);
    if (wifi >= 0) {
        ui_status_bar_set_wifi_connected(wifi == 1);
    }

    if (atomic_exchange(&s_wake_event, false)) {
        s_listening = true;
        ui_mic_button_set_state(UI_MIC_LISTENING);
    }

    // Cloud-Pipeline gestartet: Mic-Button orange, "Processing...".
    // (Die "Captured ..."-Bubble aus Phase 3/4 ist damit ersetzt.)
    if (atomic_exchange(&s_processing_event, false)) {
        s_listening = false;
        s_processing = true;
        ui_mic_button_set_state(UI_MIC_PROCESSING);
    }

    if (atomic_exchange(&s_transcript_event, false)) {
        ui_chat_add_user_bubble(s_transcript);
        // PROCESSING bleibt stehen — Gemini laeuft noch (Phase 6).
    }

    if (atomic_exchange(&s_reply_event, false)) {
        ui_chat_add_assistant_bubble(s_reply);
        // PROCESSING bleibt stehen — der TTS-Request laeuft noch;
        // SPEAKING/DONE/ERROR loesen ab (Phase 7).
    }

    if (atomic_exchange(&s_speaking_event, false)) {
        s_processing = false;
        s_speaking = true;
        ui_mic_button_set_state(UI_MIC_SPEAKING);
    }

    if (atomic_exchange(&s_cloud_done_event, false)) {
        s_processing = false;
        s_speaking = false;
        ui_mic_button_set_state(UI_MIC_IDLE);
    }

    if (atomic_exchange(&s_cloud_error_event, false)) {
        char text[64];
        snprintf(text, sizeof(text), "Cloud: %s", s_cloud_error);
        ui_chat_add_local_bubble(text);
        s_processing = false;
        s_speaking = false;
        ui_mic_button_set_state(UI_MIC_IDLE);
    }

    int cmd = atomic_exchange(&s_cmd_event, 0);
    if (cmd != 0) {
        char text[32];
        format_command_bubble((command_id_t)cmd, atomic_load(&s_cmd_arg),
                              text, sizeof(text));
        ui_chat_add_local_bubble(text);
        ui_status_bar_set_volume(volume_get());
        s_listening = false;
        // Gruen als Match-Feedback, nach 800 ms zurueck auf Idle
        // (Animationen kommen in Phase 8).
        ui_mic_button_set_state(UI_MIC_LOCAL_RESPONSE);
        lv_timer_set_repeat_count(
            lv_timer_create(mic_button_reset_cb, 800, NULL), 1);
    }

    if (atomic_exchange(&s_timer_done_event, false)) {
        ui_chat_add_local_bubble("Timer done!");
    }

    update_state_text();
}

void app_main(void)
{
    ESP_LOGI(TAG, "MiniLingo Phase 7 starting");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    lv_init();
    lv_tick_set_cb(lvgl_tick_get_cb);

    ESP_ERROR_CHECK(display_init());
    ESP_ERROR_CHECK(touch_init());

    ui_init();
    ui_mic_button_set_click_cb(on_mic_button_clicked);
    ui_status_bar_set_state_text(IDLE_STATE_TEXT);
    ui_status_bar_set_wifi_connected(false);
    lv_timer_create(app_event_timer_cb, 50, NULL);

    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 8192, NULL, 4, NULL, 0);

    ESP_ERROR_CHECK(wifi_init(on_wifi_status));

    // Cloud-Pipeline (Phase 5/6): network_task vor der AFE-Pipeline
    // starten — der afe_fetch_task ruft cloud_submit/cloud_is_busy.
    ESP_ERROR_CHECK(cloud_init(on_cloud_event));

    // Audio-Ausgabe + lokale Command-Infrastruktur (Phase 4)
    ESP_ERROR_CHECK(audio_out_init());
    volume_init(INITIAL_VOLUME);
    ui_status_bar_set_volume(volume_get());
    ESP_ERROR_CHECK(canned_audio_init());
    kitchen_timer_init(on_timer_done);

    // Speech-Pipeline: Models einmal laden, dann MultiNet (commands)
    // VOR dem Pipeline-Start initialisieren — der fetch-Task fuettert
    // die Commands ab dem ersten Wake Word.
    wav_dump_init();
    wake_word_set_listener(on_wake_word);
    ESP_ERROR_CHECK(recorder_init(on_recording_done));
    ESP_ERROR_CHECK(audio_in_init());
    srmodel_list_t *models = esp_srmodel_init("model");
    ESP_ERROR_CHECK(commands_init(models, on_local_command,
                                  on_command_timeout));
    ESP_ERROR_CHECK(afe_pipeline_init(models));

    ESP_LOGI(TAG, "Phase 7 up: local commands + cloud STT + LLM + TTS");
}
