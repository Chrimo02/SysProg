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
#include <limits.h>                    // [C] Standard-Header, hier für INT_MIN (Sentinel-Wert bei ungültiger Temperatur)
#include <stdatomic.h>                 // [C] C11-Header für atomic_int/atomic_bool/atomic_store/atomic_exchange (Cross-Task-Kommunikation ohne Mutex)
#include <stdio.h>                     // [C] snprintf/strcpy-Umfeld für Textbuffer
#include <string.h>                    // [C] strcmp/strcpy für Textvergleich/-kopie

#include "freertos/FreeRTOS.h"         // [HW] FreeRTOS-Basistypen/Makros (Tasks, Ticks)
#include "freertos/task.h"             // [HW] xTaskCreatePinnedToCore/vTaskDelay für Task-Erzeugung und Verzögerung

#include "esp_log.h"                   // [Logik] ESP_LOGI/ESP_LOGW-Logging über UART-Konsole
#include "esp_system.h"                // [HW] ESP-IDF-Systemfunktionen (u.a. Reset-Ursachen, Chip-Infos)
#include "esp_timer.h"                 // [HW] esp_timer_get_time() als hochauflösende Zeitquelle für den LVGL-Tick
#include "nvs_flash.h"                 // [HW] NVS-Flash-Init/-Erase-API
#include "sdkconfig.h"                 // [C] generierte Kconfig-Makros (CONFIG_TTS_SPEAK_REPLIES etc.)
#include "lvgl.h"                      // [HW] LVGL-Grafikbibliothek (Timer, Widgets, Tick-Callback)

#include "debug/wav_dump.h"            // [Logik] optionales Mitschneiden der WAV-Aufnahmen für Debugging
#include "drivers/audio_in.h"          // [HW] Mikrofon-Treiber (I2S-RX, siehe audio_in.c)
#include "drivers/audio_out.h"         // [HW] Lautsprecher-Treiber (I2S-TX)
#include "drivers/display.h"           // [HW] Display-Treiber-Init
#include "drivers/touch.h"             // [HW] Touch-Controller-Init
#include "local/canned_audio.h"        // [Logik] fest eingesprochene Audio-Antworten aus SPIFFS (Timer/Volume/Fehler)
#include "local/temperature.h"         // [HW] DS18B20-Temperatursensor-Treiber
#include "local/timer.h"               // [Logik] Kitchen-Timer-Logik (Start/Cancel/Restzeit)
#include "local/volume.h"              // [Logik] Lautstärkeregelung (Gain-Verwaltung)
#include "config.h"                    // [Logik] Laufzeit-Konfiguration aus NVS (SSID/Keys/Modelle/Charakter)
#include "network/cloud.h"             // [Logik] Cloud-Pipeline-Koordination (Whisper -> LLM -> TTS)
#include "network/llm.h"               // [Logik] LLM-Anbindung (System-Prompt/Request-Aufbau)
#include "network/provisioning.h"      // [HW] SoftAP-Setup-Portal für WLAN-Provisioning
#include "network/tts.h"               // [HW] Text-to-Speech-Anbindung (Gemini/lokaler Piper-Server)
#include "network/wifi.h"              // [HW] WiFi-STA-Verbindungsaufbau
#include "speech/afe_pipeline.h"       // [HW] Audio-Front-End-Pipeline (Wake-Word/VAD/Recorder-Orchestrierung)
#include "speech/commands.h"           // [Logik] lokale Sprachbefehle (MultiNet-Erkennung + IDs)
#include "speech/recorder.h"           // [Logik] Aufnahme-Puffer-Verwaltung (WAV-Erzeugung)
#include "speech/wake_word.h"          // [HW] WakeNet-Wake-Word-Erkennung
#include "ui/ui.h"                     // [HW] LVGL-UI-Aufbau (Mic-Button, Status-Bar, Chat-Bubbles)

static const char *TAG = "main";       // [C] static = Datei-lokale Linkage | [Logik] Log-Tag zur Zuordnung der Ausgaben

#define INITIAL_VOLUME   60            // [C] Präprozessor-Konstante | [Logik] Start-Lautstärke in Prozent beim Boot

// Charakter-Switcher (Phase B): Klick auf den State-Text cyclet durch
// diese Tabelle. Die Reihenfolge MUSS zum Wakenet-Model-Index-Mapping in
// afe_pipeline.c passen (0=Mycroft -> Model 1 usw.). persona ist der
// austauschbare System-Prompt-Anfang; die Tool-Hinweise hängt llm.c an.
//
// Ein Charakter-Profil: Anzeigename, Idle-Text, LLM-Persona-Prompt und
// die zugehörigen TTS-Stimmen (Cloud- und lokaler Piper-Server) sowie
// ein Begrüßungssatz nach dem Wechsel. s_characters[] enthält die
// konkreten Profile, apply_character() schaltet zwischen ihnen um.
typedef struct {
    const char *name;         // [Logik] Anzeigename, u.a. für die "Switched to ..."-Bubble
    const char *idle_text;    // [Logik] Text der Status-Bar im Idle-Zustand ("Listening for ...")
    const char *persona;      // [Logik] System-Prompt-Anfang, den llm_set_system_prompt() ans LLM schickt
    const char *voice;        // Gemini-Stimme (Fallback-TTS)   // [Logik] Stimmname für die Cloud-TTS (Gemini)
    const char *local_voice;  // Piper-Stimme des lokalen Servers (primaer)   // [Logik] Stimmname für den lokalen Piper-TTS-Server
    const char *greeting;     // kurzer Begruessungssatz nach dem Switch   // [Logik] wird nach apply_character() per cloud_speak() vorgelesen
} character_t;

