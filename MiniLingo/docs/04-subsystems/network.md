# Subsystem: Network (cloud + Wi-Fi)

The cloud pipeline and connectivity. Files: [network/](../../main/network/).
Wi-Fi setup/provisioning is covered in [config.md](config.md); this doc is the
cloud request path.

## The golden rule: one TLS connection at a time

A TLS handshake costs ~50 KB and mbedTLS allocates from PSRAM
(`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`). Two concurrent connections would exhaust
the heap. Therefore **all cloud requests run sequentially in one task** —
`network_task` ([cloud.c](../../main/network/cloud.c)) — and each request fully
closes before the next opens. This constraint shaped the whole pipeline.

## cloud.c — the orchestrator

`network_task` (Core 0, prio 3, 16 KB stack) receives one job at a time from a
depth-1 queue and runs: Wi-Fi check → Whisper → Cerebras → (text → optional
TTS) or (tool call → run locally). Key pieces:

- **`cloud_submit(wav, bytes)`** (called from `afe_fetch_task`) sets `s_busy`
  and enqueues. `s_busy` stays set until *after* the result callback — so no new
  wake word/capture can overwrite the recording buffer mid-flight
  (`cloud_is_busy()` gate).
- **Events** to the UI (`cloud_event_cb_t`, consumed via flags in main.c):
  `CLOUD_EVT_TRANSCRIPT`, `_REPLY`, `_SPEAKING`, `_DONE`, `_ERROR`.
- **`speak_reply()`** — lazy `tts_init()` (2.5 MB PSRAM only on first use),
  synthesize, set `s_speaking`, then **blocking** PCM playback. Blocking keeps
  `s_busy`/`s_speaking` true for the whole utterance (no self-trigger from the
  speaker).
- **`run_tool()`** — executes a tool call locally and writes a deterministic
  confirmation string. Safe from this task: `kitchen_timer_*` (esp_timer +
  atomics), `volume_set` (atomic). Arguments are parsed from the OpenAI
  `arguments` JSON **string**.
- **TTS toggle** — `s_tts_enabled` (atomic); `CONFIG_TTS_SPEAK_REPLIES` only
  seeds the initial value. `cloud_set_tts_enabled()`/`cloud_tts_enabled()` for
  the button.

## whisper.c — Speech-to-Text (Groq)

[whisper.c](../../main/network/whisper.c). `POST` to
`api.groq.com/openai/v1/audio/transcriptions`, model
`whisper-large-v3-turbo`, as **multipart/form-data** (model, response_format,
language, file). The WAV is streamed in three `http_write_all` calls
(prologue, the recording buffer, epilogue) so it never has to be copied.

- Language hint from `config_whisper_language()` (default `de`) — without it
  Whisper guesses, costing latency and accuracy.
- Parses the `text` field, trims Whisper's leading space, rejects empty
  transcripts.
- 429/5xx → transient (retry); other 4xx (e.g. 401 bad key) → permanent.

## llm.c — the language model (Cerebras)

[llm.c](../../main/network/llm.c). `POST` to
`api.cerebras.ai/v1/chat/completions` (OpenAI-compatible), model from
`config_llm_model()` (default `gpt-oss-120b`), `Authorization: Bearer`.

### Request

`build_request()` assembles:
- a **system** message = active **persona** (`s_persona`, set by
  `llm_set_system_prompt`) + `TOOL_INSTRUCTIONS` (kept in every persona so the
  tools always work);
- the **conversation history** (from [state.c](../../main/state.c), mapped to
  user/assistant roles);
- the new **user** turn;
- a **tools** array with four function schemas (see below);
- `max_completion_tokens=300`, `temperature=0.7`, `reasoning_effort=low`.

> `gpt-oss-120b` is a reasoning model and reasoning tokens count against the
> completion budget — hence 300 (not 200) and `reasoning_effort=low`
> (~11 reasoning tokens measured). `zai-glm-4.7` mostly ignores
> `reasoning_effort`.

### Result

`parse_reply()` returns an `llm_result_t` ([llm.h](../../main/network/llm.h)) —
a tagged union of **TEXT** or **TOOL_CALL**. Tool calls take precedence (on a
tool call `content` is empty). The result is consumed in `cloud.c`:

