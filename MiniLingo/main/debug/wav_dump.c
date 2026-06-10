// Phase-3-Testhilfe: WAV-Buffer Base64-codiert ueber UART rausziehen
// (Spec Phase 3: "Test: WAV-Buffer via UART/SPIFFS rausziehen und in
// Audacity oeffnen"). Eigener Task mit niedriger Prio auf Core 0,
// damit der blockierende UART-Write (8 s Audio ~ 341 KB Base64 ~ 30 s
// bei 115200 Baud) nicht den afe_fetch_task aufhaelt.
#include <stdio.h>

#include "sdkconfig.h"

#if CONFIG_RECORDING_WAV_DUMP

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "mbedtls/base64.h"

#include "wav_dump.h"

// 480 Rohbytes -> 640 Base64-Zeichen pro Zeile (+ NUL)
#define CHUNK_BYTES 480

static SemaphoreHandle_t s_sem;
static const uint8_t *s_data;
static size_t s_bytes;

static void wav_dump_task(void *arg)
{
    static char line[CHUNK_BYTES / 3 * 4 + 1];

    while (1) {
        xSemaphoreTake(s_sem, portMAX_DELAY);

        printf("\nWAV_DUMP_BEGIN bytes=%u\n", (unsigned)s_bytes);
        for (size_t off = 0; off < s_bytes; off += CHUNK_BYTES) {
            size_t n = s_bytes - off;
            if (n > CHUNK_BYTES) {
                n = CHUNK_BYTES;
            }
            size_t olen = 0;
            mbedtls_base64_encode((unsigned char *)line, sizeof(line),
                                  &olen, s_data + off, n);
            line[olen] = '\0';
            printf("%s\n", line);
        }
        printf("WAV_DUMP_END\n");
    }
}

void wav_dump_init(void)
{
    s_sem = xSemaphoreCreateBinary();
    assert(s_sem);
    BaseType_t ok = xTaskCreatePinnedToCore(wav_dump_task, "wav_dump",
                                            4096, NULL, 2, NULL, 0);
    assert(ok == pdPASS);
}

void wav_dump_trigger(const uint8_t *data, size_t bytes)
{
    s_data = data;
    s_bytes = bytes;
    xSemaphoreGive(s_sem);
}

#else // !CONFIG_RECORDING_WAV_DUMP

#include "wav_dump.h"

void wav_dump_init(void)
{
}

void wav_dump_trigger(const uint8_t *data, size_t bytes)
{
    (void)data;
    (void)bytes;
}

#endif
