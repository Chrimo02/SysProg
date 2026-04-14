#include "esp_timer.h"

#define GPIO_BASE       0x60004000UL
#define IO_MUX_BASE     0x60009000UL

#define GPIO_W1TS       (*( uint32_t*)(GPIO_BASE + 0x008))
#define GPIO_W1TC       (*( uint32_t*)(GPIO_BASE + 0x00C))
#define GPIO_EN_W1TS    (*( uint32_t*)(GPIO_BASE + 0x024))
#define IO_MUX_GPIO18   (*( uint32_t*)(IO_MUX_BASE + 0x4C))
#define GPIO_FUNC18_OUT (*( uint32_t*)(GPIO_BASE + 0x554 + 18*4))

#define PIN (1u << 18)

// ── Notenfrequenzen in Hz ─────────────────────────────────────────────
#define C4  262
#define D4  294
#define E4  330
#define F4  349
#define G4  392
#define A4  440
#define REST 0

#define B4  494
#define C5  523
#define D5  587
#define E5  659
#define F5  698
#define G5  784
#define A5  880

static esp_timer_handle_t toggle_timer;
static esp_timer_handle_t stop_timer;

static void on_toggle(void* arg) {
    static int level = 0;
    level ^= 1;
    if (level) GPIO_W1TS = PIN;
    else       GPIO_W1TC = PIN;
}

static void on_stop(void* arg) {
    esp_timer_stop(toggle_timer);
    GPIO_W1TC = PIN;
}

void play_tone(uint32_t freq_hz, uint32_t ms) {
    if (freq_hz == REST) {
        // Pause: kein Toggle, nur warten via one-shot timer
        esp_timer_start_once(stop_timer, (uint64_t)ms * 1000);
        return;
    }
    uint32_t half_us = 500000u / freq_hz;
    esp_timer_start_periodic(toggle_timer, half_us);
    esp_timer_start_once(stop_timer, (uint64_t)ms * 1000);
}

// ── Melodie-Struktur ──────────────────────────────────────────────────
typedef struct {
    uint32_t freq;
    uint32_t ms;
} Note;

// Al-le  mei-ne  Ent-chen  |  schwim-men  auf  dem  See  (x2)
// Köpf-chen  in  das  Was-ser  |  Schwänz-chen  in  die  Höh
/* static const Note melody[] = {
    {C4, 300}, {D4, 300}, {E4, 300}, {F4, 300},
    {G4, 450}, {REST, 150}, {G4, 450}, {REST, 150},
    {A4, 300}, {A4, 300}, {A4, 300}, {A4, 300}, {G4, 600}, {REST, 150},
    {A4, 300}, {A4, 300}, {A4, 300}, {A4, 300}, {G4, 600}, {REST, 150},
    {F4, 300}, {F4, 300}, {F4, 300}, {F4, 300},
    {E4, 450}, {REST, 150}, {E4, 450}, {REST, 150},
    {G4, 300}, {G4, 300}, {G4, 300}, {G4, 300}, {C4, 600},
}; */

static const Note melody[] = {
    // Phrase A
    {E5, 400}, {B4, 200}, {C5, 200}, {D5, 400}, {C5, 200}, {B4, 200},
    {A4, 400}, {A4, 200}, {C5, 200}, {E5, 400}, {D5, 200}, {C5, 200},
    {B4, 600}, {C5, 200}, {D5, 400}, {E5, 400},
    {C5, 400}, {A4, 400}, {A4, 600}, {REST, 200},

    // Phrase B
    {D5, 400}, {F5, 200}, {A5, 400}, {G5, 200}, {F5, 200},
    {E5, 600}, {C5, 200}, {E5, 400}, {D5, 200}, {C5, 200},
    {B4, 400}, {B4, 200}, {C5, 200}, {D5, 400}, {E5, 400},
    {C5, 400}, {A4, 400}, {A4, 400}, {REST, 200},

    // Phrase A wiederholt
    {E5, 400}, {B4, 200}, {C5, 200}, {D5, 400}, {C5, 200}, {B4, 200},
    {A4, 400}, {A4, 200}, {C5, 200}, {E5, 400}, {D5, 200}, {C5, 200},
    {B4, 600}, {C5, 200}, {D5, 400}, {E5, 400},
    {C5, 400}, {A4, 400}, {A4, 600}, {REST, 200},
};

#define NOTE_COUNT (sizeof(melody) / sizeof(melody[0]))

static int note_index = 0;
static esp_timer_handle_t melody_timer; // startet die nächste Note

static void on_next_note(void* arg) {
    if (note_index >= NOTE_COUNT) return;

    Note n = melody[note_index++];
    play_tone(n.freq, n.ms);

    // nächste Note nach Tondauer + 50ms Pause (trennt Noten voneinander)
    esp_timer_start_once(melody_timer, (uint64_t)(n.ms + 50) * 1000);
}

void app_main(void) {
    IO_MUX_GPIO18   = (1 << 12) | (3 << 10);
    GPIO_FUNC18_OUT = 256;
    GPIO_EN_W1TS    = PIN;

    esp_timer_create_args_t toggle_args = {
        .callback        = on_toggle,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = "toggle",
    };
    esp_timer_create_args_t stop_args = {
        .callback        = on_stop,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = "stop",
    };
    esp_timer_create_args_t melody_args = {
        .callback        = on_next_note,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = "melody",
    };
    esp_timer_create(&toggle_args,  &toggle_timer);
    esp_timer_create(&stop_args,    &stop_timer);
    esp_timer_create(&melody_args,  &melody_timer);

    // erste Note sofort starten
    esp_timer_start_once(melody_timer, 0);
}

