#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "driver/gpio.h"

#define UART_NUM        UART_NUM_0
#define UART_TX_PIN     4
#define UART_RX_PIN     5
#define UART_BAUD_RATE  115200
#define BUF_SIZE        256

#define LED_GPIO        2
#define BUTTON_GPIO     15

// Queue für UART-Events – der UART-Treiber schreibt hier rein
// wenn etwas passiert (z.B. Daten empfangen, Fehler, etc.)
static QueueHandle_t uart_event_queue;

// ─────────────────────────────────────────────────────────────
// UART-Event-Task: schläft solange bis der UART-Treiber meldet
// dass neue Daten angekommen sind. Erst dann wird gelesen.
// Das ist der "Interrupt-gesteuerte" Empfang – kein aktives
// Warten mehr wie beim Polling mit uart_read_bytes + Timeout.
// ─────────────────────────────────────────────────────────────
static void uart_rx_task(void *arg)
{
    uart_event_t event;  // Struct die beschreibt was passiert ist
    uint8_t data[BUF_SIZE];

    while (1) {
        // Blockiert bis ein UART-Event in der Queue landet.
        // Der UART-Treiber füllt diese Queue automatisch per Interrupt
        // sobald Bytes im Empfangspuffer ankommen.
        if (xQueueReceive(uart_event_queue, &event, portMAX_DELAY)) {

            // Es gibt verschiedene UART-Events, uns interessiert nur
            // UART_DATA = "es sind neue Daten angekommen"
            if (event.type == UART_DATA) {

                // event.size sagt uns genau wie viele Bytes angekommen sind
                int len = uart_read_bytes(UART_NUM, data, event.size, pdMS_TO_TICKS(10));

                for (int i = 0; i < len; i++) {
                    if (data[i] == 'N') {
                        gpio_set_level(LED_GPIO, 1); // 'N' empfangen → LED an
                    } else if (data[i] == 'F') {
                        gpio_set_level(LED_GPIO, 0); // 'F' empfangen → LED aus
                    }
                }
            }
            // Andere Events (Fehler, Buffer-Overflow etc.) ignorieren wir hier
        }
    }
}

// Taster-Task: unverändert per Polling
static void button_tx_task(void *arg)
{
    int last_state = 1;

    while (1) {
        int current_state = gpio_get_level(BUTTON_GPIO);

        if (current_state != last_state) {
            vTaskDelay(pdMS_TO_TICKS(20));           // Entprellung
            current_state = gpio_get_level(BUTTON_GPIO);

            if (current_state != last_state) {
                if (current_state == 0) {
                    uart_write_bytes(UART_NUM, "N", 1); // Taster gedrückt → LED an
                } else {
                    uart_write_bytes(UART_NUM, "F", 1); // Taster losgelassen → LED aus
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

    // --- UART ---
    uart_config_t uart_config = {
        .baud_rate  = UART_BAUD_RATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    // uart_event_queue als letzten Parameter übergeben →
    // der Treiber füllt sie automatisch per Interrupt wenn Events auftreten
    uart_driver_install(UART_NUM, BUF_SIZE * 2, 0, 20, &uart_event_queue, 0);
    //                                               ↑   ↑
    //                                    Queue-Tiefe   Queue-Handle
    uart_param_config(UART_NUM, &uart_config);
    uart_set_pin(UART_NUM, UART_TX_PIN, UART_RX_PIN,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    xTaskCreate(uart_rx_task,   "uart_rx", 4096, NULL, 10, NULL);
    xTaskCreate(button_tx_task, "btn_tx",  4096, NULL, 10, NULL);
}