// MultiNet7-EN + die 14 lokalen Commands (Spec Section 7).
// Das Model liegt seit Phase 2 in der model-Partition; hier wird es
// geladen und vom afe_fetch_task parallel zum Recording gefuettert
// (Spec "Audio-Fetch-Task"-Geruest).
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"

#include "commands.h"

static const char *TAG = "commands";

// MultiNet-Timeout: so lange darf nach dem Wake Word gesprochen werden,
// bis MultiNet aufgibt (laengere Aeusserungen sind Cloud-Queries).
#define MN_TIMEOUT_MS 6000

static const struct {
    command_id_t id;
    const char *phrase;
} COMMANDS[] = {
    { CMD_TIMER_1MIN,   "set timer one minute" },
    { CMD_TIMER_5MIN,   "set timer five minutes" },
    { CMD_TIMER_10MIN,  "set timer ten minutes" },
    { CMD_TIMER_15MIN,  "set timer fifteen minutes" },
    { CMD_TIMER_30MIN,  "set timer thirty minutes" },
    { CMD_TIMER_30SEC,  "set timer thirty seconds" },
    { CMD_TIMER_STOP,   "stop timer" },
    { CMD_TIMER_CANCEL, "cancel timer" },
    { CMD_TIME_LEFT,    "how much time left" },
    { CMD_STOP,         "stop" },
    { CMD_VOLUME_UP,    "volume up" },
    { CMD_VOLUME_DOWN,  "volume down" },
    { CMD_MUTE,         "mute" },
    { CMD_UNMUTE,       "unmute" },
};

static const esp_mn_iface_t *s_multinet;
static model_iface_data_t *s_mn_data;
static commands_match_cb_t s_match_cb;

static int16_t *s_chunk;        // Sammel-Buffer: 1 MultiNet-Chunk
static size_t s_chunk_samples;  // dessen Groesse
static size_t s_chunk_fill;
static bool s_active;

esp_err_t commands_init(srmodel_list_t *models, commands_match_cb_t cb)
{
    char *mn_name = models ? esp_srmodel_filter(models, ESP_MN_PREFIX,
                                                ESP_MN_ENGLISH)
                           : NULL;
    if (!mn_name) {
        ESP_LOGE(TAG, "no English MultiNet in model partition");
        return ESP_FAIL;
    }

    s_multinet = esp_mn_handle_from_name(mn_name);
    s_mn_data = s_multinet->create(mn_name, MN_TIMEOUT_MS);
    if (!s_mn_data) {
        ESP_LOGE(TAG, "MultiNet create failed (%s)", mn_name);
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK(esp_mn_commands_alloc((esp_mn_iface_t *)s_multinet,
                                          s_mn_data));
    for (size_t i = 0; i < sizeof(COMMANDS) / sizeof(COMMANDS[0]); i++) {
        ESP_ERROR_CHECK(esp_mn_commands_add(COMMANDS[i].id,
                                            COMMANDS[i].phrase));
    }
    esp_mn_error_t *err = esp_mn_commands_update();
    if (err && err->num > 0) {
        for (int i = 0; i < err->num; i++) {
            ESP_LOGE(TAG, "rejected phrase: \"%s\"",
                     err->phrases[i]->string);
        }
        return ESP_FAIL;
    }

    s_chunk_samples = (size_t)s_multinet->get_samp_chunksize(s_mn_data);
    s_chunk = heap_caps_malloc(s_chunk_samples * sizeof(int16_t),
                               MALLOC_CAP_INTERNAL);
    if (!s_chunk) {
        return ESP_ERR_NO_MEM;
    }

    s_match_cb = cb;
    ESP_LOGI(TAG, "%s ready: %d commands, chunk %u samples, timeout %d ms",
             mn_name, (int)(sizeof(COMMANDS) / sizeof(COMMANDS[0])),
             (unsigned)s_chunk_samples, MN_TIMEOUT_MS);
    return ESP_OK;
}

void commands_session_start(void)
{
    s_multinet->clean(s_mn_data);
    s_chunk_fill = 0;
    s_active = true;
}

void commands_session_stop(void)
{
    s_active = false;
}

bool commands_session_active(void)
{
    return s_active;
}

static void detect_chunk(int16_t *chunk)
{
    esp_mn_state_t state = s_multinet->detect(s_mn_data, chunk);
    if (state == ESP_MN_STATE_DETECTING) {
        return;
    }

    s_active = false;
    if (state == ESP_MN_STATE_TIMEOUT) {
        ESP_LOGI(TAG, "no local command (timeout) -> cloud query");
        return;
    }

    esp_mn_results_t *res = s_multinet->get_results(s_mn_data);
    if (res->num <= 0) {
        return;
    }
    ESP_LOGI(TAG, "command %d \"%s\" (prob %.2f)", res->command_id[0],
             res->string, res->prob[0]);
    if (s_match_cb) {
        s_match_cb((command_id_t)res->command_id[0], res->string);
    }
}

void commands_feed(const int16_t *data, size_t samples)
{
    while (s_active && samples > 0) {
        size_t n = s_chunk_samples - s_chunk_fill;
        if (n > samples) {
            n = samples;
        }
        memcpy(s_chunk + s_chunk_fill, data, n * sizeof(int16_t));
        s_chunk_fill += n;
        data += n;
        samples -= n;

        if (s_chunk_fill == s_chunk_samples) {
            s_chunk_fill = 0;
            detect_chunk(s_chunk);
        }
    }
}
