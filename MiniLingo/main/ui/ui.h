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

// Mic-Button (ui_mic_button.c)
void ui_mic_button_set_state(ui_mic_state_t state);
void ui_mic_button_set_click_cb(ui_mic_click_cb_t cb);
