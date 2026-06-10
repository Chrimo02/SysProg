// MiniLingo Phase 2: WiFi + esp-sr + Wake Word.
// app_main initialisiert LVGL/Display/Touch/UI (Phase 1), dazu NVS,
// WiFi-STA und die AFE-Pipeline mit WakeNet9-Alexa.
//
// Thread-Grenzen: WiFi-Events (Event-Loop-Task) und Wake-Word-Events
// (afe_fetch_task, Core 1) duerfen LVGL nicht direkt anfassen — sie
// setzen atomare Flags, die ein LVGL-Timer (app_event_timer_cb, laeuft
// im lvgl_task) abholt und in UI-Updates uebersetzt.
#include <stdatomic.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "lvgl.h"

#include "drivers/audio_in.h"
#include "drivers/display.h"
#include "drivers/touch.h"
#include "network/wifi.h"
#include "speech/afe_pipeline.h"
#include "speech/wake_word.h"
#include "ui/ui.h"

static const char *TAG = "main";

#define IDLE_STATE_TEXT "Listening for Alexa"
// Solange es noch keine Capture-Phase gibt (Phase 3), faellt der
// Button nach dieser Zeit wieder auf Idle zurueck.
#define WAKE_UI_RESET_MS 3000

// -1 = keine Aenderung, 0 = getrennt, 1 = verbunden
static atomic_int s_wifi_event = -1;
static atomic_bool s_wake_event;
static uint32_t s_wake_reset_at;

static uint32_t lvgl_tick_get_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void lvgl_task(void *arg)
{
    while (1) {
        uint32_t delay_ms = lv_timer_handler();
        if (delay_ms == LV_NO_TIMER_READY) {
            delay_ms = 20;
        }
        delay_ms = LV_CLAMP(1, delay_ms, 20);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

static void on_mic_button_clicked(void)
{
    // Laeuft im LVGL-Task-Kontext (Klick-Event), UI-Calls sind hier OK.
    ui_chat_add_local_bubble("Mic-Button gedrueckt!");
}

static void on_wifi_status(bool connected)
{
    atomic_store(&s_wifi_event, connected ? 1 : 0);
}

static void on_wake_word(void)
{
    atomic_store(&s_wake_event, true);
}

// LVGL-Timer: holt Events aus den anderen Task-Kontexten ab.
static void app_event_timer_cb(lv_timer_t *timer)
{
    int wifi = atomic_exchange(&s_wifi_event, -1);
    if (wifi >= 0) {
        ui_status_bar_set_wifi_connected(wifi == 1);
    }

    if (atomic_exchange(&s_wake_event, false)) {
        ui_mic_button_set_state(UI_MIC_LISTENING);
        ui_status_bar_set_state_text("Listening...");
        ui_chat_add_local_bubble("Wake word detected!");
        s_wake_reset_at = lv_tick_get() + WAKE_UI_RESET_MS;
    }

    if (s_wake_reset_at != 0 &&
        (int32_t)(lv_tick_get() - s_wake_reset_at) >= 0) {
        s_wake_reset_at = 0;
        ui_mic_button_set_state(UI_MIC_IDLE);
        ui_status_bar_set_state_text(IDLE_STATE_TEXT);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "MiniLingo Phase 2 starting");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    lv_init();
    lv_tick_set_cb(lvgl_tick_get_cb);

    ESP_ERROR_CHECK(display_init());
    ESP_ERROR_CHECK(touch_init());

    ui_init();
    ui_mic_button_set_click_cb(on_mic_button_clicked);
    ui_status_bar_set_state_text(IDLE_STATE_TEXT);
    ui_status_bar_set_wifi_connected(false);
    ui_status_bar_set_volume(60);
    lv_timer_create(app_event_timer_cb, 50, NULL);

    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 8192, NULL, 4, NULL, 0);

    ESP_ERROR_CHECK(wifi_init(on_wifi_status));

    wake_word_set_listener(on_wake_word);
    ESP_ERROR_CHECK(audio_in_init());
    ESP_ERROR_CHECK(afe_pipeline_init());

    ESP_LOGI(TAG, "Phase 2 up: UI + WiFi + wake word pipeline running");
}
