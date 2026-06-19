# Subsystem: Speech (on-device)

Everything between the microphone and the routing decision: the AFE pipeline,
wake-word handling, the recorder, and the MultiNet command recognizer. Files:
[speech/](../../main/speech/). This is the most subtle subsystem — read
[03-request-lifecycle.md](../03-request-lifecycle.md) first.

## AFE pipeline — afe_pipeline.c

[afe_pipeline.c](../../main/speech/afe_pipeline.c) owns the two real-time tasks
on Core 1 (prio 5).

- **`afe_feed_task`**: reads mic chunks (`audio_in_read`) and `feed()`s them to
  the AFE. Every ~3 s it can log mic RMS/peak at debug level (the same
  diagnostic as `mic_test.c`) — useful for gain tuning.
- **`afe_fetch_task`**: pulls AFE results (`fetch_with_delay`) and is the
  central dispatcher. Per frame it decides: start a session (wake word/touch),
  or feed an active session's recorder + MultiNet, or — during TTS playback —
  run the restricted "stop"-only MultiNet session.

Config (esp-sr 2.x API, not the spec's V1 macro): `afe_config_init("M", models,
AFE_TYPE_SR, AFE_MODE_LOW_COST)` with `aec_init=false`, `wakenet_init=true`,
`vad_init=true`, pinned to core 1 / prio 5.

**One wake word per boot.** `afe_pipeline_init(models, character_idx)` selects
exactly one WakeNet model by character (0=Mycroft→"mycroft", 1=Alexa→"alexa",
2=Jarvis→"jarvis") via `esp_srmodel_filter`. All three live in flash, but
loading more than one at boot exhausts internal RAM (AFE task-stack alloc fails
→ boot loop). Character switching reboots — see
[config.md](config.md) and [07-lessons-learned.md](../07-lessons-learned.md).

The optional `CONFIG_WAKE_WORD_THRESHOLD` (0–100, 0 = model default) maps to the
WakeNet 0.0–1.0 range; higher = stricter.

## Wake word — wake_word.c

[wake_word.c](../../main/speech/wake_word.c) is intentionally thin: it just
holds the listener callback and logs detections. `wake_word_on_detected()` and
`wake_word_on_touch()` both invoke the same listener (`on_wake_word` in
[main.c](../../main/main.c)) — so a touch of the mic button and a spoken wake
word start identical sessions.

## Recorder — recorder.c

[recorder.c](../../main/speech/recorder.c) accumulates the captured utterance
into one PSRAM buffer that already has room for the WAV header, so it hands the
cloud a ready-to-upload file.

- 16 kHz, mono, 16-bit. **Stops** after **1500 ms** of continuous silence
  (`is_speech` comes from the AFE VAD) or at the **8000 ms** hard cap.
- Tracks `speech_ms` separately — the silence guard in `main.c` uses it to
  discard near-silent captures (Whisper hallucination guard).
- `recorder_abort()` is called when a local command matches, discarding the
  capture. After capture ends or abort, further `recorder_feed()` calls are
  no-ops.
- On finish it writes the 44-byte header (`wav_write_header`) and calls
  `on_recording_done` with the buffer, durations, and stop reason.

## WAV header — wav.c

[wav.c](../../main/speech/wav.c) writes the classic 44-byte RIFF/PCM header
byte-by-byte (no alignment assumptions on the PSRAM target buffer). Used by the
recorder; the playback side has its own chunk-walking parser (see
[local.md](local.md)).

## Commands (MultiNet) — commands.c

[commands.c](../../main/speech/commands.c) loads MultiNet7-EN (quantized) from
the model partition and recognizes the 14 local commands. The
`afe_fetch_task` feeds it the same frames as the recorder, buffered to
MultiNet's chunk size.

### The command set

| Command(s) | Phrases (examples) |
|---|---|
| Timer set | "set timer five minutes", "five minute timer" (30 s / 1 / 5 / 10 / 15 / 30 min) |
| Timer stop/cancel | "stop timer", "cancel timer" |
| Time left | "how much time left" |
| Stop | "stop" |
| Volume up/down | "volume up", "volume down" |
| Volume level | "volume twenty/forty/sixty/eighty/one hundred percent" |
| Mute/unmute | "mute", "unmute" |

A command id can have multiple phrases. On a match, `s_match_cb`
(`on_local_command`) gets the id, the matched string, and the probability.

### Thresholds and false positives

- **Global detection threshold `0.03`** (`MN_DET_THRESHOLD`). The timer/volume
  phrases match with low probabilities (0.05–0.15), so the threshold must be
  low. (It was lowered from 0.05 because "volume sixty percent" only matched
  ~half the time; 0.01 caused false positives — don't go lower.)
- **Per-command minimum probability** (`min_prob`): the short commands (stop,
  mute, unmute, volume up/down) require **≥0.25**, because German free-speech
  sentences clip them mid-word ("Wie…" → "mute"). A rejected match **keeps the
  session listening** (and restarts the 6 s timeout) rather than ending it, so
  the sentence still reaches the cloud. During TTS playback, "stop" needs
  **≥0.30** (checked in `main.c`).
- The per-phrase `threshold` field esp-sr offers was deliberately *not* used
  (undocumented whether MultiNet7 respects it, and it couldn't continue the
  session).

### Phoneme workarounds

MultiNet7's English model has two real bugs that forced hand-written phonemes
(generated with the host G2P tool `multinet_g2p.py`):

- The G2P token **`MgNcTS` ("minutes") is broken in the FST** — any phrase using
  it never reaches the detected state. Workaround: register without the final
  S (`MgNcT`); the spoken plural-S is tolerated as a junk suffix. (Side effect:
  "stop timer" often matches as "stop" because the endpoint fires early.)
- The on-device G2P renders "minute" as `MgNoT` ("min-oot") but it's spoken
  `MgNcT` ("min-it") — so the short aliases get explicit phonemes too.
- **"sixty"** as one token (`SgKSTm`) never matches (likely FST-dead like
  `MgNcTS`); split as `SgKS Tm` it works, plus pronunciation aliases
  ("zixty"/"sexty"/"seexty") for real voices.

`MN_TIMEOUT_MS = 6000` is the no-match timeout; a warmup of ~16 silent
`detect()` calls primes the caches at init.

## Design notes & gotchas

- **The MultiNet session outlives the recorder.** The recorder stops at 1.5 s
  silence, but MultiNet's endpoint needs longer for long phrases — so the
  session runs to its own match/timeout. The cloud upload waits for the session
  to end (the routing decision).
- **`vad_cache` is ignored** (neither recorder nor MultiNet get it): it
  overlapped already-delivered frames → doubled phrase starts ("set set timer").
  Capture starts right after the wake word anyway, so the speech onset is
  complete without it.
- **"thirty minutes" doesn't match locally** (collides with "thirty seconds");
  low priority because tool-calling handles arbitrary timer values via the
  cloud.
- `switch_loader_mode()` is a NULL pointer in the quantized MultiNet7 — calling
  it boot-loops. Don't.
