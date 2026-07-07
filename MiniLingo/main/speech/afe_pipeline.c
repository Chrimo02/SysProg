// AFE-Pipeline (Spec Section 5, angepasst an die esp-sr-2.x-API:
// afe_config_init() + esp_afe_handle_from_config() statt des
// V1-Makros AFE_CONFIG_DEFAULT aus der Spec).
//
// afe_feed_task:  I2S0 (audio_in) -> AFE
// afe_fetch_task: AFE -> WakeNet-Ergebnis -> wake_word_on_detected(),
//                 danach Frames parallel an recorder (Phase 3, Spec
//                 Section 9.1) und MultiNet/commands (Phase 4).
// Beide auf Core 1, Prio 5 (Task-Tabelle Spec Section 8).
#include <stdatomic.h>                                          // [C] Header für atomare Typen/Operationen (lock-freie Synchronisation zwischen Tasks)

#include "freertos/FreeRTOS.h"                                   // [HW] FreeRTOS-Basistypen/Makros
#include "freertos/task.h"                                       // [HW] Task-API (xTaskCreatePinnedToCore, vTaskDelay)

#include "esp_afe_sr_models.h"                                   // [HW] esp-sr AFE-Interface (Acoustic Front End: VAD, WakeNet-Anbindung)
#include "esp_heap_caps.h"                                       // [Logik] Heap-Allokator mit Capabilities (internes RAM)
#include "esp_log.h"                                             // [Logik] ESP_LOGI/ESP_LOGE-Logging über UART-Konsole
#include "esp_wn_models.h"                                       // [HW] esp-sr WakeNet-Modell-Handling
#include "sdkconfig.h"                                            // [HW] Kconfig-Werte (u.a. CONFIG_WAKE_WORD_THRESHOLD)

#include "drivers/audio_in.h"                                    // [HW] I2S-Mikrofon-Eingang (audio_in_read)
#include "network/cloud.h"                                       // [Logik] Cloud-Pipeline-Status (cloud_is_speaking/cloud_is_busy)
#include "commands.h"                                            // [Logik] lokale Sprachbefehl-Erkennung (MultiNet)
#include "recorder.h"                                            // [Logik] Aufnahme-Buffer für Cloud-Queries
#include "wake_word.h"                                           // [Logik] Wake-Word-/Touch-Event-Weiterleitung
#include "afe_pipeline.h"                                        // [C] eigener Header: Typ-/Prototyp-Abgleich

static const char *TAG = "afe_pipeline";                         // [C] static = datei-lokale Linkage | [Logik] Log-Tag zur Zuordnung der Ausgaben

static const esp_afe_sr_iface_t *s_afe_handle;                   // [C] static-globaler Zeiger auf die AFE-Funktionstabelle (Vtable) | [HW] Handle für alle AFE-Operationen (feed/fetch/destroy/...)
static esp_afe_sr_data_t *s_afe_data;                            // [C] static-globaler Zeiger auf den internen AFE-Instanzzustand
// Touch-Trigger (Mic-Button): gesetzt aus dem LVGL-Task, konsumiert
// vom afe_fetch_task mit dem naechsten AFE-Frame (~32 ms Latenz).
static atomic_bool s_listen_request;                             // [C] atomarer bool: race-frei zwischen LVGL-Task (Setzer) und afe_fetch_task (Leser) | [Logik] Touch-Anforderung einer Listening-Session

