// DS18B20-Temperatursensor, 1-Wire auf GPIO 47 (Bit-Bang via GPIO).
// Kein externer Component — IDF5 GPIO + ets_delay_us reichen aus.
// Die Messung (12-Bit, ~750 ms) laeuft in einem periodischen esp_timer
// (one-shot, alle 10 s rearmed). temperature_get() liefert den Cache
// sofort ohne Bus-I/O, atomar wie volume_get().
//
// 1-Wire-Timings gemaess Maxim DS18B20 Datasheet Rev. 6 (Abb. 1):
//   Reset pulse:  >= 480 µs low; Sample nach 15-60 µs (Presence: 60-240 µs)
//   Write-1 slot: 1-15 µs low, dann release (gesamt >= 60 µs)
//   Write-0 slot: 60-120 µs low
//   Read slot:    1-15 µs low, dann sample innerhalb 15 µs nach Release
//
// Aufloesung: 12 Bit (default). Conversion time: max 750 ms.
// CRC: Maxim DOW CRC (x^8 + x^5 + x^4 + 1).
#include <stdatomic.h>              // [C] Header-Include | [C] atomic_int/atomic_bool für lock-freien, thread-sicheren Zugriff auf den Mess-Cache
#include <stdbool.h>                // [C] bool/true/false
#include <string.h>                 // [C] String-/Speicherfunktionen (hier ungenutzt direkt, aber Teil der Standard-Includes)

#include "driver/gpio.h"            // [HW] ESP-IDF GPIO-Treiber: gpio_config/gpio_set_direction/gpio_set_level/gpio_get_level für das Bit-Banging
#include "esp_log.h"                // [Logik] ESP_LOGI/ESP_LOGW-Logging über UART-Konsole
#include "esp_timer.h"              // [HW] ESP-IDF High-Resolution-Timer-API für die periodische Messung
#include "freertos/FreeRTOS.h"      // [HW] FreeRTOS-Basistypen/Makros (u.a. pdMS_TO_TICKS)
#include "freertos/task.h"          // [HW] vTaskDelay für blockierendes Warten im Timer-Task
#include "rom/ets_sys.h"            // [HW] ets_delay_us: Busy-Wait im Mikrosekundenbereich für exaktes 1-Wire-Timing

#include "temperature.h"            // [C] eigener Header: Public API dieser Datei (Prototyp-Abgleich)

static const char *TAG = "temperature";  // [C] static = Datei-lokale Linkage | [Logik] Log-Tag zur Zuordnung der Ausgaben

// Messung alle 10 s; Pause zwischen Warmup-Delay und naechster Messung.
#define MEASURE_INTERVAL_US  (10 * 1000 * 1000)  // [C] Präprozessor-Konstante, Ausdruck wird zur Compile-Zeit berechnet | [HW] Periodendauer zwischen zwei Messzyklen in µs (10 s)
// Wandlungszeit 12-Bit DS18B20 max 750 ms.
#define CONVERT_WAIT_MS      800  // [HW] Wartezeit nach "Convert T"-Befehl, bis das Ergebnis im Scratchpad bereitsteht (Datenblatt-Maximum + Puffer)

// Gecachter Wert in milli-Celsius; INT32_MIN = ungueltig.
static atomic_int  s_milli_celsius;  // [C] static-globale, atomare Ganzzahl | [Logik] zuletzt gemessener Wert in milli-Grad-Celsius (Cache für temperature_get())
static atomic_bool s_valid;          // [Logik] zeigt an, ob s_milli_celsius aktuell einen gültigen Messwert enthält
static esp_timer_handle_t s_timer;   // [HW] opaker Handle auf den periodischen esp_timer

// Schuetzt jeden einzelnen Reset-/Bit-Zeitschlitz vor Task-Preemption:
// die Timings sind Mikrosekunden-genau (ets_delay_us Busy-Wait), und ein
// Scheduler-Wechsel mittendrin (Audio/WiFi/UI-Tasks) verschiebt die Flanken
// genug, um Presence-Erkennung oder CRC zu zerstoeren (User-Report
// 2026-07-07: sporadisches "Sensor not ready", direkt danach wieder ok).
static portMUX_TYPE s_ow_mux = portMUX_INITIALIZER_UNLOCKED;  // [HW] Spinlock-Handle für portENTER_CRITICAL/portEXIT_CRITICAL

