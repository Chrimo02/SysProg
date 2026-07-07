// Mic-Button (unten, 60-px-Bereich). Phase 1: Zustandsfarben + Klick;
// Phase 8: Puls-Animation bei LISTENING, Spinner-Ring bei PROCESSING.
#include "ui.h"            // [C] öffentlicher Header (ui_mic_state_t, Callback-Typ, Prototypen)
#include "ui_internal.h"   // [C] interner Header (UI_MIC_AREA_H, Create-Funktion)

#include "esp_log.h"        // [Logik] ESP_LOGI-Logging über UART-Konsole

static const char *TAG = "ui_mic";  // [C] static = Datei-lokale Linkage | [Logik] Log-Tag für diese Datei

#define MIC_BTN_SIZE   48    // [C] Präprozessor-Konstante | [HW] Durchmesser/Kantenlänge des runden Mic-Buttons in Pixeln
// Spinner-Ring um den Button; muss in den 60-px-Bereich passen.
#define SPINNER_SIZE   (MIC_BTN_SIZE + 10)   // [HW] Spinner etwas größer als der Button, damit er ihn umringt
// Puls: 48 px * 1.15 = 55 px bleibt innerhalb des Mic-Bereichs.
#define PULSE_SCALE    (LV_SCALE_NONE * 115 / 100)  // [C] Ganzzahl-Arithmetik zur Compile-Zeit | [HW] Skalierungsfaktor 115% für die Puls-Animation
#define PULSE_MS       500   // [HW] Dauer einer Puls-Halbphase (Hin- oder Rückweg) in Millisekunden

static lv_obj_t *s_btn;              // [C] static-globale Variable | [HW] Zeiger auf das Mic-Button-Widget
static lv_obj_t *s_spinner;          // [HW] Zeiger auf den Spinner-Ring (Ladeanimation bei PROCESSING)
static ui_mic_click_cb_t s_click_cb; // [C] Funktionszeiger-Variable | [Logik] hier registrierter Callback, den main.c setzt

// Zuordnung Zustand -> Farbe (Spec Section 4): Array-Index über die
// enum-Werte, damit ui_mic_button_set_state() die passende Farbe direkt
// nachschlagen kann.
static const uint32_t state_colors[] = {                     // [C] Array mit designated initializers (Index über enum-Konstante statt Position)
    [UI_MIC_IDLE]           = 0x888888,   // [Logik] Grau: wartet auf Wake-Word
    [UI_MIC_LISTENING]      = 0xE24B4A,   // [Logik] Rot: Nutzeräußerung wird aufgenommen
    [UI_MIC_LOCAL_RESPONSE] = 0x1D9E75,   // [Logik] Grün: lokale Antwort
    [UI_MIC_PROCESSING]     = 0xEF9F27,   // [Logik] Orange: Cloud-Verarbeitung läuft
    [UI_MIC_SPEAKING]       = 0x1D9E75,   // [Logik] Grün: Sprachausgabe läuft
};

// LVGL-Event-Callback für den Klick auf den Mic-Button: loggt den Klick
// und reicht ihn an den registrierten externen Callback weiter (z.B.
// startet main.c damit die Voice-Pipeline manuell).
static void mic_btn_event_cb(lv_event_t *e)                  // [C] static Callback-Funktion, Signatur von LVGL vorgegeben (Funktionszeiger-Ziel) | [HW] e = LVGL-Event-Objekt (Typ/Ziel des Events)
{
    ESP_LOGI(TAG, "Mic button pressed");                      // [Logik] Log-Ausgabe zur Nachverfolgung von Klicks
    if (s_click_cb) {                                         // [C] Prüfung auf NULL-Zeiger vor Aufruf (Callback evtl. noch nicht registriert)
        s_click_cb();                                         // [C] Aufruf über Funktionszeiger | [Logik] leitet den Klick an den Anwendungscode weiter
    }
}

// Animations-Callback für die Puls-Animation: wird von LVGL pro Frame
// mit dem interpolierten Skalierungswert aufgerufen und setzt ihn als
// Transform-Scale-Style auf den Button.
static void pulse_anim_cb(void *var, int32_t v)               // [C] void* generischer Zeiger (LVGL-Animations-API kennt den konkreten Typ nicht) | [C] int32_t v = aktueller interpolierter Wert
{
    lv_obj_set_style_transform_scale(var, v, 0);               // [C] Cast von void* auf lv_obj_t* passiert implizit | [HW] skaliert das Widget visuell (Puls-Effekt)
}

