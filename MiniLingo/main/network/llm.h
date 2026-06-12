#pragma once

#include <stddef.h>

#include "esp_err.h"

// Schickt user_text + Konversations-History (state.c) an das LLM
// (Cerebras, OpenAI-kompatible Chat-Completions) und liefert die
// Antwort. Blockiert; nur aus dem network_task aufrufen (TLS strikt
// sequenziell, Spec Section 13). Retry/Backoff bei transienten
// Fehlern macht das Modul selbst (http_util.h).
esp_err_t llm_generate(const char *user_text, char *reply,
                       size_t reply_len);