// --- 1-Wire low-level (GPIO Bit-Bang, critical sections per task) ---

// Zieht die 1-Wire-Datenleitung aktiv auf Low, indem der Pin als Ausgang
// geschaltet und auf 0 gesetzt wird (Reset-Puls / Write-0 / Beginn jedes
// Zeitschlitzes).
static inline void ow_low(void)
{
    gpio_set_direction(TEMP_ONEWIRE_GPIO, GPIO_MODE_OUTPUT);  // [HW] Pin auf Ausgang schalten, damit er die Leitung aktiv treiben kann
    gpio_set_level(TEMP_ONEWIRE_GPIO, 0);                     // [HW] Leitung auf Low ziehen (Open-Drain-Verhalten des 1-Wire-Busses simuliert durch Umschalten der Richtung)
}

// Gibt die Leitung wieder frei (Eingang), sodass der externe Pullup
// sie auf High zieht — Grundzustand zwischen den Zeitschlitzen.
static inline void ow_release(void)
{
    // Pullup zieht die Leitung auf High; nur Richtung aendern.
    gpio_set_direction(TEMP_ONEWIRE_GPIO, GPIO_MODE_INPUT);  // [HW] Pin auf Eingang schalten, kein aktiver Treiber mehr -> externer 4,7k-Pullup bestimmt den Pegel
}

// Liest den aktuellen Rohpegel der Leitung (0/1) ohne Timing-Logik.
static inline int ow_read_bit(void)
{
    return gpio_get_level(TEMP_ONEWIRE_GPIO);  // [HW] direkter GPIO-Pegel-Read
}

// Gibt true zurueck, wenn ein Presence-Pulse erkannt wurde.
static bool ow_reset(void)
{
    portENTER_CRITICAL(&s_ow_mux);  // [HW] Interrupts/Preemption auf diesem Core sperren, solange die Flanken zeitkritisch sind
    ow_low();                    // [HW] Reset-Puls: Leitung mindestens 480 µs low ziehen
    ets_delay_us(480);            // [HW] Busy-Wait exakt für die geforderte Reset-Low-Zeit (Datenblatt-Minimum)
    ow_release();                 // [HW] Leitung freigeben, Pullup zieht sie hoch
    ets_delay_us(70);              // [HW] Warten in das Presence-Detect-Fenster (15-60 µs bis Sensor antwortet)
    int presence = !ow_read_bit();  // [C] logisches NICHT | [HW] Sensor zieht bei Anwesenheit die Leitung kurz low -> Pegel 0 bedeutet "Presence erkannt"
    ets_delay_us(410);             // [HW] Rest des Presence-Fensters abwarten, bevor der nächste Schritt beginnt
    portEXIT_CRITICAL(&s_ow_mux);   // [HW] Sperre wieder aufheben
    return presence;               // [Logik] true = mindestens ein Sensor am Bus hat geantwortet
}

// Sendet ein einzelnes Bit über den 1-Wire-Bus mit den datenblatt-
// konformen Write-1/Write-0-Timings.
static void ow_write_bit(int bit)
{
    portENTER_CRITICAL(&s_ow_mux);  // [HW] Zeitschlitz darf nicht durch Task-Wechsel unterbrochen werden
    ow_low();               // [HW] jeder Schreib-Zeitschlitz beginnt mit einer Low-Flanke
    if (bit) {              // [C] Bedingung auf Wahrheitswert des Parameters | [Logik] Fallunterscheidung Write-1 vs. Write-0
        ets_delay_us(6);      // [HW] Write-1: nur kurz low (innerhalb 1-15 µs)
        ow_release();         // [HW] danach früh freigeben, damit der Pullup wieder hochzieht
        ets_delay_us(64);     // [HW] Rest des Zeitschlitzes abwarten (Gesamtdauer >= 60 µs)
    } else {
        ets_delay_us(60);     // [HW] Write-0: die gesamte Slot-Zeit (60-120 µs) low halten
        ow_release();         // [HW] danach freigeben
        ets_delay_us(10);     // [HW] kurze Erholzeit vor dem nächsten Slot
    }
    portEXIT_CRITICAL(&s_ow_mux);   // [HW] Sperre wieder aufheben
}

