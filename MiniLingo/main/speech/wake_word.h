#pragma once

// Listener fuer Wake-Word-Detection. Achtung: laeuft im Kontext des
// afe_fetch_task (Core 1) — keine LVGL-Calls darin, nur Flags/Queues
// setzen (LVGL ist nicht thread-safe, Spec Section 13).
typedef void (*wake_word_listener_t)(void);      // [C] Funktionszeiger-Typedef (Callback ohne Parameter/Rückgabe) | [Logik] entkoppelt Wake-Word-Modul vom Rest der App (main.c registriert sich)

// Registriert den Listener, der bei jedem Wake-Word-/Touch-Event
// aufgerufen wird (i.d.R. einmalig aus main.c beim Start).
void wake_word_set_listener(wake_word_listener_t listener);   // [C] Funktionsprototyp, Parameter ist ein Funktionszeiger

// Wird vom afe_fetch_task bei WAKENET_DETECTED aufgerufen.
// Signalisiert ein erkanntes Wake Word und löst darüber den Listener aus.
void wake_word_on_detected(int word_index);                  // [Logik] word_index identifiziert, welches der geladenen Wakewords getroffen hat

// Touch-Trigger (Mic-Button): gleicher Listener, gleicher Kontext
// (afe_fetch_task) — fuer main.c ist es dasselbe Session-Start-Event.
// Startet dieselbe Listening-Session wie ein Wake Word, nur ausgelöst
// durch den physischen Mic-Button statt durch Spracherkennung.
void wake_word_on_touch(void);
