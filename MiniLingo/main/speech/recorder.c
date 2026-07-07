// Recording-Buffer (Spec Section 9.1): nach dem Wake Word schreibt der
// afe_fetch_task die AFE-Frames hierher. Capture endet nach 1,5 s
// zusammenhaengender Stille (VAD aus dem AFE-Result) oder dem
// 8-s-Hard-Limit. Danach steht im Buffer eine fertige WAV-Datei
// (Header + PCM) — Phase 5 schickt genau diesen Buffer an Whisper.
#include <string.h>                                          // [C] Header-Include für memcpy

#include "esp_heap_caps.h"                                    // [Logik] Heap-Allokator mit Capabilities (hier: PSRAM statt internem RAM)
#include "esp_log.h"                                          // [Logik] ESP_LOGI/ESP_LOGE-Logging über UART-Konsole

#include "wav.h"                                              // [HW] WAV-Header-Erzeugung (wav_write_header, WAV_HEADER_BYTES)
#include "recorder.h"                                         // [C] eigener Header: Typ-/Prototyp-Abgleich

static const char *TAG = "recorder";                          // [C] static = datei-lokale Linkage | [Logik] Log-Tag zur Zuordnung der Ausgaben

#define SAMPLE_RATE       16000   // [HW] 16 kHz, von esp-sr/AFE vorgegebene Abtastrate
#define MAX_RECORD_MS     8000   // Hard-Limit (Spec Phase 3)   // [Logik] maximale Aufnahmedauer, verhindert unbegrenztes Wachsen des Buffers
#define SILENCE_STOP_MS   1500   // Capture-Ende bei Stille (Spec Phase 3)  // [Logik] Schwelle für automatisches Capture-Ende nach Sprechpause

#define PCM_MAX_BYTES (MAX_RECORD_MS * (SAMPLE_RATE / 1000) * 2)  // [C] Präprozessor-Rechnung zur Compile-Zeit | [HW] max. PCM-Bytes: ms * Samples/ms * 2 Byte (16 bit)
#define BYTES_PER_MS  ((SAMPLE_RATE / 1000) * 2)                  // [HW] Bytes pro Millisekunde bei 16 kHz/16 bit mono, für Zeit<->Byte-Umrechnung

static recorder_done_cb_t s_done_cb;                          // [C] static-globale Variable eines Funktionszeiger-Typs | [Logik] Callback für Capture-Ende
static uint8_t *s_buf;        // PSRAM: WAV_HEADER_BYTES + PCM_MAX_BYTES  // [C] Zeiger auf großen Heap-Puffer im PSRAM
static size_t s_pcm_bytes;                                    // [Logik] aktuell befüllte PCM-Bytes im Buffer
static uint32_t s_silence_ms;                                 // [Logik] Zähler zusammenhängender Stille in ms
static uint32_t s_speech_ms;                                  // [Logik] Zähler tatsächlicher Sprach-Frames (VAD_SPEECH) in ms
static bool s_active;                                          // [Logik] Flag: läuft gerade eine Aufnahme

// Allokiert den Recording-Buffer im PSRAM (Header + max. PCM-Länge) und
// merkt sich den Fertig-Callback. Muss einmalig vor der ersten Aufnahme
// laufen.
esp_err_t recorder_init(recorder_done_cb_t cb)                // [C] Rückgabetyp esp_err_t; cb = Funktionszeiger-Parameter (Callback)
{
    s_buf = heap_caps_malloc(WAV_HEADER_BYTES + PCM_MAX_BYTES,  // [C] malloc-Variante mit Capability-Flag | [HW] Puffergröße = WAV-Header + max. PCM-Daten
                             MALLOC_CAP_SPIRAM);              // [HW] Capability-Flag: externes PSRAM (groß genug für 8s Audio, internes RAM würde nicht reichen)
    if (!s_buf) {                                              // [C] '!' prüft auf NULL-Zeiger (Allokation fehlgeschlagen)
        ESP_LOGE(TAG, "no PSRAM for recording buffer (%d bytes)",
                 WAV_HEADER_BYTES + PCM_MAX_BYTES);
        return ESP_ERR_NO_MEM;                                 // [Logik] Fehlercode nach oben durchreichen
    }
    s_done_cb = cb;                                             // [Logik] Callback für spätere Aufrufe merken
    ESP_LOGI(TAG, "recording buffer ready (%d KB PSRAM, max %d ms)",
             (WAV_HEADER_BYTES + PCM_MAX_BYTES) / 1024, MAX_RECORD_MS);
    return ESP_OK;                                             // [Logik] Erfolg signalisieren
}

// Startet eine neue Aufnahme-Session: setzt Fuellstand und Zaehler
// zurueck und aktiviert den Recorder, sodass recorder_feed() ab jetzt
// Daten annimmt.
void recorder_start(void)
{
    s_pcm_bytes = 0;                                           // [Logik] Puffer-Füllstand zurücksetzen (neue Aufnahme beginnt leer)
    s_silence_ms = 0;                                          // [Logik] Stille-Zähler zurücksetzen
    s_speech_ms = 0;                                           // [Logik] Sprach-Zähler zurücksetzen
    s_active = true;                                           // [Logik] Recorder als aktiv markieren
    ESP_LOGI(TAG, "capture started");
}

