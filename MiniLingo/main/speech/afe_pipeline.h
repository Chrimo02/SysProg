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
esp_err_t afe_pipeline_init(srmodel_list_t *models);
