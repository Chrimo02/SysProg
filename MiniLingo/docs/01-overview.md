# 1. Overview

## What MiniLingo is

MiniLingo is a desktop voice assistant built on a single **ESP32-S3** board
with a microphone, a small speaker, and a touch LCD. It listens for a wake
word, records what you say, and responds. Three "characters" are selectable —
**Mycroft**, **Alexa**, and **Jarvis** — each with its own wake word, its own
personality (system prompt), and its own text-to-speech voice.

It is a **hybrid** assistant:

- **Local (offline, instant):** wake-word detection and a fixed set of command
  words — kitchen timer, volume control, mute, stop, temperature. These never
  touch the network for recognition; the display response is instant and offline.
  If TTS is enabled, the temperature value is additionally spoken via the local
  Piper server (requires Wi-Fi to reach the LAN server).
- **Cloud (online):** anything that isn't a known command is treated as a
  free-form question. The recording is transcribed (Groq Whisper), answered by
  a language model (Cerebras), and — optionally — read back aloud
  (Gemini TTS).

The split exists for good reasons (latency, cost/quota, privacy of the common
case). Those reasons are spelled out in
[06-design-decisions.md](06-design-decisions.md).

## What it can do

- **Wake on "Mycroft" / "Alexa" / "Jarvis"** (one active at a time) or on a
  **touch** of the on-screen mic button.
- **Local commands** (recognized on-device, English):
  - Kitchen timer: set 30 s / 1 / 5 / 10 / 15 / 30 min, cancel, ask time left.
  - Volume: up, down, set to 20/40/60/80/100 %, mute, unmute.
  - "stop": stops playback (and cancels a running timer when not speaking).
  - Temperature: "temperature" / "what is the temperature" / "whats the current
    temperature" / "how warm is it" — reads DS18B20 on GPIO 47.
- **Free-form questions** → transcribed, answered by the LLM, shown as a chat
  bubble, and optionally spoken.
- **Tool calling**: the LLM can set arbitrary timer/volume values that the
  fixed local commands can't express (e.g. "set a timer for 7 minutes",
  "volume to 35 percent").
- **On-screen chat UI**: user/assistant/local-response bubbles, a status bar
  (Wi-Fi, state, volume), a mic button with listening/processing/speaking
  states, a TTS on/off toggle, and a touch volume slider.
- **Runtime setup portal**: change Wi-Fi and API keys from a phone, no
  re-flash needed.

## Hardware

ESP32-S3 **N16R8** (16 MB flash, 8 MB octal PSRAM), plus four peripherals.
Full wiring and the test programs that validated each part are in
[04-subsystems/drivers.md](04-subsystems/drivers.md); here is the pin map at a
glance:

| Peripheral | Part | Bus | Pins (ESP32-S3) |
|---|---|---|---|
| Microphone | INMP441 | I2S0 (RX) | WS=4, SCK=5, SD=6 (L/R→GND) |
| Amplifier | MAX98357A | I2S1 (TX) | BCLK=15, LRC=16, DIN=17 (SD→3V3) |
| Display | ILI9341 (240×320) | SPI2 | SCK=12, MOSI=11, MISO=13, CS=10, DC=8, RST=9 |
| Touch | XPT2046 | SPI2 (shared) | CS=14 (SCK/MOSI/MISO shared with display) |

The mic, amp, and display/touch deliberately use separate GPIO groups so they
can run simultaneously; display and touch share one SPI bus with separate chip
selects. (A wiring short between amp BCLK and mic SD once caused a hard-to-find
failure — see [07-lessons-learned.md](07-lessons-learned.md).)

## Software stack

| Layer | Choice | Notes |
|---|---|---|
| SDK / build | **ESP-IDF** (target esp32s3), CMake + `idf.py` | |
| RTOS | **FreeRTOS** (1000 Hz tick) | dual-core, tasks pinned per core |
| GUI | **LVGL v9** | managed component `lvgl/lvgl ^9` |
| On-device speech | **esp-sr** (WakeNet9 + MultiNet7) | models in a flash partition |
| Display driver | `espressif/esp_lcd_ili9341` | managed component |
| STT (cloud) | **Groq Whisper** `whisper-large-v3-turbo` | multipart upload |
| LLM (cloud) | **Cerebras** `gpt-oss-120b` | OpenAI-compatible chat completions |
| TTS (primary) | **local Piper server** (HTTP, LAN) | raw WAV, off by default |
| TTS (fallback) | **Gemini** `gemini-2.5-flash-preview-tts` | only if local fails/times out |

> **Note on the LLM:** the original spec used Gemini for text answers; the
> project switched to Cerebras for a far larger free-tier quota. Gemini remains
> only for (optional) TTS — and only as the **fallback** behind a local Piper
> server. If you read the archived spec, keep this in mind. Details:
> [06-design-decisions.md](06-design-decisions.md).

## Where the code lives

Everything is one ESP-IDF app under [main/](../main/):

```
main/
  main.c          app_main + the glue: state flags, event pump, dispatch
  config.c/.h     runtime config cache (NVS), getters/setters
  state.c/.h      conversation history ring buffer
  drivers/        display, touch, audio_in (mic), audio_out (amp)
  speech/         afe_pipeline, wake_word, recorder, commands, wav
  network/        wifi, provisioning, cloud, whisper, llm, tts, http_util
  local/          timer, volume, canned_audio, temperature
  ui/             ui, ui_chat, ui_status_bar, ui_mic_button, ui_tts_button, font
  debug/          wav_dump (Phase-3 UART audio dump, off by default)
  *_test.c        Phase-0 standalone hardware tests (mic/amp/display/touch)
```

Each subsystem is documented under [04-subsystems/](04-subsystems/). The next
two chapters — [architecture](02-architecture.md) and
[request lifecycle](03-request-lifecycle.md) — explain how these directories
cooperate at runtime.
