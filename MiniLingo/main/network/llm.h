#pragma once

#include <stddef.h>

#include "esp_err.h"

// Antwortlaengen: Text bleibt unter 1 KB (<3 Saetze, System-Prompt);
// Tool-Calls sind kurze Funktionsnamen + ein JSON-Arg-String.
#define LLM_TEXT_MAX      1024
#define LLM_TOOL_NAME_MAX 32
#define LLM_TOOL_ARGS_MAX 256

// Das LLM antwortet entweder mit Text ODER mit einem Tool-Call
// (Phase 10, OpenAI-Tool-Format gegen Cerebras): freie Timer-/Volume-
// Wuensche, die die lokalen MultiNet-Commands nicht abdecken.
typedef enum {
    LLM_RESULT_TEXT,       // text gefuellt
    LLM_RESULT_TOOL_CALL,  // tool_name + tool_args gefuellt
} llm_result_type_t;

typedef struct {
    llm_result_type_t type;
    char text[LLM_TEXT_MAX];
    char tool_name[LLM_TOOL_NAME_MAX];
    char tool_args[LLM_TOOL_ARGS_MAX];  // JSON-String (OpenAI "arguments")
} llm_result_t;

// Schickt user_text + Konversations-History (state.c) an das LLM
// (Cerebras, OpenAI-kompatible Chat-Completions) und liefert in *out
// entweder die Text-Antwort oder einen Tool-Call. Blockiert; nur aus
// dem network_task aufrufen (TLS strikt sequenziell, Spec Section 13).
// Retry/Backoff bei transienten Fehlern macht das Modul selbst
// (http_util.h).
esp_err_t llm_generate(const char *user_text, llm_result_t *out);
