// Output-Gain-Management (Spec Section 12, Phase 4): Prozent-Stufen
// 0/20/40/60/80/100, linear auf Q15 gemappt und an audio_out gereicht.
#include <stdatomic.h>

#include "esp_log.h"

#include "drivers/audio_out.h"
#include "volume.h"

static const char *TAG = "volume";

static atomic_int s_percent;
static atomic_int s_before_mute;

static void apply(int percent)
{
    atomic_store(&s_percent, percent);
    audio_out_set_gain_q15(AUDIO_OUT_GAIN_UNITY * percent / 100);
    ESP_LOGI(TAG, "volume %d%%", percent);
}

void volume_init(int initial_percent)
{
    atomic_store(&s_before_mute, initial_percent);
    apply(initial_percent);
}

int volume_get(void)
{
    return atomic_load(&s_percent);
}

void volume_set(int percent)
{
    if (percent < 0) {
        percent = 0;
    } else if (percent > 100) {
        percent = 100;
    }
    apply(percent);
}

void volume_adjust(int delta)
{
    volume_set(atomic_load(&s_percent) + delta);
}

void volume_mute(void)
{
    int cur = atomic_load(&s_percent);
    if (cur > 0) {
        atomic_store(&s_before_mute, cur);
    }
    apply(0);
}

void volume_unmute(void)
{
    apply(atomic_load(&s_before_mute));
}
