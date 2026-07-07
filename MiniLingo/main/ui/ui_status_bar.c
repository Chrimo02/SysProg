// Status-Bar (oben, 30 px): WiFi-Status links, State-Text mittig,
// Lautstaerke rechts. Phase 8b: Klick auf die Volume-Anzeige oeffnet
// einen Slider unter der Bar (Auto-Hide nach 3 s ohne Interaktion).
#include <stdio.h>          // [C] Standardheader (hier nicht direkt genutzt, historisch für Format-Funktionen eingebunden)

#include "ui.h"             // [C] öffentlicher Header (Callback-Typen, Prototypen)
#include "ui_internal.h"    // [C] interner Header (UI_STATUS_BAR_H, Create-Funktion)

#define VOLUME_PANEL_H     44    // [C] Präprozessor-Konstante | [HW] Höhe des ausklappbaren Lautstärke-Panels in Pixeln
#define VOLUME_HIDE_MS     3000  // [Logik] Zeit ohne Interaktion, nach der das Panel automatisch wieder ausgeblendet wird

static lv_obj_t *s_wifi_label;    // [C] static-globale Variable | [HW] Zeiger auf das WiFi-Symbol-Label
static lv_obj_t *s_state_label;   // [HW] Zeiger auf den mittleren Statustext (z.B. Charaktername)
static lv_obj_t *s_volume_label;  // [HW] Zeiger auf die Lautstärkeanzeige rechts

static lv_obj_t *s_volume_panel;         // [HW] Zeiger auf das ausklappbare Panel mit dem Lautstärke-Slider
static lv_obj_t *s_volume_slider;        // [HW] Zeiger auf das Slider-Widget im Panel
static lv_timer_t *s_volume_hide_timer;  // [C] Zeiger auf einen LVGL-Timer, NULL solange keiner läuft | [HW] steuert das automatische Ausblenden des Panels
static ui_volume_changed_cb_t s_volume_cb;  // [C] Funktionszeiger-Variable | [Logik] von main.c registrierter Callback bei Lautstärkeänderung
static int s_volume_percent;             // [Logik] zuletzt gesetzter Lautstärkewert (0-100), Basis für den Slider-Startwert
static ui_wifi_clicked_cb_t s_wifi_cb;   // [Logik] Callback für Klick auf das WiFi-Symbol
static ui_state_clicked_cb_t s_state_cb; // [Logik] Callback für Klick auf den State-Text (Charakterwechsel)

// LVGL-Event-Callback für den Klick auf das WiFi-Symbol: öffnet über
// den registrierten Callback das WLAN-Setup-Portal.
static void wifi_label_click_cb(lv_event_t *e)                 // [C] static Callback, Signatur von LVGL vorgegeben | [HW] e = Event-Objekt (hier ungenutzt)
{
    if (s_wifi_cb) {                                            // [C] NULL-Prüfung vor Aufruf über Funktionszeiger
        s_wifi_cb();                                             // [Logik] leitet Klick an main.c weiter (Setup-Portal öffnen)
    }
}

// LVGL-Event-Callback für den Klick auf den State-Text: löst über den
// registrierten Callback einen Charakterwechsel (Wake-Word + Persona) aus.
static void state_label_click_cb(lv_event_t *e)
{
    if (s_state_cb) {
        s_state_cb();                                            // [Logik] leitet Klick an main.c weiter (Charakter wechseln)
    }
}

// Blendet das Lautstärke-Panel sofort aus und räumt einen laufenden
// Auto-Hide-Timer ab (z.B. wenn der Nutzer erneut auf das Volume-Label
// klickt, während das Panel offen ist).
static void volume_panel_hide(void)
{
    lv_obj_add_flag(s_volume_panel, LV_OBJ_FLAG_HIDDEN);        // [HW] Panel unsichtbar schalten
    if (s_volume_hide_timer) {                                   // [C] NULL-Prüfung: Timer existiert nur, wenn Panel gerade offen war
        lv_timer_delete(s_volume_hide_timer);                    // [HW] löscht den LVGL-Software-Timer, gibt Ressourcen frei
        s_volume_hide_timer = NULL;                              // [C] Zeiger zurücksetzen, verhindert Zugriff auf freigegebenen Timer (dangling pointer vermeiden)
    }
}

