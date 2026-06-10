// MiniLingo Phase 3: Audio-Capture + Recording-Buffer.
// app_main initialisiert LVGL/Display/Touch/UI (Phase 1), NVS, WiFi-STA
// und die AFE-Pipeline mit WakeNet9-Alexa (Phase 2); nach dem Wake Word
// laeuft jetzt das Capture in den PSRAM-Recording-Buffer (Phase 3).
//
// Thread-Grenzen: WiFi-Events (Event-Loop-Task), Wake-Word- und
// Recorder-Events (afe_fetch_task, Core 1) duerfen LVGL nicht direkt
// anfassen — sie setzen atomare Flags, die ein LVGL-Timer
// (app_event_timer_cb, laeuft im lvgl_task) abholt und in UI-Updates
// uebersetzt.
#include <stdatomic.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "lvgl.h"

#include "debug/wav_dump.h"
#include "drivers/audio_in.h"
#include "drivers/display.h"
#include "drivers/touch.h"
#include "network/wifi.h"
#include "speech/afe_pipeline.h"
#include "speech/recorder.h"
#include "speech/wake_word.h"
#include "ui/ui.h"

static const char *TAG = "main";

#define IDLE_STATE_TEXT "Listening for Alexa"

// -1 = keine Aenderung, 0 = getrennt, 1 = verbunden
static atomic_int s_wifi_event = -1;
static atomic_bool s_wake_event;
static atomic_bool s_rec_done_event;
static atomic_uint s_rec_duration_ms;
static atomic_uint s_rec_wav_kb;

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

// Laeuft im afe_fetch_task. Der WAV-Buffer bleibt bis zum naechsten
// recorder_start() gueltig — der Dump-Task liest ihn nur.
static void on_recording_done(const uint8_t *wav, size_t wav_bytes,
                              uint32_t duration_ms,
                              recorder_stop_reason_t reason)
{
    wav_dump_trigger(wav, wav_bytes);
    atomic_store(&s_rec_duration_ms, duration_ms);
    atomic_store(&s_rec_wav_kb, (unsigned)(wav_bytes / 1024));
    atomic_store(&s_rec_done_event, true);
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
    }

    if (atomic_exchange(&s_rec_done_event, false)) {
        unsigned ms = atomic_load(&s_rec_duration_ms);
        unsigned kb = atomic_load(&s_rec_wav_kb);
        char text[48];
        snprintf(text, sizeof(text), "Captured %u.%us audio (%u KB)",
                 ms / 1000, (ms % 1000) / 100, kb);
        ui_chat_add_local_bubble(text);
        ui_mic_button_set_state(UI_MIC_IDLE);
        ui_status_bar_set_state_text(IDLE_STATE_TEXT);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "MiniLingo Phase 3 starting");

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

    wav_dump_init();
    wake_word_set_listener(on_wake_word);
    ESP_ERROR_CHECK(recorder_init(on_recording_done));
    ESP_ERROR_CHECK(audio_in_init());
    ESP_ERROR_CHECK(afe_pipeline_init());

    ESP_LOGI(TAG, "Phase 3 up: wake word + audio capture running");
}