// Liest ein einzelnes Bit vom Bus: initiiert einen Read-Zeitschlitz und
// tastet die Leitung innerhalb des vom Sensor vorgegebenen Fensters ab.
static int ow_read_bit_slot(void)
{
    portENTER_CRITICAL(&s_ow_mux);  // [HW] Sample-Fenster (15 µs) ist zu knapp für eine Preemption dazwischen
    ow_low();                 // [HW] Read-Slot beginnt ebenfalls mit kurzer Low-Flanke (Master initiiert den Slot)
    ets_delay_us(6);           // [HW] Mindest-Low-Zeit (1-15 µs), danach übernimmt der Sensor ggf. die Leitung
    ow_release();              // [HW] Leitung freigeben, Sensor kann sie halten (Bit 0) oder Pullup zieht hoch (Bit 1)
    ets_delay_us(9);            // [HW] Warten, bis der Pegel stabil ist, aber innerhalb der 15-µs-Sample-Frist
    int bit = ow_read_bit();    // [HW] eigentliches Abtasten des Bits
    ets_delay_us(55);           // [HW] Rest des Zeitschlitzes abwarten (Gesamtslot >= 60 µs)
    portEXIT_CRITICAL(&s_ow_mux);   // [HW] Sperre wieder aufheben
    return bit;                 // [Logik] gelesenes Bit an den Aufrufer zurückgeben
}

// Sendet ein ganzes Byte LSB-first über den 1-Wire-Bus (1-Wire-Protokoll
// verlangt Least-Significant-Bit zuerst).
static void ow_write_byte(uint8_t byte)
{
    for (int i = 0; i < 8; i++) {   // [C] for-Schleife über alle 8 Bits eines Bytes
        ow_write_bit(byte & 0x01);   // [C] Bitmaske isoliert das niederwertigste Bit | [HW] genau dieses Bit auf den Bus schreiben
        byte >>= 1;                  // [C] Rechtsshift: nächstes Bit rückt an Position 0 (LSB-first-Reihenfolge)
    }
}

// Liest ein ganzes Byte LSB-first vom 1-Wire-Bus.
static uint8_t ow_read_byte(void)
{
    uint8_t byte = 0;                        // [C] Akkumulator, startet bei 0
    for (int i = 0; i < 8; i++) {            // [C] Schleife über 8 Bit-Positionen
        byte |= (ow_read_bit_slot() << i);   // [C] gelesenes Bit an Position i einschieben (Shift + Oder-Verknüpfung) | [HW] LSB-first passend zum Schreibformat
    }
    return byte;                              // [Logik] vollständig zusammengesetztes Byte zurückgeben
}

// Maxim DOW CRC (Poly x^8 + x^5 + x^4 + 1).
// Berechnet die Pruefsumme über die uebergebenen Bytes nach dem Maxim/
// Dallas-1-Wire-CRC8-Verfahren, mit dem die Datenintegrität des
// Scratchpads geprüft wird.
static uint8_t ow_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0;                          // [C] CRC-Register, Startwert 0 laut Verfahren
    for (size_t i = 0; i < len; i++) {       // [C] size_t = vorzeichenlose Größe | [C] Schleife über alle Eingabe-Bytes
        uint8_t byte = data[i];               // [C] Zeiger-Indexzugriff auf das aktuelle Byte
        for (int j = 0; j < 8; j++) {        // [C] innere Schleife über die 8 Bits des Bytes
            uint8_t mix = (crc ^ byte) & 0x01;  // [C] XOR verknüpft CRC- und Daten-Bit, Maske extrahiert das niederwertigste Ergebnisbit
            crc >>= 1;                         // [C] CRC-Register um ein Bit nach rechts schieben
            if (mix) {                        // [Logik] Polynom-Term nur anwenden, wenn das XOR-Bit gesetzt war
                crc ^= 0x8C;                   // [C] XOR mit dem reflektierten Generatorpolynom (x^8+x^5+x^4+1 in Bit-reflektierter Form)
            }
            byte >>= 1;                        // [C] nächstes Datenbit für die nächste Iteration bereitstellen
        }
    }
    return crc;                                // [Logik] fertige Prüfsumme zurückgeben, wird gegen das vom Sensor gesendete CRC-Byte verglichen
}

