#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"  // Queue-API für Task-Kommunikation
#include "driver/uart.h"
#include "driver/gpio.h"

#define UART_NUM        UART_NUM_0  // UART0 ist intern mit dem USB-Bridge-Chip verbunden
#define UART_TX_PIN     4          // TXD0 laut Datasheet → geht zum RX des anderen ESP
#define UART_RX_PIN     5          // RXD0 laut Datasheet → kommt vom TX des anderen ESP
#define UART_BAUD_RATE  115200      // Beide ESPs müssen dieselbe Baudrate haben!
#define BUF_SIZE        256         // Wie viele Bytes der Empfangspuffer fassen kann

#define LED_GPIO        2           // LED an GPIO2 angeschlossen
#define BUTTON_GPIO     17           // Taster an GPIO17, andere Seite an GND

// Eine Queue ist wie ein Briefkasten zwischen ISR und Task:
// Die ISR wirft einen Brief rein, der Task holt ihn ab.
// Ohne Queue könnte die ISR nicht sicher mit dem Task kommunizieren.
static QueueHandle_t button_queue;

// ─────────────────────────────────────────────────────────────
// ISR = Interrupt Service Routine
// Wird automatisch vom Prozessor aufgerufen, sobald sich der
// Pegel an BUTTON_GPIO ändert (Taster drücken oder loslassen).
// Läuft NICHT als normaler Task – unterbricht alles andere sofort!
//
// IRAM_ATTR: Diese Funktion wird in den RAM geladen statt im Flash
// zu bleiben. Das ist nötig weil Flash-Zugriffe zu langsam für
// eine ISR sein können – RAM ist schneller.
// ─────────────────────────────────────────────────────────────
static void IRAM_ATTR button_isr_handler(void *arg)
{
    // Aktuellen Pegelstand lesen: 0 = Taster gedrückt, 1 = losgelassen
    // (wegen Pullup ist Ruhezustand HIGH)
    int level = gpio_get_level(BUTTON_GPIO);

    // Pegelwert in die Queue schreiben damit der Task ihn verarbeiten kann.
    // xQueueSendFromISR statt xQueueSend, weil wir in einer ISR sind!
    // Der NULL-Parameter wäre für einen "higher priority task woken" Flag –
    // hier nicht nötig.
    xQueueSendFromISR(button_queue, &level, NULL);

    // ISR endet hier – der normale Programmablauf wird fortgesetzt
}

// ─────────────────────────────────────────────────────────────
// Sende-Task: wartet auf Nachrichten vom Interrupt und sendet
// dann das passende Zeichen per UART an den anderen ESP.
// Läuft als eigener FreeRTOS-Task parallel zum Empfangs-Task.
// ─────────────────────────────────────────────────────────────
static void button_tx_task(void *arg)
{
    int level;
    int last_level = 1; // Letzter bekannter Zustand, Start = HIGH (Pullup)

    while (1) {
        // xQueueReceive blockiert den Task solange bis etwas in der Queue liegt.
        // portMAX_DELAY = warte unbegrenzt lang → Task schläft und verbraucht
        // keine CPU-Zeit während er wartet. Viel effizienter als Polling!
        if (xQueueReceive(button_queue, &level, portMAX_DELAY)) {

            // Nach dem Interrupt 20ms warten bevor wir reagieren.
            // Taster prellen: beim Drücken "springt" das Signal kurz mehrmals
            // zwischen HIGH und LOW hin und her, bevor es stabil wird.
            // Diese 20ms überbrücken das Prellen.
            vTaskDelay(pdMS_TO_TICKS(20));

            // Nach der Wartezeit nochmal lesen – ist der Zustand noch derselbe?
            // Wenn ja: echter Tastendruck. Wenn nein: war nur ein Preller.
            int confirmed = gpio_get_level(BUTTON_GPIO);

            if (confirmed != last_level) { // Zustand hat sich wirklich geändert
                if (confirmed == 0) {
                    // Taster gedrückt (GPIO auf GND gezogen durch Taster)
                    // 'N' senden → anderer ESP soll LED einschalten
                    uart_write_bytes(UART_NUM, "N", 1);
                } else {
                    // Taster losgelassen (Pullup zieht GPIO wieder auf HIGH)
                    // 'F' senden → anderer ESP soll LED ausschalten
                    uart_write_bytes(UART_NUM, "F", 1);
                }
                last_level = confirmed; // neuen Zustand als letzten merken
            }
            // Falls confirmed == last_level war es ein Preller → ignorieren
        }
    }
}

