# MiniLingo Voice Assistant — Implementation Spec

> Sprachassistent auf ESP32-S3 mit hybrider Architektur:
> Wake-Word + lokale Befehlserkennung auf dem Chip, freier Cloud-Stack
> (Groq + Gemini) für Free-Form-Konversation. Hardware-Komponenten sind
> bereits einzeln getestet (siehe `KOMPONENTENTEST.md`).

---

## Quick Reference

| Layer            | Technologie                                                |
|------------------|------------------------------------------------------------|
| Firmware         | ESP-IDF v5.x (C, FreeRTOS)                                |
| GUI              | LVGL v9 (Managed Component)                                |
| Display LCD      | esp_lcd + esp_lcd_ili9341 (Managed)                       |
| Touch            | XPT2046 via SPI, eigener LVGL-Input-Driver                |
| Speech Stack     | esp-sr v2.x (WakeNet9 + MultiNet7 + AFE)                  |
| Audio In         | I2S0, INMP441, 16 kHz mono (durchgehend, auch im Idle)    |
| Audio Out        | I2S1, MAX98357A, 24 kHz mono                              |
| HTTP/TLS         | esp_http_client + mbedTLS + esp_crt_bundle                |
| JSON             | cJSON (built-in)                                          |
| Wake Word        | "Alexa" (WakeNet9, kostenlos vortrainiert)                |
| Local Commands   | MultiNet7 — Timer, Stop, Volume (offline)                 |
| STT (cloud)      | Groq Whisper Large v3 — `whisper-large-v3-turbo`          |
| LLM (cloud)      | Google Gemini 2.5 Flash                                   |
| TTS (cloud)      | Google Gemini 2.5 Flash Preview TTS (PCM 24 kHz)          |
| API Keys         | `GROQ_API_KEY`, `GEMINI_API_KEY` (beide Free Tier)        |

---

## 1. Projektübersicht

Sprachgesteuerter Assistent mit Touchscreen, der **always-on** auf das
Wake-Word "Alexa" hört. Häufige Befehle (Timer, Stop, Lautstärke)
werden komplett lokal verarbeitet und brauchen kein Internet. Freie
Fragen ("Wie ist das Wetter?", "Erklär mir Rekursion") gehen
automatisch in die Cloud.

### User-Flow

1. **Idle**: Wake-Word-Detection läuft kontinuierlich im Hintergrund.
   Mic-Button auf dem Display ist grau, Status-Bar zeigt "Listening for
   Alexa".
2. **Trigger**: User sagt "Alexa" *oder* tippt den Mic-Button → State
   wechselt zu `LISTENING`, Button wird rot, ein kurzer Bestätigungston
   wird abgespielt (optional).
3. **Capture**: Die nächsten max. 8 Sekunden Audio werden aufgenommen,
   parallel läuft MultiNet auf dem Audio-Stream.
4. **Routing-Entscheidung**:
   - **MultiNet match** → State wechselt zu `LOCAL_RESPONSE`, der
     entsprechende Handler läuft (Timer setzen, Volume ändern, TTS
     pausieren). Keine Cloud-Calls.
   - **MultiNet timeout** (kein Match nach ~4s nach Audio-Ende)
     *oder* Audio länger als ~3s → State wechselt zu `PROCESSING`,
     Cloud-Pipeline startet.
5. **Cloud-Pipeline** (falls Schritt 4 → no match):
   - Groq Whisper transkribiert das Audio.
   - Gemini Flash beantwortet den Text.
   - Gemini TTS generiert das Audio.
6. **Speaking**: State `SPEAKING`, Button wird grün, PCM wird über I2S1
   wiedergegeben. Wake-Word-Detection ist **deaktiviert** während dieser
   Phase (Self-Trigger-Schutz).
7. **Idle**: State zurück zu `IDLE`, Wake-Word-Detection wieder aktiv.

Der User-flow für lokale Commands (Schritt 4a) hat ~150ms Latenz vom
Befehlsende bis zur Reaktion. Cloud-Flow (Schritt 4b) ~2-4 Sekunden.

---

## 2. Architektur: was läuft wo

### Lokal auf dem ESP32-S3

- **AFE Pipeline** (esp-sr Audio Front-End): kontinuierliches Mikrofon-
  Capture, Noise-Suppression, Wake-Word-Detection (WakeNet9), optional
  AEC.
- **MultiNet7**: Offline-Command-Recognition (~20 Phrasen fest definiert).
- **Lokale Command-Handler**: Timer (FreeRTOS-Software-Timer), Volume
  (I2S-Output-Gain-Setting), Stop (bricht TTS-Wiedergabe ab).
- **HTTPS-Clients** für die drei Cloud-Endpoints, JSON-Parsing,
  Base64-Decoding für TTS-Audio.
- **Audio-Wiedergabe**: PCM 24 kHz → MAX98357A.
- **Komplette UI**: LVGL-Rendering, Touch-Handling, Chat-History.
- **State-Machine** + FreeRTOS-Task-Orchestrierung.

### In der Cloud (alle Free Tier, kein Credit Card)

- **Groq Whisper** (`api.groq.com/openai/v1/audio/transcriptions`):
  Audio → Text. Free Tier: 2.000 Requests/Tag, 7.200 Audiosekunden/Stunde.
- **Google Gemini 2.5 Flash** (`generativelanguage.googleapis.com`):
  Text → Text. Free Tier: 1.500 Requests/Tag, 15 RPM, 1M TPM.
- **Google Gemini 2.5 Flash Preview TTS** (gleicher Endpoint):
  Text → PCM (base64-encoded, 24 kHz, 16-bit, mono).

### Hybrid-Routing-Logik

