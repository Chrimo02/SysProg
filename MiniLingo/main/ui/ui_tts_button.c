// TTS-Toggle-Button (Phase 10b): kleiner Button unten rechts im
// Mic-Bereich, schaltet das Vorlesen der Antworten an/aus. Reine
// UI-Schicht — der Klick meldet sich per Callback bei main.c, das den
// Laufzeit-Schalter in cloud.c setzt (kein direkter network-Bezug hier).
#include "ui.h"
#include "ui_internal.h"

#define TTS_BTN_SIZE 34
#define COLOR_ON     0x1D9E75  // gruen wie LOCAL_RESPONSE/SPEAKING
#define COLOR_OFF    0x444444

static lv_obj_t *s_btn;
static lv_obj_t *s_label;
static ui_tts_toggle_cb_t s_click_cb;

static void tts_btn_event_cb(lv_event_t *e)
{
    (void)e;
    if (s_click_cb) {
        s_click_cb();
    }
}

void ui_tts_button_create(lv_obj_t *parent)
{
    s_btn = lv_button_create(parent);
    lv_obj_set_size(s_btn, TTS_BTN_SIZE, TTS_BTN_SIZE);
    lv_obj_set_style_radius(s_btn, LV_RADIUS_CIRCLE, 0);
    // Unten rechts, vertikal mittig im 60-px-Mic-Bereich (wie der
    // Mic-Button), 8 px Rand zur Kante.
    lv_obj_align(s_btn, LV_ALIGN_BOTTOM_RIGHT, -8,
                 -(UI_MIC_AREA_H - TTS_BTN_SIZE) / 2);
    lv_obj_add_event_cb(s_btn, tts_btn_event_cb, LV_EVENT_CLICKED, NULL);
    // Resistiver Touch: kleine Flaeche grosszuegiger treffbar machen.
    lv_obj_set_ext_click_area(s_btn, 8);

    s_label = lv_label_create(s_btn);
    lv_label_set_text(s_label, "TTS");
    lv_obj_center(s_label);

    ui_tts_button_set_enabled(false);
}

void ui_tts_button_set_enabled(bool on)
{
    if (!s_btn) {
        return;
    }
    lv_obj_set_style_bg_color(s_btn, lv_color_hex(on ? COLOR_ON : COLOR_OFF),
                              0);
    lv_obj_set_style_text_color(
        s_label, lv_color_hex(on ? 0xFFFFFF : 0x888888), 0);
}

void ui_tts_button_set_click_cb(ui_tts_toggle_cb_t cb)
{
    s_click_cb = cb;
}