// Konkrete Charakter-Profile in fester Reihenfolge (Index = Wakenet-
// Model-Index in afe_pipeline.c). Wird von apply_character(),
// update_state_text() und den Voice-Commands (CMD_CHARACTER_*) indiziert.
static const character_t s_characters[] = {                          // [C] Array-Initialisierung von Struct-Literalen mit Designated-artigem Feld-Layout
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
#define NUM_CHARACTERS (sizeof(s_characters) / sizeof(s_characters[0]))  // [C] sizeof(array)/sizeof(element) = Elementanzahl zur Compile-Zeit | [Logik] Anzahl verfügbarer Charaktere

// Stille-Guard (Phase 8): Aufnahmen mit weniger VAD-Sprache gehen
// nicht zu Whisper — der halluziniert aus (fast) reiner Stille Saetze
// wie "Vielen Dank." (beobachtet 2026-06-12), und Gemini antwortet
// dann artig darauf. Gemessen am Geraet (2026-06-12): leere Captures
// melden trotzdem 576-608 ms "Speech" (VAD-Nachhall des Wake Words);
// eine echte kurze Frage liegt bei 3700 ms. 1000 ms trennt sauber —
// selbst Mini-Queries ("Wer bist du?") kommen mit Nachhall drueber.
#define MIN_SPEECH_MS    1000          // [C] Präprozessor-Konstante | [Logik] Schwellwert in ms; darunter gilt eine Aufnahme als reines VAD-Nachhall-Rauschen

// -1 = keine Aenderung, 0 = getrennt, 1 = verbunden
static atomic_int s_wifi_event = -1;   // [C] atomarer Int, lock-frei zwischen Event-Loop-Task und LVGL-Task lesbar/schreibbar | [Logik] Tri-State-Flag für WiFi-Statuswechsel
static atomic_bool s_wake_event;       // [C] atomares bool-Flag | [Logik] signalisiert: Wake Word wurde erkannt (afe_fetch_task -> LVGL-Task)
// 0 = kein Event, sonst command_id_t. s_cmd_arg traegt den Kontext fuer
// den Bubble-Text (Restzeit/Volume/Cancel-Erfolg) und wird VOR der ID
// geschrieben (einziger Writer: afe_fetch_task).
static atomic_int s_cmd_event;         // [Logik] letzter ausgelöster lokaler Command, als command_id_t codiert
static atomic_int s_cmd_arg;           // [Logik] Zusatzwert zum Command (z.B. neue Lautstärke, Restzeit)
// Charakter-Wechsel per Voice (Phase B2): Ziel-Index, gesetzt im
// afe_fetch_task (on_local_command), angewandt im LVGL-Task
// (apply_character). -1 = kein Event.
static atomic_int s_character_event;   // [Logik] per Voice angeforderter Ziel-Charakter-Index, -1 = kein Wechsel ausstehend
// Namensunabhaengiger Cycle-Wechsel (Teil A): gesetzt im afe_fetch_task,
// angewandt im LVGL-Task (naechster Charakter, wie der Touch-Button).
static atomic_bool s_cycle_event;      // [Logik] Flag: "wechsle zum nächsten Charakter" ohne konkreten Ziel-Index
static atomic_bool s_timer_done_event; // [Logik] Flag: Kitchen-Timer ist abgelaufen (esp_timer-Task -> LVGL-Task)
// Cloud-Pipeline (Phase 5-7): processing = Upload gestartet
// (afe_fetch_task), transcript/reply/speaking/done/error = Fortschritt
// (network_task). Die Textbuffer werden jeweils VOR dem Flag
// geschrieben; es ist nur ein Cloud-Query gleichzeitig in flight
// (cloud_is_busy-Gate).
static atomic_bool s_processing_event; // [Logik] Flag: Cloud-Upload wurde gestartet, UI soll "Processing..." zeigen
static atomic_bool s_too_quiet_event;  // [Logik] Flag: Aufnahme wurde als zu leise verworfen (Stille-Guard)
static atomic_bool s_transcript_event; // [Logik] Flag: Whisper-Transkript liegt vor (in s_transcript)
static atomic_bool s_reply_event;      // [Logik] Flag: LLM-Antwort liegt vor (in s_reply)
static atomic_bool s_speaking_event;   // [Logik] Flag: TTS-Wiedergabe hat begonnen
static atomic_bool s_cloud_done_event; // [Logik] Flag: kompletter Cloud-Turn abgeschlossen
static atomic_bool s_cloud_error_event;// [Logik] Flag: Cloud-Pipeline ist mit Fehler abgebrochen (Text in s_cloud_error)
static char s_transcript[512];         // [C] statisches Array fester Größe | [Logik] Puffer für das zuletzt empfangene Whisper-Transkript
static char s_reply[1024];             // [Logik] Puffer für die zuletzt empfangene LLM-Antwort
static char s_cloud_error[48];         // [Logik] Puffer für die zuletzt empfangene Fehlermeldung der Cloud-Pipeline

// Cloud-Pfad-Koordination, nur im afe_fetch_task: Upload erst, wenn
// BEIDES vorliegt — Capture-Ende (WAV fertig) und MultiNet-Timeout
// (kein lokaler Command; die MultiNet-Session lebt laenger als das
// Capture, siehe afe_pipeline.c).
static const uint8_t *s_pending_wav;   // [C] Zeiger auf den fertigen WAV-Puffer, NULL solange kein Capture aussteht | [HW] rohe PCM/WAV-Daten des Recorders
static size_t s_pending_wav_bytes;     // [C] size_t = vorzeichenlose Größe | [Logik] Länge des ausstehenden WAV-Puffers in Byte
static bool s_mn_timed_out;            // [Logik] merkt sich, ob MultiNet ohne Command-Match in den Timeout gelaufen ist

// Nur im LVGL-Task benutzt (app_event_timer_cb / Status-Text-Logik).
static bool s_listening;               // [Logik] UI-Zustand: aktuell wird zugehört (Mic-Button/Status-Text)
static bool s_processing;              // [Logik] UI-Zustand: Cloud-Anfrage läuft
static bool s_speaking;                // [Logik] UI-Zustand: TTS spielt gerade ab
static char s_state_text[24];          // [Logik] zuletzt gesetzter Status-Text, verhindert unnötige Redraws
static int s_volume_shown = -1;        // [C] Initialwert -1 erzwingt beim ersten Aufruf ein Update | [Logik] zuletzt in der UI angezeigte Lautstärke
static int s_character;  // aktiver Charakter-Index (LVGL-Task)   // [Logik] Index in s_characters[], der aktuell aktive Charakter

// Liefert die aktuelle Zeit in Millisekunden als Tick-Quelle für LVGL.
// LVGL braucht diese Callback-Funktion, um Animationen/Timer zeitlich
// korrekt zu takten.
static uint32_t lvgl_tick_get_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);  // [C] expliziter Cast auf uint32_t, Ganzzahldivision | [HW] esp_timer liefert Mikrosekunden seit Boot, /1000 -> Millisekunden
}

// Eigener FreeRTOS-Task, der die LVGL-Timer-Engine am Laufen hält:
// ruft periodisch lv_timer_handler() auf (Redraw/Input-Verarbeitung)
// und schläft dazwischen so lange, wie LVGL selbst vorschlägt.
static void lvgl_task(void *arg)
{
    while (1) {                                        // [C] Endlosschleife, typisch für FreeRTOS-Tasks (kehren nie zurück)
        uint32_t delay_ms = lv_timer_handler();         // [HW] verarbeitet fällige LVGL-Timer/Redraws, liefert empfohlene Wartezeit bis zum nächsten Aufruf
        if (delay_ms == LV_NO_TIMER_READY) {            // [Logik] Sonderwert: aktuell kein Timer fällig
            delay_ms = 20;                              // [Logik] Fallback-Polling-Intervall
        }
        delay_ms = LV_CLAMP(1, delay_ms, 20);           // [C] Makro klemmt Wert auf einen Bereich | [Logik] Wartezeit auf sinnvolles Intervall begrenzen (1-20 ms)
        vTaskDelay(pdMS_TO_TICKS(delay_ms));            // [HW] gibt die CPU für delay_ms Millisekunden an andere Tasks ab
    }
}

// Laeuft im LVGL-Task-Kontext (Slider-Event, Phase 8b). volume_set ist
// atomar und damit aus diesem Task sicher; die Anzeige wird direkt
// aktualisiert (gleicher Task wie die UI).
static void on_volume_slider_changed(int percent)
{
    volume_set(percent);                                // [Logik] neue Lautstärke aus dem Slider übernehmen
    ui_status_bar_set_volume(percent);                  // [HW] Lautstärke-Anzeige in der Status-Bar aktualisieren
}