```
                       ┌────────────────────┐
                       │  Trigger Event     │
                       │ Wake Word | Touch  │
                       └──────────┬─────────┘
                                  ▼
                       ┌────────────────────┐
                       │  Audio Capture     │
                       │  (max 8s, VAD)     │
                       └──────────┬─────────┘
                                  ▼
                       ┌────────────────────┐
                       │  MultiNet match?   │
                       └─────┬──────────┬───┘
                       Yes ◀─┘          └─▶ No / Timeout
                         │                    │
                         ▼                    ▼
                ┌────────────────┐   ┌────────────────┐
                │ Local Handler  │   │ Cloud Pipeline │
                │ Timer / Stop / │   │ Whisper → LLM  │
                │ Volume         │   │ → TTS          │
                └────────────────┘   └────────────────┘
```

### Wichtige Hinweise zum Free Tier

- **Datenschutz**: Auf dem Gemini Free Tier nutzt Google API-Inputs und
  -Outputs zum Training. Für Hobby-Projekte OK, sensible Daten besser
  vermeiden.
- **Rate Limits**: Bei intensiver Nutzung (Burst-Anfragen) bekommt man
  HTTP 429. Im Code Retry-Logik mit Exponential Backoff einbauen.
- **Modell-Lifecycle**: Gemini Preview-Models können sich ändern. Den
  Modellnamen als zentrale Konstante führen.

---

## 3. Hardware Pin Map (unverändert vs. v1)

| Komponente   | Funktion    | ESP32-S3 GPIO | Bus     | Notizen                    |
|--------------|-------------|---------------|---------|----------------------------|
| INMP441      | SD (Data)   | 6             | I2S0 RX |                            |
| INMP441      | SCK         | 5             | I2S0 RX |                            |
| INMP441      | WS          | 4             | I2S0 RX |                            |
| INMP441      | L/R         | GND           | —       | Linker Kanal               |
| INMP441      | VDD         | 3V3           | —       |                            |
| MAX98357A    | DIN         | 17            | I2S1 TX |                            |
| MAX98357A    | LRC         | 16            | I2S1 TX |                            |
| MAX98357A    | BCLK        | 15            | I2S1 TX |                            |
| MAX98357A    | SD          | 3V3           | —       | Shutdown disabled          |
| MAX98357A    | GAIN        | offen         | —       | = 9 dB                     |
| ILI9341      | MOSI/SDI    | 11            | SPI2    | shared mit Touch           |
| ILI9341      | SCK         | 12            | SPI2    | shared mit Touch           |
| ILI9341      | MISO/SDO    | 13            | SPI2    | shared mit Touch           |
| ILI9341      | CS          | 10            | SPI2    | eigener CS                 |
| ILI9341      | DC/RS       | 8             | —       |                            |
| ILI9341      | RESET       | 9             | —       |                            |
| ILI9341      | LED         | 3V3           | —       | Backlight immer an         |
| XPT2046      | T_DIN       | 11            | SPI2    | shared mit Display         |
| XPT2046      | T_CLK       | 12            | SPI2    | shared mit Display         |
| XPT2046      | T_DO        | 13            | SPI2    | shared mit Display         |
| XPT2046      | T_CS        | 14            | SPI2    | eigener CS                 |
| XPT2046      | T_IRQ       | nicht angeschlossen | — | Polling alle 50 ms         |
| Lautsprecher | 2x 4Ω 3W    | —             | —       | in Reihe = 8Ω Gesamtlast   |

---

## 4. Display & UI Design

### Orientation und Auflösung

ILI9341 im **Portrait-Mode**, 240 × 320, RGB565. LVGL mit
`LV_COLOR_DEPTH=16` und `LV_COLOR_16_SWAP=1` (ILI9341 ist big-endian).

### Layout (240 × 320)

```
┌────────────────────────────────────────┐ y=0
│  📶  · Listening for Alexa · 🎚 60%   │  status bar (30px)
├────────────────────────────────────────┤ y=30
│                                        │
│                    ┌──────────────┐    │
│                    │ Wie ist das  │    │  user bubble (rechts)
│                    │ Wetter hier? │    │
│                    └──────────────┘    │
│  ┌──────────────────────┐              │
│  │ In Würzburg sind es  │              │  assistant bubble (links)
│  │ heute 18°C und       │              │
│  │ teilweise bewölkt…   │              │
│  └──────────────────────┘              │
│                                        │  chat area (230px, scrollbar)
│                                        │
├────────────────────────────────────────┤ y=260
│                                        │
│              ┌──────┐                  │
│              │  🎤  │                  │  mic button (60px)
│              └──────┘                  │
└────────────────────────────────────────┘ y=320
```

### Status-Bar (oben, 30px)

Zeigt drei Informationen:
- **Links**: WiFi-Symbol (`LV_SYMBOL_WIFI`) + verbunden-Status.
- **Mitte**: Aktueller State als Text:
  - `Listening for Alexa` (Idle, Wake-Word aktiv)
  - `Listening…` (Capture läuft)
  - `Thinking…` (Cloud-Request läuft)
  - `Speaking…` (TTS-Wiedergabe)
  - `Timer: 04:23` (wenn Timer läuft, ersetzt State-Text)
- **Rechts**: Lautstärke-Indikator (`LV_SYMBOL_VOLUME_MAX` + Prozent).

### Chat-Bubbles

- **User-Bubble**: rechts-aligned, blau (`#378ADD`), weißer Text,
  `border-radius=12`, padding 10px, max. 200px breit.
- **Assistant-Bubble** (Cloud): links-aligned, grau (`#3A3A3A`), heller
  Text.
- **Local-Response-Bubble** (für lokale Befehle): links-aligned, **grün**
  (`#2D5A3D`), heller Text — visuelles Feedback, dass kein Cloud-Call
  stattfand.
- Vertikaler Abstand: 8px. Text-Wrapping via
  `lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP)`.

### Mic-Button-Zustände

