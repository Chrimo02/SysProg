// Chat-Container mit Bubble-Helpern (Spec Section 4):
// User rechts/blau, Assistant links/grau, Local-Response links/gruen.
#include "ui.h"
#include "ui_internal.h"

// Custom-Font mit Latin-1-Range (Phase 8): das eingebaute Montserrat 14
// kann nur ASCII — deutsche Transkripte/Antworten enthalten Umlaute.
// Generiert per lv_font_conv (Kommandozeile im Dateikopf), zusaetzlich
// typografische Anfuehrungszeichen/Gedankenstrich/Ellipse, die Gemini
// gern ausgibt. Nur fuer die Bubble-Labels — Status-Bar und Mic-Button
// brauchen die FontAwesome-Symbole des Default-Fonts.
LV_FONT_DECLARE(font_montserrat_14_latin1);

#define BUBBLE_MAX_W      200
#define BUBBLE_PAD        10
#define BUBBLE_RADIUS     12
#define BUBBLE_GAP        8

#define COLOR_USER_BG      0x378ADD
#define COLOR_ASSISTANT_BG 0x3A3A3A
#define COLOR_LOCAL_BG     0x2D5A3D

static lv_obj_t *s_chat;

void ui_chat_create(lv_obj_t *parent)
{
    s_chat = lv_obj_create(parent);
    lv_obj_set_pos(s_chat, 0, UI_STATUS_BAR_H);
    lv_obj_set_size(s_chat, lv_pct(100), UI_CHAT_H);
    lv_obj_set_style_bg_opa(s_chat, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(s_chat, 0, 0);
    lv_obj_set_style_border_width(s_chat, 0, 0);
    lv_obj_set_style_pad_all(s_chat, BUBBLE_GAP, 0);
    lv_obj_set_style_pad_row(s_chat, BUBBLE_GAP, 0);
    lv_obj_set_flex_flow(s_chat, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_chat, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_chat, LV_SCROLLBAR_MODE_AUTO);
}

static void add_bubble(const char *text, uint32_t bg_color, bool right_aligned)
{
    // Volle-Breite-Zeile als Traeger, damit die Bubble darin
    // links/rechts ausgerichtet werden kann (Flex kennt kein align-self).
    lv_obj_t *row = lv_obj_create(s_chat);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bubble = lv_obj_create(row);
    lv_obj_set_size(bubble, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(bubble, BUBBLE_MAX_W, 0);
    lv_obj_set_style_bg_color(bubble, lv_color_hex(bg_color), 0);
    lv_obj_set_style_radius(bubble, BUBBLE_RADIUS, 0);
    lv_obj_set_style_border_width(bubble, 0, 0);
    lv_obj_set_style_pad_all(bubble, BUBBLE_PAD, 0);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(bubble, right_aligned ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *label = lv_label_create(bubble);
    lv_label_set_text(label, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(label, BUBBLE_MAX_W - 2 * BUBBLE_PAD, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xF0F0F0), 0);
    lv_obj_set_style_text_font(label, &font_montserrat_14_latin1, 0);

    lv_obj_scroll_to_view(row, LV_ANIM_ON);
}

void ui_chat_add_user_bubble(const char *text)
{
    add_bubble(text, COLOR_USER_BG, true);
}

void ui_chat_add_assistant_bubble(const char *text)
{
    add_bubble(text, COLOR_ASSISTANT_BG, false);
}

void ui_chat_add_local_bubble(const char *text)
{
    add_bubble(text, COLOR_LOCAL_BG, false);
}
