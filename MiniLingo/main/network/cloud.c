// network_task (Spec Section 8): fuehrt die Cloud-Requests sequenziell
// aus — nie zwei TLS-Verbindungen parallel (Heap, Spec Section 13).
// Phase 5: Whisper-Transkription; Phase 6: Gemini-Antwort; Phase 7:
// Gemini-TTS + Wiedergabe (der Task blockiert bis zum Wiedergabe-Ende,
// damit busy/speaking den SPEAKING-State der Spec abbilden).
#include <stdatomic.h>              // [C] Header-Include | liefert atomic_bool/atomic_load/atomic_store für threadsichere Flags

#include "freertos/FreeRTOS.h"      // [C] Header-Include | [HW] FreeRTOS-Basistypen/Makros (u.a. portMAX_DELAY)
#include "freertos/queue.h"         // [C] Header-Include | [HW] FreeRTOS-Queue-API (QueueHandle_t, xQueueSend/Receive)
#include "freertos/task.h"          // [C] Header-Include | [HW] FreeRTOS-Task-API (xTaskCreatePinnedToCore)

#include "cJSON.h"                  // [C] Header-Include | [Logik] JSON-Parser für die Tool-Call-Argumente des LLM
#include "esp_log.h"                // [C] Header-Include | [HW] ESP-IDF-Logging-Makros (ESP_LOGW etc.)
#include "sdkconfig.h"              // [C] Header-Include | [HW] generierte Kconfig-Defines (u.a. CONFIG_TTS_SPEAK_REPLIES)

#include "local/canned_audio.h"     // [C] Header-Include | [HW] Player-Task für PCM-Wiedergabe (canned_audio_play_pcm)
#include "local/timer.h"            // [C] Header-Include | [Logik] Kuechen-Timer-Funktionen fuer Tool-Calls
#include "local/volume.h"           // [C] Header-Include | [Logik] Lautstaerke-Steuerung fuer Tool-Calls
#include "network/wifi.h"           // [C] Header-Include | [HW] WiFi-Statusabfrage (wifi_is_connected)
#include "state.h"                  // [C] Header-Include | [Logik] Chat-Historie (chat_history_push/init)
#include "whisper.h"                // [C] Header-Include | [HW] Whisper-STT-Cloud-Anfrage (whisper_transcribe)
#include "llm.h"                    // [C] Header-Include | [HW] Gemini-LLM-Cloud-Anfrage (llm_generate) + Result-Typ
#include "tts.h"                    // [C] Header-Include | [HW] Gemini-TTS-Cloud-Anfrage (tts_synthesize/tts_init)
#include "cloud.h"                  // [C] Header-Include | eigene Modul-Schnittstelle (Public API dieser Datei)

static const char *TAG = "cloud";   // [C] static = Datei-lokale Linkage | [Logik] Log-Tag zur Zuordnung der Ausgaben

// Transkript-Maximum: 8 s Sprache sind << 512 Zeichen.
#define TRANSCRIPT_MAX 512          // [C] Präprozessor-Konstante (Textersetzung) | [Logik] max. Puffergröße für Whisper-Text
// Gemini-Antwort: maxOutputTokens=200 bleibt unter 1 KB Text.
#define REPLY_MAX 1024              // [C] Präprozessor-Konstante | [Logik] max. Puffergröße für LLM-Antwort/Bestätigungstext

// speak_only = true: kein Whisper/LLM, nur speak_reply(text).
// text wird bei cloud_speak() per Wert reinkopiert.
#define SPEAK_TEXT_MAX 80           // [C] Präprozessor-Konstante | [Logik] max. Länge für direkt vorzulesenden Text

// Ein Job in der Cloud-Queue: entweder ein WAV-Buffer für die volle
// Whisper->LLM->TTS-Pipeline oder ein reiner Text für direktes TTS
// (speak_only). Wird per Wert über die FreeRTOS-Queue verschickt.
typedef struct {                    // [C] typedef struct = benannter Struct-Typ ohne extra Tag-Namen
    const uint8_t *wav;             // [C] Zeiger auf den WAV-Byte-Puffer (bleibt extern gültig, Recording-Buffer im PSRAM)
    size_t bytes;                   // [C] Länge des WAV-Puffers in Byte
    bool speak_only;                // [C] Unterscheidet Job-Art: true = nur TTS von text, false = volle Pipeline über wav
    char text[SPEAK_TEXT_MAX];      // [C] Fest dimensioniertes char-Array, wird per Wert in die Queue kopiert
} cloud_job_t;                      // [C] Bezeichner des Struct-Typs

