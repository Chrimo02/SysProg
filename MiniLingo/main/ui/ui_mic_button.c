// Mic-Button (unten, 60-px-Bereich). Phase 1: Zustandsfarben + Klick;
// Phase 8: Puls-Animation bei LISTENING, Spinner-Ring bei PROCESSING.
#include "ui.h"
#include "ui_internal.h"

#include "esp_log.h"

static const char *TAG = "ui_mic";

#define MIC_BTN_SIZE   48
// Spinner-Ring um den Button; muss in den 60-px-Bereich passen.
#define SPINNER_SIZE   (MIC_BTN_SIZE + 10)
// Puls: 48 px * 1.15 = 55 px bleibt innerhalb des Mic-Bereichs.
#define PULSE_SCALE    (LV_SCALE_NONE * 115 / 100)
#define PULSE_MS       500

static lv_obj_t *s_btn;
static lv_obj_t *s_spinner;
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

static void pulse_anim_cb(void *var, int32_t v)
{
    lv_obj_set_style_transform_scale(var, v, 0);
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
    // Puls skaliert um die Button-Mitte (Default-Pivot ist oben links).
    lv_obj_set_style_transform_pivot_x(s_btn, MIC_BTN_SIZE / 2, 0);
    lv_obj_set_style_transform_pivot_y(s_btn, MIC_BTN_SIZE / 2, 0);

    // LVGL hat kein Mikrofon-Symbol im Default-Font; LV_SYMBOL_AUDIO
    // dient als Platzhalter.
    lv_obj_t *label = lv_label_create(s_btn);
    lv_label_set_text(label, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(label);

    // Spinner-Ring fuer PROCESSING, per Default versteckt. Nicht
    // klickbar, damit er den Mic-Button nicht verdeckt.
    s_spinner = lv_spinner_create(parent);
    lv_spinner_set_anim_params(s_spinner, 1000, 200);
    lv_obj_set_size(s_spinner, SPINNER_SIZE, SPINNER_SIZE);
    lv_obj_align(s_spinner, LV_ALIGN_BOTTOM_MID, 0,
                 -(UI_MIC_AREA_H - SPINNER_SIZE) / 2);
    lv_obj_set_style_arc_width(s_spinner, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_spinner, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_spinner, lv_color_hex(0x333333),
                               LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_spinner,
                               lv_color_hex(state_colors[UI_MIC_PROCESSING]),
                               LV_PART_INDICATOR);
    lv_obj_remove_flag(s_spinner, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);

    ui_mic_button_set_state(UI_MIC_IDLE);
}

void ui_mic_button_set_state(ui_mic_state_t state)
{
    lv_obj_set_style_bg_color(s_btn, lv_color_hex(state_colors[state]), 0);

    // Puls nur bei LISTENING (Spec Phase 8)
    lv_anim_delete(s_btn, pulse_anim_cb);
    lv_obj_set_style_transform_scale(s_btn, LV_SCALE_NONE, 0);
    if (state == UI_MIC_LISTENING) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_btn);
        lv_anim_set_exec_cb(&a, pulse_anim_cb);
        lv_anim_set_values(&a, LV_SCALE_NONE, PULSE_SCALE);
        lv_anim_set_duration(&a, PULSE_MS);
        lv_anim_set_playback_duration(&a, PULSE_MS);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
    }

    if (state == UI_MIC_PROCESSING) {
        lv_obj_remove_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_mic_button_set_click_cb(ui_mic_click_cb_t cb)
{
    s_click_cb = cb;
}