| State              | Farbe       | Animation                |
|--------------------|-------------|--------------------------|
| `IDLE`             | `#888888`   | keine (Wake-Word lauscht)|
| `LISTENING`        | `#E24B4A`   | Puls mit RMS-Amplitude   |
| `LOCAL_RESPONSE`   | `#1D9E75`   | kurzer Check-Pulse       |
| `PROCESSING`       | `#EF9F27`   | LVGL-Spinner             |
| `SPEAKING`         | `#1D9E75`   | Sinus-Welle              |

### Scrolling

LVGL-native: Chat-Container mit `LV_SCROLL_DIR_VER`,
`lv_obj_scroll_to_view(new_bubble, LV_ANIM_ON)` nach neuer Nachricht.

---

## 5. esp-sr Setup (Audio Front-End)

Das ist der zentrale neue Baustein. esp-sr bietet eine `afe`-Pipeline,
die kontinuierlich Audio vom Mic verarbeitet und sowohl WakeNet als auch
MultiNet versorgt. Die Pipeline läuft auf Core 1 in einem eigenen Task
und gibt verarbeitete Audio-Frames + Erkennungsergebnisse über einen
Fetch-Callback aus.

### Komponente hinzufügen

```bash
idf.py add-dependency "espressif/esp-sr^2.0"
```

### Model-Auswahl in menuconfig

Unter `ESP Speech Recognition`:

- **WakeNet**: `wn9_alexa` aktivieren (alle anderen deaktivieren, um
  Flash zu sparen).
- **MultiNet**: `mn7_en` (Englisch, ~200 KB Modell, optimiert für S3).
- **AFE**: AFE für ESP32-S3 aktivieren, AEC zunächst **aus** (kommt in
  Phase 8 als Erweiterung), VAD im AFE aktiviert.

Die Modelle werden in eine separate Flash-Partition geflasht (siehe
`partitions.csv` im Anhang).

### Memory-Budget

- WakeNet9 Alexa: ~50 KB Modell, ~30 KB Runtime-RAM.
- MultiNet7 EN: ~200 KB Modell, ~150 KB Runtime-RAM (PSRAM).
- AFE-Buffer: ~30 KB internes RAM.

**Konsequenz**: PSRAM ist Pflicht. Internes SRAM bleibt für I2S-DMA
und LVGL-Buffer.

### AFE-Pipeline-Code-Gerüst

```c
#include "esp_afe_sr_models.h"
#include "esp_mn_models.h"
#include "model_path.h"

static srmodel_list_t *models;
static esp_afe_sr_iface_t *afe_handle;
static esp_afe_sr_data_t  *afe_data;
static esp_mn_iface_t     *multinet;
static model_iface_data_t *mn_data;

void speech_init(void) {
    models = esp_srmodel_init("model");  // Flash-Partition "model"

    // WakeNet wird vom AFE selbst geladen via Config.
    afe_config_t afe_cfg = AFE_CONFIG_DEFAULT();
    afe_cfg.aec_init   = false;     // AEC aus (Phase 8)
    afe_cfg.se_init    = true;      // Speech Enhancement an
    afe_cfg.vad_init   = true;
    afe_cfg.wakenet_init = true;
    afe_cfg.wakenet_model_name =
        esp_srmodel_filter(models, ESP_WN_PREFIX, "alexa");
    afe_cfg.pcm_config.total_ch_num = 1;
    afe_cfg.pcm_config.mic_num      = 1;
    afe_cfg.pcm_config.ref_num      = 0;
    afe_cfg.pcm_config.sample_rate  = 16000;

    afe_handle = (esp_afe_sr_iface_t *)&ESP_AFE_SR_HANDLE;
    afe_data   = afe_handle->create_from_config(&afe_cfg);

    char *mn_name = esp_srmodel_filter(models, ESP_MN_PREFIX, ESP_MN_ENGLISH);
    multinet = esp_mn_handle_from_name(mn_name);
    mn_data  = multinet->create(mn_name, 6000);  // 6s Timeout

    // Custom Commands registrieren (siehe Section 7)
    speech_commands_init();
}
```

### Audio-Feed-Task

```c
static void afe_feed_task(void *arg) {
    int chunk_size = afe_handle->get_feed_chunksize(afe_data);
    int16_t *buf = heap_caps_malloc(chunk_size * sizeof(int16_t) * 2,
                                    MALLOC_CAP_INTERNAL);
    while (1) {
        // Aus I2S0 lesen
        size_t bytes_read = 0;
        i2s_channel_read(i2s_rx_handle, buf, chunk_size * sizeof(int16_t),
                         &bytes_read, portMAX_DELAY);
        afe_handle->feed(afe_data, buf);
    }
}
```

### Audio-Fetch-Task (Erkennungs-Output)

```c
static void afe_fetch_task(void *arg) {
    while (1) {
        afe_fetch_result_t *res = afe_handle->fetch(afe_data);
        if (!res || res->ret_value == ESP_FAIL) continue;

        // Wake Word detected?
        if (res->wakeup_state == WAKENET_DETECTED) {
            on_wake_word_detected();    // setzt State auf LISTENING
            continue;
        }

        // Im LISTENING-State Frame an MultiNet weitergeben
        if (current_state == STATE_LISTENING) {
            // Frame an Recording-Buffer für mögliche Cloud-Pipeline
            record_audio_frame(res->data, res->data_size);

            // Parallel an MultiNet
            esp_mn_state_t mn_state = multinet->detect(mn_data, res->data);
            if (mn_state == ESP_MN_STATE_DETECTED) {
                esp_mn_results_t *mn_res = multinet->get_results(mn_data);
                on_local_command(mn_res->command_id[0]);
                multinet->clean(mn_data);
            } else if (mn_state == ESP_MN_STATE_TIMEOUT) {
                on_capture_finished_no_match();  // → Cloud
            }
        }
    }
}
```

---

## 6. Wake Word

### Konfiguration

