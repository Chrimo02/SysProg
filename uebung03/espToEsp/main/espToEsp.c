#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"

#define UART_NUM        UART_NUM_0  // UART0 ist mit dem USB-Bridge verbunden
#define UART_TX_PIN     4          // TXD0 laut Datasheet
#define UART_RX_PIN     5          // RXD0 laut Datasheet
#define UART_BAUD_RATE  115200      // Standard-Baudrate
#define BUF_SIZE        256         // Empfangspuffer in Bytes

#define LED_GPIO        2           // LED an GPIO2
#define BUTTON_GPIO     17           // Taster an GPIO17 (gegen GND)

// Empfangs-Task: läuft dauerhaft und wartet auf Zeichen vom anderen Board
static void uart_rx_task(void *arg)
{
    uint8_t data[BUF_SIZE];

    while (1) {
        // Blockiert bis zu 100ms auf eingehende Bytes
        int len = uart_read_bytes(UART_NUM, data, BUF_SIZE - 1, pdMS_TO_TICKS(100));

        if (len > 0) {
            for (int i = 0; i < len; i++) {
                if (data[i] == 'N') {
                    gpio_set_level(LED_GPIO, 1); // 'N' empfangen → LED an
                } else if (data[i] == 'F') {
                    gpio_set_level(LED_GPIO, 0); // 'F' empfangen → LED aus
                }
            }
        }
    }
}

// Sende-Task: überwacht den Taster und sendet bei Zustandsänderung
static void button_tx_task(void *arg)
{
    int last_state = 1; // Startzustand HIGH, da interner Pullup aktiv

    while (1) {
        int current_state = gpio_get_level(BUTTON_GPIO); // Taster einlesen

        if (current_state != last_state) {          // Zustandsänderung erkannt
            vTaskDelay(pdMS_TO_TICKS(200));           // 20ms warten zum Entprellen
            current_state = gpio_get_level(BUTTON_GPIO); // nochmal lesen zur Bestätigung

            if (current_state != last_state) {       // immer noch geändert → kein Preller
                if (current_state == 0) {
                    uart_write_bytes(UART_NUM, "N", 1); // Taster gedrückt → 'N' senden
                } else {
                    uart_write_bytes(UART_NUM, "F", 1); // Taster losgelassen → 'F' senden
                }
                last_state = current_state; // neuen Zustand merken
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10)); // 10ms Pause, verhindert CPU-Volllast
    }
}

void app_main(void)
{
    // --- LED konfigurieren ---
    gpio_config_t led_conf = {
        .pin_bit_mask = (1ULL << LED_GPIO), // Bitmaske für GPIO2
        .mode         = GPIO_MODE_OUTPUT,   // als Ausgang
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,  // kein Interrupt nötig
    };
    gpio_config(&led_conf);
    gpio_set_level(LED_GPIO, 0); // LED beim Start ausschalten

    // --- Taster konfigurieren ---
    gpio_config_t btn_conf = {
        .pin_bit_mask = (1ULL << BUTTON_GPIO), // Bitmaske für GPIO4
        .mode         = GPIO_MODE_INPUT,        // als Eingang
        .pull_up_en   = GPIO_PULLUP_ENABLE,     // interner Pullup → kein externer Widerstand nötig
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn_conf);

    // --- UART konfigurieren ---
    uart_config_t uart_config = {
        .baud_rate  = UART_BAUD_RATE,          // 115200 Baud
        .data_bits  = UART_DATA_8_BITS,        // 8 Datenbits (Standard)
        .parity     = UART_PARITY_DISABLE,     // kein Paritätsbit
        .stop_bits  = UART_STOP_BITS_1,        // 1 Stoppbit → 8N1 Format
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,// kein Hardware-Handshake
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(UART_NUM, BUF_SIZE * 2, 0, 0, NULL, 0); // Treiber + Puffer installieren
    uart_param_config(UART_NUM, &uart_config);                   // Parameter anwenden
    uart_set_pin(UART_NUM, UART_TX_PIN, UART_RX_PIN,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);        // Pins zuweisen

    // Tasks starten (laufen parallel auf FreeRTOS)
    xTaskCreate(uart_rx_task,   "uart_rx", 4096, NULL, 10, NULL); // Empfang
    xTaskCreate(button_tx_task, "btn_tx",  4096, NULL, 10, NULL); // Senden
}