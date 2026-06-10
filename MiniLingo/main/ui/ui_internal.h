// Interne Schnittstelle zwischen ui.c und den UI-Teilmodulen.
#pragma once

#include "lvgl.h"

// Layout-Konstanten (Spec Section 4, 240x320 Portrait)
#define UI_STATUS_BAR_H 30
#define UI_CHAT_H       230
#define UI_MIC_AREA_H   60

void ui_status_bar_create(lv_obj_t *parent);
void ui_chat_create(lv_obj_t *parent);
void ui_mic_button_create(lv_obj_t *parent);
