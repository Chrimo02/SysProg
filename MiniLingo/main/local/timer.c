// Kitchen-Timer (Spec Section 7): ein One-Shot-esp_timer, die Restzeit
// rechnet sich aus der gespeicherten Deadline — so braucht der
// 1-Hz-Countdown in der Status-Bar keinen eigenen Tick-Timer.
#include <stdatomic.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "canned_audio.h"
#include "timer.h"

static const char *TAG = "kitchen_timer";

static esp_timer_handle_t s_timer;
static kitchen_timer_done_cb_t s_done_cb;
static atomic_llong s_deadline_us;  // 0 = kein Timer aktiv // "us" = microseconds // atomic_llong = 64-bit signed integer -> kommt aus stdatomic.h

static void timer_done(void *arg)
{
    atomic_store(&s_deadline_us, 0); // Timer ist abgelaufen, keine Restzeit mehr
    ESP_LOGI(TAG, "timer done");
    canned_audio_play(CANNED_TIMER_DONE);  // spielt den Sound CANNED_TIMER_DONE ab
    canned_audio_play(CANNED_ALARM); // spielt den Sound CANNED_ALARM ab
    if (s_done_cb) {
        s_done_cb(); // Callback, damit on_timer_done() in main.c aufgerufen wird und die UI (Bubble) aktualisiert werden kann
    }
}

// function um kitchen_timer zu initialisieren, wird in main.c aufgerufen, um den Callback on_timer_done zu registrieren
void kitchen_timer_init(kitchen_timer_done_cb_t done_cb) 
{
    s_done_cb = done_cb; // ist der callback auf on_timer_done() aus main.c, damit die UI (Bubble) aktualisiert werden kann, wenn der Timer abgelaufen ist

    // hier die args für den esp_timer (=hardware-timer) erstellen (callback = timer_done, name = "kitchen_timer")
    const esp_timer_create_args_t args = {
        .callback = timer_done,
        .name = "kitchen_timer",
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_timer)); // erstellt den hardware-timer, gibt ihm über args die aufzurufende callback-funktion wenn der timer abgelaufen ist und der interrupt ausgelöst wird. s_timer ist der handle auf den timer, der static in timer.c gespeichert wird, damit in den anderen kitchen_timer funktionen (start/cancel/remainaining) auf den timer zugegriffen werden kann
}

void kitchen_timer_start(int seconds)
{
    esp_timer_stop(s_timer);  //falls der timer schon läuft, wird er gestoppt, damit er neu gestartet werden kann
    atomic_store(&s_deadline_us,
                 esp_timer_get_time() + (int64_t)seconds * 1000000); // speichert die deadline in microsekunden, damit der countdown in der status-bar berechnet werden kann (restzeit = deadline - aktuelle zeit)
    ESP_ERROR_CHECK(esp_timer_start_once(s_timer,
                                         (uint64_t)seconds * 1000000)); // startet den timer, der nach "seconds" sekunden den callback timer_done() aufruft
    ESP_LOGI(TAG, "timer started: %d s", seconds);
}

bool kitchen_timer_cancel(void)
{
    //check ob überhaupt ein timer läuft
    if (atomic_exchange(&s_deadline_us, 0) == 0) {
        return false; //return false, wenn überhaupt kein timer läuft, damit in main.c die bubble "no timer running" angezeigt werden kann
    }
    esp_timer_stop(s_timer); // gab einen timer -> stoppe ihn und returne true, damit in main.c die bubble "timer cancelled" angezeigt werden kann
    ESP_LOGI(TAG, "timer cancelled");
    return true;
}

int kitchen_timer_remaining_s(void)
{
    long long deadline = atomic_load(&s_deadline_us); // deadline-wert einmalig atomar auslesen
    if (deadline == 0) {
        return -1; // wenn es keinen timer gab, returne -1, damit in main.c die bubble "no timer running" angezeigt werden kann
    }
    long long left_us = deadline - esp_timer_get_time(); //verbleibende zeit berechnen
    if (left_us < 0) {
        left_us = 0; // Schutz vor negativem Wert, falls der Timer schon abgelaufen ist, aber der Callback noch nicht aufgerufen wurde (zB durch Interrupt-Latenz)
    }
    return (int)((left_us + 999999) / 1000000);
}
