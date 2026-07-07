// MAX98357A-Verstaerker an I2S1 TX, abgeleitet aus amp_test.c (Phase 0).
// Lautstaerke laeuft softwareseitig als Q15-Multiplikation ueber die
// Samples (Spec Section 12, Phase 4) — der MAX98357A hat keinen
// digitalen Gain-Eingang.
#include <stdatomic.h>              // [C] Standard-Header für atomare Typen/Operationen (thread-sichere Zugriffe ohne Mutex)

#include "freertos/FreeRTOS.h"      // [HW] FreeRTOS-Basistypen/Makros, u.a. portMAX_DELAY für blockierendes Warten
#include "driver/i2s_std.h"         // [HW] ESP-IDF I2S-Standard-Modus-Treiber (Philips-I2S-Frames) für den Verstärker-Bus
#include "esp_check.h"              // [C] stellt ESP_RETURN_ON_ERROR-Makro bereit: Fehlerprüfung mit early return + Logging
#include "esp_log.h"                // [Logik] ESP_LOGI/ESP_LOGE-Logging über UART-Konsole

#include "audio_out.h"              // [C] eigener Header: deklariert audio_out_init/write/set_rate/gain (Prototyp-Abgleich)

static const char *TAG = "audio_out";  // [C] static = Datei-lokale (interne) Linkage | [Logik] Log-Tag zur Zuordnung der Ausgaben

#define I2S_BCLK_PIN GPIO_NUM_15    // [C] Präprozessor-Konstante (Textersetzung) | [HW] GPIO15 = Bit-Clock (BCLK), taktet jedes Bit auf der Datenleitung
#define I2S_LRC_PIN  GPIO_NUM_16    // [HW] GPIO16 = Left/Right-Clock (Word-Select), wählt Kanal im I2S-Frame
#define I2S_DIN_PIN  GPIO_NUM_17    // [HW] GPIO17 = Data-Out vom ESP32 zum MAX98357A (SD-Pin des Verstärkers)

#define DEFAULT_RATE 16000          // [HW] Start-Abtastrate 16 kHz, passend zur restlichen Audio-Pipeline

static i2s_chan_handle_t s_tx_chan;   // [C] static-globale Variable, persistiert zwischen Aufrufen | [HW] opaker Handle auf den I2S-TX-Kanal
static uint32_t s_rate;               // [C] vorzeichenlose 32-bit-Variable | [Logik] merkt sich die aktuell aktive Sample-Rate
static atomic_int_least32_t s_gain_q15 = AUDIO_OUT_GAIN_UNITY;  // [C] atomarer Integer-Typ, Initialisierung mit Q15-Unity-Gain | [Logik] thread-sicherer Lautstärke-Multiplikator

// Initialisiert den I2S1-TX-Kanal für den MAX98357A-Verstärker: legt den
// Kanal an, konfiguriert Takt/Slot/GPIOs im Philips-Standardmodus und
// aktiviert die Peripherie. Muss einmalig vor dem ersten audio_out_write()
// laufen.
esp_err_t audio_out_init(void)         // [C] Rückgabetyp esp_err_t (Fehlercode); void = keine Parameter
{
    i2s_chan_config_t chan_cfg =                                    // [HW] Struct für allgemeine I2S-Kanal-Parameter (DMA, Rolle)
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);     // [HW] Makro füllt Defaults: I2S-Port 1, ESP32 als Clock-Master (erzeugt BCLK/WS selbst)
    // Ohne auto_clear loopt nach dem letzten i2s_channel_write der
    // DMA-Buffer-Inhalt endlos weiter -> "Knattern" nach jedem
    // Playback. auto_clear nullt gesendete Deskriptoren.
    chan_cfg.auto_clear = true;                                     // [C] Feldzugriff per '.' auf Struct-Member, bool-Zuweisung | [HW] DMA-Deskriptoren nach Versand automatisch nullen
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx_chan, NULL),  // [C] Makro-Expansion mit early return bei Fehler | [HW] legt Kanal an: RX=NULL (nur TX)
                        TAG, "i2s_new_channel");                       // [Logik] Tag + Meldung für den Fehlerfall

    i2s_std_config_t std_cfg = {                                    // [C] Designated Initializer (Feld-für-Feld-Initialisierung per Name)
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(DEFAULT_RATE),        // [HW] Taktkonfig: leitet MCLK/BCLK-Teiler aus 16 kHz Sample-Rate ab
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(            // [HW] Slot-Konfig im Philips-I2S-Format (Daten 1 BCLK nach WS-Flanke verschoben)
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),          // [HW] 16-bit-Slot-Breite (fertige PCM-Samples), mono (ein Kanal)
        .gpio_cfg = {                                                // [HW] Pin-Zuordnung des I2S-Busses
            .mclk = I2S_GPIO_UNUSED,                                // [HW] kein Master-Clock-Pin nötig (MAX98357A braucht kein MCLK)
            .bclk = I2S_BCLK_PIN,                                   // [HW] Bit-Clock auf GPIO15
            .ws   = I2S_LRC_PIN,                                    // [HW] Word-Select (LRC) auf GPIO16
            .dout = I2S_DIN_PIN,                                    // [HW] Datenausgang (PCM-Samples zum Verstärker) auf GPIO17
            .din  = I2S_GPIO_UNUSED,                                // [HW] kein Dateneingang (reiner Sendekanal)
        },
    };
    // MAX98357A spielt den linken Kanal (SD-Pin offen = links/mono)
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;                 // [C] Feldzugriff per '.' auf verschachteltes Struct | [HW] nur linken Slot senden (SD-Pin offen = links)

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx_chan, &std_cfg),  // [HW] schreibt Clock/Slot/GPIO-Konfig in die I2S-Hardware-Register
                        TAG, "i2s_channel_init_std_mode");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx_chan),              // [HW] startet DMA + I2S-Peripherie; ab hier läuft der BCLK und Samples können gesendet werden
                        TAG, "i2s_channel_enable");
    s_rate = DEFAULT_RATE;                                          // [Logik] aktuelle Rate im Modul-State für spätere Vergleiche merken

    ESP_LOGI(TAG, "I2S1 TX up (%d Hz, 16 bit mono)", DEFAULT_RATE);  // [C] %d = Format-Spezifizierer für Dezimalausgabe (variadisches Makro)
    return ESP_OK;                                                  // [Logik] 0/ESP_OK signalisiert erfolgreiche Initialisierung nach außen
}

