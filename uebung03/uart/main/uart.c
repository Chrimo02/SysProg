#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"

// UART0: TXD0 = GPIO43, RXD0 = GPIO44 (laut Datasheet, gehen über USB-Bridge)
#define UART_NUM        UART_NUM_0
#define UART_TX_PIN     43
#define UART_RX_PIN     44
#define UART_BAUD_RATE  115200
#define BUF_SIZE        256

// LED an einem freien GPIO (z.B. GPIO2)
// Passe diesen Pin an deinen tatsächlichen GPIO an!
#define LED_GPIO        2

static void uart_led_task(void *arg)
{
    uint8_t data[BUF_SIZE];

    while (1) {
        // Auf eingehende Daten warten (100ms Timeout)
        int len = uart_read_bytes(UART_NUM, data, BUF_SIZE - 1, pdMS_TO_TICKS(100));

        if (len > 0) {
            data[len] = '\0'; // Null-terminieren

            // Zeilenumbrüche/Leerzeichen am Ende entfernen
            while (len > 0 && (data[len - 1] == '\n' ||
                                data[len - 1] == '\r' ||
                                data[len - 1] == ' ')) {
                data[--len] = '\0';
            }


        if (strcmp((char *)data, "1") == 0) {
            gpio_set_level(LED_GPIO, 1);
            uart_write_bytes(UART_NUM, "LED an!\r\n", 9);
        } else if (strcmp((char *)data, "0") == 0) {
            gpio_set_level(LED_GPIO, 0);
            uart_write_bytes(UART_NUM, "LED aus!\r\n", 10);
            }     
        }
    }
}

void app_main(void)
{
    // --- GPIO für LED konfigurieren ---
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(LED_GPIO, 0); // LED aus beim Start

    // --- UART0 konfigurieren ---
    uart_config_t uart_config = {
        .baud_rate  = UART_BAUD_RATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    uart_driver_install(UART_NUM, BUF_SIZE * 2, 0, 0, NULL, 0);
    uart_param_config(UART_NUM, &uart_config);
    uart_set_pin(UART_NUM, UART_TX_PIN, UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    uart_write_bytes(UART_NUM, "Bereit. Sende 'ON' oder 'OFF'\r\n", 31);

    // Task starten
    xTaskCreate(uart_led_task, "uart_led_task", 4096, NULL, 10, NULL);
}