// Mic-Button (unten, 60-px-Bereich). Phase 1: Zustandsfarben + Klick-Log;
// Animationen (Puls, Spinner, Sinus-Welle) folgen in Phase 8.
#include "ui.h"
#include "ui_internal.h"

#include "esp_log.h"

static const char *TAG = "ui_mic";

#define MIC_BTN_SIZE 48

static lv_obj_t *s_btn;
static ui_mic_click_cb_t s_click_cb;

static const uint32_t state_colors[] = {
    [UI_MIC_IDLE]           = 0x888888,
    [UI_MIC_LISTENING]      = 0xE24B4A,
    [UI_MIC_LOCAL_RESPONSE] = 0x1D9E75,
    [UI_MIC_PROCESSING]     = 0xEF9F27,
    [UI_MIC_SPEAKING]       = 0x1D9E75,
};

static void mic_btn_event_cb(lv_event_t *e)
{
    ESP_LOGI(TAG, "Mic button pressed");
    if (s_click_cb) {
        s_click_cb();
    }
}

void ui_mic_button_create(lv_obj_t *parent)
{
    s_btn = lv_button_create(parent);
    lv_obj_set_size(s_btn, MIC_BTN_SIZE, MIC_BTN_SIZE);
    lv_obj_set_style_radius(s_btn, LV_RADIUS_CIRCLE, 0);
    // Mittig im unteren 60-px-Bereich (y = 260..320)
    lv_obj_align(s_btn, LV_ALIGN_BOTTOM_MID, 0,
                 -(UI_MIC_AREA_H - MIC_BTN_SIZE) / 2);
    lv_obj_add_event_cb(s_btn, mic_btn_event_cb, LV_EVENT_CLICKED, NULL);

    // LVGL hat kein Mikrofon-Symbol im Default-Font; LV_SYMBOL_AUDIO
    // dient als Platzhalter.
    lv_obj_t *label = lv_label_create(s_btn);
    lv_label_set_text(label, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(label);

    ui_mic_button_set_state(UI_MIC_IDLE);
}

void ui_mic_button_set_state(ui_mic_state_t state)
{
    lv_obj_set_style_bg_color(s_btn, lv_color_hex(state_colors[state]), 0);
}

void ui_mic_button_set_click_cb(ui_mic_click_cb_t cb)
{
    s_click_cb = cb;
}
