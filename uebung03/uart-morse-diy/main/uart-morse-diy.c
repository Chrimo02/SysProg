#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "driver/ledc.h"

#define UART_NUM        UART_NUM_0
#define UART_TX_PIN     4
#define UART_RX_PIN     5
#define UART_BAUD_RATE  115200
#define BUF_SIZE        256

#define LED_GPIO        2
#define BUTTON_GPIO     17
#define BUZZER_GPIO     21

#define BUZZER_CHANNEL  LEDC_CHANNEL_0
#define BUZZER_TIMER    LEDC_TIMER_0
#define BUZZER_FREQ     1000
#define BUZZER_RES      LEDC_TIMER_10_BIT

static QueueHandle_t uart_event_queue;

static void uart_rx_task(void *arg)
{
    uart_event_t event;
    uint8_t data[BUF_SIZE];

    while (1) {
        if (xQueueReceive(uart_event_queue, &event, portMAX_DELAY)) {
            if (event.type == UART_DATA) {
                int len = uart_read_bytes(UART_NUM, data, event.size, pdMS_TO_TICKS(10));

                for (int i = 0; i < len; i++) {
                    if (data[i] == 'N') {
                        gpio_set_level(LED_GPIO, 1);
                        ledc_set_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL, 512);
                        ledc_update_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL);
                    } else if (data[i] == 'F') {
                        gpio_set_level(LED_GPIO, 0);
                        ledc_set_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL, 0);
                        ledc_update_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL);
                    }
                }
            }
        }
    }
}

static void button_tx_task(void *arg)
{
    int last_state = 1;

    while (1) {
        int current_state = gpio_get_level(BUTTON_GPIO);

        if (current_state != last_state) {
            vTaskDelay(pdMS_TO_TICKS(20));
            current_state = gpio_get_level(BUTTON_GPIO);

            if (current_state != last_state) {
                if (current_state == 0) {
                    uart_write_bytes(UART_NUM, "N", 1);
                } else {
                    uart_write_bytes(UART_NUM, "F", 1);
                }
                last_state = current_state;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void app_main(void)
{
    // --- LED ---
    gpio_config_t led_conf = {
        .pin_bit_mask = (1ULL << LED_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&led_conf);
    gpio_set_level(LED_GPIO, 0);

    // --- Taster ---
    gpio_config_t btn_conf = {
        .pin_bit_mask = (1ULL << BUTTON_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn_conf);

    // --- LEDC Timer ---
    ledc_timer_config_t ledc_timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .timer_num       = BUZZER_TIMER,
        .duty_resolution = BUZZER_RES,
        .freq_hz         = BUZZER_FREQ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&ledc_timer);

    // --- LEDC Kanal ---
    ledc_channel_config_t ledc_channel = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = BUZZER_CHANNEL,
        .timer_sel  = BUZZER_TIMER,
        .intr_type  = LEDC_INTR_DISABLE,
        .gpio_num   = BUZZER_GPIO,
        .duty       = 0,
        .hpoint     = 0,
    };
    ledc_channel_config(&ledc_channel);

    // --- UART ---
    uart_config_t uart_config = {
        .baud_rate  = UART_BAUD_RATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(UART_NUM, BUF_SIZE * 2, 0, 20, &uart_event_queue, 0);
    uart_param_config(UART_NUM, &uart_config);
    uart_set_pin(UART_NUM, UART_TX_PIN, UART_RX_PIN,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    xTaskCreate(uart_rx_task,   "uart_rx", 4096, NULL, 10, NULL);
    xTaskCreate(button_tx_task, "btn_tx",  4096, NULL, 10, NULL);
}