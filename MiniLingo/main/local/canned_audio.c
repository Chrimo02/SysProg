// Canned-TTS-Wiedergabe (Spec Section 7 "Pre-Generated Audio-Responses"):
// vorab generierte WAVs aus der SPIFFS-Partition "storage" -> I2S1.
// Der Player-Task entspricht dem audio_out_task der Spec-Task-Tabelle
// (Prio 5, Core 1); seit Phase 7 spielt er ueber dieselbe Queue auch
// rohe PCM-Buffer (Gemini-TTS) — dadurch serialisieren sich Canned-
// und Cloud-Wiedergabe, und canned_audio_stop() ("stop") bricht beides
// ab.
//
// WAV-Parser laeuft chunk-weise (RIFF), weil afconvert (say-Fallback
// des Generator-Scripts) einen FLLR-Padding-Chunk vor "data" einfuegt —
// ein fixer 44-Byte-Skip wuerde dort Muell abspielen.
#include <stdatomic.h>              // [C] Header-Include | [C] atomic_bool für lock-freien, thread-sicheren Stop-/Waiting-Zustand
#include <stdio.h>                  // [C] FILE/fopen/fread/fclose/snprintf für Dateizugriff auf SPIFFS
#include <string.h>                 // [C] memcmp/strlen/strcpy/memcpy
#include <sys/stat.h>               // [C] struct stat/stat() für Existenzprüfung der WAV-Dateien

#include "freertos/FreeRTOS.h"      // [HW] FreeRTOS-Basistypen/Makros
#include "freertos/queue.h"         // [HW] QueueHandle_t/xQueueCreate/xQueueSend/xQueueReceive für die Player-Queue
#include "freertos/semphr.h"        // [HW] SemaphoreHandle_t/binäre Semaphore für die PCM-Fertig-Signalisierung
#include "freertos/task.h"          // [HW] xTaskCreatePinnedToCore für den Player-Task

#include "esp_check.h"              // [C] ESP_RETURN_ON_ERROR-Makro: Fehlerprüfung mit early return + Logging
#include "esp_log.h"                // [Logik] ESP_LOGI/ESP_LOGW/ESP_LOGE-Logging über UART-Konsole
#include "esp_spiffs.h"             // [HW] SPIFFS-Dateisystem-Treiber (esp_vfs_spiffs_register) für den Flash-Zugriff

#include "drivers/audio_out.h"      // [HW] deklariert audio_out_write/audio_out_set_rate: eigentliche I2S1-Ausgabe an den DAC
#include "canned_audio.h"           // [C] eigener Header: Public API dieser Datei (Prototyp-Abgleich)

static const char *TAG = "canned_audio";  // [C] static = Datei-lokale Linkage | [Logik] Log-Tag zur Zuordnung der Ausgaben

#define MOUNT_POINT "/spiffs"       // [C] Präprozessor-Konstante | [HW] VFS-Mountpunkt der SPIFFS-Partition
#define MAX_NAME    40              // [Logik] maximale Länge eines WAV-Dateinamens inkl. Nullterminator
#define CHUNK_BYTES 4096            // [Logik] Größe des Zwischenpuffers, in dem WAV-/PCM-Daten häppchenweise gelesen und ausgegeben werden

// Liste aller erwarteten Canned-Audio-Dateien; wird beim Init gegen den
// tatsächlichen SPIFFS-Inhalt geprüft, um fehlende Files frühzeitig zu
// loggen (kein Boot-Abbruch, siehe canned_audio_init()).
static const char *EXPECTED_FILES[] = {   // [C] Array von String-Zeigern, Größe implizit aus Initialisiererliste
    CANNED_TIMER_SET_5S, CANNED_TIMER_SET_10S, CANNED_TIMER_SET_15S,
    CANNED_TIMER_SET_30S, CANNED_TIMER_SET_60S, CANNED_TIMER_SET_300S,
    CANNED_TIMER_SET_600S, CANNED_TIMER_SET_900S, CANNED_TIMER_SET_1800S,
    CANNED_TIMER_CANCELLED, CANNED_TIMER_DONE, CANNED_NO_TIMER,
    CANNED_TIME_LEFT, CANNED_VOLUME_CHANGED, CANNED_MUTED, CANNED_UNMUTED,
    CANNED_ALARM, CANNED_ERROR,
};

