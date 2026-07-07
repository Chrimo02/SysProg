#pragma once

#include <stddef.h>                    // [C] Header-Include | Standardtypen (u. a. size_t) fuer Puffergroessen

#include "esp_err.h"                   // [C] Header-Include | esp_err_t-Fehlercode-Typ fuer Rueckgabewerte

// Antwortlaengen: Text bleibt unter 1 KB (<3 Saetze, System-Prompt);
// Tool-Calls sind kurze Funktionsnamen + ein JSON-Arg-String.
#define LLM_TEXT_MAX      1024        // [C] Praeprozessor-Konstante | [Logik] max. Laenge des Text-Antwort-Puffers (inkl. Nullterminator)
#define LLM_TOOL_NAME_MAX 32          // [C] Praeprozessor-Konstante | [Logik] max. Laenge des Tool-Funktionsnamens
#define LLM_TOOL_ARGS_MAX 256         // [C] Praeprozessor-Konstante | [Logik] max. Laenge des JSON-Argument-Strings eines Tool-Calls

// Das LLM antwortet entweder mit Text ODER mit einem Tool-Call
// (Phase 10, OpenAI-Tool-Format gegen Cerebras): freie Timer-/Volume-
// Wuensche, die die lokalen MultiNet-Commands nicht abdecken.
//
// Diskriminator-Enum fuer llm_result_t: legt fest, welches der beiden
// Ergebnisfelder (text oder tool_name/tool_args) gueltig befuellt ist.
typedef enum {
    LLM_RESULT_TEXT,       // text gefuellt                     // [C] Enum-Konstante (Wert 0 = erster Aufzaehlungswert) | [Logik] Antworttyp: reine Text-Antwort
    LLM_RESULT_TOOL_CALL,  // tool_name + tool_args gefuellt     // [C] Enum-Konstante (Wert 1) | [Logik] Antworttyp: Aufruf eines Tools (z. B. Timer setzen)
} llm_result_type_t;                                            // [C] typedef vergibt Aliasnamen fuer den anonymen enum-Typ

// Container fuer das Ergebnis eines llm_generate()-Aufrufs: enthaelt je
// nach type entweder die Klartext-Antwort oder Name+Argumente eines
// Tool-Calls. Vom Aufrufer allokiert (kein Heap-Objekt).
typedef struct {
    llm_result_type_t type;                    // [C] Struct-Feld: Enum-Wert als Diskriminator | [Logik] steuert, welches Feld unten gueltig ist
    char text[LLM_TEXT_MAX];                   // [C] Fixed-Size-char-Array (kein Pointer) -> Speicher liegt inline im Struct | [Logik] Text-Antwort des LLM
    char tool_name[LLM_TOOL_NAME_MAX];         // [C] Fixed-Size-char-Array | [Logik] Name des vom LLM gewaehlten Tools (z. B. "set_timer")
    char tool_args[LLM_TOOL_ARGS_MAX];  // JSON-String (OpenAI "arguments")   // [C] Fixed-Size-char-Array | [HW] JSON-kodierte Argumente wie sie die OpenAI-API liefert
} llm_result_t;                                                              // [C] typedef vergibt Aliasnamen fuer den anonymen struct-Typ

// Schickt user_text + Konversations-History (state.c) an das LLM
// (Cerebras, OpenAI-kompatible Chat-Completions) und liefert in *out
// entweder die Text-Antwort oder einen Tool-Call. Blockiert; nur aus
// dem network_task aufrufen (TLS strikt sequenziell, Spec Section 13).
// Retry/Backoff bei transienten Fehlern macht das Modul selbst
// (http_util.h).
esp_err_t llm_generate(const char *user_text, llm_result_t *out);  // [C] Funktionsdeklaration; out ist Ausgabeparameter per Pointer | [HW] loest blockierenden HTTPS-Request aus | [Logik] zentrale Schnittstelle des LLM-Moduls

// Setzt die aktive Persona (Charakter-Switcher, Phase B). persona ist ein
// statisches String-Literal (z. B. aus der character_t-Tabelle in main.c);
// die Tool-Hinweise hängt build_request selbst an. NULL = Default-Persona.
// Aus dem LVGL-Task aufrufbar.
void llm_set_system_prompt(const char *persona);  // [C] Funktionsdeklaration; const char * = schreibgeschuetzter Pointer auf statisches String-Literal | [Logik] setzt globalen Persona-Zustand
