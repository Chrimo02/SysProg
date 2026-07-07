// Konversations-History fuer Gemini (Phase 6). Die State-Machine aus
// der Spec (Section 8) ist als atomare Flags in main.c umgesetzt —
// hier lebt nur der History-Ringpuffer. Alle Zugriffe laufen im
// network_task (push nach erfolgreichem Turn, get beim Request-Bau).
#include <stdio.h>                     // [C] snprintf für sicheres Kopieren in den Turn-Buffer

#include "esp_heap_caps.h"             // [Logik] Heap-Allokator mit Capabilities (hier: PSRAM statt internem RAM)

#include "state.h"                     // [C] eigener Header: deklariert chat_role_t/chat_history_* (Prototyp-Abgleich)

// 1 KB pro Turn: Transkripte sind < TRANSCRIPT_MAX (512), Antworten
// durch maxOutputTokens=200 begrenzt.
#define TURN_TEXT_MAX 1024             // [C] Präprozessor-Konstante | [Logik] feste Obergrenze für die Textlänge eines gespeicherten Turns

// Ein einzelner History-Eintrag: Rolle (User/Model) plus zugehöriger
// Text. Bildet das Element-Array des Ringpuffers s_turns.
typedef struct {
    chat_role_t role;                 // [C] Enum-Feld | [Logik] wer hat diesen Turn erzeugt (User oder Modell)
    char text[TURN_TEXT_MAX];          // [C] eingebettetes Array fester Größe im Struct | [Logik] gespeicherter Gesprächstext dieses Turns
} chat_turn_t;

static chat_turn_t *s_turns;  // PSRAM, ~10 KB      // [C] Zeiger auf Heap-Array, static = Datei-lokale Linkage | [Logik] NULL bis chat_history_init() alloziert
static size_t s_head;         // Index des aeltesten Turns   // [C] size_t = vorzeichenlose Indexvariable
static size_t s_count;                                       // [Logik] aktuelle Anzahl belegter Slots (0..CHAT_HISTORY_TURNS)

// Alloziert den Ringpuffer im PSRAM (extern) statt im knappen internen
// RAM, da die History nicht DMA-fähig sein muss. Muss vor jedem
// push/get einmalig laufen.
esp_err_t chat_history_init(void)
{
    s_turns = heap_caps_calloc(CHAT_HISTORY_TURNS, sizeof(*s_turns),   // [C] sizeof(*s_turns) = Größe eines Elements über Dereferenzierung ermittelt
                               MALLOC_CAP_SPIRAM);                     // [HW] Capability-Flag: externes PSRAM statt internem DRAM
    return s_turns ? ESP_OK : ESP_ERR_NO_MEM;                          // [C] Ternärer Operator: NULL-Check als Bedingung | [Logik] Allokation fehlgeschlagen -> Fehlercode
}

// Fügt einen neuen Turn ans Ende des Ringpuffers an. Ist der Puffer
// voll, wird der älteste Turn überschrieben (FIFO-Verdrängung), damit
// immer nur die letzten CHAT_HISTORY_TURNS Turns im Speicher bleiben.
void chat_history_push(chat_role_t role, const char *text)
{
    // Bei vollem Puffer zeigt (head + count) % N wieder auf head:
    // der aelteste Turn wird ueberschrieben.
    size_t slot = (s_head + s_count) % CHAT_HISTORY_TURNS;    // [C] Modulo-Operator realisiert zirkulären Index | [Logik] nächster freier bzw. zu überschreibender Slot
    if (s_count == CHAT_HISTORY_TURNS) {                       // [Logik] Puffer bereits voll?
        s_head = (s_head + 1) % CHAT_HISTORY_TURNS;           // [C] zirkulärer Vorschub | [Logik] ältester Turn rückt weiter, alter Slot wird gleich überschrieben
    } else {
        s_count++;                                             // [C] Post-Inkrement | [Logik] Puffer noch nicht voll: Anzahl erhöhen
    }
    s_turns[slot].role = role;                                 // [C] Array-Indexierung + Feldzugriff | [Logik] Rolle des neuen Turns speichern
    snprintf(s_turns[slot].text, TURN_TEXT_MAX, "%s", text);   // [C] snprintf begrenzt auf TURN_TEXT_MAX, verhindert Overflow | [Logik] Text in den Slot kopieren (ggf. gekürzt)
}

// Liefert die Anzahl aktuell gespeicherter Turns (für Iteration per
// chat_history_get() beim Aufbau des Gemini-Request-Bodys).
size_t chat_history_count(void)
{
    return s_count;                                            // [Logik] aktueller Füllstand des Ringpuffers
}

// Liefert den Text (und optional die Rolle) des idx-ten Turns, wobei
// idx=0 der älteste noch gespeicherte Turn ist. Rechnet den logischen
// Index auf die physische Ringpuffer-Position um.
const char *chat_history_get(size_t idx, chat_role_t *role)
{
    if (idx >= s_count) {                                      // [C] Bereichsprüfung mit >= | [Logik] Index außerhalb der gespeicherten Turns
        return NULL;                                           // [Logik] ungültiger Zugriff signalisiert durch NULL-Rückgabe
    }
    const chat_turn_t *turn =                                  // [C] const-Zeiger: nur lesender Zugriff auf den internen Puffer
        &s_turns[(s_head + idx) % CHAT_HISTORY_TURNS];        // [C] '&' Adressoperator, Modulo für zirkuläre Umrechnung logisch->physisch
    if (role) {                                                 // [C] Null-Check: role-Parameter ist optional
        *role = turn->role;                                    // [C] Dereferenzierung des Ausgabe-Zeigers | [Logik] Rolle an den Aufrufer zurückgeben
    }
    return turn->text;                                          // [Logik] Zeiger auf den gespeicherten Text zurückgeben (gültig bis zum nächsten push)
}