// Klick-Handler des Mic-Buttons: startet eine Listening-Session wie
// das Wake Word.
static void on_mic_button_clicked(void)
{
    // Laeuft im LVGL-Task-Kontext (Klick-Event). Startet eine
    // Listening-Session wie das Wake Word (Spec Section 2: Trigger =
    // "Wake Word | Touch"); das UI-Feedback (roter Button,
    // "Listening...") kommt ueber denselben Wake-Event-Pfad zurueck.
    // Waehrend laufender Session/Cloud-Pipeline wird der Klick in der
    // Pipeline ignoriert.
    afe_pipeline_request_listening();                   // [Logik] fordert bei der AFE-Pipeline eine manuelle Listening-Session an
}

// Klick-Handler des TTS-Toggle-Buttons: schaltet Sprachausgabe der
// Cloud-Antworten ein/aus.
static void on_tts_button_clicked(void)
{
    // LVGL-Task-Kontext. cloud_set_tts_enabled ist atomar; das eigentliche
    // tts_init (2,5 MB PSRAM) passiert lazy im network_task beim ersten
    // Vorlesen. Button-Optik + kurze Bestaetigung direkt hier.
    bool on = !cloud_tts_enabled();                     // [C] '!' logisches NICHT: Zustand invertieren | [Logik] neuen Soll-Zustand aus dem Gegenteil des aktuellen bilden
    cloud_set_tts_enabled(on);                          // [Logik] TTS global ein-/ausschalten
    ui_tts_button_set_enabled(on);                      // [HW] Button-Optik an neuen Zustand anpassen
    ui_chat_add_local_bubble(on ? "Voice replies on" : "Voice replies off");  // [C] Ternärer Operator wählt den Bubble-Text | [Logik] kurze Nutzer-Rückmeldung
}

// Klick-Handler des WiFi-Icons: öffnet das Setup-Portal für manuelles
// (Re-)Provisioning.
static void on_wifi_clicked(void)
{
    // LVGL-Task-Kontext. Setup-Portal öffnen (SoftAP + Web-Formular)
    // und dem Nutzer Hotspot + URL anzeigen.
    provisioning_start();                                                       // [HW] startet SoftAP + Webserver für das Provisioning-Formular
    ui_chat_add_local_bubble("Setup-WLAN: MiniLingo-Setup (PW minilingo)");     // [Logik] zeigt dem Nutzer die Zugangsdaten des Setup-Hotspots
    ui_chat_add_local_bubble("Browser: http://192.168.4.1");                   // [HW] Standard-Gateway-IP im SoftAP-Modus als Portal-URL
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
    if (idx < 0 || idx >= (int)NUM_CHARACTERS || idx == s_character) {  // [C] '||' logisches ODER, Cast auf int für Vergleich | [Logik] ungültiger oder bereits aktiver Index -> abbrechen
        return;                                                        // [Logik] No-Op, kein Zustandswechsel
    }
    s_character = idx;                                                 // [Logik] neuen aktiven Charakter-Index übernehmen
    const character_t *c = &s_characters[idx];                         // [C] const-Zeiger auf das gewählte Profil | [Logik] Kurzform für die folgenden Feldzugriffe

    llm_set_system_prompt(c->persona);                                 // [C] Feldzugriff per '->' auf Zeiger-Struct | [Logik] LLM-Persona umschalten
    tts_set_voice(c->voice);                                           // [Logik] Cloud-TTS-Stimme auf die des neuen Charakters setzen
    tts_set_local_voice(c->local_voice);                               // [Logik] lokale Piper-Stimme auf die des neuen Charakters setzen
    config_set_character(idx);                                        // [Logik] Auswahl persistent im NVS ablegen
    // Wakeword zur Laufzeit neu laden (~100 ms Taubheit) und den neuen
    // Charakter kurz begruessen lassen — in seiner Stimme (oben gesetzt;
    // cloud_speak nutzt sie im lokalen Piper, nur bei aktivem TTS).
    afe_pipeline_request_wakeword(idx);                               // [HW] lädt das passende WakeNet-Modell für den neuen Charakter-Namen nach
    cloud_speak(c->greeting);                                          // [Logik] spricht den Begrüßungssatz des neuen Charakters

    char bubble[40];                                                   // [C] lokales Array fester Größe auf dem Stack
    snprintf(bubble, sizeof(bubble), "Switched to %s", c->name);        // [C] snprintf begrenzt auf Puffergröße, verhindert Overflow
    ui_chat_add_local_bubble(bubble);                                  // [Logik] Wechsel als Bubble im Chat sichtbar machen
}

// Klick-Handler des Status-Texts: wechselt per Touch zum nächsten
// Charakter (namensunabhängiger Cycle wie CMD_CHARACTER_NEXT per Voice).
static void on_character_clicked(void)
{
    // LVGL-Task-Kontext. Nächsten Charakter wählen — kein Reboot mehr:
    // das Wakeword wird zur Laufzeit getauscht (afe_pipeline, Phase B2).
    apply_character((s_character + 1) % NUM_CHARACTERS);   // [C] Modulo-Operator realisiert zirkulären Index | [Logik] nächsten Charakter in der Liste wählen (mit Wrap-Around)
}

// WiFi-Status-Callback (läuft im Event-Loop-Task): meldet Verbindungs-
// wechsel als atomares Flag an den LVGL-Task weiter.
static void on_wifi_status(bool connected)
{
    atomic_store(&s_wifi_event, connected ? 1 : 0);   // [C] atomic_store schreibt lock-frei über Task-Grenzen hinweg | [Logik] Tri-State-Flag auf verbunden/getrennt setzen
}

// Laeuft (wie alle Recorder-/Command-Callbacks) im afe_fetch_task.
// Wird bei jeder erkannten Wake-Word-Äußerung aufgerufen und setzt den
// Cloud-Pfad-Zustand für eine neue Session zurück.
static void on_wake_word(void)
{
    // Neue Session: Cloud-Pfad-Zustand der vorigen zuruecksetzen.
    s_pending_wav = NULL;                              // [Logik] kein WAV aus einer vorherigen Session mehr ausstehend
    s_mn_timed_out = false;                            // [Logik] MultiNet-Timeout-Flag für die neue Session zurücksetzen
    atomic_store(&s_wake_event, true);                 // [Logik] signalisiert dem LVGL-Task: Wake Word erkannt, UI auf "Listening" schalten
}

// Cloud-Pfad starten, sobald WAV + MultiNet-Timeout beide da sind.
// Der Recording-Buffer bleibt waehrend des Uploads gueltig, weil
// afe_pipeline bei cloud_is_busy() kein neues Capture startet.
static void submit_cloud_query(void)
{
    cloud_submit(s_pending_wav, s_pending_wav_bytes);  // [Logik] übergibt den fertigen WAV-Puffer an die Cloud-Pipeline (Whisper-Upload)
    s_pending_wav = NULL;                              // [Logik] Zustand zurücksetzen, verhindert doppeltes Absenden
    s_mn_timed_out = false;                            // [Logik] Timeout-Flag für die nächste Session zurücksetzen
    atomic_store(&s_processing_event, true);           // [Logik] UI auf "Processing..." schalten lassen
}

