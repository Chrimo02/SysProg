// Status-Bar (oben, 30 px): WiFi-Status links, State-Text mittig,
// Lautstaerke rechts.
#include <stdio.h>

#include "ui.h"
#include "ui_internal.h"

static lv_obj_t *s_wifi_label;
static lv_obj_t *s_state_label;
static lv_obj_t *s_volume_label;

void ui_status_bar_create(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_size(bar, lv_pct(100), UI_STATUS_BAR_H);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x2A2A2A), 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_hor(bar, 8, 0);
    lv_obj_set_style_pad_ver(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    s_wifi_label = lv_label_create(bar);
    lv_label_set_text(s_wifi_label, LV_SYMBOL_WIFI);
    lv_obj_align(s_wifi_label, LV_ALIGN_LEFT_MID, 0, 0);

    s_state_label = lv_label_create(bar);
    lv_label_set_text(s_state_label, "");
    lv_obj_set_style_text_color(s_state_label, lv_color_hex(0xE0E0E0), 0);
    lv_obj_align(s_state_label, LV_ALIGN_CENTER, 0, 0);

    s_volume_label = lv_label_create(bar);
    lv_label_set_text(s_volume_label, "");
    lv_obj_set_style_text_color(s_volume_label, lv_color_hex(0xE0E0E0), 0);
    lv_obj_align(s_volume_label, LV_ALIGN_RIGHT_MID, 0, 0);

    ui_status_bar_set_wifi_connected(false);
}

void ui_status_bar_set_state_text(const char *text)
{
    lv_label_set_text(s_state_label, text);
}

void ui_status_bar_set_wifi_connected(bool connected)
{
    lv_obj_set_style_text_color(s_wifi_label,
                                lv_color_hex(connected ? 0xE0E0E0 : 0x555555),
                                0);
}

void ui_status_bar_set_volume(int percent)
{
    lv_label_set_text_fmt(s_volume_label, LV_SYMBOL_VOLUME_MAX " %d%%", percent);
}
