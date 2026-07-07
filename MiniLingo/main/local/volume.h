#pragma once

// Lautstaerke-Logik (Spec Section 7, Commands 11-14): Stufen in
// 20-%-Schritten, mute merkt sich den vorherigen Wert. Der eigentliche
// Gain liegt als Q15-Multiplikator in drivers/audio_out.c.
//
// Setter laufen im afe_fetch_task (Command-Dispatch), volume_get()
// darf aus jedem Task lesen (atomar).

#define VOLUME_STEP 20

void volume_init(int initial_percent);
int volume_get(void);

void volume_set(int percent);     // clamp auf 0..100
void volume_adjust(int delta);    // z.B. +VOLUME_STEP / -VOLUME_STEP
void volume_mute(void);
void volume_unmute(void);         // stellt den Wert vor mute wieder her
