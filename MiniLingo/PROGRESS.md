# MiniLingo – Projektfortschritt

> **Lies dieses Dokument zuerst**, wenn du eine neue Session zu diesem
> Projekt startest. Es zeigt, wo wir stehen und was als nächstes ansteht.
>
> - Architektur & Implementierungsplan: [MINILINGO_VOICE_ASSISTANT_SPEC.md](MINILINGO_VOICE_ASSISTANT_SPEC.md)
>   (insb. Section 12 "Implementierungsphasen")
> - Hardware-Validierung der Einzelkomponenten: [COMPONENT_TESTS.md](COMPONENT_TESTS.md)

---

## Status-Übersicht

| Phase | Beschreibung | Status | Notizen/Datum |
|-------|--------------|--------|---------------|
| 0 | Hardware-Komponententests (Mic, Amp, Display, Touch) | ✅ erledigt | Siehe [COMPONENT_TESTS.md](COMPONENT_TESTS.md), 2026-06-10 |
| 1 | Projekt-Skelett + LVGL + UI-Mockup | ✅ erledigt | 2026-06-10, am Gerät verifiziert (UI, Touch, Scrolling) |
| 2 | WiFi + esp-sr + Wake Word | ✅ erledigt | 2026-06-10, am Gerät verifiziert: Wake Word 3/3 Detections, WiFi verbindet (2,4-GHz-SSID) |
| 3 | Audio-Capture + Recording-Buffer | ✅ erledigt | 2026-06-10, am Gerät verifiziert: WAV via UART-Dump extrahiert, Sprache anhörbar |
| 4 | MultiNet + Lokale Commands | 🟡 implementiert | 2026-06-10, geflasht, Boot-Log sauber (mn7_en + SPIFFS ok); Sprachtest (Acceptance) ausstehend |
| 5 | Groq Whisper Integration | ⬜ offen | |
| 6 | Gemini Flash Integration | ⬜ offen | |
| 7 | Gemini TTS + Wiedergabe | ⬜ offen | |
| 8 | Polish + Robustness | ⬜ offen | |
| 9 (optional) | AEC | ⬜ offen | |
| 10 (optional) | Function-Calling für freie Timer-Werte | ⬜ offen | |

---

## Aktueller Stand / Nächster Schritt

**Stand 2026-06-10 (Session 5):** Phase 4 implementiert, gebaut und
geflasht (inkl. SPIFFS-Image). Boot-Log sauber: `mn7_en ready: 14
commands, chunk 512 samples`, `SPIFFS mounted, 0 canned file(s)
missing`, AFE-Pipeline läuft. Aufbau:

- `main/speech/commands.c`: MultiNet7-EN aus der model-Partition,
  14 Commands (Spec Section 7), Timeout 6s. Der `afe_fetch_task`
  feedet MultiNet parallel zum Recorder (gleiche Frames inkl.
  `vad_cache`); ein Sammel-Buffer puffert auf die MultiNet-
  Chunk-Größe (derzeit identisch zur AFE-Chunk-Größe, 512 Samples).
  Match → `on_local_command()` in `main.c` (verwirft die Aufnahme per
  `recorder_abort()`, führt den Handler aus, spielt die
  Canned-Response); MultiNet-Timeout → Aufnahme läuft weiter und ist
  ab Phase 5 der Cloud-Pfad.
- `main/local/timer.c`: Kitchen-Timer als One-Shot-`esp_timer`,
  Restzeit aus gespeicherter Deadline (atomar) → Status-Bar zeigt
  `Timer: MM:SS` (LVGL-Timer in `main.c`). Bei Ablauf: Canned
  "Your timer is done." + Alarm-Beeps.
- `main/local/volume.c` + `main/drivers/audio_out.c`: I2S1/MAX98357A
  (Pins aus `amp_test.c`), Lautstärke als Q15-Software-Gain über die
  Samples, Stufen 0–100% in 20er-Schritten, mute merkt sich den
  vorherigen Wert. Rate pro File umschaltbar (16k canned / 24k
  Gemini-TTS ab Phase 7).
- `main/local/canned_audio.c`: SPIFFS-Partition `storage` gemountet,
  Player-Task (= `audio_out_task` der Spec, Prio 5/Core 1) streamt
  WAVs chunk-weise; Queue Tiefe 4, `canned_audio_stop()` für den
  "stop"-Command. WAV-Parser läuft chunk-weise (RIFF), weil afconvert
  einen FLLR-Chunk vor `data` einfügt.
- `scripts/generate_canned_tts.py`: erzeugt die 13 Phrasen + Alarm
  nach `spiffs/` (~630 KB, 16 kHz mono). Engine `gemini` (Spec) oder
  `say` (macOS, offline) — mangels `GEMINI_API_KEY` aktuell mit `say`
  generiert; mit Key einfach neu laufen lassen. SPIFFS-Image wird via
  `spiffs_create_partition_image` bei `idf.py flash` mitgeflasht.
- `main.c`: Command-Dispatch + UI-Events (grüne Local-Bubble pro
  Command, Mic-Button kurz grün, Volume-Anzeige, Timer-Countdown).