// --- DS18B20 high-level ---

// DS18B20-Befehle
#define DS18B20_CMD_SKIP_ROM      0xCC  // [HW] 1-Wire-ROM-Befehl: adressiert alle Sensoren am Bus (hier: genau 1 Sensor, daher zulässig ohne Einzeladressierung)
#define DS18B20_CMD_CONVERT_T     0x44  // [HW] Funktionsbefehl: startet eine Temperaturwandlung im Sensor
#define DS18B20_CMD_READ_SCRATCH  0xBE  // [HW] Funktionsbefehl: liest das interne Scratchpad-Register (enthält Temperaturdaten + CRC)

// Startet eine Temperaturwandlung (Skip ROM: genau 1 Sensor am Bus).
static esp_err_t ds_convert(void)
{
    if (!ow_reset()) {                          // [C] logisches NICHT auf Rückgabewert | [Logik] kein Sensor hat auf den Reset reagiert
        return ESP_ERR_NOT_FOUND;                // [Logik] Fehlercode: Sensor nicht gefunden
    }
    ow_write_byte(DS18B20_CMD_SKIP_ROM);         // [HW] ROM-Adressierung überspringen (nur 1 Sensor am Bus)
    ow_write_byte(DS18B20_CMD_CONVERT_T);        // [HW] Wandlung anstoßen; Sensor braucht danach bis zu 750 ms
    return ESP_OK;                                // [Logik] Befehl erfolgreich abgesetzt
}

// Liest das 9-Byte-Scratchpad und gibt die Temperatur in Grad zurueck.
// Prueft CRC byte 8 ueber bytes 0-7.
static esp_err_t ds_read_temp(float *out_temp)
{
    if (!ow_reset()) {                           // [Logik] Sensor antwortet nicht mehr auf den Reset vor dem Lesebefehl
        return ESP_ERR_NOT_FOUND;
    }
    ow_write_byte(DS18B20_CMD_SKIP_ROM);         // [HW] wieder ohne ROM-Adressierung, da nur ein Sensor am Bus
    ow_write_byte(DS18B20_CMD_READ_SCRATCH);     // [HW] Scratchpad-Lesevorgang anfordern

    uint8_t sp[9];                                // [C] Array für die 9 Scratchpad-Bytes (2 Temperaturbytes + Konfig + CRC)
    for (int i = 0; i < 9; i++) {                // [C] Schleife über alle 9 Bytes
        sp[i] = ow_read_byte();                   // [HW] jedes Byte einzeln über den Bus lesen
    }
    if (ow_crc8(sp, 8) != sp[8]) {                // [C] Ungleichheitsvergleich | [Logik] berechnete CRC über Bytes 0-7 gegen das vom Sensor gesendete CRC-Byte 8 prüfen
        ESP_LOGW(TAG, "CRC mismatch");            // [Logik] Datenübertragungsfehler protokollieren
        return ESP_ERR_INVALID_CRC;               // [Logik] Fehlercode: Daten verworfen, kein gültiger Messwert
    }
    int16_t raw = (int16_t)((sp[1] << 8) | sp[0]);  // [C] Bytes zu 16-bit-Wert zusammensetzen (Little-Endian) | [C] Cast auf signed int16_t, da Temperatur negativ sein kann
    *out_temp = (float)raw / 16.0f;                 // [C] Cast auf float, Division durch 16 wandelt die 12-Bit-Festkomma-Rohzahl (1/16 °C Auflösung) in Grad Celsius um
    return ESP_OK;                                   // [Logik] Messwert erfolgreich gelesen und verifiziert
}

// --- Timer-Callback ---

