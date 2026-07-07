// Chat-Container mit Bubble-Helpern (Spec Section 4):
// User rechts/blau, Assistant links/grau, Local-Response links/gruen.
#include "ui.h"            // [C] öffentlicher Header (Prototyp von ui_chat_add_*)
#include "ui_internal.h"   // [C] interner Header mit Layout-Konstanten (UI_STATUS_BAR_H, UI_CHAT_H)

// Custom-Font mit Latin-1-Range (Phase 8): das eingebaute Montserrat 14
// kann nur ASCII — deutsche Transkripte/Antworten enthalten Umlaute.
// Generiert per lv_font_conv (Kommandozeile im Dateikopf), zusaetzlich
// typografische Anfuehrungszeichen/Gedankenstrich/Ellipse, die Gemini
// gern ausgibt. Nur fuer die Bubble-Labels — Status-Bar und Mic-Button
// brauchen die FontAwesome-Symbole des Default-Fonts.
LV_FONT_DECLARE(font_montserrat_14_latin1);  // [C] Makro erzeugt extern-Deklaration der Font-Datenstruktur | [HW] bindet die generierte LVGL-Font-Ressource ein

#define BUBBLE_MAX_W      200    // [C] Präprozessor-Konstante | [HW] maximale Breite einer Sprechblase in Pixeln
#define BUBBLE_PAD        10     // [HW] Innenabstand (Padding) der Sprechblase in Pixeln
#define BUBBLE_RADIUS     12     // [HW] Eckenradius der Sprechblase (abgerundete Ecken)
#define BUBBLE_GAP        8      // [HW] Abstand zwischen aufeinanderfolgenden Sprechblasen/Zeilen

#define COLOR_USER_BG      0x378ADD  // [Logik] Hintergrundfarbe für Nutzer-Sprechblasen (Blau)
#define COLOR_ASSISTANT_BG 0x3A3A3A  // [Logik] Hintergrundfarbe für Assistant-Sprechblasen (Grau)
#define COLOR_LOCAL_BG     0x2D5A3D  // [Logik] Hintergrundfarbe für lokale Offline-Antworten (Grün)

static lv_obj_t *s_chat;   // [C] static-globale Variable, Datei-lokal | [HW] Zeiger auf den Chat-Container (Eltern-Widget aller Bubble-Zeilen)

// Erstellt den scrollbaren Chat-Container unterhalb der Status-Bar.
// Ordnet neue Sprechblasen per Flex-Layout untereinander an; wird
// einmalig aus ui_init() heraus aufgerufen.
void ui_chat_create(lv_obj_t *parent)                              // [C] Parameter parent = Eltern-Widget (der Screen)
{
    s_chat = lv_obj_create(parent);                                 // [HW] erzeugt neues LVGL-Basis-Objekt als Kind von parent
    lv_obj_set_pos(s_chat, 0, UI_STATUS_BAR_H);                     // [HW] positioniert den Container direkt unter der Status-Bar
    lv_obj_set_size(s_chat, lv_pct(100), UI_CHAT_H);                // [HW] volle Breite (100%), feste Höhe laut Layout-Konstante
    lv_obj_set_style_bg_opa(s_chat, LV_OPA_TRANSP, 0);              // [HW] Hintergrund transparent (Screen-Hintergrund scheint durch)
    lv_obj_set_style_radius(s_chat, 0, 0);                          // [HW] keine abgerundeten Ecken beim Container selbst
    lv_obj_set_style_border_width(s_chat, 0, 0);                    // [HW] keine sichtbare Rahmenlinie
    lv_obj_set_style_pad_all(s_chat, BUBBLE_GAP, 0);                // [HW] Innenabstand rundum, damit Bubbles nicht am Rand kleben
    lv_obj_set_style_pad_row(s_chat, BUBBLE_GAP, 0);                // [HW] Abstand zwischen den einzelnen Flex-Zeilen (Bubbles)
    lv_obj_set_flex_flow(s_chat, LV_FLEX_FLOW_COLUMN);              // [HW] Flex-Layout-Modus: Kinder werden vertikal (spaltenweise) gestapelt
    lv_obj_set_scroll_dir(s_chat, LV_DIR_VER);                      // [HW] Scroll-Richtung auf vertikal beschränkt
    lv_obj_set_scrollbar_mode(s_chat, LV_SCROLLBAR_MODE_AUTO);      // [HW] Scrollbar nur einblenden, wenn Inhalt überläuft
}

