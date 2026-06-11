// Konversations-History fuer Gemini (Phase 6). Die State-Machine aus
// der Spec (Section 8) ist als atomare Flags in main.c umgesetzt —
// hier lebt nur der History-Ringpuffer. Alle Zugriffe laufen im
// network_task (push nach erfolgreichem Turn, get beim Request-Bau).
#include <stdio.h>

#include "esp_heap_caps.h"

#include "state.h"

// 1 KB pro Turn: Transkripte sind < TRANSCRIPT_MAX (512), Antworten
// durch maxOutputTokens=200 begrenzt.
#define TURN_TEXT_MAX 1024

typedef struct {
    chat_role_t role;
    char text[TURN_TEXT_MAX];
} chat_turn_t;

static chat_turn_t *s_turns;  // PSRAM, ~10 KB
static size_t s_head;         // Index des aeltesten Turns
static size_t s_count;

esp_err_t chat_history_init(void)
{
    s_turns = heap_caps_calloc(CHAT_HISTORY_TURNS, sizeof(*s_turns),
                               MALLOC_CAP_SPIRAM);
    return s_turns ? ESP_OK : ESP_ERR_NO_MEM;
}

void chat_history_push(chat_role_t role, const char *text)
{
    // Bei vollem Puffer zeigt (head + count) % N wieder auf head:
    // der aelteste Turn wird ueberschrieben.
    size_t slot = (s_head + s_count) % CHAT_HISTORY_TURNS;
    if (s_count == CHAT_HISTORY_TURNS) {
        s_head = (s_head + 1) % CHAT_HISTORY_TURNS;
    } else {
        s_count++;
    }
    s_turns[slot].role = role;
    snprintf(s_turns[slot].text, TURN_TEXT_MAX, "%s", text);
}

size_t chat_history_count(void)
{
    return s_count;
}

const char *chat_history_get(size_t idx, chat_role_t *role)
{
    if (idx >= s_count) {
        return NULL;
    }
    const chat_turn_t *turn =
        &s_turns[(s_head + idx) % CHAT_HISTORY_TURNS];
    if (role) {
        *role = turn->role;
    }
    return turn->text;
}
