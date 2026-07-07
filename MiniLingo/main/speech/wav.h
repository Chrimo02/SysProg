#pragma once

#include <stdint.h>

// PCM-WAV-Header (RIFF), Spec Section 9.2. Wird direkt vor die Samples
// in den Recording-Buffer geschrieben, damit der Buffer als Ganzes
// upload-fertig ist (Phase 5: Whisper-Multipart).
#define WAV_HEADER_BYTES 44   // [C] Präprozessor-Konstante | [HW] fixe Größe eines minimalen RIFF/PCM-Headers ohne Extra-Chunks

// Schreibt die 44 Header-Bytes nach dest. pcm_bytes ist die Laenge der
// nachfolgenden Sample-Daten (ohne Header).
// Erzeugt den WAV-Header vor den rohen PCM-Samples, damit der Buffer als
// Ganzes eine gültige .wav-Datei ergibt (für den Whisper-Upload).
void wav_write_header(uint8_t *dest, uint32_t pcm_bytes,          // [C] Funktionsprototyp/Deklaration, dest = Ziel-Byte-Puffer (Zeiger)
                      uint32_t sample_rate, uint16_t channels,
                      uint16_t bits_per_sample);
