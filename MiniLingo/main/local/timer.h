#pragma once

#include <stdbool.h>

// Kitchen-Timer (Spec Section 7 "Timer-Implementierung"), Commands 1-9.
// start/cancel laufen im afe_fetch_task (Command-Dispatch), der
// Done-Callback im esp_timer-Task — dort keine LVGL-Calls, nur
// Flags/Queues (gleiche Regel wie recorder_done_cb_t).
typedef void (*kitchen_timer_done_cb_t)(void);

void kitchen_timer_init(kitchen_timer_done_cb_t done_cb);

// Startet (bzw. ersetzt) den Timer. Spielt KEINE Bestaetigung ab —
// das macht der Command-Dispatch (main.c).
void kitchen_timer_start(int seconds);

// true, wenn ein Timer lief und abgebrochen wurde.
bool kitchen_timer_cancel(void);

// Restzeit in Sekunden (aufgerundet), -1 wenn kein Timer laeuft.
// Aus jedem Task lesbar (fuer den Status-Bar-Countdown im LVGL-Task).
int kitchen_timer_remaining_s(void);