static QueueHandle_t s_queue;                 // [C] static = Datei-lokal | [HW] FreeRTOS-Queue-Handle für cloud_job_t (Tiefe 1)
static cloud_event_cb_t s_cb;                  // [C] static | [Logik] gespeicherter Callback aus cloud_init() für alle Ereignisse
// busy von submit (afe_fetch_task) bis nach dem Ergebnis-Callback
// (network_task); gelesen auch vom afe_fetch_task (Wake-Word-Gate).
static atomic_bool s_busy;                     // [C] atomic_bool = lock-freies boolesches Flag für Task-übergreifenden Zugriff
// speaking = TTS-Wiedergabe laeuft (Teilintervall von busy); gelesen
// vom afe_fetch_task (MultiNet-"stop"-Session) und von main.c.
static atomic_bool s_speaking;                 // [C] atomic_bool | [Logik] Teilzustand von busy: TTS-Wiedergabe aktiv
// Laufzeit-Schalter (Phase 10b): TTS-Vorlesen an/aus ueber den Button.
// CONFIG_TTS_SPEAK_REPLIES gibt nur noch den Startwert vor.
static atomic_bool s_tts_enabled;              // [C] atomic_bool | [Logik] Laufzeit-Schalter für automatisches Vorlesen
// PSRAM-Buffer erst beim ersten Vorlesen allokieren (lazy); nur im
// network_task gelesen/geschrieben, daher kein atomic noetig.
static bool s_tts_inited;                      // [C] gewöhnliches bool, da nur ein Task (network_task) darauf zugreift

// Antwort vorlesen (Phase 7): TTS-Request (Retry/Backoff macht tts.c),
// dann blockierende Wiedergabe ueber den Player-Task — "stop"
// (MultiNet) bricht sie per canned_audio_stop() ab. Ein TTS-Fehler ist
// nicht fatal: die Antwort steht bereits als Bubble auf dem Display.
static void speak_reply(const char *reply)     // [C] static = interne Sichtbarkeit | Parameter const char* = zu sprechender Text
{
    if (!s_tts_inited) {                                       // [C] Negation, Bedingung | [Logik] TTS-Modul noch nicht initialisiert?
        // Lazy: 2,5 MB PSRAM erst jetzt holen (Toggle war an).
        if (tts_init() != ESP_OK) {                             // [HW] TTS-Init allokiert PSRAM-Puffer für Synthese
            s_cb(CLOUD_EVT_ERROR, "TTS init failed");           // [C] Callback über Funktionszeiger aufgerufen | [Logik] Fehler nach außen melden
            return;                                             // [Logik] Abbruch, keine Wiedergabe möglich
        }
        s_tts_inited = true;                                    // [Logik] Init erfolgreich merken, kein erneuter tts_init() nötig
    }

    const int16_t *pcm;                                          // [C] Zeiger auf 16-Bit-PCM-Samples (wird von tts_synthesize gefüllt)
    size_t samples;                                              // [C] Anzahl der PCM-Samples
    uint32_t rate;                                                // [C] Abtastrate der synthetisierten Sprache

    esp_err_t err = tts_synthesize(reply, &pcm, &samples, &rate); // [HW] Gemini-TTS-Cloud-Request, Out-Parameter per Adress-Operator &
    if (err != ESP_OK) {                                         // [Logik] Synthese fehlgeschlagen?
        // ESP_ERR_NOT_ALLOWED = HTTP 429: das Free-Tier-Tageslimit
        // (10 TTS-Requests) ist erschoepft.
        s_cb(CLOUD_EVT_ERROR, err == ESP_ERR_NOT_ALLOWED         // [HW] HTTP-429-Mapping auf ESP_ERR_NOT_ALLOWED | [C] ternärer Operator wählt Meldungstext
                                  ? "TTS daily limit"
                                  : "Speech failed");
        return;                                                  // [Logik] Abbruch, keine Wiedergabe
    }

    atomic_store(&s_speaking, true);                              // [C] atomarer Schreibzugriff | [Logik] Speaking-Zustand setzen, bevor Wiedergabe startet
    s_cb(CLOUD_EVT_SPEAKING, "");                                 // [Logik] UI über Beginn der Sprachausgabe informieren
    canned_audio_play_pcm(pcm, samples, rate);                    // [HW] blockierende PCM-Wiedergabe über den Player-Task
    atomic_store(&s_speaking, false);                             // [C] atomarer Schreibzugriff | [Logik] Speaking-Zustand nach Wiedergabe-Ende zurücksetzen
}

