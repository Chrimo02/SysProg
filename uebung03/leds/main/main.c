/**
 * ============================================================
 * ESP32-S3 Timer/Counter – Sekundentakt mit ISR (ESP-IDF / C)
 * ============================================================
 *
 * Hardware: ESP32-S3-WROOM-1 (DevKitC-1)
 * SDK:      ESP-IDF v5.x (auch kompatibel mit v4.4+)
 *
 * Aufgabe:
 *   a) LEDs blinken im Sekundentakt (abwechselnd / binär / BCD)
 *   b) KEIN vTaskDelay() – stattdessen Hardware-Timer mit ISR
 *
 * Anschlüsse:
 *   GPIO4  → 220Ω → LED0 (Bit 0, LSB) → GND
 *   GPIO5  → 220Ω → LED1 (Bit 1)       → GND
 *   GPIO6  → 220Ω → LED2 (Bit 2)       → GND
 *   GPIO7  → 220Ω → LED3 (Bit 3, MSB)  → GND
 *
 * Vorwiderstand:
 *   R = (3,3V − 2,0V) / 5mA ≈ 220Ω
 *
 * Timer-Konfiguration:
 *   - Taktquelle:  APB-Takt = 80 MHz
 *   - Prescaler:   80 → Timer-Takt = 80 MHz / 80 = 1 MHz
 *   - 1 Tick = 1 µs
 *   - Alarm-Wert:  1.000.000 → 1 Sekunde
 *   - Auto-Reload: aktiv → periodischer Interrupt
 *
 * Modus (MODE):
 *   0 = Zwei LEDs abwechselnd blinken
 *   1 = 4-Bit Binärzähler (0–15)
 *   2 = BCD-Anzeige (0–9)
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "esp_log.h"

/* ============================================================
 * KONFIGURATION
 * ============================================================ */
#define MODE 2  /* 0 = abwechselnd, 1 = binär, 2 = BCD */

/* ============================================================
 * Pin-Definitionen
 * ============================================================ */
#define LED_BIT0  GPIO_NUM_4   /* Bit 0 (LSB) */
#define LED_BIT1  GPIO_NUM_5   /* Bit 1        */
#define LED_BIT2  GPIO_NUM_6   /* Bit 2        */
#define LED_BIT3  GPIO_NUM_7   /* Bit 3 (MSB)  */

static const gpio_num_t led_pins[] = {
    LED_BIT0, LED_BIT1, LED_BIT2, LED_BIT3
};
#define NUM_LEDS  (sizeof(led_pins) / sizeof(led_pins[0]))

static const char *TAG = "TIMER_LED";

/* ============================================================
 * Queue für ISR → Hauptprogramm Kommunikation
 * ============================================================
 * Die ISR soll möglichst kurz sein. Deshalb sendet sie nur
 * ein Signal über eine FreeRTOS-Queue an die Hauptschleife.
 * Dort findet die eigentliche Verarbeitung statt.
 */
static QueueHandle_t timer_queue = NULL;

/* ============================================================
 * ISR – Interrupt Service Routine (Timer-Alarm Callback)
 * ============================================================
 *
 * Diese Funktion wird aufgerufen, wenn der Hardware-Timer
 * den Alarm-Wert (Compare-Register) erreicht.
 *
 * IRAM_ATTR: Funktion liegt im internen RAM → schneller
 * Zugriff, kein Flash-Cache-Miss während des Interrupts.
 *
 * Parameter:
 *   timer       – Handle des auslösenden Timers
 *   edata       – Event-Daten (Alarm-Wert, Zählerstand)
 *   user_ctx    – Benutzerkontext (hier nicht verwendet)
 *
 * Rückgabe:
 *   true  → ein Higher-Priority-Task wurde aufgeweckt
 *           (Context Switch nach ISR anfordern)
 *   false → kein Context Switch nötig
 */
static bool IRAM_ATTR timer_alarm_callback(
    gptimer_handle_t timer,
    const gptimer_alarm_event_data_t *edata,
    void *user_ctx)
{
    BaseType_t high_task_woken = pdFALSE;

    /* Signal an die Hauptschleife senden */
    uint64_t alarm_value = edata->alarm_value;
    xQueueSendFromISR(timer_queue, &alarm_value, &high_task_woken);

    /* true = FreeRTOS soll nach der ISR einen Context Switch
     * durchführen, falls ein höherpriorer Task aufgeweckt wurde */
    return (high_task_woken == pdTRUE);
}

/* ============================================================
 * GPIO-Initialisierung
 * ============================================================ */
