#pragma once

#include <stdbool.h>
#include "lvgl.h"

// Mic-Button-Zustaende gemaess Spec Section 4 (Farben);
// Animationen folgen in Phase 8.
typedef enum {
    UI_MIC_IDLE,            // #888888 — Wake-Word lauscht
    UI_MIC_LISTENING,       // #E24B4A
    UI_MIC_LOCAL_RESPONSE,  // #1D9E75
    UI_MIC_PROCESSING,      // #EF9F27
    UI_MIC_SPEAKING,        // #1D9E75
} ui_mic_state_t;

typedef void (*ui_mic_click_cb_t)(void);

// Baut das statische Layout (Status-Bar, Chat-Container, Mic-Button)
// auf dem aktiven Screen auf. Nur aus dem LVGL-Task-Kontext bzw. vor
// dessen Start aufrufen — LVGL ist nicht thread-safe.
void ui_init(void);

// Chat (ui_chat.c)
void ui_chat_add_user_bubble(const char *text);
void ui_chat_add_assistant_bubble(const char *text);
void ui_chat_add_local_bubble(const char *text);

// Status-Bar (ui_status_bar.c)
void ui_status_bar_set_state_text(const char *text);
void ui_status_bar_set_wifi_connected(bool connected);
void ui_status_bar_set_volume(int percent);

// Touch-Volume (Phase 8b): Klick auf die Volume-Anzeige oeffnet einen
// Slider; jede Aenderung ruft den Callback (LVGL-Task-Kontext, wie
// der Mic-Button-Klick). Der Aufrufer setzt den neuen Wert per
// volume_set() und meldet ihn mit ui_status_bar_set_volume() zurueck.
typedef void (*ui_volume_changed_cb_t)(int percent);
void ui_status_bar_set_volume_changed_cb(ui_volume_changed_cb_t cb);

// Klick aufs WiFi-Icon (Status-Bar) -> Setup-Portal öffnen.
typedef void (*ui_wifi_clicked_cb_t)(void);
void ui_status_bar_set_wifi_clicked_cb(ui_wifi_clicked_cb_t cb);

// Klick auf den State-Text (Status-Bar) -> Charakter wechseln.
typedef void (*ui_state_clicked_cb_t)(void);
void ui_status_bar_set_state_clicked_cb(ui_state_clicked_cb_t cb);

// Mic-Button (ui_mic_button.c)
void ui_mic_button_set_state(ui_mic_state_t state);
void ui_mic_button_set_click_cb(ui_mic_click_cb_t cb);

// TTS-Toggle-Button (ui_tts_button.c): kleiner Button unten rechts,
// schaltet das Vorlesen an/aus. Klick im LVGL-Task-Kontext, wie der
// Mic-Button. set_enabled spiegelt nur die Optik (gruen an / grau aus).
typedef void (*ui_tts_toggle_cb_t)(void);
void ui_tts_button_set_enabled(bool on);
void ui_tts_button_set_click_cb(ui_tts_toggle_cb_t cb);
