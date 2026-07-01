// MiniLingo Phase 8: Polish + Robustness.
// app_main initialisiert LVGL/Display/Touch/UI (Phase 1), NVS, WiFi-STA,
// die AFE-Pipeline mit WakeNet9-Mycroft (Phase 2; bis 2026-06-11
// Alexa), den Recording-Buffer (Phase 3), MultiNet7-EN mit 14 lokalen
// Commands, Kitchen-Timer, Lautstaerke und Canned-TTS aus SPIFFS
// (Phase 4) und den network_task, der Aufnahmen ohne lokalen
// Command-Match an Groq Whisper schickt (Phase 5, User-Bubble), das
// Transkript ans LLM weiterreicht (Phase 6, Assistant-Bubble; seit
// 2026-06-12 Cerebras statt Gemini) und die Antwort per Gemini-TTS
// vorliest (Phase 7, SPEAKING-State, per Kconfig deaktivierbar;
// "stop" bricht die Wiedergabe ab).
//
// Thread-Grenzen: WiFi-Events (Event-Loop-Task), Wake-Word-, Recorder-
// und Command-Events (afe_fetch_task, Core 1) sowie Timer-Done
// (esp_timer-Task) duerfen LVGL nicht direkt anfassen — sie setzen
// atomare Flags, die ein LVGL-Timer (app_event_timer_cb, laeuft im
// lvgl_task) abholt und in UI-Updates uebersetzt.
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "lvgl.h"

#include "debug/wav_dump.h"
#include "drivers/audio_in.h"
#include "drivers/audio_out.h"
#include "drivers/display.h"
#include "drivers/touch.h"
#include "local/canned_audio.h"
#include "local/temperature.h"
#include "local/timer.h"
#include "local/volume.h"
#include "config.h"
#include "network/cloud.h"
#include "network/llm.h"
#include "network/provisioning.h"
#include "network/tts.h"
#include "network/wifi.h"
#include "speech/afe_pipeline.h"
#include "speech/commands.h"
#include "speech/recorder.h"
#include "speech/wake_word.h"
#include "ui/ui.h"

static const char *TAG = "main";

#define INITIAL_VOLUME   60

// Charakter-Switcher (Phase B): Klick auf den State-Text cyclet durch
// diese Tabelle. Die Reihenfolge MUSS zum Wakenet-Model-Index-Mapping in
// afe_pipeline.c passen (0=Mycroft -> Model 1 usw.). persona ist der
// austauschbare System-Prompt-Anfang; die Tool-Hinweise hängt llm.c an.
typedef struct {
    const char *name;
    const char *idle_text;
    const char *persona;
    const char *voice;        // Gemini-Stimme (Fallback-TTS)
    const char *local_voice;  // Piper-Stimme des lokalen Servers (primaer)
} character_t;

static const character_t s_characters[] = {
    { "Mycroft", "Listening for Mycroft",
      "You are Mycroft, a strikingly eloquent, well-read and wise "
      "personality, like the older brother of Sherlock Holmes. You "
      "speak in a refined, courteous manner, occasionally offer a "
      "sharp observation or a subtle thought, and remain unfailingly "
      "considerate.", "Charon", "mycroft" },
    { "Alexa", "Listening for Alexa",
      "You are Alexa, a friendly and helpful voice assistant in the "
      "style of Amazon's Alexa: casual, warm and straightforward.",
      "Aoede", "alexa" },
    { "Jarvis", "Listening for Jarvis",
      "You are Jarvis, a highly technical AI in the style of Iron "
      "Man's J.A.R.V.I.S.: precise, matter-of-fact, thinking in "
      "technical detail, and addressing the user formally as 'Sir'.",
      "Fenrir", "jarvis" },
};
#define NUM_CHARACTERS (sizeof(s_characters) / sizeof(s_characters[0]))

// Stille-Guard (Phase 8): Aufnahmen mit weniger VAD-Sprache gehen
// nicht zu Whisper — der halluziniert aus (fast) reiner Stille Saetze
// wie "Vielen Dank." (beobachtet 2026-06-12), und Gemini antwortet
// dann artig darauf. Gemessen am Geraet (2026-06-12): leere Captures
// melden trotzdem 576-608 ms "Speech" (VAD-Nachhall des Wake Words);
// eine echte kurze Frage liegt bei 3700 ms. 1000 ms trennt sauber —
// selbst Mini-Queries ("Wer bist du?") kommen mit Nachhall drueber.
#define MIN_SPEECH_MS    1000

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
static atomic_bool s_too_quiet_event;
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
static int s_volume_shown = -1;
static int s_character;  // aktiver Charakter-Index (LVGL-Task)

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