- Wake Word: **"Alexa"**.
- Model: `wn9_alexa` (WakeNet9, ESP32-S3 optimiert).
- Empfindlichkeit (`det_threshold`): Default belassen, ggf. nach Test
  anpassen. Wert zwischen 0.0 und 1.0, höher = strenger.

### Self-Trigger-Schutz

Problem: Während TTS-Wiedergabe nimmt das Mikrofon den eigenen
Lautsprecher auf — wenn die Antwort zufällig das Wort "Alexa" enthält,
würde sich das System selbst triggern.

Lösung **ohne** AEC (Phase 1-7):

```c
// In der State-Machine
case STATE_SPEAKING:
    afe_handle->disable_wakenet(afe_data);
    break;
case STATE_IDLE:
    afe_handle->enable_wakenet(afe_data);
    break;
```

Lösung **mit** AEC (Phase 8, optional): AFE im `aec_init = true`
Modus konfigurieren, Reference-Signal vom DAC bereitstellen — entfernt
das eigene Audio aus dem Mic-Stream. Erfordert ~80 KB zusätzliches RAM
und exakte Kalibrierung der Audio-Latenz.

### Bestätigungs-Ton

Optional bei Wake-Word-Detection einen kurzen Beep abspielen (z.B.
200ms 1 kHz Sinus aus dem Mic-Test). Verbessert die User-Experience
deutlich — der User weiß, dass das Gerät zuhört.

---

## 7. Lokale Commands (MultiNet)

### Command-Liste (Englisch)

MultiNet matcht feste Phrasen — Variable-Slot-Erkennung (z.B. eine
beliebige Zahl) ist nicht unterstützt. Daher feste Set:

| Command ID | Phrase                          | Handler                       |
|-----------:|----------------------------------|-------------------------------|
| 1          | `set timer one minute`          | Timer 60s starten             |
| 2          | `set timer five minutes`        | Timer 300s starten            |
| 3          | `set timer ten minutes`         | Timer 600s starten            |
| 4          | `set timer fifteen minutes`     | Timer 900s starten            |
| 5          | `set timer thirty minutes`      | Timer 1800s starten           |
| 6          | `set timer thirty seconds`      | Timer 30s starten             |
| 7          | `stop timer`                    | Timer abbrechen               |
| 8          | `cancel timer`                  | Timer abbrechen               |
| 9          | `how much time left`            | Restzeit ansagen              |
| 10         | `stop`                          | TTS-Wiedergabe abbrechen      |
| 11         | `volume up`                     | Output-Gain +20%              |
| 12         | `volume down`                   | Output-Gain −20%              |
| 13         | `mute`                          | Output-Gain auf 0             |
| 14         | `unmute`                        | Output-Gain auf vorherigem Wert |

Beliebige andere Timer-Werte (z.B. "set timer seven minutes") matchen
nicht und gehen automatisch in die Cloud — Gemini kann den Wert dann
extrahieren und über einen Function-Call zurückgeben (Phase 9, optional).

### Commands registrieren

```c
static void speech_commands_init(void) {
    esp_mn_commands_alloc(multinet, mn_data);

    esp_mn_commands_add(1,  "set timer one minute");
    esp_mn_commands_add(2,  "set timer five minutes");
    esp_mn_commands_add(3,  "set timer ten minutes");
    esp_mn_commands_add(4,  "set timer fifteen minutes");
    esp_mn_commands_add(5,  "set timer thirty minutes");
    esp_mn_commands_add(6,  "set timer thirty seconds");
    esp_mn_commands_add(7,  "stop timer");
    esp_mn_commands_add(8,  "cancel timer");
    esp_mn_commands_add(9,  "how much time left");
    esp_mn_commands_add(10, "stop");
    esp_mn_commands_add(11, "volume up");
    esp_mn_commands_add(12, "volume down");
    esp_mn_commands_add(13, "mute");
    esp_mn_commands_add(14, "unmute");

    esp_mn_commands_update();
}
```

### Command-Handler

```c
static esp_timer_handle_t kitchen_timer = NULL;
static int active_timer_seconds = 0;
static int64_t timer_start_us = 0;

static void on_local_command(int command_id) {
    switch (command_id) {
        case 1: start_timer(60); break;
        case 2: start_timer(300); break;
        case 3: start_timer(600); break;
        // ...
        case 7:
        case 8: cancel_timer(); break;
        case 9: announce_time_left(); break;
        case 10: abort_tts_playback(); break;
        case 11: volume_adjust(+20); break;
        case 12: volume_adjust(-20); break;
        case 13: volume_set(0); break;
        case 14: volume_set(previous_volume); break;
    }
    set_state(STATE_LOCAL_RESPONSE);
    // → kurze TTS-Ausgabe (vorab generiert, siehe unten)
    play_canned_response(command_id);
    set_state(STATE_IDLE);
}
```

### Pre-Generated Audio-Responses (offline TTS)

Für lokale Commands keine Cloud-TTS — stattdessen WAV-Files im SPIFFS,
die einmalig (z.B. per Build-Script) generiert werden:

```
spiffs/
├── tts_timer_set_60s.wav
├── tts_timer_set_300s.wav
├── tts_timer_set_600s.wav
├── tts_timer_set_900s.wav
├── tts_timer_set_1800s.wav
├── tts_timer_cancelled.wav
├── tts_timer_done.wav
├── tts_volume_changed.wav
└── tts_muted.wav
```

Vorab mit einem Python-Script via Gemini-TTS-API erzeugen, in SPIFFS
flashen. Beim Boot wird auf Existenz geprüft. So bleiben lokale
Commands komplett offline-fähig.

### Timer-Implementierung