// Timer-Callback: wird genau einmal aufgerufen, wenn seit der letzten
// Interaktion VOLUME_HIDE_MS ohne weitere Bedienung vergangen sind, und
// blendet das Panel automatisch aus.
static void volume_hide_timer_cb(lv_timer_t *timer)             // [C] Parametername timer wird hier nicht verwendet
{
    s_volume_hide_timer = NULL;                                  // [Logik] Timer hat sich selbst beendet (repeat_count 1) -> Zeiger als "kein Timer aktiv" markieren
    lv_obj_add_flag(s_volume_panel, LV_OBJ_FLAG_HIDDEN);         // [HW] Panel verstecken
}

// Startet den Auto-Hide-Timer neu bzw. legt ihn beim ersten Aufruf an:
// jede Nutzerinteraktion mit Panel/Slider ruft diese Funktion auf, um
// die Sichtbarkeitsdauer zu verlängern.
static void volume_hide_timer_restart(void)
{
    if (s_volume_hide_timer) {                                   // [Logik] Timer läuft bereits -> nur zurücksetzen statt neu anzulegen
        lv_timer_reset(s_volume_hide_timer);                     // [HW] setzt den Countdown des bestehenden Timers auf den Anfang zurück
    } else {
        s_volume_hide_timer =                                    // [Logik] noch kein Timer aktiv -> neuen anlegen
            lv_timer_create(volume_hide_timer_cb, VOLUME_HIDE_MS, NULL);  // [C] Übergabe eines Funktionszeigers als Timer-Callback | [HW] registriert wiederkehrenden LVGL-Software-Timer
        lv_timer_set_repeat_count(s_volume_hide_timer, 1);       // [Logik] Timer soll nur einmal auslösen (kein Dauer-Intervall)
    }
}

// LVGL-Event-Callback für den Klick auf die Lautstärkeanzeige: öffnet
// das Slider-Panel (falls versteckt) oder schließt es (falls sichtbar).
static void volume_label_click_cb(lv_event_t *e)
{
    if (lv_obj_has_flag(s_volume_panel, LV_OBJ_FLAG_HIDDEN)) {  // [C] Bit-Flag-Abfrage | [Logik] Panel aktuell versteckt? -> öffnen
        lv_slider_set_value(s_volume_slider, s_volume_percent,
                            LV_ANIM_OFF);                        // [HW] Slider-Startposition auf den zuletzt bekannten Lautstärkewert setzen, ohne Animation
        // Das Panel wird VOR dem Chat-Container erstellt und laege
        // sonst unter ihm — der Chat verdeckt es dann nicht nur,
        // sondern faengt auch die Touch-Events ab.
        lv_obj_move_foreground(s_volume_panel);                  // [HW] verschiebt das Panel in der Z-Reihenfolge ganz nach vorn (über den Chat-Container)
        lv_obj_remove_flag(s_volume_panel, LV_OBJ_FLAG_HIDDEN);  // [HW] Panel sichtbar schalten
        volume_hide_timer_restart();                             // [Logik] Auto-Hide-Countdown starten
    } else {
        volume_panel_hide();                                     // [Logik] Panel war offen -> sofort schließen
    }
}