**Acceptance-Test Phase 4 (ausstehend):** "Alexa" → "set timer five
minutes" → Bestätigungsansage, Status-Bar zählt runter, am Ende
Alarm. Außerdem prüfen: "stop timer"/"cancel timer", "how much time
left", "volume up/down", "mute"/"unmute", "stop" (bricht den Alarm
ab) und dass freie Sätze weiterhin als Capture enden ("Captured
X.Xs"-Bubble, MultiNet-Timeout-Log).

**Nächster Schritt:** Acceptance-Test Phase 4 am Gerät (siehe oben),
dann Phase 5 — Groq Whisper (`whisper.c`, Multipart-Upload des
Recording-Buffers, `esp_crt_bundle`); `GROQ_API_KEY` in menuconfig
setzen und `CONFIG_RECORDING_WAV_DUMP` abschalten.

**Offene Fragen/Blocker:** keine. Canned-WAVs sind vorerst
macOS-`say`-Stimme statt Gemini-TTS (kein API-Key konfiguriert) —
für Phase 4 funktional gleichwertig. Touch-Feinkalibrierung weiterhin
optional. Hinweise aus Phase 2, die weiter gelten: WakeNet-Threshold
auf Model-Default (0.64) belassen; ESP32-S3 kann nur 2,4-GHz-WiFi
(5-GHz-SSID → reason 201).

---

## Wichtige Entscheidungen / Abweichungen vom Spec

- **`LV_COLOR_16_SWAP` existiert in LVGL v9 nicht mehr** (Spec Section 4
  und 14 stammen aus der v8-Welt). Der RGB565-Byte-Swap für den ILI9341
  passiert stattdessen per `lv_draw_sw_rgb565_swap()` im Flush-Callback
  (`main/drivers/display.c`) — so macht es auch das offizielle
  IDF-Beispiel `spi_lcd_touch`.
- **LVGL-Version**: 9.5.0 (Managed Component `lvgl/lvgl ^9`).
- **Mic-Button-Symbol**: LVGL hat kein Mikrofon-Glyph im Default-Font;
  vorerst `LV_SYMBOL_AUDIO` als Platzhalter.
- **Umlaute**: Der Default-Font (Montserrat 14) enthält nur ASCII —
  Dummy-Texte sind umlautfrei. Für echte deutsche Antworten braucht es
  später (Phase 8) einen Custom-Font mit Latin-1-Range.
- **Partitionstabelle** (`model`/`storage`-Partitionen) schon in Phase 1
  angelegt statt erst mit esp-sr/SPIFFS, um spätere Re-Flashes zu
  vermeiden.
- **esp-sr 2.x hat eine andere AFE-API als die Spec** (Section 5 zeigt
  V1-Code): statt `AFE_CONFIG_DEFAULT()` + `ESP_AFE_SR_HANDLE` nun
  `afe_config_init("M", models, AFE_TYPE_SR, AFE_MODE_LOW_COST)` +
  `esp_afe_handle_from_config()`; das WakeNet-Model wird automatisch
  aus der model-Partition gewählt. Threshold via
  `set_wakenet_threshold(afe, 1, x)`.
- **MultiNet7-EN schon in Phase 2 in die model-Partition gepackt**
  (srmodels.bin ~3 MB von 5 MB) — geladen wird es erst in Phase 4,
  spart aber ein erneutes Model-Flashen.
- **INMP441-Pegel**: 32→16-bit-Wandlung in `audio_in.c` mit Shift 14
  statt neutral 16 (= +12 dB, mit Sättigung), da das Mic leise ist.
  Tuning-Punkt, falls Wake-Word-Erkennung zu un-/übersensibel ist.
- **"how much time left" (Command 9) sagt keine variable Restzeit an**:
  Canned-Phrasen sind fix, deshalb spielt der Command "The remaining
  time is on the display." und die exakte Zeit steht in Bubble +
  Status-Bar. Echte Ansage wäre erst mit Cloud-TTS (Phase 7) möglich.
- **Canned-Files erweitert ggü. Spec-Liste** (Section 7):
  zusätzlich `tts_timer_set_30s`, `tts_no_timer` (stop/cancel/time-left
  ohne laufenden Timer), `tts_time_left`, `tts_unmuted`, `tts_alarm`
  (synthetische Beeps, hängt nach `tts_timer_done` dran). Die
  Dateinamen-Konstanten leben in `main/local/canned_audio.h`, die
  Phrasen in `scripts/generate_canned_tts.py`.
- **Model-Liste wird einmal in `main.c` geladen** (`esp_srmodel_init`)
  und an `commands_init()` (MultiNet) und `afe_pipeline_init(models)`
  (WakeNet/AFE) gereicht — Commands müssen vor dem Pipeline-Start
  initialisiert sein, weil der fetch-Task sie ab dem ersten Wake Word
  füttert.

---

## Hardware-Pin-Map

Quelle der Wahrheit ist Spec Section 3 ("Hardware Pin Map"). Falls sich
während der Implementierung Pins ändern: **sowohl hier als auch in der
Spec aktualisieren** (kurzer Verweis + Eintrag oben unter "Wichtige
Entscheidungen").

---

## Konventionen

- Jede Phase wird in eigenen, logisch getrennten Commits abgeschlossen
  (siehe Vorgehen bei den Komponententests: ein Commit pro Test).
- `PROGRESS.md` wird am Ende jeder Session aktualisiert.

---

## ⚠️ Vor Sessionende

Aktualisiere:
1. Die Status-Tabelle oben (Phase erledigt/in Arbeit + Datum).
2. Den Abschnitt "Aktueller Stand / Nächster Schritt".
3. Bei Bedarf: "Wichtige Entscheidungen / Abweichungen vom Spec".