```c
static void timer_done_callback(void *arg) {
    play_wav_from_spiffs("tts_timer_done.wav");
    // Optional: zusätzlich ein Alarmgeräusch wiederholt
    kitchen_timer = NULL;
}

static void start_timer(int seconds) {
    if (kitchen_timer) esp_timer_delete(kitchen_timer);
    esp_timer_create_args_t args = {
        .callback = timer_done_callback,
        .name = "kitchen_timer",
    };
    esp_timer_create(&args, &kitchen_timer);
    esp_timer_start_once(kitchen_timer, seconds * 1000000LL);
    active_timer_seconds = seconds;
    timer_start_us = esp_timer_get_time();
    // Status-Bar updaten via lv_async_call
    ui_show_timer(seconds);
}
```

Restzeit in der Status-Bar live aktualisieren (1Hz-LVGL-Timer).

---

## 8. Software-Architektur

### FreeRTOS-Tasks

| Task              | Prio | Core | Stack  | Aufgabe                                     |
|-------------------|------|------|--------|---------------------------------------------|
| `lvgl_task`       | 4    | 0    | 8 KB   | LVGL-Tick + UI-Render                       |
| `afe_feed_task`   | 5    | 1    | 4 KB   | I2S0 → AFE feed                             |
| `afe_fetch_task`  | 5    | 1    | 8 KB   | AFE fetch + WakeNet + MultiNet              |
| `network_task`    | 3    | 0    | 16 KB  | HTTP-Requests (Whisper → Gemini → TTS)      |
| `audio_out_task`  | 5    | 1    | 4 KB   | tts_queue → I2S1                            |
| `main_task`       | 2    | 0    | 4 KB   | State-Machine, Koordination                 |

### State-Machine

```c
typedef enum {
    STATE_IDLE,            // WakeNet aktiv, wartet auf "Alexa" oder Touch
    STATE_LISTENING,       // Audio-Capture + MultiNet aktiv
    STATE_LOCAL_RESPONSE,  // Lokaler Command-Handler läuft
    STATE_PROCESSING,      // Cloud-Pipeline läuft (Whisper/Gemini/TTS)
    STATE_SPEAKING,        // TTS-Wiedergabe (WakeNet aus)
    STATE_ERROR
} app_state_t;
```

State-Transitionen:

```
IDLE ──"alexa"/touch──▶ LISTENING ──match──▶ LOCAL_RESPONSE ──done──▶ IDLE
                            │
                            └──timeout/no-match──▶ PROCESSING ──tts-ready──▶ SPEAKING ──done──▶ IDLE
                                                       │                        │
                                                       └────"stop"──────────────┘ (lokal abbrechen)
```

### Queues und Events

| Queue                | Item             | Größe  | Producer        | Consumer         |
|----------------------|------------------|--------|-----------------|------------------|
| `recording_buffer`   | int16_t* (PSRAM) | static | `afe_fetch_task`| `network_task`   |
| `tts_queue`          | `audio_chunk_t`  | 32     | `network_task`  | `audio_out_task` |
| `state_event_queue`  | `state_event_t`  | 8      | `main_task`     | mehrere          |
| `local_command_queue`| `int command_id` | 4      | `afe_fetch_task`| `main_task`      |

---

## 9. Audio-Pipeline

### 9.1 Recording (im LISTENING-State)

- AFE liefert weiterhin Frames (auch im LISTENING-State).
- `afe_fetch_task` schreibt Frame-Daten parallel:
  1. In den AFE-internen MultiNet-Detector.
  2. In den globalen `recording_buffer` (PSRAM, 8s @ 16kHz @ 16bit
     = 256 KB).
- VAD vom AFE liefert `vad_state` — bei 1.5s Stille wird Capture beendet
  und an MultiNet ein "force timeout" gegeben.

### 9.2 WAV-Encoding für Whisper

Identisch zur v1-Spec: PCM-WAV mit 44-Byte-Header, 16 kHz mono 16-bit.
Header direkt vor den Samples in den Upload-Buffer schreiben.

### 9.3 HTTPS-Request: Groq Whisper

```
POST https://api.groq.com/openai/v1/audio/transcriptions
Headers:
  Authorization: Bearer <GROQ_API_KEY>
  Content-Type: multipart/form-data; boundary=----esp32boundary

Body (multipart/form-data):
  --esp32boundary
  Content-Disposition: form-data; name="model"

  whisper-large-v3-turbo
  --esp32boundary
  Content-Disposition: form-data; name="response_format"

  json
  --esp32boundary
  Content-Disposition: form-data; name="language"

  de
  --esp32boundary
  Content-Disposition: form-data; name="file"; filename="audio.wav"
  Content-Type: audio/wav

  <WAV binary data>
  --esp32boundary--

Response (200 OK):
  {"text": "Wie ist das Wetter heute in Würzburg?"}
```

**Wichtig**:
- `language=de` setzen, sonst rät Whisper. Spart Latenz und verbessert
  Genauigkeit.
- `whisper-large-v3-turbo` ist schneller als `whisper-large-v3` bei
  praktisch identischer Qualität — ~89% billiger im Paid-Tier und im
  Free-Tier gleicher Bucket.

### 9.4 HTTPS-Request: Gemini 2.5 Flash

```
POST https://generativelanguage.googleapis.com/v1beta/models/gemini-2.5-flash:generateContent
Headers:
  x-goog-api-key: <GEMINI_API_KEY>
  Content-Type: application/json

Body:
  {
    "systemInstruction": {
      "parts": [{
        "text": "Du bist ein kompakter Sprachassistent namens MiniLingo. Halte Antworten unter 3 Sätzen und ohne Markdown-Formatierung."
      }]
    },
    "contents": [
      {"role": "user",  "parts": [{"text": "Wie spät ist es?"}]},
      {"role": "model", "parts": [{"text": "Ich habe leider keine Uhrzeit-Information."}]},
      {"role": "user",  "parts": [{"text": "Wie ist das Wetter heute in Würzburg?"}]}
    ],
    "generationConfig": {
      "maxOutputTokens": 200,
      "temperature": 0.7
    }
  }

Response (200 OK):
  {
    "candidates": [{
      "content": {
        "parts": [{"text": "In Würzburg sind heute 18°C..."}],
        "role": "model"
      },
      "finishReason": "STOP"
    }],
    "usageMetadata": { ... }
  }
```

