#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "driver/ledc.h"   // LEDC = LED PWM Controller, nutzen wir für Tonerzeugung

#define UART_NUM        UART_NUM_0
#define UART_TX_PIN     4
#define UART_RX_PIN     5
#define UART_BAUD_RATE  115200
#define BUF_SIZE        256

#define LED_GPIO        2
#define BUTTON_GPIO     17
#define BUZZER_GPIO     21   // Passiver Buzzer/Lautsprecher an GPIO21

// LEDC Konfiguration für den Buzzer
#define BUZZER_CHANNEL  LEDC_CHANNEL_0  // Einer von 8 verfügbaren PWM-Kanälen
#define BUZZER_TIMER    LEDC_TIMER_0    // Einer von 4 verfügbaren Timern
#define BUZZER_FREQ     1000            // 1000 Hz = hörbarer Ton (A5 wäre 880Hz)
#define BUZZER_RES      LEDC_TIMER_10_BIT // 10-bit Auflösung → 0-1023 für Duty Cycle

static QueueHandle_t uart_event_queue;

// Buzzer für eine bestimmte Dauer einschalten, dann wieder aus
static void buzzer_beep(uint32_t duration_ms)
{
    // Duty Cycle auf 50% setzen → 512 bei 10-bit (1023/2 ≈ 512)
    // 50% Duty Cycle = lautester Ton bei PWM-Buzzern
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL, 512);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL); // Änderung anwenden

    vTaskDelay(pdMS_TO_TICKS(duration_ms)); // Ton so lange halten

    // Duty Cycle auf 0 → kein Signal → Ton aus
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL);
}

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
                        gpio_set_level(LED_GPIO, 1); // LED an
                        buzzer_beep(200);            // 200ms Ton beim Einschalten
                    } else if (data[i] == 'F') {
                        gpio_set_level(LED_GPIO, 0); // LED aus
                        // kein Ton beim Ausschalten – nur beim Drücken
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
            vTaskDelay(pdMS_TO_TICKS(200));
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

    // --- LEDC Timer konfigurieren ---
    // Der Timer bestimmt die Frequenz des PWM-Signals = die Tonhöhe
    ledc_timer_config_t ledc_timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE, // Low-Speed reicht für Audio
        .timer_num       = BUZZER_TIMER,
        .duty_resolution = BUZZER_RES,          // 10-bit Auflösung
        .freq_hz         = BUZZER_FREQ,         // 1000 Hz Ton
        .clk_cfg         = LEDC_AUTO_CLK,       // Taktquelle automatisch wählen
    };
    ledc_timer_config(&ledc_timer);

    // --- LEDC Kanal konfigurieren ---
    // Der Kanal verbindet Timer + GPIO und steuert den Duty Cycle
    ledc_channel_config_t ledc_channel = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = BUZZER_CHANNEL,
        .timer_sel  = BUZZER_TIMER,
        .intr_type  = LEDC_INTR_DISABLE,
        .gpio_num   = BUZZER_GPIO,  // PWM-Signal kommt aus diesem Pin
        .duty       = 0,            // Start mit Duty 0 → Ton aus
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