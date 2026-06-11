#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// Cloud-Pipeline (Spec "network_task"): Whisper-STT (Phase 5) +
// Gemini-LLM (Phase 6); TTS (Phase 7) haengt sich hier an.

typedef enum {
    CLOUD_EVT_TRANSCRIPT,  // text = Whisper-Transkript
    CLOUD_EVT_REPLY,       // text = Gemini-Antwort
    CLOUD_EVT_ERROR,       // text = kurze Fehlerbeschreibung (fuer UI)
} cloud_event_type_t;

// Laeuft im network_task (Core 0, Prio 3) — keine LVGL-Calls, nur
// Flags/Queues setzen (gleiche Regel wie recorder_done_cb_t). text ist
// nur fuer die Dauer des Callbacks gueltig.
typedef void (*cloud_event_cb_t)(cloud_event_type_t type,
                                 const char *text);

// Startet den network_task (Spec Section 8: Prio 3, Core 0, 16 KB).
esp_err_t cloud_init(cloud_event_cb_t cb);

// Startet die Cloud-Pipeline fuer einen fertigen WAV-Buffer. Der
// Buffer (Recording-Buffer im PSRAM) muss gueltig bleiben, bis das
// Ergebnis-Event kommt — deshalb unterdrueckt afe_pipeline neue
// Wake-Word-Sessions, solange cloud_is_busy() (recorder_start wuerde
// den Buffer ueberschreiben). Aufruf aus dem afe_fetch_task.
void cloud_submit(const uint8_t *wav, size_t wav_bytes);

bool cloud_is_busy(void);
