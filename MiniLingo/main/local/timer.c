// Kitchen-Timer (Spec Section 7): ein One-Shot-esp_timer, die Restzeit
// rechnet sich aus der gespeicherten Deadline — so braucht der
// 1-Hz-Countdown in der Status-Bar keinen eigenen Tick-Timer.
#include <stdatomic.h>              // [C] Header-Include | [C] atomic_llong/atomic_exchange etc. für lock-freien, thread-sicheren Zugriff

#include "esp_log.h"                // [Logik] ESP_LOGI-Logging über UART-Konsole
#include "esp_timer.h"              // [HW] ESP-IDF High-Resolution-Timer-API (esp_timer_create/start_once/get_time)

#include "canned_audio.h"           // [C] eigener Header: deklariert canned_audio_play() für die Sprachausgabe bei Timer-Ende
#include "timer.h"                  // [C] eigener Header: Public API dieser Datei (Prototyp-Abgleich)

static const char *TAG = "kitchen_timer";  // [C] static = Datei-lokale Linkage | [Logik] Log-Tag zur Zuordnung der Ausgaben

static esp_timer_handle_t s_timer;           // [C] static-globale Variable | [HW] opaker Handle auf den esp_timer (Peripherie-Abstraktion)
static kitchen_timer_done_cb_t s_done_cb;    // [C] Funktionszeiger-Variable | [Logik] registrierter Callback, der bei Ablauf aufgerufen wird
static atomic_llong s_deadline_us;  // 0 = kein Timer aktiv   // [C] atomarer 64-bit-Zeitstempel | [Logik] absoluter Zielzeitpunkt in Mikrosekunden; 0 = inaktiv

// Wird vom esp_timer-Task aufgerufen, sobald die Deadline erreicht ist:
// setzt den Zustand zurueck, spielt Timer-Ende + Alarm-Sound ab und
// ruft den registrierten Callback (z.B. fuer LVGL-Signalisierung per
// Flag/Queue).
static void timer_done(void *arg)
{
    atomic_store(&s_deadline_us, 0);          // [C] atomarer Schreibzugriff | [Logik] Timer als "nicht mehr aktiv" markieren
    ESP_LOGI(TAG, "timer done");               // [Logik] Ablauf protokollieren
    canned_audio_play(CANNED_TIMER_DONE);     // [Logik] vorab generierte Sprachausgabe "Timer fertig" abspielen
    canned_audio_play(CANNED_ALARM);          // [Logik] anschließend Alarmton abspielen (Queue serialisiert beide)
    if (s_done_cb) {                          // [C] Prüfung auf NULL-Funktionszeiger | [Logik] nur aufrufen, wenn ein Callback registriert wurde
        s_done_cb();                          // [C] Aufruf über Funktionszeiger | [Logik] z.B. Statusbar/LVGL-Update anstoßen
    }
}

// Legt den zugrundeliegenden esp_timer an und merkt sich den
// Fertig-Callback. Muss einmalig vor kitchen_timer_start() laufen.
void kitchen_timer_init(kitchen_timer_done_cb_t done_cb)
{
    s_done_cb = done_cb;                      // [Logik] übergebenen Callback für spätere Ablauf-Benachrichtigung merken
    const esp_timer_create_args_t args = {    // [C] const Struct-Initialisierung (Designated Initializer) | [HW] esp_timer-Erzeugungsparameter
        .callback = timer_done,               // [C] Feldzuweisung per Name | [HW] Funktion, die die Peripherie beim Ablauf aufruft
        .name = "kitchen_timer",              // [Logik] Name für Debug-/Trace-Zwecke
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_timer));  // [C] Makro prüft Rückgabewert und bricht bei Fehler hart ab | [HW] legt den esp_timer-Handle in der Peripherie an
}

// Startet den Timer neu (bzw. ersetzt einen laufenden): berechnet die
// absolute Deadline aus der aktuellen Zeit + Sekunden und startet den
// darunterliegenden One-Shot-esp_timer.
void kitchen_timer_start(int seconds)
{
    esp_timer_stop(s_timer);  // ESP_ERR_INVALID_STATE wenn nicht aktiv: ok   // [HW] evtl. laufenden Timer zuerst stoppen, bevor er neu gestartet wird | [Logik] Fehlerfall (war nicht aktiv) ist hier bewusst ignoriert
    atomic_store(&s_deadline_us,                                  // [C] atomarer Schreibzugriff
                 esp_timer_get_time() + (int64_t)seconds * 1000000);  // [C] Cast auf int64_t verhindert Ganzzahlüberlauf bei der Multiplikation | [HW] aktuelle Systemzeit (µs) + Sekunden in µs = absolute Deadline
    ESP_ERROR_CHECK(esp_timer_start_once(s_timer,                 // [HW] startet den Timer für genau einen Ablauf in der Zukunft
                                         (uint64_t)seconds * 1000000));  // [C] Cast auf uint64_t | [HW] Sekunden in Mikrosekunden umgerechnet
    ESP_LOGI(TAG, "timer started: %d s", seconds);                // [Logik] Start protokollieren
}

// Bricht einen laufenden Timer ab. Gibt zurueck, ob tatsaechlich einer
// aktiv war (fuer die Sprachrueckmeldung "Timer abgebrochen" vs.
// "kein Timer aktiv").
bool kitchen_timer_cancel(void)
{
    if (atomic_exchange(&s_deadline_us, 0) == 0) {  // [C] atomic_exchange liest alten Wert und schreibt 0 in einem atomaren Schritt | [Logik] war vorher schon 0 (kein Timer aktiv)?
        return false;                                // [Logik] nichts abzubrechen, Aufrufer soll "kein Timer" melden
    }
    esp_timer_stop(s_timer);                         // [HW] Peripherie-Timer tatsächlich anhalten
    ESP_LOGI(TAG, "timer cancelled");                // [Logik] Abbruch protokollieren
    return true;                                      // [Logik] Aufrufer soll Abbruch-Bestätigung ausgeben
}

// Berechnet die verbleibende Zeit bis zur Deadline in ganzen (aufgerundeten)
// Sekunden, ohne selbst einen Tick-Timer zu benötigen — kann daher
// beliebig oft aus dem LVGL-Task für die Status-Bar abgefragt werden.
int kitchen_timer_remaining_s(void)
{
    long long deadline = atomic_load(&s_deadline_us);  // [C] atomarer Lesezugriff | [Logik] aktuell gültige Deadline holen
    if (deadline == 0) {                                // [Logik] kein Timer aktiv
        return -1;                                       // [Logik] Sentinel-Wert für "kein Timer"
    }
    long long left_us = deadline - esp_timer_get_time();  // [HW] Differenz aus Deadline und aktueller Systemzeit in µs
    if (left_us < 0) {                                    // [Logik] Deadline evtl. bereits (knapp) überschritten, bevor der Callback lief
        left_us = 0;                                       // [Logik] negative Restzeit auf 0 klemmen
    }
    return (int)((left_us + 999999) / 1000000);           // [C] Ganzzahl-Rundungstrick (+999999 vor Division) für Aufrunden | [Logik] µs in ganze Sekunden umrechnen, aufgerundet
}
