// Output-Gain-Management (Spec Section 12, Phase 4): Prozent-Stufen
// 0/20/40/60/80/100, linear auf Q15 gemappt und an audio_out gereicht.
#include <stdatomic.h>

#include "esp_log.h"

#include "drivers/audio_out.h"
#include "volume.h"

static const char *TAG = "volume";

static atomic_int s_percent;
static atomic_int s_before_mute;

// Helferfunktion, die die neue Lautstärke via audio_out_set_gain_q15() an den Audio-Treiber weitergibt und den neuen Wert in s_percent speichert. Garantiert konsistent, da alle Volume-Änderungen über diese Funktion laufen.
static void apply(int percent)
{
    atomic_store(&s_percent, percent); //-> schreib an adresse &s_percent den neuen Wert percent -> garantierte Konsistenz
    audio_out_set_gain_q15(AUDIO_OUT_GAIN_UNITY * percent / 100); //AUDIO_OUT_GAIN_UNITY = Konstante für "volle Lautstärke" im Q15-Format (definiert in audio_out.h)-> "*percent /100" skaliert den Wert zwischen 0 und AUDIO_OUT_GAIN_UNITY
    // beispiel: Q15: 32768 * 50 / 100 = 16384 (= Darstellung von 0.5 nur mit Integern in Q15)
    ESP_LOGI(TAG, "volume %d%%", percent);
}

void volume_init(int initial_percent) //speichert bei Systemstart den konfigurierten initialen Lautstärkewert in Prozent und wendet ihn an (in main.c auf 60 definiert)
{
    atomic_store(&s_before_mute, initial_percent); // speichert Lauttärkewert vor "mute" command, damit bei "unmute" der vorherige Wert wiederhergestellt werden kann
    apply(initial_percent);
}

int volume_get(void) // liefert garantiert konsistent den aktuellen Lautstärkewert in Prozent zurück
{
    return atomic_load(&s_percent);
}

void volume_set(int percent) // setzt Lautstärke auf übergebenen Prozentwert
{
    if (percent < 0) {
        percent = 0;
    } else if (percent > 100) {
        percent = 100;
    }
    apply(percent);
}

// function um volume um Wert delta zu erhöhen/verringern
// zb. volume_adjust(+20); // erhöht Volume um 20%
void volume_adjust(int delta)
{
    volume_set(atomic_load(&s_percent) + delta); 
}

void volume_mute(void)
{
    int cur = atomic_load(&s_percent); // lädt den aktuellen Lautstärkewert in Prozent aus dem atomaren Speicher
    
    // wenn bereits mute ist und nochmal mute vom user ausgeführt wird, verhindert dieses if, dass der ursprüngliche Wert vor dem ersten Mute dadurch mit 0 überschrieben wird und somit nie wiederhergestellt werden könnte
    if (cur > 0) { 
        atomic_store(&s_before_mute, cur);
    }
    apply(0);
}

void volume_unmute(void) // stellt den Wert vor mute wieder her
{
    apply(atomic_load(&s_before_mute));
}