// Queue-Item: SPIFFS-WAV (pcm == NULL) oder roher PCM-Buffer (Phase 7,
// Gemini-TTS). Der PCM-Buffer gehoert dem Aufrufer und muss bis zum
// Ende der Wiedergabe gueltig bleiben (canned_audio_play_pcm blockiert
// deshalb).
typedef struct {
    char name[MAX_NAME];       // [C] feste Zeichen-Array-Größe | [Logik] Dateiname für den SPIFFS-Wiedergabepfad; leer/ungenutzt bei PCM-Items
    const int16_t *pcm;        // [C] const Zeiger: Player darf die Daten nicht verändern | [Logik] NULL = Datei-Item, sonst PCM-Item
    size_t samples;            // [Logik] Anzahl der Samples im PCM-Buffer (nur bei PCM-Items relevant)
    uint32_t rate;             // [HW] Abtastrate des PCM-Buffers, wird an audio_out_set_rate() weitergereicht
} play_item_t;

static QueueHandle_t s_queue;             // [C] static-globale Variable | [HW] FreeRTOS-Queue zwischen Aufrufer-Tasks und dem Player-Task
static atomic_bool s_stop;                 // [Logik] Abbruch-Flag, das die aktuelle Wiedergabe (Datei oder PCM) vorzeitig beendet
// Signalisiert dem (einzigen) PCM-Aufrufer das Wiedergabe-Ende;
// s_pcm_waiting verhindert, dass ein stop() ohne wartenden Aufrufer
// ein stales Token hinterlaesst.
static SemaphoreHandle_t s_pcm_done;      // [HW] binäre Semaphore zur Rendezvous-Synchronisation zwischen Player-Task und PCM-Aufrufer
static atomic_bool s_pcm_waiting;         // [Logik] zeigt an, ob gerade ein Aufrufer in canned_audio_play_pcm() auf s_pcm_done wartet

// Liest den RIFF-Header, springt zum "data"-Chunk und liefert dessen
// Laenge; *sample_rate aus dem fmt-Chunk. Nur PCM/16-bit/mono.
static bool wav_open_data(FILE *f, uint32_t *sample_rate,
                          uint32_t *data_bytes)
{
    uint8_t hdr[12];                                          // [C] Byte-Puffer für den 12-Byte-RIFF-Header
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) != 0 ||  // [C] fread liest 12 Bytes; Kurzschluss-Oder mit memcmp prüft Magic-Bytes | [Logik] Header unvollständig oder kein "RIFF"?
        memcmp(hdr + 8, "WAVE", 4) != 0) {                     // [C] Zeiger-Arithmetik hdr+8 auf das WAVE-Feld nach Größenangabe | [Logik] Format-Kennung prüfen
        return false;                                           // [Logik] kein gültiges WAV
    }

    bool fmt_ok = false;                                        // [Logik] merkt, ob bereits ein gültiger fmt-Chunk gelesen wurde
    uint8_t chunk[8];                                            // [C] Puffer für Chunk-ID (4 Byte) + Chunk-Größe (4 Byte)
    while (fread(chunk, 1, 8, f) == 8) {                        // [C] Schleife liest RIFF-Chunks nacheinander, solange volle 8 Bytes gelesen werden
        uint32_t size = chunk[4] | chunk[5] << 8 | chunk[6] << 16 |  // [C] manuelles Little-Endian-Zusammensetzen per Bitshift/Oder, da WAV Little-Endian ist
                        (uint32_t)chunk[7] << 24;                 // [C] Cast verhindert Überlauf/undefiniertes Verhalten beim Schieben um 24 Bit
        if (memcmp(chunk, "fmt ", 4) == 0 && size >= 16) {       // [Logik] fmt-Chunk gefunden und mindestens 16 Byte groß (Minimalgröße für PCM-fmt)
            uint8_t fmt[16];                                      // [C] Puffer für die 16 Pflichtfelder des fmt-Chunks
            if (fread(fmt, 1, 16, f) != 16) {                    // [Logik] fmt-Daten unvollständig gelesen
                return false;
            }
            uint16_t format = fmt[0] | fmt[1] << 8;              // [C] Little-Endian-16-bit-Zusammensetzung | [HW] Audio-Format-Code (1 = PCM)
            uint16_t channels = fmt[2] | fmt[3] << 8;            // [HW] Kanalanzahl (erwartet: 1 = mono)
            uint16_t bits = fmt[14] | fmt[15] << 8;               // [HW] Bits pro Sample (erwartet: 16)
            *sample_rate = fmt[4] | fmt[5] << 8 | fmt[6] << 16 |  // [C] Ausgabeparameter per Zeiger befüllen | [HW] Abtastrate aus dem fmt-Chunk
                           (uint32_t)fmt[7] << 24;
            if (format != 1 || channels != 1 || bits != 16) {    // [Logik] nur unkomprimiertes PCM/mono/16-bit wird unterstützt
                ESP_LOGE(TAG, "unsupported WAV (fmt %u, %u ch, %u bit)",
                         format, channels, bits);                 // [Logik] genaue Fehlerursache loggen
                return false;
            }
            fmt_ok = true;                                        // [Logik] gültigen fmt-Chunk vermerken
            if (fseek(f, (long)(size - 16 + (size & 1)), SEEK_CUR) != 0) {  // [C] Cast auf long | [C] size & 1 = RIFF-Padding-Byte bei ungerader Chunk-Größe | [Logik] über evtl. zusätzliche fmt-Erweiterungsbytes hinwegspringen
                return false;
            }
        } else if (memcmp(chunk, "data", 4) == 0) {              // [Logik] data-Chunk gefunden: enthält die eigentlichen Audiosamples
            *data_bytes = size;                                    // [Logik] Chunk-Größe als Ausgabeparameter zurückgeben
            return fmt_ok;                                         // [Logik] nur erfolgreich, wenn vorher auch ein gültiger fmt-Chunk gelesen wurde
        } else if (fseek(f, (long)(size + (size & 1)), SEEK_CUR) != 0) {  // [Logik] unbekannter Chunk (z.B. FLLR-Padding von afconvert): überspringen statt fest 44 Byte anzunehmen
            return false;
        }
    }
    return false;                                                 // [Logik] Datei zu Ende, ohne dass ein data-Chunk gefunden wurde
}

