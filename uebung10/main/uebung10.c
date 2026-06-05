/*
 * Übung 10 – Park Distance Control (PDC)
 * ESP32-S3-DevKitC-1, ESP-IDF v5.x
 *
 * HC-SR04: Trig → GPIO4, Echo → GPIO5
 * Lautsprecher: GPIO6 (direkt, kein Verstärker)
 */

#include <stdio.h>                  // Standard Ein-/Ausgabe (printf etc.)
#include <math.h>                   // Mathematische Funktionen (nicht direkt genutzt, aber für Erweiterungen)
#include "freertos/FreeRTOS.h"      // FreeRTOS Basisbibliothek (Scheduler, Typen)
#include "freertos/task.h"          // FreeRTOS Task-Funktionen (vTaskDelay)
#include "driver/gpio.h"            // GPIO-Treiber (Ein-/Ausgabe, Interrupts)
#include "driver/ledc.h"            // LEDC-Treiber (PWM für Ton-Ausgabe)
#include "driver/gptimer.h"         // General Purpose Timer (Zeitstempel für Echo-Messung)
#include "esp_log.h"                // Logging-Funktionen (ESP_LOGI etc.)
#include "esp_rom_sys.h"            // ROM-Systemfunktionen (esp_rom_delay_us)

static const char *TAG = "PDC";     // Log-Tag, erscheint als Prefix im Monitor

/* ── Konfiguration ─────────────────────────────────────────── */
#define TRIG_GPIO       GPIO_NUM_4  // GPIO für Trig-Pin des HC-SR04
#define ECHO_GPIO       GPIO_NUM_5  // GPIO für Echo-Pin des HC-SR04
#define SPEAKER_GPIO    GPIO_NUM_6  // GPIO für Lautsprecher

#define TEMP_C          20.0f       // Umgebungstemperatur in °C (beeinflusst Schallgeschwindigkeit)

#define BEEP_FREQ_HZ    1000        // Tonfrequenz des Beeps in Hz
#define BEEP_DUTY       512         // Lautstärke: 512 von 1023 (10-bit, ~50% Duty Cycle)
#define BEEP_DURATION_MS 80         // Dauer eines einzelnen Beeps in Millisekunden

#define DIST_CONTACT    8           // Unter 8 cm → Dauerton (Kontakt)
#define DIST_CLOSE      20          // Unter 20 cm → sehr schnelle Beeps (100ms Pause)
#define DIST_NEAR       50          // Unter 50 cm → schnelle Beeps (300ms Pause)
#define DIST_MID        100         // Unter 100 cm → mittlere Beeps (600ms Pause)
#define DIST_FAR        200         // Unter 200 cm → langsame Beeps (1000ms Pause)

/* ── Globale Variablen für Echo-Messung ────────────────────── */
static volatile uint64_t echo_start_us = 0;  // Zeitstempel der steigenden Echo-Flanke in µs
static volatile uint64_t echo_end_us   = 0;  // Zeitstempel der fallenden Echo-Flanke in µs
static volatile bool     echo_done     = false; // Flag: true wenn vollständige Messung vorliegt

static gptimer_handle_t gptimer = NULL;      // Handle für den Hardware-Timer

/* ── Schallgeschwindigkeit aus Temperatur ──────────────────── */
static inline float us_to_cm(uint64_t pulse_us) {
    float v = 331.5f + 0.606f * TEMP_C;       // Schallgeschwindigkeit in m/s aus Temperatur
    return (float)pulse_us * v / 20000.0f;    // Pulsdauer (µs) → Distanz (cm): d = t * v / 20000
}

/* ── GPIO-Interrupt Handler (Echo-Pin) ─────────────────────── */
static void IRAM_ATTR echo_isr_handler(void *arg)  // IRAM_ATTR: ISR liegt im schnellen RAM
{
    uint64_t count = 0;                            // Lokale Variable für aktuellen Timer-Zählerstand
    gptimer_get_raw_count(gptimer, &count);        // Aktuellen Zeitstempel aus Timer lesen

    if (gpio_get_level(ECHO_GPIO) == 1) {          // Steigende Flanke: Echo beginnt
        echo_start_us = count;                     // Startzeitpunkt speichern
        echo_done     = false;                     // Messung noch nicht abgeschlossen
    } else {                                       // Fallende Flanke: Echo endet
        echo_end_us = count;                       // Endzeitpunkt speichern
        echo_done   = true;                        // Messung vollständig
    }
}