// Recorder-Callback: wird aufgerufen, sobald eine Aufnahme (WAV) fertig
// ist. Filtert per Stille-Guard leere Captures aus und stößt bei
// bereits abgelaufenem MultiNet-Timeout direkt den Cloud-Upload an.
static void on_recording_done(const uint8_t *wav, size_t wav_bytes,
                              uint32_t duration_ms, uint32_t speech_ms,
                              recorder_stop_reason_t reason)
{
    wav_dump_trigger(wav, wav_bytes);                  // [Logik] optionales Debug-Mitschneiden der Aufnahme
    // Stille-Guard (Phase 8): fast stumme Aufnahme verwerfen statt
    // hochladen. Die MultiNet-Session laeuft normal in ihren Timeout;
    // on_command_timeout findet dann kein pending WAV mehr vor.
    if (speech_ms < MIN_SPEECH_MS) {                   // [Logik] weniger erkannte Sprache als Schwellwert -> vermutlich nur Nachhall des Wake Words
        ESP_LOGW(TAG, "discarding capture: only %u ms speech",
                 (unsigned)speech_ms);                 // [C] expliziter Cast auf unsigned für den Format-Spezifizierer %u
        atomic_store(&s_too_quiet_event, true);        // [Logik] UI-Hinweis "Didn't catch that" auslösen
        return;                                        // [Logik] Aufnahme wird verworfen, kein Cloud-Upload
    }
    s_pending_wav = wav;                               // [C] Zeiger auf den Recorder-eigenen WAV-Puffer übernehmen | [Logik] Aufnahme merken, bis MultiNet-Timeout feststeht
    s_pending_wav_bytes = wav_bytes;                    // [Logik] zugehörige Puffergröße merken
    if (s_mn_timed_out) {                               // [Logik] MultiNet war schon vorher in den Timeout gelaufen (kein lokaler Command)?
        submit_cloud_query();                           // [Logik] beide Bedingungen erfüllt -> Cloud-Upload jetzt anstoßen
    }
}

// MultiNet-Timeout = kein lokaler Command. Meist liegt das WAV schon
// bereit (Capture stoppt nach 1,5 s Stille, MultiNet erst nach 6 s);
// redet der User aber laenger als 6 s, kommt das Capture-Ende spaeter.
static void on_command_timeout(void)
{
    s_mn_timed_out = true;                             // [Logik] merkt: kein lokaler Command wurde erkannt
    if (s_pending_wav) {                                // [C] Zeiger als Bedingung: != NULL | [Logik] WAV liegt bereits vor -> beide Bedingungen erfüllt
        submit_cloud_query();                          // [Logik] Cloud-Upload jetzt anstoßen
    }
}

// Laeuft im network_task (Core 0): Ergebnis der Cloud-Pipeline.
// Verteilt die verschiedenen Cloud-Ereignistypen (Transkript, Antwort,
// Sprachausgabe-Start, fertig, Fehler) auf ihre jeweiligen atomaren
// Flags/Textbuffer für den LVGL-Task.
static void on_cloud_event(cloud_event_type_t type, const char *text)
{
    if (type == CLOUD_EVT_TRANSCRIPT) {                                // [C] Vergleich gegen Enum-Wert steuert Verzweigung
        snprintf(s_transcript, sizeof(s_transcript), "%s", text);      // [C] Puffergrößen-begrenztes Kopieren
        atomic_store(&s_transcript_event, true);                      // [Logik] User-Bubble mit Transkript anzeigen lassen
    } else if (type == CLOUD_EVT_REPLY) {
        snprintf(s_reply, sizeof(s_reply), "%s", text);               // [Logik] LLM-Antworttext für die UI zwischenspeichern
        atomic_store(&s_reply_event, true);                           // [Logik] Assistant-Bubble anzeigen lassen
    } else if (type == CLOUD_EVT_SPEAKING) {
        atomic_store(&s_speaking_event, true);                        // [Logik] UI in den SPEAKING-Zustand versetzen
    } else if (type == CLOUD_EVT_DONE) {
        atomic_store(&s_cloud_done_event, true);                      // [Logik] Pipeline abgeschlossen, UI zurück auf Idle
    } else {
        snprintf(s_cloud_error, sizeof(s_cloud_error), "%s", text);   // [Logik] Fehlertext für die UI zwischenspeichern
        atomic_store(&s_cloud_error_event, true);                     // [Logik] Fehleranzeige + Audio-Cue auslösen
    }
}

