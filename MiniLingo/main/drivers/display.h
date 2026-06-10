#pragma once

#include "esp_err.h"
#include "lvgl.h"

#define DISPLAY_H_RES 240
#define DISPLAY_V_RES 320

// Initialisiert SPI2-Bus + ILI9341 und registriert das LVGL-Display.
// Muss nach lv_init() und vor touch_init() aufgerufen werden
// (touch_init() haengt sich an den hier initialisierten SPI-Bus).
esp_err_t display_init(void);

lv_display_t *display_get_lv_display(void);
