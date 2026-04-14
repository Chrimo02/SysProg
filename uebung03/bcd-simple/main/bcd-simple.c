/*
 * Minimaler BCD-Zähler (0–9) mit Hardware-Timer-ISR
 * ESP32-S3-WROOM-1, ESP-IDF v5.x
 *
 * GPIO4=Bit0, GPIO5=Bit1, GPIO6=Bit2, GPIO7=Bit3
 * Jeweils über 220Ω an LED → GND
 */

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "driver/gptimer.h"

static const gpio_num_t leds[] = {4, 5, 6, 7};
static QueueHandle_t q;

/* ISR – nur Signal senden, sofort raus */
static bool IRAM_ATTR on_alarm(gptimer_handle_t t,
    const gptimer_alarm_event_data_t *e, void *ctx)
{
    BaseType_t w = pdFALSE;
    xQueueSendFromISR(q, &(uint8_t){0}, &w);
    return w == pdTRUE;
}

void app_main(void)
{
    /* GPIOs als Ausgang */
    for (int i = 0; i < 4; i++) {
        gpio_reset_pin(leds[i]);
        gpio_set_direction(leds[i], GPIO_MODE_OUTPUT);
    }

    q = xQueueCreate(4, sizeof(uint8_t));

    /* Timer: 1 MHz Auflösung (= Prescaler 80), Alarm bei 1.000.000 = 1s */
    gptimer_handle_t tmr;
    gptimer_new_timer(&(gptimer_config_t){
        .clk_src       = GPTIMER_CLK_SRC_DEFAULT,
        .direction     = GPTIMER_COUNT_UP,
        .resolution_hz = 1000000,
    }, &tmr);

    gptimer_register_event_callbacks(tmr,
        &(gptimer_event_callbacks_t){.on_alarm = on_alarm}, NULL);

    gptimer_set_alarm_action(tmr, &(gptimer_alarm_config_t){
        .alarm_count = 1000000,
        .reload_count = 0,
        .flags.auto_reload_on_alarm = true,
    });

    gptimer_enable(tmr);
    gptimer_start(tmr);

    /* Hauptschleife */
    uint8_t cnt = 0, dummy;
    while (1) {
        xQueueReceive(q, &dummy, portMAX_DELAY);

        for (int i = 0; i < 4; i++)
            gpio_set_level(leds[i], (cnt >> i) & 1);

        cnt = (cnt >= 9) ? 0 : cnt + 1;
    }
}