// Kitchen-Timer (Spec Section 7): ein One-Shot-esp_timer, die Restzeit
// rechnet sich aus der gespeicherten Deadline — so braucht der
// 1-Hz-Countdown in der Status-Bar keinen eigenen Tick-Timer.
#include <stdatomic.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "canned_audio.h"
#include "timer.h"

static const char *TAG = "kitchen_timer";

static esp_timer_handle_t s_timer;
static kitchen_timer_done_cb_t s_done_cb;
static atomic_llong s_deadline_us;  // 0 = kein Timer aktiv

static void timer_done(void *arg)
{
    atomic_store(&s_deadline_us, 0);
    ESP_LOGI(TAG, "timer done");
    canned_audio_play(CANNED_TIMER_DONE);
    canned_audio_play(CANNED_ALARM);
    if (s_done_cb) {
        s_done_cb();
    }
}

void kitchen_timer_init(kitchen_timer_done_cb_t done_cb)
{
    s_done_cb = done_cb;
    const esp_timer_create_args_t args = {
        .callback = timer_done,
        .name = "kitchen_timer",
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_timer));
}

void kitchen_timer_start(int seconds)
{
    esp_timer_stop(s_timer);  // ESP_ERR_INVALID_STATE wenn nicht aktiv: ok
    atomic_store(&s_deadline_us,
                 esp_timer_get_time() + (int64_t)seconds * 1000000);
    ESP_ERROR_CHECK(esp_timer_start_once(s_timer,
                                         (uint64_t)seconds * 1000000));
    ESP_LOGI(TAG, "timer started: %d s", seconds);
}

bool kitchen_timer_cancel(void)
{
    if (atomic_exchange(&s_deadline_us, 0) == 0) {
        return false;
    }
    esp_timer_stop(s_timer);
    ESP_LOGI(TAG, "timer cancelled");
    return true;
}

int kitchen_timer_remaining_s(void)
{
    long long deadline = atomic_load(&s_deadline_us);
    if (deadline == 0) {
        return -1;
    }
    long long left_us = deadline - esp_timer_get_time();
    if (left_us < 0) {
        left_us = 0;
    }
    return (int)((left_us + 999999) / 1000000);
}