// Wird periodisch vom esp_timer-Task aufgerufen: stösst eine Wandlung
// an, wartet die Wandlungszeit ab, liest das Ergebnis und aktualisiert
// den atomaren Cache — anschliessend wird der Timer fuer den naechsten
// Zyklus neu scharf gemacht (One-Shot statt periodisch, damit die
// Wartezeit dazwischen exakt eingehalten wird).
static void measure_cb(void *arg)
{
    esp_err_t err = ds_convert();                 // [HW] Wandlung im Sensor anstoßen
    if (err != ESP_OK) {                          // [Logik] Sensor antwortet nicht (z.B. abgezogen)
        ESP_LOGW(TAG, "convert failed: %s", esp_err_to_name(err));  // [Logik] Fehlercode in lesbaren Namen umwandeln und loggen
        atomic_store(&s_valid, false);            // [Logik] Cache als ungültig markieren, bis nächste erfolgreiche Messung
        esp_timer_start_once(s_timer, MEASURE_INTERVAL_US);  // [HW] trotzdem nächsten Messzyklus in 10 s einplanen
        return;                                    // [Logik] dieser Zyklus wird abgebrochen
    }
    // 12-Bit-Wandlung abwarten — wir laufen im esp_timer-Task.
    vTaskDelay(pdMS_TO_TICKS(CONVERT_WAIT_MS));   // [HW] blockierendes Warten (RTOS-Tick-Umrechnung), bis der Sensor die Wandlung abgeschlossen hat

    float temp;                                    // [C] lokale Variable für den gelesenen Temperaturwert
    err = ds_read_temp(&temp);                     // [HW] Scratchpad lesen und Temperatur extrahieren
    if (err != ESP_OK) {                           // [Logik] Lesefehler (Bus-Problem oder CRC-Mismatch)
        atomic_store(&s_valid, false);             // [Logik] Cache invalidieren
        esp_timer_start_once(s_timer, MEASURE_INTERVAL_US);  // [HW] nächsten Zyklus dennoch einplanen
        return;                                     // [Logik] Zyklus abbrechen, kein Update des Caches
    }
    atomic_store(&s_milli_celsius, (int)(temp * 1000.0f));  // [C] Cast auf int, Skalierung Grad -> milli-Grad | [Logik] neuen Messwert atomar im Cache ablegen
    atomic_store(&s_valid, true);                  // [Logik] Cache ist jetzt gültig
    ESP_LOGI(TAG, "%.1f degC", temp);              // [Logik] Messwert protokollieren (%.1f = eine Nachkommastelle)
    esp_timer_start_once(s_timer, MEASURE_INTERVAL_US);  // [HW] nächsten Messzyklus in 10 s neu scharf machen (One-Shot-Rearm statt periodischem Timer)
}

// --- Public API ---