// Spielt eine einzelne WAV-Datei aus SPIFFS chunk-weise über den
// Audio-Ausgang ab; buf ist der vom Aufrufer bereitgestellte
// Zwischenspeicher (wiederverwendet, kein Heap-Alloc pro Aufruf).
static void play_file(const char *name, int16_t *buf)
{
    char path[sizeof(MOUNT_POINT) + MAX_NAME];                   // [C] sizeof zur Compile-Zeit für Puffergröße | [Logik] vollständiger Dateipfad im VFS
    snprintf(path, sizeof(path), MOUNT_POINT "/%s", name);       // [C] snprintf begrenzt auf Puffergröße, verhindert Overflow | [C] adjazente String-Literal-Verkettung "/spiffs" + "/%s"

    FILE *f = fopen(path, "rb");                                  // [C] Datei binär zum Lesen öffnen
    if (!f) {                                                      // [C] NULL-Prüfung auf fehlgeschlagenes fopen
        ESP_LOGE(TAG, "missing %s", path);                        // [Logik] fehlende Datei loggen (siehe Init-Kommentar: kein Boot-Abbruch)
        return;                                                    // [Logik] Wiedergabe schlicht überspringen
    }

    uint32_t rate = 0;                                             // [C] Ausgabeparameter für wav_open_data, initialisiert
    uint32_t left = 0;                                             // [Logik] verbleibende zu lesende data-Bytes
    if (!wav_open_data(f, &rate, &left)) {                        // [Logik] RIFF-Header/fmt/data parsen; bei Fehler abbrechen
        ESP_LOGE(TAG, "bad WAV: %s", path);
        fclose(f);                                                  // [C] Dateihandle im Fehlerfall dennoch schließen (Ressourcenleck vermeiden)
        return;
    }

    ESP_LOGI(TAG, "playing %s (%u Hz, %u bytes)", name, (unsigned)rate,
             (unsigned)left);                                      // [C] Cast auf unsigned für printf-kompatible Formatierung
    audio_out_set_rate(rate);                                      // [HW] Ausgabe-Abtastrate passend zur WAV-Datei einstellen

    while (left > 0 && !atomic_load(&s_stop)) {                   // [Logik] solange Daten übrig sind UND kein Abbruch angefordert wurde
        size_t want = left < CHUNK_BYTES ? left : CHUNK_BYTES;    // [C] ternärer Operator: min(left, CHUNK_BYTES) | [Logik] letzten Rest nicht über Puffergröße hinaus lesen
        size_t got = fread(buf, 1, want, f);                       // [C] tatsächlich gelesene Bytes können kleiner als want sein (z.B. EOF)
        if (got == 0) {                                             // [Logik] nichts mehr lesbar (z.B. defekte Datei) -> Schleife abbrechen
            break;
        }
        audio_out_write(buf, got / sizeof(int16_t));               // [C] Bytes in Sample-Anzahl umrechnen | [HW] Chunk an die I2S-Ausgabe übergeben
        left -= got;                                                 // [Logik] verbleibende Byte-Anzahl reduzieren
    }
    fclose(f);                                                      // [C] Dateihandle nach Abschluss/Abbruch schließen
}