// Liest ein Integer-Argument aus dem cJSON-Objekt args; liefert fallback,
// falls der Schlüssel fehlt oder kein Zahlenwert ist. Hilfsfunktion für
// das Parsen der LLM-Tool-Call-Argumente in run_tool().
static int arg_int(const cJSON *args, const char *key, int fallback)  // [C] static Hilfsfunktion, Rückgabetyp int
{
    const cJSON *item = cJSON_GetObjectItem(args, key);                // [Logik] Feld per Schlüsselname im JSON-Objekt suchen
    return cJSON_IsNumber(item) ? item->valueint : fallback;            // [C] ternärer Operator, Struct-Zugriff per -> auf Zeiger | [Logik] Typprüfung vor Zugriff
}

// Tool-Call ausfuehren (Phase 10) und eine deterministische Bestaetigung
// formulieren — kein zweiter LLM-Call. Laeuft im network_task; die
// Aktionen sind aus jedem Task sicher: kitchen_timer_* nutzt esp_timer +
// atomics, volume_set ist atomar. Wortlaut wie format_command_bubble in
// main.c, damit Cloud- und lokale Aktionen gleich aussehen.
static void run_tool(const llm_result_t *r, char *confirm, size_t n)    // [C] static | Parameter: const-Zeiger auf Ergebnis, Ausgabepuffer + Größe
{
    cJSON *args = cJSON_Parse(r->tool_args);  // NULL bei "{}"/Fehler: ok  // [Logik] Tool-Argumente als JSON parsen; NULL ist unkritisch

    if (strcmp(r->tool_name, "set_timer") == 0) {                       // [C] strcmp vergleicht C-Strings, 0 = gleich | [Logik] Tool-Name auswerten
        int seconds = arg_int(args, "seconds", 0);                      // [Logik] Sekunden-Argument mit Default 0 lesen
        if (seconds < 1) {                                              // [Logik] Untergrenze absichern
            seconds = 1;
        } else if (seconds > 86400) {                                   // [Logik] Obergrenze absichern (24h in Sekunden)
            seconds = 86400;
        }
        kitchen_timer_start(seconds);                                   // [Logik] Timer mit validierter Dauer starten
        snprintf(confirm, n, "Timer set: %d:%02d", seconds / 60,        // [C] snprintf begrenzt Schreibzugriff auf Puffergröße n | [Logik] Bestätigungstext formatieren
                 seconds % 60);
    } else if (strcmp(r->tool_name, "set_volume") == 0) {                // [Logik] Tool "Lautstärke setzen"
        volume_set(arg_int(args, "percent", volume_get()));  // clampt selbst // [Logik] Default = aktueller Wert, volume_set begrenzt intern
        snprintf(confirm, n, "Volume: %d%%", volume_get());              // [Logik] Bestätigungstext mit aktuellem Wert
    } else if (strcmp(r->tool_name, "cancel_timer") == 0) {              // [Logik] Tool "Timer abbrechen"
        snprintf(confirm, n, kitchen_timer_cancel() ? "Timer cancelled"  // [C] ternärer Operator wählt Text je nach Rückgabewert
                                                    : "No timer running");
    } else if (strcmp(r->tool_name, "get_time_left") == 0) {             // [Logik] Tool "Restzeit abfragen"
        int left = kitchen_timer_remaining_s();                          // [Logik] verbleibende Sekunden ermitteln
        if (left >= 0) {                                                 // [Logik] negativer Wert = kein laufender Timer
            snprintf(confirm, n, "Time left: %d:%02d", left / 60, left % 60); // [Logik] Restzeit formatieren
        } else {
            snprintf(confirm, n, "No timer running");                   // [Logik] kein Timer aktiv
        }
    } else {
        ESP_LOGW(TAG, "unknown tool: %s", r->tool_name);                 // [HW] Log-Ausgabe für unbekannten Tool-Namen
        snprintf(confirm, n, "Unknown action");                         // [Logik] generische Fehlermeldung als Bestätigungstext
    }

    cJSON_Delete(args);                                                  // [C] Speicher des geparsten JSON-Baums freigeben (auch bei args == NULL sicher)
}

