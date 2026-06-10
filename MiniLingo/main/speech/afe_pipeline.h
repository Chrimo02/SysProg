#pragma once

#include "esp_err.h"

// Initialisiert esp-sr (Models aus der Flash-Partition "model"),
// erstellt die AFE-Instanz (WakeNet9-Alexa, VAD, kein AEC) und startet
// afe_feed_task + afe_fetch_task auf Core 1 (Spec Section 5/8).
// Erwartet initialisiertes audio_in (audio_in_init).
esp_err_t afe_pipeline_init(void);
