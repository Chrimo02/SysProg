#pragma once

#include "esp_err.h"                                          // [Logik] esp_err_t-Fehlertyp für die Rückgabewerte
#include "model_path.h"                                        // [HW] esp-sr-Typ srmodel_list_t (geladene Modelle aus der model-Partition)

// Erstellt die AFE-Instanz (WakeNet9-Alexa, VAD, kein AEC) und startet
// afe_feed_task + afe_fetch_task auf Core 1 (Spec Section 5/8).
// models kommt von esp_srmodel_init("model") — main.c laedt die Liste
// einmal und reicht sie auch an commands_init (MultiNet) weiter; die
// Commands muessen VOR dem Pipeline-Start initialisiert sein, weil der
// fetch-Task sie ab dem ersten Wake Word fuettert.
// Erwartet initialisiertes audio_in (audio_in_init).
// character_idx (Phase B) waehlt das EINE zu ladende Wakeword
// (0=Mycroft, 1=Alexa, 2=Jarvis). Drei Wakenets gleichzeitig sprengen
// den internen RAM (Task-Stack-Alloc schlaegt fehl) — es wird deshalb
// immer nur das aktive Modell geladen; der Wechsel tauscht es zur
// Laufzeit (afe_pipeline_request_wakeword), ohne Reboot.
esp_err_t afe_pipeline_init(srmodel_list_t *models, int character_idx);  // [HW] models = geladene esp-sr-Modellliste, character_idx wählt das aktive Wakeword

// Charakter-Wechsel (Phase B2): fordert an, das WakeNet-Modell zur
// Laufzeit auf character_idx umzustellen (destroy + recreate). Nur ein
// atomares Flag; die eigentliche Umstellung macht der afe_fetch_task am
// naechsten Schleifenkopf (zwischen zwei fetch-Calls), damit kein Task
// mitten in einem AFE-Call den Buffer unter sich wegzieht. Aus jedem
// Task-Kontext aufrufbar. RAM-sicher, weil destroy VOR create laeuft
// (Peak = 1 Modell, wie beim Boot).
void afe_pipeline_request_wakeword(int character_idx);        // [Logik] setzt nur ein Request-Flag; die eigentliche Umkonfiguration läuft asynchron im fetch-Task

// Touch-Trigger (Spec Section 2: "Wake Word | Touch"): startet eine
// Listening-Session wie nach dem Wake Word, nur ohne WakeNet. Aus
// beliebigem Task-Kontext aufrufbar (Mic-Button, LVGL-Task); der
// afe_fetch_task holt das Flag mit dem naechsten AFE-Frame ab.
// Ignoriert, solange schon ein Capture oder die Cloud-Pipeline laeuft.
void afe_pipeline_request_listening(void);                    // [Logik] Touch-Button-Trigger, funktional äquivalent zu einem erkannten Wake Word