// Erstellt den runden Mic-Button unten mittig im 60-px-Bereich samt
// Audio-Symbol-Label und dem (zunächst versteckten) Spinner-Ring für
// den PROCESSING-Zustand. Wird einmalig aus ui_init() aufgerufen.
void ui_mic_button_create(lv_obj_t *parent)                   // [C] parent = Eltern-Widget, an das der Button gehängt wird
{
    s_btn = lv_button_create(parent);                          // [HW] erzeugt ein LVGL-Button-Widget als Kind von parent
    lv_obj_set_size(s_btn, MIC_BTN_SIZE, MIC_BTN_SIZE);        // [HW] quadratische Größe (später durch Radius rund)
    lv_obj_set_style_radius(s_btn, LV_RADIUS_CIRCLE, 0);       // [HW] maximaler Radius -> aus dem Quadrat wird ein Kreis
    // Mittig im unteren 60-px-Bereich (y = 260..320)
    lv_obj_align(s_btn, LV_ALIGN_BOTTOM_MID, 0,                // [HW] Ausrichtung unten mittig relativ zum Elternobjekt
                 -(UI_MIC_AREA_H - MIC_BTN_SIZE) / 2);         // [C] Ganzzahl-Arithmetik berechnet den Y-Offset zur vertikalen Zentrierung im Mic-Bereich
    lv_obj_add_event_cb(s_btn, mic_btn_event_cb, LV_EVENT_CLICKED, NULL);  // [C] Registrierung eines Funktionszeigers als Event-Handler | [HW] LVGL ruft mic_btn_event_cb bei Klick-Events auf
    // Puls skaliert um die Button-Mitte (Default-Pivot ist oben links).
    lv_obj_set_style_transform_pivot_x(s_btn, MIC_BTN_SIZE / 2, 0);  // [HW] setzt den Skalierungs-Drehpunkt horizontal auf die Button-Mitte
    lv_obj_set_style_transform_pivot_y(s_btn, MIC_BTN_SIZE / 2, 0);  // [HW] setzt den Skalierungs-Drehpunkt vertikal auf die Button-Mitte

    // LVGL hat kein Mikrofon-Symbol im Default-Font; LV_SYMBOL_AUDIO
    // dient als Platzhalter.
    lv_obj_t *label = lv_label_create(s_btn);                  // [HW] Textlabel (Icon-Symbol) als Kind des Buttons
    lv_label_set_text(label, LV_SYMBOL_AUDIO);                 // [HW] setzt das eingebaute FontAwesome-Audio-Symbol als Text
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);  // [HW] weiße Symbolfarbe für Kontrast auf dem farbigen Button
    lv_obj_center(label);                                      // [HW] zentriert das Symbol exakt im Button

    // Spinner-Ring fuer PROCESSING, per Default versteckt. Nicht
    // klickbar, damit er den Mic-Button nicht verdeckt.
    s_spinner = lv_spinner_create(parent);                     // [HW] erzeugt LVGL-Spinner-Widget (rotierender Ladeindikator) als Geschwister des Buttons
    lv_spinner_set_anim_params(s_spinner, 1000, 200);          // [HW] Rotationsdauer 1000 ms, Bogenlänge 200° des Spinner-Arcs
    lv_obj_set_size(s_spinner, SPINNER_SIZE, SPINNER_SIZE);    // [HW] Größe des Spinner-Rings (etwas größer als der Button)
    lv_obj_align(s_spinner, LV_ALIGN_BOTTOM_MID, 0,            // [HW] gleiche Ausrichtung wie der Mic-Button (überlagert ihn konzentrisch)
                 -(UI_MIC_AREA_H - SPINNER_SIZE) / 2);         // [C] analoge Zentrierungsberechnung wie beim Button, mit SPINNER_SIZE
    lv_obj_set_style_arc_width(s_spinner, 3, LV_PART_MAIN);    // [HW] Strichbreite des Hintergrund-Bogens (Spinner-Track)
    lv_obj_set_style_arc_width(s_spinner, 3, LV_PART_INDICATOR);  // [HW] Strichbreite des rotierenden Vordergrund-Bogens
    lv_obj_set_style_arc_color(s_spinner, lv_color_hex(0x333333),
                               LV_PART_MAIN);                  // [HW] Farbe des statischen Hintergrund-Bogens (dunkelgrau)
    lv_obj_set_style_arc_color(s_spinner,
                               lv_color_hex(state_colors[UI_MIC_PROCESSING]),
                               LV_PART_INDICATOR);             // [C] Array-Zugriff per enum-Index | [HW] Farbe des rotierenden Indikator-Bogens (PROCESSING-Orange)
    lv_obj_remove_flag(s_spinner, LV_OBJ_FLAG_CLICKABLE);      // [HW] Spinner ist nicht klickbar, blockiert also keine Touch-Events auf dem Mic-Button
    lv_obj_add_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);            // [HW] Spinner initial unsichtbar (nur bei PROCESSING eingeblendet)

    ui_mic_button_set_state(UI_MIC_IDLE);                      // [Logik] initialer Button-Zustand: IDLE (wartet auf Wake-Word)
}

