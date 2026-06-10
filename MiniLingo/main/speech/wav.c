// WAV-Header-Generierung (Spec Section 9.2): klassischer 44-Byte
// RIFF/PCM-Header. Felder sind little-endian — der ESP32 ist es auch,
// aber wir schreiben byteweise, um keine Alignment-Annahmen auf dem
// (PSRAM-)Zielbuffer zu machen.
#include <string.h>

#include "wav.h"

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

void wav_write_header(uint8_t *dest, uint32_t pcm_bytes,
                      uint32_t sample_rate, uint16_t channels,
                      uint16_t bits_per_sample)
{
    uint16_t block_align = channels * (bits_per_sample / 8);

    memcpy(dest, "RIFF", 4);
    put_u32(dest + 4, 36 + pcm_bytes);          // Datei-Rest ab Offset 8
    memcpy(dest + 8, "WAVEfmt ", 8);
    put_u32(dest + 16, 16);                     // fmt-Chunk-Laenge
    put_u16(dest + 20, 1);                      // AudioFormat: PCM
    put_u16(dest + 22, channels);
    put_u32(dest + 24, sample_rate);
    put_u32(dest + 28, sample_rate * block_align);
    put_u16(dest + 32, block_align);
    put_u16(dest + 34, bits_per_sample);
    memcpy(dest + 36, "data", 4);
    put_u32(dest + 40, pcm_bytes);
}
