#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "model_path.h"                                        // [HW] esp-sr-Typ srmodel_list_t für die geladene Modell-Liste

// Command-IDs gemaess Spec Section 7 (Command-Liste).
// Aufzählung aller per MultiNet erkennbaren lokalen Sprachbefehle;
// jede ID identifiziert eindeutig, welche Aktion nach einem Match
// ausgeführt werden soll (Timer, Lautstärke, Charakterwechsel, ...).
typedef enum {
    CMD_TIMER_1MIN = 1,   // "set timer one minute"                // [Logik] explizite Werte ab 1, damit 0 kein gültiger Command ist
    CMD_TIMER_5MIN = 2,   // "set timer five minutes"
    CMD_TIMER_10MIN = 3,  // "set timer ten minutes"
    CMD_TIMER_15MIN = 4,  // "set timer fifteen minutes"
    CMD_TIMER_30MIN = 5,  // "set timer thirty minutes"
    CMD_TIMER_30SEC = 6,  // "set timer thirty seconds"
    CMD_TIMER_STOP = 7,   // "stop timer"
    CMD_TIMER_CANCEL = 8, // "cancel timer"
    CMD_TIME_LEFT = 9,    // "how much time left"
    CMD_STOP = 10,        // "stop"
    CMD_VOLUME_UP = 11,   // "volume up"
    CMD_VOLUME_DOWN = 12, // "volume down"
    CMD_MUTE = 13,        // "mute"
    CMD_UNMUTE = 14,      // "unmute"
    // Phase 8b (User-Wunsch, nicht in Spec Section 7): direkte
    // Volume-Stufen — die 20-%-Schritte von volume.c als Phrasen.
    CMD_VOLUME_20 = 15,   // "volume twenty percent"
    CMD_VOLUME_40 = 16,   // "volume forty percent"
    CMD_VOLUME_60 = 17,   // "volume sixty percent"
    CMD_VOLUME_80 = 18,   // "volume eighty percent"
    CMD_VOLUME_100 = 19,  // "volume one hundred percent"
    CMD_TEMPERATURE = 20, // "temperature" / "what is the temperature"
    // Charakter-Wechsel per Voice (Phase B2): IDs fortlaufend, damit
    // (id - CMD_CHARACTER_MYCROFT) direkt der Charakter-Index 0/1/2 ist
    // (Reihenfolge = s_characters[] in main.c).
    CMD_CHARACTER_MYCROFT = 21, // "switch to mycroft"               // [Logik] fortlaufende IDs erlauben Umrechnung ID -> Charakter-Index per Subtraktion
    CMD_CHARACTER_ALEXA = 22,   // "switch to alexa"
    CMD_CHARACTER_JARVIS = 23,  // "switch to jarvis"
    // Namensunabhaengiger, zuverlaessiger Wechsel: zykelt wie der Touch-
    // Button. Haeufige englische Woerter -> MultiNet trifft sie gut, im
    // Gegensatz zu den Eigennamen (v.a. "alexa").
    CMD_CHARACTER_NEXT = 24,    // "next character" / "change assistant"
    // Sekunden-Timer (2026-07-02): ohne diese matcht "N seconds" den
    // gleichnamigen Minuten-Command (z.B. "fifteen seconds" -> 15 min).
    CMD_TIMER_5SEC = 25,        // "set timer five seconds"
    CMD_TIMER_10SEC = 26,       // "set timer ten seconds"
    CMD_TIMER_15SEC = 27,       // "set timer fifteen seconds"
} command_id_t;                                                // [C] enum-Typedef: symbolische Konstanten für alle erkennbaren Commands

// Match-Callback. Laeuft im afe_fetch_task (Core 1) — keine LVGL-Calls,
// nur Handler/Flags/Queues (gleiche Regel wie recorder_done_cb_t).
// prob = MultiNet-Wahrscheinlichkeit des Matches; der per-Command-
// Mindestwert ist da schon geprueft, prob dient zusaetzlicher Policy
// (z.B. strengeres "stop" waehrend der TTS-Wiedergabe in main.c).
typedef void (*commands_match_cb_t)(command_id_t command_id,   // [C] Funktionszeiger-Typedef für den Treffer-Callback
                                    const char *phrase, float prob);

// MultiNet-Timeout = kein lokaler Command -> die Aufnahme ist ein
// Cloud-Query (Phase 5). Gleicher Kontext wie der Match-Callback.
typedef void (*commands_timeout_cb_t)(void);                   // [C] Funktionszeiger-Typedef für den Timeout-Callback (keine Parameter)

// Laedt MultiNet7-EN aus der Model-Partition und registriert die
// Commands (Tabelle in commands.c). models kommt von
// afe_pipeline_get_models().
esp_err_t commands_init(srmodel_list_t *models, commands_match_cb_t cb,  // [HW] models = geladene esp-sr-Modelle | [Logik] cb/timeout_cb werden für spätere Events gespeichert
                        commands_timeout_cb_t timeout_cb);

// Session = ein Erkennungsfenster nach dem Wake Word. start() setzt den
// Model-State zurueck, stop() beendet das Fenster (Capture-Ende ohne
// Match -> Aufnahme geht ab Phase 5 in die Cloud). Alle drei laufen im
// afe_fetch_task.
void commands_session_start(void);                             // [Logik] beginnt ein neues Erkennungsfenster (MultiNet-State zurücksetzen)
void commands_session_stop(void);                               // [Logik] beendet das aktuelle Erkennungsfenster
bool commands_session_active(void);                             // [Logik] Statusabfrage, ob gerade eine Erkennung läuft

// Fuettert einen AFE-Frame (samples = Anzahl int16). Sammelt intern auf
// die MultiNet-Chunk-Groesse und ruft bei Match den Callback; danach
// (oder bei MultiNet-Timeout) ist die Session inaktiv.
void commands_feed(const int16_t *data, size_t samples);       // [HW] data = 16-bit-PCM-Samples eines AFE-Frames