// Liefert, ob gerade eine Aufnahme laeuft (State-Abfrage fuer den
// afe_fetch_task, um Doppel-Starts zu vermeiden).
bool recorder_is_active(void)
{
    return s_active;                                           // [Logik] aktuellen Aktiv-Status zurückgeben
}

// Bricht eine laufende Aufnahme sofort ab, OHNE den Fertig-Callback
// aufzurufen (z.B. weil ein lokaler Sprachbefehl erkannt wurde und die
// Aufnahme kein Cloud-Query mehr werden soll).
void recorder_abort(void)
{
    if (s_active) {                                            // [C] Bedingung prüft aktuellen Zustand vor Änderung
        s_active = false;                                       // [Logik] Aufnahme deaktivieren, ohne den Buffer weiterzureichen
        ESP_LOGI(TAG, "capture aborted (local command match)");
    }
}

// Schliesst eine Aufnahme regulaer ab: schreibt den WAV-Header ueber die
// gesammelten PCM-Daten und ruft (falls gesetzt) den Fertig-Callback mit
// dem kompletten Buffer auf. reason dokumentiert, warum beendet wurde.
static void finish(recorder_stop_reason_t reason)             // [C] static Hilfsfunktion, nur in dieser Datei aufrufbar
{
    s_active = false;                                          // [Logik] Aufnahme beenden, weitere feed()-Aufrufe werden ignoriert
    wav_write_header(s_buf, (uint32_t)s_pcm_bytes, SAMPLE_RATE, 1, 16);  // [C] Cast auf uint32_t | [HW] schreibt den 44-Byte-RIFF-Header vor die gesammelten PCM-Daten

    uint32_t duration_ms = (uint32_t)(s_pcm_bytes / BYTES_PER_MS);  // [C] Integer-Division + Cast | [Logik] Gesamtdauer der Aufnahme in ms berechnen
    ESP_LOGI(TAG, "capture done: %u ms (%u ms speech), %u bytes WAV (%s)",
             (unsigned)duration_ms, (unsigned)s_speech_ms,
             (unsigned)(WAV_HEADER_BYTES + s_pcm_bytes),
             reason == RECORDER_STOP_SILENCE ? "silence" : "buffer full");  // [C] Ternärer Operator zur Auswahl des Log-Texts

    if (s_done_cb) {                                           // [C] NULL-Check vor Aufruf des Funktionszeigers
        s_done_cb(s_buf, WAV_HEADER_BYTES + s_pcm_bytes, duration_ms,  // [C] indirekter Aufruf über Funktionszeiger | [Logik] übergibt fertigen WAV-Buffer + Metadaten nach oben
                  s_speech_ms, reason);
    }
}

// Haengt einen AFE-Frame an den Aufnahmepuffer an, aktualisiert Stille-/
// Sprech-Zaehler anhand des VAD-Ergebnisses und beendet die Aufnahme
// selbststaendig bei Pufferueberlauf oder 1,5 s Stille.
void recorder_feed(const int16_t *data, size_t bytes, bool is_speech)  // [HW] data = 16-bit-PCM-Samples eines AFE-Frames
{
    if (!s_active || bytes == 0) {                             // [C] logisches ODER; kurzschließende Auswertung | [Logik] nichts zu tun, wenn inaktiv oder leerer Frame
        return;
    }

    size_t room = PCM_MAX_BYTES - s_pcm_bytes;                 // [C] Größenrechnung mit size_t (vorzeichenlos) | [Logik] verbleibender Platz im Puffer
    size_t n = bytes < room ? bytes : room;                    // [C] Ternärer Operator: Minimum aus angebotenen Bytes und freiem Platz
    memcpy(s_buf + WAV_HEADER_BYTES + s_pcm_bytes, data, n);   // [C] Zeigerarithmetik: Schreibposition = Puffer-Anfang + Header + bisheriger Füllstand | [HW] kopiert PCM-Samples in den Recording-Buffer
    s_pcm_bytes += n;                                           // [Logik] Füllstand um kopierte Bytes erhöhen

    if (s_pcm_bytes >= PCM_MAX_BYTES) {                        // [Logik] Hard-Limit erreicht (Buffer voll)?
        finish(RECORDER_STOP_FULL);                            // [Logik] Aufnahme zwangsweise beenden
        return;
    }

    if (is_speech) {                                            // [HW] VAD-Ergebnis dieses Frames: enthält Sprache?
        s_silence_ms = 0;                                       // [Logik] Stille-Zähler zurücksetzen, da gerade gesprochen wird
        s_speech_ms += bytes / BYTES_PER_MS;                    // [C] Integer-Division | [Logik] Sprach-Dauer aufsummieren (für Stille-Guard beim Upload)
    } else {
        s_silence_ms += bytes / BYTES_PER_MS;                   // [Logik] Stille-Dauer aufsummieren
        if (s_silence_ms >= SILENCE_STOP_MS) {                  // [Logik] genug zusammenhängende Stille -> Capture-Ende
            finish(RECORDER_STOP_SILENCE);                      // [Logik] Aufnahme regulär wegen Stille beenden
        }
    }
}
