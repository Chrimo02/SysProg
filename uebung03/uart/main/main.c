/*
 * UART-Kommunikation zwischen zwei ESP32-S3-DevKitC-1
 * =====================================================
 * Beide Boards bekommen denselben Code!
 *
 * Verdrahtung (Nullmodem):
 *   Board A  GPIO17 (TX) ───→ GPIO18 (RX)  Board B
 *   Board A  GPIO18 (RX) ←─── GPIO17 (TX)  Board B
 *   Board A  GND         ←──→ GND          Board B
 *
 * Pro Board auf dem Breadboard:
 *   GPIO4  → 220Ω → LED → GND       (Ausgang, LED)
 *   GPIO5  → Taster → GND            (Eingang, interner Pull-Up)
 *
 * Funktion:
 *   Taster auf Board A drücken → LED auf Board B leuchtet
 *   Taster auf Board B drücken → LED auf Board A leuchtet
 *
 * UART-Konfiguration:
 *   UART1, 115200 Baud, 8N1, kein Flow-Control
 *   TX=GPIO17, RX=GPIO18
 *
 * Zwei Varianten:
 *   VERSION 0 = Polling (Taster in Schleife abfragen)
 *   VERSION 1 = Interrupt (GPIO-ISR bei Tastendruck)
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"

/* ============================================================
 * KONFIGURATION
 * ============================================================ */
#define VERSION 1       /* 0 = Polling, 1 = Interrupt */

#define UART_NUM    UART_NUM_1
#define UART_TX_PIN GPIO_NUM_17
#define UART_RX_PIN GPIO_NUM_18
#define BAUD_RATE   115200
#define BUF_SIZE    256

#define LED_PIN     GPIO_NUM_4
#define BTN_PIN     GPIO_NUM_5

/* Protokoll: ein Byte wird gesendet */
#define CMD_LED_ON  0xA1
#define CMD_LED_OFF 0xA2

static const char *TAG = "UART_LED";

/* ============================================================
 * UART initialisieren
 * ============================================================ */
static void uart_init_custom(void)
{
    uart_config_t cfg = {
        .baud_rate  = BAUD_RATE,
        .data_bits  = UART_DATA_8_BITS,    /* 8 Datenbits       */
        .parity     = UART_PARITY_DISABLE, /* Keine Parität     */
        .stop_bits  = UART_STOP_BITS_1,    /* 1 Stoppbit        */
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE, /* Kein Flow-Ctrl */
        .source_clk = UART_SCLK_DEFAULT,
    };

    /* UART-Treiber installieren (RX-Buffer, kein TX-Buffer) */
    uart_driver_install(UART_NUM, BUF_SIZE, 0, 0, NULL, 0);
    uart_param_config(UART_NUM, &cfg);

    /* Pins zuweisen: TX=17, RX=18, kein RTS/CTS */
    uart_set_pin(UART_NUM, UART_TX_PIN, UART_RX_PIN,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    ESP_LOGI(TAG, "UART1 initialisiert: %d Baud, TX=GPIO%d, RX=GPIO%d",
             BAUD_RATE, UART_TX_PIN, UART_RX_PIN);
}

/* ============================================================
 * GPIO initialisieren (LED + Taster)
 * ============================================================ */
static void gpio_init_led_btn(void)
{
    /* LED als Ausgang */
    gpio_reset_pin(LED_PIN);
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_PIN, 0);

    /* Taster als Eingang mit internem Pull-Up */
    gpio_reset_pin(BTN_PIN);
    gpio_set_direction(BTN_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BTN_PIN, GPIO_PULLUP_ONLY);

    ESP_LOGI(TAG, "LED=GPIO%d, Taster=GPIO%d (Pull-Up)", LED_PIN, BTN_PIN);
}

/* ============================================================
 * Byte über UART senden
 * ============================================================ */
static void uart_send_cmd(uint8_t cmd)
{
    uart_write_bytes(UART_NUM, (const char *)&cmd, 1);
}

/* ============================================================
 * Empfangenes Byte verarbeiten
 * ============================================================ */