- **Konversations-History**: Lokal als Ringpuffer der letzten 10 Turns
  (5 User + 5 Model). Bei jedem Request komplett mitsenden.
- **System-Instruction**: Erzwingt knappe Antworten ohne Markdown
  (kritisch — Markdown-Sternchen klingen schrecklich im TTS).
- **maxOutputTokens=200**: Hard-Limit, hält Antworten unter ~10 Sekunden
  Audio.

### 9.5 HTTPS-Request: Gemini TTS

```
POST https://generativelanguage.googleapis.com/v1beta/models/gemini-2.5-flash-preview-tts:generateContent
Headers:
  x-goog-api-key: <GEMINI_API_KEY>
  Content-Type: application/json

Body:
  {
    "contents": [{
      "parts": [{"text": "In Würzburg sind heute 18°C und teilweise bewölkt."}]
    }],
    "generationConfig": {
      "responseModalities": ["AUDIO"],
      "speechConfig": {
        "voiceConfig": {
          "prebuiltVoiceConfig": {"voiceName": "Kore"}
        }
      }
    }
  }

Response (200 OK):
  {
    "candidates": [{
      "content": {
        "parts": [{
          "inlineData": {
            "mimeType": "audio/L16;codec=pcm;rate=24000",
            "data": "<base64-encoded raw PCM, 16-bit LE, 24kHz, mono>"
          }
        }]
      }
    }]
  }
```

**Verarbeitung im ESP32**:

1. Komplette JSON-Response in PSRAM puffern (Response kann mehrere MB
   für lange Antworten sein).
2. Mit cJSON `inlineData.data` extrahieren.
3. Base64-decoden (`mbedtls_base64_decode`) in einen zweiten PSRAM-Buffer.
4. PCM-Buffer in Chunks (z.B. 4 KB) in die `tts_queue` schieben.
5. `audio_out_task` schreibt aus Queue in I2S1.

**Voice-Optionen**: Kore (default), Puck, Charon, Fenrir, Aoede.

### 9.6 Wiedergabe (PCM → Speaker)

- I2S1 fest auf 24 kHz / 16-Bit / Mono.
- `audio_out_task` blockiert auf `tts_queue`, schreibt Chunks via
  `i2s_channel_write`.
- Während State `SPEAKING`: WakeNet deaktiviert, MultiNet aktiv
  (für "stop" Command).

---

## 10. Konfiguration & Secrets

### Kconfig (Development)

`main/Kconfig.projbuild`:

```
menu "MiniLingo Configuration"
    config WIFI_SSID
        string "WiFi SSID"
        default "yourwifi"

    config WIFI_PASSWORD
        string "WiFi Password"
        default ""

    config GROQ_API_KEY
        string "Groq API Key"
        default ""

    config GEMINI_API_KEY
        string "Gemini API Key"
        default ""

    config GEMINI_VOICE_NAME
        string "Gemini TTS Voice"
        default "Kore"
        help
            Available: Kore, Puck, Charon, Fenrir, Aoede

    config WAKE_WORD_THRESHOLD
        int "WakeNet detection threshold (0-100)"
        default 80
endmenu
```

`sdkconfig` in `.gitignore`. Für Production später NVS-Partition
(siehe v1-Spec, Section 7).

### TLS-Root-Zertifikate

`esp_crt_bundle` aktivieren:

```
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL=y
```

Beide APIs (Groq, Gemini) nutzen Standard-Public-CAs, die im Bundle
enthalten sind.

---

## 11. Projektstruktur

```
minilingo/
├── CMakeLists.txt
├── sdkconfig.defaults             # PSRAM, mbedTLS, esp-sr, partition
├── partitions.csv                 # nvs, factory, model, spiffs
├── main/
│   ├── CMakeLists.txt
│   ├── Kconfig.projbuild          # Secrets + Voice config
│   ├── main.c                     # app_main: init + start tasks
│   ├── state.c / state.h          # State-Machine, atomic state
│   ├── ui/
│   │   ├── ui.c / ui.h            # LVGL-Screen-Setup
│   │   ├── ui_chat.c              # Chat-Container, Bubble-Helper
│   │   ├── ui_mic_button.c        # Mic-Button mit Animationen
│   │   └── ui_status_bar.c        # Status-Bar (WiFi, State, Volume, Timer)
│   ├── drivers/
│   │   ├── display.c / .h         # ILI9341 init + LVGL flush_cb
│   │   ├── touch.c / .h           # XPT2046 + LVGL indev
│   │   ├── audio_in.c / .h        # INMP441 (I2S0), feed AFE
│   │   └── audio_out.c / .h       # MAX98357A (I2S1), Volume-Gain
│   ├── speech/
│   │   ├── afe_pipeline.c / .h    # AFE init, feed-task, fetch-task
│   │   ├── wake_word.c / .h       # WakeNet-Callbacks, Disable/Enable
│   │   └── commands.c / .h        # MultiNet Commands + Handler
│   ├── local/
│   │   ├── timer.c / .h           # Kitchen-Timer + Pre-recorded responses
│   │   ├── volume.c / .h          # Output-Gain-Management
│   │   └── canned_audio.c / .h    # WAV-File aus SPIFFS abspielen
│   ├── cloud/
│   │   ├── whisper.c / .h         # POST groq.com/.../transcriptions
│   │   ├── gemini.c / .h          # POST .../gemini-2.5-flash:generateContent
│   │   └── tts.c / .h             # POST .../gemini-2.5-flash-preview-tts
│   ├── audio/
│   │   ├── wav.c / .h             # WAV-Header
│   │   └── base64.c / .h          # mbedtls base64 wrapper
│   └── network/
│       └── wifi.c / .h            # WiFi-STA-Connect
├── spiffs/                        # Pre-generated TTS files
│   ├── tts_timer_set_*.wav
│   ├── tts_timer_done.wav
│   ├── tts_timer_cancelled.wav
│   ├── tts_volume_changed.wav
│   ├── tts_muted.wav
│   └── tts_unmuted.wav
├── scripts/
│   └── generate_canned_tts.py     # Python: Gemini-TTS-API → SPIFFS-WAVs
└── managed_components/
    ├── espressif__esp-sr/
    ├── espressif__esp_lcd_ili9341/
    └── lvgl__lvgl/
```

