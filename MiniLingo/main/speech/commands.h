#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "model_path.h"

// Command-IDs gemaess Spec Section 7 (Command-Liste).
typedef enum {
    CMD_TIMER_1MIN = 1,   // "set timer one minute"
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
} command_id_t;

// Match-Callback. Laeuft im afe_fetch_task (Core 1) — keine LVGL-Calls,
// nur Handler/Flags/Queues (gleiche Regel wie recorder_done_cb_t).
typedef void (*commands_match_cb_t)(command_id_t command_id,
                                    const char *phrase);

// Laedt MultiNet7-EN aus der Model-Partition und registriert die
// 14 Commands. models kommt von afe_pipeline_get_models().
esp_err_t commands_init(srmodel_list_t *models, commands_match_cb_t cb);

// Session = ein Erkennungsfenster nach dem Wake Word. start() setzt den
// Model-State zurueck, stop() beendet das Fenster (Capture-Ende ohne
// Match -> Aufnahme geht ab Phase 5 in die Cloud). Alle drei laufen im
// afe_fetch_task.
void commands_session_start(void);
void commands_session_stop(void);
bool commands_session_active(void);

// Fuettert einen AFE-Frame (samples = Anzahl int16). Sammelt intern auf
// die MultiNet-Chunk-Groesse und ruft bei Match den Callback; danach
// (oder bei MultiNet-Timeout) ist die Session inaktiv.
void commands_feed(const int16_t *data, size_t samples);
