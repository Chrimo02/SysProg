#pragma once

// Listener fuer Wake-Word-Detection. Achtung: laeuft im Kontext des
// afe_fetch_task (Core 1) — keine LVGL-Calls darin, nur Flags/Queues
// setzen (LVGL ist nicht thread-safe, Spec Section 13).
typedef void (*wake_word_listener_t)(void);

void wake_word_set_listener(wake_word_listener_t listener);

// Wird vom afe_fetch_task bei WAKENET_DETECTED aufgerufen.
void wake_word_on_detected(int word_index);

// Touch-Trigger (Mic-Button): gleicher Listener, gleicher Kontext
// (afe_fetch_task) — fuer main.c ist es dasselbe Session-Start-Event.
void wake_word_on_touch(void);