// Interner Helfer, der eine einzelne Sprechblase erzeugt: legt eine
// volle-Breite-Zeile an, darin die farbige Bubble mit Textlabel, links
// oder rechts ausgerichtet, und scrollt automatisch dorthin.
static void add_bubble(const char *text, uint32_t bg_color, bool right_aligned)  // [C] static = nur in dieser Datei sichtbar (interner Helfer) | [C] uint32_t = 24-bit-RGB-Farbwert in den unteren Bits
{
    // Volle-Breite-Zeile als Traeger, damit die Bubble darin
    // links/rechts ausgerichtet werden kann (Flex kennt kein align-self).
    lv_obj_t *row = lv_obj_create(s_chat);                          // [HW] neue Flex-Zeile als Kind des Chat-Containers
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);             // [HW] volle Breite, Höhe passt sich automatisch dem Inhalt an
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);                 // [HW] Zeilen-Hintergrund unsichtbar (nur die Bubble soll farbig sein)
    lv_obj_set_style_border_width(row, 0, 0);                       // [HW] kein Rahmen um die Zeile
    lv_obj_set_style_pad_all(row, 0, 0);                            // [HW] kein Innenabstand der Zeile selbst
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);                 // [HW] Zeile selbst nicht scrollbar (nur der äußere Chat-Container scrollt)

    lv_obj_t *bubble = lv_obj_create(row);                          // [HW] die eigentliche farbige Sprechblase als Kind der Zeile
    lv_obj_set_size(bubble, LV_SIZE_CONTENT, LV_SIZE_CONTENT);      // [HW] Größe passt sich automatisch dem Textinhalt an
    lv_obj_set_style_max_width(bubble, BUBBLE_MAX_W, 0);            // [HW] Umbruchgrenze: Bubble wächst nicht breiter als BUBBLE_MAX_W
    lv_obj_set_style_bg_color(bubble, lv_color_hex(bg_color), 0);   // [C] Parameterwert bg_color wird hier verwendet | [HW] setzt die Hintergrundfarbe je nach Bubble-Typ
    lv_obj_set_style_radius(bubble, BUBBLE_RADIUS, 0);              // [HW] abgerundete Ecken für den Sprechblasen-Look
    lv_obj_set_style_border_width(bubble, 0, 0);                    // [HW] kein Rahmen
    lv_obj_set_style_pad_all(bubble, BUBBLE_PAD, 0);                // [HW] Innenabstand zwischen Rand und Text
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);              // [HW] Bubble selbst nicht scrollbar
    lv_obj_align(bubble, right_aligned ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, 0, 0);  // [C] ternärer Operator wählt Ausrichtung je nach Bool-Parameter | [HW] positioniert die Bubble innerhalb der Zeile

    lv_obj_t *label = lv_label_create(bubble);                      // [HW] Textlabel als Kind der Bubble
    lv_label_set_text(label, text);                                 // [C] übernimmt den übergebenen Text-Zeiger | [HW] setzt den anzuzeigenden Text
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);              // [HW] Zeilenumbruch bei zu langem Text statt Abschneiden
    lv_obj_set_width(label, LV_SIZE_CONTENT);                       // [HW] Label-Breite passt sich dem (umgebrochenen) Text an
    lv_obj_set_style_max_width(label, BUBBLE_MAX_W - 2 * BUBBLE_PAD, 0);  // [C] Ausdrucksberechnung zur Compile-Zeit | [HW] Umbruchbreite abzüglich beidseitigem Padding
    lv_obj_set_style_text_color(label, lv_color_hex(0xF0F0F0), 0);  // [HW] fast-weiße Textfarbe für Lesbarkeit auf dunklem Hintergrund
    lv_obj_set_style_text_font(label, &font_montserrat_14_latin1, 0);  // [C] Adressoperator liefert Zeiger auf die Font-Struktur | [HW] verwendet die Latin-1-Font für Umlaut-Unterstützung

    lv_obj_scroll_to_view(row, LV_ANIM_ON);                         // [HW] scrollt den Chat-Container automatisch zur neuen (letzten) Zeile, animiert
}

// Fügt eine rechtsbündige, blaue Sprechblase für eine Nutzeräußerung
// in den Chat-Verlauf ein.
void ui_chat_add_user_bubble(const char *text)                      // [C] öffentliche Wrapper-Funktion um add_bubble
{
    add_bubble(text, COLOR_USER_BG, true);                          // [Logik] rechts ausgerichtet, blaue Farbe -> Nutzer-Sprechblase
}

// Fügt eine linksbündige, graue Sprechblase für eine Assistant-Antwort
// (Cloud/LLM) in den Chat-Verlauf ein.
void ui_chat_add_assistant_bubble(const char *text)
{
    add_bubble(text, COLOR_ASSISTANT_BG, false);                    // [Logik] links ausgerichtet, graue Farbe -> Assistant-Antwort
}

// Fügt eine linksbündige, grüne Sprechblase für eine lokal (offline)
// erzeugte Antwort in den Chat-Verlauf ein.
void ui_chat_add_local_bubble(const char *text)
{
    add_bubble(text, COLOR_LOCAL_BG, false);                        // [Logik] links ausgerichtet, grüne Farbe -> lokale Antwort
}