---

## 12. Implementierungsphasen

Empfohlene Reihenfolge — jede Phase soll lauffähig + testbar enden,
bevor die nächste begonnen wird.

### Phase 1: Projekt-Skelett + LVGL + UI-Mockup

- Neues ESP-IDF-Projekt anlegen.
- LVGL + `esp_lcd_ili9341` via `idf.py add-dependency` integrieren.
- `display.c`: ILI9341-Init aus `display_test.c`, Flush-Callback für
  LVGL.
- `touch.c`: XPT2046 als LVGL-Input-Device. Touch-Kalibrierwerte vorerst
  hardcoden (4 Eckpunkte manuell ausmessen).
- `ui.c`: Statisches Layout (Status-Bar, leerer Chat-Container,
  Mic-Button) + Dummy-Bubbles zum Testen.
- **Acceptance**: UI gerendert, Touch auf Mic-Button löst `printf` aus,
  Scrolling funktioniert.

### Phase 2: WiFi + esp-sr + Wake Word

- `wifi.c`: STA-Connect mit Kconfig-Credentials.
- esp-sr via `idf.py add-dependency espressif/esp-sr` integrieren.
- WakeNet9-Alexa-Model in menuconfig aktivieren + flashen.
- `audio_in.c`: I2S0-Init aus `mic_test.c`, in `afe_feed_task` einbauen.
- `afe_pipeline.c`: AFE create + feed + fetch.
- `wake_word.c`: Callback bei `WAKENET_DETECTED` → State-Change +
  UI-Update (Button rot).
- **Acceptance**: "Alexa" sagen → Button wird rot, Log zeigt
  "Wake word detected".

### Phase 3: Audio-Capture + Recording-Buffer

- Im `afe_fetch_task`: nach Wake-Word Frames in PSRAM-Recording-Buffer
  schreiben.
- VAD aus AFE-Result auslesen, Capture nach 1.5s Stille beenden (oder
  nach 8s Hard-Limit).
- `wav.c`: WAV-Header generieren, fertigen WAV-Buffer im PSRAM ablegen.
- Test: WAV-Buffer via UART/SPIFFS rausziehen und in Audacity öffnen.
- **Acceptance**: 5s Sprache nach "Alexa" erzeugt valide WAV-Datei.

### Phase 4: MultiNet + Lokale Commands

- MultiNet7-EN-Model in menuconfig aktivieren.
- `commands.c`: 14 Commands registrieren (Section 7).
- Im `afe_fetch_task`: parallel zum Recording MultiNet-Detect aufrufen.
- Bei Match: `on_local_command(command_id)` aufrufen, Recording verwerfen.
- `timer.c`: FreeRTOS-Software-Timer mit Callback.
- `volume.c`: Output-Gain via Tabelle (0%, 20%, 40%, 60%, 80%, 100%) —
  softwareseitig durch Sample-Multiplikation in `audio_out_task`.
- `scripts/generate_canned_tts.py`: Lokal auf dem Dev-Rechner laufen
  lassen, generiert WAVs in `spiffs/`.
- SPIFFS-Partition in `partitions.csv`, beim Build flashen.
- `canned_audio.c`: WAV aus SPIFFS in I2S1 streamen.
- **Acceptance**: "Alexa" → "set timer five minutes" → Timer läuft in
  der Status-Bar runter, am Ende kommt der Alarm.

### Phase 5: Groq Whisper Integration

- `whisper.c`: Multipart-Upload mit `esp_http_client_open` +
  `_write` (Header-Parts + WAV-Daten + Footer).
- `esp_crt_bundle` aktivieren.
- JSON-Response mit cJSON parsen.
- Bei MultiNet-Timeout: Recording-Buffer an Whisper schicken.
- Transkript als User-Bubble in der UI anzeigen.
- **Acceptance**: "Alexa" → freie Frage → Transkript erscheint als
  User-Bubble.

### Phase 6: Gemini Flash Integration

- `gemini.c`: JSON-Request mit cJSON bauen (system-instruction,
  contents, generationConfig), Response parsen.
- Konversations-History als Ringpuffer (10 Turns) in `state.c`.
- Antwort-Text als Assistant-Bubble in der UI anzeigen.
- **Acceptance**: Komplette Konversation funktioniert ohne Audio-
  Ausgabe (nur Text).

### Phase 7: Gemini TTS + Wiedergabe

- `tts.c`: JSON-Request mit `responseModalities=AUDIO`.
- Response in PSRAM puffern, `inlineData.data` extrahieren.
- Base64-Decode → PCM-Buffer.
- PCM in Chunks in `tts_queue`.
- `audio_out_task`: Chunks aus Queue in I2S1.
- WakeNet während `SPEAKING`-State deaktivieren.
- "stop" Command bricht TTS ab (Queue flush + I2S clear).
- **Acceptance**: Komplette End-to-End-Erfahrung: Wake Word →
  Frage → gesprochene Antwort. "stop" während Wiedergabe funktioniert.

### Phase 8: Polish + Robustness

- Mic-Button-Animationen (Puls bei `LISTENING`, Spinner bei
  `PROCESSING`).
- Retry-Logik mit Exponential Backoff bei HTTP 429.
- Error-Recovery: bei jedem Fehler zurück zu `IDLE` mit kurzem
  Audio-Cue.
