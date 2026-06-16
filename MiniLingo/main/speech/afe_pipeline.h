#pragma once

#include "esp_err.h"
#include "model_path.h"

// Erstellt die AFE-Instanz (WakeNet9-Alexa, VAD, kein AEC) und startet
// afe_feed_task + afe_fetch_task auf Core 1 (Spec Section 5/8).
// models kommt von esp_srmodel_init("model") — main.c laedt die Liste
// einmal und reicht sie auch an commands_init (MultiNet) weiter; die
// Commands muessen VOR dem Pipeline-Start initialisiert sein, weil der
// fetch-Task sie ab dem ersten Wake Word fuettert.
// Erwartet initialisiertes audio_in (audio_in_init).
// character_idx (Phase B) waehlt das EINE zu ladende Wakeword
// (0=Mycroft, 1=Alexa, 2=Jarvis). Drei Wakenets gleichzeitig sprengen
// den internen RAM (Task-Stack-Alloc schlaegt fehl) — der Charakter-
// Switcher laedt deshalb nur das aktive Modell und rebootet beim Wechsel.
esp_err_t afe_pipeline_init(srmodel_list_t *models, int character_idx);

// Touch-Trigger (Spec Section 2: "Wake Word | Touch"): startet eine
// Listening-Session wie nach dem Wake Word, nur ohne WakeNet. Aus
// beliebigem Task-Kontext aufrufbar (Mic-Button, LVGL-Task); der
// afe_fetch_task holt das Flag mit dem naechsten AFE-Frame ab.
// Ignoriert, solange schon ein Capture oder die Cloud-Pipeline laeuft.
void afe_pipeline_request_listening(void);
