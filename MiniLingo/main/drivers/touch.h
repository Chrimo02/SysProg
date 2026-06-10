#pragma once

#include "esp_err.h"
#include "lvgl.h"

// Haengt den XPT2046 an den von display_init() initialisierten SPI2-Bus
// und registriert ihn als LVGL-Pointer-Input-Device (Polling, 50 ms).
esp_err_t touch_init(void);

lv_indev_t *touch_get_lv_indev(void);
