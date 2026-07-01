# Subsystem: UI (LVGL)

The on-screen interface. Files: [ui/](../../main/ui/). All of it runs in
`lvgl_task`; nothing else may call these functions directly (see the thread
boundaries in [02-architecture.md](../02-architecture.md)).

## Layout

`ui_init()` ([ui.c](../../main/ui/ui.c)) sets a dark background and builds three
regions:

```
┌───────────────────────────────────────┐
│ WiFi   |   state text   |   volume     │  status bar (top, 30 px)
├───────────────────────────────────────┤
│  user bubble (right, blue)             │
│  assistant bubble (left, grey)         │  chat area (scrollable)
│  local-response bubble (left, green)   │
├───────────────────────────────────────┤
│            ( mic )            [TTS]     │  mic button area (bottom, 60 px)
└───────────────────────────────────────┘
```

## Status bar — ui_status_bar.c

[ui_status_bar.c](../../main/ui/ui_status_bar.c). Wi-Fi indicator (left), state
text (center), volume (right). Three regions are **clickable**, each via a
callback registered from `main.c`:

- **Wi-Fi icon** → `on_wifi_clicked` → opens the setup portal
  ([config.md](config.md)).
- **State text** → `on_character_clicked` → cycles Mycroft→Alexa→Jarvis (runtime
  WakeNet swap, no reboot; also switchable by voice "switch to X").
- **Volume** → opens a touch **slider** below the bar (auto-hides after 3 s).
  Because resistive touch is coarse and the chat container sits above it, the
  panel is moved to the foreground and given an extended click area.

The state text shows (in priority order) "Listening…", "Processing…",
"Speaking…", a "Timer: MM:SS" countdown, or the active character's idle text —
computed by `update_state_text()` in main.c, written only on change.

## Chat — ui_chat.c

[ui_chat.c](../../main/ui/ui_chat.c). Three bubble helpers:
`ui_chat_add_user_bubble` (right, blue), `ui_chat_add_assistant_bubble` (left,
grey), `ui_chat_add_local_bubble` (left, green, for command confirmations and
system messages).

- Bubbles use a **custom Latin-1 font**,
  [font_montserrat_14_latin1.c](../../main/ui/font_montserrat_14_latin1.c),
  generated with `lv_font_conv` (the command line is in the file header). The
  built-in Montserrat 14 is ASCII-only, but German transcripts/answers contain
  umlauts (and the LLM emits typographic quotes/dashes/ellipsis). The status bar
  and mic button keep the default font because they need its FontAwesome glyphs
  (Wi-Fi, audio).

## Mic button — ui_mic_button.c

[ui_mic_button.c](../../main/ui/ui_mic_button.c). Center-bottom button with
state colors and animations: a **pulse** (scale 100→115 %) while LISTENING and
an orange **spinner ring** while PROCESSING; green for LOCAL_RESPONSE/SPEAKING.
Tapping it triggers a listening session (same path as the wake word). States are
driven by `ui_mic_button_set_state()` from the event pump.

## TTS toggle — ui_tts_button.c

[ui_tts_button.c](../../main/ui/ui_tts_button.c). A small button bottom-right
(green = on, grey = off) that toggles spoken replies at runtime. Pure UI — the
click reports via callback to `main.c`, which flips the atomic flag in
`cloud.c`. The 2.5 MB TTS buffers are still allocated lazily on first actual
use, so "off" costs no PSRAM.

## Design notes & gotchas

- **Everything UI goes through the 50 ms event pump.** Producers in other tasks
  set atomic flags + text buffers; `app_event_timer_cb` (main.c) turns them into
  these calls. Don't call `ui_*` from audio/network/timer callbacks.
- `update_state_text()` and `update_volume_display()` write only on change (no
  per-tick redraw); the volume display is polled from `volume_get()` so it
  reflects all paths (voice command, slider, and cloud `set_volume`).
- The mic-button glyph is `LV_SYMBOL_AUDIO` as a placeholder (LVGL's default
  font has no microphone glyph).
