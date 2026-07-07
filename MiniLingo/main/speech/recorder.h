#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// Grund, warum eine Aufnahme beendet wurde: entweder normale Stille-
// Erkennung per VAD, oder das harte Zeitlimit wurde erreicht.
typedef enum {
    RECORDER_STOP_SILENCE,  // 1,5 s zusammenhaengende Stille (VAD)   // [Logik] normales Ende: Sprecher hat aufgehört
    RECORDER_STOP_FULL,     // 8-s-Hard-Limit (Buffer voll)           // [Logik] Zwangsende: Aufnahmepuffer ist voll gelaufen
} recorder_stop_reason_t;                                    // [C] enum-Definition: benannte Konstanten für den Stopp-Grund

// Callback nach Capture-Ende. wav zeigt auf den fertigen WAV-Buffer
// (44-Byte-Header + PCM) im PSRAM; er bleibt gueltig bis zum naechsten
// recorder_start(). speech_ms = Summe der Frames mit VAD_SPEECH
// (Phase 8: Stille-Guard — fast stumme Aufnahmen nicht zu Whisper
// schicken, der halluziniert daraus Saetze wie "Vielen Dank.").
// Laeuft im Kontext des afe_fetch_task (Core 1) — keine LVGL-Calls,
// nur Flags/Queues setzen.
typedef void (*recorder_done_cb_t)(const uint8_t *wav, size_t wav_bytes,  // [C] Funktionszeiger-Typedef für den Recording-Fertig-Callback
                                   uint32_t duration_ms,
                                   uint32_t speech_ms,
                                   recorder_stop_reason_t reason);

// Allokiert den Recording-Buffer im PSRAM (8 s @ 16 kHz @ 16 bit
// = 256 KB + Header).
esp_err_t recorder_init(recorder_done_cb_t cb);              // [C] Prototyp, cb wird für spätere Fertig-Meldungen gespeichert

// start/feed/abort/is_active laufen alle im afe_fetch_task — kein
// Locking noetig, solange das so bleibt.
// Startet eine neue Aufnahme-Session: setzt Fuellstand/Zaehler zurueck
// und aktiviert den Recorder fuer recorder_feed().
void recorder_start(void);                                   // [Logik] initialisiert den Aufnahmezustand für eine neue Capture-Session
// Liefert, ob gerade eine Aufnahme laeuft (fuer die Wake-Fetch-Task-
// Logik, um Doppel-Starts/parallele Trigger auszuschliessen).
bool recorder_is_active(void);                                // [Logik] Statusabfrage für die State-Machine im afe_fetch_task

// Capture verwerfen ohne Done-Callback (Phase 4: MultiNet-Match —
// die Aufnahme war ein lokaler Command, kein Cloud-Query).
void recorder_abort(void);                                    // [Logik] bricht eine laufende Aufnahme ohne Weiterverarbeitung ab

// Haengt einen AFE-Frame an (bytes = Laenge in Bytes, wie
// afe_fetch_result_t.data_size). is_speech = VAD-Ergebnis des Frames.
// Beendet das Capture selbst (Stille/Limit) und ruft dann den Callback.
void recorder_feed(const int16_t *data, size_t bytes, bool is_speech);  // [HW] data = 16-bit-PCM-Frame vom AFE | [Logik] Kernfunktion: Puffern + Stille-/Limit-Erkennung
