# MiniLingo — Code-Tour (Lese-Guide)

Diese Tour führt dich einmal komplett durch die Codebase, in der Reihenfolge, in der
der Code bei einem echten Sprachbefehl ("Alexa, stell einen Timer auf 5 Minuten")
tatsächlich ausgeführt wird. Öffne diese Datei neben dem Code und klick dich Station
für Station durch. Am Ende jeder Station steht ✍️ = das solltest du auf Papier festhalten.

> **Diese Datei ist praktisch, nicht referenziell.** Sie sagt dir *wohin klicken* und
> *worauf achten*. Die ausführliche Erklärung des *Warum* steht in den Referenz-Docs —
> vor allem [03-request-lifecycle.md](03-request-lifecycle.md) (dieselbe Reise als
> Sequenzdiagramm) und [02-architecture.md](02-architecture.md) (Task-Modell). Nutze diese
> Tour zum Durchklicken, jene Docs zum Nachschlagen.

## 0. Navigieren in VS Code (statt Debugger)

Das sind die einzigen Shortcuts, die du für die Tour brauchst (macOS):

| Aktion | Shortcut | Wofür |
|---|---|---|
| **Go to Definition** | `F12` oder `⌘`+Klick | In eine Funktion hineinspringen |
| **Go Back** | `Ctrl`+`-` | Zurück zur Absprungstelle (⚠️ Control, nicht ⌘) |
| **Go Forward** | `Ctrl`+`Shift`+`-` | Wieder vor |
| **Peek Definition** | `⌥`+`F12` | Funktion inline ansehen, ohne die Datei zu wechseln |
| **Find All References** | `⇧`+`F12` | "Wer ruft das auf?" |
| **Go to Symbol (Datei)** | `⌘`+`Shift`+`O` | Funktions-Übersicht der aktuellen Datei |
| **Go to Symbol (Projekt)** | `⌘`+`T` | Irgendeine Funktion per Name öffnen |
| **Quick Open** | `⌘`+`P` | Datei per Name öffnen |

**Der Trick:** `F12` rein in eine Funktion, lesen, `Ctrl`+`-` wieder raus. So "durchklickst"
du den Call-Stack ohne laufende Hardware.

## 1. Das große Bild (erst lesen, dann losklicken)

MiniLingo ist ein **event-getriebener Sprachassistent** auf dem ESP32-S3 (2 Kerne).
Zwei Dinge musst du verinnerlichen, dann ergibt der Rest sich von selbst:

**(a) Mehrere Tasks auf zwei Kernen** — grob:
- **Core 1:** `afe_feed_task` + `afe_fetch_task` — Mikrofon → Audio-Frontend → Wakeword/Command-Erkennung
- **Core 0:** `lvgl_task` (Display/UI), `network_task` (Cloud-Requests), WiFi-Event-Task
- **Player:** `player_task` — spielt PCM über I2S auf den Lautsprecher
- **esp_timer-Task:** Küchen-Timer

**(b) Die Callbacks dürfen die UI NICHT direkt anfassen** (LVGL ist nicht thread-safe).
Deshalb das zentrale Muster im ganzen Projekt:

```
Callback (irgendein Task)  →  setzt atomares Flag  →  app_event_timer_cb (alle 50 ms,
                                                       LVGL-Task) holt Flag ab → UI-Update
```

