// Recording-Buffer (Spec Section 9.1): nach dem Wake Word schreibt der
// afe_fetch_task die AFE-Frames hierher. Capture endet nach 1,5 s
// zusammenhaengender Stille (VAD aus dem AFE-Result) oder dem
// 8-s-Hard-Limit. Danach steht im Buffer eine fertige WAV-Datei
// (Header + PCM) — Phase 5 schickt genau diesen Buffer an Whisper.
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "wav.h"
#include "recorder.h"

static const char *TAG = "recorder";

#define SAMPLE_RATE       16000
#define MAX_RECORD_MS     8000   // Hard-Limit (Spec Phase 3)
#define SILENCE_STOP_MS   1500   // Capture-Ende bei Stille (Spec Phase 3)

#define PCM_MAX_BYTES (MAX_RECORD_MS * (SAMPLE_RATE / 1000) * 2)
#define BYTES_PER_MS  ((SAMPLE_RATE / 1000) * 2)

static recorder_done_cb_t s_done_cb;
static uint8_t *s_buf;        // PSRAM: WAV_HEADER_BYTES + PCM_MAX_BYTES
static size_t s_pcm_bytes;
static uint32_t s_silence_ms;
static bool s_active;

esp_err_t recorder_init(recorder_done_cb_t cb)
{
    s_buf = heap_caps_malloc(WAV_HEADER_BYTES + PCM_MAX_BYTES,
                             MALLOC_CAP_SPIRAM);
    if (!s_buf) {
        ESP_LOGE(TAG, "no PSRAM for recording buffer (%d bytes)",
                 WAV_HEADER_BYTES + PCM_MAX_BYTES);
        return ESP_ERR_NO_MEM;
    }
    s_done_cb = cb;
    ESP_LOGI(TAG, "recording buffer ready (%d KB PSRAM, max %d ms)",
             (WAV_HEADER_BYTES + PCM_MAX_BYTES) / 1024, MAX_RECORD_MS);
    return ESP_OK;
}

void recorder_start(void)
{
    s_pcm_bytes = 0;
    s_silence_ms = 0;
    s_active = true;
    ESP_LOGI(TAG, "capture started");
}

bool recorder_is_active(void)
{
    return s_active;
}

static void finish(recorder_stop_reason_t reason)
{
    s_active = false;
    wav_write_header(s_buf, (uint32_t)s_pcm_bytes, SAMPLE_RATE, 1, 16);

    uint32_t duration_ms = (uint32_t)(s_pcm_bytes / BYTES_PER_MS);
    ESP_LOGI(TAG, "capture done: %u ms, %u bytes WAV (%s)",
             (unsigned)duration_ms,
             (unsigned)(WAV_HEADER_BYTES + s_pcm_bytes),
             reason == RECORDER_STOP_SILENCE ? "silence" : "buffer full");

    if (s_done_cb) {
        s_done_cb(s_buf, WAV_HEADER_BYTES + s_pcm_bytes, duration_ms,
                  reason);
    }
}

void recorder_feed(const int16_t *data, size_t bytes, bool is_speech)
{
    if (!s_active || bytes == 0) {
        return;
    }

    size_t room = PCM_MAX_BYTES - s_pcm_bytes;
    size_t n = bytes < room ? bytes : room;
    memcpy(s_buf + WAV_HEADER_BYTES + s_pcm_bytes, data, n);
    s_pcm_bytes += n;

    if (s_pcm_bytes >= PCM_MAX_BYTES) {
        finish(RECORDER_STOP_FULL);
        return;
    }

    if (is_speech) {
        s_silence_ms = 0;
    } else {
        s_silence_ms += bytes / BYTES_PER_MS;
        if (s_silence_ms >= SILENCE_STOP_MS) {
            finish(RECORDER_STOP_SILENCE);
        }
    }
}
