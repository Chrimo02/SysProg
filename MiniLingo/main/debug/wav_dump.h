#pragma once                         // [C] Include-Guard-Ersatz: verhindert Mehrfach-Einbindung dieses Headers

#include <stddef.h>                  // [C] Standardbibliothek für size_t
#include <stdint.h>                  // [C] Standardbibliothek für feste Integer-Breiten (uint8_t)

// Phase-3-Testhilfe (CONFIG_RECORDING_WAV_DUMP): gibt den fertigen
// WAV-Buffer Base64-codiert ueber UART aus. Aus dem gespeicherten
// Monitor-Log extrahiert tools/extract_wav.py die Datei fuer Audacity.
// Bei deaktivierter Option sind beide Funktionen No-Ops.

// Initialisiert die WAV-Dump-Infrastruktur (Semaphor + Hintergrund-Task).
// Muss einmalig beim Systemstart aufgerufen werden, bevor wav_dump_trigger()
// benutzt wird.
void wav_dump_init(void);

// Von beliebigem Task-Kontext aufrufbar (gibt nur ein Semaphor an den
// Dump-Task). data muss bis zum Ende des Dumps gueltig bleiben — beim
// Recorder-Buffer ist das erfuellt, solange kein neues Capture startet.
void wav_dump_trigger(const uint8_t *data, size_t bytes);
