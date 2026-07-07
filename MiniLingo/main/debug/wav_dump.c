// Phase-3-Testhilfe: WAV-Buffer Base64-codiert ueber UART rausziehen
// (Spec Phase 3: "Test: WAV-Buffer via UART/SPIFFS rausziehen und in
// Audacity oeffnen"). Eigener Task mit niedriger Prio auf Core 0,
// damit der blockierende UART-Write (8 s Audio ~ 341 KB Base64 ~ 30 s
// bei 115200 Baud) nicht den afe_fetch_task aufhaelt.
#include <stdio.h>                    // [C] Standardbibliothek für printf

#include "sdkconfig.h"                // [C] generierte Konfig-Header aus menuconfig/Kconfig | [Logik] stellt CONFIG_RECORDING_WAV_DUMP bereit

#if CONFIG_RECORDING_WAV_DUMP          // [C] Präprozessor-Bedingung: kompiliert nur, wenn Option in menuconfig aktiv ist

#include "freertos/FreeRTOS.h"        // [HW] FreeRTOS-Basistypen (BaseType_t, portMAX_DELAY)
#include "freertos/semphr.h"          // [HW] Semaphor-API für Task-Synchronisation
#include "freertos/task.h"            // [HW] Task-Erzeugung (xTaskCreatePinnedToCore), vTaskDelay

#include "esp_log.h"                  // [Logik] Logging-API, hier zum Stummschalten anderer Tasks während des Dumps
#include "mbedtls/base64.h"           // [HW] Base64-Encoder aus mbedTLS, wandelt Binärdaten in UART-taugliche ASCII-Zeichen um

#include "wav_dump.h"                 // [C] eigener Header: deklariert wav_dump_init/wav_dump_trigger (Prototyp-Abgleich)

// 480 Rohbytes -> 640 Base64-Zeichen pro Zeile (+ NUL)
#define CHUNK_BYTES 480               // [C] Präprozessor-Konstante | [HW] Chunk-Größe pro Base64-Zeile, klein genug für UART-Zeilenpuffer

static SemaphoreHandle_t s_sem;       // [C] static = Datei-lokale Linkage | [Logik] Binär-Semaphor, signalisiert dem Dump-Task "neue Daten liegen bereit"
static const uint8_t *s_data;         // [C] Zeiger auf Rohbytes des zu dumpenden Puffers, wird von wav_dump_trigger gesetzt
static size_t s_bytes;                // [C] size_t = vorzeichenlose Größe | [Logik] Länge des zu dumpenden Puffers in Bytes

// Dauerlauf-Task: wartet auf ein Semaphor-Signal, base64-codiert dann den
// per wav_dump_trigger() übergebenen Puffer in Chunks und schreibt sie
// zeilenweise per printf/UART raus, umrahmt von BEGIN/END-Markern für das
// Extraktions-Script auf der PC-Seite.
static void wav_dump_task(void *arg)
{
    static char line[CHUNK_BYTES / 3 * 4 + 1];  // [C] static = einmalig allokierter Zeilenpuffer, nicht auf dem Stack | [HW] Base64 macht aus 3 Rohbytes 4 Zeichen (+1 für NUL-Terminator)

    while (1) {                                  // [C] Endlosschleife: Task lebt für die gesamte Programmlaufzeit
        xSemaphoreTake(s_sem, portMAX_DELAY);     // [C] blockierender Warteaufruf | [Logik] schläft, bis wav_dump_trigger() das Semaphor gibt

        // ESP_LOG anderer Tasks (z. B. WiFi-Reconnect) wuerde sich
        // zeichenweise in die Base64-Zeilen mischen und den Dump
        // korrumpieren — fuer die Dauer des Dumps stummschalten.
        esp_log_level_t prev_level = esp_log_level_get("*");  // [Logik] aktuellen globalen Log-Level merken, um ihn später wiederherzustellen
        esp_log_level_set("*", ESP_LOG_NONE);                 // [Logik] alle Logs global stummschalten, damit kein Fremd-Output die Base64-Zeilen unterbricht

        printf("\nWAV_DUMP_BEGIN bytes=%u\n", (unsigned)s_bytes);  // [HW] Start-Marker mit Gesamtgröße, den das PC-seitige Extraktions-Script erkennt
        for (size_t off = 0; off < s_bytes; off += CHUNK_BYTES) {  // [C] Schleife über den Puffer in CHUNK_BYTES-Schritten | [Logik] off = aktueller Byte-Offset im Quellpuffer
            size_t n = s_bytes - off;                              // [C] verbleibende Restbytes ab aktuellem Offset
            if (n > CHUNK_BYTES) {                                 // [Logik] mehr Restbytes als ein Chunk fasst?
                n = CHUNK_BYTES;                                   // [Logik] auf Chunk-Größe begrenzen
            }
            size_t olen = 0;                                       // [C] Ausgabe-Variable, wird von mbedtls_base64_encode per Zeiger befüllt
            mbedtls_base64_encode((unsigned char *)line, sizeof(line),  // [C] Cast auf unsigned char* (Base64-API erwartet vorzeichenlose Bytes) | [HW] codiert n Rohbytes nach Base64-ASCII
                                  &olen, s_data + off, n);          // [C] Zeigerarithmetik s_data + off = Startadresse des aktuellen Chunks
            line[olen] = '\0';                                     // [C] Base64-Ausgabe manuell NUL-terminieren, damit printf("%s") korrekt endet
            printf("%s\n", line);                                  // [HW] eine Base64-Zeile über die UART-Konsole ausgeben
            // printf busy-waitet auf die UART-FIFO (Default-Console
            // ohne UART-Treiber) — ohne Yield verhungert IDLE0 und der
            // Task-Watchdog schreibt seinen Report mitten in den Dump.
            vTaskDelay(1);                                          // [Logik] gibt CPU an andere Tasks ab (v.a. IDLE0), verhindert Watchdog-Trigger während des langen Dumps
        }
        printf("WAV_DUMP_END\n");                                  // [HW] End-Marker, signalisiert dem Extraktions-Script das Ende des Dumps

        esp_log_level_set("*", prev_level);                        // [Logik] globalen Log-Level wieder auf den vor dem Dump gültigen Stand zurücksetzen
    }
}