static void gpio_init_leds(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 0,
        .mode         = GPIO_MODE_OUTPUT,       /* Ausgang          */
        .pull_up_en   = GPIO_PULLUP_DISABLE,    /* Kein Pull-Up     */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,   /* Kein Pull-Down   */
        .intr_type    = GPIO_INTR_DISABLE,       /* Kein GPIO-Intr.  */
    };

    /* Bit-Maske für alle LED-Pins setzen */
    for (int i = 0; i < NUM_LEDS; i++) {
        io_conf.pin_bit_mask |= (1ULL << led_pins[i]);
    }

    gpio_config(&io_conf);

    /* Alle LEDs initial ausschalten */
    for (int i = 0; i < NUM_LEDS; i++) {
        gpio_set_level(led_pins[i], 0);
    }

    ESP_LOGI(TAG, "GPIOs %d, %d, %d, %d als Ausgang konfiguriert",
             LED_BIT0, LED_BIT1, LED_BIT2, LED_BIT3);
}

/* ============================================================
 * Timer-Initialisierung
 * ============================================================
 *
 * Schritte (entspricht den Aufgabenpunkten c.ii–c.v):
 *
 * c.ii)  Timer erstellen und konfigurieren
 * c.iii) Prescaler auf 80 einstellen
 *        → Timer-Takt = APB (80 MHz) / 80 = 1 MHz
 *        Alarm-Wert (Compare-Register) = 1.000.000
 *        → 1.000.000 Ticks × 1 µs = 1 Sekunde
 * c.iv)  Alarm-Interrupt aktivieren
 * c.v)   ISR (Callback) registrieren und Timer starten
 */
static gptimer_handle_t gptimer = NULL;

static void timer_init_1sec(void)
{
    /* --- c.ii) Timer erstellen --- */
    gptimer_config_t timer_config = {
        .clk_src       = GPTIMER_CLK_SRC_DEFAULT,  /* APB-Takt (80 MHz)  */
        .direction     = GPTIMER_COUNT_UP,          /* Aufwärts zählen    */
        .resolution_hz = 1000000,                   /* c.iii) 1 MHz       */
        /* resolution_hz = clk_src / prescaler
         * 1 MHz = 80 MHz / 80 → Prescaler wird intern auf 80 gesetzt
         * → 1 Tick = 1 µs                                               */
    };

    ESP_ERROR_CHECK(gptimer_new_timer(&timer_config, &gptimer));
    ESP_LOGI(TAG, "Timer erstellt: Resolution = 1 MHz (Prescaler=80)");

    /* --- c.v) ISR-Callback registrieren --- */
    gptimer_event_callbacks_t cbs = {
        .on_alarm = timer_alarm_callback,  /* Unsere ISR-Funktion */
    };
    ESP_ERROR_CHECK(gptimer_register_event_callbacks(gptimer, &cbs, NULL));

    /* --- c.iv) Alarm (Compare-Register) aktivieren --- */
    gptimer_alarm_config_t alarm_config = {
        .alarm_count  = 1000000,    /* c.iii) 1.000.000 Ticks = 1 Sekunde */
        .reload_count = 0,          /* Nach Alarm: Zähler auf 0 zurücksetzen */
        .flags.auto_reload_on_alarm = true,  /* Periodischer Interrupt      */
    };
    ESP_ERROR_CHECK(gptimer_set_alarm_action(gptimer, &alarm_config));

    /* --- Timer aktivieren und starten --- */
    ESP_ERROR_CHECK(gptimer_enable(gptimer));
    ESP_ERROR_CHECK(gptimer_start(gptimer));

    ESP_LOGI(TAG, "Timer gestartet: Alarm alle 1 Sekunde");
}

/* ============================================================
 * Hilfsfunktion: 4-Bit-Wert auf LEDs anzeigen
 * ============================================================ */
static void display_binary(uint8_t value)
{
    for (int i = 0; i < NUM_LEDS; i++) {
        gpio_set_level(led_pins[i], (value >> i) & 0x01);
    }
}

/* ============================================================
 * Hauptprogramm (app_main)
 * ============================================================ */
