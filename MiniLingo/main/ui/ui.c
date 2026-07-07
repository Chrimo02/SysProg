// LVGL-Screen-Setup: setzt den Hintergrund und baut die drei
// Layout-Bereiche (Status-Bar, Chat, Mic-Button) auf.
#include "ui.h"            // [C] eigener öffentlicher Header (Prototyp von ui_init, Typdefinitionen)
#include "ui_internal.h"   // [C] interner Header mit den Create-Funktionen der Teilmodule

// Baut das komplette statische UI-Layout einmalig auf dem aktiven
// LVGL-Screen auf: Hintergrund, Status-Bar, Chat-Bereich, Mic-Button
// und TTS-Button. Muss aus dem LVGL-Task-Kontext heraus aufgerufen werden.
void ui_init(void)                                            // [C] Rückgabetyp void, keine Parameter -> reine Initialisierungsfunktion
{
    lv_obj_t *scr = lv_screen_active();                       // [C] lv_obj_t* = Zeiger auf LVGL-Objekt-Struct | [HW] liefert den aktuell aktiven Display-Screen (Wurzel-Widget)
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x1A1A1A), 0); // [HW] setzt Hintergrundfarbe des Screens (dunkles Grau) über das Style-System
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);             // [HW] Deckkraft der Hintergrundfarbe voll (kein Durchscheinen)
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);            // [HW] verhindert, dass der Screen selbst scrollbar ist (nur Chat-Container soll scrollen)

    ui_status_bar_create(scr);                                // [Logik] baut Status-Bar (oben) als Kind-Objekt des Screens auf
    ui_chat_create(scr);                                       // [Logik] baut Chat-Container (Mitte) auf
    ui_mic_button_create(scr);                                 // [Logik] baut Mikrofon-Button (unten) auf
    ui_tts_button_create(scr);                                 // [Logik] baut TTS-Toggle-Button (unten rechts) auf
}