// Initialisiert den 1-Wire-GPIO, prueft die Sensorpraesenz, fuehrt eine
// synchrone Erstmessung durch (damit sofort nach dem Boot ein gueltiger
// Wert vorliegt) und startet danach den periodischen Mess-esp_timer.
esp_err_t temperature_init(void)
{
    atomic_store(&s_valid, false);   // [Logik] Cache zu Beginn explizit als ungültig markieren

    gpio_config_t cfg = {                                  // [C] Designated Initializer (Feld-für-Feld-Initialisierung per Name) | [HW] Struct für die GPIO-Konfiguration
        .pin_bit_mask = (1ULL << TEMP_ONEWIRE_GPIO),        // [C] Bitmaske per Linksshift auf ein 64-bit-Literal (ULL) | [HW] wählt genau den 1-Wire-Datenpin aus
        .mode         = GPIO_MODE_INPUT,                    // [HW] Grundzustand Eingang (Bus wird nur bei Bedarf per ow_low() aktiv auf Ausgang geschaltet)
        .pull_up_en   = GPIO_PULLUP_DISABLE, // externer 4,7k Pullup   // [HW] interner Pullup deaktiviert, da bereits ein externer 4,7-kOhm-Pullup den Bus auf High zieht
        .pull_down_en = GPIO_PULLDOWN_DISABLE,              // [HW] kein Pulldown, würde den Bus-Pegel verfälschen
        .intr_type    = GPIO_INTR_DISABLE,                  // [HW] keine GPIO-Interrupts nötig, da rein per Busy-Wait (Bit-Bang) kommuniziert wird
    };
    esp_err_t err = gpio_config(&cfg);   // [HW] Konfiguration in die GPIO-Hardware-Register schreiben
    if (err != ESP_OK) {                  // [Logik] Konfigurationsfehler
        return err;                        // [Logik] Fehler unverändert nach oben durchreichen
    }

    // Pruefen, ob ein Sensor vorhanden ist.
    if (!ow_reset()) {                                    // [Logik] Presence-Test: reagiert überhaupt ein Sensor auf den Bus?
        ESP_LOGW(TAG, "no DS18B20 presence on GPIO %d", TEMP_ONEWIRE_GPIO);  // [Logik] Warnung, kein Sensor gefunden
        return ESP_ERR_NOT_FOUND;                          // [Logik] Initialisierung bricht mit Fehlercode ab
    }
    ESP_LOGI(TAG, "DS18B20 detected on GPIO %d", TEMP_ONEWIRE_GPIO);  // [Logik] erfolgreiche Sensor-Erkennung protokollieren

    // Erste Messung synchron im Init: Wandlung auslösen, kurz warten,
    // sofort lesen — so ist der Wert beim ersten User-Command bereits
    // gueltig (kein "Sensor not ready" nach dem Boot/Charakter-Wechsel).
    err = ds_convert();                                    // [HW] erste Wandlung direkt im Init anstoßen
    if (err == ESP_OK) {                                   // [Logik] nur weitermachen, wenn der Sensor die Wandlung angenommen hat
        vTaskDelay(pdMS_TO_TICKS(CONVERT_WAIT_MS));        // [HW] blockierend auf die Wandlungszeit warten (Init läuft im Boot-Task, das ist hier unkritisch)
        float temp;                                         // [C] lokale Variable für den Erstmesswert
        if (ds_read_temp(&temp) == ESP_OK) {                // [Logik] nur bei erfolgreichem, CRC-validem Lesen den Cache befüllen
            atomic_store(&s_milli_celsius, (int)(temp * 1000.0f));  // [C] Cast + Skalierung wie im periodischen Callback
            atomic_store(&s_valid, true);                    // [Logik] Cache ab sofort gültig, noch bevor der periodische Timer überhaupt läuft
            ESP_LOGI(TAG, "initial: %.1f degC", temp);        // [Logik] Erstmesswert protokollieren
        }
    }

    esp_timer_create_args_t ta = {           // [C] Designated Initializer | [HW] Erzeugungsparameter für den periodischen Mess-Timer
        .callback = measure_cb,               // [C] Funktionszeiger auf den Timer-Callback | [HW] wird bei jedem Ablauf von der Timer-Peripherie/-Task aufgerufen
        .name     = "temp_measure",           // [Logik] Name für Debug-/Trace-Zwecke
    };
    err = esp_timer_create(&ta, &s_timer);   // [HW] Timer-Handle in der esp_timer-Infrastruktur anlegen
    if (err != ESP_OK) {                      // [Logik] Anlage fehlgeschlagen (z.B. kein Speicher)
        return err;                            // [Logik] Fehler nach oben durchreichen
    }
    return esp_timer_start_once(s_timer, MEASURE_INTERVAL_US);  // [HW] ersten periodischen Messzyklus in 10 s einplanen | [Logik] Rückgabewert des Starts ist zugleich der Rückgabewert der Init-Funktion
}

// Liefert den zuletzt gecachten Temperaturwert in Grad Celsius, ohne
// Bus-Zugriff — beliebig oft und aus jedem Task aufrufbar. Der Wert ist
// nur sinnvoll, wenn temperature_valid() true zurückgibt.
float temperature_get(void)
{
    return (float)atomic_load(&s_milli_celsius) / 1000.0f;  // [C] atomarer Lesezugriff, Cast auf float, Division wandelt milli-Grad zurück in Grad
}

// Gibt an, ob der gecachte Temperaturwert aktuell gültig ist (false vor
// der ersten erfolgreichen Messung sowie nach Bus-/CRC-Fehlern).
bool temperature_valid(void)
{
    return atomic_load(&s_valid);  // [C] atomarer Lesezugriff auf das Gültigkeits-Flag
}