- Touch-Kalibrierungs-Screen beim ersten Boot.
- WiFi-Reconnect-Logik.
- **Acceptance**: 20 aufeinanderfolgende Interaktionen ohne Reset,
  WiFi-Drop wird abgefangen.

### Phase 9 (optional): AEC

- AFE mit `aec_init=true`.
- Reference-Signal vom I2S1-TX-Stream parallel ins AFE feeden
  (`afe_handle->feed` mit 2-Channel-Buffer: Mic + Ref).
- Vorteil: Wake-Word funktioniert *auch während* TTS-Wiedergabe.

### Phase 10 (optional): Function-Calling für freie Timer-Werte

- System-Instruction um Function-Schema erweitern (Gemini Function
  Calling).
- Bei `set_timer(seconds)` Tool-Call: lokalen Timer-Handler aufrufen
  statt Audio-Antwort.

---

## 13. Stolperfallen

### LVGL ist nicht thread-safe

UI-Calls aus anderen Tasks müssen über `lv_async_call()` laufen.
Direkt aus `network_task` ein Label setzen → Race-Conditions.

### esp-sr Memory-Hunger

WakeNet + MultiNet zusammen brauchen ~300 KB im PSRAM. Plus
Recording-Buffer (256 KB) und TTS-Response-Buffer (bis 2 MB). Ohne
PSRAM nicht machbar. Im `sdkconfig.defaults`:

```
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_USE_MALLOC=y
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384
CONFIG_ESP32S3_SPIRAM_SUPPORT=y
```

### TLS frisst RAM

mbedTLS-Handshake braucht ~30-50 KB pro Verbindung. **Nie parallel** —
Whisper fertig + close → Gemini open + close → TTS open. Sonst
Heap-Exhaustion.

### Self-Trigger durch eigenen Speaker

In Phase 1-8: WakeNet deaktivieren während `SPEAKING`. Konsequenz: Der
User kann die Antwort *nicht* mit "Alexa" unterbrechen. Aber MultiNet
ist aktiv → "stop" funktioniert. Phase 9 löst das vollständig per AEC.

### Gemini TTS Response-Größe

10 Sekunden PCM bei 24 kHz / 16-bit = 480 KB raw. Base64-encoded
~640 KB. Plus JSON-Overhead = ~700 KB Response. Bei `maxOutputTokens=200`
liegt man typischerweise unter 1 MB. Immer in PSRAM puffern.

### Gemini Preview-Models können sich ändern

`gemini-2.5-flash-preview-tts` ist Preview. Modellname als Konstante
führen, bei Bedarf in einer Stelle ändern.

### Groq Free-Tier-Limits

2.000 Whisper-Requests/Tag, 30 RPM bei LLM. Bei intensivem Testen
ggf. Mock-Server lokal aufsetzen.

### MultiNet Custom Commands kollidieren mit Wake Word

MultiNet erkennt nur *nach* Wake Word. Wenn ein Command zufällig
"Alexa" enthält → Doppel-Trigger möglich. Im Command-Set vermeiden.

### Audio Sample-Format

- INMP441 liefert 32-bit Frames mit 24-bit gültigen Daten in 32-bit-Slot.
- AFE erwartet `int16_t` Samples.
- Konvertierung: `int16_t s16 = (int16_t)(s32 >> 16);` (nimmt die oberen
  16 Bits — siehe `mic_test.c` für 24-Bit-Variante, hier reicht 16-Bit).

### SPI-Bus-Arbitration Display vs. Touch

Beide auf SPI2_HOST, eigene `spi_device_handle_t`. Display mit DMA,
Touch ohne. ESP-IDF serialisiert automatisch — aber **niemals** beide
Devices gleichzeitig aus verschiedenen Tasks ansprechen ohne Mutex.

### WiFi und I2S auf demselben Core

WiFi + LVGL auf Core 0, AFE + I2S auf Core 1. Falls beide auf Core 0:
Audio-Dropouts beim ersten WiFi-Reconnect.

---

## 14. Anhang: sdkconfig.defaults (Auszug)

```
# PSRAM
CONFIG_ESP32S3_SPIRAM_SUPPORT=y
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_USE_MALLOC=y
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384

# Flash
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"

# mbedTLS Cert Bundle
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL=y

# HTTP Client
CONFIG_ESP_HTTP_CLIENT_ENABLE_HTTPS=y
CONFIG_ESP_TLS_USE_DS_PERIPHERAL=n

# FreeRTOS
CONFIG_FREERTOS_HZ=1000
CONFIG_FREERTOS_UNICORE=n

# LVGL
CONFIG_LV_COLOR_DEPTH_16=y
CONFIG_LV_COLOR_16_SWAP=y
CONFIG_LV_MEM_CUSTOM=y
CONFIG_LV_MEM_SIZE_KILOBYTES=96

# esp-sr
CONFIG_USE_WAKENET=y
CONFIG_SR_WN_WN9_ALEXA=y
CONFIG_USE_MULTINET=y
CONFIG_SR_MN_EN_MULTINET7_QUANT=y
CONFIG_USE_AFE=y
CONFIG_AFE_INTERFACE_V1=y
```

## Anhang B: partitions.csv

```
# Name,     Type, SubType,  Offset,   Size,     Flags
nvs,        data, nvs,      0x9000,   0x6000,
phy_init,   data, phy,      0xf000,   0x1000,
factory,    app,  factory,  0x10000,  0x300000,
model,      data, spiffs,            0x500000,
storage,    data, spiffs,            0x100000,
```

Partition `model` ist für esp-sr-Modelle reserviert (WakeNet +
MultiNet zusammen ~500 KB, mit Reserve auf 5 MB). Partition `storage`
für die Canned-TTS-WAVs.

---

*Spec-Version 2.0 — finale Ausgangsbasis für die Implementierung mit
Free-Tier-APIs, Wake Word "Alexa" und lokalen Commands ab Phase 1.*