void app_main(void)
{
    ESP_LOGI(TAG, "=== ESP32-S3 Timer LED-Steuerung ===");

    #if MODE == 0
        ESP_LOGI(TAG, "Modus: Abwechselndes Blinken");
    #elif MODE == 1
        ESP_LOGI(TAG, "Modus: 4-Bit Binaerzaehler (0-15)");
    #elif MODE == 2
        ESP_LOGI(TAG, "Modus: BCD-Anzeige (0-9)");
    #endif

    /* Queue erstellen (Tiefe 10, je 8 Byte pro Element) */
    timer_queue = xQueueCreate(10, sizeof(uint64_t));
    if (timer_queue == NULL) {
        ESP_LOGE(TAG, "Queue konnte nicht erstellt werden!");
        return;
    }

    /* Hardware initialisieren */
    gpio_init_leds();
    timer_init_1sec();

    /* Zählerstand */
    uint8_t counter = 0;
    uint64_t alarm_val;

    /* -------------------------------------------------------
     * Hauptschleife – wartet auf Timer-Signal aus der ISR
     * Kein Polling, kein delay()!
     * xQueueReceive() blockiert den Task, bis ein Element
     * in der Queue eintrifft (= Timer hat ausgelöst).
     * ------------------------------------------------------- */
    while (1) {
        /* Blockierend auf Timer-Signal warten (max. 2s Timeout) */
        if (xQueueReceive(timer_queue, &alarm_val, pdMS_TO_TICKS(2000))) {

            /* -----------------------------------------------
             * MODE 0: Zwei LEDs abwechselnd
             * ----------------------------------------------- */
            #if MODE == 0
            {
                static bool toggle = false;
                toggle = !toggle;
                gpio_set_level(LED_BIT0, toggle ? 1 : 0);
                gpio_set_level(LED_BIT1, toggle ? 0 : 1);
                ESP_LOGI(TAG, "LED0=%d  LED1=%d", toggle, !toggle);
            }

            /* -----------------------------------------------
             * MODE 1: 4-Bit Binärzähler (0 bis 15)
             * ----------------------------------------------- */
            #elif MODE == 1
            {
                display_binary(counter);
                ESP_LOGI(TAG, "Sekunde %2d -> binaer: %d%d%d%d",
                         counter,
                         (counter >> 3) & 1,
                         (counter >> 2) & 1,
                         (counter >> 1) & 1,
                         (counter >> 0) & 1);
                counter++;
                if (counter > 15) {
                    counter = 0;  /* Überlauf nach 1111 → 0000 */
                }
            }

            /* -----------------------------------------------
             * MODE 2: BCD-Anzeige (0 bis 9)
             * ----------------------------------------------- */
            #elif MODE == 2
            {
                display_binary(counter);
                ESP_LOGI(TAG, "Sekunde %d -> BCD: %d%d%d%d",
                         counter,
                         (counter >> 3) & 1,
                         (counter >> 2) & 1,
                         (counter >> 1) & 1,
                         (counter >> 0) & 1);
                counter++;
                if (counter > 9) {
                    counter = 0;  /* BCD: nur 0–9, dann Neustart */
                }
            }
            #endif

        } else {
            ESP_LOGW(TAG, "Timeout – kein Timer-Signal empfangen!");
        }
    }
}

/*
 * ============================================================
 * ZUSAMMENFASSUNG DER FRAGEN
 * ============================================================
 *
 * c.i) Taktfrequenz:
 *      - CPU:  Xtensa LX7 Dual-Core, bis zu 240 MHz
 *      - APB-Bus (Timer-Taktquelle): 80 MHz
 *
 * c.ii) Timer-Konfiguration:
 *       - ESP-IDF General Purpose Timer (gptimer)
 *       - 54-Bit Aufwärtszähler
 *       - Alarm bei Erreichen des Compare-Werts
 *
 * c.iii) Prescaler = 80 (implizit durch resolution_hz = 1 MHz)
 *        → Timer-Takt = 80 MHz / 80 = 1 MHz (1 Tick = 1 µs)
 *        → Alarm-Wert (Compare-Register) = 1.000.000
 *        → 1.000.000 × 1 µs = 1,0 Sekunde
 *
 * c.iv) Interrupt aktivieren:
 *       - gptimer_register_event_callbacks() registriert ISR
 *       - gptimer_set_alarm_action() konfiguriert Compare-
 *         Register und aktiviert Alarm-Interrupt
 *       - gptimer_enable() aktiviert den Interrupt global
 *
 * c.v) ISR-Aufruf:
 *      - timer_alarm_callback() wird bei jedem Alarm aufgerufen
 *      - IRAM_ATTR → liegt im internen RAM
 *      - Sendet Signal über FreeRTOS-Queue an app_main()
 *      - Eigentliche Verarbeitung in der Hauptschleife
 *        (ISR bleibt minimal kurz!)
 *
 * ============================================================
 * BUILD & FLASH (ESP-IDF)
 * ============================================================
 *
 *   idf.py set-target esp32s3
 *   idf.py build
 *   idf.py -p /dev/ttyUSB0 flash monitor
 *
 * ============================================================
 */