// Charakter-Wechsel ohne Reboot (Phase B2):
static srmodel_list_t *s_models;       // fuer create_from_config beim Reconfigure  // [HW] gemerkte Modell-Liste, damit reconfigure_wakeword später erneut ein Wakenet laden kann
static int s_character_idx = -1;       // aktuell geladenes Wakeword (0/1/2)         // [Logik] -1 = noch kein Charakter geladen; sonst Index des aktiven Wakewords
// -1 = keine Anforderung, sonst Ziel-Charakter. Gesetzt aus beliebigem
// Task (afe_pipeline_request_wakeword), konsumiert im afe_fetch_task.
static atomic_int s_reconfig_char;                               // [C] atomarer int: race-freier Cross-Task-Request für einen Charakterwechsel
// Handshake zwischen fetch- (Reconfigure) und feed-Task: feed parkt,
// damit waehrend destroy/create kein feed() auf s_afe_data laeuft.
static atomic_bool s_feed_park;                                  // [C] atomares Flag: fetch-Task fordert den feed-Task zum Pausieren auf
static atomic_bool s_feed_parked;                                // [C] atomares Flag: feed-Task bestätigt, dass er tatsächlich pausiert ist

// Setzt das Touch-Request-Flag; wird aus beliebigem Task-Kontext (z.B.
// LVGL-Task bei Mic-Button-Druck) aufgerufen, ausgewertet im
// afe_fetch_task beim naechsten Frame.
void afe_pipeline_request_listening(void)
{
    atomic_store(&s_listen_request, true);                       // [C] atomarer Schreibzugriff (verhindert Data Race mit dem lesenden Task) | [Logik] Touch-Ereignis für den fetch-Task vormerken
}

// Merkt den gewuenschten Ziel-Charakter fuer einen Wakeword-Wechsel vor
// (nur ein Flag, keine sofortige Umsetzung); der eigentliche Wechsel
// passiert asynchron im afe_fetch_task.
void afe_pipeline_request_wakeword(int character_idx)
{
    atomic_store(&s_reconfig_char, character_idx);                // [C] atomarer Schreibzugriff | [Logik] Charakter-Wechsel-Wunsch für den fetch-Task hinterlegen
}

// AFE mit dem Wakeword des Charakters bauen (aus init + reconfigure).
// Setzt s_afe_handle/s_afe_data/s_character_idx; wendet Threshold an.
static esp_err_t create_afe(int character_idx)                   // [C] static Hilfsfunktion, nur in dieser Datei sichtbar
{
    afe_config_t *cfg =
        afe_config_init("M", s_models, AFE_TYPE_SR, AFE_MODE_LOW_COST);  // [HW] "M" = 1 Mic-Kanal; AFE_TYPE_SR = Speech-Recognition-Profil; LOW_COST spart Rechenzeit/RAM
    if (!cfg) {                                                    // [C] NULL-Check nach Config-Erzeugung
        ESP_LOGE(TAG, "afe_config_init failed");
        return ESP_FAIL;
    }
    cfg->aec_init = false;                                         // [HW] AEC (Echo-Cancellation) aus: kein Lautsprecher-Feedback zu kompensieren
    cfg->wakenet_init = true;                                       // [HW] WakeNet-Erkennung aktivieren
    cfg->vad_init = true;                                           // [HW] Voice-Activity-Detection aktivieren (für Recorder-Stille-Erkennung)
    cfg->afe_perferred_core = 1;                                    // [HW] AFE-interne Tasks auf Core 1 pinnen
    cfg->afe_perferred_priority = 5;                                // [HW] Task-Priorität für AFE-interne Verarbeitung

    const char *kw = (character_idx == 1)   ? "alexa"              // [C] verschachtelter Ternär-Operator als kompakte if/else-Kette | [Logik] Charakter-Index -> Wakeword-Name
                     : (character_idx == 2) ? "jarvis"
                                            : "mycroft";
    char *wn = esp_srmodel_filter(s_models, ESP_WN_PREFIX, kw);     // [HW] sucht das passende WakeNet-Modell für den gewählten Namen in der Modell-Liste
    cfg->wakenet_model_name = wn;                                   // [HW] genau EIN Wakenet wird geladen (RAM-Grund, siehe Header-Kommentar)
    cfg->wakenet_model_name_2 = NULL;                                // [HW] zweites Wakenet-Slot bewusst ungenutzt
    ESP_LOGI(TAG, "wakenet model: %s (character %d)",
             wn ? wn : "(none)", character_idx);                    // [C] Ternärer Operator für NULL-sicheres Logging

    s_afe_handle = esp_afe_handle_from_config(cfg);                 // [HW] liefert die zur Config passende AFE-Funktionstabelle
    s_afe_data = s_afe_handle->create_from_config(cfg);              // [C] Aufruf über Funktionszeiger in der Vtable | [HW] erzeugt die eigentliche AFE-Instanz (alloziert internes RAM)
    afe_config_free(cfg);                                            // [HW] Config-Struct wird nach der Instanzerzeugung nicht mehr gebraucht
    if (!s_afe_data) {                                               // [C] NULL-Check: Instanzerzeugung fehlgeschlagen (z.B. RAM knapp)
        ESP_LOGE(TAG, "AFE create_from_config failed");
        return ESP_FAIL;
    }

    // Kconfig-Threshold (0-100) auf WakeNet-Range 0.0-1.0 mappen;
    // Index 1 = erstes Wake Word. 0 = Model-Default belassen.
#if CONFIG_WAKE_WORD_THRESHOLD > 0                                  // [C] Präprozessor-Bedingung: Code nur einkompiliert, wenn Kconfig-Wert gesetzt ist
    s_afe_handle->set_wakenet_threshold(s_afe_data, 1,
                                        CONFIG_WAKE_WORD_THRESHOLD / 100.0f);  // [C] Ganzzahl-durch-Float-Division skaliert 0-100 auf 0.0-1.0 | [HW] Erkennungsschwelle für Wake Word Index 1
#endif
    s_character_idx = character_idx;                                 // [Logik] aktuell geladenen Charakter merken (für spätere No-Op-Prüfung bei erneutem Request)
    return ESP_OK;
}

