// WAV-Header-Generierung (Spec Section 9.2): klassischer 44-Byte
// RIFF/PCM-Header. Felder sind little-endian — der ESP32 ist es auch,
// aber wir schreiben byteweise, um keine Alignment-Annahmen auf dem
// (PSRAM-)Zielbuffer zu machen.
#include <string.h>                                            // [C] Header-Include für memcpy

#include "wav.h"                                               // [C] eigener Header: Prototyp-Abgleich für wav_write_header

// Schreibt einen 16-bit-Wert little-endian (niederwertiges Byte zuerst)
// in den Zielpuffer — byteweise statt per Pointer-Cast, um Alignment-
// Probleme auf dem PSRAM-Buffer zu vermeiden.
static void put_u16(uint8_t *p, uint16_t v)                    // [C] static = datei-lokale Hilfsfunktion, keine externe Linkage
{
    p[0] = (uint8_t)v;                                          // [C] Cast auf 8 bit schneidet die oberen Bits ab -> niederwertiges Byte | [HW] little-endian: Byte 0 = LSB
    p[1] = (uint8_t)(v >> 8);                                   // [C] Rechtsshift um 8 bit + Cast -> höherwertiges Byte | [HW] little-endian Byte 1 = MSB
}

// Schreibt einen 32-bit-Wert little-endian (analog zu put_u16, byteweise
// in 4 Schritten) — für die Groessenfelder im WAV-Header.
static void put_u32(uint8_t *p, uint32_t v)                    // [C] static Hilfsfunktion, nur in dieser Datei sichtbar
{
    p[0] = (uint8_t)v;                                          // [C] LSB zuerst | [HW] little-endian-Byte-Reihenfolge wie von RIFF/WAV verlangt
    p[1] = (uint8_t)(v >> 8);                                   // [C] zweites Byte per Shift+Cast extrahiert
    p[2] = (uint8_t)(v >> 16);                                  // [C] drittes Byte
    p[3] = (uint8_t)(v >> 24);                                  // [C] MSB zuletzt
}

// Baut den 44-Byte-RIFF/WAVE-Header direkt im Recording-Buffer vor den
// PCM-Samples auf (Spec Section 9.2), damit der Buffer als Ganzes eine
// gültige .wav-Datei für den Whisper-Upload ergibt.
void wav_write_header(uint8_t *dest, uint32_t pcm_bytes,
                      uint32_t sample_rate, uint16_t channels,
                      uint16_t bits_per_sample)                 // [C] Funktionsdefinition, Parameter beschreiben das gewünschte PCM-Format
{
    uint16_t block_align = channels * (bits_per_sample / 8);   // [HW] Bytes pro Sample-Frame (alle Kanäle) = WAV-Feld "BlockAlign"

    memcpy(dest, "RIFF", 4);                                    // [C] memcpy kopiert Bytes ohne Terminierung nötig | [HW] RIFF-Chunk-ID, Magic-Bytes am Dateianfang
    put_u32(dest + 4, 36 + pcm_bytes);          // Datei-Rest ab Offset 8 // [HW] ChunkSize = Restgröße der Datei ohne die ersten 8 Bytes
    memcpy(dest + 8, "WAVEfmt ", 8);                            // [HW] WAVE-Format-Kennung + "fmt "-Sub-Chunk-ID
    put_u32(dest + 16, 16);                     // fmt-Chunk-Laenge // [HW] Länge des fmt-Sub-Chunks: 16 Byte für PCM ohne Extension
    put_u16(dest + 20, 1);                      // AudioFormat: PCM  // [HW] AudioFormat=1 bedeutet unkomprimiertes PCM
    put_u16(dest + 22, channels);                               // [HW] Anzahl Kanäle (hier: 1 = mono)
    put_u32(dest + 24, sample_rate);                            // [HW] Abtastrate in Hz (16000)
    put_u32(dest + 28, sample_rate * block_align);              // [HW] ByteRate = Bytes pro Sekunde, für Wiedergabe-Timing benötigt
    put_u16(dest + 32, block_align);                            // [HW] BlockAlign = Bytes pro Sample-Frame
    put_u16(dest + 34, bits_per_sample);                        // [HW] Bit-Tiefe pro Sample (16 bit)
    memcpy(dest + 36, "data", 4);                               // [HW] "data"-Sub-Chunk-ID, danach folgen die PCM-Samples
    put_u32(dest + 40, pcm_bytes);                              // [HW] Länge der PCM-Nutzdaten in Byte
}