// Der eigentliche network_task: läuft endlos in einer Queue-Schleife,
// holt Jobs (WAV-Aufnahme oder reiner Sprechtext) und fährt sequenziell
// Whisper-Transkription, Gemini-Antwort/Tool-Call und optional TTS-
// Wiedergabe ab. Setzt s_busy erst nach dem Done-Callback wieder frei.
static void network_task(void *arg)                                     // [C] Task-Funktion, Signatur von FreeRTOS vorgegeben (void*-Parameter)
{
    cloud_job_t job;                                                     // [C] lokale Struct-Instanz für den aktuell empfangenen Job
    static char text[TRANSCRIPT_MAX];                                    // [C] static = bleibt über Funktionsaufrufe hinweg erhalten, kein Stack-Verbrauch bei jedem Schleifendurchlauf
    static llm_result_t result;                                          // [C] static Ergebnis-Struct für den LLM-Aufruf
    static char confirm[REPLY_MAX];                                      // [C] static Puffer für Tool-Bestätigungstexte

    while (1) {                                                          // [C] Endlosschleife, typisch für FreeRTOS-Tasks
        xQueueReceive(s_queue, &job, portMAX_DELAY);                     // [HW] blockierendes Warten auf nächsten Job (kein Timeout, portMAX_DELAY)

        if (job.speak_only) {                                            // [Logik] Unterscheidung: reiner TTS-Job vs. volle Pipeline
            // Lokale TTS-Ansage (kein Whisper/LLM): nur sprechen, wenn
            // TTS an und WiFi vorhanden; sonst still verwerfen.
            if (!wifi_is_connected()) {                                  // [HW] WiFi-Verbindungsstatus prüfen
                ESP_LOGW(TAG, "speak_only: no WiFi, skipping TTS");       // [HW] Log-Hinweis auf fehlende Netzwerkverbindung
            } else if (atomic_load(&s_tts_enabled)) {                    // [C] atomarer Lesezugriff | [Logik] TTS-Toggle prüfen
                speak_reply(job.text);                                   // [Logik] Text direkt vorlesen
            }
            s_cb(CLOUD_EVT_DONE, "");                                    // [Logik] Pipeline-Ende melden (Rückkehr zu Idle)
        } else if (!wifi_is_connected()) {                               // [HW] volle Pipeline ohne WiFi nicht möglich
            ESP_LOGW(TAG, "no WiFi, dropping query");                    // [HW] Log-Hinweis
            s_cb(CLOUD_EVT_ERROR, "No WiFi connection");                 // [Logik] Fehler an UI melden
        } else if (whisper_transcribe(job.wav, job.bytes, text,          // [HW] Whisper-Cloud-Request mit WAV-Buffer und Ausgabepuffer
                                      sizeof(text)) != ESP_OK) {          // [C] sizeof liefert Pufferkapazität zur Laufzeit-Begrenzung
            s_cb(CLOUD_EVT_ERROR, "Transcription failed");               // [Logik] Transkriptionsfehler melden
        } else {
            s_cb(CLOUD_EVT_TRANSCRIPT, text);                            // [Logik] erfolgreiches Transkript an UI weitergeben
            // LLM direkt im Anschluss — die Whisper-Verbindung ist
            // hier schon geschlossen (TLS strikt sequenziell).
            if (llm_generate(text, &result) != ESP_OK) {                 // [HW] Gemini-LLM-Cloud-Request, Ergebnis per Adress-Operator zurückgegeben
                s_cb(CLOUD_EVT_ERROR, "Assistant failed");                // [Logik] LLM-Fehler melden
            } else if (result.type == LLM_RESULT_TOOL_CALL) {             // [Logik] Antwort ist ein Tool-Aufruf statt Freitext
                // Phase 10: Tool lokal ausfuehren, Bestaetigung als
                // Reply-Bubble. History fuehrt den Bestaetigungstext als
                // Model-Turn (kein OpenAI-tool-Roundtrip) — Follow-ups
                // ("zwei Minuten kuerzer") haben damit Kontext.
                run_tool(&result, confirm, sizeof(confirm));              // [C] Adress-Operator übergibt Zeiger auf Struct | [Logik] Tool ausführen, Bestätigungstext füllen
                chat_history_push(CHAT_ROLE_USER, text);                  // [Logik] User-Eingabe in Chat-Historie aufnehmen
                chat_history_push(CHAT_ROLE_MODEL, confirm);              // [Logik] Bestätigungstext als Model-Antwort in Historie aufnehmen
                s_cb(CLOUD_EVT_REPLY, confirm);                           // [Logik] Bestätigung als Antwort-Bubble anzeigen
                s_cb(CLOUD_EVT_DONE, "");                                 // [Logik] Pipeline-Ende melden
            } else {
                // History erst bei Erfolg fortschreiben: sie enthaelt
                // nur vollstaendige User/Model-Paare.
                chat_history_push(CHAT_ROLE_USER, text);                  // [Logik] User-Eingabe in Historie aufnehmen
                chat_history_push(CHAT_ROLE_MODEL, result.text);          // [Logik] LLM-Antwort in Historie aufnehmen
                s_cb(CLOUD_EVT_REPLY, result.text);                       // [Logik] Antworttext an UI weitergeben
                if (atomic_load(&s_tts_enabled)) {                        // [C] atomarer Lesezugriff | [Logik] nur vorlesen, wenn Toggle an
                    speak_reply(result.text);                             // [Logik] Antwort per TTS vorlesen
                }
                s_cb(CLOUD_EVT_DONE, "");                                 // [Logik] Pipeline-Ende melden
            }
        }

        // Erst nach dem Callback freigeben: solange busy, startet kein
        // neues Wake Word / Capture (Recording-Buffer-Schutz).
        atomic_store(&s_busy, false);                                    // [C] atomarer Schreibzugriff | [Logik] Busy-Gate erst nach vollständiger Verarbeitung freigeben
    }
}