// PCM-Buffer (PSRAM) chunk-weise ausgeben. audio_out_write wendet den
// Gain in-place an — deshalb erst in den Chunk-Buffer kopieren.
static void play_pcm(const play_item_t *item, int16_t *buf)
{
    ESP_LOGI(TAG, "playing PCM (%u Hz, %u samples)", (unsigned)item->rate,
             (unsigned)item->samples);                              // [C] Pfeiloperator für Zugriff auf Struct-Felder über Zeiger
    audio_out_set_rate(item->rate);                                 // [HW] Ausgabe-Abtastrate passend zum PCM-Buffer (z.B. TTS-Sample-Rate) einstellen

    size_t pos = 0;                                                  // [Logik] aktuelle Leseposition im PCM-Buffer (in Samples)
    while (pos < item->samples && !atomic_load(&s_stop)) {         // [Logik] solange noch Samples übrig UND kein Abbruch angefordert wurde
        size_t n = item->samples - pos;                             // [Logik] verbleibende Samples in diesem Durchlauf
        if (n > CHUNK_BYTES / sizeof(int16_t)) {                    // [C] Umrechnung Byte-Chunk-Größe in Sample-Anzahl
            n = CHUNK_BYTES / sizeof(int16_t);                       // [Logik] auf maximale Chunk-Größe begrenzen
        }
        memcpy(buf, item->pcm + pos, n * sizeof(int16_t));          // [C] Zeigerarithmetik item->pcm + pos | [Logik] Kopie in den lokalen Chunk-Puffer, weil audio_out_write den Gain in-place anwendet und der Original-PCM-Buffer unverändert bleiben soll
        audio_out_write(buf, n);                                     // [HW] Chunk an die I2S-Ausgabe übergeben
        pos += n;                                                     // [Logik] Leseposition um die ausgegebenen Samples weiterrücken
    }
}

// Der eigentliche Player-Task (audio_out_task aus der Spec-Task-
// Tabelle): wartet blockierend auf Queue-Items und spielt sie
// nacheinander ab — entweder eine SPIFFS-Datei oder einen PCM-Buffer,
// je nach dem, ob item.pcm gesetzt ist.
static void player_task(void *arg)
{
    static int16_t chunk_buf[CHUNK_BYTES / sizeof(int16_t)];        // [C] static = einmalig allokiert, lebt für die gesamte Task-Laufzeit statt auf dem Stack | [Logik] gemeinsamer Zwischenpuffer für Datei- und PCM-Wiedergabe
    play_item_t item;                                                 // [C] lokale Kopie des aktuell abzuspielenden Queue-Items

    while (1) {                                                        // [C] Endlosschleife: typischer FreeRTOS-Task-Aufbau
        if (xQueueReceive(s_queue, &item, portMAX_DELAY) == pdTRUE) {  // [HW] blockierendes Warten auf das nächste Queue-Item (unbegrenzt) | [C] '&item' übergibt Zieladresse für die Kopie
            atomic_store(&s_stop, false);                              // [Logik] Abbruch-Flag für das neue Item zurücksetzen (vorheriger stop() darf die neue Wiedergabe nicht sofort beenden)
            if (item.pcm) {                                            // [C] Wahrheitswert eines Zeigers: ungleich NULL? | [Logik] Unterscheidung PCM- vs. Datei-Item
                play_pcm(&item, chunk_buf);                            // [Logik] PCM-Buffer abspielen
                xSemaphoreGive(s_pcm_done);                             // [HW] Semaphore freigeben, um den wartenden PCM-Aufrufer aufzuwecken
            } else {
                play_file(item.name, chunk_buf);                       // [Logik] SPIFFS-Datei abspielen
            }
        }
    }
}