// Legt das Semaphor an und startet den wav_dump_task angepinnt auf Core 0
// mit niedriger Priorität. Muss einmalig beim Systemstart aufgerufen werden,
// bevor wav_dump_trigger() benutzt werden kann.
void wav_dump_init(void)
{
    s_sem = xSemaphoreCreateBinary();                              // [HW] legt ein binäres Semaphor im Heap an (FreeRTOS-Objekt)
    assert(s_sem);                                                 // [C] Laufzeitprüfung: bricht ab, falls Allokation fehlschlug (sollte bei ausreichend Heap nie passieren)
    BaseType_t ok = xTaskCreatePinnedToCore(wav_dump_task, "wav_dump",  // [HW] erzeugt Task fest gebunden an Core 0 (verhindert Störung des AFE-Tasks auf Core 1)
                                            4096, NULL, 2, NULL, 0);    // [Logik] 4096 Byte Stack, kein Task-Parameter, Priorität 2 (niedrig), kein Handle-Rückgabewert, Core-ID 0
    assert(ok == pdPASS);                                          // [C] Vergleich mit pdPASS (FreeRTOS-Erfolgskonstante) | [Logik] Abbruch bei fehlgeschlagener Task-Erzeugung
}

// Übergibt einen Puffer zum Dumpen an den Hintergrund-Task und weckt ihn per
// Semaphor auf. Nicht-blockierend, von beliebigem Task-Kontext aufrufbar.
// data muss bis zum Ende des Dumps gültig bleiben (siehe Header-Kommentar).
void wav_dump_trigger(const uint8_t *data, size_t bytes)
{
    s_data = data;                    // [Logik] Zeiger auf den zu dumpenden Puffer für den Task hinterlegen
    s_bytes = bytes;                  // [Logik] zugehörige Länge hinterlegen
    xSemaphoreGive(s_sem);            // [C] gibt das Semaphor frei | [Logik] weckt wav_dump_task auf, ohne selbst zu blockieren
}

#else // !CONFIG_RECORDING_WAV_DUMP

#include "wav_dump.h"                 // [C] Header auch im deaktivierten Zweig einbinden, damit Prototypen zu den No-Op-Definitionen passen

// No-Op-Variante, wenn CONFIG_RECORDING_WAV_DUMP deaktiviert ist: tut nichts,
// damit Aufrufer nicht per #ifdef unterscheiden müssen.
void wav_dump_init(void)
{
}

// No-Op-Variante: verwirft die übergebenen Parameter, wenn das WAV-Dump-
// Feature per Kconfig ausgeschaltet ist.
void wav_dump_trigger(const uint8_t *data, size_t bytes)
{
    (void)data;                       // [C] explizites Verwerfen, unterdrückt "unused parameter"-Warnung
    (void)bytes;                      // [C] explizites Verwerfen, unterdrückt "unused parameter"-Warnung
}

#endif