// LVGL-Event-Callback für den Lautstärke-Slider: reagiert sowohl auf
// abgeschlossene Wertänderungen als auch auf laufendes Ziehen, um den
// Auto-Hide-Timer bei jeder Interaktion zu verlängern und den neuen
// Wert nach außen zu melden.
static void volume_slider_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);                 // [C] lv_event_code_t = Enum-Typ für Event-Arten | [HW] fragt ab, welche Art von Event gerade ausgelöst wurde
    if (code != LV_EVENT_VALUE_CHANGED && code != LV_EVENT_PRESSING) {  // [C] logisches UND verknüpft zwei Ungleichheits-Vergleiche | [Logik] andere Event-Typen (z.B. Draw-Events) werden ignoriert
        return;                                                  // [Logik] früher Ausstieg bei nicht relevanten Events
    }
    volume_hide_timer_restart();                                 // [Logik] jede relevante Interaktion verlängert die Sichtbarkeit des Panels
    if (code == LV_EVENT_VALUE_CHANGED && s_volume_cb) {         // [Logik] nur bei abgeschlossener Wertänderung (nicht bei jedem Zwischenschritt) den Callback feuern
        s_volume_cb((int)lv_slider_get_value(s_volume_slider));  // [C] expliziter Cast auf int | [HW] liest aktuellen Slider-Wert aus und meldet ihn dem Aufrufer
    }
}