// Initialisiert die Canned-Audio-Infrastruktur: mountet SPIFFS,
// prueft die erwarteten Dateien, legt Queue/Semaphore an und startet
// den Player-Task auf Core 1. Muss nach audio_out_init() aufgerufen
// werden.
esp_err_t canned_audio_init(void)
{
    esp_vfs_spiffs_conf_t conf = {               // [C] Designated Initializer | [HW] Konfigurationsstruct für die SPIFFS-VFS-Registrierung
        .base_path = MOUNT_POINT,                 // [HW] Mountpunkt im virtuellen Dateisystem
        .partition_label = "storage",              // [HW] Name der Flash-Partition laut Partitionstabelle
        .max_files = 2,                             // [HW] maximale Anzahl gleichzeitig offener Dateihandles
        .format_if_mount_failed = false,            // [Logik] bei Mount-Fehler NICHT automatisch formatieren (würde Daten löschen)
    };
    ESP_RETURN_ON_ERROR(esp_vfs_spiffs_register(&conf), TAG,   // [C] Makro: bei Fehler sofortiger return mit Logging | [HW] SPIFFS-Partition ins VFS einhängen
                        "spiffs mount (storage geflasht?)");

    int missing = 0;                              // [Logik] Zähler für fehlende erwartete Dateien
    for (size_t i = 0;
         i < sizeof(EXPECTED_FILES) / sizeof(EXPECTED_FILES[0]); i++) {  // [C] sizeof-Trick zur Ermittlung der Array-Elementanzahl zur Compile-Zeit
        char path[sizeof(MOUNT_POINT) + MAX_NAME];  // [C] lokaler Pfadpuffer je Schleifendurchlauf
        struct stat st;                              // [C] Ausgabestruktur für stat()
        snprintf(path, sizeof(path), MOUNT_POINT "/%s", EXPECTED_FILES[i]);  // [Logik] vollständigen Pfad zur erwarteten Datei bauen
        if (stat(path, &st) != 0) {                  // [C] stat() liefert != 0 bei Fehler (z.B. Datei existiert nicht) | [Logik] Existenzprüfung
            ESP_LOGW(TAG, "canned file missing: %s", path);  // [Logik] fehlende Datei loggen
            missing++;                                 // [C] Post-Inkrement | [Logik] Zähler erhöhen
        }
    }

    s_queue = xQueueCreate(4, sizeof(play_item_t));  // [HW] Queue mit Tiefe 4 für Wiedergabe-Aufträge anlegen
    s_pcm_done = xSemaphoreCreateBinary();             // [HW] binäre Semaphore für die PCM-Fertig-Signalisierung anlegen
    if (!s_queue || !s_pcm_done) {                     // [C] NULL-Prüfung beider Handles (Allokation kann fehlschlagen)
        return ESP_ERR_NO_MEM;                          // [Logik] Fehlercode: kein Speicher für Queue/Semaphore
    }
    BaseType_t ok = xTaskCreatePinnedToCore(player_task, "audio_out", 4096,
                                            NULL, 5, NULL, 1);  // [HW] Player-Task mit 4096 Byte Stack, Priorität 5, fest auf Core 1 (entspricht der Spec-Task-Tabelle)
    if (ok != pdPASS) {                                 // [Logik] Task-Erzeugung fehlgeschlagen
        return ESP_FAIL;
    }

    // Fehlende Files sind kein Boot-Abbruch: der jeweilige Command
    // bleibt dann nur stumm (play_file loggt es erneut).
    ESP_LOGI(TAG, "SPIFFS mounted, %d canned file(s) missing", missing);  // [Logik] Zusammenfassung des Init-Ergebnisses loggen
    return ESP_OK;                                       // [Logik] Initialisierung erfolgreich abgeschlossen
}

// Spielt eine WAV-Datei asynchron ab, indem ein Datei-Item in die
// Player-Queue eingereiht wird — kehrt sofort zurück, aus jedem
// Task-Kontext aufrufbar.
void canned_audio_play(const char *name)
{
    play_item_t item = { 0 };                            // [C] Aggregat-Initialisierung mit 0: alle Felder genullt (insbesondere pcm = NULL -> Datei-Item)
    if (strlen(name) >= MAX_NAME) {                      // [C] strlen ermittelt Stringlänge ohne Nullterminator | [Logik] Namenslänge gegen Puffergröße prüfen (verhindert Überlauf bei strcpy)
        ESP_LOGE(TAG, "name too long: %s", name);
        return;                                            // [Logik] zu langer Name wird verworfen statt den Puffer zu überschreiben
    }
    strcpy(item.name, name);                              // [C] Kopiert den Namen inklusive Nullterminator in das feste Array (Länge zuvor geprüft)
    if (xQueueSend(s_queue, &item, 0) != pdTRUE) {        // [HW] Queue-Einreihung ohne Wartezeit (Timeout 0) | [Logik] Queue voll -> Item wird verworfen
        ESP_LOGW(TAG, "play queue full, dropping %s", name);
    }
}

