/*
 * Minimaler UART-Test zwischen zwei ESP32-S3
 * ============================================
 *
 * ROLE 0 = SENDER   → sendet jede Sekunde "HALLO\n"
 * ROLE 1 = EMPFÄNGER → gibt empfangene Bytes auf dem Monitor aus
 *
 * Verdrahtung (nur 3 Drähte!):
 *   Sender  GPIO17 (TX) ────── GPIO18 (RX)  Empfänger
 *   Sender  GND         ────── GND          Empfänger
 *
 * Erst Sender flashen (ROLE 0), dann Empfänger (ROLE 1).
 * Wenn "HALLO" im Monitor des Empfängers erscheint,
 * funktioniert die UART-Verbindung.
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"

#define ROLE 0   /* ← HIER ÄNDERN: 0 = Sender, 1 = Empfänger */

#define UART_NUM    UART_NUM_1
#define TX_PIN      17
#define RX_PIN      18
#define BAUD        115200

void app_main(void)
{
    uart_config_t cfg = {
        .baud_rate  = BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(UART_NUM, 256, 0, 0, NULL, 0);
    uart_param_config(UART_NUM, &cfg);
    uart_set_pin(UART_NUM, TX_PIN, RX_PIN,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

#if ROLE == 0
    /* ---- SENDER ---- */
    ESP_LOGI("TX", "Ich bin der SENDER. Sende jede Sekunde...");
    int count = 0;
    while (1) {
        char msg[32];
        int len = snprintf(msg, sizeof(msg), "HALLO %d\n", count++);
        uart_write_bytes(UART_NUM, msg, len);
        ESP_LOGI("TX", "Gesendet: HALLO %d", count - 1);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

#else
    /* ---- EMPFÄNGER ---- */
    ESP_LOGI("RX", "Ich bin der EMPFAENGER. Warte auf Daten...");
    uint8_t buf[256];
    while (1) {
        int len = uart_read_bytes(UART_NUM, buf, sizeof(buf) - 1,
                                  pdMS_TO_TICKS(100));
        if (len > 0) {
            buf[len] = '\0';
            ESP_LOGI("RX", "Empfangen: %s", buf);
        }
    }
#endif
}