// esp_timer-Callback: meldet den Ablauf des Kitchen-Timers als
// atomares Flag an den LVGL-Task.
static void on_timer_done(void)
{
    // esp_timer-Task; Done-/Alarm-Sound spielt timer.c selbst.
    atomic_store(&s_timer_done_event, true);           // [Logik] löst die "Timer done!"-Bubble im LVGL-Task aus
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
    if (cloud_is_speaking() && (id != CMD_STOP || prob < 0.30f)) {   // [C] '&&'/'||' Verknüpfung, Vergleichsoperatoren | [Logik] während TTS nur echte "stop"-Kommandos mit hoher Konfidenz akzeptieren
        ESP_LOGW(TAG, "ignoring \"%s\" (prob %.2f) during TTS playback",
                 phrase, prob);                                      // [C] %.2f Format-Spezifizierer für Fließkommazahl mit 2 Nachkommastellen
        return;                                                      // [Logik] Command während der Wiedergabe ignorieren (vermutlich False-Positive)
    }

    recorder_abort();                                                // [Logik] laufende Aufnahme abbrechen, da es sich um einen lokalen Command handelt
    // Falls das Capture schon fertig war (Stille vor dem Match):
    // Aufnahme verwerfen, sie war ein lokaler Command.
    s_pending_wav = NULL;                                             // [Logik] verhindert, dass ein bereits fertiges WAV trotzdem an die Cloud geht
    int arg = 0;                                                      // [C] lokale Variable, sammelt Zusatzinfo für die Bubble (Restzeit/Volume/Cancel-Erfolg)

    switch (id) {                                                     // [C] switch über den Enum-Typ command_id_t
    case CMD_TIMER_1MIN:
        kitchen_timer_start(60);                                     // [Logik] Timer auf 60 Sekunden stellen
        canned_audio_play(CANNED_TIMER_SET_60S);                     // [Logik] passende Sprachbestätigung abspielen
        break;
    case CMD_TIMER_5MIN:
        kitchen_timer_start(300);                                    // [Logik] Timer auf 5 Minuten stellen
        canned_audio_play(CANNED_TIMER_SET_300S);
        break;
    case CMD_TIMER_10MIN:
        kitchen_timer_start(600);                                    // [Logik] Timer auf 10 Minuten stellen
        canned_audio_play(CANNED_TIMER_SET_600S);
        break;
    case CMD_TIMER_15MIN:
        kitchen_timer_start(900);                                    // [Logik] Timer auf 15 Minuten stellen
        canned_audio_play(CANNED_TIMER_SET_900S);
        break;
    case CMD_TIMER_30MIN:
        kitchen_timer_start(1800);                                   // [Logik] Timer auf 30 Minuten stellen
        canned_audio_play(CANNED_TIMER_SET_1800S);
        break;
    case CMD_TIMER_30SEC:
        kitchen_timer_start(30);                                     // [Logik] Timer auf 30 Sekunden stellen
        canned_audio_play(CANNED_TIMER_SET_30S);
        break;
    case CMD_TIMER_5SEC:
        kitchen_timer_start(5);                                      // [Logik] Timer auf 5 Sekunden stellen
        canned_audio_play(CANNED_TIMER_SET_5S);
        break;
    case CMD_TIMER_10SEC:
        kitchen_timer_start(10);                                     // [Logik] Timer auf 10 Sekunden stellen
        canned_audio_play(CANNED_TIMER_SET_10S);
        break;
    case CMD_TIMER_15SEC:
        kitchen_timer_start(15);                                     // [Logik] Timer auf 15 Sekunden stellen
        canned_audio_play(CANNED_TIMER_SET_15S);
        break;
    case CMD_TIMER_STOP:
    case CMD_TIMER_CANCEL:
        arg = kitchen_timer_cancel();                                // [Logik] laufenden Timer abbrechen, Rückgabewert zeigt Erfolg an
        canned_audio_play(arg ? CANNED_TIMER_CANCELLED : CANNED_NO_TIMER);  // [C] Ternärer Operator wählt die passende Audio-Antwort
        break;
    case CMD_TIME_LEFT:
        arg = kitchen_timer_remaining_s();                           // [Logik] verbleibende Timer-Zeit in Sekunden ermitteln
        // Feste Phrasen koennen keine variable Restzeit ansagen — die
        // exakte Zeit zeigen Bubble + Status-Bar (Phase-4-Entscheidung).
        canned_audio_play(arg >= 0 ? CANNED_TIME_LEFT : CANNED_NO_TIMER);  // [Logik] arg >= 0 heißt: ein Timer läuft
        break;
    case CMD_STOP:
        // Bricht Canned- und TTS-Wiedergabe ab (Phase 4/7). Laeuft ein
        // Timer, gilt "stop" als "stop timer" — MultiNet feuert bei
        // "stop timer" oft schon nach "stop" (Diagnose 2026-06-11).
        // Ausnahme SPEAKING: da meint "stop" nur die Wiedergabe, der
        // Timer laeuft weiter.
        canned_audio_stop();                                          // [Logik] laufende Sprachausgabe/Sound sofort abbrechen
        if (!cloud_is_speaking() && kitchen_timer_cancel()) {         // [C] '!' Negation, '&&' Kurzschlussauswertung | [Logik] nur außerhalb der TTS-Wiedergabe auch den Timer abbrechen
            arg = 1;                                                  // [Logik] merkt, dass zusätzlich ein Timer abgebrochen wurde (für Bubble-Text)
            canned_audio_play(CANNED_TIMER_CANCELLED);
        }
        break;
    case CMD_VOLUME_UP:
        volume_adjust(+VOLUME_STEP);                                  // [C] unäres '+' rein kosmetisch, Wert bleibt positiv | [Logik] Lautstärke um einen Schritt erhöhen
        arg = volume_get();                                           // [Logik] neuen Lautstärkewert für die Bubble merken
        canned_audio_play(CANNED_VOLUME_CHANGED);
        break;
    case CMD_VOLUME_DOWN:
        volume_adjust(-VOLUME_STEP);                                  // [C] unäres '-' negiert den Schrittwert | [Logik] Lautstärke um einen Schritt verringern
        arg = volume_get();
        canned_audio_play(CANNED_VOLUME_CHANGED);
        break;
    case CMD_VOLUME_20:
    case CMD_VOLUME_40:
    case CMD_VOLUME_60:
    case CMD_VOLUME_80:
    case CMD_VOLUME_100:
        // Phase 8b: direkte Stufen, IDs liegen fortlaufend ab 20 %.
        volume_set((id - CMD_VOLUME_20 + 1) * VOLUME_STEP);           // [C] Enum-Arithmetik: Differenz zweier Enum-Werte als Index | [Logik] direkte Prozent-Stufe aus der Command-ID errechnen
        arg = volume_get();
        canned_audio_play(CANNED_VOLUME_CHANGED);
        break;
    case CMD_MUTE:
        volume_mute();                                                 // [Logik] Lautstärke auf 0 setzen, vorherigen Wert merken
        canned_audio_play(CANNED_MUTED);  // bei Gain 0 bewusst stumm
        break;
    case CMD_UNMUTE:
        volume_unmute();                                               // [Logik] vorherige Lautstärke wiederherstellen
        arg = volume_get();
        canned_audio_play(CANNED_UNMUTED);
        break;
    case CMD_TEMPERATURE:
        arg = temperature_valid()                                     // [C] Ternärer Operator über mehrere Zeilen
                  ? (int)(temperature_get() + 0.5f)                    // [C] Cast auf int rundet ab; +0.5f davor rundet stattdessen kaufmännisch
                  : INT_MIN;                                           // [Logik] Sentinel-Wert für "kein gültiger Messwert"
        if (arg != INT_MIN && cloud_tts_enabled()) {                  // [Logik] nur bei gültigem Wert und aktivem TTS laut ansagen
            char say[64];                                             // [C] lokaler Stack-Puffer für den Ansage-Text
            snprintf(say, sizeof(say), "The temperature is %d degrees", arg);
            cloud_speak(say);                                          // [Logik] Temperatur per TTS vorlesen lassen
        }
        break;
    case CMD_CHARACTER_MYCROFT:
    case CMD_CHARACTER_ALEXA:
    case CMD_CHARACTER_JARVIS:
        // Charakter-Wechsel (Phase B2): den eigentlichen Swap macht der
        // LVGL-Task (apply_character) — hier nur den Ziel-Index melden.
        // IDs liegen fortlaufend, Index = id - CMD_CHARACTER_MYCROFT.
        // Kein s_cmd_event: die Bubble kommt aus apply_character.
        atomic_store(&s_character_event, (int)id - CMD_CHARACTER_MYCROFT);  // [C] Enum-Differenz als Index, Cast auf int | [Logik] Ziel-Charakter-Index für den LVGL-Task hinterlegen
        return;                                                       // [Logik] kein generisches Command-Event, apply_character übernimmt die Bubble
    case CMD_CHARACTER_NEXT:
        // Namensunabhaengig zum naechsten Charakter (LVGL kennt s_character).
        atomic_store(&s_cycle_event, true);                            // [Logik] signalisiert dem LVGL-Task "wechsle zum nächsten Charakter"
        return;
    }

    atomic_store(&s_cmd_arg, arg);                                     // [Logik] Zusatzwert VOR der Command-ID veröffentlichen (Reihenfolge wichtig für den Leser)
    atomic_store(&s_cmd_event, (int)id);                               // [C] Cast des Enum-Werts auf int für den atomaren Typ | [Logik] Command-Event für den LVGL-Task auslösen
}