// Laeuft im LVGL-Task-Kontext (Slider-Event, Phase 8b). volume_set ist
// atomar und damit aus diesem Task sicher; die Anzeige wird direkt
// aktualisiert (gleicher Task wie die UI).
static void on_volume_slider_changed(int percent)
{
    volume_set(percent);
    ui_status_bar_set_volume(percent);
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

static void on_tts_button_clicked(void)
{
    // LVGL-Task-Kontext. cloud_set_tts_enabled ist atomar; das eigentliche
    // tts_init (2,5 MB PSRAM) passiert lazy im network_task beim ersten
    // Vorlesen. Button-Optik + kurze Bestaetigung direkt hier.
    bool on = !cloud_tts_enabled();
    cloud_set_tts_enabled(on);
    ui_tts_button_set_enabled(on);
    ui_chat_add_local_bubble(on ? "Voice replies on" : "Voice replies off");
}

static void on_wifi_clicked(void)
{
    // LVGL-Task-Kontext. Setup-Portal öffnen (SoftAP + Web-Formular)
    // und dem Nutzer Hotspot + URL anzeigen.
    provisioning_start();
    ui_chat_add_local_bubble("Setup-WLAN: MiniLingo-Setup (PW minilingo)");
    ui_chat_add_local_bubble("Browser: http://192.168.4.1");
}

static void reboot_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1200));  // Bubble kurz sichtbar lassen
    esp_restart();
}

