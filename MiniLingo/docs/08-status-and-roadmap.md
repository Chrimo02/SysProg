# 8. Status & Roadmap

Where the project stands and what's left. This replaces the status table that
used to live at the top of `PROGRESS.md`; the full session history is archived
in [archive/PROGRESS-sessions-de.md](archive/PROGRESS-sessions-de.md).

## Phase status

| Phase | Description | Status |
|---|---|---|
| 0 | Hardware component tests (mic/amp/display/touch) | ✅ done |
| 1 | Project skeleton + LVGL + UI mockup | ✅ done |
| 2 | Wi-Fi + esp-sr + wake word | ✅ done |
| 3 | Audio capture + recording buffer | ✅ done |
| 4 | MultiNet + local commands | ✅ done |
| 5 | Groq Whisper (STT) | ✅ done |
| 6 | LLM text answers + history | ✅ done |
| 7 | Gemini TTS + playback | ✅ done (off by default) |
| 8 | Polish + robustness (fonts, retries, silence guard, animations) | ✅ done |
| 8b | Touch volume slider + spoken volume levels | ✅ done |
| 8c | LLM switch Gemini → Cerebras | ✅ done |
| 9 | AEC (acoustic echo cancellation) | ⬜ **open, optional** |
| 10 | Function/tool calling for free timer/volume values | ✅ done |
| A | Wi-Fi setup portal (runtime provisioning) | ✅ done |
| B | Character / wake-word switcher (Mycroft/Alexa/Jarvis) | ✅ done |
| — | Per-character personas + per-character TTS voices | ✅ done |

The assistant is **feature-complete** for its intended scope. The only
unfinished phase is AEC, which is optional.

## Verified on hardware vs. pending bench → device

Most things have been confirmed on the device. A few changes are built and
flashed but still want a real on-device pass:

- **Per-character personas + voices** (latest change) — builds; confirm on
  hardware that the three characters sound and behave distinctly.
- **Tool calling (Phase 10)** — build green; run the device test: set/cancel a
  timer with an odd value, ask time left, set an odd volume, and a general
  question (must stay a text answer, not a tool call).
- **Wi-Fi setup portal (Phase A)** — flashed; do a full phone → portal → save →
  reboot pass.
- **TTS toggle** — confirm the button toggles spoken replies.
- **Wi-Fi drop test** — drop the router mid-question: expect "Cloud: No WiFi
  connection" + beep, local commands still work, auto-reconnect within ~5 s.

## Open items / ideas

- **TTS latency & long-answer failures.** With TTS on, there's download-then-play
  latency, and answers longer than ~21 s of audio (`PCM_MAX`) fail with "Speech
  failed". The clean fix is a **PSRAM jitter buffer** (ring buffer + playback
  task, I2S-DMA left at default). Device-side streaming was tried and reverted
  (choppy due to DMA underrun); **do not** fix it by enlarging the I2S-DMA
  buffer — that starves the TLS AES (see
  [07-lessons-learned.md](07-lessons-learned.md)). Not pursued, by choice.
- **Persona breadth.** `gpt-oss-120b` once refused a general question ("tell a
  joke" → "I only control timer/volume"). Adding "answer general questions
  normally; use tools only for timer/volume" to `TOOL_INSTRUCTIONS` in
  [llm.c](../main/network/llm.c) would loosen it.
- **Male canned voices.** The pre-recorded prompts could be regenerated with a
  male "British gentleman" voice; now partly overlapping with the per-character
  voice work. Regen via
  [scripts/generate_canned_tts.py](../scripts/generate_canned_tts.py) (mind the
  ~10 TTS req/day limit; `--skip-existing` to resume). 4 files are still in the
  macOS `say` voice.
- **"thirty minutes" local command** doesn't match (collides with "thirty
  seconds") — low priority; tool calling covers arbitrary timer values.
- **Display flicker during playback** — suspected power-rail dip (hardware).

## How to extend

- **Add a character:** add a row to `s_characters` in [main.c](../main/main.c)
  (name, idle text, persona, voice), make sure its WakeNet is selected in
  `sdkconfig.defaults` and mapped in
  [afe_pipeline.c](../main/speech/afe_pipeline.c).
- **Add a local command:** add a row to `COMMANDS` in
  [commands.c](../main/speech/commands.c) (with explicit phonemes if the
  on-device G2P struggles — see [04-subsystems/speech.md](04-subsystems/speech.md)),
  handle the id in `on_local_command` ([main.c](../main/main.c)), and add a
  canned WAV if you want spoken confirmation.
- **Add a cloud tool:** add a schema in `add_tools`
  ([llm.c](../main/network/llm.c)) and a branch in `run_tool`
  ([cloud.c](../main/network/cloud.c)).
