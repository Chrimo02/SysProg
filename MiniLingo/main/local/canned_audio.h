#pragma once

#include <stddef.h>
#include <stdint.h>

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
#define CANNED_ERROR           "tts_error.wav"

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

// Streaming-Sink fuer die TTS-Wiedergabe (geraeteseitiges Streaming):
// der network_task dekodiert das Gemini-Audio chunkweise und schreibt es
// direkt ans I2S, statt erst den ganzen PCM-Buffer zu puffern. Waehrend
// SPEAKING spielt der Player-Task nichts, daher ist der Aufrufer der
// einzige I2S-Schreiber. "stop" (canned_audio_stop) laesst den naechsten
// _write abbrechen.
void canned_audio_stream_begin(uint32_t rate);
// Schreibt einen PCM-Chunk (16 bit LE mono). Veraendert den Buffer
// (Gain in-place). ESP_OK, oder ESP_ERR_INVALID_STATE bei "stop".
esp_err_t canned_audio_stream_write(int16_t *pcm, size_t samples);
