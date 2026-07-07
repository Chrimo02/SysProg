#pragma once

// Lautstaerke-Logik (Spec Section 7, Commands 11-14): Stufen in
// 20-%-Schritten, mute merkt sich den vorherigen Wert. Der eigentliche
// Gain liegt als Q15-Multiplikator in drivers/audio_out.c.
//
// Setter laufen im afe_fetch_task (Command-Dispatch), volume_get()
// darf aus jedem Task lesen (atomar).

#define VOLUME_STEP 20              // [C] Präprozessor-Konstante | [Logik] Schrittweite pro Lautstärke-Kommando (+/- 20 %)

void volume_init(int initial_percent);  // [Logik] Initialisiert die Lautstärke beim Boot auf einen Startwert
int volume_get(void);                   // [Logik] Liefert die aktuell aktive Lautstärke in Prozent (0..100)

void volume_set(int percent);     // clamp auf 0..100         // [Logik] Setzt Lautstärke direkt, wird intern auf 0..100 begrenzt
void volume_adjust(int delta);    // z.B. +VOLUME_STEP / -VOLUME_STEP   // [Logik] Relative Änderung der aktuellen Lautstärke
void volume_mute(void);           // [Logik] Schaltet auf Lautstärke 0, merkt sich den vorherigen Wert
void volume_unmute(void);         // stellt den Wert vor mute wieder her   // [Logik] Stellt den vor volume_mute() gültigen Wert wieder her
