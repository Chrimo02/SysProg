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
| 4 | MultiNet + Lokale Commands | ⬜ offen | |
| 5 | Groq Whisper Integration | ⬜ offen | |
| 6 | Gemini Flash Integration | ⬜ offen | |
| 7 | Gemini TTS + Wiedergabe | ⬜ offen | |
| 8 | Polish + Robustness | ⬜ offen | |
| 9 (optional) | AEC | ⬜ offen | |
| 10 (optional) | Function-Calling für freie Timer-Werte | ⬜ offen | |

---

## Aktueller Stand / Nächster Schritt

**Stand 2026-06-10 (Session 4):** Phase 3 implementiert, gebaut und
geflasht — Boot-Log sauber (`recording buffer ready (250 KB PSRAM,
max 8000 ms)`, AFE-Pipeline läuft). Aufbau:

- `main/speech/recorder.c`: 256-KB-Recording-Buffer im PSRAM
  (8s @ 16kHz @ 16bit + 44-Byte-Header-Platz). `afe_fetch_task`
  startet das Capture beim Wake Word (das Detection-Frame selbst wird
  übersprungen, es enthält noch das Wake-Word-Ende) und feedet dann
  jeden AFE-Frame; `vad_cache` wird vorangestellt (vom VAD
  zurückgehaltener Sprachanfang). Stop bei 1,5s zusammenhängender
  Stille (`res->vad_state`) oder 8s-Hard-Limit.
- `main/speech/wav.c`: 44-Byte-RIFF-Header, wird bei Capture-Ende vor
  die Samples geschrieben → Buffer ist upload-fertig für Phase 5.
- `main/debug/wav_dump.c` (`CONFIG_RECORDING_WAV_DUMP`, Default an):
  dumpt den fertigen WAV-Buffer Base64-codiert über UART (eigener
  Task Prio 2/Core 0, blockiert die Pipeline nicht; ~30s bei 115200
  Baud). `tools/extract_wav.py` extrahiert die .wav aus dem
  Monitor-Log. Ab Phase 5 abschalten.
- `main.c`: Mic-Button bleibt rot bis Capture-Ende (3s-Reset-Hack aus
  Phase 2 entfernt), danach lokale Bubble "Captured X.Xs audio (Y KB)".
  Recorder-Callback läuft im afe_fetch_task → atomare Flags wie gehabt.

**Acceptance-Test Phase 3 (2026-06-10): bestanden.** WAV via
`idf.py monitor | tee monitor.log` + `tools/extract_wav.py`
extrahiert, Aufnahme anhörbar und verständlich. Der erste Lauf
schlug fehl (96 Bytes fehlten): der Task-Watchdog (Dump-Task
busy-waitet auf die UART-FIFO, IDLE0 verhungert) und interleavte
WiFi-Logs schrieben mitten in die Base64-Zeilen. Fix: `vTaskDelay(1)`
pro Dump-Zeile + Logs während des Dumps stummschalten;
`extract_wav.py` erkennt korrupte Zeilen jetzt an der Zeilenlänge.

**Nächster Schritt:** Phase 4 — MultiNet7-EN laden (Model liegt schon
in der model-Partition), 14 lokale Commands (`commands.c`), `timer.c`,
`volume.c`, Canned-TTS-WAVs generieren (`scripts/
generate_canned_tts.py`) + SPIFFS (`canned_audio.c`).
`CONFIG_RECORDING_WAV_DUMP` kann vorerst an bleiben (stört nur durch
die ~30s Dump-Zeit nach jedem Capture); spätestens ab Phase 5
abschalten.

**Offene Fragen/Blocker:** keine. Touch-Feinkalibrierung weiterhin
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
