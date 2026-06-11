#pragma once

#include <stddef.h>

#include "esp_err.h"

// Konversations-History fuer Gemini (Spec Section 9.4, Phase 6):
// Ringpuffer der letzten 10 Turns (5 User + 5 Model), wird bei jedem
// Request komplett mitgesendet. Zugriff nur aus dem network_task —
// kein Locking.

typedef enum {
    CHAT_ROLE_USER,
    CHAT_ROLE_MODEL,
} chat_role_t;

#define CHAT_HISTORY_TURNS 10

esp_err_t chat_history_init(void);

// Verdraengt bei vollem Puffer den aeltesten Turn; text wird kopiert
// (und ggf. auf die interne Turn-Groesse gekuerzt).
void chat_history_push(chat_role_t role, const char *text);

size_t chat_history_count(void);

// idx 0 = aeltester Turn. Pointer bleibt bis zum naechsten push
// gueltig. NULL bei idx >= count.
const char *chat_history_get(size_t idx, chat_role_t *role);
