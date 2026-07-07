#pragma once                        // [C] Include-Guard-Ersatz: verhindert Mehrfach-Einbindung dieses Headers

#include <stdbool.h>                // [C] Header-Include | liefert den Typ bool für boolesche Werte
#include <stddef.h>                 // [C] Header-Include | liefert size_t für Byte-/Element-Größen
#include <stdint.h>                 // [C] Header-Include | liefert feste Integer-Breiten wie uint8_t

#include "esp_err.h"                // [C] Header-Include | [HW] esp_err_t-Fehlercode-Typ des ESP-IDF-Frameworks

// Cloud-Pipeline (Spec "network_task"): Whisper-STT (Phase 5) +
// Gemini-LLM (Phase 6) + Gemini-TTS mit Wiedergabe (Phase 7).

// Ereignistypen, die die Cloud-Pipeline über den Callback nach außen meldet.
// Bildet die Zustände der Sprachassistenten-Pipeline (Transkript, Antwort,
// Sprechen, Fertig, Fehler) für UI/Statemachine ab.
typedef enum {                                          // [C] typedef enum = benannter Aufzählungstyp
    CLOUD_EVT_TRANSCRIPT,  // text = Whisper-Transkript          // [Logik] Ergebnis der Spracherkennung liegt vor
    CLOUD_EVT_REPLY,       // text = Gemini-Antwort              // [Logik] LLM-Antworttext liegt vor
    CLOUD_EVT_SPEAKING,    // TTS-Wiedergabe startet (text = "") // [Logik] Wiedergabe der synthetisierten Stimme beginnt
    CLOUD_EVT_DONE,        // Pipeline fertig, zurueck zu Idle (text = "") // [Logik] Pipeline-Durchlauf abgeschlossen
    CLOUD_EVT_ERROR,       // text = kurze Fehlerbeschreibung (fuer UI)   // [Logik] Fehlerfall, Text für UI-Anzeige
} cloud_event_type_t;                                   // [C] Bezeichner des Aufzählungstyps

// Laeuft im network_task (Core 0, Prio 3) — keine LVGL-Calls, nur
// Flags/Queues setzen (gleiche Regel wie recorder_done_cb_t). text ist
// nur fuer die Dauer des Callbacks gueltig.
// Funktionszeigertyp für den Ereignis-Callback der Cloud-Pipeline: wird von
// cloud_init() registriert und bei jedem cloud_event_type_t aufgerufen.
typedef void (*cloud_event_cb_t)(cloud_event_type_t type,  // [C] Funktionszeiger-Typedef: Signatur eines Callbacks
                                 const char *text);         // [C] const char* = schreibgeschützter String-Parameter

// Startet den network_task (Spec Section 8: Prio 3, Core 0, 16 KB).
// Initialisiert die Cloud-Pipeline: legt Queue/Task an, registriert den
// Callback und setzt den TTS-Startwert. Einmalig beim Systemstart aufrufen.
esp_err_t cloud_init(cloud_event_cb_t cb);              // [C] Deklaration, Rückgabetyp esp_err_t (Fehlercode)

// Startet die Cloud-Pipeline fuer einen fertigen WAV-Buffer. Der
// Buffer (Recording-Buffer im PSRAM) muss gueltig bleiben, bis das
// Ergebnis-Event kommt — deshalb unterdrueckt afe_pipeline neue
// Wake-Word-Sessions, solange cloud_is_busy() (recorder_start wuerde
// den Buffer ueberschreiben). Aufruf aus dem afe_fetch_task.
// Stößt die vollständige Cloud-Pipeline (Whisper -> LLM -> TTS) für einen
// aufgenommenen WAV-Buffer an; liefert das Ergebnis asynchron per Callback.
void cloud_submit(const uint8_t *wav, size_t wav_bytes); // [C] const uint8_t* = unveränderlicher Byte-Puffer-Zeiger

// Prüft, ob die Cloud-Pipeline gerade einen Job bearbeitet (Busy-Gate für
// Wake-Word-Unterdrückung, damit der Recording-Buffer nicht überschrieben wird).
bool cloud_is_busy(void);                                // [C] Deklaration ohne Parameter, Rückgabe bool

// true, solange die TTS-Wiedergabe laeuft (SPEAKING-State der Spec).
// Der afe_fetch_task laesst waehrenddessen MultiNet fuer den
// "stop"-Command mitlaufen (WakeNet-Detections sind ueber das
// cloud_is_busy-Gate eh unterdrueckt — kein Self-Trigger).
bool cloud_is_speaking(void);                            // [C] Deklaration, liefert den SPEAKING-Teilzustand von busy

// Laufzeit-Schalter fuer das Vorlesen der Antworten (ersetzt den
// reinen Compile-Schalter CONFIG_TTS_SPEAK_REPLIES, der nur noch den
// Startwert vorgibt). Die 2,5 MB PSRAM-TTS-Buffer werden lazy beim
// ersten tatsaechlichen Vorlesen allokiert — aus bleibt also so
// speichersparsam wie bisher. Atomar, aus jedem Task aufrufbar.
// Schaltet das automatische Vorlesen von LLM-Antworten zur Laufzeit ein/aus.
void cloud_set_tts_enabled(bool enabled);                // [C] Setter-Deklaration, Parameter bool
// Liefert den aktuellen Zustand des TTS-Vorlese-Schalters.
bool cloud_tts_enabled(void);                            // [C] Getter-Deklaration

// Spricht text ueber den lokalen Piper-TTS ohne Whisper/LLM-Roundtrip.
// Nur aktiv, wenn cloud_tts_enabled() && WiFi verbunden. Verwendet
// denselben Busy-Gate wie cloud_submit — bei belegter Pipeline wird der
// Aufruf verworfen (Bubble steht ohnehin schon im Display).
// Aufruf aus afe_fetch_task (on_local_command); text wird kopiert.
// Spricht einen fest vorgegebenen Text direkt per TTS, ohne den Umweg über
// Whisper/LLM (z. B. für lokale Sprachkommandos/Bestätigungen).
void cloud_speak(const char *text);                      // [C] Deklaration, text wird intern per Wert kopiert
