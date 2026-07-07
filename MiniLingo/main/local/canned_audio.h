#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// Dateinamen in der SPIFFS-Partition "storage" — erzeugt von
// scripts/generate_canned_tts.py (dort ist die Phrase-Tabelle).
#define CANNED_TIMER_SET_5S    "tts_timer_set_5s.wav"     // [C] Präprozessor-Konstante | [Logik] Dateiname der vorab generierten Sprachausgabe "Timer 5 Sekunden gestellt"
#define CANNED_TIMER_SET_10S   "tts_timer_set_10s.wav"    // [Logik] Sprachausgabe "Timer 10 Sekunden gestellt"
#define CANNED_TIMER_SET_15S   "tts_timer_set_15s.wav"    // [Logik] Sprachausgabe "Timer 15 Sekunden gestellt"
#define CANNED_TIMER_SET_30S   "tts_timer_set_30s.wav"    // [Logik] Sprachausgabe "Timer 30 Sekunden gestellt"
#define CANNED_TIMER_SET_60S   "tts_timer_set_60s.wav"    // [Logik] Sprachausgabe "Timer 1 Minute gestellt"
#define CANNED_TIMER_SET_300S  "tts_timer_set_300s.wav"   // [Logik] Sprachausgabe "Timer 5 Minuten gestellt"
#define CANNED_TIMER_SET_600S  "tts_timer_set_600s.wav"   // [Logik] Sprachausgabe "Timer 10 Minuten gestellt"
#define CANNED_TIMER_SET_900S  "tts_timer_set_900s.wav"   // [Logik] Sprachausgabe "Timer 15 Minuten gestellt"
#define CANNED_TIMER_SET_1800S "tts_timer_set_1800s.wav"  // [Logik] Sprachausgabe "Timer 30 Minuten gestellt"
#define CANNED_TIMER_CANCELLED "tts_timer_cancelled.wav"  // [Logik] Sprachausgabe "Timer abgebrochen"
#define CANNED_TIMER_DONE      "tts_timer_done.wav"       // [Logik] Sprachausgabe "Timer fertig"
#define CANNED_NO_TIMER        "tts_no_timer.wav"         // [Logik] Sprachausgabe "kein Timer aktiv"
#define CANNED_TIME_LEFT       "tts_time_left.wav"        // [Logik] Sprachausgabe zur Ansage der Restzeit
#define CANNED_VOLUME_CHANGED  "tts_volume_changed.wav"   // [Logik] Sprachausgabe "Lautstärke geändert"
#define CANNED_MUTED           "tts_muted.wav"             // [Logik] Sprachausgabe "stummgeschaltet"
#define CANNED_UNMUTED         "tts_unmuted.wav"           // [Logik] Sprachausgabe "Stummschaltung aufgehoben"
#define CANNED_ALARM           "tts_alarm.wav"             // [Logik] Alarmton, der z.B. nach Timer-Ende gespielt wird
#define CANNED_ERROR           "tts_error.wav"             // [Logik] generische Fehler-Sprachausgabe

// Mountet SPIFFS (Partition "storage"), prueft die erwarteten Files
// und startet den Player-Task (audio_out_task der Spec-Task-Tabelle).
// audio_out_init() muss vorher gelaufen sein.
esp_err_t canned_audio_init(void);

// Spielt ein WAV aus SPIFFS asynchron ab (Queue zum Player-Task,
// Tiefe 4) — aus jedem Task-Kontext aufrufbar. Mehrere Aufrufe spielen
// nacheinander (z.B. Timer-Done + Alarm).
void canned_audio_play(const char *name);

// Spielt einen rohen PCM-Buffer (16 bit LE mono, Phase 7: Gemini-TTS)
// ueber den Player-Task und BLOCKIERT bis zum Wiedergabe-Ende bzw.
// canned_audio_stop() — der Buffer muss solange gueltig bleiben.
// Es darf nur einen solchen Aufrufer geben (network_task).
esp_err_t canned_audio_play_pcm(const int16_t *pcm, size_t samples,
                                uint32_t rate);

// Bricht die laufende Wiedergabe ab und leert die Queue ("stop") —
// seit Phase 7 auch die TTS-PCM-Wiedergabe.
void canned_audio_stop(void);
