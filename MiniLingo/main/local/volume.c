// Output-Gain-Management (Spec Section 12, Phase 4): Prozent-Stufen
// 0/20/40/60/80/100, linear auf Q15 gemappt und an audio_out gereicht.
#include <stdatomic.h>              // [C] Header-Include | [C] stellt atomic_int/atomic_load/atomic_store bereit (lock-freie, thread-sichere Ganzzahlen)

#include "esp_log.h"                // [Logik] ESP_LOGI-Logging über UART-Konsole

#include "drivers/audio_out.h"      // [HW] deklariert audio_out_set_gain_q15/AUDIO_OUT_GAIN_UNITY (Zugriff auf die eigentliche Ausgabe-Hardware/den DAC-Gain)
#include "volume.h"                 // [C] eigener Header: deklariert die Public API dieser Datei (Prototyp-Abgleich)

static const char *TAG = "volume";  // [C] static = Datei-lokale Linkage | [Logik] Log-Tag zur Zuordnung der Ausgaben

static atomic_int s_percent;        // [C] static-globale, atomare Variable | [Logik] aktuell aktive Lautstärke in Prozent (0..100)
static atomic_int s_before_mute;    // [Logik] zwischengespeicherter Wert vor volume_mute(), für volume_unmute()

// Wendet einen Prozentwert tatsächlich an: speichert ihn atomar und
// rechnet ihn linear in den Q15-Gain-Multiplikator der Audio-Ausgabe um.
static void apply(int percent)
{
    atomic_store(&s_percent, percent);                              // [C] atomarer Schreibzugriff | [Logik] neuen Prozentwert als aktuellen Zustand übernehmen
    audio_out_set_gain_q15(AUDIO_OUT_GAIN_UNITY * percent / 100);   // [C] Ganzzahl-Multiplikation/-Division | [HW] linear skalierter Q15-Fixpoint-Gain (100 % = AUDIO_OUT_GAIN_UNITY) an den Audio-Treiber übergeben
    ESP_LOGI(TAG, "volume %d%%", percent);                          // [Logik] Lautstärkeänderung protokollieren
}

// Initialisiert die Lautstärkeverwaltung beim Systemstart: setzt den
// Startwert und merkt ihn zugleich als "vor Mute"-Wert vor.
void volume_init(int initial_percent)
{
    atomic_store(&s_before_mute, initial_percent);  // [Logik] Startwert gleich als Mute-Rückfallwert hinterlegen
    apply(initial_percent);                          // [Logik] Startlautstärke tatsächlich auf die Hardware anwenden
}

// Liefert die aktuell aktive Lautstärke in Prozent; darf aus jedem
// Task gelesen werden (atomarer Zugriff, kein Lock nötig).
int volume_get(void)
{
    return atomic_load(&s_percent);  // [C] atomarer Lesezugriff | [Logik] gibt den zuletzt gesetzten Prozentwert zurück
}

// Setzt die Lautstärke auf einen absoluten Prozentwert, nach unten/oben
// auf den gültigen Bereich 0..100 begrenzt (Clamping).
void volume_set(int percent)
{
    if (percent < 0) {              // [C] Vergleichsoperator | [Logik] Unterlauf abfangen
        percent = 0;                 // [Logik] auf Minimum klemmen
    } else if (percent > 100) {     // [Logik] Überlauf abfangen
        percent = 100;               // [Logik] auf Maximum klemmen
    }
    apply(percent);                  // [Logik] geklemmten Wert übernehmen und anwenden
}

// Ändert die Lautstärke relativ um delta (z.B. +/- VOLUME_STEP),
// Clamping erfolgt in volume_set().
void volume_adjust(int delta)
{
    volume_set(atomic_load(&s_percent) + delta);  // [C] Addition auf atomar gelesenem Wert | [Logik] relative Änderung, Grenzprüfung überlässt es volume_set()
}

// Schaltet stumm: merkt sich den aktuellen (von Null verschiedenen)
// Wert für spätere Wiederherstellung und setzt die Lautstärke auf 0.
void volume_mute(void)
{
    int cur = atomic_load(&s_percent);   // [Logik] aktuellen Wert vor dem Stummschalten sichern
    if (cur > 0) {                       // [Logik] nur echten (nicht schon stummen) Wert merken, sonst würde erneutes Muten den Wert auf 0 überschreiben
        atomic_store(&s_before_mute, cur);  // [Logik] Rückfallwert für volume_unmute() aktualisieren
    }
    apply(0);                            // [Logik] Lautstärke auf 0 setzen (stumm)
}

// Hebt die Stummschaltung auf: stellt den vor volume_mute() gültigen
// Wert wieder her.
void volume_unmute(void)
{
    apply(atomic_load(&s_before_mute));  // [Logik] gespeicherten Vor-Mute-Wert wieder anwenden
}