// Baut die komplette Status-Bar auf: WiFi-Symbol, State-Text und
// Lautstärkeanzeige nebeneinander, sowie das (versteckte)
// Lautstärke-Panel mit Slider darunter. Wird einmalig aus ui_init()
// aufgerufen.
void ui_status_bar_create(lv_obj_t *parent)                      // [C] parent = Eltern-Widget (der Screen)
{
    lv_obj_t *bar = lv_obj_create(parent);                        // [HW] erzeugt das Basis-Objekt der Status-Bar als Kind von parent
    lv_obj_set_pos(bar, 0, 0);                                    // [HW] Position oben links (Ursprung des Screens)
    lv_obj_set_size(bar, lv_pct(100), UI_STATUS_BAR_H);          // [HW] volle Breite, feste Höhe laut Layout-Konstante
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x2A2A2A), 0);   // [HW] dunkelgraue Hintergrundfarbe der Bar
    lv_obj_set_style_radius(bar, 0, 0);                          // [HW] keine abgerundeten Ecken
    lv_obj_set_style_border_width(bar, 0, 0);                    // [HW] kein Rahmen
    lv_obj_set_style_pad_hor(bar, 8, 0);                         // [HW] horizontales Padding, damit Inhalte nicht am Rand kleben
    lv_obj_set_style_pad_ver(bar, 0, 0);                         // [HW] kein vertikales Padding (Bar ist ohnehin niedrig)
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);              // [HW] Bar selbst nicht scrollbar
    // Kleinerer Font als im Chat: die 30-px-Bar muss WiFi-Symbol,
    // State-Text und Volume nebeneinander unterbringen.
    lv_obj_set_style_text_font(bar, &lv_font_montserrat_12, 0);  // [C] Adressoperator liefert Zeiger auf eingebaute Font-Struktur | [HW] setzt die Standard-Schriftgröße für alle Kind-Labels der Bar

    s_wifi_label = lv_label_create(bar);                          // [HW] WiFi-Symbol-Label als Kind der Bar
    lv_label_set_text(s_wifi_label, LV_SYMBOL_WIFI);             // [HW] setzt das eingebaute FontAwesome-WiFi-Symbol als Text
    lv_obj_align(s_wifi_label, LV_ALIGN_LEFT_MID, 0, 0);         // [HW] Ausrichtung linksbündig, vertikal mittig
    // Klick aufs WiFi-Icon öffnet das Setup-Portal (Provisioning).
    lv_obj_add_flag(s_wifi_label, LV_OBJ_FLAG_CLICKABLE);        // [HW] macht das Label klickbar (standardmäßig sind Labels nicht klickbar)
    lv_obj_set_ext_click_area(s_wifi_label, 12);                 // [HW] vergrößert die Trefferfläche über die sichtbare Größe hinaus (bessere Touch-Bedienung)
    lv_obj_add_event_cb(s_wifi_label, wifi_label_click_cb,
                        LV_EVENT_CLICKED, NULL);                 // [C] registriert Funktionszeiger als Event-Handler | [HW] LVGL ruft wifi_label_click_cb bei Klick-Events auf

    s_state_label = lv_label_create(bar);                         // [HW] State-Text-Label als Kind der Bar
    lv_label_set_text(s_state_label, "");                        // [Logik] initial leerer Text, wird später per ui_status_bar_set_state_text gesetzt
    lv_obj_set_style_text_color(s_state_label, lv_color_hex(0xE0E0E0), 0);  // [HW] helle Textfarbe für Lesbarkeit
    // Festes Breitenbudget zwischen WiFi-Symbol und Volume-Anzeige;
    // laengere Texte werden mit "..." gekappt statt zu ueberlappen.
    lv_obj_set_width(s_state_label, 124);                        // [HW] feste Breite, verhindert Überlappung mit den Nachbar-Widgets
    lv_label_set_long_mode(s_state_label, LV_LABEL_LONG_DOT);    // [HW] zu langer Text wird mit "..." abgeschnitten statt umzubrechen
    lv_obj_set_style_text_align(s_state_label, LV_TEXT_ALIGN_CENTER, 0);  // [HW] Text innerhalb des Labels zentriert
    lv_obj_align(s_state_label, LV_ALIGN_CENTER, 0, 0);          // [HW] Label selbst horizontal/vertikal mittig in der Bar
    // Klick auf den State-Text wechselt den Charakter (Mycroft/Alexa/
    // Jarvis) — Wakeword + Persona.
    lv_obj_add_flag(s_state_label, LV_OBJ_FLAG_CLICKABLE);       // [HW] macht das Label klickbar
    lv_obj_set_ext_click_area(s_state_label, 8);                 // [HW] vergrößerte Touch-Trefferfläche
    lv_obj_add_event_cb(s_state_label, state_label_click_cb,
                        LV_EVENT_CLICKED, NULL);                 // [HW] registriert den Klick-Handler für Charakterwechsel

    s_volume_label = lv_label_create(bar);                        // [HW] Lautstärke-Label als Kind der Bar
    lv_label_set_text(s_volume_label, "");                       // [Logik] initial leerer Text, wird später per ui_status_bar_set_volume gesetzt
    lv_obj_set_style_text_color(s_volume_label, lv_color_hex(0xE0E0E0), 0);  // [HW] helle Textfarbe
    lv_obj_align(s_volume_label, LV_ALIGN_RIGHT_MID, 0, 0);      // [HW] Ausrichtung rechtsbündig, vertikal mittig
    // Klick-Ziel fuer den Volume-Slider; das Label ist klein, deshalb
    // grosszuegige Ext-Click-Area (Touch-Treffsicherheit).
    lv_obj_add_flag(s_volume_label, LV_OBJ_FLAG_CLICKABLE);      // [HW] macht das Label klickbar
    lv_obj_set_ext_click_area(s_volume_label, 12);               // [HW] großzügige Touch-Trefferfläche für das kleine Label
    lv_obj_add_event_cb(s_volume_label, volume_label_click_cb,
                        LV_EVENT_CLICKED, NULL);                 // [HW] registriert den Klick-Handler zum Öffnen/Schließen des Volume-Panels

    // Slider-Panel direkt unter der Bar, ueberlagert den Chat;
    // per Default versteckt.
    s_volume_panel = lv_obj_create(parent);                       // [HW] Panel als Kind des Screens (nicht der Bar), damit es über den Chat gelegt werden kann
    lv_obj_set_pos(s_volume_panel, 0, UI_STATUS_BAR_H);          // [HW] Position direkt unterhalb der Status-Bar
    lv_obj_set_size(s_volume_panel, lv_pct(100), VOLUME_PANEL_H); // [HW] volle Breite, feste Höhe laut Konstante
    lv_obj_set_style_bg_color(s_volume_panel, lv_color_hex(0x2A2A2A), 0);  // [HW] gleiche Hintergrundfarbe wie die Status-Bar (optische Einheit)
    lv_obj_set_style_radius(s_volume_panel, 0, 0);               // [HW] keine abgerundeten Ecken
    lv_obj_set_style_border_width(s_volume_panel, 0, 0);         // [HW] kein Rahmen
    lv_obj_set_style_pad_hor(s_volume_panel, 16, 0);             // [HW] horizontales Padding um den Slider herum
    lv_obj_clear_flag(s_volume_panel, LV_OBJ_FLAG_SCROLLABLE);   // [HW] Panel selbst nicht scrollbar
    lv_obj_add_flag(s_volume_panel, LV_OBJ_FLAG_HIDDEN);         // [Logik] Panel startet versteckt, wird erst per Klick eingeblendet

    s_volume_slider = lv_slider_create(s_volume_panel);           // [HW] Slider-Widget als Kind des Panels
    lv_slider_set_range(s_volume_slider, 0, 100);                // [Logik] Wertebereich 0-100 (Prozent)
    lv_obj_set_size(s_volume_slider, lv_pct(100), 8);            // [HW] volle Breite des Panels, schmale Track-Höhe von 8 px
    lv_obj_center(s_volume_slider);                               // [HW] zentriert den Slider im Panel
    // Die 8-px-Track-Hoehe ist fuer den resistiven Touch zu schmal —
    // Klickflaeche nach oben/unten aufweiten.
    lv_obj_set_ext_click_area(s_volume_slider, 14);              // [HW] vergrößert die Touch-Trefferfläche über den sichtbaren Track hinaus
    lv_obj_add_event_cb(s_volume_slider, volume_slider_cb,
                        LV_EVENT_ALL, NULL);                     // [HW] registriert den Slider-Handler für alle Event-Typen (Wertänderung UND Ziehen)

    ui_status_bar_set_wifi_connected(false);                      // [Logik] initialer Anzeigezustand: WiFi als nicht verbunden darstellen
}

