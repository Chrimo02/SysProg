// Status-Bar (oben, 30 px): WiFi-Status links, State-Text mittig,
// Lautstaerke rechts. Phase 8b: Klick auf die Volume-Anzeige oeffnet
// einen Slider unter der Bar (Auto-Hide nach 3 s ohne Interaktion).
#include <stdio.h>

#include "ui.h"
#include "ui_internal.h"

#define VOLUME_PANEL_H     44
#define VOLUME_HIDE_MS     3000

static lv_obj_t *s_wifi_label;
static lv_obj_t *s_state_label;
static lv_obj_t *s_volume_label;

static lv_obj_t *s_volume_panel;
static lv_obj_t *s_volume_slider;
static lv_timer_t *s_volume_hide_timer;
static ui_volume_changed_cb_t s_volume_cb;
static int s_volume_percent;

static void volume_panel_hide(void)
{
    lv_obj_add_flag(s_volume_panel, LV_OBJ_FLAG_HIDDEN);
    if (s_volume_hide_timer) {
        lv_timer_delete(s_volume_hide_timer);
        s_volume_hide_timer = NULL;
    }
}

static void volume_hide_timer_cb(lv_timer_t *timer)
{
    s_volume_hide_timer = NULL;
    lv_obj_add_flag(s_volume_panel, LV_OBJ_FLAG_HIDDEN);
}

static void volume_hide_timer_restart(void)
{
    if (s_volume_hide_timer) {
        lv_timer_reset(s_volume_hide_timer);
    } else {
        s_volume_hide_timer =
            lv_timer_create(volume_hide_timer_cb, VOLUME_HIDE_MS, NULL);
        lv_timer_set_repeat_count(s_volume_hide_timer, 1);
    }
}

static void volume_label_click_cb(lv_event_t *e)
{
    if (lv_obj_has_flag(s_volume_panel, LV_OBJ_FLAG_HIDDEN)) {
        lv_slider_set_value(s_volume_slider, s_volume_percent,
                            LV_ANIM_OFF);
        lv_obj_remove_flag(s_volume_panel, LV_OBJ_FLAG_HIDDEN);
        volume_hide_timer_restart();
    } else {
        volume_panel_hide();
    }
}

static void volume_slider_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_VALUE_CHANGED && code != LV_EVENT_PRESSING) {
        return;
    }
    volume_hide_timer_restart();
    if (code == LV_EVENT_VALUE_CHANGED && s_volume_cb) {
        s_volume_cb((int)lv_slider_get_value(s_volume_slider));
    }
}

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
    // Kleinerer Font als im Chat: die 30-px-Bar muss WiFi-Symbol,
    // State-Text und Volume nebeneinander unterbringen.
    lv_obj_set_style_text_font(bar, &lv_font_montserrat_12, 0);

    s_wifi_label = lv_label_create(bar);
    lv_label_set_text(s_wifi_label, LV_SYMBOL_WIFI);
    lv_obj_align(s_wifi_label, LV_ALIGN_LEFT_MID, 0, 0);

    s_state_label = lv_label_create(bar);
    lv_label_set_text(s_state_label, "");
    lv_obj_set_style_text_color(s_state_label, lv_color_hex(0xE0E0E0), 0);
    // Festes Breitenbudget zwischen WiFi-Symbol und Volume-Anzeige;
    // laengere Texte werden mit "..." gekappt statt zu ueberlappen.
    lv_obj_set_width(s_state_label, 124);
    lv_label_set_long_mode(s_state_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_state_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_state_label, LV_ALIGN_CENTER, 0, 0);

    s_volume_label = lv_label_create(bar);
    lv_label_set_text(s_volume_label, "");
    lv_obj_set_style_text_color(s_volume_label, lv_color_hex(0xE0E0E0), 0);
    lv_obj_align(s_volume_label, LV_ALIGN_RIGHT_MID, 0, 0);
    // Klick-Ziel fuer den Volume-Slider; das Label ist klein, deshalb
    // grosszuegige Ext-Click-Area (Touch-Treffsicherheit).
    lv_obj_add_flag(s_volume_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_volume_label, 12);
    lv_obj_add_event_cb(s_volume_label, volume_label_click_cb,
                        LV_EVENT_CLICKED, NULL);

    // Slider-Panel direkt unter der Bar, ueberlagert den Chat;
    // per Default versteckt.
    s_volume_panel = lv_obj_create(parent);
    lv_obj_set_pos(s_volume_panel, 0, UI_STATUS_BAR_H);
    lv_obj_set_size(s_volume_panel, lv_pct(100), VOLUME_PANEL_H);
    lv_obj_set_style_bg_color(s_volume_panel, lv_color_hex(0x2A2A2A), 0);
    lv_obj_set_style_radius(s_volume_panel, 0, 0);
    lv_obj_set_style_border_width(s_volume_panel, 0, 0);
    lv_obj_set_style_pad_hor(s_volume_panel, 16, 0);
    lv_obj_clear_flag(s_volume_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_volume_panel, LV_OBJ_FLAG_HIDDEN);

    s_volume_slider = lv_slider_create(s_volume_panel);
    lv_slider_set_range(s_volume_slider, 0, 100);
    lv_obj_set_size(s_volume_slider, lv_pct(100), 8);
    lv_obj_center(s_volume_slider);
    lv_obj_add_event_cb(s_volume_slider, volume_slider_cb,
                        LV_EVENT_ALL, NULL);

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
    s_volume_percent = percent;
    lv_label_set_text_fmt(s_volume_label, LV_SYMBOL_VOLUME_MAX " %d%%", percent);
}

void ui_status_bar_set_volume_changed_cb(ui_volume_changed_cb_t cb)
{
    s_volume_cb = cb;
}