// Bubble-Text fuer ein Command-Event (laeuft im LVGL-Task).
// Übersetzt Command-ID + Zusatzwert in einen für den Chat lesbaren
// Kurztext.
static void format_command_bubble(command_id_t id, int arg, char *text,
                                  size_t len)
{
    switch (id) {                                                                              // [C] switch über den Enum-Typ, ein case pro Command-ID
    case CMD_TIMER_1MIN:   snprintf(text, len, "Timer set: 1 min"); break;                      // [C] snprintf mit übergebener Puffergröße len (kein fester sizeof möglich, da text ein Parameter ist)
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
        snprintf(text, len, arg ? "Timer cancelled" : "No timer running");   // [C] Ternärer Operator wählt Text nach Erfolg des Cancel
        break;
    case CMD_TIME_LEFT:
        if (arg >= 0) {                                                     // [Logik] arg >= 0 heißt: ein Timer läuft noch
            snprintf(text, len, "Time left: %d:%02d", arg / 60, arg % 60);  // [C] Ganzzahldivision/Modulo trennen Minuten und Sekunden | [C] %02d füllt mit führender Null auf 2 Stellen
        } else {
            snprintf(text, len, "No timer running");
        }
        break;
    case CMD_STOP:
        snprintf(text, len, arg ? "Timer cancelled" : "Stopped");           // [C] Ternärer Operator: arg zeigt an, ob zusätzlich ein Timer gestoppt wurde
        break;
    case CMD_VOLUME_UP:
    case CMD_VOLUME_DOWN:
    case CMD_VOLUME_20:
    case CMD_VOLUME_40:
    case CMD_VOLUME_60:
    case CMD_VOLUME_80:
    case CMD_VOLUME_100:
    case CMD_UNMUTE:      snprintf(text, len, "Volume: %d%%", arg); break;  // [C] "%%" ist das escapte Prozentzeichen im Format-String
    case CMD_MUTE:        snprintf(text, len, "Muted"); break;
    case CMD_TEMPERATURE:
        if (arg != INT_MIN) {                                              // [Logik] gültiger Messwert (kein Sentinel)?
            snprintf(text, len, "Temperature: %d" "\xc2\xb0" "C", arg);    // [C] String-Literal-Konkatenation, "\xc2\xb0" = UTF-8-Bytes für das Grad-Zeichen °
        } else {
            snprintf(text, len, "Sensor not ready");
        }
        break;
    default:              snprintf(text, len, "Command %d", (int)id); break;  // [C] default-Zweig fängt nicht explizit behandelte IDs ab, Cast auf int fürs Format
    }
}

// LVGL-Timer-Callback: setzt den Mic-Button nach einer kurzen
// "Match"-Anzeige (grün) wieder auf Idle zurück. Wird einmalig 800 ms
// nach einem lokalen Command bzw. Charakter-Wechsel gestartet.
static void mic_button_reset_cb(lv_timer_t *timer)
{
    ui_mic_button_set_state(UI_MIC_IDLE);                              // [HW] Mic-Button-Optik zurück auf den Idle-Zustand
}

// Status-Text mittig in der Bar: Listening > Processing >
// Timer-Countdown > Idle (Spec Section 4: "Timer: 04:23" ersetzt den
// State-Text).
// Bestimmt anhand der UI-Zustandsflags und einer eventuellen
// Timer-Restzeit den anzuzeigenden Status-Text und schreibt ihn nur
// bei tatsächlicher Änderung in die UI (vermeidet unnötige Redraws).
static void update_state_text(void)
{
    char text[24];                                                     // [C] lokaler Stack-Puffer, muss zu s_state_text passen
    int left = kitchen_timer_remaining_s();                            // [Logik] verbleibende Timer-Zeit abfragen (-1 wenn kein Timer läuft)

    if (s_listening) {                                                 // [Logik] Priorität 1: gerade wird zugehört
        snprintf(text, sizeof(text), "Listening...");
    } else if (s_processing) {                                         // [Logik] Priorität 2: Cloud-Anfrage läuft
        snprintf(text, sizeof(text), "Processing...");
    } else if (s_speaking) {                                           // [Logik] Priorität 3: TTS spielt gerade ab
        snprintf(text, sizeof(text), "Speaking...");
    } else if (left >= 0) {                                            // [Logik] Priorität 4: ein Timer läuft, Countdown anzeigen
        snprintf(text, sizeof(text), "Timer: %02d:%02d", left / 60,
                 left % 60);                                            // [C] Ganzzahldivision/Modulo für Minuten/Sekunden, %02d mit führender Null
    } else {                                                            // [Logik] Fallback: Idle-Text des aktiven Charakters
        snprintf(text, sizeof(text), "%s",
                 s_characters[s_character].idle_text);                 // [C] Array-Indexierung + Feldzugriff per '.'
    }

    if (strcmp(text, s_state_text) != 0) {                             // [C] strcmp != 0 bedeutet: Texte unterscheiden sich | [Logik] nur bei Änderung die UI aktualisieren
        strcpy(s_state_text, text);                                    // [C] strcpy kopiert bis zum Nullterminator (Ziel-Puffer gleich groß dimensioniert)
        ui_status_bar_set_state_text(text);                            // [HW] neuen Status-Text in der Status-Bar anzeigen
    }
}

// Volume-Anzeige (Label + Slider-Position) aus dem aktuellen Gain
// nachziehen — deckt alle Pfade ab: lokaler Command, Touch-Slider und
// seit Phase 10 die Cloud-Tool-Calls (volume_set im network_task). Wie
// update_state_text() nur bei Aenderung schreiben (kein Tick-Redraw).
static void update_volume_display(void)
{
    int v = volume_get();                                               // [Logik] aktuellen Lautstärke-Gain abfragen
    if (v != s_volume_shown) {                                          // [C] Ungleichheitsvergleich | [Logik] nur bei Änderung die UI aktualisieren
        s_volume_shown = v;                                             // [Logik] neuen Wert als "zuletzt angezeigt" merken
        ui_status_bar_set_volume(v);                                    // [HW] Lautstärke-Label/Slider in der Status-Bar aktualisieren
    }
}