static void process_rx(uint8_t cmd)
{
    if (cmd == CMD_LED_ON) {
        gpio_set_level(LED_PIN, 1);
        ESP_LOGI(TAG, "Empfangen: LED AN");
    } else if (cmd == CMD_LED_OFF) {
        gpio_set_level(LED_PIN, 0);
        ESP_LOGI(TAG, "Empfangen: LED AUS");
    }
}

/* ============================================================
 * UART-Empfangs-Task
 * Läuft dauerhaft und prüft ob Daten angekommen sind.
 * Wird in BEIDEN Versionen (Polling und Interrupt) genutzt.
 * ============================================================ */
static void uart_rx_task(void *arg)
{
    uint8_t buf[BUF_SIZE];

    while (1) {
        /* Blockierend auf UART-Daten warten (Timeout 20ms) */
        int len = uart_read_bytes(UART_NUM, buf, BUF_SIZE,
                                  pdMS_TO_TICKS(20));
        if (len > 0) {
            for (int i = 0; i < len; i++) {
                process_rx(buf[i]);
            }
        }
    }
}

/* ************************************************************
 * VERSION 0: POLLING
 * ************************************************************
 * Der Taster wird in einer Schleife ständig abgefragt.
 * Einfach, aber die CPU dreht dauerhaft in der Schleife.
 * ********************************************************** */
#if VERSION == 0

void app_main(void)
{
    ESP_LOGI(TAG, "=== UART LED (Polling-Version) ===");

    gpio_init_led_btn();
    uart_init_custom();

    /* Empfangs-Task auf Core 1 starten */
    xTaskCreatePinnedToCore(uart_rx_task, "uart_rx", 4096,
                            NULL, 5, NULL, 1);

    /* Polling-Schleife: Taster abfragen */
    bool last_state = true;  /* true = nicht gedrückt (Pull-Up) */

    while (1) {
        bool pressed = (gpio_get_level(BTN_PIN) == 0);

        /* Flanke erkennen: nur bei Zustandswechsel senden */
        if (pressed && !last_state == false) {
            /* Taste gerade gedrückt → LED beim Nachbarn AN */
            if (pressed != (!last_state)) {
                /* Vereinfacht: */
            }
        }

        /* Saubere Flankenerkennung: */
        if (pressed && last_state) {
            /* Fallende Flanke: Taste wurde gerade gedrückt */
            uart_send_cmd(CMD_LED_ON);
            ESP_LOGI(TAG, "Gesendet: LED AN");
        } else if (!pressed && !last_state) {
            /* Steigende Flanke: Taste wurde losgelassen */
            uart_send_cmd(CMD_LED_OFF);
            ESP_LOGI(TAG, "Gesendet: LED AUS");
        }

        last_state = !pressed;

        vTaskDelay(pdMS_TO_TICKS(50)); /* Entprellen: 50ms */
    }
}

/* ************************************************************
 * VERSION 1: INTERRUPT
 * ************************************************************
 * Der Taster löst einen GPIO-Interrupt aus (fallende +
 * steigende Flanke). Die ISR sendet über eine Queue an
 * einen Task, der dann das UART-Byte verschickt.
 *
 * Was fällt auf an app_main()?
 * → app_main() ist fast leer! Sie konfiguriert nur die
 *   Hardware und startet die Tasks. Danach endet sie.
 *   Die gesamte Logik läuft in den Tasks und der ISR.
 *   Es gibt keine while(1)-Schleife in app_main().
 *   FreeRTOS hält das System am Laufen.
 * ********************************************************** */
#elif VERSION == 1

static QueueHandle_t btn_queue = NULL;

/* ISR: wird bei jeder Flanke am Taster-Pin aufgerufen */
static void IRAM_ATTR btn_isr_handler(void *arg)
{
    uint32_t level = gpio_get_level(BTN_PIN);
    xQueueSendFromISR(btn_queue, &level, NULL);
}