Merk dir diese zwei Ankerpunkte — dahin springst du immer wieder zurück:
- [main.c:667](../main/main.c#L667) — `app_main()`: baut alles auf, registriert alle Callbacks
- [main.c:567](../main/main.c#L567) — `app_event_timer_cb()`: übersetzt Flags → UI

✍️ Skizziere die 2 Kerne + die Task-Namen und das "Flag → Timer → UI"-Muster.

## 2. Start: `app_main()` — der Aufbau

📍 [main.c:667](../main/main.c#L667)

Lies `app_main()` einmal von oben nach unten. Es ist eine reine Init-Sequenz. Achte auf
die Reihenfolge und **wer welchen Callback bekommt** (das verrät dir den ganzen Flow):

- [main.c:681](../main/main.c#L681) `config_init()` — Config aus NVS (SSID/Keys/Modelle)
- [main.c:690-692](../main/main.c#L690-L692) Charakter/Persona setzen (`llm_set_system_prompt`, `tts_set_voice`)
- [main.c:696-719](../main/main.c#L696-L719) LVGL/Display/Touch/UI + **`app_event_timer_cb` als 50-ms-Timer** (Zeile 717) + `lvgl_task`
- [main.c:721](../main/main.c#L721) `wifi_init(on_wifi_status)`
- [main.c:732](../main/main.c#L732) `cloud_init(on_cloud_event)` — startet `network_task`
- [main.c:735-739](../main/main.c#L735-L739) Audio-Ausgabe, Volume, Canned-Audio, Küchen-Timer
- [main.c:749](../main/main.c#L749) **`wake_word_set_listener(on_wake_word)`** ← merken!
- [main.c:750](../main/main.c#L750) **`recorder_init(on_recording_done)`** ← merken!
- [main.c:753](../main/main.c#L753) **`commands_init(models, on_local_command, on_command_timeout)`** ← merken!
- [main.c:756](../main/main.c#L756) `afe_pipeline_init(models, s_character)` — startet die Audio-Tasks

✍️ Notiere die 4 wichtigen Callback-Registrierungen (Zeilen 749/750/753 + `on_cloud_event`).
Diese Callbacks sind die Landepunkte des restlichen Flows.

## 3. Der Haupt-Flow: ein Cloud-Sprachbefehl

Ab hier folgst du dem Weg des Audios. Springe mit `F12` rein, `Ctrl`+`-` raus.

### 3.1 Dauerbetrieb: Audio → Wakeword

📍 [afe_pipeline.c:132](../main/speech/afe_pipeline.c#L132) `afe_feed_task` — Mikrofon → AFE (Rauschunterdrückung etc.)
📍 [afe_pipeline.c:182](../main/speech/afe_pipeline.c#L182) `afe_fetch_task` — holt AFE-Ergebnisse, prüft Wakenet.

In `afe_fetch_task` bei [afe_pipeline.c:232](../main/speech/afe_pipeline.c#L232) wird bei
erkanntem Wakeword aufgerufen: `wake_word_on_detected(res->wake_word_index)`.

✍️ Halte fest: die zwei AFE-Tasks laufen dauernd; erst das Wakeword startet den Rest.

### 3.2 Wakeword erkannt

📍 [wake_word.c:16](../main/speech/wake_word.c#L16) `wake_word_on_detected()` — nur Log +
ruft `s_listener()`. Der Listener wurde in `app_main` gesetzt → `F12` auf `s_listener`
landet bei:

📍 [main.c:251](../main/main.c#L251) `on_wake_word()` — setzt Session-Zustand zurück,
`s_wake_event = true`.

➡️ Flag-Abholung: [main.c:574](../main/main.c#L574) in `app_event_timer_cb` → Mic-Button auf **LISTENING**.

✍️ Erste komplette Kette: `wake_word_on_detected → on_wake_word → (Flag) → UI LISTENING`.

### 3.3 Aufnahme + parallele Command-Erkennung

Nach dem Wakeword füttert `afe_fetch_task` jeden Frame an **zwei** Abnehmer parallel
(`afe_pipeline.c` ~Zeile 266):

- **Recorder** (nimmt WAV auf): 📍 [recorder.c:84](../main/speech/recorder.c#L84)
  `recorder_feed()` — sammelt Samples, erkennt Sprache/Stille; nach ~1,5 s Stille →
  📍 [recorder.c:67](../main/speech/recorder.c#L67) `finish()` → ruft `on_recording_done`.
- **MultiNet** (lokale Commands): 📍 [commands.c:308](../main/speech/commands.c#L308)
  `commands_feed()` → 📍 [commands.c:264](../main/speech/commands.c#L264) `detect_chunk()`.
  Bei Treffer → `on_local_command`; bei Timeout ohne Treffer → `on_command_timeout`.

📍 [main.c:270](../main/main.c#L270) `on_recording_done()` — **Stille-Guard** (zu leise →
verwerfen), sonst WAV zwischenspeichern (`s_pending_wav`). Ruft ggf. `submit_cloud_query()`.

📍 [main.c:294](../main/main.c#L294) `on_command_timeout()` — "kein lokaler Command"; ruft
ggf. `submit_cloud_query()`.

⚠️ Wichtiges Detail (auf Papier!): Der Cloud-Pfad startet erst, wenn **beide** Bedingungen
erfüllt sind — WAV fertig UND MultiNet-Timeout. Deshalb rufen sowohl `on_recording_done`
als auch `on_command_timeout` `submit_cloud_query()` auf (wer zuletzt fertig wird, gewinnt).

### 3.4 Cloud-Query abschicken

📍 [main.c:262](../main/main.c#L262) `submit_cloud_query()` → `cloud_submit(...)`, setzt `s_processing_event`.
📍 [cloud.c:222](../main/network/cloud.c#L222) `cloud_submit()` — setzt `busy`, legt einen Job in die Queue.

➡️ Flag `s_processing_event`: [main.c:581](../main/main.c#L581) → Mic-Button **PROCESSING**.

### 3.5 Die Cloud-Pipeline (`network_task`) — das Herzstück

📍 [cloud.c:139](../main/network/cloud.c#L139) `network_task()` — lies diese `while(1)`-Schleife
**komplett**, sie orchestriert alles Folgende sequenziell (nie zwei TLS-Verbindungen
gleichzeitig):

1. **STT:** 📍 [cloud.c:161](../main/network/cloud.c#L161) `whisper_transcribe(...)` → Text. Fires `CLOUD_EVT_TRANSCRIPT`.
2. **LLM:** 📍 [cloud.c:168](../main/network/cloud.c#L168) `llm_generate(text, &result)` → Antwort **oder** Tool-Call.
3. Bei **Tool-Call** (`LLM_RESULT_TOOL_CALL`): 📍 [cloud.c:175](../main/network/cloud.c#L175)
   `run_tool(...)` → Timer/Volume etc. lokal ausführen. Fires `CLOUD_EVT_REPLY` + `CLOUD_EVT_DONE`.
4. Bei **normaler Antwort:** `CLOUD_EVT_REPLY`, dann falls TTS an →
   📍 [cloud.c:62](../main/network/cloud.c#L62) `speak_reply(...)`.
5. Ganz am Ende: `busy = false` ([cloud.c:195](../main/network/cloud.c#L195)) → erst jetzt
   darf ein neues Wakeword kommen.

✍️ Zeichne die Pipeline als 3 Boxen: Whisper → LLM → (TTS). Jede Box feuert ein Cloud-Event.

### 3.6 STT-Details: Whisper

📍 [whisper.c:162](../main/network/whisper.c#L162) `whisper_transcribe()` — Retry-Wrapper. Ruft:
📍 [whisper.c:78](../main/network/whisper.c#L78) `transcribe_once()` — baut multipart/form-data
HTTP-POST (WAV → Groq Whisper), Antwort → 📍 [whisper.c:48](../main/network/whisper.c#L48)
`parse_transcript()` (JSON → Text).

### 3.7 LLM-Details: Cerebras

📍 [llm.c:314](../main/network/llm.c#L314) `llm_generate()` — Retry-Wrapper. Ruft:
📍 [llm.c:234](../main/network/llm.c#L234) `generate_once()` →
- 📍 [llm.c:135](../main/network/llm.c#L135) `build_request()` — baut JSON: System-Prompt +
  Chat-History + User-Text, hängt via 📍 [llm.c:118](../main/network/llm.c#L118) `add_tools()`
  die Tool-Definitionen (Timer/Volume/…) an.
- HTTP-POST, Antwort → 📍 [llm.c:176](../main/network/llm.c#L176) `parse_reply()` → Text oder Tool-Call.

✍️ Notiere: Tools werden dem LLM als JSON-Schema mitgegeben; die Ausführung passiert aber
lokal in `run_tool` (cloud.c), nicht in der Cloud.

### 3.8 TTS-Details + Wiedergabe

📍 [tts.c:404](../main/network/tts.c#L404) `tts_synthesize()` — versucht **zuerst lokal**
(eigener Piper-Server), dann Cloud als Fallback:
- 📍 [tts.c:150](../main/network/tts.c#L150) `synthesize_local()` (nutzt
  📍 [tts.c:84](../main/network/tts.c#L84) `text_is_german` + [tts.c:133](../main/network/tts.c#L133)
  `build_local_request`),
- Fallback: 📍 [tts.c:314](../main/network/tts.c#L314) `synthesize_once()` (Gemini-TTS,
  [tts.c:234](../main/network/tts.c#L234) `build_request`).

Zurück in `speak_reply` ([cloud.c:62](../main/network/cloud.c#L62)): das PCM geht an
📍 [canned_audio.c:234](../main/local/canned_audio.c#L234) `canned_audio_play_pcm()` → Queue an
📍 [canned_audio.c:163](../main/local/canned_audio.c#L163) `player_task` →
📍 [canned_audio.c:145](../main/local/canned_audio.c#L145) `play_pcm()` →
📍 [audio_out.c:77](../main/drivers/audio_out.c#L77) `audio_out_write()` → I2S → Lautsprecher.

➡️ Events: `CLOUD_EVT_SPEAKING` → Mic-Button **SPEAKING** ([main.c:607](../main/main.c#L607));
danach `CLOUD_EVT_DONE` → zurück auf **IDLE** ([main.c:613](../main/main.c#L613)).

### 3.9 Wo alle Cloud-Events landen

📍 [main.c:303](../main/main.c#L303) `on_cloud_event()` — der eine Callback, den `network_task`
für jedes Ereignis ruft. Er macht nichts als das passende atomare Flag setzen. Die eigentliche
UI-Reaktion steht im Timer:

📍 [main.c:567](../main/main.c#L567) `app_event_timer_cb()` — lies alle
`if (atomic_exchange(...))`-Blöcke (Zeilen 574–626): Transcript-Bubble, Reply-Bubble,
Mic-Button-States, Fehler-Cue.

✍️ Damit ist der Kreis geschlossen: Callback → Flag → Timer → UI. Zeichne den ganzen
Haupt-Flow einmal als eine Linie vom Wakeword bis zur DONE-Rückkehr auf IDLE.

## 4. Die Nebenpfade (Abzweigungen)

Jetzt, wo der Haupt-Flow sitzt, die Verzweigungen — jeweils kurz reinklicken:

### 4.1 Lokaler Command (kein Cloud)
📍 [main.c:331](../main/main.c#L331) `on_local_command()` — MultiNet hat einen der 14 Commands
erkannt. Bricht die Aufnahme ab (`recorder_abort`), führt den Handler aus, spielt eine
Canned-Antwort. Interessant: der `if (cloud_is_speaking() ...)`-Guard
([main.c:342](../main/main.c#L342)) gegen False-Positives aus dem eigenen Lautsprecher.

### 4.2 Charakter-Wechsel
📍 [main.c:238](../main/main.c#L238) `on_character_clicked()` (Tap auf State-Text) →
📍 [main.c:212](../main/main.c#L212) `apply_character()` — tauscht Persona (LLM), Stimme (TTS)
und **Wakeword zur Laufzeit** (`afe_pipeline_request_wakeword`), lässt den neuen Charakter
sich per `cloud_speak(greeting)` vorstellen. (Ein Command kann dasselbe auslösen — siehe
`on_local_command`.)

### 4.3 Mic-Button / Touch statt Sprache
📍 [main.c:176](../main/main.c#L176) `on_mic_button_clicked()` →
📍 [wake_word.c:24](../main/speech/wake_word.c#L24) `wake_word_on_touch()` — derselbe Listener
wie das Wakeword, nur per Touch ausgelöst.

### 4.4 Küchen-Timer
📍 [timer.c:39](../main/local/timer.c#L39) `kitchen_timer_start()` (aus `run_tool` aufgerufen)
→ läuft ab → 📍 [timer.c:18](../main/local/timer.c#L18) `timer_done()` →
📍 [main.c:321](../main/main.c#L321) `on_timer_done()` → Flag → Alarm-Sound + UI.

### 4.5 Fehler-Recovery
In `app_event_timer_cb` bei [main.c:619](../main/main.c#L619) (`s_cloud_error_event`):
Fehler-Bubble + `canned_audio_play(CANNED_ERROR)` + zurück auf IDLE. Die Fehlerquellen stehen
in `network_task` (jedes `CLOUD_EVT_ERROR`).

## 5. Referenz: Datei-Landkarte

| Ordner | Rolle |
|---|---|
| [main/main.c](../main/main.c) | Orchestrator: Init, alle Callbacks, Event→UI-Timer |
| [main/speech/](../main/speech/) | Audio-Frontend, Wakeword, Recorder, lokale Commands |
| [main/network/](../main/network/) | Cloud: `cloud.c` (Orchestrierung), `whisper` (STT), `llm`, `tts`, `wifi` |
| [main/local/](../main/local/) | Auf-Gerät-Aktionen: Timer, Volume, Temperatur, Canned-Audio |
| [main/drivers/](../main/drivers/) | Hardware: Display, Touch, Audio-In (Mic), Audio-Out (Speaker) |
| [main/ui/](../main/ui/) | LVGL-UI: Chat-Bubbles, Mic-Button, Status-Leiste |
| [main/config.c](../main/config.c), [main/state.c](../main/state.c) | Laufzeit-Config (NVS), Chat-History |

Tiefergehende Erklärungen pro Bereich: [04-subsystems/](04-subsystems/).

## 6. Empfohlene Klick-Reihenfolge (Kurzfassung zum Abhaken)

1. [main.c:667](../main/main.c#L667) `app_main` — Aufbau + Callbacks
2. [main.c:567](../main/main.c#L567) `app_event_timer_cb` — Flag→UI-Muster
3. [afe_pipeline.c:182](../main/speech/afe_pipeline.c#L182) `afe_fetch_task` → [wake_word.c:16](../main/speech/wake_word.c#L16) → [main.c:251](../main/main.c#L251) `on_wake_word`
4. [recorder.c:84](../main/speech/recorder.c#L84) `recorder_feed` → [recorder.c:67](../main/speech/recorder.c#L67) `finish` → [main.c:270](../main/main.c#L270) `on_recording_done`
5. [commands.c:308](../main/speech/commands.c#L308) `commands_feed` → [commands.c:264](../main/speech/commands.c#L264) `detect_chunk` → [main.c:294](../main/main.c#L294)/[main.c:331](../main/main.c#L331)
6. [main.c:262](../main/main.c#L262) `submit_cloud_query` → [cloud.c:222](../main/network/cloud.c#L222) `cloud_submit`
7. [cloud.c:139](../main/network/cloud.c#L139) `network_task` (die Pipeline)
8. [whisper.c:162](../main/network/whisper.c#L162) → [llm.c:314](../main/network/llm.c#L314) → [tts.c:404](../main/network/tts.c#L404) → [canned_audio.c:234](../main/local/canned_audio.c#L234) → [audio_out.c:77](../main/drivers/audio_out.c#L77)
9. [main.c:303](../main/main.c#L303) `on_cloud_event` (zurück zum Timer in Schritt 2)
10. Nebenpfade: [main.c:331](../main/main.c#L331), [main.c:212](../main/main.c#L212), [main.c:176](../main/main.c#L176), [timer.c:18](../main/local/timer.c#L18)