// Initialisiert die Cloud-Pipeline: Chat-Historie anlegen, TTS-Toggle-
// Startwert aus Kconfig übernehmen, Job-Queue und den network_task auf
// Core 0 mit Priorität 3 anlegen. Muss einmalig beim Systemstart laufen.
esp_err_t cloud_init(cloud_event_cb_t cb)                                // [C] Parameter ist ein Funktionszeiger-Typ (siehe cloud.h)
{
    esp_err_t err = chat_history_init();                                 // [Logik] Chat-Historie-Modul initialisieren
    if (err != ESP_OK) {                                                 // [Logik] Fehler früh durchreichen
        return err;
    }
    // Startwert aus dem Kconfig; tts_init() (2,5 MB PSRAM) passiert lazy
    // in speak_reply(), sobald der Toggle das erste Mal an ist.
#ifdef CONFIG_TTS_SPEAK_REPLIES                                          // [C] Präprozessor-Bedingung, abhängig vom Kconfig-Define
    atomic_store(&s_tts_enabled, true);                                  // [HW] Startwert TTS an (Kconfig-Option gesetzt)
#else
    atomic_store(&s_tts_enabled, false);                                 // [HW] Startwert TTS aus (Kconfig-Option nicht gesetzt)
#endif
    s_cb = cb;                                                           // [C] Funktionszeiger in statischer Variable ablegen | [Logik] Callback für spätere Ereignisse merken
    s_queue = xQueueCreate(1, sizeof(cloud_job_t));                      // [HW] FreeRTOS-Queue mit Tiefe 1 (nur ein Job gleichzeitig in Bearbeitung)
    if (!s_queue) {                                                      // [C] Zeiger-Nullprüfung | [Logik] Queue-Erzeugung fehlgeschlagen
        return ESP_ERR_NO_MEM;                                          // [Logik] Speicherfehler melden
    }
    BaseType_t ok = xTaskCreatePinnedToCore(network_task, "network",     // [HW] Task fest an Core 0 gebunden (Pinned), Prio 3, Stackgröße 16 KB
                                            16384, NULL, 3, NULL, 0);
    return ok == pdPASS ? ESP_OK : ESP_FAIL;                             // [C] ternärer Operator | [Logik] Task-Erzeugung erfolgreich?
}

