#pragma once

#include "esp_err.h"

// Dateinamen in der SPIFFS-Partition "storage" — erzeugt von
// scripts/generate_canned_tts.py (dort ist die Phrase-Tabelle).
#define CANNED_TIMER_SET_30S   "tts_timer_set_30s.wav"
#define CANNED_TIMER_SET_60S   "tts_timer_set_60s.wav"
#define CANNED_TIMER_SET_300S  "tts_timer_set_300s.wav"
#define CANNED_TIMER_SET_600S  "tts_timer_set_600s.wav"
#define CANNED_TIMER_SET_900S  "tts_timer_set_900s.wav"
#define CANNED_TIMER_SET_1800S "tts_timer_set_1800s.wav"
#define CANNED_TIMER_CANCELLED "tts_timer_cancelled.wav"
#define CANNED_TIMER_DONE      "tts_timer_done.wav"
#define CANNED_NO_TIMER        "tts_no_timer.wav"
#define CANNED_TIME_LEFT       "tts_time_left.wav"
#define CANNED_VOLUME_CHANGED  "tts_volume_changed.wav"
#define CANNED_MUTED           "tts_muted.wav"
#define CANNED_UNMUTED         "tts_unmuted.wav"
#define CANNED_ALARM           "tts_alarm.wav"

// Mountet SPIFFS (Partition "storage"), prueft die erwarteten Files
// und startet den Player-Task (audio_out_task der Spec-Task-Tabelle).
// audio_out_init() muss vorher gelaufen sein.
esp_err_t canned_audio_init(void);

// Spielt ein WAV aus SPIFFS asynchron ab (Queue zum Player-Task,
// Tiefe 4) — aus jedem Task-Kontext aufrufbar. Mehrere Aufrufe spielen
// nacheinander (z.B. Timer-Done + Alarm).
void canned_audio_play(const char *name);

// Bricht die laufende Wiedergabe ab und leert die Queue ("stop").
void canned_audio_stop(void);