// Spielt einen rohen PCM-Buffer ab (Gemini-Cloud-TTS) und blockiert den
// Aufrufer, bis die Wiedergabe beendet ist oder canned_audio_stop()
// aufgerufen wurde — so bleibt der PCM-Buffer garantiert waehrend der
// gesamten Wiedergabe gueltig. Darf nur von einem einzigen Aufrufer
// (network_task) verwendet werden.
esp_err_t canned_audio_play_pcm(const int16_t *pcm, size_t samples,
                                uint32_t rate)
{
    play_item_t item = { .pcm = pcm, .samples = samples, .rate = rate };  // [C] Designated Initializer: nur die relevanten Felder gesetzt, name bleibt implizit leer

    // Stales Done-Token entsorgen (stop() ohne wartenden Aufrufer).
    while (xSemaphoreTake(s_pcm_done, 0) == pdTRUE) {     // [HW] Semaphore ohne Warten leeren, falls von einem früheren stop() ohne wartenden Aufrufer ein Token übrig blieb
    }

    atomic_store(&s_pcm_waiting, true);                   // [Logik] signalisiert canned_audio_stop(), dass jetzt ein Aufrufer wartet
    if (xQueueSend(s_queue, &item, pdMS_TO_TICKS(100)) != pdTRUE) {  // [HW] Queue-Einreihung mit 100 ms Timeout (blockiert kurz, falls Queue voll)
        atomic_store(&s_pcm_waiting, false);                // [Logik] Wartezustand zurücksetzen, da gar keine Wiedergabe angestoßen wurde
        ESP_LOGW(TAG, "play queue full, dropping PCM");
        return ESP_FAIL;                                     // [Logik] Fehlercode: PCM konnte nicht eingereiht werden
    }

    // Timeout = Spieldauer + Vorlauf fuer ggf. davor laufende
    // Canned-Files (z.B. Timer-Alarm) — nur Absicherung gegen Haenger.
    TickType_t timeout =
        pdMS_TO_TICKS((uint64_t)samples * 1000 / rate + 30000);  // [C] Cast auf uint64_t verhindert Überlauf bei der Multiplikation | [Logik] Wiedergabedauer in ms + 30 s Sicherheitszuschlag als Timeout
    BaseType_t ok = xSemaphoreTake(s_pcm_done, timeout);    // [HW] blockierend auf das Wiedergabe-Ende-Signal warten (mit Timeout als Absicherung)
    atomic_store(&s_pcm_waiting, false);                     // [Logik] Wartezustand in jedem Fall zurücksetzen (egal ob erfolgreich oder Timeout)
    if (ok != pdTRUE) {                                       // [Logik] Timeout abgelaufen, ohne dass die Semaphore gegeben wurde
        ESP_LOGE(TAG, "PCM playback did not finish in time");
        return ESP_ERR_TIMEOUT;                                // [Logik] Fehlercode: Wiedergabe hat vermutlich gehangen
    }
    return ESP_OK;                                             // [Logik] Wiedergabe regulär (oder per stop()) beendet
}

// Bricht die aktuell laufende Wiedergabe ab und leert die Warteschlange
// — betrifft sowohl SPIFFS-Dateien als auch (seit Phase 7) laufende
// PCM-Wiedergaben; weckt einen ggf. wartenden PCM-Aufrufer zusätzlich
// explizit auf.
void canned_audio_stop(void)
{
    xQueueReset(s_queue);                                     // [HW] alle noch nicht abgespielten Items aus der Queue entfernen
    atomic_store(&s_stop, true);                               // [Logik] Abbruch-Flag setzen, damit die play_file/play_pcm-Schleifen sofort enden
    // Falls das PCM-Item noch ungespielt in der Queue lag, hat es der
    // Reset verworfen — den wartenden Aufrufer trotzdem aufwecken.
    // (Lief es bereits, gibt der Player gleich selbst; doppeltes Give
    // kappt die binaere Semaphore.)
    if (atomic_load(&s_pcm_waiting)) {                         // [Logik] nur wecken, wenn tatsächlich ein Aufrufer in canned_audio_play_pcm() wartet
        xSemaphoreGive(s_pcm_done);                             // [HW] Semaphore geben, um den wartenden Aufrufer aus xSemaphoreTake() zu befreien
    }
}
