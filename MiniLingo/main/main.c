// MiniLingo Phase 1: Projekt-Skelett + LVGL + statisches UI-Mockup.
// app_main initialisiert LVGL, Display, Touch und UI und startet den
// LVGL-Task (Core 0, Prio 4 — Spec Section 8).
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

#include "drivers/display.h"
#include "drivers/touch.h"
#include "ui/ui.h"

static const char *TAG = "main";

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

// Dummy-Inhalte zum Testen von Layout, Wrapping und Scrolling
// (Acceptance Phase 1). Bewusst ohne Umlaute — der LVGL-Default-Font
// (Montserrat 14) enthaelt nur ASCII.
static void ui_populate_demo(void)
{
    ui_status_bar_set_state_text("Listening for Alexa");
    ui_status_bar_set_volume(60);
    ui_status_bar_set_wifi_connected(false);

    ui_chat_add_user_bubble("Wie ist das Wetter heute in Wuerzburg?");
    ui_chat_add_assistant_bubble(
        "In Wuerzburg sind es heute 18 Grad und teilweise bewoelkt. "
        "Am Nachmittag bleibt es trocken.");
    ui_chat_add_user_bubble("Set timer five minutes");
    ui_chat_add_local_bubble("Timer gesetzt: 5 Minuten (lokal, ohne Cloud)");
    ui_chat_add_user_bubble("Erklaer mir Rekursion in einem Satz");
    ui_chat_add_assistant_bubble(
        "Rekursion ist eine Technik, bei der eine Funktion sich selbst "
        "aufruft, bis eine Abbruchbedingung erreicht ist - zum Beispiel "
        "beim Berechnen der Fakultaet einer Zahl.");
}

void app_main(void)
{
    ESP_LOGI(TAG, "MiniLingo Phase 1 starting");

    lv_init();
    lv_tick_set_cb(lvgl_tick_get_cb);

    ESP_ERROR_CHECK(display_init());
    ESP_ERROR_CHECK(touch_init());

    ui_init();
    ui_mic_button_set_click_cb(on_mic_button_clicked);
    ui_populate_demo();

    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 8192, NULL, 4, NULL, 0);

    ESP_LOGI(TAG, "UI up and running");
}
