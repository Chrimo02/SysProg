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

static const char *TAG = "main"; // Logging-Tag

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
    const char *greeting;     // kurzer Begruessungssatz nach dem Switch
} character_t;

static const character_t s_characters[] = {
    { "Mycroft", "Listening for Mycroft",
      "You are Mycroft, a strikingly eloquent, well-read and wise "
      "personality, like the older brother of Sherlock Holmes. You "
      "speak in a refined, courteous manner, occasionally offer a "
      "sharp observation or a subtle thought, and remain unfailingly "
      "considerate.", "Charon", "mycroft",
      "A pleasure. Mycroft here, entirely at your disposal." },
    { "Alexa", "Listening for Alexa",
      "You are Alexa, a friendly and helpful voice assistant in the "
      "style of Amazon's Alexa: casual, warm and straightforward.",
      "Aoede", "alexa",
      "Hey there! Alexa here. What can I do for you?" },
    { "Jarvis", "Listening for Jarvis",
      "You are Jarvis, a highly technical AI in the style of Iron "
      "Man's J.A.R.V.I.S.: precise, matter-of-fact, thinking in "
      "technical detail, and addressing the user formally as 'Sir'.",
      "Fenrir", "jarvis",
      "Jarvis online. At your service, Sir." },
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
// Charakter-Wechsel per Voice (Phase B2): Ziel-Index, gesetzt im
// afe_fetch_task (on_local_command), angewandt im LVGL-Task
// (apply_character). -1 = kein Event.
static atomic_int s_character_event;
// Namensunabhaengiger Cycle-Wechsel (Teil A): gesetzt im afe_fetch_task,
// angewandt im LVGL-Task (naechster Charakter, wie der Touch-Button).
static atomic_bool s_cycle_event;
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
static const uint8_t *s_pending_wav; //Pointer auf array von 8-bit integer -> zur compilezeit größe nicht bekannt, weil abhängig von aufnahmedauer -> größe ist s_pending_wav_bytes
static size_t s_pending_wav_bytes; // -> größe der aufgenommenen wav-Datei (in bytes) -> size_t wird in c für größenangaben benutzt, weil es unsigned ist und somit nicht negativ werden kann.
static bool s_mn_timed_out; // mn=MultiNet --> flag wird gesetzt wenn der 6s Multinet-Timeout erreicht ist, damit die Cloud-Pipeline gestartet werden kann, sobald das WAV fertig ist (on_recording_done).

// Nur im LVGL-Task benutzt (app_event_timer_cb / Status-Text-Logik).
static bool s_listening;
static bool s_processing;
static bool s_speaking;
static char s_state_text[24]; // globaler string buffer der den aktuellen status-text (wie auf display in statusbar) speichert
static int s_volume_shown = -1; //prüft ob der im UI-angezeigte Volume-Wert mit dem aktuell gespeicherten Volume-Wert übereinstimmt -> nur bei änderung wird UI geupdated (update_volume_display)
static int s_character;  // aktiver Charakter-Index (LVGL-Task)

static uint32_t lvgl_tick_get_cb(void) // Callback-Funktion, die die aktuelle Zeit in Millisekunden zurückgibt. Diese Funktion wird von LVGL verwendet, um die Zeit zu verfolgen und Timer-Events zu verwalten.
{
    return (uint32_t)(esp_timer_get_time() / 1000); //esp_timer_get_time gibt die Zeit in Mikrosekunden zurück, daher wird hier durch 1000 geteilt, um Millisekunden zu erhalten. Der Rückgabewert ist ein uint32_t, der die aktuelle Zeit in Millisekunden darstellt.
}

// "void *arg" ist nur konvention und wird vom lvgl_task nicht benötigt -> FreeRTOS schreibt einheitliche Funktions-signatur für alle Tasks vor
static void lvgl_task(void *arg) // lvgl_task ist die Endlosschleife, die die GUI am Laufen hält
{
    while (1) {
        uint32_t delay_ms = lv_timer_handler(); // lv_timer_handler() ist eine Funktion von LVGL, die alle Timer-Events verarbeitet und die GUI aktualisiert. Sie gibt die Zeit in Millisekunden zurück, bis das nächste Timer-Event fällig ist. delay_ms = 50 würde bedeuten, dass das nächste Timer-Event in 50 Millisekunden fällig ist, bzw. der lvgl_task in 50ms wieder aufgeweckt werden sollte, um die GUI zu aktualisieren.
        if (delay_ms == LV_NO_TIMER_READY) { //LV_NO_TIMER_READY ist ein spezieller Wert, der anzeigt, dass kein Timer-Event fällig ist. In diesem Fall wird delay_ms auf 20 gesetzt, um die GUI trotzdem regelmäßig zu aktualisieren.
            delay_ms = 20;
        }
        delay_ms = LV_CLAMP(1, delay_ms, 20); //GUI "schläft" minimal 1ms um die CPU nicht zu verschwenden, maximal 20ms um die GUI responsiv zu halten (also wenn nächster task zb erst in 500ms fällig wäre, wird trotzdem nach 20ms die GUI aktualisiert, um zB zwischenzeitliche Touch-Events zu verarbeiten)
        vTaskDelay(pdMS_TO_TICKS(delay_ms)); //vTaskDelay konvertiert ms in Ticks (weil FreeRTOS nicht mit ms, sondern Ticks arbeitet) -> sagt Scheduler, dass GUI-Task für delay_ms schlafen soll, damit andere Tasks (wie Netzwerk, Audio, etc.) CPU-Zeit bekommen.
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
    bool on = !cloud_tts_enabled(); //kommt aus cloud.h
    cloud_set_tts_enabled(on); //kommt aus cloud.h
    ui_tts_button_set_enabled(on); // TTS-Button-Optik sofort anpassen, damit der User sieht, dass er geklickt hat
    ui_chat_add_local_bubble(on ? "Voice replies on" : "Voice replies off"); // if (on) -> "Voice replies on" else -> "Voice replies off" chat bubble auf dem screen anzeigen
}

static void on_wifi_clicked(void)
{
    // LVGL-Task-Kontext. Setup-Portal öffnen (SoftAP + Web-Formular)
    // und dem Nutzer Hotspot + URL anzeigen.
    provisioning_start(); // hostet eigenes MiniLingo-Setup WLAN + startet kleinen Webserver unter 192.168.4.1 für SSID/Passwort + API-Key Eingabe
    ui_chat_add_local_bubble("Setup-WLAN: MiniLingo-Setup (PW minilingo)");
    ui_chat_add_local_bubble("Browser: http://192.168.4.1");
}

// Charakter aktiv schalten OHNE Reboot (Phase B2). Nur im LVGL-Task
// aufrufen: setzt Persona/Stimmen (Pointer-Swaps, aus jedem Task ok),
// persistiert den Index, stellt das Wakeword zur Laufzeit um (die AFE
// macht das selbst am fetch-Schleifenkopf) und zeigt eine Bubble. Der
// Idle-Text zieht ueber update_state_text() nach (liest s_character).
static void apply_character(int idx)
{
    // Schon aktiv (oder ungueltig) -> echter No-Op: kein Bubble, kein
    // Greeting, kein Wakeword-Reload. So bleibt ein Self-Trigger durch das
    // eigene Wake Word (Name == aktiver Charakter) folgenlos.
    if (idx < 0 || idx >= (int)NUM_CHARACTERS || idx == s_character) {
        return;
    }
    s_character = idx;
    const character_t *c = &s_characters[idx]; //Pointer auf den "character_t" bei Index "idx" in "s_characters[]"

    llm_set_system_prompt(c->persona); // Pfeiloperator (-> spart derefernzierung) zsm mit dem oben definierten Pointer auf das character_t-Struct um den Wert "persona" dieses character_t zu holen
    tts_set_voice(c->voice); // Gemini-Stimme (Fallback-TTS)
    tts_set_local_voice(c->local_voice); // lokaler Piper-Server-Stimme (Raspi)
    config_set_character(idx); // schreibt neuen Character-idx in Flash (NVS) und RAM (s_character)

    // Wakeword zur Laufzeit neu laden (~100 ms Taubheit) und den neuen
    // Charakter kurz begruessen lassen — in seiner Stimme (oben gesetzt;
    // cloud_speak nutzt sie im lokalen Piper, nur bei aktivem TTS).
    afe_pipeline_request_wakeword(idx);
    cloud_speak(c->greeting);

    char bubble[40];
    snprintf(bubble, sizeof(bubble), "Switched to %s", c->name); // Text für Chat-Bubble mit neuem Charakter
    ui_chat_add_local_bubble(bubble); // Chat-Bubble im UI anzeigen
}

static void on_character_clicked(void)
{
    // LVGL-Task-Kontext. Nächsten Charakter wählen — kein Reboot mehr:
    // das Wakeword wird zur Laufzeit getauscht (afe_pipeline, Phase B2).
    apply_character((s_character + 1) % NUM_CHARACTERS); //aktuellen character-index +1 + modulo um in range NUM_CHARACTERS zu bleiben
}

static void on_wifi_status(bool connected)
{
    atomic_store(&s_wifi_event, connected ? 1 : 0); // connected = true -> 1, connected = false -> 0
}

// Laeuft (wie alle Recorder-/Command-Callbacks) im afe_fetch_task.
static void on_wake_word(void)
{
    // Neue Session: Cloud-Pfad-Zustand der vorigen zuruecksetzen.
    s_pending_wav = NULL; // falls noch audio von letzter session vorhanden ist, wird es verworfen, da eine neue session gestartet wurde
    s_mn_timed_out = false; // MultiNet-Timeout-Flag zurücksetzen, da eine neue Session gestartet wurde und der Timeout für die neue Session erneut ablaufen muss
    atomic_store(&s_wake_event, true); //s_wake_event flag atomar auf true setzen
}

// Cloud-Pfad starten, sobald WAV + MultiNet-Timeout beide da sind.
// Der Recording-Buffer bleibt waehrend des Uploads gueltig, weil
// afe_pipeline bei cloud_is_busy() kein neues Capture startet.
static void submit_cloud_query(void)
{
    cloud_submit(s_pending_wav, s_pending_wav_bytes); // WAV-Datei und deren Größe an die Cloud-Pipeline übergeben, um die Verarbeitung zu starten
    s_pending_wav = NULL; //audio-buffer leeren, weil verarbeitet und jetzt nichtmehr benötigt
    s_mn_timed_out = false; // multinet-timeout zurücksetzen
    atomic_store(&s_processing_event, true); // "processing" flag setzen, damit auch UI geupdated werden kann (ist daann oben in der status bar sichtbar)
}

// Wird im afe_fetch_task aufgerufen, wenn die Aufnahme abgeschlossen ist. Es überprüft, ob die Aufnahme genügend Sprachanteil enthält und startet gegebenenfalls die Cloud-Pipeline.
static void on_recording_done(const uint8_t *wav, size_t wav_bytes,
                              uint32_t duration_ms, uint32_t speech_ms,
                              recorder_stop_reason_t reason)
{
    wav_dump_trigger(wav, wav_bytes); // nur optional für debugging zwecke -> speichert die aufnahme

    // Stille-Guard (Phase 8): fast stumme Aufnahme verwerfen statt
    // hochladen. Die MultiNet-Session laeuft normal in ihren Timeout;
    // on_command_timeout findet dann kein pending WAV mehr vor.
    if (speech_ms < MIN_SPEECH_MS) {
        ESP_LOGW(TAG, "discarding capture: only %u ms speech",
                 (unsigned)speech_ms);
        atomic_store(&s_too_quiet_event, true);
        return;
    }
    s_pending_wav = wav; // Pointer auf Audio-Buffer mit aufgenommener WAV
    s_pending_wav_bytes = wav_bytes; // Größe der aufgenommenen WAV-Datei in Bytes
    if (s_mn_timed_out) { 
        submit_cloud_query(); //falls MultiNet Timeout bereits erreicht, direkt die cloud-pipeline starten
    }
}

// MultiNet-Timeout = kein lokaler Command. Meist liegt das WAV schon
// bereit (Capture stoppt nach 1,5 s Stille, MultiNet erst nach 6 s);
// redet der User aber laenger als 6 s, kommt das Capture-Ende spaeter.
static void on_command_timeout(void)
{
    s_mn_timed_out = true; // MultiNet Timeout flag setzen, damit die Cloud-Pipeline gestartet werden kann, sobald das WAV fertig ist
    if (s_pending_wav) {
        submit_cloud_query(); //sobald s_pending_wav gesetzt ist (=aufnahme fertig)-> cloud-pipeline starten
    }
}

// Laeuft im network_task (Core 0): Ergebnis der Cloud-Pipeline.
//"cloud_event_type_t" = enum aus cloud.h, "const char *text" = pointer auf den text der cloud-event (zB transkript, antwort, fehlerbeschreibung)
static void on_cloud_event(cloud_event_type_t type, const char *text) 
{
    if (type == CLOUD_EVT_TRANSCRIPT) { // whisper liefert transkript zurück
        snprintf(s_transcript, sizeof(s_transcript), "%s", text); // speichert den erkannten Text (von Whisper STT)
        atomic_store(&s_transcript_event, true); // flag setzen um lvgl_task zu signalisieren, dass ein neues Transkript vorliegt und die UI geupdated werden kann
    } else if (type == CLOUD_EVT_REPLY) { // LLM liefert antwort zurück
        snprintf(s_reply, sizeof(s_reply), "%s", text); // speichert die Antwort des LLM
        atomic_store(&s_reply_event, true); // flag setzen um lvgl_task zu signalisieren, dass eine neue Antwort vorliegt und die UI geupdated werden kann
    } else if (type == CLOUD_EVT_SPEAKING) { // TTS-Wiedergabe startet
        atomic_store(&s_speaking_event, true); // signalisiert dem lvgl_task, dass die TTS-Wiedergabe gestartet wurde und die UI geupdated werden kann ("Speaking..." in der Statusbar)
    } else if (type == CLOUD_EVT_DONE) { // Pipeline fertig, zurück zu Idle
        atomic_store(&s_cloud_done_event, true); // signalisiert dem lvgl_task, dass die Cloud-Pipeline fertig ist und die UI geupdated werden kann (Statusbar wieder auf Idle (bzw "Listening for <character>") setzen)
    } else { // CLOUD_EVT_ERROR: kurze Fehlerbeschreibung (für UI)
        snprintf(s_cloud_error, sizeof(s_cloud_error), "%s", text); // speichert Fehlerbeschreibung
        atomic_store(&s_cloud_error_event, true); // flag setzen um lvgl_task zu signalisieren, dass ein neuer Fehler vorliegt und die UI geupdated werden kann
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
// command_id_t = enum aus commands.h, phrase = erkannte Phrase, prob = Wahrscheinlichkeit des Matches (0.0-1.0)
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

    recorder_abort(); // stoppt die Aufnahme sofort, damit die Cloud-Pipeline nicht gestartet wird (s_pending_wav wird nicht gesetzt) und der lokale Command sofort ausgeführt werden kann

    // Falls das Capture schon fertig war (Stille vor dem Match):
    // Aufnahme verwerfen, sie war ein lokaler Command.
    s_pending_wav = NULL;
    int arg = 0; // wird unten genutzt um rückgabewert von zb cancel_timer (1=erfolgreich, 0=kein timer aktiv) zu speichern um damit die passende wav datei für die canned_audio_play function zu wählen (zB CANNED_TIMER_CANCELLED oder CANNED_NO_TIMER)

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
    case CMD_TIMER_5SEC:
        kitchen_timer_start(5);
        canned_audio_play(CANNED_TIMER_SET_5S);
        break;
    case CMD_TIMER_10SEC:
        kitchen_timer_start(10);
        canned_audio_play(CANNED_TIMER_SET_10S);
        break;
    case CMD_TIMER_15SEC:
        kitchen_timer_start(15);
        canned_audio_play(CANNED_TIMER_SET_15S);
        break;
    case CMD_TIMER_STOP:
    case CMD_TIMER_CANCEL: // timer-stop und timer-cancel sollen gleiche Wirkung haben, können sich deswegen gleichen case-code teilen
        arg = kitchen_timer_cancel(); // 1=Timer war aktiv und wurde abgebrochen, 0=kein Timer
        canned_audio_play(arg ? CANNED_TIMER_CANCELLED : CANNED_NO_TIMER); // je nach wert von arg richtigen sound spielen
        break;
    case CMD_TIME_LEFT:
        arg = kitchen_timer_remaining_s(); //liefert restzeit in sekunden, oder -1 wenn kein timer aktiv ist
        // Feste Phrasen koennen keine variable Restzeit ansagen — die
        // exakte Zeit zeigen Bubble + Status-Bar (Phase-4-Entscheidung).
        canned_audio_play(arg >= 0 ? CANNED_TIME_LEFT : CANNED_NO_TIMER); // hinweis auf restzeit auf display abspielen oder hinweis dass kein timer aktiv ist
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
        volume_set((id - CMD_VOLUME_20 + 1) * VOLUME_STEP); //da id's fortlaufend in enum liegen, kommt beim klammerausdruck immer ein wert 1-5 raus -> dann mit VOLUME_STEP (20) multiplizieren um die gewünschte Lautstärke in Prozent zu erhalten
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
                  ? (int)(temperature_get() + 0.5f) // +0.5f für mathematische rundung, weil bei cast auf int einfach abgeschnitten wird (zB 23.7 -> 23) -> +0.5f sorgt dafür dass ab 23.5 aufgerundet wird auf 24
                  : INT_MIN; // INT_MIN = kleinster int-wert, wird genutzt um anzuzeigen dass der sensor nicht bereit ist (zB noch nicht initialisiert oder fehlerhaft)
        if (arg != INT_MIN && cloud_tts_enabled()) { //wenn temperatur gültig und TTS aktiv -> temperature über piper-tts-server ansagen
            char say[64];
            snprintf(say, sizeof(say), "The temperature is %d degrees", arg);
            cloud_speak(say);
        }
        break;
    case CMD_CHARACTER_MYCROFT:
    case CMD_CHARACTER_ALEXA:
    case CMD_CHARACTER_JARVIS:
        // Charakter-Wechsel (Phase B2): den eigentlichen Swap macht der
        // LVGL-Task (apply_character) — hier nur den Ziel-Index melden.
        // IDs liegen fortlaufend, Index = id - CMD_CHARACTER_MYCROFT.
        // Kein s_cmd_event: die Bubble kommt aus apply_character.
        atomic_store(&s_character_event, (int)id - CMD_CHARACTER_MYCROFT);
        return;
    case CMD_CHARACTER_NEXT:
        // Namensunabhaengig zum naechsten Charakter (LVGL kennt s_character).
        atomic_store(&s_cycle_event, true);
        return;
    }

    atomic_store(&s_cmd_arg, arg); // speichert Kontext in s_cmd_arg -> wird später von lvgl_task gelesen und verarbeitet
    atomic_store(&s_cmd_event, (int)id); // setzt die s_cmd_event-flag auf die command-id, damit lvgl_task weiß, dass ein neuer Command vorliegt und die UI geupdated werden kann
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
    case CMD_TIMER_5SEC:   snprintf(text, len, "Timer set: 5 sec"); break;
    case CMD_TIMER_10SEC:  snprintf(text, len, "Timer set: 10 sec"); break;
    case CMD_TIMER_15SEC:  snprintf(text, len, "Timer set: 15 sec"); break;
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

// LVGL-Timer: nach 800 ms den Mic-Button wieder auf Idle setzen (Phase 8).
static void mic_button_reset_cb(lv_timer_t *timer)
{
    ui_mic_button_set_state(UI_MIC_IDLE);
}

// Status-Text mittig in der Bar: Listening > Processing >
// Timer-Countdown > Idle (Spec Section 4: "Timer: 04:23" ersetzt den
// State-Text).
static void update_state_text(void)
{
    char text[24]; // in status bar ist nicht so viel platz, deswegen nur 24 zeichen
    int left = kitchen_timer_remaining_s(); // holt restzeit eines timers falls einer läuft (sonst -1)

    if (s_listening) {
        snprintf(text, sizeof(text), "Listening..."); // s_listening flag gesetzt -> status bar zeigt "Listening..." an
    } else if (s_processing) {
        snprintf(text, sizeof(text), "Processing..."); // s_processing flag gesetzt -> status bar zeigt "Processing..." an
    } else if (s_speaking) {
        snprintf(text, sizeof(text), "Speaking..."); // s_speaking flag gesetzt -> status bar zeigt "Speaking..." an
    } else if (left >= 0) {
        snprintf(text, sizeof(text), "Timer: %02d:%02d", left / 60,
                 left % 60); // s_listening, s_processing und s_speaking flags nicht gesetzt, aber ein timer läuft -> status bar zeigt restzeit des timers an
    } else {
        snprintf(text, sizeof(text), "%s",
                 s_characters[s_character].idle_text); // kein timer, kein listening, kein processing, kein speaking -> status bar zeigt idle text des aktuellen charakters an ("Listning for <character>")
    }

    // statusbar-text nur updaten wenn wirklich änderung nötig:
    if (strcmp(text, s_state_text) != 0) { // wennn result von strcmp() = 0 -> strings sind gleich, wenn != 0 -> strings sind unterschiedlich -> nur dann statusbar-text updaten
        strcpy(s_state_text, text); // neuen text in s_state_text kopieren
        ui_status_bar_set_state_text(text); // statusbar-text auf dem display updaten
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
        ui_status_bar_set_volume(v); // aktuelle Lautstärke in Prozent auf der Statusbar anzeigen (Label + Slider-Postion werden geupdated)
    }
}

// LVGL-Timer: holt Events aus den anderen Task-Kontexten ab.
// checkt ob flags gesetzt sind, updated die UI entsprechend und setzt flags wieder zurück (atomic_exchange). Wird alle 50 ms aufgerufen (LVGL-Timer).
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

    // Charakter-Wechsel per Voice (Phase B2): im LVGL-Task anwenden.
    // Gleiches UI-Feedback wie ein lokaler Command (s_cmd_event oben):
    // Listening beenden, kurz gruen, dann zurueck auf Idle — sonst bliebe
    // die Bar auf "Listening..." haengen (der Recorder ist da laengst
    // per recorder_abort() gestoppt, nur der UI-State fehlte).
    int chr = atomic_exchange(&s_character_event, -1);
    bool cycle = atomic_exchange(&s_cycle_event, false);
    if (chr >= 0 || cycle) {
        apply_character(cycle ? (s_character + 1) % NUM_CHARACTERS : chr);
        s_listening = false;
        ui_mic_button_set_state(UI_MIC_LOCAL_RESPONSE);
        lv_timer_set_repeat_count(
            lv_timer_create(mic_button_reset_cb, 800, NULL), 1);
    }

    update_state_text();
    update_volume_display();
}

void app_main(void)
{
    ESP_LOGI(TAG, "MiniLingo Phase 8 starting");

    esp_err_t err = nvs_flash_init(); // NVS-Flash initialisieren -> Treiber laden, Speicher vorbereiten, etc.

    // NVS-Flash kann voll sein oder eine neue Version haben -> dann Flash löschen und neu initialisieren
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err); // Fehlerbehandlung: wenn nvs_flash_init() oder nvs_flash_erase() fehlschlägt, wird das Programm hier abgebrochen und eine Fehlermeldung ausgegeben (sieht man dann im serial monitor)

    // Laufzeit-Config aus NVS laden (SSID/Keys/Modelle); fällt auf die
    // Kconfig-Defaults zurück, wenn nichts provisioniert ist.
    ESP_ERROR_CHECK(config_init());

    // Aktiven Charakter aus dem NVS wiederherstellen (Phase B). Persona
    // hier setzen (llm = network_task, Pointer-Tausch); der Wakenet-
    // Filter wird nach afe_pipeline_init scharfgeschaltet.
    s_character = config_character_index(); // holt letzten ausgewählten Character aus NVS (siehe config.c)
    if (s_character < 0 || s_character >= (int)NUM_CHARACTERS) {
        s_character = 0;
    }
    llm_set_system_prompt(s_characters[s_character].persona);
    tts_set_voice(s_characters[s_character].voice);
    tts_set_local_voice(s_characters[s_character].local_voice);
    // -1 = kein Charakter-Wechsel angefordert (0 waere Mycroft).
    atomic_store(&s_character_event, -1); // character-event-flag initialisieren mit -1 (kein character-change-event)

    lv_init(); // LVGL initialisieren (GUI-Framework)
    lv_tick_set_cb(lvgl_tick_get_cb); // LVGL-Tick-Callback setzen (wird alle 1 ms aufgerufen, um die Zeit für LVGL zu aktualisieren)

    ESP_ERROR_CHECK(display_init()); // Display initialisieren (Treiber laden, Display vorbereiten, etc.)
    ESP_ERROR_CHECK(touch_init()); // Touchscreen initialisieren (Treiber laden, Touch vorbereiten, etc.)

    ui_init(); // UI-Elemente initialisieren (Statusbar, Chat, Buttons, etc.)
    ui_mic_button_set_click_cb(on_mic_button_clicked); // Callback für Mic-Button setzen (wenn der Button geklickt wird, wird die Funktion on_mic_button_clicked aufgerufen)
    ui_status_bar_set_volume_changed_cb(on_volume_slider_changed); // Callback für Volume-Slider setzen (wenn der Slider bewegt wird, wird die Funktion on_volume_slider_changed aufgerufen)
    ui_status_bar_set_wifi_clicked_cb(on_wifi_clicked); // Callback für WiFi-Icon setzen (wenn das WiFi-Icon geklickt wird, wird die Funktion on_wifi_clicked aufgerufen)
    ui_status_bar_set_state_clicked_cb(on_character_clicked); // Callback für Statusbar-Text setzen (wenn der Text in der Statusbar geklickt wird, wird die Funktion on_character_clicked aufgerufen)
    ui_status_bar_set_state_text(s_characters[s_character].idle_text); // Statusbar-Text initial auf den Idle-Text des aktuellen Charakters setzen
    ui_status_bar_set_wifi_connected(false);
    // TTS-Toggle: Startzustand aus demselben Kconfig wie cloud.c (vor
    // dem lvgl_task setzen — danach waeren UI-Calls nicht thread-safe).
    ui_tts_button_set_click_cb(on_tts_button_clicked);
#ifdef CONFIG_TTS_SPEAK_REPLIES // in sdconfig.h auf 1 gesetzt, damit TTS-Button defaultmäßig enabled
    ui_tts_button_set_enabled(true);
#else
    ui_tts_button_set_enabled(false); // falls irgendwann defaultmäßig deaktiviertes TTS gewünscht ist, in sdconfig.h CONFIG_TTS_SPEAK_REPLIES auf 0 setzen
#endif
    lv_timer_create(app_event_timer_cb, 50, NULL); //lvgl timer mit callback auf app_event_timer_cb erstellen -> alle 50 ms wird dieser callback aufgerufen um UI zu aktualisieren (NULL parameter, weil keine zusätzlichen Daten benötigt werden)

    // erstelle lvgl task, der function lvgl_task auf Core 0 mit Prio 4 ausführt und Stackgröße von 8192 Bytes hat (8KB genug für lokale variablen und verschachtelte funktionsaufrufe) 
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
    ui_status_bar_set_volume(volume_get()); // Statusbar-Volume-Label initial auf den aktuellen Gain setzen
    ESP_ERROR_CHECK(canned_audio_init());
    kitchen_timer_init(on_timer_done); // kitchen_timer initialisieren und Callback für Timer-Done-Event setzen

    // DS18B20: Fehler nicht fatal — Sensor koennte nicht angeschlossen sein.
    if (temperature_init() != ESP_OK) {
        ESP_LOGW(TAG, "temperature sensor not found");
    }

    // Speech-Pipeline: Models einmal laden, dann MultiNet (commands)
    // VOR dem Pipeline-Start initialisieren — der fetch-Task fuettert
    // die Commands ab dem ersten Wake Word.
    wav_dump_init(); // optional für debugging
    wake_word_set_listener(on_wake_word); // Callback für Wake-Word-Event setzen (wird aufgerufen, wenn das Wake-Word erkannt wird)
    ESP_ERROR_CHECK(recorder_init(on_recording_done));
    ESP_ERROR_CHECK(audio_in_init());
    srmodel_list_t *models = esp_srmodel_init("model"); // Speech-Recognition-Modelle initialisieren (Wake-Word + MultiNet) -> liefert Pointer auf srmodel_list_t zurück, der die geladenen Modelle enthält
    ESP_ERROR_CHECK(commands_init(models, on_local_command,
                                  on_command_timeout));
    // Nur das Wakeword des aktiven Charakters laden (RAM, s. afe_pipeline).
    ESP_ERROR_CHECK(afe_pipeline_init(models, s_character)); // AFE-Pipeline initialisieren (Wake-Word + MultiNet) -> nur das Wake-Word des aktuellen Charakters wird geladen, damit RAM gespart wird

    ESP_LOGI(TAG, "Phase 8 up: local commands + cloud STT + LLM"
#if CONFIG_TTS_SPEAK_REPLIES
                  " + TTS"
#endif
    );
}
