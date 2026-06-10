// Canned-TTS-Wiedergabe (Spec Section 7 "Pre-Generated Audio-Responses"):
// vorab generierte WAVs aus der SPIFFS-Partition "storage" -> I2S1.
// Der Player-Task entspricht dem audio_out_task der Spec-Task-Tabelle
// (Prio 5, Core 1); ab Phase 7 konsumiert er zusaetzlich die tts_queue.
//
// WAV-Parser laeuft chunk-weise (RIFF), weil afconvert (say-Fallback
// des Generator-Scripts) einen FLLR-Padding-Chunk vor "data" einfuegt —
// ein fixer 44-Byte-Skip wuerde dort Muell abspielen.
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_spiffs.h"

#include "drivers/audio_out.h"
#include "canned_audio.h"

static const char *TAG = "canned_audio";

#define MOUNT_POINT "/spiffs"
#define MAX_NAME    40
#define CHUNK_BYTES 4096

static const char *EXPECTED_FILES[] = {
    CANNED_TIMER_SET_30S, CANNED_TIMER_SET_60S, CANNED_TIMER_SET_300S,
    CANNED_TIMER_SET_600S, CANNED_TIMER_SET_900S, CANNED_TIMER_SET_1800S,
    CANNED_TIMER_CANCELLED, CANNED_TIMER_DONE, CANNED_NO_TIMER,
    CANNED_TIME_LEFT, CANNED_VOLUME_CHANGED, CANNED_MUTED, CANNED_UNMUTED,
    CANNED_ALARM,
};

static QueueHandle_t s_queue;
static atomic_bool s_stop;

// Liest den RIFF-Header, springt zum "data"-Chunk und liefert dessen
// Laenge; *sample_rate aus dem fmt-Chunk. Nur PCM/16-bit/mono.
static bool wav_open_data(FILE *f, uint32_t *sample_rate,
                          uint32_t *data_bytes)
{
    uint8_t hdr[12];
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) != 0 ||
        memcmp(hdr + 8, "WAVE", 4) != 0) {
        return false;
    }

    bool fmt_ok = false;
    uint8_t chunk[8];
    while (fread(chunk, 1, 8, f) == 8) {
        uint32_t size = chunk[4] | chunk[5] << 8 | chunk[6] << 16 |
                        (uint32_t)chunk[7] << 24;
        if (memcmp(chunk, "fmt ", 4) == 0 && size >= 16) {
            uint8_t fmt[16];
            if (fread(fmt, 1, 16, f) != 16) {
                return false;
            }
            uint16_t format = fmt[0] | fmt[1] << 8;
            uint16_t channels = fmt[2] | fmt[3] << 8;
            uint16_t bits = fmt[14] | fmt[15] << 8;
            *sample_rate = fmt[4] | fmt[5] << 8 | fmt[6] << 16 |
                           (uint32_t)fmt[7] << 24;
            if (format != 1 || channels != 1 || bits != 16) {
                ESP_LOGE(TAG, "unsupported WAV (fmt %u, %u ch, %u bit)",
                         format, channels, bits);
                return false;
            }
            fmt_ok = true;
            if (fseek(f, (long)(size - 16 + (size & 1)), SEEK_CUR) != 0) {
                return false;
            }
        } else if (memcmp(chunk, "data", 4) == 0) {
            *data_bytes = size;
            return fmt_ok;
        } else if (fseek(f, (long)(size + (size & 1)), SEEK_CUR) != 0) {
            return false;
        }
    }
    return false;
}

static void play_file(const char *name, int16_t *buf)
{
    char path[sizeof(MOUNT_POINT) + MAX_NAME];
    snprintf(path, sizeof(path), MOUNT_POINT "/%s", name);

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "missing %s", path);
        return;
    }

    uint32_t rate = 0;
    uint32_t left = 0;
    if (!wav_open_data(f, &rate, &left)) {
        ESP_LOGE(TAG, "bad WAV: %s", path);
        fclose(f);
        return;
    }

    ESP_LOGI(TAG, "playing %s (%u Hz, %u bytes)", name, (unsigned)rate,
             (unsigned)left);
    audio_out_set_rate(rate);

    while (left > 0 && !atomic_load(&s_stop)) {
        size_t want = left < CHUNK_BYTES ? left : CHUNK_BYTES;
        size_t got = fread(buf, 1, want, f);
        if (got == 0) {
            break;
        }
        audio_out_write(buf, got / sizeof(int16_t));
        left -= got;
    }
    fclose(f);
}

static void player_task(void *arg)
{
    static int16_t chunk_buf[CHUNK_BYTES / sizeof(int16_t)];
    char name[MAX_NAME];

    while (1) {
        if (xQueueReceive(s_queue, name, portMAX_DELAY) == pdTRUE) {
            atomic_store(&s_stop, false);
            play_file(name, chunk_buf);
        }
    }
}

esp_err_t canned_audio_init(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = MOUNT_POINT,
        .partition_label = "storage",
        .max_files = 2,
        .format_if_mount_failed = false,
    };
    ESP_RETURN_ON_ERROR(esp_vfs_spiffs_register(&conf), TAG,
                        "spiffs mount (storage geflasht?)");

    int missing = 0;
    for (size_t i = 0;
         i < sizeof(EXPECTED_FILES) / sizeof(EXPECTED_FILES[0]); i++) {
        char path[sizeof(MOUNT_POINT) + MAX_NAME];
        struct stat st;
        snprintf(path, sizeof(path), MOUNT_POINT "/%s", EXPECTED_FILES[i]);
        if (stat(path, &st) != 0) {
            ESP_LOGW(TAG, "canned file missing: %s", path);
            missing++;
        }
    }

    s_queue = xQueueCreate(4, MAX_NAME);
    if (!s_queue) {
        return ESP_ERR_NO_MEM;
    }
    BaseType_t ok = xTaskCreatePinnedToCore(player_task, "audio_out", 4096,
                                            NULL, 5, NULL, 1);
    if (ok != pdPASS) {
        return ESP_FAIL;
    }

    // Fehlende Files sind kein Boot-Abbruch: der jeweilige Command
    // bleibt dann nur stumm (play_file loggt es erneut).
    ESP_LOGI(TAG, "SPIFFS mounted, %d canned file(s) missing", missing);
    return ESP_OK;
}

void canned_audio_play(const char *name)
{
    char item[MAX_NAME];
    if (strlen(name) >= MAX_NAME) {
        ESP_LOGE(TAG, "name too long: %s", name);
        return;
    }
    strcpy(item, name);
    if (xQueueSend(s_queue, item, 0) != pdTRUE) {
        ESP_LOGW(TAG, "play queue full, dropping %s", name);
    }
}

void canned_audio_stop(void)
{
    xQueueReset(s_queue);
    atomic_store(&s_stop, true);
}