// ─────────────────────────────────────────────────────────────
// Empfangs-Task: wartet dauerhaft auf Bytes vom anderen ESP
// und steuert je nach empfangenem Zeichen die lokale LED.
// ─────────────────────────────────────────────────────────────
static void uart_rx_task(void *arg)
{
    uint8_t data[BUF_SIZE]; // Puffer für empfangene Bytes

    while (1) {
        // Versuche Bytes zu lesen, warte maximal 100ms.
        // Gibt die Anzahl tatsächlich gelesener Bytes zurück (0 wenn Timeout).
        int len = uart_read_bytes(UART_NUM, data, BUF_SIZE - 1, pdMS_TO_TICKS(100));

        if (len > 0) {
            // Schleife über alle empfangenen Bytes (könnten mehrere auf einmal kommen)
            for (int i = 0; i < len; i++) {
                if (data[i] == 'N') {
                    gpio_set_level(LED_GPIO, 1); // 'N' empfangen → LED an
                } else if (data[i] == 'F') {
                    gpio_set_level(LED_GPIO, 0); // 'F' empfangen → LED aus
                }
                // Andere Zeichen werden einfach ignoriert
            }
        }
        // Wenn len == 0: Timeout, nichts empfangen → Schleife wiederholt sich
    }
}

void app_main(void)
{
    // ── LED konfigurieren ──────────────────────────────────────
    gpio_config_t led_conf = {
        .pin_bit_mask = (1ULL << LED_GPIO), // Bitmaske: welcher GPIO gemeint ist
                                            // 1ULL << 2 = Bit 2 setzen = GPIO2
        .mode         = GPIO_MODE_OUTPUT,   // GPIO als Ausgang schalten
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,  // Kein Interrupt für die LED
    };
    gpio_config(&led_conf);
    gpio_set_level(LED_GPIO, 0); // LED beim Start ausschalten

    // ── Taster konfigurieren ───────────────────────────────────
    gpio_config_t btn_conf = {
        .pin_bit_mask = (1ULL << BUTTON_GPIO),
        .mode         = GPIO_MODE_INPUT,        // GPIO als Eingang
        .pull_up_en   = GPIO_PULLUP_ENABLE,     // Interner Pullup-Widerstand aktiv:
                                                // ohne Pullup wäre der Pin "floating"
                                                // (undefinierter Zustand). Mit Pullup
                                                // ist Ruhezustand sicher HIGH.
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_ANYEDGE,      // Interrupt bei BEIDEN Flanken:
                                                // fallend (drücken) UND
                                                // steigend (loslassen)
    };
    gpio_config(&btn_conf);

    // ── Queue erstellen ────────────────────────────────────────
    // Parameter: Tiefe 10 (bis zu 10 unverarbeitete Events speichern),
    // Elementgröße: sizeof(int) weil wir den Pegelwert (0 oder 1) speichern
    button_queue = xQueueCreate(10, sizeof(int));

    // ── Interrupt einrichten ───────────────────────────────────
    // ISR-Service starten: erlaubt individuelle ISRs pro GPIO-Pin
    gpio_install_isr_service(0);
    // button_isr_handler für BUTTON_GPIO registrieren
    // Ab jetzt wird button_isr_handler automatisch aufgerufen bei Pegelwechsel
    gpio_isr_handler_add(BUTTON_GPIO, button_isr_handler, NULL);

    // ── UART konfigurieren ─────────────────────────────────────
    uart_config_t uart_config = {
        .baud_rate  = UART_BAUD_RATE,           // 115200 Baud
        .data_bits  = UART_DATA_8_BITS,         // 8 Datenbits pro Zeichen
        .parity     = UART_PARITY_DISABLE,      // kein Paritätsbit
        .stop_bits  = UART_STOP_BITS_1,         // 1 Stoppbit
                                                // → zusammen: "8N1" Format (Standard)
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE, // Kein Hardware-Handshake (RTS/CTS)
        .source_clk = UART_SCLK_DEFAULT,        // Standard-Taktquelle
    };
    // Treiber installieren: RX-Puffer = BUF_SIZE*2, kein TX-Puffer (0 = direkt senden)
    uart_driver_install(UART_NUM, BUF_SIZE * 2, 0, 0, NULL, 0);
    uart_param_config(UART_NUM, &uart_config);  // Konfiguration anwenden
    uart_set_pin(UART_NUM, UART_TX_PIN, UART_RX_PIN,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE); // Pins zuweisen
                                                          // UART_PIN_NO_CHANGE = RTS/CTS nicht belegen

    // ── Tasks starten ──────────────────────────────────────────
    // Beide Tasks laufen parallel – FreeRTOS wechselt zwischen ihnen.
    // Parameter: Funktion, Name, Stack-Größe, Argument, Priorität, Handle
    xTaskCreate(uart_rx_task,   "uart_rx", 4096, NULL, 10, NULL); // Empfangen
    xTaskCreate(button_tx_task, "btn_tx",  4096, NULL, 10, NULL); // Senden
}