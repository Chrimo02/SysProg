// TTS-Toggle-Button (Phase 10b): kleiner Button unten rechts im
// Mic-Bereich, schaltet das Vorlesen der Antworten an/aus. Reine
// UI-Schicht — der Klick meldet sich per Callback bei main.c, das den
// Laufzeit-Schalter in cloud.c setzt (kein direkter network-Bezug hier).
#include "ui.h"            // [C] öffentlicher Header (Callback-Typ, Prototypen)
#include "ui_internal.h"   // [C] interner Header (UI_MIC_AREA_H, Create-Funktion)

#define TTS_BTN_SIZE 34    // [C] Präprozessor-Konstante | [HW] Durchmesser/Kantenlänge des runden TTS-Buttons in Pixeln
#define COLOR_ON     0x1D9E75  // gruen wie LOCAL_RESPONSE/SPEAKING   // [Logik] Farbe im aktivierten Zustand (TTS an)
#define COLOR_OFF    0x444444  // [Logik] Farbe im deaktivierten Zustand (TTS aus)

static lv_obj_t *s_btn;                // [C] static-globale Variable | [HW] Zeiger auf das Button-Widget
static lv_obj_t *s_label;              // [HW] Zeiger auf das Textlabel ("TTS") im Button
static ui_tts_toggle_cb_t s_click_cb;  // [C] Funktionszeiger-Variable | [Logik] von main.c registrierter Klick-Callback

// LVGL-Event-Callback für den Klick auf den TTS-Button: reicht das
// Event unverändert an den registrierten externen Callback weiter, der
// den TTS-Schalter im Anwendungscode umschaltet.
static void tts_btn_event_cb(lv_event_t *e)                   // [C] static Callback, Signatur von LVGL vorgegeben
{
    (void)e;                                                   // [C] explizit ungenutzter Parameter, unterdrückt Compiler-Warnung
    if (s_click_cb) {                                          // [C] NULL-Prüfung vor Aufruf über Funktionszeiger
        s_click_cb();                                          // [Logik] leitet Klick an main.c weiter (TTS an/aus schalten)
    }
}

// Erstellt den runden TTS-Toggle-Button unten rechts im Mic-Bereich
// samt "TTS"-Textlabel. Startzustand ist ausgeschaltet. Wird einmalig
// aus ui_init() aufgerufen.
void ui_tts_button_create(lv_obj_t *parent)                    // [C] parent = Eltern-Widget, an das der Button gehängt wird
{
    s_btn = lv_button_create(parent);                           // [HW] erzeugt LVGL-Button-Widget als Kind von parent
    lv_obj_set_size(s_btn, TTS_BTN_SIZE, TTS_BTN_SIZE);        // [HW] quadratische Größe (später durch Radius rund)
    lv_obj_set_style_radius(s_btn, LV_RADIUS_CIRCLE, 0);       // [HW] maximaler Radius -> aus dem Quadrat wird ein Kreis
    // Unten rechts, vertikal mittig im 60-px-Mic-Bereich (wie der
    // Mic-Button), 8 px Rand zur Kante.
    lv_obj_align(s_btn, LV_ALIGN_BOTTOM_RIGHT, -8,             // [HW] Ausrichtung unten rechts mit 8 px Randabstand
                 -(UI_MIC_AREA_H - TTS_BTN_SIZE) / 2);         // [C] Ganzzahl-Arithmetik zur vertikalen Zentrierung im Mic-Bereich
    lv_obj_add_event_cb(s_btn, tts_btn_event_cb, LV_EVENT_CLICKED, NULL);  // [C] registriert Funktionszeiger als Event-Handler | [HW] LVGL ruft tts_btn_event_cb bei Klick-Events auf
    // Resistiver Touch: kleine Flaeche grosszuegiger treffbar machen.
    lv_obj_set_ext_click_area(s_btn, 8);                        // [HW] vergrößert die Touch-Trefferfläche über die sichtbare Buttongröße hinaus

    s_label = lv_label_create(s_btn);                           // [HW] Textlabel als Kind des Buttons
    lv_label_set_text(s_label, "TTS");                         // [HW] statischer Beschriftungstext
    lv_obj_center(s_label);                                     // [HW] zentriert das Label exakt im Button

    ui_tts_button_set_enabled(false);                           // [Logik] initialer Zustand: TTS ausgeschaltet (grau)
}

// Aktualisiert nur die Optik des Buttons (Hintergrund- und
// Textfarbe) je nach TTS-Zustand — die eigentliche Logik zum Ein-/
// Ausschalten der Sprachausgabe liegt außerhalb dieses Moduls.
void ui_tts_button_set_enabled(bool on)                        // [C] on = aktueller TTS-Zustand (true = aktiv)
{
    if (!s_btn) {                                                // [C] '!' logisches NICHT, prüft auf NULL-Zeiger | [Logik] Schutz vor Aufruf, bevor der Button erstellt wurde
        return;                                                  // [Logik] ohne existierenden Button gibt es nichts zu aktualisieren
    }
    lv_obj_set_style_bg_color(s_btn, lv_color_hex(on ? COLOR_ON : COLOR_OFF),
                              0);                                 // [C] ternärer Operator wählt Farbe je nach Bool-Parameter | [HW] setzt Button-Hintergrundfarbe (grün an / grau aus)
    lv_obj_set_style_text_color(
        s_label, lv_color_hex(on ? 0xFFFFFF : 0x888888), 0);     // [C] ternärer Operator für die Textfarbe | [HW] weißer Text bei aktiv, gedimmter Text bei inaktiv
}

// Registriert den Callback, der bei jedem Klick auf den TTS-Button
// aufgerufen wird (Aufrufer ist main.c).
void ui_tts_button_set_click_cb(ui_tts_toggle_cb_t cb)          // [C] cb = übergebener Funktionszeiger
{
    s_click_cb = cb;                                             // [Logik] Callback für spätere Verwendung in tts_btn_event_cb merken
}
