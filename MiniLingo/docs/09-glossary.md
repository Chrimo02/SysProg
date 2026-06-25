# Glossary

Terms and acronyms used throughout this documentation, grouped roughly by
topic. If a term shows up in another doc with a `→`, it points here.

## Speech / audio

- **Wake word** — a short trigger phrase ("Mycroft"/"Alexa"/"Jarvis") that the
  device listens for continuously and locally. Detecting it starts a listening
  session.
- **WakeNet** — Espressif's neural wake-word detector (we use **WakeNet9**).
  Runs on-device, recognizes exactly one trained phrase at a time.
- **MultiNet** — Espressif's on-device **command-word** recognizer (we use
  **MultiNet7**, English/quantized). Recognizes a small fixed set of phrases
  (our 14 local commands) without the cloud.
- **AFE** — Audio Front End. Espressif's audio pre-processing pipeline
  (`esp-sr`) that takes raw mic samples and produces cleaned frames plus
  wake-word and voice-activity results. Feeds WakeNet and MultiNet.
- **esp-sr** — Espressif Speech Recognition component; bundles AFE, WakeNet,
  and MultiNet.
- **VAD** — Voice Activity Detection. Tells whether a frame contains speech;
  the recorder uses it to know when the user stopped talking.
- **G2P** — Grapheme-to-Phoneme. Converts written words to the phoneme tokens
  MultiNet matches against. We sometimes hand-write phonemes because the
  on-device G2P or the model's FST mishandles certain words.
- **FST** — Finite-State Transducer; the internal graph MultiNet uses to match
  phoneme sequences. A couple of tokens in MultiNet7's FST are buggy (see
  [07-lessons-learned.md](07-lessons-learned.md)).
- **STT / TTS** — Speech-to-Text / Text-to-Speech.
- **PCM** — Pulse-Code Modulation; raw uncompressed audio samples (here:
  16-bit signed, mono).
- **WAV / RIFF** — the container format we wrap PCM in for upload; RIFF is the
  chunked file structure WAV uses.

## Hardware

- **ESP32-S3** — the dual-core microcontroller this runs on (model N16R8:
  16 MB flash, 8 MB PSRAM).
- **PSRAM** — external pseudo-static RAM (8 MB). Large buffers live here; the
  internal SRAM (~512 KB) is precious and reserved for things that need it
  (TLS, task stacks, esp-sr).
- **I2S** — Inter-IC Sound; the digital audio bus. We use **I2S0** for the
  microphone (RX) and **I2S1** for the amplifier (TX).
- **DMA** — Direct Memory Access; moves audio between I2S peripherals and
  memory without CPU involvement.
- **SPI** — Serial Peripheral Interface; the bus shared by the display and the
  touch controller (**SPI2**).
- **INMP441** — the MEMS I2S microphone.
- **MAX98357A** — the I2S Class-D amplifier driving the speaker(s).
- **ILI9341** — the 240×320 SPI LCD controller.
- **XPT2046** — the resistive touch controller on the same SPI bus.

## Firmware / RTOS

- **ESP-IDF** — Espressif's official C SDK and build system (CMake +
  `idf.py`).
- **FreeRTOS** — the real-time OS underneath ESP-IDF; everything runs as
  tasks pinned to one of the two cores.
- **NVS** — Non-Volatile Storage; a key/value store in flash. Holds Wi-Fi
  credentials, API keys, the active character, etc.
- **Kconfig** — ESP-IDF's compile-time configuration system (`menuconfig`).
  Provides defaults that NVS values override at runtime.
- **SPIFFS** — a simple flash filesystem. Used for the model partition and the
  canned-audio `storage` partition.
- **LVGL** — Light and Versatile Graphics Library (v9); draws the UI.
- **Q15** — a 16-bit fixed-point format (1 sign + 15 fraction bits). Used for
  the software volume gain: `out = (sample * gain_q15) >> 15`.

## Cloud / LLM

- **LLM** — Large Language Model; produces the assistant's text answers. We use
  **Cerebras** (OpenAI-compatible chat completions, model `gpt-oss-120b`).
- **Whisper** — OpenAI's speech-to-text model; we call it via **Groq**
  (`whisper-large-v3-turbo`).
- **TTS (text-to-speech)** — turns the answer text into spoken audio. Primary
  source is a **local Piper server**; **Gemini TTS** is the fallback.
- **Piper** — a fast, local neural TTS engine; here it runs on a Raspberry Pi on
  the LAN and answers `POST /tts` with raw WAV bytes (voices `jarvis`/`mycroft`/
  `alexa`/`de`/`en`). The primary TTS source.
- **Gemini TTS** — Google's text-to-speech (model
  `gemini-2.5-flash-preview-tts`); the **fallback** used only when the local
  Piper server is unreachable or times out.
- **Tool calling / function calling** — the LLM, instead of replying with
  text, asks the firmware to run a named function (e.g. `set_timer`) with
  arguments. See [network.md](04-subsystems/network.md) and
  [06-design-decisions.md](06-design-decisions.md).
- **TLS** — Transport Layer Security; the encryption on every cloud HTTPS
  request. Expensive in RAM, which is why requests run strictly one at a time.
- **mbedTLS** — the TLS library ESP-IDF uses; we configure it to allocate from
  PSRAM.
