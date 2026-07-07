#pragma once                          // [C] Include-Guard-Ersatz: verhindert Mehrfach-Einbindung dieses Headers

#include "esp_err.h"                   // [HW] ESP-IDF-Header: definiert esp_err_t und Fehlercodes (z.B. ESP_OK)
#include "lvgl.h"                      // [HW] LVGL-Grafikbibliothek: liefert lv_indev_t und die Input-Device-API

// Haengt den XPT2046 an den von display_init() initialisierten SPI2-Bus
// und registriert ihn als LVGL-Pointer-Input-Device (Polling, 50 ms).
esp_err_t touch_init(void);            // [C] Funktionsdeklaration/Prototyp; void = keine Parameter, Rückgabe esp_err_t

lv_indev_t *touch_get_lv_indev(void);  // [C] Rückgabetyp Zeiger auf lv_indev_t (Getter für das LVGL-Input-Device-Handle)
