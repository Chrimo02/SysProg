// Wake-Word-Handling (Spec Section 6). In Phase 2 nur Log + Listener;
// die State-Machine (LISTENING etc.) kommt in Phase 3/4.
#include "esp_log.h"                                        // [Logik] ESP_LOGI-Logging über UART-Konsole

#include "wake_word.h"                                       // [C] eigener Header: Typdeklarationen/Prototyp-Abgleich

static const char *TAG = "wake_word";                        // [C] static = datei-lokale Linkage | [Logik] Log-Tag zur Zuordnung der Ausgaben

static wake_word_listener_t s_listener;                      // [C] static-globale Variable eines Funktionszeiger-Typs; NULL bis Registrierung

// Registriert den Callback, der bei Wake-Word- oder Touch-Events
// aufgerufen wird. Ersetzt einen evtl. vorher gesetzten Listener.
void wake_word_set_listener(wake_word_listener_t listener)   // [C] Parameter ist selbst ein Funktionszeiger (Callback-Registrierung)
{
    s_listener = listener;                                   // [Logik] Callback für spätere Events merken
}

// Wird vom afe_fetch_task aufgerufen, sobald WakeNet ein Wake Word
// erkannt hat; loggt den Treffer und benachrichtigt den Listener
// (i.d.R. main.c, das daraufhin die Listening-UI/State-Machine startet).
void wake_word_on_detected(int word_index)                  // [C] int-Parameter identifiziert das erkannte Wakeword
{
    ESP_LOGI(TAG, "Wake word detected (index %d)", word_index);  // [C] %d Format-Spezifizierer für Dezimalzahl | [Logik] Diagnose-Log für erkanntes Wake Word
    if (s_listener) {                                        // [C] Prüfung auf Nicht-NULL vor Aufruf des Funktionszeigers
        s_listener();                                        // [C] Aufruf über Funktionszeiger (indirekter Call) | [Logik] informiert den Rest der App über das Event
    }
}

// Wird beim Druecken des Mic-Buttons aufgerufen (Touch-Trigger statt
// Sprach-Erkennung); nutzt denselben Listener wie ein echtes Wake Word,
// damit main.c beide Auslöser identisch behandeln kann.
void wake_word_on_touch(void)                                // [C] keine Parameter (void), reines Event ohne Zusatzdaten
{
    ESP_LOGI(TAG, "Listening via touch (mic button)");       // [Logik] Diagnose-Log für den Touch-Auslöser
    if (s_listener) {                                        // [C] NULL-Check vor Callback-Aufruf
        s_listener();                                        // [C] indirekter Aufruf über Funktionszeiger | [Logik] gleiches Event wie Wake-Word-Detection
    }
}
