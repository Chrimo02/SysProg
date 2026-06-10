#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    RECORDER_STOP_SILENCE,  // 1,5 s zusammenhaengende Stille (VAD)
    RECORDER_STOP_FULL,     // 8-s-Hard-Limit (Buffer voll)
} recorder_stop_reason_t;

// Callback nach Capture-Ende. wav zeigt auf den fertigen WAV-Buffer
// (44-Byte-Header + PCM) im PSRAM; er bleibt gueltig bis zum naechsten
// recorder_start(). Laeuft im Kontext des afe_fetch_task (Core 1) —
// keine LVGL-Calls, nur Flags/Queues setzen.
typedef void (*recorder_done_cb_t)(const uint8_t *wav, size_t wav_bytes,
                                   uint32_t duration_ms,
                                   recorder_stop_reason_t reason);

// Allokiert den Recording-Buffer im PSRAM (8 s @ 16 kHz @ 16 bit
// = 256 KB + Header).
esp_err_t recorder_init(recorder_done_cb_t cb);

// start/feed/is_active laufen alle im afe_fetch_task — kein Locking
// noetig, solange das so bleibt.
void recorder_start(void);
bool recorder_is_active(void);

// Haengt einen AFE-Frame an (bytes = Laenge in Bytes, wie
// afe_fetch_result_t.data_size). is_speech = VAD-Ergebnis des Frames.
// Beendet das Capture selbst (Stille/Limit) und ruft dann den Callback.
void recorder_feed(const int16_t *data, size_t bytes, bool is_speech);