/* ── LEDC (Ton) initialisieren ─────────────────────────────── */
static void speaker_init(void)
{
    ledc_timer_config_t timer_conf = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,    // Low-Speed-Modus (für ESP32-S3 geeignet)
        .duty_resolution = LEDC_TIMER_10_BIT,      // 10-bit Auflösung → Duty 0–1023
        .timer_num       = LEDC_TIMER_0,           // Hardware-Timer 0 verwenden
        .freq_hz         = BEEP_FREQ_HZ,           // PWM-Frequenz = Tonfrequenz (1000 Hz)
        .clk_cfg         = LEDC_AUTO_CLK,          // Taktquelle automatisch wählen
    };
    ledc_timer_config(&timer_conf);                // Timer-Konfiguration anwenden

    ledc_channel_config_t ch_conf = {
        .gpio_num   = SPEAKER_GPIO,                // PWM-Signal auf Lautsprecher-GPIO ausgeben
        .speed_mode = LEDC_LOW_SPEED_MODE,         // Gleicher Modus wie Timer
        .channel    = LEDC_CHANNEL_0,              // Kanal 0 verwenden
        .timer_sel  = LEDC_TIMER_0,               // Kanal mit Timer 0 verknüpfen
        .duty       = 0,                           // Startet mit Duty 0 (Ton aus)
        .hpoint     = 0,                           // High-Point auf 0 (Standard)
    };
    ledc_channel_config(&ch_conf);                 // Kanal-Konfiguration anwenden
}

/* ── Einzelnen Beep ausgeben ───────────────────────────────── */
static void beep(void)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, BEEP_DUTY);   // Duty auf 512 setzen → Ton an
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);           // Duty-Änderung übernehmen
    vTaskDelay(pdMS_TO_TICKS(BEEP_DURATION_MS));                     // 80ms warten (Tondauer)
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);           // Duty auf 0 setzen → Ton aus
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);           // Duty-Änderung übernehmen
}

/* ── Distanz messen ────────────────────────────────────────── */
static float measure_distance_cm(void)
{
    echo_done = false;                             // Vorherige Messung zurücksetzen

    // Trig-Puls senden
    gpio_set_level(TRIG_GPIO, 0);                 // Trig sicherstellen LOW
    esp_rom_delay_us(2);                          // 2µs warten (Stabilisierung)
    gpio_set_level(TRIG_GPIO, 1);                 // Trig HIGH: Messung starten
    esp_rom_delay_us(10);                         // 10µs HIGH halten (HC-SR04 Anforderung)
    gpio_set_level(TRIG_GPIO, 0);                 // Trig wieder LOW

    // Auf Echo-Interrupt warten: max. 38ms in 100µs-Schritten
    int timeout_us = 38000;                       // Maximale Wartezeit 38ms = kein Hindernis
    while (!echo_done && timeout_us > 0) {        // Warten bis Messung fertig oder Timeout
        esp_rom_delay_us(100);                    // 100µs warten (feinere Auflösung als vTaskDelay)
        timeout_us -= 100;                        // Verbleibende Wartezeit reduzieren
    }

    if (!echo_done) return -1.0f;                 // Timeout → kein Echo empfangen

    uint64_t pulse_us = echo_end_us - echo_start_us;           // Pulsdauer berechnen
    if (pulse_us < 100 || pulse_us > 25000) return -1.0f;      // Plausibilitätscheck (2–400 cm)

    return us_to_cm(pulse_us);                    // Pulsdauer in Zentimeter umrechnen
}

