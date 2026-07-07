// Interne Schnittstelle zwischen ui.c und den UI-Teilmodulen.
#pragma once                    // [C] Include-Guard über Compiler-Pragma

#include "lvgl.h"                // [HW] LVGL-Typen (lv_obj_t) für die Create-Funktionen

// Layout-Konstanten (Spec Section 4, 240x320 Portrait)
#define UI_STATUS_BAR_H 30       // [C] Präprozessor-Konstante | [HW] Höhe der Status-Bar in Pixeln (oben)
#define UI_CHAT_H       230      // [HW] Höhe des Chat-Bereichs in Pixeln (Mitte)
#define UI_MIC_AREA_H   60       // [HW] Höhe des unteren Mic-/TTS-Button-Bereichs in Pixeln

void ui_status_bar_create(lv_obj_t *parent);  // [C] Zeiger-Parameter auf Eltern-Widget | [Logik] baut die Status-Bar als Kind von parent auf
void ui_chat_create(lv_obj_t *parent);        // [Logik] baut den Chat-Container als Kind von parent auf
void ui_mic_button_create(lv_obj_t *parent);  // [Logik] baut den Mic-Button als Kind von parent auf
void ui_tts_button_create(lv_obj_t *parent);  // [Logik] baut den TTS-Toggle-Button als Kind von parent auf