// Nimmt einen fertigen WAV-Buffer entgegen und stößt die Pipeline im
// network_task an, sofern diese nicht bereits beschäftigt ist (Busy-Gate
// schützt den PSRAM-Recording-Buffer vor Überschreiben).
void cloud_submit(const uint8_t *wav, size_t wav_bytes)                  // [C] const uint8_t* = unveränderlicher Byte-Puffer-Zeiger
{
    if (atomic_exchange(&s_busy, true)) {                                // [C] atomic_exchange setzt und liefert Altwert atomar | [Logik] bereits busy?
        ESP_LOGW(TAG, "pipeline busy, dropping query");                  // [HW] Log-Hinweis, Anfrage wird verworfen
        return;                                                          // [Logik] Abbruch, kein neuer Job eingereiht
    }
    cloud_job_t job = { .wav = wav, .bytes = wav_bytes };                 // [C] designierte Initialisierer setzen nur die benannten Felder, Rest = 0
    xQueueSend(s_queue, &job, 0);                                        // [HW] Job in die Queue legen, Timeout 0 = nicht blockierend
}

// Liefert den aktuellen Busy-Zustand der Cloud-Pipeline (Wake-Word-Gate).
bool cloud_is_busy(void)                                                 // [C] Getter ohne Parameter
{
    return atomic_load(&s_busy);                                        // [C] atomarer Lesezugriff auf das Busy-Flag
}

// Liefert, ob gerade eine TTS-Wiedergabe läuft (Teilzustand von busy).
bool cloud_is_speaking(void)                                             // [C] Getter ohne Parameter
{
    return atomic_load(&s_speaking);                                    // [C] atomarer Lesezugriff auf das Speaking-Flag
}

// Setzt den Laufzeit-Schalter für automatisches Vorlesen der Antworten.
void cloud_set_tts_enabled(bool enabled)                                 // [C] Setter, Parameter per Wert
{
    atomic_store(&s_tts_enabled, enabled);                              // [C] atomarer Schreibzugriff auf das TTS-Enabled-Flag
}

// Liefert den aktuellen Zustand des TTS-Vorlese-Schalters.
bool cloud_tts_enabled(void)                                             // [C] Getter ohne Parameter
{
    return atomic_load(&s_tts_enabled);                                 // [C] atomarer Lesezugriff auf das TTS-Enabled-Flag
}

// Spricht einen fest vorgegebenen Text direkt per TTS (ohne Whisper/LLM-
// Roundtrip), z. B. für lokale Sprachkommando-Bestätigungen. Nutzt
// dasselbe Busy-Gate wie cloud_submit.
void cloud_speak(const char *text)                                       // [C] Parameter const char* = zu sprechender Text
{
    if (atomic_exchange(&s_busy, true)) {                                // [C] atomic_exchange setzt und liefert Altwert atomar | [Logik] Pipeline bereits belegt?
        ESP_LOGW(TAG, "cloud_speak: pipeline busy, dropping");           // [HW] Log-Hinweis, Aufruf wird verworfen
        return;                                                         // [Logik] Abbruch, kein neuer Job eingereiht
    }
    cloud_job_t job = { .speak_only = true };                            // [C] designierter Initialisierer, Rest der Struct-Felder = 0
    snprintf(job.text, sizeof(job.text), "%s", text);                    // [C] snprintf kopiert und begrenzt auf Puffergröße | [Logik] Text in Job-Struct hineinkopieren
    xQueueSend(s_queue, &job, 0);                                        // [HW] Job in die Queue legen, Timeout 0 = nicht blockierend
}