- **TEXT** → assistant bubble (+ optional TTS), pushed to history.
- **TOOL_CALL** → `run_tool()` executes it, the confirmation becomes the model
  turn in history. No second LLM round-trip (saves tokens + latency).

### The four tools (function calling)

| Tool | Args | Effect (in `run_tool`) |
|---|---|---|
| `set_timer` | `seconds` (1–86400) | start kitchen timer, "Timer set: M:SS" |
| `set_volume` | `percent` (0–100) | `volume_set` (self-clamps), "Volume: N%" |
| `cancel_timer` | — | "Timer cancelled" / "No timer running" |
| `get_time_left` | — | "Time left: M:SS" / "No timer running" |

These exist for values the fixed MultiNet phrases can't express
("7 minutes", "35 percent"). See
[06-design-decisions.md](../06-design-decisions.md) for why this is single-call.

## tts.c — Text-to-Speech (local Piper, Gemini fallback)

[tts.c](../../main/network/tts.c). `tts_synthesize()` tries **two sources in
order** and returns the first that yields PCM:

1. **Local Piper server (primary).** `POST http://192.168.3.143:5050/tts`
   (`TTS_LOCAL_URL`), plain HTTP, body `{"text": ..., "voice": ...}`. The voice
   normally comes from `tts_set_local_voice()` (per-character: `jarvis`/
   `mycroft`/`alexa`; falls back to `TTS_LOCAL_VOICE`) — **except**
   `synthesize_local()` runs a tiny stopword/Umlaut heuristic
   (`text_is_german()`) over the reply text first and overrides the voice to
   `"de"` whenever it looks German. The Piper voices are single-language
   models, so sending German text to an English voice (or vice versa) makes
   it mispronounce badly; the per-character voice only applies to text
   detected as English. The response is **raw WAV bytes** (PCM, mono,
   16-bit). The 44-byte RIFF/WAVE header is skipped; the sample rate is read
   from header offset 24, and `*pcm` points straight into the WAV body in the
   PSRAM response buffer (no copy, no decode). Single attempt, 3 s timeout
   (`TTS_LOCAL_TIMEOUT_MS`) — any failure/timeout falls through to Gemini.
   Logs `[TTS] Using local` on success (and the chosen voice in the
   `local audio (voice=...)` debug line).
2. **Gemini (fallback).** Only reached if the local server fails. Logs
   `[TTS] Fallback to Gemini`, then `POST` to Gemini
   `gemini-2.5-flash-preview-tts:generateContent` with `responseModalities=AUDIO`
   and the voice from `tts_set_voice()` (per-character; falls back to
   `config_gemini_voice()`). `inlineData.data` is base64-decoded into a separate
   PSRAM buffer; the sample rate is parsed from the MIME type
   (`audio/L16;codec=pcm;rate=24000`). HTTP **429 → `ESP_ERR_NOT_ALLOWED`** =
   the free-tier **10 requests/day** limit (surfaced as "TTS daily limit", never
   retried). 5xx and missing-audio responses are transient and retried.

- Both the local WAV response (up to 1.5 MB) and the decoded Gemini PCM (up to
  1 MB) live in static **PSRAM** buffers allocated by `tts_init()`.

## http_util.c — shared HTTP helpers

[http_util.c](../../main/network/http_util.c): `http_write_all` (write a full
buffer), `http_read_body` (read into a fixed buffer), and the retry policy —
`http_err_is_transient()` + `http_backoff_delay()` with `HTTP_MAX_ATTEMPTS=3`
and 1 s/2 s backoff. Whisper/LLM/TTS each wrap their `*_once()` in this loop.

## Design notes & gotchas

- **Sequential by necessity, not choice** — see the golden rule above.
- **History only on success, in pairs** ([state.c](../../main/state.c)): a ring
  buffer of 10 turns × 1 KB in PSRAM; a failed request never leaves a half-turn,
  so follow-ups ("two minutes shorter") always have consistent context.
- **API keys** come from NVS (via `config_*` getters), built into `Bearer`
  headers at runtime — see [config.md](config.md).
- The original spec used Gemini for text; this is now **Cerebras**. Gemini is
  TTS-only — and only as a **fallback** behind the local Piper server.