/* Task: wartet auf Taster-Events aus der ISR */
static void btn_task(void *arg)
{
    uint32_t level;

    while (1) {
        /* Blockierend auf Signal aus der ISR warten */
        if (xQueueReceive(btn_queue, &level, portMAX_DELAY)) {
            if (level == 0) {
                /* Taster gedrückt (LOW durch Pull-Up→GND) */
                uart_send_cmd(CMD_LED_ON);
                ESP_LOGI(TAG, "Gesendet: LED AN");
            } else {
                /* Taster losgelassen (HIGH durch Pull-Up) */
                uart_send_cmd(CMD_LED_OFF);
                ESP_LOGI(TAG, "Gesendet: LED AUS");
            }

            /* Einfache Software-Entprellung: 50ms Pause */
            vTaskDelay(pdMS_TO_TICKS(50));
            xQueueReset(btn_queue);  /* Preller verwerfen */
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== UART LED (Interrupt-Version) ===");

    gpio_init_led_btn();
    uart_init_custom();

    /* Queue für Taster-ISR → Task Kommunikation */
    btn_queue = xQueueCreate(8, sizeof(uint32_t));

    /* GPIO-Interrupt für Taster konfigurieren:
     * ANY_EDGE = bei fallender UND steigender Flanke */
    gpio_set_intr_type(BTN_PIN, GPIO_INTR_ANYEDGE);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(BTN_PIN, btn_isr_handler, NULL);

    ESP_LOGI(TAG, "GPIO-Interrupt fuer Taster aktiviert");

    /* Tasks starten */
    xTaskCreatePinnedToCore(uart_rx_task, "uart_rx", 4096,
                            NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(btn_task, "btn_task", 4096,
                            NULL, 5, NULL, 0);

    /*
     * !! BEOBACHTUNG zu Aufgabe (d): !!
     *
     * app_main() endet hier – KEINE while(1)-Schleife!
     *
     * In der Polling-Version (VERSION 0) bleibt app_main()
     * für immer in einer Endlosschleife hängen und fragt
     * den Taster ständig ab.
     *
     * In der Interrupt-Version braucht app_main() das nicht.
     * Sie richtet alles ein (GPIO, UART, ISR, Tasks) und
     * kehrt dann zurück. Die CPU ist frei für andere Dinge.
     * FreeRTOS verwaltet die Tasks, und die ISR reagiert
     * nur wenn sich tatsächlich etwas am Pin ändert.
     *
     * → Interrupt-Version ist ressourcenschonender
     * → app_main() wird zur reinen Initialisierungsfunktion
     * → Keine verschwendete CPU-Zeit durch Polling
     */
    ESP_LOGI(TAG, "Initialisierung abgeschlossen. "
                   "app_main() endet jetzt.");
}

#endif

/*
 * ============================================================
 * BUILD & FLASH
 * ============================================================
 *
 * Projektstruktur:
 *   uart_led/
 *   ├── CMakeLists.txt       (project)
 *   └── main/
 *       ├── CMakeLists.txt   (component)
 *       └── main.c           (diese Datei)
 *
 * main/CMakeLists.txt:
 *   idf_component_register(SRCS "main.c" INCLUDE_DIRS ".")
 *
 * CMakeLists.txt (project):
 *   cmake_minimum_required(VERSION 3.16)
 *   include($ENV{IDF_PATH}/tools/cmake/project.cmake)
 *   project(uart_led)
 *
 * Befehle:
 *   idf.py set-target esp32s3
 *   idf.py build
 *   idf.py -p /dev/ttyUSB0 flash monitor
 *
 * ============================================================
 * VERDRAHTUNG AUF DEM BREADBOARD
 * ============================================================
 *
 *   Board A                          Board B
 *   -------                          -------
 *   GPIO17 (TX) ──────────────────── GPIO18 (RX)
 *   GPIO18 (RX) ──────────────────── GPIO17 (TX)
 *   GND ──────────────────────────── GND
 *
 *   GPIO4 ── 220Ω ── LED ── GND     GPIO4 ── 220Ω ── LED ── GND
 *   GPIO5 ── Taster ── GND          GPIO5 ── Taster ── GND
 *
 * WICHTIG:
 * - UART0 (GPIO43/44) NICHT verwenden → ist für USB-Konsole
 * - UART1 auf GPIO17/18 ist frei verfügbar
 * - Nullmodem: TX↔RX kreuzen, NIEMALS TX↔TX!
 * - GND muss verbunden sein, sonst kein gemeinsames Bezugspotential
 * - Beide Boards mit gleicher Baudrate (115200)
 * - Beide Boards bekommen denselben Code
 *
 * ============================================================
 */