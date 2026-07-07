#pragma once                    // [C] Include-Guard über Compiler-Pragma statt #ifndef/#define/#endif

#include <stdbool.h>             // [C] Standardheader für den Typ bool
#include "lvgl.h"                // [HW] LVGL-Grafikbibliothek: Widget-Typen (lv_obj_t etc.), Rendering-API

// Zustände des Mikrofon-Buttons: bestimmen Farbe und (in Phase 8)
// Animation des Buttons je nach Voice-Pipeline-Phase (Wake-Word warten,
// Aufnahme läuft, lokale Antwort, Cloud-Verarbeitung, Sprachausgabe).
typedef enum {                              // [C] enum = benannte Ganzzahl-Konstanten, hier als Zustandsmenge
    UI_MIC_IDLE,            // #888888 — Wake-Word lauscht         // [Logik] Grundzustand: wartet auf Wake-Word
    UI_MIC_LISTENING,       // #E24B4A                             // [Logik] Nutzeräußerung wird aufgenommen
    UI_MIC_LOCAL_RESPONSE,  // #1D9E75                             // [Logik] lokale (Offline-)Antwort wird angezeigt
    UI_MIC_PROCESSING,      // #EF9F27                             // [Logik] Cloud-Anfrage läuft, wartet auf Antwort
    UI_MIC_SPEAKING,        // #1D9E75                             // [Logik] Text-to-Speech gibt Antwort per Audio aus
} ui_mic_state_t;                            // [C] Alias-Name für den anonymen enum-Typ via typedef

// Funktionszeiger-Typ für den Klick-Callback des Mic-Buttons: main.c
// registriert hier eine Funktion, die bei jedem Button-Klick aufgerufen
// wird (Voice-Pipeline anstoßen).
typedef void (*ui_mic_click_cb_t)(void);     // [C] typedef auf Zeiger-auf-Funktion (Signatur: keine Parameter, kein Rückgabewert) | [HW] dient als LVGL-ähnlicher Event-Callback

// Baut das statische Layout (Status-Bar, Chat-Container, Mic-Button)
// auf dem aktiven Screen auf. Nur aus dem LVGL-Task-Kontext bzw. vor
// dessen Start aufrufen — LVGL ist nicht thread-safe.
void ui_init(void);                          // [C] Funktionsprototyp/Deklaration ohne Körper (Definition in ui.c)

// Chat (ui_chat.c)
void ui_chat_add_user_bubble(const char *text);        // [C] const char* = schreibgeschützter String-Zeiger | [Logik] fügt Nutzer-Sprechblase (rechts) hinzu
void ui_chat_add_assistant_bubble(const char *text);   // [Logik] fügt Assistant-Sprechblase (links, grau) hinzu
void ui_chat_add_local_bubble(const char *text);       // [Logik] fügt lokale Offline-Antwort-Sprechblase (links, grün) hinzu

// Status-Bar (ui_status_bar.c)
void ui_status_bar_set_state_text(const char *text);       // [Logik] setzt den mittleren Statustext (z.B. Charaktername)
void ui_status_bar_set_wifi_connected(bool connected);      // [Logik] färbt das WiFi-Symbol je nach Verbindungsstatus
void ui_status_bar_set_volume(int percent);                 // [Logik] aktualisiert die Lautstärkeanzeige rechts in der Bar

// Touch-Volume (Phase 8b): Klick auf die Volume-Anzeige oeffnet einen
// Slider; jede Aenderung ruft den Callback (LVGL-Task-Kontext, wie
// der Mic-Button-Klick). Der Aufrufer setzt den neuen Wert per
// volume_set() und meldet ihn mit ui_status_bar_set_volume() zurueck.
typedef void (*ui_volume_changed_cb_t)(int percent);        // [C] Funktionszeiger-Typ mit int-Parameter für Lautstärkeänderungen
void ui_status_bar_set_volume_changed_cb(ui_volume_changed_cb_t cb);  // [Logik] registriert den Callback beim Status-Bar-Modul

// Klick aufs WiFi-Icon (Status-Bar) -> Setup-Portal öffnen.
typedef void (*ui_wifi_clicked_cb_t)(void);                 // [C] Funktionszeiger-Typ ohne Parameter für WiFi-Klick
void ui_status_bar_set_wifi_clicked_cb(ui_wifi_clicked_cb_t cb);  // [Logik] registriert den Callback für WiFi-Icon-Klicks

// Klick auf den State-Text (Status-Bar) -> Charakter wechseln.
typedef void (*ui_state_clicked_cb_t)(void);                // [C] Funktionszeiger-Typ ohne Parameter für State-Text-Klick
void ui_status_bar_set_state_clicked_cb(ui_state_clicked_cb_t cb);  // [Logik] registriert den Callback für Charakterwechsel-Klicks

// Mic-Button (ui_mic_button.c)
void ui_mic_button_set_state(ui_mic_state_t state);         // [Logik] setzt Farbe/Animation des Mic-Buttons je nach Zustand
void ui_mic_button_set_click_cb(ui_mic_click_cb_t cb);      // [Logik] registriert den Klick-Callback des Mic-Buttons

// TTS-Toggle-Button (ui_tts_button.c): kleiner Button unten rechts,
// schaltet das Vorlesen an/aus. Klick im LVGL-Task-Kontext, wie der
// Mic-Button. set_enabled spiegelt nur die Optik (gruen an / grau aus).
typedef void (*ui_tts_toggle_cb_t)(void);                   // [C] Funktionszeiger-Typ ohne Parameter für TTS-Toggle-Klick
void ui_tts_button_set_enabled(bool on);                    // [Logik] aktualisiert nur die Optik des Buttons (an/aus)
void ui_tts_button_set_click_cb(ui_tts_toggle_cb_t cb);     // [Logik] registriert den Klick-Callback des TTS-Buttons
