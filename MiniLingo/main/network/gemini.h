#pragma once

#include <stddef.h>

#include "esp_err.h"

// Gemini 2.5 Flash LLM (Spec Section 9.4, Phase 6). user_text ist der
// neue User-Turn; die History (state.c) wird komplett mitgesendet,
// aber NICHT hier fortgeschrieben — der Aufrufer pusht beide Turns
// erst nach Erfolg, damit die History nur vollstaendige Paare enthaelt.
// Laeuft im network_task (eine TLS-Verbindung, Spec Section 13).
esp_err_t gemini_generate(const char *user_text, char *reply,
                          size_t reply_len);