/* ── Pause aus Distanz berechnen ───────────────────────────── */
static int dist_to_pause_ms(float dist_cm)
{
    if (dist_cm < 0)              return -1;   // Kein Echo → kein Ton
    if (dist_cm < DIST_CONTACT)   return 0;    // Unter 8 cm → Dauerton (0 = Sonderfall)
    if (dist_cm < DIST_CLOSE)     return 100;  // Unter 20 cm → 100ms Pause
    if (dist_cm < DIST_NEAR)      return 300;  // Unter 50 cm → 300ms Pause
    if (dist_cm < DIST_MID)       return 600;  // Unter 100 cm → 600ms Pause
    if (dist_cm < DIST_FAR)       return 1000; // Unter 200 cm → 1000ms Pause
    return -1;                                 // Über 200 cm → kein Ton
}

/* ── Haupt-Task ────────────────────────────────────────────── */
void app_main(void)
{
    // gptimer initialisieren (1 MHz → 1 Tick = 1 µs)
    gptimer_config_t timer_cfg = {
        .clk_src       = GPTIMER_CLK_SRC_DEFAULT, // Standard-Taktquelle (APB-Clock)
        .direction     = GPTIMER_COUNT_UP,         // Timer zählt aufwärts
        .resolution_hz = 1000000,                  // 1 MHz → jeder Tick = 1 µs
    };
    ESP_ERROR_CHECK(gptimer_new_timer(&timer_cfg, &gptimer)); // Timer erstellen
    ESP_ERROR_CHECK(gptimer_enable(gptimer));                 // Timer aktivieren
    ESP_ERROR_CHECK(gptimer_start(gptimer));                  // Timer starten (zählt ab jetzt)

    // Trig-Pin als Ausgang konfigurieren
    gpio_config_t trig_cfg = {
        .pin_bit_mask = (1ULL << TRIG_GPIO),       // Bitmaske: nur TRIG_GPIO betroffen
        .mode         = GPIO_MODE_OUTPUT,           // Als Ausgang setzen
    };
    gpio_config(&trig_cfg);                        // Konfiguration anwenden
    gpio_set_level(TRIG_GPIO, 0);                  // Trig initial LOW setzen

    // Echo-Pin als Eingang mit Interrupt auf beide Flanken
    gpio_config_t echo_cfg = {
        .pin_bit_mask = (1ULL << ECHO_GPIO),       // Bitmaske: nur ECHO_GPIO betroffen
        .mode         = GPIO_MODE_INPUT,            // Als Eingang setzen
        .intr_type    = GPIO_INTR_ANYEDGE,          // Interrupt bei steigender UND fallender Flanke
    };
    gpio_config(&echo_cfg);                        // Konfiguration anwenden

    gpio_install_isr_service(0);                   // GPIO-ISR-Dienst starten (0 = Standard-Flags)
    gpio_isr_handler_add(ECHO_GPIO, echo_isr_handler, NULL); // ISR-Handler für Echo-Pin registrieren

    speaker_init();                                // LEDC für Lautsprecher initialisieren

    ESP_LOGI(TAG, "PDC gestartet | Temp: %.1f°C", TEMP_C); // Startmeldung im Monitor

    // Hauptschleife
    while (1) {
        float dist_cm  = measure_distance_cm();    // Distanz messen
        int   pause_ms = dist_to_pause_ms(dist_cm); // Pause aus Distanz berechnen

        if (dist_cm >= 0) {                        // Gültige Messung
            ESP_LOGI(TAG, "Distanz: %.1f cm", dist_cm); // Distanz im Monitor ausgeben
        } else {                                   // Kein Echo empfangen
            ESP_LOGI(TAG, "Distanz: kein Echo");   // Fehlermeldung im Monitor
        }

        if (pause_ms == 0) {                       // Unter 8 cm → Dauerton
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, BEEP_DUTY); // Ton einschalten
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);         // Änderung übernehmen
            vTaskDelay(pdMS_TO_TICKS(100));        // 100ms warten, dann neu messen
        } else if (pause_ms > 0) {                 // Objekt in Reichweite → Beep-Modus
            beep();                                // Einzelnen Beep ausgeben
            vTaskDelay(pdMS_TO_TICKS(pause_ms));   // Pause entsprechend Distanz warten
        } else {                                   // Kein Objekt in Reichweite
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0); // Ton sicher ausschalten
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0); // Änderung übernehmen
            vTaskDelay(pdMS_TO_TICKS(200));        // 200ms warten bis nächste Messung
        }
    }
}