// LVGL-Timer: holt Events aus den anderen Task-Kontexten ab.
// Zentrale Übersetzungsstelle: liest alle atomaren Flags/Textbuffer,
// die von anderen Tasks gesetzt wurden, und übersetzt sie in
// LVGL-Widget-Updates. Läuft alle 50 ms im lvgl_task.
static void app_event_timer_cb(lv_timer_t *timer)
{
    int wifi = atomic_exchange(&s_wifi_event, -1);                     // [C] atomic_exchange liest den alten Wert und setzt gleichzeitig den neuen (hier: -1 = "abgeholt") | [Logik] Einmal-Konsum des WiFi-Events
    if (wifi >= 0) {                                                   // [Logik] -1 bedeutet: kein neuer Status seit dem letzten Abholen
        ui_status_bar_set_wifi_connected(wifi == 1);                   // [HW] WiFi-Icon je nach Verbindungsstatus umschalten
    }

    if (atomic_exchange(&s_wake_event, false)) {                       // [Logik] Wake Word seit letztem Tick erkannt?
        s_listening = true;                                            // [Logik] UI-Zustand auf "hört zu" setzen
        ui_mic_button_set_state(UI_MIC_LISTENING);                     // [HW] Mic-Button visuell auf Listening umschalten
    }

    // Cloud-Pipeline gestartet: Mic-Button orange, "Processing...".
    // (Die "Captured ..."-Bubble aus Phase 3/4 ist damit ersetzt.)
    if (atomic_exchange(&s_processing_event, false)) {
        s_listening = false;                                           // [Logik] Listening-Phase beendet
        s_processing = true;                                           // [Logik] Cloud-Verarbeitung läuft jetzt
        ui_mic_button_set_state(UI_MIC_PROCESSING);                    // [HW] Mic-Button visuell auf Processing umschalten
    }

    // Stille-Guard: Aufnahme verworfen — kurzer Hinweis + Cue, zurueck
    // zu Idle (Phase 8).
    if (atomic_exchange(&s_too_quiet_event, false)) {
        ui_chat_add_local_bubble("Didn't catch that");                 // [Logik] Nutzer-Feedback: Aufnahme war zu leise
        canned_audio_play(CANNED_ERROR);                                // [Logik] akustischer Hinweis auf verworfene Aufnahme
        s_listening = false;
        ui_mic_button_set_state(UI_MIC_IDLE);                           // [HW] Mic-Button zurück auf Idle
    }

    if (atomic_exchange(&s_transcript_event, false)) {
        ui_chat_add_user_bubble(s_transcript);                         // [Logik] Whisper-Transkript als Nutzer-Bubble im Chat anzeigen
        // PROCESSING bleibt stehen — Gemini laeuft noch (Phase 6).
    }

    if (atomic_exchange(&s_reply_event, false)) {
        ui_chat_add_assistant_bubble(s_reply);                         // [Logik] LLM-Antwort als Assistant-Bubble im Chat anzeigen
        // PROCESSING bleibt stehen — der TTS-Request laeuft noch;
        // SPEAKING/DONE/ERROR loesen ab (Phase 7).
    }

    if (atomic_exchange(&s_speaking_event, false)) {
        s_processing = false;                                          // [Logik] Verarbeitungsphase endet, Wiedergabe beginnt
        s_speaking = true;
        ui_mic_button_set_state(UI_MIC_SPEAKING);                      // [HW] Mic-Button visuell auf Speaking umschalten
    }

    if (atomic_exchange(&s_cloud_done_event, false)) {
        s_processing = false;                                          // [Logik] kompletter Turn beendet, zurück in den Ruhezustand
        s_speaking = false;
        ui_mic_button_set_state(UI_MIC_IDLE);                           // [HW] Mic-Button zurück auf Idle
    }

    if (atomic_exchange(&s_cloud_error_event, false)) {
        char text[64];                                                 // [C] lokaler Stack-Puffer für die Fehler-Bubble
        snprintf(text, sizeof(text), "Cloud: %s", s_cloud_error);      // [Logik] Fehlermeldung für die Anzeige aufbereiten
        ui_chat_add_local_bubble(text);                                // [Logik] Fehler als Bubble im Chat sichtbar machen
        // Error-Recovery (Phase 8): kurzer Audio-Cue, zurueck zu Idle.
        canned_audio_play(CANNED_ERROR);                               // [Logik] akustischer Fehler-Hinweis
        s_processing = false;
        s_speaking = false;
        ui_mic_button_set_state(UI_MIC_IDLE);                           // [HW] Mic-Button zurück auf Idle
    }

    int cmd = atomic_exchange(&s_cmd_event, 0);                        // [Logik] 0 = kein neuer lokaler Command seit letztem Tick
    if (cmd != 0) {
        char text[32];                                                 // [C] lokaler Stack-Puffer für den Command-Bubble-Text
        format_command_bubble((command_id_t)cmd, atomic_load(&s_cmd_arg),
                              text, sizeof(text));                     // [C] Cast des int zurück auf command_id_t | [Logik] Zusatzwert erst nach der ID lesen (war vom Writer vorher gesetzt)
        ui_chat_add_local_bubble(text);                                // [Logik] Command-Ergebnis als Bubble anzeigen
        s_listening = false;
        // Gruen als Match-Feedback, nach 800 ms zurueck auf Idle
        // (Animationen kommen in Phase 8).
        ui_mic_button_set_state(UI_MIC_LOCAL_RESPONSE);                // [HW] Mic-Button kurz grün als Erfolgsfeedback
        lv_timer_set_repeat_count(
            lv_timer_create(mic_button_reset_cb, 800, NULL), 1);       // [HW] Einmal-Timer: nach 800 ms zurück auf Idle | [C] repeat_count=1 verhindert Wiederholung
    }

    if (atomic_exchange(&s_timer_done_event, false)) {
        ui_chat_add_local_bubble("Timer done!");                       // [Logik] Hinweis-Bubble, dass der Kitchen-Timer abgelaufen ist
    }

    // Charakter-Wechsel per Voice (Phase B2): im LVGL-Task anwenden.
    // Gleiches UI-Feedback wie ein lokaler Command (s_cmd_event oben):
    // Listening beenden, kurz gruen, dann zurueck auf Idle — sonst bliebe
    // die Bar auf "Listening..." haengen (der Recorder ist da laengst
    // per recorder_abort() gestoppt, nur der UI-State fehlte).
    int chr = atomic_exchange(&s_character_event, -1);                 // [Logik] -1 = kein per Voice angeforderter Charakter-Wechsel
    bool cycle = atomic_exchange(&s_cycle_event, false);                // [Logik] namensunabhängiger Cycle-Wechsel angefordert?
    if (chr >= 0 || cycle) {                                           // [C] '||' logisches ODER: einer der beiden Wege reicht
        apply_character(cycle ? (s_character + 1) % NUM_CHARACTERS : chr);  // [C] Ternärer Operator wählt Ziel-Index | [Logik] Cycle nimmt den nächsten, sonst den explizit genannten Charakter
        s_listening = false;
        ui_mic_button_set_state(UI_MIC_LOCAL_RESPONSE);                // [HW] gleiches Erfolgsfeedback wie bei lokalen Commands
        lv_timer_set_repeat_count(
            lv_timer_create(mic_button_reset_cb, 800, NULL), 1);
    }

    update_state_text();                                               // [Logik] Status-Text-Priorität (Listening>Processing>Timer>Idle) neu auswerten
    update_volume_display();                                           // [Logik] Lautstärke-Anzeige mit aktuellem Gain abgleichen
}