// Wakeword zur Laufzeit umstellen. NUR aus dem afe_fetch_task am
// Schleifenkopf aufrufen (zwischen zwei fetch-Calls) — dann ist weder
// fetch noch (nach dem Park-Handshake) feed in einem AFE-Call.
// Zerstoert die alte AFE-Instanz und baut sie mit dem neuen Charakter
// neu auf; parkt dafuer kurz den feed_task, damit kein Task gleichzeitig
// auf s_afe_data zugreift (RAM-Peak bleibt bei genau 1 Modell).
static void reconfigure_wakeword(int character_idx)                 // [C] static Hilfsfunktion, nur vom afe_fetch_task aufgerufen
{
    if (character_idx == s_character_idx) {                          // [C] Gleichheitsvergleich | [Logik] gewünschter Charakter ist schon aktiv -> nichts zu tun
        return;  // schon aktiv
    }
    int prev = s_character_idx;                                      // [Logik] alten Charakter merken für ein eventuelles Rollback
    ESP_LOGI(TAG, "reconfigure wakenet: character %d -> %d",
             prev, character_idx);

    // Feed-Task parken und warten, bis er wirklich aus feed() raus ist.
    atomic_store(&s_feed_park, true);                                 // [C] atomarer Schreibzugriff | [Logik] fordert den feed-Task auf, zu pausieren
    while (!atomic_load(&s_feed_parked)) {                            // [C] Busy-Wait-Schleife mit atomarem Lesezugriff | [Logik] warten, bis feed-Task bestätigt hat, wirklich pausiert zu sein
        vTaskDelay(pdMS_TO_TICKS(5));                                  // [HW] FreeRTOS-Delay: gibt die CPU für 5 ms an andere Tasks ab, statt aktiv zu blockieren
    }

    s_afe_handle->destroy(s_afe_data);                                // [HW] gibt die alte AFE-Instanz (inkl. altem Wakenet) frei
    s_afe_data = NULL;                                                 // [C] Zeiger auf NULL setzen, um Use-after-free zu vermeiden
    esp_err_t err = create_afe(character_idx);                        // [Logik] neue AFE-Instanz mit dem gewünschten Wakenet aufbauen
    if (err != ESP_OK && prev >= 0) {                                  // [C] logisches UND | [Logik] Neuaufbau fehlgeschlagen, es gab vorher einen gültigen Zustand?
        ESP_LOGE(TAG, "reconfigure failed, restoring character %d", prev);
        create_afe(prev);  // best effort: alte Pipeline zurueckholen   // [Logik] Rollback-Versuch, damit die Pipeline nicht ganz ausfällt
    }

    atomic_store(&s_feed_park, false);                                 // [C] atomarer Schreibzugriff | [Logik] feed-Task darf weiterlaufen
    ESP_LOGI(TAG, "wakenet reconfigured, free internal: %u B (largest block %u B)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));  // [HW] Diagnose: freier/größter zusammenhängender interner RAM-Block nach dem Wechsel
}

// FreeRTOS-Task: liest fortlaufend Mikrofon-Samples per I2S (audio_in)
// und fuettert sie in die AFE-Pipeline. Beruecksichtigt den Park-
// Handshake fuer den Wakeword-Reconfigure und loggt periodisch den
// Mic-Pegel (RMS/Peak) zur Gain-Diagnose.
static void afe_feed_task(void *arg)                                 // [C] FreeRTOS-Task-Funktion, Signatur von xTaskCreatePinnedToCore vorgegeben | [HW] arg ungenutzt (kein Task-Parameter nötig)
{
    int chunk = s_afe_handle->get_feed_chunksize(s_afe_data);         // [HW] von AFE vorgegebene Chunk-Größe (Samples pro feed()-Aufruf)
    int channels = s_afe_handle->get_feed_channel_num(s_afe_data);    // [HW] von AFE erwartete Kanalzahl pro Frame
    ESP_LOGI(TAG, "feed task: chunk %d samples, %d channel(s)",
             chunk, channels);

    int16_t *buf = heap_caps_malloc(chunk * channels * sizeof(int16_t),  // [C] Größenberechnung: Samples * Kanäle * Elementgröße | [HW] Puffer für genau einen AFE-Feed-Chunk
                                    MALLOC_CAP_INTERNAL);              // [HW] Capability-Flag: internes RAM
    assert(buf);                                                       // [C] Programmabbruch, falls Allokation fehlschlägt (Init-kritischer Fehler)

    int chunks = 0;                                                    // [Logik] Zähler für die periodische Mic-Pegel-Diagnose

    while (1) {                                                        // [C] Endlosschleife: typische Struktur eines FreeRTOS-Dauer-Tasks
        // Park-Handshake fuer den Wakeword-Reconfigure (Phase B2): der
        // fetch-Task setzt s_feed_park, wir bestaetigen mit s_feed_parked
        // und ruehren s_afe_data nicht an, bis er wieder freigibt.
        if (atomic_load(&s_feed_park)) {                                // [C] atomarer Lesezugriff | [Logik] wurde ein Pausieren angefordert?
            atomic_store(&s_feed_parked, true);                          // [Logik] Pause bestätigen
            while (atomic_load(&s_feed_park)) {                          // [Logik] warten, bis die Anforderung wieder zurückgenommen wird
                vTaskDelay(pdMS_TO_TICKS(5));                             // [HW] 5 ms CPU an andere Tasks abgeben, statt busy zu warten
            }
            atomic_store(&s_feed_parked, false);                         // [Logik] Pause-Bestätigung zurücknehmen, feed läuft wieder
            continue;                                                    // [C] überspringt den restlichen Schleifenkörper, zurück zum Schleifenkopf
        }
        if (audio_in_read(buf, (size_t)chunk * channels) == ESP_OK) {   // [C] Cast auf size_t | [HW] blockierender I2S-Read genau einer AFE-Chunk-Länge
            // Mic-Pegel-Diagnose alle ~3s (Chunk = 32 ms), analog zu
            // mic_test.c. Debug-Level: fuer Gain-Tuning per
            // CONFIG_LOG_MAXIMUM_LEVEL_DEBUG aktivieren. Referenz vom
            // Wake-Word-Test 2026-06-10: Ruhe rms 10-30, "Alexa" in
            // normaler Lautstaerke rms 250-400 — detektiert zuverlaessig.
            if (++chunks % 100 == 0) {                                   // [C] Pre-Inkrement + Modulo: alle 100 Chunks eine Diagnose-Zeile
                int64_t sum_sq = 0;                                       // [C] 64-bit-Akkumulator, um Overflow bei Quadratsummen zu vermeiden
                int16_t peak = 0;                                        // [Logik] höchster Betragswert im Chunk
                for (int i = 0; i < chunk * channels; i++) {              // [C] for-Schleife über alle Samples des Chunks
                    sum_sq += (int32_t)buf[i] * buf[i];                   // [C] Cast verhindert 16-bit-Überlauf bei der Multiplikation | [HW] Quadratsumme für RMS-Berechnung
                    int16_t a = buf[i] < 0 ? -buf[i] : buf[i];            // [C] Ternärer Operator: manueller Betrag (abs) für int16_t
                    if (a > peak) {                                        // [Logik] neuen Spitzenwert gefunden?
                        peak = a;
                    }
                }
                ESP_LOGD(TAG, "mic rms %.0f peak %d",
                         __builtin_sqrt((double)sum_sq / (chunk * channels)),  // [C] Compiler-Intrinsic sqrt | [HW] RMS = Wurzel des mittleren Quadrats -> Lautstärke-Maß
                         peak);
            }
            s_afe_handle->feed(s_afe_data, buf);                          // [HW] übergibt den gelesenen Audio-Chunk an die AFE-Pipeline zur Weiterverarbeitung
        }
    }
}

// FreeRTOS-Task: holt fortlaufend verarbeitete AFE-Ergebnisse ab (VAD +
// WakeNet), fuehrt Wakeword-Reconfigures zwischen zwei Fetch-Aufrufen
// aus, startet/stoppt Recorder- und Commands-Sessions je nach Wake-
// Word-/Touch-Event und Cloud-Status, und speist waehrend einer aktiven
// Session Frames parallel an recorder und commands weiter. Herzstueck
// der State-Machine fuer Wake-Word -> Aufnahme -> lokaler Command bzw.
// Cloud-Query.
static void afe_fetch_task(void *arg)                                  // [C] FreeRTOS-Task-Funktion | [HW] arg ungenutzt
{
    // MultiNet-Session, die nur fuer den "stop"-Command waehrend der
    // TTS-Wiedergabe laeuft (Phase 7, Spec Section 9.6: WakeNet aus,
    // MultiNet an). Nur in diesem Task benutzt.
    bool speak_session = false;                                         // [Logik] merkt, ob die aktuelle Commands-Session extra für die TTS-Stop-Erkennung gestartet wurde

    while (1) {                                                          // [C] Endlosschleife des Fetch-Tasks
        // Charakter-Wechsel (Phase B2): Wakeword hier umstellen, zwischen
        // zwei fetch-Calls — nie mitten in fetch_with_delay, und das
        // vorige res ist ausgewertet (kein Use-after-free auf res->data).
        int reconfig = atomic_exchange(&s_reconfig_char, -1);            // [C] atomares Read-und-Reset in einer Operation (verhindert verlorene/doppelte Requests) | [Logik] holt einen evtl. ausstehenden Charakter-Wechsel-Wunsch ab
        if (reconfig >= 0) {                                              // [Logik] -1 bedeutet "kein Request"
            reconfigure_wakeword(reconfig);                               // [Logik] Wakeword sicher an dieser Stelle umstellen
        }

        afe_fetch_result_t *res =
            s_afe_handle->fetch_with_delay(s_afe_data, portMAX_DELAY);    // [HW] blockierender Abruf des nächsten verarbeiteten AFE-Ergebnisses (VAD/WakeNet/Audio)
        if (!res || res->ret_value == ESP_FAIL) {                        // [C] logisches ODER, kurzschließend | [Logik] kein gültiges Ergebnis -> überspringen
            continue;
        }

        // SPEAKING: MultiNet mitlaufen lassen, damit "stop" die
        // Wiedergabe abbrechen kann. Laeuft die Session in ihren
        // 6-s-Timeout oder matcht ein False-Positive, wird sie hier
        // einfach neu gestartet, solange die Wiedergabe dauert; nach
        // dem Wiedergabe-Ende wird sie beendet. (Eine normale
        // Wake-Word-Session kann hier nicht aktiv sein — waehrend der
        // Cloud-Pipeline startet keine, s.u.)
        bool speaking = cloud_is_speaking();                              // [Logik] läuft gerade eine TTS-Wiedergabe?
        if (speaking && !commands_session_active()) {                     // [C] logisches UND mit Negation | [Logik] Wiedergabe läuft, aber noch keine Erkennungs-Session -> starten
            commands_session_start();                                     // [Logik] MultiNet mitlaufen lassen, damit "stop" erkannt werden kann
            speak_session = true;                                          // [Logik] merken, dass diese Session TTS-spezifisch ist
        } else if (!speaking && speak_session) {                           // [Logik] Wiedergabe zu Ende, und die laufende Session war die TTS-Session
            commands_session_stop();                                       // [Logik] TTS-Stop-Erkennung wird nicht mehr gebraucht
            speak_session = false;                                          // [Logik] Markierung zurücksetzen
        }

        // Solange die Cloud-Pipeline laeuft, kein neues Wake Word
        // und kein Touch-Trigger annehmen: recorder_start() wuerde
        // den WAV-Buffer ueberschreiben, den der Whisper-Upload
        // gerade liest. Ein Touch waehrend einer laufenden Session
        // wird konsumiert und ignoriert (no-op, Session laeuft schon).
        bool touch = atomic_exchange(&s_listen_request, false);           // [C] atomares Read-und-Reset | [Logik] Touch-Request abholen und zurücksetzen
        if ((res->wakeup_state == WAKENET_DETECTED || touch)               // [C] logisches ODER | [Logik] entweder Wake Word erkannt oder Touch-Trigger
            && !recorder_is_active() && !cloud_is_busy()                   // [C] logisches UND mit mehreren Negationen | [Logik] nur wenn gerade keine Aufnahme läuft und die Cloud-Pipeline frei ist
            && !commands_session_active()) {                               // [Logik] und auch keine Command-Erkennung schon läuft
            if (touch && res->wakeup_state != WAKENET_DETECTED) {          // [C] logisches UND mit Ungleichheitsvergleich | [Logik] reiner Touch-Trigger ohne gleichzeitiges Wake Word
                wake_word_on_touch();                                       // [Logik] Touch-Event wie ein Wake Word behandeln
            } else {
                wake_word_on_detected(res->wake_word_index);                // [HW] übergibt den Index des erkannten Wakewords weiter
            }
            // Capture ab dem naechsten Frame — res->data dieses Frames
            // enthaelt noch das Ende des Wake Words selbst (bzw. beim
            // Touch das Tipp-Geraeusch).
            recorder_start();                                               // [Logik] Aufnahme-Session für die folgende Sprachäußerung starten
            commands_session_start();                                       // [Logik] parallel MultiNet-Erkennung für lokale Commands starten
            continue;                                                       // [C] überspringt den Rest der Schleife, das aktuelle Frame wird nicht mehr gefüttert
        }

        if (recorder_is_active() || commands_session_active()) {           // [C] logisches ODER | [Logik] läuft gerade eine Aufnahme oder Command-Erkennung?
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
            commands_feed(res->data, (size_t)res->data_size / sizeof(int16_t));  // [C] Cast + Division: Byte-Größe in Sample-Anzahl umrechnen | [HW] füttert den AFE-Frame in die MultiNet-Erkennung
            recorder_feed(res->data, (size_t)res->data_size,
                          res->vad_state == VAD_SPEECH);                    // [C] Vergleichsausdruck als bool-Argument | [HW] übergibt Frame + VAD-Ergebnis an den Recorder
        }
    }
}

// Baut die AFE-Instanz fuer den gegebenen Charakter auf und startet die
// beiden Dauer-Tasks (feed/fetch), die die komplette Wake-Word- und
// Aufnahme-Pipeline auf Core 1 am Laufen halten. Muss nach audio_in_init
// und NACH commands_init aufgerufen werden (der fetch-Task fuettert
// commands ab dem ersten Wake Word).
esp_err_t afe_pipeline_init(srmodel_list_t *models, int character_idx)   // [HW] models = geladene esp-sr-Modellliste, character_idx wählt das initial aktive Wakeword
{
    if (!models || models->num <= 0) {                                    // [C] logisches ODER, kurzschließend | [Logik] Plausibilitätsprüfung: wurden überhaupt Modelle geladen?
        ESP_LOGE(TAG, "no models in partition 'model' — flashed?");
        return ESP_FAIL;                                                   // [Logik] ohne Modelle keine AFE-Initialisierung möglich
    }
    s_models = models;                                                     // [Logik] Modell-Liste für spätere Reconfigures merken
    atomic_store(&s_reconfig_char, -1);                                    // [C] atomarer Schreibzugriff | [Logik] initial keine Charakter-Wechsel-Anforderung ausstehend

    // "M" = 1 Mic-Kanal, 16 kHz, VAD im AFE_TYPE_SR-Default, kein AEC.
    // Nur das EINE Wakeword des aktiven Charakters laden (0=Mycroft,
    // 1=Alexa, 2=Jarvis). Drei Wakenets gleichzeitig sprengen den
    // internen RAM; der Wechsel tauscht das Modell zur Laufzeit
    // (afe_pipeline_request_wakeword -> reconfigure_wakeword).
    esp_err_t err = create_afe(character_idx);                             // [Logik] initiale AFE-Instanz mit dem gewünschten Start-Charakter aufbauen
    if (err != ESP_OK) {                                                    // [C] Ungleichheitsvergleich
        return err;                                                        // [Logik] Fehler beim Aufbau nach oben durchreichen
    }

    BaseType_t ok;                                                          // [C] FreeRTOS-Rückgabetyp für Task-Erzeugung (pdPASS/pdFAIL)
    ok = xTaskCreatePinnedToCore(afe_feed_task, "afe_feed", 4096, NULL, 5,  // [HW] erzeugt den Feed-Task mit 4096 Byte Stack, Priorität 5, angepinnt auf Core 1 (letzter Parameter)
                                 NULL, 1);
    assert(ok == pdPASS);                                                   // [C] Programmabbruch, falls Task-Erzeugung fehlschlägt (Init-kritisch)
    ok = xTaskCreatePinnedToCore(afe_fetch_task, "afe_fetch", 8192, NULL, 5,  // [HW] Fetch-Task mit größerem Stack (8192 Byte), da er mehr Logik/Callbacks ausführt
                                 NULL, 1);
    assert(ok == pdPASS);                                                   // [C] Programmabbruch bei Fehlschlag

    ESP_LOGI(TAG, "AFE pipeline up (character %d), free internal: %u B "
                  "(largest block %u B)", character_idx,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));  // [HW] Diagnose: freier/größter zusammenhängender interner RAM-Block nach dem Start
    return ESP_OK;                                                          // [Logik] Pipeline erfolgreich gestartet
}
