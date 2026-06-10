// Wake-Word-Handling (Spec Section 6). In Phase 2 nur Log + Listener;
// die State-Machine (LISTENING etc.) kommt in Phase 3/4.
#include "esp_log.h"

#include "wake_word.h"

static const char *TAG = "wake_word";

static wake_word_listener_t s_listener;

void wake_word_set_listener(wake_word_listener_t listener)
{
    s_listener = listener;
}

void wake_word_on_detected(int word_index)
{
    ESP_LOGI(TAG, "Wake word detected (index %d)", word_index);
    if (s_listener) {
        s_listener();
    }
}