// Haupteinstiegspunkt der Firmware: initialisiert NVS, Konfiguration,
// Charakter-Zustand, LVGL/Display/Touch/UI, WiFi, die Cloud-Pipeline,
// lokale Audio-Ausgabe/Commands sowie die komplette Speech-Pipeline
// (Wake Word, Recorder, MultiNet, AFE) und startet die Haupttasks.
// Läuft einmalig beim Boot im Haupt-Task von ESP-IDF.
void app_main(void)
{
    ESP_LOGI(TAG, "MiniLingo Phase 8 starting");                       // [Logik] Boot-Log als Startmarker

    esp_err_t err = nvs_flash_init();                                  // [HW] initialisiert die NVS-Flash-Partition
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {                        // [C] '||' logisches ODER über zwei Fehlercodes | [Logik] NVS-Partition ist inkompatibel/voll -> muss gelöscht werden
        ESP_ERROR_CHECK(nvs_flash_erase());                            // [C] Makro: prüft Rückgabewert, bricht bei Fehler mit Abort ab | [HW] löscht die gesamte NVS-Partition
        err = nvs_flash_init();                                        // [HW] Init nach dem Löschen erneut versuchen
    }
    ESP_ERROR_CHECK(err);                                               // [Logik] jeder verbleibende Fehler ist fatal (Boot ohne NVS nicht sinnvoll)

    // Laufzeit-Config aus NVS laden (SSID/Keys/Modelle); fällt auf die
    // Kconfig-Defaults zurück, wenn nichts provisioniert ist.
    ESP_ERROR_CHECK(config_init());                                    // [Logik] lädt alle Konfigurationswerte in den Cache

    // Aktiven Charakter aus dem NVS wiederherstellen (Phase B). Persona
    // hier setzen (llm = network_task, Pointer-Tausch); der Wakenet-
    // Filter wird nach afe_pipeline_init scharfgeschaltet.
    s_character = config_character_index();                            // [Logik] zuletzt gewählten Charakter-Index aus NVS übernehmen
    if (s_character < 0 || s_character >= (int)NUM_CHARACTERS) {       // [C] Bereichsprüfung mit '||' | [Logik] Schutz gegen ungültigen/veralteten NVS-Wert
        s_character = 0;                                               // [Logik] Fallback auf den ersten Charakter (Mycroft)
    }
    llm_set_system_prompt(s_characters[s_character].persona);          // [Logik] LLM-Persona für den wiederhergestellten Charakter setzen
    tts_set_voice(s_characters[s_character].voice);                    // [Logik] Cloud-TTS-Stimme entsprechend setzen
    tts_set_local_voice(s_characters[s_character].local_voice);        // [Logik] lokale Piper-Stimme entsprechend setzen
    // -1 = kein Charakter-Wechsel angefordert (0 waere Mycroft).
    atomic_store(&s_character_event, -1);                               // [Logik] initialisiert das Event-Flag auf "kein Wechsel ausstehend"

    lv_init();                                                          // [HW] initialisiert die LVGL-Grafikbibliothek
    lv_tick_set_cb(lvgl_tick_get_cb);                                  // [HW] registriert die Zeitquelle für LVGL-Animationen/Timer

    ESP_ERROR_CHECK(display_init());                                  // [HW] Display-Controller initialisieren
    ESP_ERROR_CHECK(touch_init());                                    // [HW] Touch-Controller initialisieren

    ui_init();                                                          // [HW] LVGL-Widgets (Mic-Button, Status-Bar, Chat) aufbauen
    ui_mic_button_set_click_cb(on_mic_button_clicked);                // [Logik] Klick-Handler für den Mic-Button registrieren
    ui_status_bar_set_volume_changed_cb(on_volume_slider_changed);    // [Logik] Callback für den Lautstärke-Slider registrieren
    ui_status_bar_set_wifi_clicked_cb(on_wifi_clicked);               // [Logik] Callback für Klick aufs WiFi-Icon registrieren
    ui_status_bar_set_state_clicked_cb(on_character_clicked);         // [Logik] Callback für Klick auf den Status-Text (Charakter-Wechsel) registrieren
    ui_status_bar_set_state_text(s_characters[s_character].idle_text); // [Logik] initialen Idle-Text des aktiven Charakters anzeigen
    ui_status_bar_set_wifi_connected(false);                          // [Logik] Anfangszustand: WiFi noch nicht verbunden
    // TTS-Toggle: Startzustand aus demselben Kconfig wie cloud.c (vor
    // dem lvgl_task setzen — danach waeren UI-Calls nicht thread-safe).
    ui_tts_button_set_click_cb(on_tts_button_clicked);                // [Logik] Klick-Handler für den TTS-Toggle registrieren
#ifdef CONFIG_TTS_SPEAK_REPLIES                                       // [C] Präprozessor-Bedingung: nur wenn dieses Kconfig-Feature aktiviert ist
    ui_tts_button_set_enabled(true);                                  // [Logik] TTS-Button-Anzeige startet aktiv
#else
    ui_tts_button_set_enabled(false);                                 // [Logik] TTS-Button-Anzeige startet inaktiv
#endif
    lv_timer_create(app_event_timer_cb, 50, NULL);                    // [HW] registriert den 50-ms-Polling-Timer für Cross-Task-Events

    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 8192, NULL, 4, NULL, 0);  // [HW] startet den LVGL-Task fest auf Core 0 mit 8 KB Stack und Priorität 4

    ESP_ERROR_CHECK(wifi_init(on_wifi_status));                       // [HW] WiFi-STA-Verbindung starten, Statusänderungen über Callback

    // Erststart-Komfort: ist keine SSID provisioniert, direkt das
    // Setup-Portal öffnen (Tap aufs WiFi-Icon tut dasselbe).
    if (config_wifi_unprovisioned()) {                                // [Logik] prüft, ob noch der Kconfig-Platzhalter als SSID aktiv ist
        ESP_LOGW(TAG, "no WiFi provisioned — starting setup portal");
        provisioning_start();                                         // [HW] startet automatisch das Setup-Portal
    }

    // Cloud-Pipeline (Phase 5/6): network_task vor der AFE-Pipeline
    // starten — der afe_fetch_task ruft cloud_submit/cloud_is_busy.
    ESP_ERROR_CHECK(cloud_init(on_cloud_event));                      // [Logik] startet den network_task, der Whisper/LLM/TTS orchestriert

    // Audio-Ausgabe + lokale Command-Infrastruktur (Phase 4)
    ESP_ERROR_CHECK(audio_out_init());                                // [HW] I2S-TX-Kanal für den Lautsprecher initialisieren
    volume_init(INITIAL_VOLUME);                                      // [Logik] Lautstärke-Verwaltung mit Startwert initialisieren
    ui_status_bar_set_volume(volume_get());                           // [HW] initiale Lautstärke in der Status-Bar anzeigen
    ESP_ERROR_CHECK(canned_audio_init());                             // [Logik] vorbereitete Audio-Antworten aus SPIFFS laden
    kitchen_timer_init(on_timer_done);                                // [Logik] Kitchen-Timer-Modul mit Ablauf-Callback initialisieren
    // DS18B20: Fehler nicht fatal — Sensor koennte nicht angeschlossen sein.
    if (temperature_init() != ESP_OK) {                               // [HW] versucht, den DS18B20-Temperatursensor zu initialisieren
        ESP_LOGW(TAG, "temperature sensor not found");                // [Logik] nur eine Warnung, kein Abbruch (Sensor optional)
    }

    // Speech-Pipeline: Models einmal laden, dann MultiNet (commands)
    // VOR dem Pipeline-Start initialisieren — der fetch-Task fuettert
    // die Commands ab dem ersten Wake Word.
    wav_dump_init();                                                   // [Logik] optionales WAV-Debug-Mitschneiden initialisieren
    wake_word_set_listener(on_wake_word);                             // [Logik] Callback für erkannte Wake Words registrieren
    ESP_ERROR_CHECK(recorder_init(on_recording_done));                // [Logik] Aufnahme-Puffer-Verwaltung mit Fertig-Callback initialisieren
    ESP_ERROR_CHECK(audio_in_init());                                 // [HW] I2S-RX-Kanal für das Mikrofon initialisieren
    srmodel_list_t *models = esp_srmodel_init("model");               // [HW] lädt die verfügbaren Speech-Recognition-Modelle (WakeNet/MultiNet) aus der Partition "model"
    ESP_ERROR_CHECK(commands_init(models, on_local_command,
                                  on_command_timeout));                // [Logik] MultiNet-Commands mit Match- und Timeout-Callback initialisieren
    // Nur das Wakeword des aktiven Charakters laden (RAM, s. afe_pipeline).
    ESP_ERROR_CHECK(afe_pipeline_init(models, s_character));          // [HW] AFE-Pipeline (Wake-Word/VAD/Recorder-Orchestrierung) mit aktivem Charakter starten

    ESP_LOGI(TAG, "Phase 8 up: local commands + cloud STT + LLM"       // [C] mehrzeiliges String-Literal wird vom Compiler verkettet
#if CONFIG_TTS_SPEAK_REPLIES                                          // [C] Präprozessor-Bedingung erweitert den Log-Text nur bei aktiviertem TTS-Feature
                  " + TTS"
#endif
    );
}
