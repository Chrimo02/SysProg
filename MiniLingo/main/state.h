#pragma once                          // [C] Include-Guard-Ersatz: verhindert Mehrfach-Einbindung dieses Headers

#include <stddef.h>                    // [C] Standard-Header für size_t

#include "esp_err.h"                   // [HW] ESP-IDF-Header: definiert esp_err_t und Fehlercodes

// Konversations-History fuer Gemini (Spec Section 9.4, Phase 6):
// Ringpuffer der letzten 10 Turns (5 User + 5 Model), wird bei jedem
// Request komplett mitgesendet. Zugriff nur aus dem network_task —
// kein Locking.

// Rolle eines History-Turns: unterscheidet Nutzer-Eingabe von
// Modell-Antwort, damit chat_history_get() den Turn korrekt einordnen
// und der Request-Body die richtige Reihenfolge/Rollen abbilden kann.
typedef enum {
    CHAT_ROLE_USER,                    // [Logik] Turn stammt vom Nutzer (transkribierte Spracheingabe)
    CHAT_ROLE_MODEL,                   // [Logik] Turn stammt vom Sprachmodell (generierte Antwort)
} chat_role_t;

#define CHAT_HISTORY_TURNS 10          // [C] Präprozessor-Konstante | [Logik] Kapazität des Ringpuffers (5 User- + 5 Model-Turns)

// Alloziert den Ringpuffer im PSRAM. Muss einmalig vor dem ersten
// chat_history_push()/chat_history_get() laufen.
esp_err_t chat_history_init(void);                                       // [C] Rückgabetyp esp_err_t; void = keine Parameter

// Verdraengt bei vollem Puffer den aeltesten Turn; text wird kopiert
// (und ggf. auf die interne Turn-Groesse gekuerzt).
void chat_history_push(chat_role_t role, const char *text);              // [C] role per Wert, text als const-Zeiger (nur lesender Zugriff)

// Liefert die aktuelle Anzahl gespeicherter Turns (0..CHAT_HISTORY_TURNS).
size_t chat_history_count(void);                                         // [C] Rückgabetyp size_t (vorzeichenlose Zählgröße)

// idx 0 = aeltester Turn. Pointer bleibt bis zum naechsten push
// gueltig. NULL bei idx >= count.
const char *chat_history_get(size_t idx, chat_role_t *role);             // [C] role als Ausgabe-Zeiger (optional NULL); Rückgabe const-Zeiger auf internen Puffer