// Setzt den sichtbaren Zustand des Mic-Buttons: aktualisiert die
// Hintergrundfarbe passend zur Voice-Pipeline-Phase, startet/stoppt die
// Puls-Animation (nur bei LISTENING) und blendet den Spinner-Ring
// (nur bei PROCESSING) ein oder aus.
void ui_mic_button_set_state(ui_mic_state_t state)             // [C] state = Aufzählungswert, steuert den gesamten Funktionsablauf
{
    lv_obj_set_style_bg_color(s_btn, lv_color_hex(state_colors[state]), 0);  // [C] Array-Zugriff mit Enum als Index | [HW] Button-Hintergrundfarbe je nach Zustand

    // Puls nur bei LISTENING (Spec Phase 8)
    lv_anim_delete(s_btn, pulse_anim_cb);                      // [HW] entfernt eine evtl. laufende Puls-Animation auf dem Button (verhindert Doppel-Animationen)
    lv_obj_set_style_transform_scale(s_btn, LV_SCALE_NONE, 0); // [HW] setzt Skalierung zurück auf 100% (Normalgröße), falls kein Puls aktiv ist
    if (state == UI_MIC_LISTENING) {                           // [C] Vergleich mit Enum-Konstante | [Logik] Puls-Animation nur im LISTENING-Zustand starten
        lv_anim_t a;                                           // [C] lokale Struct-Variable (Stack) für die Animationskonfiguration
        lv_anim_init(&a);                                      // [C] Adressoperator übergibt Zeiger zum Initialisieren | [HW] setzt Default-Werte der Animations-Struct
        lv_anim_set_var(&a, s_btn);                             // [HW] verknüpft die Animation mit dem Mic-Button als Zielobjekt
        lv_anim_set_exec_cb(&a, pulse_anim_cb);                // [C] Übergabe eines Funktionszeigers als Ausführungs-Callback | [HW] pro Frame aufgerufene Funktion, die den Skalierungswert anwendet
        lv_anim_set_values(&a, LV_SCALE_NONE, PULSE_SCALE);    // [HW] Animationsbereich: von 100% bis PULSE_SCALE (115%)
        lv_anim_set_duration(&a, PULSE_MS);                    // [HW] Dauer der Hinbewegung (500 ms)
        lv_anim_set_playback_duration(&a, PULSE_MS);           // [HW] Dauer der Rückbewegung (Playback), erzeugt Puls-Effekt
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE); // [Logik] wiederholt endlos, solange der Zustand LISTENING bleibt
        lv_anim_start(&a);                                     // [HW] startet die Animation im LVGL-Animationssystem
    }

    if (state == UI_MIC_PROCESSING) {                          // [Logik] Spinner nur während der Cloud-Verarbeitung anzeigen
        lv_obj_remove_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);     // [HW] Spinner sichtbar machen
    } else {
        lv_obj_add_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);        // [HW] Spinner in allen anderen Zuständen verstecken
    }
}

// Registriert den Callback, der bei jedem Klick auf den Mic-Button
// aufgerufen wird (Aufrufer ist typischerweise main.c).
void ui_mic_button_set_click_cb(ui_mic_click_cb_t cb)          // [C] cb = übergebener Funktionszeiger
{
    s_click_cb = cb;                                           // [Logik] Callback für spätere Verwendung in mic_btn_event_cb merken
}
