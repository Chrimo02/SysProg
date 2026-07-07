#pragma once                          // [C] Include-Guard-Ersatz: verhindert Mehrfach-Einbindung dieses Headers

#include "esp_err.h"                   // [HW] ESP-IDF-Header: definiert esp_err_t und Fehlercodes (z.B. ESP_OK)
#include "lvgl.h"                      // [HW] LVGL-Grafikbibliothek: liefert lv_display_t und die Rendering-API

#define DISPLAY_H_RES 240              // [HW] horizontale Auflösung des ILI9341-Panels in Pixel
#define DISPLAY_V_RES 320              // [HW] vertikale Auflösung des ILI9341-Panels in Pixel

// Initialisiert SPI2-Bus + ILI9341 und registriert das LVGL-Display.
// Muss nach lv_init() und vor touch_init() aufgerufen werden
// (touch_init() haengt sich an den hier initialisierten SPI-Bus).
esp_err_t display_init(void);          // [C] Funktionsdeklaration/Prototyp; void = keine Parameter, Rückgabe esp_err_t

lv_display_t *display_get_lv_display(void);  // [C] Rückgabetyp Zeiger auf lv_display_t (Getter für das LVGL-Display-Handle)