static void on_character_clicked(void)
{
    // LVGL-Task-Kontext. Nächsten Charakter wählen, in NVS speichern und
    // neu starten: drei Wakenets gleichzeitig sprengen den internen RAM,
    // deshalb lädt der Boot nur das eine aktive Wakeword (afe_pipeline).
    s_character = (s_character + 1) % NUM_CHARACTERS;
    const character_t *c = &s_characters[s_character];
    config_set_character(s_character);

    char bubble[40];
    snprintf(bubble, sizeof(bubble), "Switching to %s...", c->name);
    ui_chat_add_local_bubble(bubble);
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
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
                              uint32_t duration_ms, uint32_t speech_ms,
                              recorder_stop_reason_t reason)
{
    wav_dump_trigger(wav, wav_bytes);
    // Stille-Guard (Phase 8): fast stumme Aufnahme verwerfen statt
    // hochladen. Die MultiNet-Session laeuft normal in ihren Timeout;
    // on_command_timeout findet dann kein pending WAV mehr vor.
    if (speech_ms < MIN_SPEECH_MS) {
        ESP_LOGW(TAG, "discarding capture: only %u ms speech",
                 (unsigned)speech_ms);
        atomic_store(&s_too_quiet_event, true);
        return;
    }
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
static void on_local_command(command_id_t id, const char *phrase,
                             float prob)
{
    // Waehrend der TTS-Wiedergabe (Phase 7) laeuft MultiNet nur fuer
    // "stop" — alle anderen Matches sind dann mit hoher
    // Wahrscheinlichkeit False-Positives aus dem eigenen
    // Lautsprecher-Signal. Fuer "stop" selbst gilt dabei eine
    // strengere Schwelle als der per-Command-Mindestwert in
    // commands.c: die Wiedergabe (deutsche Sprache, Zimmerlautstaerke)
    // hat im User-Test 2026-06-12 ein "stop" getriggert. Tuning-Punkt:
    // Prob eines echten "stop" gegen die Wiedergabe im Log pruefen.
    if (cloud_is_speaking() && (id != CMD_STOP || prob < 0.30f)) {
        ESP_LOGW(TAG, "ignoring \"%s\" (prob %.2f) during TTS playback",
                 phrase, prob);
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
    case CMD_VOLUME_20:
    case CMD_VOLUME_40:
    case CMD_VOLUME_60:
    case CMD_VOLUME_80:
    case CMD_VOLUME_100:
        // Phase 8b: direkte Stufen, IDs liegen fortlaufend ab 20 %.
        volume_set((id - CMD_VOLUME_20 + 1) * VOLUME_STEP);
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
    case CMD_TEMPERATURE:
        arg = temperature_valid()
                  ? (int)(temperature_get() + 0.5f)
                  : INT_MIN;
        if (arg != INT_MIN && cloud_tts_enabled()) {
            char say[64];
            snprintf(say, sizeof(say), "The temperature is %d degrees", arg);
            cloud_speak(say);
        }
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
    case CMD_VOLUME_20:
    case CMD_VOLUME_40:
    case CMD_VOLUME_60:
    case CMD_VOLUME_80:
    case CMD_VOLUME_100:
    case CMD_UNMUTE:      snprintf(text, len, "Volume: %d%%", arg); break;
    case CMD_MUTE:        snprintf(text, len, "Muted"); break;
    case CMD_TEMPERATURE:
        if (arg != INT_MIN) {
            snprintf(text, len, "Temperature: %d" "\xc2\xb0" "C", arg);
        } else {
            snprintf(text, len, "Sensor not ready");
        }
        break;
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
        snprintf(text, sizeof(text), "%s",
                 s_characters[s_character].idle_text);
    }

    if (strcmp(text, s_state_text) != 0) {
        strcpy(s_state_text, text);
        ui_status_bar_set_state_text(text);
    }
}

// Volume-Anzeige (Label + Slider-Position) aus dem aktuellen Gain
// nachziehen — deckt alle Pfade ab: lokaler Command, Touch-Slider und
// seit Phase 10 die Cloud-Tool-Calls (volume_set im network_task). Wie
// update_state_text() nur bei Aenderung schreiben (kein Tick-Redraw).
static void update_volume_display(void)
{
    int v = volume_get();
    if (v != s_volume_shown) {
        s_volume_shown = v;
        ui_status_bar_set_volume(v);
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

    // Stille-Guard: Aufnahme verworfen — kurzer Hinweis + Cue, zurueck
    // zu Idle (Phase 8).
    if (atomic_exchange(&s_too_quiet_event, false)) {
        ui_chat_add_local_bubble("Didn't catch that");
        canned_audio_play(CANNED_ERROR);
        s_listening = false;
        ui_mic_button_set_state(UI_MIC_IDLE);
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
        // Error-Recovery (Phase 8): kurzer Audio-Cue, zurueck zu Idle.
        canned_audio_play(CANNED_ERROR);
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
    update_volume_display();
}

void app_main(void)
{
    ESP_LOGI(TAG, "MiniLingo Phase 8 starting");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // Laufzeit-Config aus NVS laden (SSID/Keys/Modelle); fällt auf die
    // Kconfig-Defaults zurück, wenn nichts provisioniert ist.
    ESP_ERROR_CHECK(config_init());

    // Aktiven Charakter aus dem NVS wiederherstellen (Phase B). Persona
    // hier setzen (llm = network_task, Pointer-Tausch); der Wakenet-
    // Filter wird nach afe_pipeline_init scharfgeschaltet.
    s_character = config_character_index();
    if (s_character < 0 || s_character >= (int)NUM_CHARACTERS) {
        s_character = 0;
    }
    llm_set_system_prompt(s_characters[s_character].persona);
    tts_set_voice(s_characters[s_character].voice);
    tts_set_local_voice(s_characters[s_character].local_voice);

    lv_init();
    lv_tick_set_cb(lvgl_tick_get_cb);

    ESP_ERROR_CHECK(display_init());
    ESP_ERROR_CHECK(touch_init());

    ui_init();
    ui_mic_button_set_click_cb(on_mic_button_clicked);
    ui_status_bar_set_volume_changed_cb(on_volume_slider_changed);
    ui_status_bar_set_wifi_clicked_cb(on_wifi_clicked);
    ui_status_bar_set_state_clicked_cb(on_character_clicked);
    ui_status_bar_set_state_text(s_characters[s_character].idle_text);
    ui_status_bar_set_wifi_connected(false);
    // TTS-Toggle: Startzustand aus demselben Kconfig wie cloud.c (vor
    // dem lvgl_task setzen — danach waeren UI-Calls nicht thread-safe).
    ui_tts_button_set_click_cb(on_tts_button_clicked);
#ifdef CONFIG_TTS_SPEAK_REPLIES
    ui_tts_button_set_enabled(true);
#else
    ui_tts_button_set_enabled(false);
#endif
    lv_timer_create(app_event_timer_cb, 50, NULL);

    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 8192, NULL, 4, NULL, 0);

    ESP_ERROR_CHECK(wifi_init(on_wifi_status));

    // Erststart-Komfort: ist keine SSID provisioniert, direkt das
    // Setup-Portal öffnen (Tap aufs WiFi-Icon tut dasselbe).
    if (config_wifi_unprovisioned()) {
        ESP_LOGW(TAG, "no WiFi provisioned — starting setup portal");
        provisioning_start();
    }

    // Cloud-Pipeline (Phase 5/6): network_task vor der AFE-Pipeline
    // starten — der afe_fetch_task ruft cloud_submit/cloud_is_busy.
    ESP_ERROR_CHECK(cloud_init(on_cloud_event));

    // Audio-Ausgabe + lokale Command-Infrastruktur (Phase 4)
    ESP_ERROR_CHECK(audio_out_init());
    volume_init(INITIAL_VOLUME);
    ui_status_bar_set_volume(volume_get());
    ESP_ERROR_CHECK(canned_audio_init());
    kitchen_timer_init(on_timer_done);
    // DS18B20: Fehler nicht fatal — Sensor koennte nicht angeschlossen sein.
    if (temperature_init() != ESP_OK) {
        ESP_LOGW(TAG, "temperature sensor not found");
    }

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
    // Nur das Wakeword des aktiven Charakters laden (RAM, s. afe_pipeline).
    ESP_ERROR_CHECK(afe_pipeline_init(models, s_character));

    ESP_LOGI(TAG, "Phase 8 up: local commands + cloud STT + LLM"
#if CONFIG_TTS_SPEAK_REPLIES
                  " + TTS"
#endif
    );
}
