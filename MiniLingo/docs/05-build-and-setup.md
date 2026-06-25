# 5. Build & Setup

How to build, flash, configure, and run MiniLingo from a clean checkout.

## Prerequisites

- **ESP-IDF v5.x** installed (the project targets `esp32s3` and was built with
  IDF 5.5). Managed components (LVGL v9, `esp_lcd_ili9341`, `esp-sr`) are pulled
  automatically from [idf_component.yml] on first build.
- The board: **ESP32-S3 N16R8** (16 MB flash, 8 MB octal PSRAM) wired per the
  [pin map](04-subsystems/drivers.md).
- API keys (free tiers): **Cerebras** (LLM, required), **Groq** (Whisper,
  required), **Gemini** (TTS **fallback** only, optional).
- For spoken replies, a reachable **local Piper TTS server** on the LAN
  (`TTS_LOCAL_URL`, default `http://192.168.3.143:5050/tts`) — the primary TTS
  source. Without it, TTS still works via the Gemini fallback (if a key is set).

## Activate the toolchain

In this environment the IDF environment is activated with the `get_idf` alias
(it sources ESP-IDF's `export.sh`); afterwards `idf.py` is on the PATH:

```bash
get_idf
```

## Configure (optional)

Defaults come from [Kconfig.projbuild](../main/Kconfig.projbuild) and can be set
with `idf.py menuconfig` (menu **"MiniLingo Configuration"**). You can skip this
entirely and provision at runtime instead (see below).

| Option | Default | Purpose |
|---|---|---|
| `WIFI_SSID` / `WIFI_PASSWORD` | `yourwifi` / empty | Wi-Fi STA creds (placeholder triggers auto-portal) |
| `CEREBRAS_API_KEY` | empty | LLM (required) |
| `LLM_MODEL` | `gpt-oss-120b` | Cerebras model (free tier: also `zai-glm-4.7`) |
| `GROQ_API_KEY` | empty | Whisper STT (required) |
| `WHISPER_LANGUAGE` | `en` | language hint sent to Whisper |
| `GEMINI_API_KEY` | empty | Gemini TTS fallback (optional) |
| `GEMINI_VOICE_NAME` | `Kore` | default Gemini TTS voice (per-character voices override) |
| `TTS_SPEAK_REPLIES` | `y` | initial state of the TTS toggle |
| `WAKE_WORD_THRESHOLD` | `0` | 0 = model default; higher = stricter |
| `RECORDING_WAV_DUMP` | `n` | Phase-3 debug: dump captures as Base64 over UART |

The local Piper server endpoint and default voice are compile-time `#define`s at
the top of [tts.c](../main/network/tts.c) (`TTS_LOCAL_URL`, `TTS_LOCAL_VOICE`,
`TTS_LOCAL_TIMEOUT_MS`), not Kconfig — edit them there if your server differs.

Core build settings live in [sdkconfig.defaults](../sdkconfig.defaults): PSRAM
(octal, 8 MB), 16 MB flash with the custom [partitions.csv](../partitions.csv),
240 MHz CPU (esp-sr needs it), 1000 Hz FreeRTOS tick, mbedTLS-from-PSRAM, the
three WakeNets + MultiNet7, and LVGL v9. Don't lower these without reading
[07-lessons-learned.md](07-lessons-learned.md).

## Build & flash

```bash
get_idf
idf.py build
idf.py -p <PORT> flash monitor
```

`idf.py flash` writes everything: bootloader, partition table, app, the
**`model`** SPIFFS image (esp-sr models, built by the component), and the
**`storage`** SPIFFS image (canned audio, built from [spiffs/](../spiffs/) via
`spiffs_create_partition_image`).

> **Stale managed-component build dir:** a leftover
> `managed_components/espressif__esp-sr/model/build` folder can cause a CMake
> `register_component` error. Delete it and rebuild.

> **Flashing a boot-looping native-USB board:** if the device is in a reboot
> loop, `esptool` can't get in (the USB port re-enumerates constantly). Put the
> chip into download mode by hand — hold **BOOT**, tap **RESET**, release BOOT —
> then flash.

## First-run provisioning (no PC)

You don't need to bake credentials into the firmware. On a board whose SSID is
still the placeholder `yourwifi`, the **setup portal opens automatically**; you
can also open it any time by tapping the Wi-Fi icon. Then:

1. On a phone, join Wi-Fi **`MiniLingo-Setup`** (password `minilingo`).
2. Browse to **`http://192.168.4.1`**.
3. Enter Wi-Fi SSID/password and the API keys; submit. Empty secret fields keep
   their previous value.
4. The device saves to NVS and reboots onto your network.

Details: [04-subsystems/config.md](04-subsystems/config.md).

## Regenerating the canned audio

The 15 prompt WAVs in [spiffs/](../spiffs/) are produced by
[scripts/generate_canned_tts.py](../scripts/generate_canned_tts.py):

```bash
# from the project root
python3 scripts/generate_canned_tts.py            # auto: Gemini if key set, else macOS `say`
python3 scripts/generate_canned_tts.py --engine gemini --voice Charon
```

- `gemini` needs `GEMINI_API_KEY` (env or `--api-key`); output is 24 kHz PCM.
  Mind the free-tier limit (~10 TTS requests/day) — use `--skip-existing` to
  resume across days.
- `say` is the offline macOS fallback (AIFF → 16 kHz/16-bit/mono WAV via
  `afconvert`).
- `tts_alarm.wav`/`tts_error.wav` are always synthesized as beep patterns (no
  TTS).
- The **filenames are the contract** with the firmware
  ([canned_audio.h](../main/local/canned_audio.h)); change both sides together.

After regenerating, re-flash so the `storage` image is rebuilt.

## Sanity check on boot

A healthy `idf.py monitor` log shows, in order: config loaded
(`config: loaded (ssid=… character=N)`), SPIFFS mounted with 0 canned files
missing, MultiNet ready (`mn7_en ready: … phrases`), the chosen WakeNet
(`wakenet model: … (character N)`), and `AFE pipeline up, listening for wake
word`. Then say the active wake word and watch the state transitions.

[idf_component.yml]: ../main/idf_component.yml
