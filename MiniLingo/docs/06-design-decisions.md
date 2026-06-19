# 6. Design Decisions (the "why")

The non-obvious choices, each with the reasoning behind it. When a decision was
forced by a bug we hit, the war-story is in
[07-lessons-learned.md](07-lessons-learned.md); here we keep the rationale.

## Hybrid local/cloud split

**Decision:** wake word + a fixed command set run on-device; everything else
goes to the cloud.
**Why:** the common cases (timer, volume, stop) must be instant, free, and work
offline. Round-tripping "volume up" to a server would be absurd. Free-form
questions need an LLM, which the chip can't host — so only those pay the latency
and quota cost. The routing happens automatically: recorder and MultiNet run on
the same frames, and a MultiNet timeout (no match) is the cloud trigger.

## Cerebras for the LLM (not Gemini)

**Decision:** text answers come from Cerebras (`gpt-oss-120b`,
OpenAI-compatible), not Gemini as the original spec said.
**Why:** Gemini's free tier (250 requests/day) was too tight for daily use once
even text answers counted against it. Cerebras free is ~1M tokens/day
(~1,500–2,500 questions) — roughly 8–10× headroom — and answers in ~500 ms.
Groq Whisper was kept for STT; Gemini remains only for (optional) TTS. Cerebras
also supports tool calling, which set the format for Phase 10.

## Single-call tool calling (no second round-trip)

**Decision:** when the LLM returns a tool call, the firmware executes it and
**synthesizes its own confirmation text** — it does not send the tool result
back to the model for a second completion.
**Why:** saves tokens and latency (same motivation as the Cerebras switch). The
confirmation is worded exactly like the local-command bubbles
(`format_command_bubble`), so cloud and local actions look identical, and the
confirmation is pushed to history as the model turn so follow-ups ("two minutes
shorter") still have context. No `CHAT_ROLE_TOOL` needed.

## TTS off by default, runtime-toggleable

**Decision:** spoken replies are off by default; a button toggles them live;
`CONFIG_TTS_SPEAK_REPLIES` only seeds the initial state.
**Why:** Gemini TTS free tier is **10 requests/day** and adds 3–5 s latency —
too little for everyday use. The text bubble is enough most of the time. The
2.5 MB PSRAM buffers are allocated lazily on first use, so "off" is exactly as
memory-cheap as never compiling TTS in. The original compile-only Kconfig flag
became just the default.

## Per-character persona + voice

**Decision:** each character (Mycroft/Alexa/Jarvis) has its own wake word,
system-prompt persona, and Gemini TTS voice, in one `character_t` table in
[main.c](../main/main.c).
**Why:** it's the product's personality, and keeping name/idle-text/persona/voice
together makes adding a character a one-line change. Persona is pushed to the LLM
via `llm_set_system_prompt` (a pointer swap) and the voice via `tts_set_voice`,
both at boot — mirroring one pattern.

## Reboot to switch characters (one WakeNet per boot)

**Decision:** switching character saves the index to NVS and reboots; only the
active character's WakeNet is loaded.
**Why:** loading all three WakeNets at once exhausts internal SRAM — the AFE
task stacks fail to allocate and the device boot-loops. All three models live in
flash; loading one is well within budget. A reboot (~3 s) is an acceptable price
for a rare action, and it sidesteps any live-reconfiguration complexity. (The
live-filter approach was tried and failed — see
[07-lessons-learned.md](07-lessons-learned.md).)

## One TLS connection at a time; mbedTLS in PSRAM

**Decision:** all cloud requests are serialized in `network_task`, and mbedTLS
allocates from PSRAM.
**Why:** a TLS handshake is ~50 KB and didn't fit in internal SRAM next to
esp-sr (`mbedtls_ssl_setup -0x7F00` = alloc failed). Moving mbedTLS to PSRAM
fixes the handshake; serializing requests guarantees we never pay for two at
once. This is also why STT→LLM→TTS is a strict sequence, not parallel.

## One audio player for canned + TTS

**Decision:** pre-recorded WAVs and Gemini-TTS PCM share one player task and one
queue (`canned_audio`), instead of a separate TTS queue.
**Why:** one I2S writer means "stop" aborts both, and the timer alarm vs. a
spoken reply serialize naturally. `canned_audio_play_pcm()` blocks until
playback ends, which conveniently keeps the pipeline "busy" so the speaker can't
self-trigger a new wake word.

## State machine as flags in main.c (not in state.c)

**Decision:** the IDLE/LISTENING/PROCESSING/SPEAKING state is atomic flags +
the event pump in [main.c](../main/main.c); `state.c` holds only the
conversation history.
**Why:** LVGL isn't thread-safe, so cross-task UI updates had to funnel through
one place anyway. Flags + a 50 ms pump are simpler than a locked state object,
and there's exactly one consumer (the LVGL task). No locking is needed because
each producer writes its text buffer before its flag.

## Per-command minimum probability (not a single threshold)

**Decision:** the global MultiNet threshold is low (0.03), but short commands
additionally require prob ≥0.25 (≥0.30 for "stop" during playback); a rejected
match keeps listening.
**Why:** the timer/volume phrases only match at 0.05–0.15, so the global
threshold must be low — but at that level German free-speech sentences trip the
short commands ("Wie…" → "mute"). A per-command floor rejects those while
keeping the session alive so the sentence still reaches the cloud. Measured:
real short commands match at 0.84–0.92, false positives at 0.04–0.20 — clean
separation.

## Config from NVS with Kconfig fallback

**Decision:** runtime settings live in NVS; Kconfig provides defaults; a SoftAP
portal writes NVS.
**Why:** lets a user change Wi-Fi and keys without a PC or a re-flash, while a
fresh board still boots from compiled defaults. Keys are plain text in NVS
(hobby-acceptable, same as the old sdkconfig).

## Things deliberately *not* done

- **AEC** (acoustic echo cancellation) — optional; left off (Phase 9).
- **Device-side TTS streaming** — tried, reverted (audio was choppy; the fix
  that would work, a PSRAM jitter buffer, wasn't pursued). See
  [08-status-and-roadmap.md](08-status-and-roadmap.md).
- **Touch calibration screen** — touch was accurate enough in every test.
- **A second LLM round-trip for tool results** — see single-call above.
