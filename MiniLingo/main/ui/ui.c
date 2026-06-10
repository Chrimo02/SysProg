// LVGL-Screen-Setup: setzt den Hintergrund und baut die drei
// Layout-Bereiche (Status-Bar, Chat, Mic-Button) auf.
#include "ui.h"
#include "ui_internal.h"

void ui_init(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    ui_status_bar_create(scr);
    ui_chat_create(scr);
    ui_mic_button_create(scr);
}
