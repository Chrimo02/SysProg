# Subsystem: Config & Provisioning

How MiniLingo gets its settings — Wi-Fi credentials, API keys, model/voice
choices, the active character — and how a user changes them without a PC. Files:
[config.c](../../main/config.c)/[.h](../../main/config.h),
[provisioning.c](../../main/network/provisioning.c),
[wifi.c](../../main/network/wifi.c).

## Two layers: Kconfig defaults + NVS overrides

Settings come from **NVS** (the `minilingo` namespace) with **Kconfig** as the
fallback. `config_init()` (called early in `app_main`) loads each value into a
RAM cache: if the NVS key exists it wins, otherwise the compiled-in
`CONFIG_*` default is used. So a freshly flashed board works from the Kconfig
defaults, and provisioning later overrides them — no re-flash, no code change.

```mermaid
flowchart LR
    K[Kconfig CONFIG_* defaults] -->|fallback| C[config.c RAM cache]
    NVS[(NVS minilingo namespace)] -->|if key present| C
    Portal[Setup portal POST /save] -->|config_set_str| NVS
    C --> Consumers[wifi / whisper / llm / tts]
```

### What's stored

String entries (`s_entries` table in [config.c](../../main/config.c)): Wi-Fi
SSID/password, Groq key, Cerebras key, Gemini key, LLM model, Whisper language,
Gemini voice. Plus an integer **character index** (`config_set_character` /
`config_character_index`, used by the wake-word loader and persona/voice setup).

Getters (`config_wifi_ssid()`, `config_cerebras_key()`, …) replace the old
`CONFIG_*` macros throughout `wifi.c`/`whisper.c`/`llm.c`/`tts.c`; auth headers
are built at runtime (`snprintf("Bearer %s", …)`).

> **Security note:** keys are stored in NVS **in plain text** (as they were in
> the sdkconfig before). Acceptable for this hobby setup; don't ship it as-is.

The Kconfig options themselves live in
[Kconfig.projbuild](../../main/Kconfig.projbuild) and are summarized in
[05-build-and-setup.md](../05-build-and-setup.md).

## Wi-Fi — wifi.c

[wifi.c](../../main/network/wifi.c). Station mode, credentials from the config
cache. A disconnect schedules a reconnect after 5 s via a one-shot `esp_timer`
(don't block the event-loop task). `wifi_is_connected()` (atomic) gates the
cloud pipeline. Connection status is reported via callback → `s_wifi_event` →
the status-bar Wi-Fi icon.

> The ESP32-S3 is **2.4 GHz only** — a 5 GHz SSID fails to associate
> (disconnect reason 201).

## Setup portal — provisioning.c

[provisioning.c](../../main/network/provisioning.c). Tap the Wi-Fi icon (or boot
unprovisioned) → `provisioning_start()`:

1. Brings up a **SoftAP** `MiniLingo-Setup` (WPA2, password `minilingo`) and an
   `esp_http_server`.
2. `GET /` serves a form pre-filled with the non-secret values (SSID, model,
   language, voice); secret fields (password, keys) are blank placeholders.
3. `POST /save` URL-decodes the body and writes each field via
   `config_set_str()` (which updates both NVS and the cache). **Empty secret
   fields keep the old value.** Then it reboots.

> **AP-only during setup.** The ESP has one radio. In APSTA mode the AP must sit
> on the station's channel and kept kicking the setup client (join/leave reason
> 1). So the portal switches to **pure AP mode** for setup; after saving, the
> device reboots and the station connection returns. (This was a real bug — see
> [07-lessons-learned.md](../07-lessons-learned.md).)

The portal is at `http://192.168.4.1`. No TLS on the local portal.

## Character switching

Characters switch **at runtime, without a reboot** (Phase B2). Two triggers, both
funnel into `apply_character(idx)` in [main.c](../../main/main.c):
- **Tap the state text** → `on_character_clicked` cycles to the next character
  (LVGL task).
- **Voice** → "switch to Jarvis" / "talk to Alexa" etc. are local MultiNet
  commands (`CMD_CHARACTER_*`); the match (afe_fetch_task) posts the target index
  via an atomic, and the LVGL task applies it.

`apply_character` sets, all without reboot:
- the **persona** system prompt (`llm_set_system_prompt`,
  [llm.c](../../main/network/llm.c)) — pointer swap;
- the **TTS voices** (`tts_set_voice` + `tts_set_local_voice`,
  [tts.c](../../main/network/tts.c));
- persists the index to NVS (`config_set_character`);
- the **wake word**, by requesting a runtime WakeNet swap
  (`afe_pipeline_request_wakeword`) — the AFE destroys and recreates its instance
  at the fetch-loop top (~100 ms), so only **one** WakeNet is ever in RAM at once
  (three at once blow internal RAM). See [speech.md](speech.md) and
  [06-design-decisions.md](../06-design-decisions.md).

At boot, `config_character_index()` restores the last character and `app_main`
sets the same persona/voices. The `character_t` table (name, idle text, persona,
Gemini + Piper voice) lives in [main.c](../../main/main.c).

## Design notes & gotchas

- **"Unprovisioned" = SSID is still the placeholder `"yourwifi"`.** That's the
  trigger for the auto-portal at boot; real Kconfig creds or NVS values count as
  provisioned.
- Changing a Wi-Fi/model setting via the portal takes effect after the reboot it
  performs — there's no live re-read. The **character switch is the exception**:
  persona/voice/wake-word all update live (see above), no reboot.