// Wechselt die I2S-Ausgabe-Sample-Rate zur Laufzeit (z.B. zwischen
// verschiedenen Audio-Assets mit unterschiedlicher Abtastrate). Deaktiviert
// dazu kurz den Kanal, konfiguriert den Takt neu und aktiviert ihn wieder.
esp_err_t audio_out_set_rate(uint32_t sample_rate)  // [C] Parameter uint32_t = neue Ziel-Sample-Rate in Hz
{
    if (sample_rate == s_rate) {                    // [C] Gleichheitsvergleich | [Logik] keine Änderung nötig, unnötigen Reconfig-Overhead vermeiden
        return ESP_OK;                              // [Logik] no-op, weil schon auf gewünschter Rate
    }
    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);  // [HW] neue Taktkonfig aus gewünschter Rate ableiten
    ESP_RETURN_ON_ERROR(i2s_channel_disable(s_tx_chan), TAG, "disable");     // [HW] Kanal muss vor Clock-Reconfig gestoppt werden
    ESP_RETURN_ON_ERROR(i2s_channel_reconfig_std_clock(s_tx_chan, &clk_cfg), // [HW] schreibt neuen Taktteiler in die I2S-Register
                        TAG, "reconfig_std_clock");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx_chan), TAG, "enable");       // [HW] Kanal mit neuer Taktrate wieder starten
    s_rate = sample_rate;                            // [Logik] neue Rate im Modul-State für künftige Vergleiche merken
    ESP_LOGI(TAG, "sample rate -> %u Hz", (unsigned)sample_rate);  // [C] Cast auf unsigned für den %u-Formatspezifizierer
    return ESP_OK;                                   // [Logik] Erfolg; Kanal läuft jetzt mit neuer Rate
}

// Schreibt einen Sample-Block blockierend über I2S1 an den Verstärker und
// wendet dabei den aktuell gesetzten Software-Gain (Q15-Multiplikation) an.
// Verändert dabei den übergebenen Buffer in-place.
esp_err_t audio_out_write(int16_t *samples, size_t count)  // [C] Zeiger-Parameter samples (In-/Out-Buffer), count = Anzahl Samples
{
    int32_t gain = atomic_load(&s_gain_q15);        // [C] atomarer Lesezugriff (thread-sicher ohne Mutex) | [Logik] aktuellen Lautstärke-Multiplikator holen
    if (gain != AUDIO_OUT_GAIN_UNITY) {             // [C] Ungleichheitsvergleich | [Logik] Gain nur anwenden, wenn er von 1.0 abweicht (Performance)
        for (size_t i = 0; i < count; i++) {        // [C] for-Schleife über alle Samples im Buffer
            samples[i] = (int16_t)(((int32_t)samples[i] * gain) >> 15);  // [C] Cast auf int32_t vor Multiplikation (Überlauf vermeiden), Rechtsshift um 15 = Division durch Q15-Skalierung, Rück-Cast auf int16_t | [Logik] Sample mit Gain-Faktor skalieren
        }
    }

    size_t written = 0;                              // [C] Ausgabe-Variable, wird per Zeiger von i2s_channel_write gefüllt
    return i2s_channel_write(s_tx_chan, samples, count * sizeof(int16_t),  // [HW] blockierender DMA-Write auf den I2S-TX-Kanal | [C] sizeof liefert Typgröße zur Compile-Zeit
                             &written, portMAX_DELAY);  // [C] '&' = Adressoperator, Ausgabe per Zeiger | [HW] portMAX_DELAY = unbegrenzt auf freien DMA-Platz warten
}

// Setzt den Software-Gain (Q15-Multiplikator) atomar; wird von der
// Lautstärke-Logik (local/volume.c) aufgerufen.
void audio_out_set_gain_q15(int32_t gain)  // [C] Rückgabetyp void = kein Wert, Parameter neuer Q15-Gain
{
    atomic_store(&s_gain_q15, gain);        // [C] atomarer Schreibzugriff (thread-sicher ohne Mutex) | [Logik] neuen Gain-Wert für audio_out_write() hinterlegen
}

// Liest den aktuell gesetzten Software-Gain (Q15-Multiplikator) atomar aus.
int32_t audio_out_get_gain_q15(void)  // [C] void = keine Parameter, Rückgabe des Q15-Gain-Werts
{
    return atomic_load(&s_gain_q15);   // [C] atomarer Lesezugriff (thread-sicher ohne Mutex) | [Logik] aktuellen Gain nach außen liefern
}