// Setzt den mittleren Statustext (z.B. aktiver Charaktername oder
// Systemzustand).
void ui_status_bar_set_state_text(const char *text)
{
    lv_label_set_text(s_state_label, text);                       // [HW] überschreibt den angezeigten Text des State-Labels
}

// Färbt das WiFi-Symbol je nach Verbindungsstatus: hell bei
// bestehender Verbindung, gedimmt/grau bei fehlender Verbindung.
void ui_status_bar_set_wifi_connected(bool connected)
{
    lv_obj_set_style_text_color(s_wifi_label,
                                lv_color_hex(connected ? 0xE0E0E0 : 0x555555),
                                0);                               // [C] ternärer Operator wählt Farbwert je nach Bool-Parameter | [HW] setzt die Textfarbe des WiFi-Symbols
}

// Aktualisiert die Lautstärkeanzeige rechts in der Bar und merkt sich
// den Wert als Startposition für den nächsten Slider-Öffnen-Vorgang.
void ui_status_bar_set_volume(int percent)
{
    s_volume_percent = percent;                                   // [Logik] Wert für spätere Slider-Initialisierung merken
    lv_label_set_text_fmt(s_volume_label, LV_SYMBOL_VOLUME_MAX " %d%%", percent);  // [C] printf-artiges Format (%d) im LVGL-Label | [HW] baut Text aus Lautstärke-Symbol + Prozentzahl zusammen
}

// Registriert den Callback, der bei jeder abgeschlossenen
// Lautstärkeänderung über den Slider aufgerufen wird.
void ui_status_bar_set_volume_changed_cb(ui_volume_changed_cb_t cb)
{
    s_volume_cb = cb;                                             // [Logik] Callback für spätere Verwendung in volume_slider_cb merken
}

// Registriert den Callback für Klicks auf das WiFi-Symbol.
void ui_status_bar_set_wifi_clicked_cb(ui_wifi_clicked_cb_t cb)
{
    s_wifi_cb = cb;                                                // [Logik] Callback für spätere Verwendung in wifi_label_click_cb merken
}

// Registriert den Callback für Klicks auf den State-Text
// (Charakterwechsel).
void ui_status_bar_set_state_clicked_cb(ui_state_clicked_cb_t cb)
{
    s_state_cb = cb;                                               // [Logik] Callback für spätere Verwendung in state_label_click_cb merken
}
